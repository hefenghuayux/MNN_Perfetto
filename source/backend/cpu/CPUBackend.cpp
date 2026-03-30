//
//  CPUBackend.cpp
//  MNN
//
//  Created by MNN on 2018/07/06.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "backend/cpu/CPUBackend.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <sstream>
#include <strings.h>
#include <unordered_map>
#include "CPUResizeCache.hpp"
#include "core/BufferAllocator.hpp"
#include "CPUTensorConvert.hpp"
#include "compute/CommonOptFunction.h"
#include "core/TensorUtils.hpp"
#include "ThreadPool.hpp"
#include "core/Concurrency.h"
#include "CPUCast.hpp"
#include "core/OpCommonUtils.hpp"
#include "core/WrapExecution.hpp"
#include "core/MNNFileUtils.h"
#include "core/WorkerThread.hpp"
#include "../../utils/trace_marker_helper.h"
#ifdef _OPENMP
#include <omp.h>
#endif // _OPENMP
#include "backend/cpu/CPURuntime.hpp"
#include "core/Macro.h"
#ifdef MNN_USE_ARMV82
#include "backend/arm82/Arm82Backend.hpp"
#endif
#define MAX_THREAD_NUMBER 32
#define LARGE_MEMORY 1024 * 1024 * 500
#ifdef MNN_SUPPORT_BF16
#include "bf16/BF16Functions.hpp"
#endif

#ifdef MNN_USE_SSE
#include "x86_x64/AVX2Backend.hpp"
#endif

#define MNN_CPU_MAX_BUFFER_INDEX 2
#define MNN_CPU_CHECK_NAN 1
#define MNN_CPU_USE_DEFAULT_BACKEND 4
extern "C" {
    __attribute__((visibility("default"))) std::atomic<int> g_small_task_count(0);
    __attribute__((visibility("default"))) std::atomic<int> g_task_count(0);
    __attribute__((visibility("default"))) std::atomic<long long> g_divide_size_total(0);
    __attribute__((visibility("default"))) std::atomic<int> g_divide_size_count(0);
}
namespace MNN {
namespace {

static TuningParams sanitizeTuningParams(const TuningParams& input) {
    TuningParams params = input;
    params.static_ratio = std::max(0.0f, std::min(1.0f, params.static_ratio));
    params.dynamic_blocks = std::max(0, params.dynamic_blocks);
    if (params.dynamic_target_chunks <= 0) {
        params.dynamic_target_chunks = params.dynamic_blocks;
    }
    params.dynamic_target_chunks = std::max(0, params.dynamic_target_chunks);
    params.min_chunk_size = std::max(1, params.min_chunk_size);
    return params;
}

static bool parseIntEnv(const char* name, int& value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0')) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

static bool parseFloatEnv(const char* name, float& value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(raw, &end);
    if (errno != 0 || end == raw || (end != nullptr && *end != '\0')) {
        return false;
    }
    value = parsed;
    return true;
}

static bool parsePolicyString(const char* raw, SchedulerPolicy& value) {
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    if (0 == strcasecmp(raw, "dynamic") || 0 == std::strcmp(raw, "0")) {
        value = SchedulerPolicy::DYNAMIC;
        return true;
    }
    if (0 == strcasecmp(raw, "hybrid") || 0 == std::strcmp(raw, "1")) {
        value = SchedulerPolicy::HYBRID;
        return true;
    }
    if (0 == strcasecmp(raw, "guided") || 0 == std::strcmp(raw, "2")) {
        value = SchedulerPolicy::GUIDED;
        return true;
    }
    return false;
}

static bool parsePolicyEnv(const char* name, SchedulerPolicy& value) {
    return parsePolicyString(std::getenv(name), value);
}

static bool parseBoolEnv(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    return 0 == strcasecmp(raw, "1")
        || 0 == strcasecmp(raw, "true")
        || 0 == strcasecmp(raw, "yes")
        || 0 == strcasecmp(raw, "on");
}

static TuningParams applyEnvOverrides(TuningParams params, bool isDecode) {
    const char* phaseStaticRatioEnv = isDecode
        ? "MNN_HYBRID_DECODE_STATIC_RATIO"
        : "MNN_HYBRID_PREFILL_STATIC_RATIO";
    const char* phaseTargetChunksEnv = isDecode
        ? "MNN_HYBRID_DECODE_TARGET_CHUNKS"
        : "MNN_HYBRID_PREFILL_TARGET_CHUNKS";
    const char* phaseMinChunkEnv = isDecode
        ? "MNN_HYBRID_DECODE_MIN_CHUNK_SIZE"
        : "MNN_HYBRID_PREFILL_MIN_CHUNK_SIZE";
    const char* phasePolicyEnv = isDecode
        ? "MNN_HYBRID_DECODE_POLICY"
        : "MNN_HYBRID_PREFILL_POLICY";
    float staticRatio = 0.0f;
    int targetChunks = 0;
    int minChunkSize = 0;
    SchedulerPolicy policy = params.policy;

    if (parseFloatEnv("MNN_HYBRID_STATIC_RATIO", staticRatio)) {
        params.static_ratio = staticRatio;
    }
    if (parseFloatEnv(phaseStaticRatioEnv, staticRatio)) {
        params.static_ratio = staticRatio;
    }

    if (parseIntEnv("MNN_HYBRID_TARGET_CHUNKS", targetChunks)) {
        params.dynamic_target_chunks = targetChunks;
    }
    if (parseIntEnv(phaseTargetChunksEnv, targetChunks)) {
        params.dynamic_target_chunks = targetChunks;
    }

    if (parseIntEnv("MNN_HYBRID_MIN_CHUNK_SIZE", minChunkSize)) {
        params.min_chunk_size = minChunkSize;
    }
    if (parseIntEnv(phaseMinChunkEnv, minChunkSize)) {
        params.min_chunk_size = minChunkSize;
    }

    if (parsePolicyEnv("MNN_HYBRID_POLICY", policy)) {
        params.policy = policy;
    }
    if (parsePolicyEnv(phasePolicyEnv, policy)) {
        params.policy = policy;
    }

    return sanitizeTuningParams(params);
}

static bool forceHybridScheduling(bool isDecode) {
    if (parseBoolEnv("MNN_HYBRID_FORCE_ENABLE")) {
        return true;
    }
    return isDecode ? parseBoolEnv("MNN_HYBRID_FORCE_DECODE")
                    : parseBoolEnv("MNN_HYBRID_FORCE_PREFILL");
}

static ExecutionParams sanitizeExecutionParams(int active_threads, unsigned long affinity_mask) {
    return ExecutionParams(std::max(1, active_threads), affinity_mask);
}

static void appendThreadLoads(std::ostringstream& stream,
                              const std::array<long long, MNN_MAX_SCHEDULER_THREADS>& values) {
    stream << "[";
    bool first = true;
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] <= 0) {
            continue;
        }
        if (!first) {
            stream << ",";
        }
        first = false;
        stream << i << ":" << values[i];
    }
    stream << "]";
}

static int _effectiveThreadCount(int configuredThreads) {
    return std::min(configuredThreads, AutoTuner::getInstance()->getActiveThreadCount());
}

static int _maxCpuId(const MNNCPUInfo* cpuInfo) {
    int maxCpu = -1;
    if (cpuInfo == nullptr) {
        return maxCpu;
    }
    for (const auto& group : cpuInfo->groups) {
        for (auto cpuId : group.ids) {
            maxCpu = std::max(maxCpu, cpuId);
        }
    }
    return maxCpu;
}

static std::vector<int> _defaultCoreCapacities(const MNNCPUInfo* cpuInfo) {
    std::vector<int> capacities;
    if (cpuInfo == nullptr) {
        return capacities;
    }
    capacities.resize(_maxCpuId(cpuInfo) + 1, 0);
    for (const auto& group : cpuInfo->groups) {
        const int capacity = std::max(1, static_cast<int>(group.maxFreq));
        for (auto cpuId : group.ids) {
            if (cpuId >= 0 && cpuId < static_cast<int>(capacities.size())) {
                capacities[cpuId] = std::max(capacities[cpuId], capacity);
            }
        }
    }
    return capacities;
}

static int _cpuCapacity(const std::vector<int>& capacities, int cpuId) {
    if (cpuId >= 0 && cpuId < static_cast<int>(capacities.size()) && capacities[cpuId] > 0) {
        return capacities[cpuId];
    }
    return 1;
}

