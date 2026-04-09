#ifndef LLM_BENCH_AECS_HPP
#define LLM_BENCH_AECS_HPP

#include "aecs_tuner.hpp"
#include "backend/cpu/AutoTuner.hpp"
#include "llm/llm.hpp"

#include <memory>
#include <string>
#include <vector>

namespace MNN {
namespace Transformer {

struct LlmBenchPhaseScheduleConfig {
    SchedulerPolicy policy = SchedulerPolicy::DYNAMIC;
    int dynamic_target_chunks = 0;
    bool dynamic_target_chunks_explicit = false;

    explicit LlmBenchPhaseScheduleConfig(SchedulerPolicy value = SchedulerPolicy::DYNAMIC)
        : policy(value) {
    }
};

struct LlmBenchScheduleConfig {
    LlmBenchPhaseScheduleConfig prefill{SchedulerPolicy::WORK_STEAL};
    LlmBenchPhaseScheduleConfig decode{SchedulerPolicy::DYNAMIC};
};

struct LlmBenchAecsSetupParams {
    std::string model_path;
    int backend = 0;
    int precision = 0;
    int memory = 0;
    int power = 0;
    int dynamic_option = 0;
    bool use_mmap = false;
    int prompt_tokens = 0;
    int decode_tokens = 0;
    int threads = 1;
    int prefill_threads = 1;
    int decode_threads = 1;
    std::vector<int> cpu_ids;
    std::vector<int> prefill_cpu_ids;
    std::vector<int> decode_cpu_ids;
    bool prefill_manual = false;
    bool decode_manual = false;
    AecsTuningConfig tuning_config;
    AecsHeuristicParams heuristic_params;
    LlmBenchScheduleConfig schedule_config;
    bool split_phase_bench = false;
};

struct LlmBenchAecsBuildPlan {
    bool use_prefill_auto = false;
    bool use_decode_auto = false;
    int pool_threads = 1;
    std::vector<int> pool_cpu_ids;
    int build_prefill_threads = 1;
    int build_decode_threads = 1;
    std::vector<int> build_prefill_cpu_ids;
    std::vector<int> build_decode_cpu_ids;
    std::vector<int> core_capacities;
};

struct LlmBenchAecsRuntimePlan {
    int pool_threads = 1;
    std::vector<int> pool_cpu_ids;
    int final_prefill_threads = 1;
    std::vector<int> final_prefill_cpu_ids;
    int final_decode_threads = 1;
    std::vector<int> final_decode_cpu_ids;
    bool split_phase_bench = false;
    std::vector<int> core_capacities;
};

std::vector<int> mergePhaseCpuIds(const std::vector<int>& cpu_ids,
                                  const std::vector<int>& prefill_cpu_ids,
                                  const std::vector<int>& decode_cpu_ids);

void configurePhaseExecutionPlan(int pool_threads,
                                 const std::vector<int>& pool_cpu_ids,
                                 int prefill_threads,
                                 const std::vector<int>& prefill_cpu_ids,
                                 int decode_threads,
                                 const std::vector<int>& decode_cpu_ids,
                                 const LlmBenchScheduleConfig& schedule_config,
                                 const std::vector<int>& core_capacities,
                                 bool verbose = true);

class LlmBenchAecsController {
public:
    explicit LlmBenchAecsController(const LlmBenchAecsSetupParams& params);

    const LlmBenchAecsBuildPlan& buildPlan() const;
    const LlmBenchAecsRuntimePlan& prepare(Llm* llm);
    void checkPrefillTemperature() const;
    void checkDecodeTemperature() const;
    bool enabled() const;

private:
    void computeBuildPlan();

private:
    LlmBenchAecsSetupParams mParams;
    AecsCpuTopology mTopology;
    LlmBenchAecsBuildPlan mBuildPlan;
    LlmBenchAecsRuntimePlan mRuntimePlan;
    bool mPrepared = false;
    std::unique_ptr<ThermalGuard> mThermalGuard;
    std::unique_ptr<EnergyProfiler> mEnergyProfiler;
};

} // namespace Transformer
} // namespace MNN

#endif // LLM_BENCH_AECS_HPP
