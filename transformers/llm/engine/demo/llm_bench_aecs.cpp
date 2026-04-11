#include "llm_bench_aecs.hpp"

#include "backend/cpu/AutoTuner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace MNN {
namespace Transformer {
namespace {

static void appendUniqueCpuIds(std::vector<int>& dst, const std::vector<int>& src) {
    for (const auto& value : src) {
        if (std::find(dst.begin(), dst.end(), value) == dst.end()) {
            dst.push_back(value);
        }
    }
}

static std::string joinCpuIds(const std::vector<int>& cpu_ids) {
    std::ostringstream stream;
    for (size_t i = 0; i < cpu_ids.size(); ++i) {
        if (i > 0) {
            stream << ",";
        }
        stream << cpu_ids[i];
    }
    return stream.str();
}

template <typename T>
static T medianValue(std::vector<T> values) {
    if (values.empty()) {
        return T();
    }
    std::sort(values.begin(), values.end());
    const size_t mid = values.size() / 2;
    if ((values.size() & 1U) == 1U) {
        return values[mid];
    }
    return (values[mid - 1] + values[mid]) / static_cast<T>(2);
}

static unsigned long cpuIdsToMask(const std::vector<int>& cpu_ids) {
    unsigned long mask = 0;
    for (auto cpu_id : cpu_ids) {
        if (cpu_id >= 0 && cpu_id < static_cast<int>(sizeof(mask) * 8)) {
            mask |= (1UL << cpu_id);
        }
    }
    return mask;
}

static std::vector<int> buildCoreCapacities(const AecsCpuTopology& topology) {
    int max_cpu_id = -1;
    for (const auto& cluster : topology.clusters_desc) {
        for (auto cpu_id : cluster.cpu_ids) {
            max_cpu_id = std::max(max_cpu_id, cpu_id);
        }
    }
    if (max_cpu_id < 0) {
        return {};
    }
    std::vector<int> capacities(max_cpu_id + 1, 0);
    for (const auto& cluster : topology.clusters_desc) {
        const int capacity = std::max(1, cluster.capacity);
        for (auto cpu_id : cluster.cpu_ids) {
            if (cpu_id >= 0 && cpu_id < static_cast<int>(capacities.size())) {
                capacities[cpu_id] = capacity;
            }
        }
    }
    return capacities;
}

static AecsCacheKey buildCacheKey(const LlmBenchAecsSetupParams& params,
                                  const AecsCpuTopology& topology) {
    AecsCacheKey cache_key;
    cache_key.device_fingerprint = topology.device_fingerprint;
    cache_key.model_path = params.model_path;
    cache_key.mnn_version = MNN_VERSION;
    cache_key.backend = params.backend;
    cache_key.precision = params.precision;
    cache_key.memory = params.memory;
    cache_key.power = params.power;
    cache_key.dynamic_option = params.dynamic_option;
    cache_key.use_mmap = params.use_mmap;
    cache_key.n_prompt = params.prompt_tokens;
    return cache_key;
}

static TuningParams buildPhaseTuningParams(const LlmBenchScheduleConfig& schedule_config,
                                           bool is_prefill,
                                           int active_threads) {
    const auto& phase_config = is_prefill ? schedule_config.prefill : schedule_config.decode;
    TuningParams params;
    if (is_prefill) {
        params.policy = phase_config.policy == SchedulerPolicy::STATIC
            ? SchedulerPolicy::STATIC
            : SchedulerPolicy::WORK_STEAL;
        if (phase_config.policy != params.policy) {
            MNN_PRINT("[llm_bench] Ignore unsupported prefill policy override: requested=%s fixed=%s\n",
                      schedulerPolicyName(phase_config.policy),
                      schedulerPolicyName(params.policy));
        }
    } else {
        params.policy = SchedulerPolicy::DYNAMIC;
        if (phase_config.policy != params.policy) {
            MNN_PRINT("[llm_bench] Ignore phase policy override: phase=%s requested=%s fixed=%s\n",
                      "decode",
                      schedulerPolicyName(phase_config.policy),
                      schedulerPolicyName(params.policy));
        }
        params.dynamic_target_chunks = phase_config.dynamic_target_chunks_explicit
            ? std::max(1, phase_config.dynamic_target_chunks)
            : std::max(1, active_threads * 2);
        params.dynamic_blocks = params.dynamic_target_chunks;
    }
    return params;
}

static std::string summarizeDecodeCandidates(const std::vector<AecsCandidateResult>& candidates, bool feasible_only) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& candidate : candidates) {
        if (feasible_only && !candidate.feasible) {
            continue;
        }
        if (!first) {
            stream << "; ";
        }
        first = false;
        stream << "[" << joinCpuIds(candidate.cpu_ids)
               << "] speed=" << candidate.measurement.speed_tok_s
               << " objective=" << candidate.objective;
    }
    return first ? "none" : stream.str();
}