static std::vector<int> _activeCpuIds(const MNNCPUInfo* cpuInfo, unsigned long affinityMask, int effectiveThreads) {
    std::vector<int> cpuIds;
    if (affinityMask != 0) {
        for (int i = static_cast<int>(sizeof(affinityMask) * 8) - 1; i >= 0; --i) {
            if ((affinityMask >> i) & 1UL) {
                cpuIds.push_back(i);
                if (static_cast<int>(cpuIds.size()) >= effectiveThreads) {
                    return cpuIds;
                }
            }
        }
    }
    if (cpuInfo != nullptr) {
        for (auto groupIter = cpuInfo->groups.rbegin(); groupIter != cpuInfo->groups.rend(); ++groupIter) {
            auto ids = groupIter->ids;
            std::sort(ids.begin(), ids.end(), std::greater<int>());
            for (auto cpuId : ids) {
                if (std::find(cpuIds.begin(), cpuIds.end(), cpuId) != cpuIds.end()) {
                    continue;
                }
                cpuIds.push_back(cpuId);
                if (static_cast<int>(cpuIds.size()) >= effectiveThreads) {
                    return cpuIds;
                }
            }
        }
    }
    for (int i = static_cast<int>(cpuIds.size()); i < effectiveThreads; ++i) {
        cpuIds.push_back(i);
    }
    return cpuIds;
}

static std::vector<float> _activeThreadWeights(const MNNCPUInfo* cpuInfo,
                                               unsigned long affinityMask,
                                               int effectiveThreads) {
    std::vector<float> weights;
    if (effectiveThreads <= 0) {
        return weights;
    }
    weights.resize(effectiveThreads, 1.0f / static_cast<float>(effectiveThreads));
    const auto capacities = _defaultCoreCapacities(cpuInfo);
    const auto activeCpuIds = _activeCpuIds(cpuInfo, affinityMask, effectiveThreads);
    if (static_cast<int>(activeCpuIds.size()) < effectiveThreads) {
        return weights;
    }

    double total = 0.0;
    for (int i = 0; i < effectiveThreads; ++i) {
        total += _cpuCapacity(capacities, activeCpuIds[i]);
    }
    if (total <= 0.0) {
        return weights;
    }
    for (int i = 0; i < effectiveThreads; ++i) {
        weights[i] = static_cast<float>(_cpuCapacity(capacities, activeCpuIds[i]) / total);
    }
    return weights;
}

struct WeightDispersion {
    float min_weight = 0.0f;
    float max_weight = 0.0f;
    float ratio = 1.0f;
    float cv = 0.0f;
};

static WeightDispersion _weightDispersion(const std::vector<float>& weights) {
    WeightDispersion dispersion;
    if (weights.empty()) {
        return dispersion;
    }
    const auto range = std::minmax_element(weights.begin(), weights.end());
    dispersion.min_weight = *range.first;
    dispersion.max_weight = *range.second;
    if (dispersion.min_weight > 0.0f) {
        dispersion.ratio = dispersion.max_weight / dispersion.min_weight;
    }
    const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    const double mean = sum / static_cast<double>(weights.size());
    if (mean <= 0.0) {
        return dispersion;
    }
    double variance = 0.0;
    for (const auto weight : weights) {
        const double diff = static_cast<double>(weight) - mean;
        variance += diff * diff;
    }
    variance /= static_cast<double>(weights.size());
    dispersion.cv = static_cast<float>(std::sqrt(variance) / mean);
    return dispersion;
}

static bool _hasWeightVariance(const std::vector<float>& weights) {
    if (weights.size() <= 1) {
        return false;
    }
    const auto dispersion = _weightDispersion(weights);
    return dispersion.ratio >= 1.12f || dispersion.cv >= 0.05f;
}

static bool _hasHighWeightVariance(const std::vector<float>& weights) {
    if (weights.size() <= 1) {
        return false;
    }
    const auto dispersion = _weightDispersion(weights);
    return dispersion.ratio >= 1.50f || dispersion.cv >= 0.15f;
}

static void _fillUniformDivides(int size, int* dst, int effectiveThreads, int totalThreads, int fillValue) {
    const int length = effectiveThreads > 0 ? UP_DIV(size, effectiveThreads) : size;
    int cur = length;
    for (int i = 0; i < effectiveThreads; ++i) {
        dst[i] = cur;
        cur += length;
        cur = ALIMIN(cur, size);
    }
    for (int i = effectiveThreads; i < totalThreads; ++i) {
        dst[i] = fillValue;
    }
}

static void _fillWeightedDivides(int size,
                                 int* dst,
                                 const std::vector<float>& weights,
                                 int effectiveThreads,
                                 int totalThreads,
                                 int fillValue) {
    if (effectiveThreads <= 0 || weights.size() < static_cast<size_t>(effectiveThreads)) {
        _fillUniformDivides(size, dst, effectiveThreads, totalThreads, fillValue);
        return;
    }
    int previous = 0;
    double cumulative = 0.0;
    for (int i = 0; i < effectiveThreads; ++i) {
        cumulative += static_cast<double>(size) * static_cast<double>(weights[i]);
        int boundary = (i + 1 == effectiveThreads) ? size : static_cast<int>(std::round(cumulative));
        boundary = std::max(boundary, previous);
        boundary = std::min(boundary, size);
        dst[i] = boundary;
        previous = boundary;
    }
    for (int i = effectiveThreads; i < totalThreads; ++i) {
        dst[i] = fillValue;
    }
}

static int _simulateGuidedChunks(int dynamicSize, int activeThreads, int minChunkSize) {
    int chunks = 0;
    int remaining = std::max(0, dynamicSize);
    const int divisor = std::max(1, activeThreads * 2);
    while (remaining > 0) {
        const int chunk = std::max(minChunkSize, UP_DIV(remaining, divisor));
        remaining -= chunk;
        ++chunks;
    }
    return chunks;
}

} // namespace

AutoTuner* AutoTuner::sInstance = nullptr;
std::mutex AutoTuner::sInstanceMutex;

const char* schedulerPolicyName(SchedulerPolicy policy) {
    switch (policy) {
        case SchedulerPolicy::HYBRID:
            return "hybrid";
        case SchedulerPolicy::GUIDED:
            return "guided";
        case SchedulerPolicy::DYNAMIC:
        default:
            return "dynamic";
    }
}

PhaseScheduleStats::PhaseScheduleStats() {
    reset();
}

void PhaseScheduleStats::reset() {
    op_count.store(0, std::memory_order_relaxed);
    total_tasks.store(0, std::memory_order_relaxed);
    total_static_tasks.store(0, std::memory_order_relaxed);
    total_dynamic_tasks.store(0, std::memory_order_relaxed);
    total_step_size.store(0, std::memory_order_relaxed);
    step_samples.store(0, std::memory_order_relaxed);
    total_target_chunks.store(0, std::memory_order_relaxed);
    target_chunk_samples.store(0, std::memory_order_relaxed);
    theoretical_dynamic_chunks.store(0, std::memory_order_relaxed);
    actual_dynamic_chunks.store(0, std::memory_order_relaxed);
    last_total_size.store(0, std::memory_order_relaxed);
    last_total_static.store(0, std::memory_order_relaxed);
    last_dynamic_size.store(0, std::memory_order_relaxed);
    last_step_size.store(1, std::memory_order_relaxed);
    last_target_chunks.store(0, std::memory_order_relaxed);
    last_active_threads.store(1, std::memory_order_relaxed);
    last_min_chunk_size.store(1, std::memory_order_relaxed);
    last_policy.store(static_cast<int>(SchedulerPolicy::DYNAMIC), std::memory_order_relaxed);
    for (size_t i = 0; i < static_tasks_per_thread.size(); ++i) {
        static_tasks_per_thread[i].store(0, std::memory_order_relaxed);
        dynamic_tasks_per_thread[i].store(0, std::memory_order_relaxed);
    }
}

PhaseScheduleStatsSnapshot PhaseScheduleStats::snapshot() const {
    PhaseScheduleStatsSnapshot result;
    result.op_count = op_count.load(std::memory_order_relaxed);
    result.total_tasks = total_tasks.load(std::memory_order_relaxed);
    result.total_static_tasks = total_static_tasks.load(std::memory_order_relaxed);
    result.total_dynamic_tasks = total_dynamic_tasks.load(std::memory_order_relaxed);
    result.total_step_size = total_step_size.load(std::memory_order_relaxed);
    result.step_samples = step_samples.load(std::memory_order_relaxed);
    result.total_target_chunks = total_target_chunks.load(std::memory_order_relaxed);
    result.target_chunk_samples = target_chunk_samples.load(std::memory_order_relaxed);
    result.theoretical_dynamic_chunks = theoretical_dynamic_chunks.load(std::memory_order_relaxed);
    result.actual_dynamic_chunks = actual_dynamic_chunks.load(std::memory_order_relaxed);
    result.last_total_size = last_total_size.load(std::memory_order_relaxed);
    result.last_total_static = last_total_static.load(std::memory_order_relaxed);
    result.last_dynamic_size = last_dynamic_size.load(std::memory_order_relaxed);
    result.last_step_size = last_step_size.load(std::memory_order_relaxed);
    result.last_target_chunks = last_target_chunks.load(std::memory_order_relaxed);
    result.last_active_threads = last_active_threads.load(std::memory_order_relaxed);
    result.last_min_chunk_size = last_min_chunk_size.load(std::memory_order_relaxed);
    result.last_policy = static_cast<SchedulerPolicy>(last_policy.load(std::memory_order_relaxed));
    for (size_t i = 0; i < static_tasks_per_thread.size(); ++i) {
        result.static_tasks_per_thread[i] = static_tasks_per_thread[i].load(std::memory_order_relaxed);
        result.dynamic_tasks_per_thread[i] = dynamic_tasks_per_thread[i].load(std::memory_order_relaxed);
    }
    return result;
}

