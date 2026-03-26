//
//  AutoTuner.cpp
//  MNN
//
//  Created for MNN heterogeneous scheduling optimization.
//

#include "AutoTuner.hpp"

#include <algorithm>
#include <sstream>

#include <MNN/MNNDefine.h>

namespace MNN {

namespace {

static TuningParams sanitizeTuningParams(const TuningParams& input) {
    TuningParams params = input;
    if (params.static_ratio < 0.0f) {
        params.static_ratio = 0.0f;
    }
    if (params.static_ratio > 1.0f) {
        params.static_ratio = 1.0f;
    }
    if (params.dynamic_blocks < 0) {
        params.dynamic_blocks = 0;
    }
    if (params.dynamic_target_chunks <= 0) {
        params.dynamic_target_chunks = params.dynamic_blocks;
    }
    if (params.dynamic_target_chunks < 0) {
        params.dynamic_target_chunks = 0;
    }
    if (params.min_chunk_size < 1) {
        params.min_chunk_size = 1;
    }
    return params;
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
    if (sInstance != nullptr) {
        delete sInstance;
        sInstance = nullptr;
    }
}

AutoTuner::AutoTuner()
    : mPrefillParams(0.0f, 60, 60, SchedulerPolicy::DYNAMIC, 1)
    , mDecodeParams(0.0f, 2, 2, SchedulerPolicy::DYNAMIC, 1)
    , mDefaultExecution(1, 0)
    , mPrefillExecution(1, 0)
    , mDecodeExecution(1, 0)
    , mCurrentPhase(InferencePhase::UNKNOWN)
    , mPanicMode(false) {
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
    MNN_PRINT("[AutoTuner] Initialized. Default(threads=%d, affinity=0x%lX) Prefill(policy=%s, static=%.2f, target_chunks=%d, threads=%d, affinity=0x%lX) Decode(policy=%s, static=%.2f, target_chunks=%d, threads=%d, affinity=0x%lX)\n",
              mDefaultExecution.active_threads,
              mDefaultExecution.affinity_mask,
              schedulerPolicyName(mPrefillParams.policy),
              mPrefillParams.static_ratio,
              mPrefillParams.dynamic_target_chunks,
              mPrefillExecution.active_threads,
              mPrefillExecution.affinity_mask,
              schedulerPolicyName(mDecodeParams.policy),
              mDecodeParams.static_ratio,
              mDecodeParams.dynamic_target_chunks,
              mDecodeExecution.active_threads,
              mDecodeExecution.affinity_mask);
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
            mCurrentActiveThreadCount.store(std::max(1, mFallbackActiveThreadCount.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            break;
    }
}

void AutoTuner::setPhase(InferencePhase phase) {
    InferencePhase oldPhase = mCurrentPhase.exchange(phase, std::memory_order_release);
    updateFastPhaseState(phase);
    if (oldPhase != phase) {
        const char* oldName = (oldPhase == InferencePhase::PREFILL) ? "PREFILL" :
                              (oldPhase == InferencePhase::DECODE) ? "DECODE" : "UNKNOWN";
        const char* newName = (phase == InferencePhase::PREFILL) ? "PREFILL" :
                              (phase == InferencePhase::DECODE) ? "DECODE" : "UNKNOWN";
        MNN_PRINT("[AutoTuner] Phase changed: %s -> %s (threads=%d, affinity=0x%lX)\n",
                  oldName,
                  newName,
                  getActiveThreadCount(),
                  getFastAffinityMask());
    }
}

InferencePhase AutoTuner::getPhase() const {
    return mCurrentPhase.load(std::memory_order_acquire);
}

TuningParams AutoTuner::getTuningParams() const {
    if (mPanicMode.load(std::memory_order_relaxed)) {
        return TuningParams(1.0f, 0, 0, SchedulerPolicy::HYBRID, 1);
    }

    switch (mCurrentPhase.load(std::memory_order_acquire)) {
        case InferencePhase::PREFILL:
            return mPrefillParams;
        case InferencePhase::DECODE:
            return mDecodeParams;
        case InferencePhase::UNKNOWN:
        default:
            return mPrefillParams;
    }
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
    MNN_PRINT("[AutoTuner] Prefill tuning updated: policy=%s, static_ratio=%.2f, target_chunks=%d, min_chunk=%d\n",
              schedulerPolicyName(mPrefillParams.policy),
              mPrefillParams.static_ratio,
              mPrefillParams.dynamic_target_chunks,
              mPrefillParams.min_chunk_size);
}

void AutoTuner::setDecodeParams(const TuningParams& params) {
    mDecodeParams = sanitizeTuningParams(params);
    MNN_PRINT("[AutoTuner] Decode tuning updated: policy=%s, static_ratio=%.2f, target_chunks=%d, min_chunk=%d\n",
              schedulerPolicyName(mDecodeParams.policy),
              mDecodeParams.static_ratio,
              mDecodeParams.dynamic_target_chunks,
              mDecodeParams.min_chunk_size);
}

void AutoTuner::setDefaultExecution(int active_threads, unsigned long affinity_mask) {
    if (active_threads < 1) {
        active_threads = 1;
    }
    mDefaultExecution.active_threads = active_threads;
    mDefaultExecution.affinity_mask = affinity_mask;
    refreshFallbackExecutionState();

    InferencePhase phase = mCurrentPhase.load(std::memory_order_acquire);
    if (phase == InferencePhase::UNKNOWN) {
        updateFastPhaseState(phase);
    }
    MNN_PRINT("[AutoTuner] Default execution updated: threads=%d, affinity=0x%lX\n",
              active_threads,
              affinity_mask);
}

void AutoTuner::setPrefillExecution(int active_threads, unsigned long affinity_mask) {
    if (active_threads < 1) {
        active_threads = 1;
    }
    mPrefillExecution.active_threads = active_threads;
    mPrefillExecution.affinity_mask = affinity_mask;
    refreshFallbackExecutionState();

    InferencePhase phase = mCurrentPhase.load(std::memory_order_acquire);
    if (phase == InferencePhase::PREFILL || phase == InferencePhase::UNKNOWN) {
        updateFastPhaseState(phase);
    }
    MNN_PRINT("[AutoTuner] Prefill execution updated: threads=%d, affinity=0x%lX\n",
              active_threads,
              affinity_mask);
}

void AutoTuner::setDecodeExecution(int active_threads, unsigned long affinity_mask) {
    if (active_threads < 1) {
        active_threads = 1;
    }
    mDecodeExecution.active_threads = active_threads;
    mDecodeExecution.affinity_mask = affinity_mask;
    refreshFallbackExecutionState();

    InferencePhase phase = mCurrentPhase.load(std::memory_order_acquire);
    if (phase == InferencePhase::DECODE || phase == InferencePhase::UNKNOWN) {
        updateFastPhaseState(phase);
    }
    MNN_PRINT("[AutoTuner] Decode execution updated: threads=%d, affinity=0x%lX\n",
              active_threads,
              affinity_mask);
}

void AutoTuner::setCoreCapacities(const std::vector<int>& capacities) {
    mCoreCapacities = capacities;
    MNN_PRINT("[AutoTuner] Core capacities set to: [");
    for (size_t i = 0; i < capacities.size(); ++i) {
        MNN_PRINT("%d%s", capacities[i], (i + 1 < capacities.size()) ? ":" : "");
    }
    MNN_PRINT("]\n");
}

const std::vector<int>& AutoTuner::getCoreCapacities() const {
    return mCoreCapacities;
}

void AutoTuner::setCoreRatios(const std::vector<int>& ratios) {
    setCoreCapacities(ratios);
}

const std::vector<int>& AutoTuner::getCoreRatios() const {
    return mCoreCapacities;
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
    if (thread_id < 0 || thread_id >= MNN_MAX_SCHEDULER_THREADS || end <= start) {
        return;
    }
    scheduleStats(phase).static_tasks_per_thread[thread_id].fetch_add(end - start, std::memory_order_relaxed);
}

void AutoTuner::noteDynamicRange(InferencePhase phase, int thread_id, int start, int end) {
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

void AutoTuner::feedback(float cost_time) {
    (void)cost_time;
}

void AutoTuner::setPanicMode(bool enable) {
    bool oldValue = mPanicMode.exchange(enable, std::memory_order_release);
    if (oldValue != enable) {
        MNN_PRINT(enable ? "[AutoTuner] Panic mode enabled\n" : "[AutoTuner] Panic mode disabled\n");
    }
}

bool AutoTuner::isPanicMode() const {
    return mPanicMode.load(std::memory_order_relaxed);
}

void AutoTuner::reset() {
    mPrefillParams = TuningParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1);
    mDecodeParams = TuningParams(0.0f, 0, 0, SchedulerPolicy::DYNAMIC, 1);
    mDefaultExecution = ExecutionParams(1, 0);
    mPrefillExecution = ExecutionParams(1, 0);
    mDecodeExecution = ExecutionParams(1, 0);
    mCurrentPhase.store(InferencePhase::UNKNOWN, std::memory_order_release);
    mCoreCapacities.clear();
    mPanicMode.store(false, std::memory_order_release);
    mPrefillScheduleStats.reset();
    mDecodeScheduleStats.reset();
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
    MNN_PRINT("[AutoTuner] Reset to defaults\n");
}

TuningParams AutoTuner::getDecodeParams() const {
    return mDecodeParams;
}

TuningParams AutoTuner::getPrefillParams() const {
    return mPrefillParams;
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

} // namespace MNN