static const char* runKindLabel(int run_index, int warmup_runs) {
    return run_index < warmup_runs ? "warmup" : "measure";
}

static AecsMeasurement measurePrefillCandidate(Llm* llm,
                                               int prompt_tokens,
                                               int warmup_runs,
                                               int measure_runs,
                                               const std::vector<int>& candidate_cpu_ids,
                                               int candidate_threads,
                                               int decode_threads,
                                               const std::vector<int>& decode_cpu_ids,
                                               int pool_threads,
                                               const std::vector<int>& pool_cpu_ids,
                                               const LlmBenchScheduleConfig& schedule_config,
                                               const std::vector<int>& core_capacities,
                                               ThermalGuard* thermal_guard) {
    std::vector<double> speed_samples;
    std::vector<double> time_samples;
    const std::vector<int> tokens(std::max(1, prompt_tokens), 16);

    for (int i = 0; i < warmup_runs + measure_runs; ++i) {
        if (thermal_guard != nullptr) {
            thermal_guard->checkAndPause("prefill candidate");
        }
        llm->reset();
        configurePhaseExecutionPlan(pool_threads, pool_cpu_ids,
                                    candidate_threads, candidate_cpu_ids,
                                    decode_threads, decode_cpu_ids,
                                    schedule_config, core_capacities,
                                    false);
        MNN::AutoTuner::getInstance()->setPhase(MNN::InferencePhase::PREFILL);
        llm->response(tokens, nullptr, nullptr, 1);
        auto context = llm->getContext();
        const double time_s = context->prefill_us > 0 ? static_cast<double>(context->prefill_us) / 1e6 : 0.0;
        const double speed_tok_s = context->prefill_us > 0
                                       ? 1e6 * static_cast<double>(tokens.size()) /
                                             static_cast<double>(context->prefill_us)
                                       : 0.0;
        MNN_PRINT("[AECS][Prefill][%s %d/%d] candidate=%s threads=%d prompt=%zu time=%.6f s speed=%.3f tok/s\n",
                  runKindLabel(i, warmup_runs),
                  (i < warmup_runs ? i + 1 : i - warmup_runs + 1),
                  (i < warmup_runs ? warmup_runs : measure_runs),
                  joinCpuIds(candidate_cpu_ids).c_str(),
                  candidate_threads,
                  tokens.size(),
                  time_s,
                  speed_tok_s);
        if (i >= warmup_runs && context->prefill_us > 0) {
            time_samples.push_back(time_s);
            speed_samples.push_back(speed_tok_s);
        }
    }

    AecsMeasurement measurement;
    measurement.speed_tok_s = medianValue(speed_samples);
    measurement.time_s = medianValue(time_samples);
    return measurement;
}