AutoTuner* AutoTuner::getInstance() {
    if (sInstance == nullptr) {
        std::lock_guard<std::mutex> lock(sInstanceMutex);
        if (sInstance == nullptr) {
            sInstance = new AutoTuner();
        }
    }
    return sInstance;
}

void AutoTuner::destroy() {
    std::lock_guard<std::mutex> lock(sInstanceMutex);
    delete sInstance;
    sInstance = nullptr;
}

AutoTuner::AutoTuner()
    : mPrefillParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1)
    , mDecodeParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1)
    , mDefaultExecution(1, 0)
    , mPrefillExecution(1, 0)
    , mDecodeExecution(1, 0)
    , mCurrentPhase(InferencePhase::UNKNOWN) {
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
}

void AutoTuner::refreshFallbackExecutionState() {
    int fallbackThreads = std::max(mDefaultExecution.active_threads,
                                   std::max(mPrefillExecution.active_threads, mDecodeExecution.active_threads));
    if (fallbackThreads < 1) {
        fallbackThreads = 1;
    }
    unsigned long fallbackMask = mDefaultExecution.affinity_mask;
    if (fallbackMask == 0) {
        fallbackMask = mPrefillExecution.affinity_mask | mDecodeExecution.affinity_mask;
    }
    mFallbackActiveThreadCount.store(fallbackThreads, std::memory_order_relaxed);
    mFallbackAffinityMask.store(fallbackMask, std::memory_order_relaxed);
}

void AutoTuner::updateFastPhaseState(InferencePhase phase) {
    switch (phase) {
        case InferencePhase::PREFILL:
            mCurrentAffinityMask.store(mPrefillExecution.affinity_mask, std::memory_order_relaxed);
            mCurrentActiveThreadCount.store(std::max(1, mPrefillExecution.active_threads), std::memory_order_relaxed);
            break;
        case InferencePhase::DECODE:
            mCurrentAffinityMask.store(mDecodeExecution.affinity_mask, std::memory_order_relaxed);
            mCurrentActiveThreadCount.store(std::max(1, mDecodeExecution.active_threads), std::memory_order_relaxed);
            break;
        case InferencePhase::UNKNOWN:
        default:
            mCurrentAffinityMask.store(mFallbackAffinityMask.load(std::memory_order_relaxed), std::memory_order_relaxed);
            mCurrentActiveThreadCount.store(std::max(1, mFallbackActiveThreadCount.load(std::memory_order_relaxed)),
                                            std::memory_order_relaxed);
            break;
    }
}

void AutoTuner::setPhase(InferencePhase phase) {
    mCurrentPhase.store(phase, std::memory_order_release);
    updateFastPhaseState(phase);
}

InferencePhase AutoTuner::getPhase() const {
    return mCurrentPhase.load(std::memory_order_acquire);
}

TuningParams AutoTuner::getTuningParams() const {
    switch (mCurrentPhase.load(std::memory_order_acquire)) {
        case InferencePhase::DECODE:
            return getDecodeParams();
        case InferencePhase::PREFILL:
        case InferencePhase::UNKNOWN:
        default:
            return getPrefillParams();
    }
}

TuningParams AutoTuner::getPrefillParams() const {
    return applyEnvOverrides(mPrefillParams, false);
}

TuningParams AutoTuner::getDecodeParams() const {
    return applyEnvOverrides(mDecodeParams, true);
}

void AutoTuner::setPrefillParams(float static_ratio, int dynamic_blocks) {
    setPrefillParams(TuningParams(static_ratio, dynamic_blocks, dynamic_blocks,
                                  static_ratio > 0.0f ? SchedulerPolicy::HYBRID : SchedulerPolicy::DYNAMIC,
                                  1));
}

void AutoTuner::setDecodeParams(float static_ratio, int dynamic_blocks) {
    setDecodeParams(TuningParams(static_ratio, dynamic_blocks, dynamic_blocks,
                                 static_ratio > 0.0f ? SchedulerPolicy::HYBRID : SchedulerPolicy::DYNAMIC,
                                 1));
}

void AutoTuner::setPrefillParams(const TuningParams& params) {
    mPrefillParams = sanitizeTuningParams(params);
}

void AutoTuner::setDecodeParams(const TuningParams& params) {
    mDecodeParams = sanitizeTuningParams(params);
}

void AutoTuner::setDefaultExecution(int active_threads, unsigned long affinity_mask) {
    mDefaultExecution = sanitizeExecutionParams(active_threads, affinity_mask);
    refreshFallbackExecutionState();
    if (mCurrentPhase.load(std::memory_order_acquire) == InferencePhase::UNKNOWN) {
        updateFastPhaseState(InferencePhase::UNKNOWN);
    }
}

void AutoTuner::setPrefillExecution(int active_threads, unsigned long affinity_mask) {
    mPrefillExecution = sanitizeExecutionParams(active_threads, affinity_mask);
    refreshFallbackExecutionState();
    if (mCurrentPhase.load(std::memory_order_acquire) == InferencePhase::PREFILL) {
        updateFastPhaseState(InferencePhase::PREFILL);
    }
}

void AutoTuner::setDecodeExecution(int active_threads, unsigned long affinity_mask) {
    mDecodeExecution = sanitizeExecutionParams(active_threads, affinity_mask);
    refreshFallbackExecutionState();
    if (mCurrentPhase.load(std::memory_order_acquire) == InferencePhase::DECODE) {
        updateFastPhaseState(InferencePhase::DECODE);
    }
}

void AutoTuner::resetScheduleStats(InferencePhase phase) {
    scheduleStats(phase).reset();
}

void AutoTuner::noteSchedulePlan(InferencePhase phase,
                                 SchedulerPolicy policy,
                                 int active_threads,
                                 int total_size,
                                 int total_static,
                                 int dynamic_size,
                                 int step_size,
                                 int target_chunks,
                                 int theoretical_dynamic_chunks,
                                 int min_chunk_size) {
    if (!mnn_hybrid_instrumentation_enabled()) {
        return;
    }
    auto& stats = scheduleStats(phase);
    stats.op_count.fetch_add(1, std::memory_order_relaxed);
    stats.total_tasks.fetch_add(total_size, std::memory_order_relaxed);
    stats.total_static_tasks.fetch_add(total_static, std::memory_order_relaxed);
    stats.total_dynamic_tasks.fetch_add(dynamic_size, std::memory_order_relaxed);
    stats.total_step_size.fetch_add(step_size, std::memory_order_relaxed);
    stats.step_samples.fetch_add(1, std::memory_order_relaxed);
    stats.total_target_chunks.fetch_add(target_chunks, std::memory_order_relaxed);
    stats.target_chunk_samples.fetch_add(1, std::memory_order_relaxed);
    stats.theoretical_dynamic_chunks.fetch_add(theoretical_dynamic_chunks, std::memory_order_relaxed);
    stats.last_total_size.store(total_size, std::memory_order_relaxed);
    stats.last_total_static.store(total_static, std::memory_order_relaxed);
    stats.last_dynamic_size.store(dynamic_size, std::memory_order_relaxed);
    stats.last_step_size.store(std::max(1, step_size), std::memory_order_relaxed);
    stats.last_target_chunks.store(target_chunks, std::memory_order_relaxed);
    stats.last_active_threads.store(std::max(1, active_threads), std::memory_order_relaxed);
    stats.last_min_chunk_size.store(std::max(1, min_chunk_size), std::memory_order_relaxed);
    stats.last_policy.store(static_cast<int>(policy), std::memory_order_relaxed);
}

void AutoTuner::noteStaticRange(InferencePhase phase, int thread_id, int start, int end) {
    if (!mnn_hybrid_instrumentation_enabled()) {
        return;
    }
    if (thread_id < 0 || thread_id >= MNN_MAX_SCHEDULER_THREADS || end <= start) {
        return;
    }
    scheduleStats(phase).static_tasks_per_thread[thread_id].fetch_add(end - start, std::memory_order_relaxed);
}

void AutoTuner::noteDynamicRange(InferencePhase phase, int thread_id, int start, int end) {
    if (!mnn_hybrid_instrumentation_enabled()) {
        return;
    }
    if (end <= start) {
        return;
    }
    auto& stats = scheduleStats(phase);
    stats.actual_dynamic_chunks.fetch_add(1, std::memory_order_relaxed);
    if (thread_id >= 0 && thread_id < MNN_MAX_SCHEDULER_THREADS) {
        stats.dynamic_tasks_per_thread[thread_id].fetch_add(end - start, std::memory_order_relaxed);
    }
}

PhaseScheduleStatsSnapshot AutoTuner::getScheduleStats(InferencePhase phase) const {
    return scheduleStats(phase).snapshot();
}