static AecsMeasurement measureDecodeCandidate(Llm* llm,
                                              int prompt_tokens,
                                              int decode_tokens,
                                              int warmup_runs,
                                              int measure_runs,
                                              const std::vector<int>& prefill_cpu_ids,
                                              int prefill_threads,
                                              const std::vector<int>& decode_cpu_ids,
                                              int decode_threads,
                                              int pool_threads,
                                              const std::vector<int>& pool_cpu_ids,
                                              const LlmBenchScheduleConfig& schedule_config,
                                              const std::vector<int>& core_capacities,
                                              ThermalGuard* thermal_guard,
                                              EnergyProfiler* energy_profiler) {
    std::vector<double> speed_samples;
    std::vector<double> time_samples;
    std::vector<double> energy_samples;
    std::vector<double> power_samples;
    const std::vector<int> prompt(std::max(1, prompt_tokens), 16);
    const std::vector<int> decode_seed(1, 16);

    for (int i = 0; i < warmup_runs + measure_runs; ++i) {
        if (thermal_guard != nullptr) {
            thermal_guard->checkAndPause("decode candidate");
        }
        llm->reset();
        configurePhaseExecutionPlan(pool_threads, pool_cpu_ids,
                                    prefill_threads, prefill_cpu_ids,
                                    decode_threads, decode_cpu_ids,
                                    schedule_config, core_capacities,
                                    false);
        MNN::AutoTuner::getInstance()->setPhase(MNN::InferencePhase::PREFILL);
        llm->response(prompt, nullptr, nullptr, 1);

        if (energy_profiler != nullptr && energy_profiler->available()) {
            energy_profiler->begin();
        }
        MNN::AutoTuner::getInstance()->setPhase(MNN::InferencePhase::DECODE);
        llm->response(decode_seed, nullptr, nullptr, std::max(1, decode_tokens));
        const auto power_result = (energy_profiler != nullptr && energy_profiler->available())
                                      ? energy_profiler->end()
                                      : PowerSampleResult();

        auto context = llm->getContext();
        const double prefill_time_s = context->prefill_us > 0 ? static_cast<double>(context->prefill_us) / 1e6 : 0.0;
        const double decode_time_s = context->decode_us > 0 ? static_cast<double>(context->decode_us) / 1e6 : 0.0;
        const double decode_speed_tok_s = context->decode_us > 0
                                              ? 1e6 * static_cast<double>(std::max(1, decode_tokens)) /
                                                    static_cast<double>(context->decode_us)
                                              : 0.0;
        MNN_PRINT("[AECS][Decode][%s %d/%d] prefill=%s/%d decode=%s/%d prompt=%zu gen=%d prefill_time=%.6f s decode_time=%.6f s decode_speed=%.3f tok/s%s\n",
                  runKindLabel(i, warmup_runs),
                  (i < warmup_runs ? i + 1 : i - warmup_runs + 1),
                  (i < warmup_runs ? warmup_runs : measure_runs),
                  joinCpuIds(prefill_cpu_ids).c_str(),
                  prefill_threads,
                  joinCpuIds(decode_cpu_ids).c_str(),
                  decode_threads,
                  prompt.size(),
                  std::max(1, decode_tokens),
                  prefill_time_s,
                  decode_time_s,
                  decode_speed_tok_s,
                  power_result.valid ? "" : " energy=n/a");
        if (i >= warmup_runs && context->decode_us > 0) {
            time_samples.push_back(decode_time_s);
            speed_samples.push_back(decode_speed_tok_s);
            if (power_result.valid) {
                energy_samples.push_back(power_result.energy_j);
                power_samples.push_back(power_result.avg_power_w);
                MNN_PRINT("[AECS][Decode][measure %d/%d] energy=%.6f J avg_power=%.6f W\n",
                          i - warmup_runs + 1,
                          measure_runs,
                          power_result.energy_j,
                          power_result.avg_power_w);
            }
        }
    }

    AecsMeasurement measurement;
    measurement.speed_tok_s = medianValue(speed_samples);
    measurement.time_s = medianValue(time_samples);
    if (!energy_samples.empty()) {
        measurement.energy_j = medianValue(energy_samples);
        measurement.avg_power_w = medianValue(power_samples);
        measurement.energy_valid = true;
    }
    return measurement;
}

} // namespace

std::vector<int> mergePhaseCpuIds(const std::vector<int>& cpu_ids,
                                  const std::vector<int>& prefill_cpu_ids,
                                  const std::vector<int>& decode_cpu_ids) {
    std::vector<int> merged = cpu_ids;
    appendUniqueCpuIds(merged, prefill_cpu_ids);
    appendUniqueCpuIds(merged, decode_cpu_ids);
    return merged;
}