std::string AutoTuner::formatScheduleStats(InferencePhase phase) const {
    const auto snapshot = getScheduleStats(phase);
    const double avg_step = snapshot.step_samples > 0
        ? static_cast<double>(snapshot.total_step_size) / static_cast<double>(snapshot.step_samples)
        : 0.0;
    const double avg_target_chunks = snapshot.target_chunk_samples > 0
        ? static_cast<double>(snapshot.total_target_chunks) / static_cast<double>(snapshot.target_chunk_samples)
        : 0.0;
    std::ostringstream stream;
    stream << "policy=" << schedulerPolicyName(snapshot.last_policy)
           << " ops=" << snapshot.op_count
           << " total=" << snapshot.total_tasks
           << " static=" << snapshot.total_static_tasks
           << " dynamic=" << snapshot.total_dynamic_tasks
           << " avg_step=" << avg_step
           << " avg_target_chunks=" << avg_target_chunks
           << " theo_chunks=" << snapshot.theoretical_dynamic_chunks
           << " actual_chunks=" << snapshot.actual_dynamic_chunks
           << " last={total=" << snapshot.last_total_size
           << ",static=" << snapshot.last_total_static
           << ",dynamic=" << snapshot.last_dynamic_size
           << ",step=" << snapshot.last_step_size
           << ",target=" << snapshot.last_target_chunks
           << ",threads=" << snapshot.last_active_threads
           << ",min=" << snapshot.last_min_chunk_size
           << "} static_loads=";
    appendThreadLoads(stream, snapshot.static_tasks_per_thread);
    stream << " dynamic_loads=";
    appendThreadLoads(stream, snapshot.dynamic_tasks_per_thread);
    return stream.str();
}

void AutoTuner::reset() {
    mPrefillParams = TuningParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1);
    mDecodeParams = TuningParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1);
    mDefaultExecution = ExecutionParams(1, 0);
    mPrefillExecution = ExecutionParams(1, 0);
    mDecodeExecution = ExecutionParams(1, 0);
    mCurrentPhase.store(InferencePhase::UNKNOWN, std::memory_order_release);
    mPrefillScheduleStats.reset();
    mDecodeScheduleStats.reset();
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
}

PhaseScheduleStats& AutoTuner::scheduleStats(InferencePhase phase) {
    switch (phase) {
        case InferencePhase::DECODE:
            return mDecodeScheduleStats;
        case InferencePhase::PREFILL:
        case InferencePhase::UNKNOWN:
        default:
            return mPrefillScheduleStats;
    }
}

const PhaseScheduleStats& AutoTuner::scheduleStats(InferencePhase phase) const {
    switch (phase) {
        case InferencePhase::DECODE:
            return mDecodeScheduleStats;
        case InferencePhase::PREFILL:
        case InferencePhase::UNKNOWN:
        default:
            return mPrefillScheduleStats;
    }
}

void registerCPUOps();
ErrorCode CastWrapExecution::onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    auto convertType = mRunType == DataType_DT_INT8 ? CPUCastCreator::FlOAT_TO_INT8 : CPUCastCreator::INT8_TO_FlOAT;
    auto cpuBackend = ((CPUBackend*)backend());
    CPUCastCreator::cast(inputs[0], outputs[0], cpuBackend, convertType);
    return NO_ERROR;
}
void CPUBackend::computeDivideSizes(int size, int* dst, float avgDiv) const {
    const bool instrument = mnn_hybrid_instrumentation_enabled();
    if (instrument) {
        begin_trace_marker("CPUBackend::computeDivideSizes");
        g_task_count.fetch_add(1, std::memory_order_relaxed);
        g_divide_size_total.fetch_add(size, std::memory_order_relaxed);
        g_divide_size_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (mGroupWithComputeRate.size() <= 1 || (avgDiv > 0 && avgDiv < mComputeI)) {
        int length = UP_DIV(size, mThreadNumber);
        int cur = length;
        for (int i = 0; i < mThreadNumber; ++i) {
            dst[i] = cur;
            cur += length;
            cur = ALIMIN(cur, size);
        }
        if (instrument) {
            g_small_task_count.fetch_add(1, std::memory_order_relaxed);
            end_trace_marker();
        }
        return;
    }

    int cur = 0;
    int curPos = 0;
    for (auto& group : mGroupWithComputeRate) {
        int currentGroupTotal = static_cast<int>(ceilf(static_cast<float>(size) * group.first));
        int length = UP_DIV(currentGroupTotal, group.second);
        for (int i = 0; i < group.second; ++i) {
            cur += length;
            cur = ALIMIN(cur, size);
            dst[curPos + i] = cur;
        }
        curPos += group.second;
    }
    if (instrument) {
        end_trace_marker();
    }
}

DivideSchedulePlan CPUBackend::computeDivideSizesHybrid(int size, int* dst, float avgDiv) const {
    const bool instrument = mnn_hybrid_instrumentation_enabled();
    if (instrument) {
        begin_trace_marker("CPUBackend::computeDivideSizesHybrid");
        g_task_count.fetch_add(1, std::memory_order_relaxed);
        g_divide_size_total.fetch_add(size, std::memory_order_relaxed);
        g_divide_size_count.fetch_add(1, std::memory_order_relaxed);
    }
    DivideSchedulePlan plan;
    plan.total_size = size;
    plan.active_threads = _effectiveThreadCount(mThreadNumber);

    auto* tuner = AutoTuner::getInstance();
    const bool isDecodeFeatures = avgDiv > 0 && avgDiv < mComputeI;
    const TuningParams params = isDecodeFeatures ? tuner->getDecodeParams() : tuner->getPrefillParams();
    plan.policy = params.policy;
    plan.min_chunk_size = std::max(1, params.min_chunk_size);
    plan.target_chunks = params.dynamic_target_chunks > 0
        ? params.dynamic_target_chunks
        : (isDecodeFeatures ? std::max(1, plan.active_threads * 2) : std::max(1, plan.active_threads * 4));

    if (size <= 0 || plan.active_threads <= 1) {
        computeDivideSizes(size, dst, avgDiv);
        plan.total_static = size;
        plan.dynamic_size = 0;
        plan.step_size = 1;
        if (instrument) {
            g_small_task_count.fetch_add(1, std::memory_order_relaxed);
            end_trace_marker();
        }
        return plan;
    }

    const auto* cpuInfo = MNNGetCPUInfo();
    const auto weights = _activeThreadWeights(cpuInfo, tuner->getFastAffinityMask(), plan.active_threads);
    if (!forceHybridScheduling(isDecodeFeatures) && !_hasWeightVariance(weights)) {
        computeDivideSizes(size, dst, avgDiv);
        plan.total_static = size;
        plan.dynamic_size = 0;
        plan.step_size = 1;
        if (instrument) {
            g_small_task_count.fetch_add(1, std::memory_order_relaxed);
            end_trace_marker();
        }
        return plan;
    }

    float staticRatio = std::max(0.0f, std::min(1.0f, params.static_ratio));
    if (plan.policy == SchedulerPolicy::DYNAMIC) {
        staticRatio = 0.0f;
    } else if (plan.policy == SchedulerPolicy::GUIDED) {
        const bool smallTask = size < plan.active_threads * 2;
        const bool highVariance = _hasHighWeightVariance(weights);
        if (smallTask || (highVariance && size < plan.active_threads * 4)) {
            staticRatio = 0.0f;
        }
    }
    plan.total_static = std::min(size, std::max(0, static_cast<int>(std::floor(size * staticRatio))));
    if (plan.total_static > 0) {
        computeDivideSizes(plan.total_static, dst, avgDiv);
    } else {
        std::fill_n(dst, mThreadNumber, 0);
    }

    plan.dynamic_size = std::max(0, size - plan.total_static);
    if (plan.dynamic_size <= 0) {
        plan.step_size = 1;
        if (instrument) {
            end_trace_marker();
        }
        return plan;
    }

    if (plan.policy == SchedulerPolicy::GUIDED) {
        plan.step_size = std::max(plan.min_chunk_size, UP_DIV(plan.dynamic_size, std::max(1, plan.active_threads * 2)));
        plan.theoretical_dynamic_chunks = _simulateGuidedChunks(plan.dynamic_size,
                                                                plan.active_threads,
                                                                plan.min_chunk_size);
    } else {
        if (plan.target_chunks <= 0) {
            plan.target_chunks = std::max(1, plan.active_threads * 2);
        }
        plan.step_size = std::max(plan.min_chunk_size, UP_DIV(plan.dynamic_size, plan.target_chunks));
        plan.step_size = std::min(plan.dynamic_size, plan.step_size);
        plan.theoretical_dynamic_chunks = UP_DIV(plan.dynamic_size, std::max(1, plan.step_size));
    }
    if (instrument) {
        end_trace_marker();
    }
    return plan;
}

void CPUBackend::initDynamicTaskState(int static_end,
                                      int total_size,
                                      int step_size,
                                      SchedulerPolicy policy,
                                      int active_threads,
                                      int target_chunks,
                                      int min_chunk_size) const {
    auto* tuner = AutoTuner::getInstance();
    const TuningParams params = tuner->getTuningParams();
    if (active_threads <= 0) {
        active_threads = _effectiveThreadCount(mThreadNumber);
    }
    if (target_chunks <= 0) {
        target_chunks = params.dynamic_target_chunks > 0
            ? params.dynamic_target_chunks
            : ((tuner->getPhase() == InferencePhase::DECODE) ? std::max(1, active_threads * 2) : std::max(1, active_threads * 4));
    }
    if (policy == SchedulerPolicy::DYNAMIC && params.policy != SchedulerPolicy::DYNAMIC) {
        policy = params.policy;
    }
    if (min_chunk_size < 1) {
        min_chunk_size = std::max(1, params.min_chunk_size);
    }

    mDynamicState.cursor.store(static_end, std::memory_order_release);
    mDynamicState.end = total_size;
    mDynamicState.step_size = std::max(1, step_size);
    mDynamicState.min_step_size = std::max(1, min_chunk_size);
    mDynamicState.active_threads = std::max(1, active_threads);
    mDynamicState.target_chunks = std::max(0, target_chunks);
    mDynamicState.policy = policy;
    const int dynamicSize = std::max(0, total_size - static_end);
    const int theoreticalChunks = (policy == SchedulerPolicy::GUIDED)
        ? _simulateGuidedChunks(dynamicSize, mDynamicState.active_threads, mDynamicState.min_step_size)
        : (dynamicSize > 0 ? UP_DIV(dynamicSize, std::max(1, mDynamicState.step_size)) : 0);
    tuner->noteSchedulePlan(tuner->getPhase(),
                            policy,
                            mDynamicState.active_threads,
                            total_size,
                            static_end,
                            dynamicSize,
                            mDynamicState.step_size,
                            mDynamicState.target_chunks,
                            theoreticalChunks,
                            mDynamicState.min_step_size);
}

std::pair<int, int> CPUBackend::fetchDynamicChunk() const {
    if (mDynamicState.policy == SchedulerPolicy::GUIDED) {
        while (true) {
            int start = mDynamicState.cursor.load(std::memory_order_acquire);
            if (start >= mDynamicState.end) {
                return {0, 0};
            }
            const int remaining = mDynamicState.end - start;
            const int guidedStep = std::max(mDynamicState.min_step_size,
                                            UP_DIV(remaining, std::max(1, mDynamicState.active_threads * 2)));
            const int end = std::min(mDynamicState.end, start + guidedStep);
            int expected = start;
            if (mDynamicState.cursor.compare_exchange_weak(expected, end, std::memory_order_acq_rel)) {
                return {start, end};
            }
        }
    }

    const int step = std::max(1, mDynamicState.step_size);
    const int start = mDynamicState.cursor.fetch_add(step, std::memory_order_acq_rel);
    if (start >= mDynamicState.end) {
        return {0, 0};
    }
    return {start, std::min(mDynamicState.end, start + step)};
}

bool CPUBackend::hasDynamicTasks() const {
    return mDynamicState.cursor.load(std::memory_order_acquire) < mDynamicState.end;
}

void CPURuntime::_bindCPUCore() const {
    if (mCpuIds.empty()) {
        return;
    }
    auto tid = MNNGetCurrentPid();
    if (tid == mCurrentTID) {
        return;
    }
    mCurrentTID = tid;
    // Bind CPU Core
    std::vector<std::pair<const int*, int>> lockCPUIndexes(mThreadNumber);
    for (int v=0; v<mThreadNumber; ++v) {
        lockCPUIndexes[v] = std::make_pair(mCpuIds.data(), mCpuIds.size());
    }
    // Set CPU Affinity
#ifdef _OPENMP
    auto threadsNumber = mThreadNumber;
    std::vector<int> result(threadsNumber, 0);
#pragma omp parallel for
    for (int i = 0; i < threadsNumber; ++i) {
        result[i] = MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
    }
#endif
#ifdef MNN_USE_THREAD_POOL
    if (nullptr != mThreadPool) {
        mThreadPool->active();
        mThreadPool->enqueue(std::make_pair([&](int i) {
            MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
            return 0;
        }, mThreadNumber), mTaskIndex);
        mThreadPool->deactive();
    }
#endif
}

void CPURuntime::_resetThreadPool() const {
    mThreadNumber = std::max(1, mThreadNumber);
    mThreadNumber = std::min(mThreadNumber, MAX_THREAD_NUMBER);
#ifdef MNN_USE_THREAD_POOL
    if (mThreadNumber > 1) {
        mThreadNumber = ALIMIN(ThreadPool::init(mThreadNumber, mCpuMask, mThreadPool), mThreadNumber);
    }
#endif
    // Reset tid to rebind cpu if necessary
    mCurrentTID = 0;
}
void CPURuntime::_validateCpuIds() const{
    bool valid = true;

    do {
        if (mCpuIds.empty()) {
            valid = false;
            break;
        }

        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.empty()) {
            valid = false;
            break;
        }

        std::unordered_map<int, bool> cpuLittleMap;
        for (auto id : cpuInfo->groups[0].ids) {
            cpuLittleMap[id] = true;
        }
        for (size_t i = 1; i < cpuInfo->groups.size(); i++) {
            for (auto id : cpuInfo->groups[i].ids) {
                cpuLittleMap[id] = false;
            }
        }

        if (cpuLittleMap.find(mCpuIds[0]) == cpuLittleMap.end()) {
            MNN_ERROR("CPU ID %d is not valid. CpuIds will not be used.\n", mCpuIds[0]);
            valid = false;
            break;
        }

        auto cpuLittle = cpuLittleMap[mCpuIds[0]];
        for (size_t i = 1; i < mCpuIds.size(); i++) {
            if (cpuLittleMap.find(mCpuIds[i]) == cpuLittleMap.end()) {
                MNN_ERROR("CPU ID %d is not valid. CpuIds will not be used.\n", mCpuIds[i]);
                valid = false;
                break;
            }
            // Using the same group of CPU cores helps maximize multi thread performance.
            // Mixing little cores with others can lead to significant performance degradation, so it is strictly prohibited.
            // Even on architectures with more than two clusters, when little cores are not involved,
            // it's still strongly recommended to avoid cross-cluster usage between different big core groups.
            if (cpuLittleMap[mCpuIds[i]] != cpuLittle) {
                MNN_ERROR("CPU ID %d and %d are not from the same group. CpuIds will not be used.\n", mCpuIds[0], mCpuIds[i]);
                valid = false;
                break;
            }
        }

    } while (false);

    if(!valid) {
        mCpuIds.clear();
    }

    if(mCpuIds.empty()) {
        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.size() == 0) {
            return;
        }
        switch (mPower) {
            case BackendConfig::Power_Low:
                    mCpuIds = cpuInfo->groups[0].ids;
                break;
            case BackendConfig::Power_High: {
                int selectCPUSize = 0;
                int groupIndex = cpuInfo->groups.size() - 1;
                while (selectCPUSize < mThreadNumber && groupIndex >= 0) {
                    auto& group = cpuInfo->groups[groupIndex];
                    mCpuIds.insert(mCpuIds.end(), group.ids.begin(), group.ids.end());
                    groupIndex--;
                    selectCPUSize += group.ids.size();
                }
            }
                break;
            default:
                break;
        }
    }
}
void CPURuntime::onReset(int numberThread, const BackendConfig* config, bool full) {
    if (config != nullptr) {
        mPower = config->power;
        if (full) {
            mPrecision = config->precision;
            mMemory = config->memory;
            mFlags = config->flags;
        }
    }
    mThreadNumber = numberThread;
    mCpuIds = hint().cpuIds;
    _validateCpuIds();
    // mCpuMask = MNNGetCPUMask(mCpuIds);
    if (mCpuMask == 0) {
        mCpuMask = MNNGetCPUMask(mCpuIds);
    }
    _resetThreadPool();
    AutoTuner::getInstance()->setDefaultExecution(mThreadNumber, mCpuMask);
}

CPURuntime::CPURuntime(const Backend::Info& info) {
    auto rawAlloc = BufferAllocator::Allocator::createDefault();
    mStaticAllocator.reset(new EagerBufferAllocator(rawAlloc));
    mDynamic.resize(MNN_CPU_MAX_BUFFER_INDEX);
    for (auto& buf : mDynamic) {
        buf.root = rawAlloc;
    }
    mThreadNumber = info.numThread;
    mCpuMask = info.cpuMask;
    AutoTuner::getInstance()->setDefaultExecution(std::max(1, mThreadNumber), mCpuMask);
    mPower   = BackendConfig::Power_Normal;
    mMemory  = BackendConfig::Memory_Normal;
    mPrecision = BackendConfig::Precision_Normal;
    mCpuIds.clear();
    if (info.user != nullptr) {
        mPrecision = info.user->precision;
        mPower = info.user->power;
        mMemory = info.user->memory;
        mFlags = info.user->flags;
    }
#ifdef LOG_VERBOSE
    MNN_PRINT("create CPURuntime:%p\n", this);
#endif
}