void configurePhaseExecutionPlan(int pool_threads,
                                 const std::vector<int>& pool_cpu_ids,
                                 int prefill_threads,
                                 const std::vector<int>& prefill_cpu_ids,
                                 int decode_threads,
                                 const std::vector<int>& decode_cpu_ids,
                                 const LlmBenchScheduleConfig& schedule_config,
                                 const std::vector<int>& core_capacities,
                                 bool verbose) {
    const auto pool_affinity_mask = cpuIdsToMask(pool_cpu_ids);
    const auto prefill_affinity_mask = cpuIdsToMask(prefill_cpu_ids);
    const auto decode_affinity_mask = cpuIdsToMask(decode_cpu_ids);
    auto* tuner = MNN::AutoTuner::getInstance();
    tuner->setCoreCapacities(core_capacities);
    tuner->setPrefillParams(buildPhaseTuningParams(schedule_config, true, std::max(1, prefill_threads)));
    tuner->setDecodeParams(buildPhaseTuningParams(schedule_config, false, std::max(1, decode_threads)));
    tuner->setDefaultExecution(std::max(1, pool_threads), pool_affinity_mask);
    tuner->setPrefillExecution(std::max(1, prefill_threads), prefill_affinity_mask);
    tuner->setDecodeExecution(std::max(1, decode_threads), decode_affinity_mask);
    if (verbose) {
        const auto prefill_params = buildPhaseTuningParams(schedule_config, true, std::max(1, prefill_threads));
        const auto decode_params = buildPhaseTuningParams(schedule_config, false, std::max(1, decode_threads));
        MNN_PRINT("[llm_bench] Thread config: pool=%d, prefill=%d, decode=%d\n",
                  pool_threads, prefill_threads, decode_threads);
        MNN_PRINT("[llm_bench] Pool    cpu ids: %s | affinity mask: 0x%lX\n",
                  joinCpuIds(pool_cpu_ids).c_str(), pool_affinity_mask);
        MNN_PRINT("[llm_bench] Prefill cpu ids: %s | affinity mask: 0x%lX\n",
                  joinCpuIds(prefill_cpu_ids).c_str(), prefill_affinity_mask);
        MNN_PRINT("[llm_bench] Decode  cpu ids: %s | affinity mask: 0x%lX\n",
                  joinCpuIds(decode_cpu_ids).c_str(), decode_affinity_mask);
        MNN_PRINT("[llm_bench] Scheduler prefill=%s decode=%s\n",
                  schedulerPolicyName(prefill_params.policy),
                  schedulerPolicyName(decode_params.policy));
    }
}

LlmBenchAecsController::LlmBenchAecsController(const LlmBenchAecsSetupParams& params)
    : mParams(params) {
    computeBuildPlan();
}