CPURuntime:: ~ CPURuntime() {
    // Do nothing
}
float CPURuntime::onGetMemoryInMB() {
    auto staticMemoryInMB = mStaticAllocator->totalSize() / 1024.0f / 1024.0f;
    float dynamicMemoryInMB = 0.0f;
    for (auto& buf : mDynamic) {
        dynamicMemoryInMB += buf.currentSize / 1024.0f / 1024.0f;
    }
    return staticMemoryInMB + dynamicMemoryInMB;
}
bool CPURuntime::onCheckInfo(Backend::Info& info) const {
    info.numThread = mThreadNumber;
    return true;
}
SingleBufferWithAllocator* CPURuntime::buffer(int index) const {
    if (mDynamicMmap.empty()) {
        return mDynamic.data() + index;
    }
    return mDynamicMmap.data() + index;
}

Backend* CPURuntime::onCreate(const BackendConfig* config, Backend* origin) const {
    {
        mCpuIds = hint().cpuIds;
        _validateCpuIds();
        // 【修改前】
        // mCpuMask = MNNGetCPUMask(mCpuIds);

        // 【修改后】同样的逻辑，保护 mCpuMask
        if (mCpuMask == 0) {
            mCpuMask = MNNGetCPUMask(mCpuIds);
        }
        _resetThreadPool();
        AutoTuner::getInstance()->setDefaultExecution(mThreadNumber, mCpuMask);
    }
    if (hint().midMemoryPath.size() > 0) {
        if (mDynamicMmap.empty()) {
            // Only support set featuremap dir once
            mDynamicMmap.resize(2);
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().midMemoryPath.c_str(), "", "dynamic");
            for (auto& buf : mDynamicMmap) {
                buf.root = mmapMem;
            }
        }
    }
    if (hint().weightMemoryPath.size() > 0) {
        // forward_type, precision_type, memory_type, power_type
        std::string prefix = "0_0_0_0_";
        prefix[2] += mPrecision;
        prefix[4] += mMemory;
        prefix[6] += mPower;
        // prefix += hint().modelUUID + "_";
        bool autoRemove = true;
        if (hint().useCachedMmap) {
            autoRemove = false;
            std::string fileName = MNNFilePathConcat(hint().weightMemoryPath, prefix + "sync.static");
            const_cast<RuntimeHint&>(hint()).useCachedMmap += MNNFileExist(fileName.c_str());
        }
        if (nullptr == mStaticAllocatorMMap.get()) {
            // Only support set weightmap dir once
            mStaticAllocatorRaw = mStaticAllocator;
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().weightMemoryPath.c_str(), prefix.c_str(), "static", autoRemove);
            size_t mmapSize = static_cast<size_t>(hint().mmapFileSize) * 1024 * 1024;
            mStaticAllocator.reset(new EagerBufferAllocator(mmapMem, 32, mmapSize));
            mStaticAllocatorMMap = mStaticAllocator;
        }
    }
    auto precision = mPrecision;
    auto memory = mMemory;
    size_t flags = mFlags;
    if (nullptr != origin) {
        auto cpuBn = static_cast<CPUBackend*>(origin);
        mSharedDmaInfo = cpuBn->mDmaInfo;
    }
    if (nullptr != config) {
        precision = config->precision;
        flags = config->flags;
        memory = config->memory;
    }
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend was created by runtime:%p\n", this);
#endif
    CPUBackend* res = nullptr;
    auto initThreadNumber = hint().initThreadNumber;
    do {
#ifdef MNN_USE_ARMV82
        auto core = MNNGetCoreFunctions();
        if (core->supportFp16arith && precision == BackendConfig::Precision_Low) {
            res = new Arm82Backend(this, memory);
            if (hint().useArmSme2Cores && res->threadNumber() <= 2 && core->supportSME2 && res->functions()->sme2Int8MatmulRelatedFuncionsHp32.Int8GemmKernel) {
                res->mRelatedFunctions = &(res->functions()->sme2Int8MatmulRelatedFuncionsHp32);
            } else {
                res->mRelatedFunctions = &(res->functions()->int8MatmulRelatedFunctions);
            }
            break;
        }
#endif
#ifdef MNN_SUPPORT_BF16
        if (precision == BackendConfig::Precision_Low_BF16 && BF16Functions::get()) {
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU_EXTENSION);
            res->mCoreFunctions = BF16Functions::get();
            break;
        }
#endif
        if (flags == MNN_CPU_USE_DEFAULT_BACKEND) {
            // Default don't use multi-thread init
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU);
            break;
        }
#ifdef MNN_USE_SSE
        if (AVX2Backend::isValid()) {
            res = new AVX2Backend(this, memory, flags);
            break;
        }
#endif
        res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU, flags);
    } while (false);
    mSharedDmaInfo = nullptr;
    res->setMetaPtr(pMeta);
    return res;
}

int CPURuntime::onGetRuntimeStatus(RuntimeStatus statusEnum) const {
    switch (statusEnum) {
        case STATUS_SUPPORT_FP16: {
            return MNNGetCoreFunctions()->supportFp16arith;
            break;
        }
        case STATUS_SUPPORT_DOT_PRODUCT: {
            return MNNGetCoreFunctions()->supportSDot;
            break;
        }
        default: {
            MNN_ERROR("unsupported interface");
            break;
        }
    }

    return 0;
}

void CPURuntime::onGabageCollect(int level) {
    mStaticAllocator->release(false);
    if (nullptr != mStaticAllocatorMMap) {
        mStaticAllocatorMMap->release(false);
    }
    if (level >= 100) {
        for (auto& buf : mDynamic) {
            buf.release();
        }
    }
}


void CPURuntime::onConcurrencyBegin() const {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex < 0 && nullptr != mThreadPool) {
        mTaskIndex = mThreadPool->acquireWorkIndex();
    }
    if (mTaskIndex >= 0) {
        // mThreadOpen 0 -> 1, active ThreadPool
        // For next onConcurrencyBegin, will only add mThreadOpen
        if (0 == mThreadOpen) {
            mThreadPool->active();
        }
        mThreadOpen++;
    }
#else
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(mThreadNumber);
#endif
#endif
    _bindCPUCore();
}

void CPURuntime::onConcurrencyEnd() const {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex >= 0) {
        mThreadOpen--;
        mThreadOpen = mThreadOpen < 0 ? 0 : mThreadOpen;
        if (0 == mThreadOpen) {
            mThreadPool->releaseWorkIndex(mTaskIndex);
            mThreadPool->deactive();
            mTaskIndex = -1;
        }
    }
#endif
}

std::map<OpType, CPUBackend::Creator*>* CPUBackend::gCreator = nullptr;
void CPUBackend::initCreatorMap() {
    gCreator = new std::map<OpType, CPUBackend::Creator*>;
}

bool CPUBackend::addCreator(OpType t, Creator* c) {
    auto map = gCreator;
    if (map->find(t) != map->end()) {
        MNN_PRINT("Error: %d type has be added\n", t);
        return false;
    }
    map->insert(std::make_pair(t, c));
    return true;
}
BufferAllocator* CPURuntime::createDynamicBufferAlloctor(int index) const {
    if (hint().memoryAllocatorType == Runtime::Allocator_Defer) {
        return new DeferBufferAllocator(buffer(index));
    }
    if (nullptr != mStaticAllocatorRaw.get()) {
        return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocatorRaw.get()));
    }
    return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocator.get()));
}
CPUBackend::CPUBackend(const CPURuntime* runtime, BackendConfig::PrecisionMode precision, BackendConfig::MemoryMode memory, MNNForwardType type, size_t flags) : Backend(type) {
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend create\n");
#endif
    mMemory = memory;
    mRuntime = const_cast<CPURuntime*>(runtime);
    auto core = MNNGetCoreFunctions();
    mThreadNumber = mRuntime->mThreadNumber;
    if (mRuntime->hint().useArmSme2Cores && core->supportSME2 && core->sme2Int8MatmulRelatedFuncionsHp32.Int8GemmKernel) {
        mThreadNumber = ALIMIN(2, mThreadNumber);
        mRelatedFunctions = &core->sme2Int8MatmulRelatedFuncionsHp32;
    } else {
        mRelatedFunctions = &core->int8MatmulRelatedFunctions;
    }
    // Compute Group Rate
    do {
        if (mThreadNumber <= 1 || mRuntime->mPower == BackendConfig::Power_Low) {
            break;
        }
        auto rate = mRuntime->hint().cpuDecreaseRate;
        if (rate >= 100 || rate <= 0) {
            break;
        }
        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.size() < 2) {
            break;
        }
        if (cpuInfo->i8mm) {
            mComputeI = 28.f;
        } else if (cpuInfo->dot) {
            mComputeI = 14.f;
        } else {
            mComputeI = 7.f;
        }
        mGroupWithComputeRate.clear();
        float decreaseRate = static_cast<float>(rate) / 100.0f;
        int validCpuSize = static_cast<int>(cpuInfo->groups[cpuInfo->groups.size() - 1].ids.size());
        int groupIndex = static_cast<int>(cpuInfo->groups.size()) - 2;
        validCpuSize = ALIMIN(validCpuSize, mThreadNumber);
        float totalComputeRate = 1.0f * validCpuSize;
        mGroupWithComputeRate.emplace_back(std::make_pair(totalComputeRate, validCpuSize));
        float currentRate = 1.0f;
        while (validCpuSize < mThreadNumber && groupIndex >= 0) {
            auto& group = cpuInfo->groups[groupIndex];
            int selectSize = ALIMIN(mThreadNumber - validCpuSize, static_cast<int>(group.ids.size()));
            validCpuSize += group.ids.size();
            currentRate *= decreaseRate;
            totalComputeRate += currentRate * selectSize;
            mGroupWithComputeRate.emplace_back(std::make_pair(currentRate * selectSize, selectSize));
            groupIndex--;
        }
        for (auto& g : mGroupWithComputeRate) {
            g.first = g.first / totalComputeRate;
        }
    } while (false);
    auto dynamicAlloc = mRuntime->mSharedDmaInfo;
    if (nullptr == dynamicAlloc.get()) {
        mDmaInfo.reset(new CPURuntime::DynamicAllocator);
        mDmaInfo->mDynamicAllocator.reset(mRuntime->createDynamicBufferAlloctor(0));
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocator.get();
        mDmaInfo->mCacheGroup.resize(MNN_CPU_MAX_BUFFER_INDEX);
        for (int i=0; i<mDmaInfo->mCacheGroup.size(); ++i) {
            mDmaInfo->mCacheGroup[i].reset(new CPUResizeCache);
        }
    } else {
        mDmaInfo = dynamicAlloc;
    }
    mPrecisionMode = precision;
    mCoreFunctions = MNNGetCoreFunctions();
    mInt8CoreFunctions = MNNGetInt8CoreFunctions();
    mCache = mDmaInfo->mCacheGroup[0].get();
}