void LlmBenchAecsController::computeBuildPlan() {
    mBuildPlan.use_prefill_auto = mParams.tuning_config.prefill_auto_bind && !mParams.prefill_manual;
    mBuildPlan.use_decode_auto = mParams.tuning_config.decode_aecs && !mParams.decode_manual;
    mTopology = AecsCpuInspector::inspect(mParams.tuning_config.prefill_start_cpu);
    mBuildPlan.core_capacities = buildCoreCapacities(mTopology);
    mCachedPhaseResult = PhaseTuningResult();
    mUseCachedStaticCalibration = false;
    mUseCachedPlan = false;

    if (!mParams.tuning_config.force_retune) {
        const auto cache_key = buildCacheKey(mParams, mTopology);
        AecsTuner cache_probe(mTopology, mParams.tuning_config, mParams.heuristic_params);
        mCachedPhaseResult = cache_probe.tune(
            cache_key,
            std::vector<int>(),
            std::vector<int>(),
            std::max(1, mParams.prefill_threads),
            std::max(1, mParams.decode_threads),
            [&](const std::vector<int>&, int) {
                MNN_ERROR("[AECS] Unexpected prefill measurement during cache-only probe\n");
                return AecsMeasurement();
            },
            [&](const std::vector<int>&, int, const std::vector<int>&, int) {
                MNN_ERROR("[AECS] Unexpected decode measurement during cache-only probe\n");
                return AecsMeasurement();
            });
        if (mCachedPhaseResult.cache_hit) {
            const std::string cached_prefill_ids = mCachedPhaseResult.prefill_cpu_ids.empty()
                ? std::string("<none>")
                : joinCpuIds(mCachedPhaseResult.prefill_cpu_ids);
            const std::string cached_decode_ids = mCachedPhaseResult.decode_cpu_ids.empty()
                ? std::string("<none>")
                : joinCpuIds(mCachedPhaseResult.decode_cpu_ids);
            const std::string cached_capacities = mCachedPhaseResult.static_calibration.core_capacities.empty()
                ? std::string("<none>")
                : joinCpuIds(mCachedPhaseResult.static_calibration.core_capacities);
            MNN_PRINT("[AECS][Cache] execution plan prefill=%s/%d decode=%s/%d\n",
                      cached_prefill_ids.c_str(),
                      mCachedPhaseResult.prefill_threads,
                      cached_decode_ids.c_str(),
                      mCachedPhaseResult.decode_threads);
            MNN_PRINT("[AECS][Cache] static core capacities=%s (valid=%d, cluster_count=%d)\n",
                      cached_capacities.c_str(),
                      mCachedPhaseResult.static_calibration.valid ? 1 : 0,
                      mCachedPhaseResult.static_calibration.cluster_count);
        } else {
            MNN_PRINT("[AECS][Cache] miss for current key (no cached execution plan/static capacities)\n");
        }
        mUseCachedStaticCalibration = mCachedPhaseResult.cache_hit &&
                                     mCachedPhaseResult.static_calibration.valid;
        mUseCachedPlan = !mParams.prefill_manual &&
                         !mParams.decode_manual &&
                         mCachedPhaseResult.cache_hit &&
                         (!mCachedPhaseResult.prefill_cpu_ids.empty() ||
                          !mCachedPhaseResult.decode_cpu_ids.empty());
        if (mUseCachedStaticCalibration) {
            mBuildPlan.core_capacities = mCachedPhaseResult.static_calibration.core_capacities;
        }
        if ((mParams.prefill_manual || mParams.decode_manual) &&
            (!mCachedPhaseResult.prefill_cpu_ids.empty() || !mCachedPhaseResult.decode_cpu_ids.empty())) {
            MNN_PRINT("[AECS][Cache] manual phase binding present, ignore cached execution plan and reuse static capacities only\n");
        }
    }

    mBuildPlan.pool_threads = std::max(mParams.threads, std::max(mParams.prefill_threads, mParams.decode_threads));
    mBuildPlan.pool_cpu_ids = mergePhaseCpuIds(mParams.cpu_ids, mParams.prefill_cpu_ids, mParams.decode_cpu_ids);
    if (enabled() && mBuildPlan.pool_cpu_ids.empty()) {
        if (mUseCachedPlan) {
            appendUniqueCpuIds(mBuildPlan.pool_cpu_ids, mCachedPhaseResult.prefill_cpu_ids);
            appendUniqueCpuIds(mBuildPlan.pool_cpu_ids, mCachedPhaseResult.decode_cpu_ids);
        }
        // Keep the build-time pool aligned with the AECS search frontier so candidate measurements
        // are not distorted by extra low-priority cores that are outside the intended search space.
        if (mBuildPlan.pool_cpu_ids.empty()) {
            const auto& auto_pool_cpu_ids =
                !mTopology.decode_stage1_order.empty() ? mTopology.decode_stage1_order : mTopology.prefill_order;
            appendUniqueCpuIds(mBuildPlan.pool_cpu_ids, auto_pool_cpu_ids);
        }
        mBuildPlan.pool_threads = std::max(mBuildPlan.pool_threads, static_cast<int>(mBuildPlan.pool_cpu_ids.size()));
    }

    mBuildPlan.build_prefill_threads = mBuildPlan.use_prefill_auto ? mBuildPlan.pool_threads : mParams.prefill_threads;
    mBuildPlan.build_decode_threads = mBuildPlan.use_decode_auto ? mBuildPlan.pool_threads : mParams.decode_threads;
    mBuildPlan.build_prefill_cpu_ids = mBuildPlan.use_prefill_auto ? mBuildPlan.pool_cpu_ids : mParams.prefill_cpu_ids;
    mBuildPlan.build_decode_cpu_ids = mBuildPlan.use_decode_auto ? mBuildPlan.pool_cpu_ids : mParams.decode_cpu_ids;
    if (mUseCachedPlan && !mBuildPlan.use_prefill_auto && !mBuildPlan.use_decode_auto) {
        if (!mCachedPhaseResult.prefill_cpu_ids.empty()) {
            mBuildPlan.build_prefill_cpu_ids = mCachedPhaseResult.prefill_cpu_ids;
            mBuildPlan.build_prefill_threads = std::max(1, mCachedPhaseResult.prefill_threads);
        }
        if (!mCachedPhaseResult.decode_cpu_ids.empty()) {
            mBuildPlan.build_decode_cpu_ids = mCachedPhaseResult.decode_cpu_ids;
            mBuildPlan.build_decode_threads = std::max(1, mCachedPhaseResult.decode_threads);
        }
        mBuildPlan.pool_threads = std::max(mBuildPlan.pool_threads,
                                           std::max(mBuildPlan.build_prefill_threads, mBuildPlan.build_decode_threads));
    }
}