CPUBackend::~CPUBackend() {
    // Do nothing
}
void CPUBackend::_resetDynamicMemory() const {
    mRuntime->pCurrentStatus = mDmaInfo->mDynamicAllocator->apply();
    if (NO_ERROR != mRuntime->pCurrentStatus) {
        return;
    }
    if (nullptr != mDmaInfo->mDynamicAllocatorBackup.get()) {
        mRuntime->pCurrentStatus  = mDmaInfo->mDynamicAllocatorBackup->apply();
    }
}

void CPUBackend::onExecuteBegin() const {
    mInitWorkQueue.reset();
    _resetDynamicMemory();
}

void CPUBackend::onExecuteEnd() const {
    // Do nothing
}

void CPUBackend::onResizeBegin() {
    mDmaInfo->mCurrentDynamicAllocator->reset();
}
bool CPUBackend::onSelectDynamicAllocator(int index, int maxIndex) {
    if (maxIndex > 2) {
        return false;
    }
    if (maxIndex == 2 && mDmaInfo->mDynamicAllocatorBackup.get() == nullptr) {
        mDmaInfo->mDynamicAllocatorBackup.reset(mRuntime->createDynamicBufferAlloctor(1));
    }
    if (1 == index) {
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocatorBackup.get();
    } else {
        mRuntime->buffer(0)->release();
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocator.get();
    }
    mCache = mDmaInfo->mCacheGroup[index].get();
    return true;
}

ErrorCode CPUBackend::onResizeEnd() {
    getCache()->release();
    auto code = mDmaInfo->mCurrentDynamicAllocator->compute();
    if (NO_ERROR != code) {
        return code;
    }
    return NO_ERROR;
}

Backend::MemObj* CPUBackend::allocBuffer(size_t size, Tensor* dest, StorageType storageType) {
    auto originMem = TensorUtils::getDescribeOrigin(dest)->mem.get();
    if (nullptr != originMem) {
        if (static_cast<CPUMemObj*>(originMem)->getSize() >= size) {
            return originMem;
        } else {
            TensorUtils::getDescribeOrigin(dest)->mem = nullptr;
        }
    }
    // MNN_PRINT("Acquire size = %d\n", size);
    if (size <= 0) {
        MNN_PRINT("Acquire buffer size = %lu\n", size);
        MNN_ASSERT(false);
        return nullptr;
    }
    // if (size > LARGE_MEMORY) {
    //     MNN_PRINT("Size larger than 500 M :%d\n", size);
    // }
    auto& buffer = dest->buffer();
    auto des = TensorUtils::getDescribe(dest);
    MemChunk chunk;
    switch (storageType) {
        case STATIC: {
            chunk = mRuntime->mStaticAllocator->alloc(size, false);
            break;
        }
        case DYNAMIC: {
            chunk = mDmaInfo->mCurrentDynamicAllocator->alloc(size, false);
            break;
        }
        case DYNAMIC_SEPERATE: {
            chunk = mDmaInfo->mCurrentDynamicAllocator->alloc(size, true);
            break;
        }
        default:
            MNN_ASSERT(false);
            break;
    }

    if (chunk.invalid()) {
        MNN_ERROR("Alloc buffer error for cpu backend\n");
        return nullptr;
    }

    Backend::MemObj* res = nullptr;

    if (storageType == STATIC) {
        res = new CPUMemObj(mRuntime->mStaticAllocator.get(), chunk, size);
    } else {
        res = new CPUMemObj(mDmaInfo->mCurrentDynamicAllocator, chunk, size);
        chunk.attach(dest);
    }
    if (chunk.ptr()) {
        buffer.host = chunk.ptr();
    }
    des->extra.offset = 0;
    return res;
}

void CPUBackend::enqueueTask(std::function<int()>&& task) {
    if (mInitWorkQueue != nullptr) {
        mInitWorkQueue->postTask(std::move(task));
    } else {
        task();
    }
}

Backend::MemObj* CPUBackend::onAcquire(const MNN::Tensor* nativeTensorConst, StorageType storageType) {
    if (nativeTensorConst == nullptr) {
        return nullptr;
    }
    //FUNC_PRINT_ALL(nativeTensorConst, p);
    auto nativeTensor = (Tensor*)nativeTensorConst;
    auto size = getTensorSize(nativeTensor, true);
    return allocBuffer(size, nativeTensor, storageType);
}

static OpType _getRealOpType(OpType opType) {
    switch (opType) {
        case OpType_Convolution:
            return OpType_ConvInt8;
        case OpType_ConvolutionDepthwise:
            return OpType_DepthwiseConvInt8;
        case OpType_Pooling:
            return OpType_PoolInt8;

        // case OpType_Eltwise:
        //     // TODO: just support EltwiseAdd
        //     return OpType_EltwiseInt8;
        default:
            return opType;
    }
}
void* CPUBackend::onMapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* srcTensor) {
    if (static_cast<int>(getBytes(this, srcTensor)) != srcTensor->getType().bytes()) {
        return nullptr;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat) != dtype) {
        return nullptr;
    }
    _resetDynamicMemory();
    return srcTensor->host<void>();
}

bool CPUBackend::onUnmapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* dstTensor, void* mapPtr) {
    if (static_cast<int>(getBytes(this, dstTensor)) != dstTensor->getType().bytes()) {
        return false;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(dstTensor)->dimensionFormat) != dtype) {
        return false;
    }
    return true;
}

size_t CPUBackend::getTensorSize(const Tensor* tensor, bool multiBytes) const {
    auto core = mCoreFunctions;
    size_t dataSize = 1;
    auto des = TensorUtils::getDescribe(tensor);
    for (int i = 0; i < tensor->dimensions(); i++) {
        size_t currentDimSize = tensor->length(i);
        if (des->dimensionFormat == MNN_DATA_FORMAT_NC4HW4 && 1 == i) {
            currentDimSize = UP_DIV(currentDimSize, core->pack) * core->pack;
        }
        dataSize *= currentDimSize;
    }
    if (multiBytes) {
        size_t bytes = getBytes(this, tensor);
        return dataSize * bytes;
    }
    return dataSize;
}

size_t CPUBackend::getBytes(const Backend* backend, const Tensor* output) {
    size_t bytes = output->getType().bytes();
    auto core = static_cast<const CPUBackend*>(backend)->functions();
    auto quant = TensorUtils::getDescribe(output)->quantAttr.get();
    if (output->getType().code == halide_type_float) {
        bytes = core->bytes;
    }
    if (nullptr != quant && TensorUtils::getDescribe(output)->applyQuant) {
        bytes = 1;
    }
    return bytes;
}

DataType CPUBackend::getDataType(const Tensor* tensor) {
    auto des = TensorUtils::getDescribe(tensor);
    if (nullptr == des->quantAttr.get() || (!des->applyQuant)) {
        return DataType_DT_FLOAT;
    }
    return des->quantAttr->type;
}

/// get execution
Execution* CPUBackend::onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op) {
    /**
     BatchNorm it will be converted to scale
     for model convert, don't print error log
     */
    if (op->type() == OpType_BatchNorm) {
        return nullptr;
    }
    auto opType = op->type();
    if (outputs.size() > 0 && inputs.size() > 0) {
        bool outputQuant = TensorUtils::getDescribe(outputs[0])->quantAttr != nullptr && TensorUtils::getDescribe(outputs[0])->quantAttr->type == DataType_DT_INT8;
        bool inputQuant = TensorUtils::getDescribe(inputs[0])->quantAttr != nullptr && TensorUtils::getDescribe(inputs[0])->quantAttr->type == DataType_DT_INT8;
        if (inputQuant && outputQuant) {
            opType = _getRealOpType(opType);
        }
    }

    // TODO: rm this convert when merge diff datatyoe of op
    auto map  = gCreator;
    auto iter = map->find(opType);
    if (iter == map->end() ) {
        MNN_PRINT("Don't support type [%s]\n", MNN::EnumNameOpType(op->type()));
        return nullptr;
    }
    Execution* exe = nullptr;
    bool needCast = false;
    if (exe == nullptr) {
        exe = iter->second->onCreate(inputs, outputs, op, this);
    }
    return exe;
}
const Runtime* CPUBackend::getRuntime() {
    return mRuntime;
}

bool CPUBackend::onClearBuffer() {
    if (nullptr != mRuntime->mStaticAllocatorRaw.get()) {
        mRuntime->mStaticAllocator->sync();
        mRuntime->mStaticAllocator = mRuntime->mStaticAllocatorRaw;
        mRuntime->mStaticAllocatorRaw = nullptr;
    }
    mCache->reset();
    mDmaInfo->mCurrentDynamicAllocator->release(true);
    return true;
}

std::pair<int, int> CPUBackend::multiThreadDivide(int size) const {
    int sizeDivide = size / threadNumber();
    sizeDivide = UP_DIV(sizeDivide, mCoreFunctions->pack) * mCoreFunctions->pack;
    int scheduleNumber = 1;
    if (sizeDivide > 0) {
        scheduleNumber = UP_DIV(size, sizeDivide);
    }
    return std::make_pair(sizeDivide, scheduleNumber);
}
void CPUBackend::onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const {
    _resetDynamicMemory();
    auto& srcBuffer = srcTensor->buffer();
    auto& dstBuffer = dstTensor->buffer();
    if (srcBuffer.dimensions != dstBuffer.dimensions ) {
        if (srcBuffer.dim[srcBuffer.dimensions - 1].extent != 1 && dstBuffer.dim[dstBuffer.dimensions - 1].extent != 1) {
            MNN_ERROR("srcBuffer dimension not equal to dstBuffer, can't copy buffer\n");
        }
    }
    if (srcTensor->getDimensionType() == dstTensor->getDimensionType()) {
        for (int i = 0; i < srcBuffer.dimensions; ++i) {
            MNN_ASSERT(srcBuffer.dim[i].extent <= dstBuffer.dim[i].extent);
        }
    }
    if (nullptr == srcBuffer.host || nullptr == dstBuffer.host) {
        return;
    }
    std::unique_ptr<Tensor> wrapTensor;
    if (getDataType(srcTensor) != getDataType(dstTensor)) {
        auto dimType =  OpCommonUtils::convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat);
        auto convertType = CPUCastCreator::FlOAT_TO_INT8;
        if (getDataType(srcTensor) == DataType_DT_INT8) {
            convertType = CPUCastCreator::INT8_TO_FlOAT;
        }
        wrapTensor.reset(Tensor::createDevice(srcTensor->shape(), dstTensor->getType(), dimType));
        auto dstType = getDataType(dstTensor);
        if (dstType != DataType_DT_FLOAT) {
            wrapTensor->setType(dstType);
        }
        wrapTensor->buffer().host = (uint8_t*)MNNMemoryAllocAlign(getTensorSize(wrapTensor.get()) * wrapTensor->getType().bytes(), MNN_MEMORY_ALIGN_DEFAULT);

#ifdef LOG_VERBOSE
        MNN_PRINT("CPU backend copy tensor ptr:%p -> ptr:%p hostPtr:%p -> %p, format %d -> %d, dims: [",
        srcTensor, dstTensor, srcTensor->host<void>(), dstTensor->host<void>(), TensorUtils::getDescribe(srcTensor)->dimensionFormat, TensorUtils::getDescribe(dstTensor)->dimensionFormat);
        for (int i=0; i<srcTensor->dimensions(); ++i) {
            MNN_PRINT("%d ", srcTensor->length(i));
        }
        MNN_PRINT("]\n");
#endif

        TensorUtils::getDescribe(wrapTensor.get())->memoryType = Tensor::InsideDescribe::MEMORY_HOST;
        auto code = CPUCastCreator::cast(srcTensor, wrapTensor.get(), this, convertType);
        if (NO_ERROR != code) {
            MNN_ERROR("Error in CPUBackend::onCopyBuffer:cast\n");
        }
        srcTensor = wrapTensor.get();
    } else if (srcTensor->getType() != dstTensor->getType()) {
        MNN_ERROR("Input type not match session's tensor\n");
        return;
    }
    auto code = CPUTensorConverter::convert(srcTensor, dstTensor);
    if (NO_ERROR != code) {
        MNN_ERROR("Error in CPUBackend::onCopyBuffer:convert\n");
    }
}

class CPURuntimeCreator : public RuntimeCreator {
public:
    virtual Runtime* onCreate(const Backend::Info& info) const override {
        return new CPURuntime(info);
    }
    static bool _supportQuant(const Op* op, const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
        auto otype = op->type();
        for (auto t : inputs) {
            auto des = TensorUtils::getDescribe(t);
            if (des->quantAttr == nullptr) {
                return false;
            }
            auto type = des->quantAttr->type;
            if (type != DataType_DT_INT8) {
                return false;
            }
        }
        switch (otype) {
            case OpType_Convolution:
            case OpType_ConvolutionDepthwise:
                if (inputs.size() > 1) {
                    return false;
                }
                if (op->main_as_Convolution2D() && op->main_as_Convolution2D()->weight() != nullptr) {
                    return false;
                } else {
                    return true;
                }
            case OpType_ConvInt8:
            case OpType_DepthwiseConvInt8:
                return true;
                // case OpType_Eltwise:
            case OpType_Raster:
            {
                for (auto input : inputs) {
                    if (TensorUtils::getDescribe(input)->quantAttr.get() != TensorUtils::getDescribe(outputs[0])->quantAttr.get()) {
                        return false;
                    }
                    if (TensorUtils::getDescribe(input)->quantAttr.get() && TensorUtils::getDescribe(outputs[0])->quantAttr.get() && (TensorUtils::getDescribe(input)->quantAttr.get()->scale == 0 || TensorUtils::getDescribe(outputs[0])->quantAttr.get()->scale == 0)) {
                        return false;
                    }
                }
                return true;
            }
            case OpType_Pooling:
                if (op->main_as_Pool() && op->main_as_Pool()->type() == PoolType_MAXPOOL ) {
                    return true;
                } else if (op->main_as_Pool() && op->main_as_Pool()->type() == PoolType_AVEPOOL) {
                    return true;
                } else {
                    return false;
                }
            case OpType_Softmax:
                return true;
            case OpType_LayerNorm:
                return true;
#ifdef MNN_SUPPORT_QUANT_EXTEND
            case OpType_ReLU:
                if (TensorUtils::getDescribe(inputs[0])->quantAttr.get() != TensorUtils::getDescribe(outputs[0])->quantAttr.get()) {
                    return false;
                }
                // now just relu without slope support quant
                if ((op->main_as_Relu() == nullptr) || op->main_as_Relu()->slope() == 0.f) {
                    return true;
                } else {
                    return false;
                }
            case OpType_BinaryOp:
                return true;
            case OpType_Scale:
                return true;
            case OpType_Interp:
                return true;
            case OpType_UnaryOp:
                if (op->main_as_UnaryOp()->tableInt8() || op->main_as_UnaryOp()->opType() == UnaryOpOperation_NEG || op->main_as_UnaryOp()->opType() == UnaryOpOperation_ABS || op->main_as_UnaryOp()->opType() == UnaryOpOperation_SIGN) {
                    return true;
                } else {
                    return false;
                }
            case OpType_PReLU:
                return true;
#endif
            default:
                break;
        }
        return false;
    }
    virtual bool onSetQuantInfo(const Op* op, const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) const override {
        if (nullptr == op) {
            return true;
        }
        auto res = _supportQuant(op, inputs, outputs);
        for (auto t : outputs) {
            TensorUtils::getDescribe(t)->applyQuant = res;
        }
        return res;
    }
};


#ifdef MNN_SUPPORT_BF16
extern void registerBF16Backend();
#endif
#ifdef ENABLE_ARMV82
extern void registerArm82RuntimeCreator();
#endif
void registerCPURuntimeCreator() {
    MNNCoreFunctionInit();
    CPUBackend::initCreatorMap();
    registerCPUOps();
#ifdef MNN_SUPPORT_BF16
    registerBF16Backend();
#endif
#ifdef MNN_USE_ARMV82
    registerArm82RuntimeCreator();
#endif
    // TODO: Merge _initCoreFunction MNNFunctionInit and cpuinfo_arm_init
    MNNInsertExtraRuntimeCreator(MNN_FORWARD_CPU, new CPURuntimeCreator);
};
} // namespace MNN