const LlmBenchAecsBuildPlan& LlmBenchAecsController::buildPlan() const {
    return mBuildPlan;
}

const LlmBenchAecsRuntimePlan& LlmBenchAecsController::prepare(Llm* llm) {
    if (mPrepared) {
        return mRuntimePlan;
    }

    mRuntimePlan.pool_threads = mBuildPlan.pool_threads;
    mRuntimePlan.pool_cpu_ids = mBuildPlan.pool_cpu_ids;
    mRuntimePlan.final_prefill_threads = std::max(1, mParams.prefill_threads);
    mRuntimePlan.final_prefill_cpu_ids = mParams.prefill_cpu_ids;
    mRuntimePlan.final_decode_threads = std::max(1, mParams.decode_threads);
    mRuntimePlan.final_decode_cpu_ids = mParams.decode_cpu_ids;
    mRuntimePlan.split_phase_bench = enabled() || mParams.split_phase_bench;
    mRuntimePlan.core_capacities = mBuildPlan.core_capacities;

    if (mUseCachedStaticCalibration) {
        mBuildPlan.core_capacities = mCachedPhaseResult.static_calibration.core_capacities;
        mRuntimePlan.core_capacities = mCachedPhaseResult.static_calibration.core_capacities;
        MNN_PRINT("[AECS] Reuse cached static calibration capacities=%s\n",
                  joinCpuIds(mCachedPhaseResult.static_calibration.core_capacities).c_str());
    }

    const bool cache_only_mode = mUseCachedPlan && !mBuildPlan.use_prefill_auto && !mBuildPlan.use_decode_auto;
    if (cache_only_mode) {
        if (!mCachedPhaseResult.prefill_cpu_ids.empty()) {
            mRuntimePlan.final_prefill_cpu_ids = mCachedPhaseResult.prefill_cpu_ids;
            mRuntimePlan.final_prefill_threads = std::max(1, mCachedPhaseResult.prefill_threads);
        }
        if (!mCachedPhaseResult.decode_cpu_ids.empty()) {
            mRuntimePlan.final_decode_cpu_ids = mCachedPhaseResult.decode_cpu_ids;
            mRuntimePlan.final_decode_threads = std::max(1, mCachedPhaseResult.decode_threads);
        }
        MNN_PRINT("[AECS] Reuse cached execution plan prefill=%s/%d decode=%s/%d without retune\n",
                  joinCpuIds(mRuntimePlan.final_prefill_cpu_ids).c_str(),
                  mRuntimePlan.final_prefill_threads,
                  joinCpuIds(mRuntimePlan.final_decode_cpu_ids).c_str(),
                  mRuntimePlan.final_decode_threads);
    } else if (enabled()) {
        MNN_PRINT("[AECS] Start tuning model=%s prompt=%d decode=%d prefill_auto=%d decode_aecs=%d\n",
                  mParams.model_path.c_str(),
                  mParams.prompt_tokens,
                  mParams.decode_tokens,
                  mBuildPlan.use_prefill_auto ? 1 : 0,
                  mBuildPlan.use_decode_auto ? 1 : 0);
        MNN_PRINT("[AECS] Initial execution plan pool=%d pool_cpu_ids=%s build_prefill=%d/%s build_decode=%d/%s\n",
                  mBuildPlan.pool_threads,
                  joinCpuIds(mBuildPlan.pool_cpu_ids).c_str(),
                  mBuildPlan.build_prefill_threads,
                  joinCpuIds(mBuildPlan.build_prefill_cpu_ids).c_str(),
                  mBuildPlan.build_decode_threads,
                  joinCpuIds(mBuildPlan.build_decode_cpu_ids).c_str());
        mThermalGuard.reset(new ThermalGuard(mParams.tuning_config));
        mEnergyProfiler.reset(new EnergyProfiler(mParams.tuning_config));

        const auto cache_key = buildCacheKey(mParams, mTopology);

        const int tuning_prompt_tokens = std::max(1, mParams.prompt_tokens);
        // Keep AECS search workload aligned with the llm_bench case so the selected plan
        // is measured under the same prompt/decode length and repeat count as the final run.
        const int tuning_decode_tokens = std::max(1, mParams.decode_tokens);
        const int tuning_warmup_runs = 1;
        const int tuning_measure_runs = std::max(1, mParams.benchmark_repeat);
        const int tuning_default_decode_threads = std::max(1, mParams.decode_threads);
        const auto& tuning_default_decode_cpu_ids =
            !mParams.decode_cpu_ids.empty() ? mParams.decode_cpu_ids : mBuildPlan.pool_cpu_ids;
        if (mParams.prompt_tokens <= 0) {
            MNN_PRINT("[AECS] prompt_tokens=%d, fallback to %d token for tuning representative workload\n",
                      mParams.prompt_tokens, tuning_prompt_tokens);
        }
        MNN_PRINT("[AECS] Tuning workload prompt=%d decode=%d warmup=%d measure=%d default_decode=%d/%s\n",
                  tuning_prompt_tokens,
                  tuning_decode_tokens,
                  tuning_warmup_runs,
                  tuning_measure_runs,
                  tuning_default_decode_threads,
                  joinCpuIds(tuning_default_decode_cpu_ids).c_str());

        LlmBenchScheduleConfig static_schedule_config = mParams.schedule_config;
        static_schedule_config.prefill.policy = SchedulerPolicy::STATIC;
        AecsTuner static_tuner(mTopology, mParams.tuning_config, mParams.heuristic_params);
        MNN_PRINT("[AECS] Start static calibration with build pool=%d/%s and inspected capacities=%s, force prefill policy=%s\n",
                  mBuildPlan.pool_threads,
                  joinCpuIds(mBuildPlan.pool_cpu_ids).c_str(),
                  joinCpuIds(mBuildPlan.core_capacities).c_str(),
                  schedulerPolicyName(static_schedule_config.prefill.policy));
        const auto static_calibration = static_tuner.calibrateStaticCapacities(
            cache_key,
            [&](const std::vector<int>& cpu_ids, int threads, const std::vector<int>& core_capacities) {
                return measurePrefillCandidate(llm,
                                               tuning_prompt_tokens,
                                               tuning_warmup_runs,
                                               tuning_measure_runs,
                                               cpu_ids,
                                               threads,
                                               tuning_default_decode_threads,
                                               tuning_default_decode_cpu_ids,
                                               mBuildPlan.pool_threads,
                                               mBuildPlan.pool_cpu_ids,
                                               static_schedule_config,
                                               core_capacities,
                                               mThermalGuard.get());
            });
        if (static_calibration.valid) {
            mBuildPlan.core_capacities = static_calibration.core_capacities;
            mRuntimePlan.core_capacities = static_calibration.core_capacities;
            MNN_PRINT("[AECS] Static calibration %s cluster_count=%d capacities=%s\n",
                      static_calibration.cache_hit ? "cache-hit" : "measured",
                      static_calibration.cluster_count,
                      joinCpuIds(static_calibration.core_capacities).c_str());
        } else {
            MNN_PRINT("[AECS] Static calibration unavailable, continue with inspected capacities=%s\n",
                      joinCpuIds(mBuildPlan.core_capacities).c_str());
        }

        AecsTuner tuner(mTopology, mParams.tuning_config, mParams.heuristic_params, mBuildPlan.pool_cpu_ids);
        const auto tuned = tuner.tune(
            cache_key,
            mParams.prefill_manual ? mParams.prefill_cpu_ids : std::vector<int>(),
            mParams.decode_manual ? mParams.decode_cpu_ids : std::vector<int>(),
            mRuntimePlan.final_prefill_threads,
            mRuntimePlan.final_decode_threads,
            [&](const std::vector<int>& cpu_ids, int threads) {
                return measurePrefillCandidate(llm,
                                               tuning_prompt_tokens,
                                               tuning_warmup_runs,
                                               tuning_measure_runs,
                                               cpu_ids,
                                               threads,
                                               tuning_default_decode_threads,
                                               tuning_default_decode_cpu_ids,
                                               mBuildPlan.pool_threads,
                                               mBuildPlan.pool_cpu_ids,
                                               mParams.schedule_config,
                                               mBuildPlan.core_capacities,
                                               mThermalGuard.get());
            },
            [&](const std::vector<int>& prefill_cpu_ids, int prefill_threads,
                const std::vector<int>& decode_cpu_ids, int decode_threads) {
                return measureDecodeCandidate(llm,
                                              tuning_prompt_tokens,
                                              tuning_decode_tokens,
                                              tuning_warmup_runs,
                                              tuning_measure_runs,
                                              prefill_cpu_ids,
                                              prefill_threads,
                                              decode_cpu_ids,
                                              decode_threads,
                                              mBuildPlan.pool_threads,
                                              mBuildPlan.pool_cpu_ids,
                                              mParams.schedule_config,
                                              mBuildPlan.core_capacities,
                                              mThermalGuard.get(),
                                              mEnergyProfiler.get());
            });

        if (!tuned.prefill_cpu_ids.empty()) {
            mRuntimePlan.final_prefill_cpu_ids = tuned.prefill_cpu_ids;
            mRuntimePlan.final_prefill_threads = std::max(1, tuned.prefill_threads);
        }
        if (!tuned.decode_cpu_ids.empty()) {
            mRuntimePlan.final_decode_cpu_ids = tuned.decode_cpu_ids;
            mRuntimePlan.final_decode_threads = std::max(1, tuned.decode_threads);
        }

        if (!tuned.fastest_decode_candidate.cpu_ids.empty()) {
            MNN_PRINT("[AECS] Fastest decode candidate=%s speed=%.3f tok/s\n",
                      joinCpuIds(tuned.fastest_decode_candidate.cpu_ids).c_str(),
                      tuned.fastest_decode_candidate.measurement.speed_tok_s);
        }
        if (!tuned.selected_decode_candidate.cpu_ids.empty()) {
            MNN_PRINT("[AECS] Selected decode candidate=%s speed=%.3f tok/s objective=%.6f\n",
                      joinCpuIds(tuned.selected_decode_candidate.cpu_ids).c_str(),
                      tuned.selected_decode_candidate.measurement.speed_tok_s,
                      tuned.selected_decode_candidate.objective);
        }
        if (!tuned.decode_candidates.empty()) {
            MNN_PRINT("[AECS] Feasible decode candidates: %s\n",
                      summarizeDecodeCandidates(tuned.decode_candidates, true).c_str());
            MNN_PRINT("[AECS] All decode candidates: %s\n",
                      summarizeDecodeCandidates(tuned.decode_candidates, false).c_str());
        }
    }

    configurePhaseExecutionPlan(mRuntimePlan.pool_threads, mRuntimePlan.pool_cpu_ids,
                                mRuntimePlan.final_prefill_threads, mRuntimePlan.final_prefill_cpu_ids,
                                mRuntimePlan.final_decode_threads, mRuntimePlan.final_decode_cpu_ids,
                                mParams.schedule_config, mRuntimePlan.core_capacities,
                                true);
    MNN_PRINT("[AECS] Final execution plan prefill=%s/%d decode=%s/%d split_phase_bench=%d\n",
              joinCpuIds(mRuntimePlan.final_prefill_cpu_ids).c_str(),
              mRuntimePlan.final_prefill_threads,
              joinCpuIds(mRuntimePlan.final_decode_cpu_ids).c_str(),
              mRuntimePlan.final_decode_threads,
              mRuntimePlan.split_phase_bench ? 1 : 0);
    MNN_PRINT("[AECS] Final static core capacities=%s\n",
              joinCpuIds(mRuntimePlan.core_capacities).c_str());
    MNN::AutoTuner::getInstance()->setPhase(MNN::InferencePhase::UNKNOWN);
    llm->reset();
    mPrepared = true;
    return mRuntimePlan;
}

void LlmBenchAecsController::checkPrefillTemperature() const {
    if (mThermalGuard != nullptr) {
        mThermalGuard->checkAndPause("benchmark prefill");
    }
}

void LlmBenchAecsController::checkDecodeTemperature() const {
    if (mThermalGuard != nullptr) {
        mThermalGuard->checkAndPause("benchmark decode");
    }
}

bool LlmBenchAecsController::enabled() const {
    return mBuildPlan.use_prefill_auto || mBuildPlan.use_decode_auto || mUseCachedPlan;
}

} // namespace Transformer
} // namespace MNN
