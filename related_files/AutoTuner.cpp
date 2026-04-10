//
//  AutoTuner.cpp
//  MNN
//
//  Created for MNN heterogeneous scheduling optimization.
//

#include "AutoTuner.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include <MNN/MNNDefine.h>

namespace MNN {

namespace {

static TuningParams sanitizeTuningParams(const TuningParams& input) {
    TuningParams params = input;
    if (params.dynamic_blocks < 0) {
        params.dynamic_blocks = 0;
    }
    if (params.dynamic_target_chunks <= 0) {
        params.dynamic_target_chunks = params.dynamic_blocks;
    }
    if (params.dynamic_target_chunks < 0) {
        params.dynamic_target_chunks = 0;
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

static bool scheduleInstrumentEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("MNN_ENABLE_SCHEDULE_INSTRUMENT");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

} // namespace

AutoTuner* AutoTuner::sInstance = nullptr;
std::mutex AutoTuner::sInstanceMutex;

const char* schedulerPolicyName(SchedulerPolicy policy) {
    switch (policy) {
        case SchedulerPolicy::STATIC:
            return "static";
        case SchedulerPolicy::WORK_STEAL:
            return "work_steal";
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
    total_chunks.store(0, std::memory_order_relaxed);
    local_pop_calls.store(0, std::memory_order_relaxed);
    local_pop_success.store(0, std::memory_order_relaxed);
    steal_attempts.store(0, std::memory_order_relaxed);
    steal_success.store(0, std::memory_order_relaxed);
    steal_empty.store(0, std::memory_order_relaxed);
    steal_cas_retries.store(0, std::memory_order_relaxed);
    stolen_tasks.store(0, std::memory_order_relaxed);
    dynamic_claim_calls.store(0, std::memory_order_relaxed);
    dynamic_claim_success.store(0, std::memory_order_relaxed);
    dynamic_claim_empty.store(0, std::memory_order_relaxed);
    dynamic_claim_tasks.store(0, std::memory_order_relaxed);
    last_total_size.store(0, std::memory_order_relaxed);
    last_step_size.store(1, std::memory_order_relaxed);
    last_active_threads.store(1, std::memory_order_relaxed);
    last_target_chunks.store(0, std::memory_order_relaxed);
    last_policy.store(static_cast<int>(SchedulerPolicy::DYNAMIC), std::memory_order_relaxed);
    for (size_t i = 0; i < tasks_per_thread.size(); ++i) {
        tasks_per_thread[i].store(0, std::memory_order_relaxed);
    }
}

PhaseScheduleStatsSnapshot PhaseScheduleStats::snapshot() const {
    PhaseScheduleStatsSnapshot result;
    result.op_count = op_count.load(std::memory_order_relaxed);
    result.total_tasks = total_tasks.load(std::memory_order_relaxed);
    result.total_chunks = total_chunks.load(std::memory_order_relaxed);
    result.local_pop_calls = local_pop_calls.load(std::memory_order_relaxed);
    result.local_pop_success = local_pop_success.load(std::memory_order_relaxed);
    result.steal_attempts = steal_attempts.load(std::memory_order_relaxed);
    result.steal_success = steal_success.load(std::memory_order_relaxed);
    result.steal_empty = steal_empty.load(std::memory_order_relaxed);
    result.steal_cas_retries = steal_cas_retries.load(std::memory_order_relaxed);
    result.stolen_tasks = stolen_tasks.load(std::memory_order_relaxed);
    result.dynamic_claim_calls = dynamic_claim_calls.load(std::memory_order_relaxed);
    result.dynamic_claim_success = dynamic_claim_success.load(std::memory_order_relaxed);
    result.dynamic_claim_empty = dynamic_claim_empty.load(std::memory_order_relaxed);
    result.dynamic_claim_tasks = dynamic_claim_tasks.load(std::memory_order_relaxed);
    result.last_total_size = last_total_size.load(std::memory_order_relaxed);
    result.last_step_size = last_step_size.load(std::memory_order_relaxed);
    result.last_active_threads = last_active_threads.load(std::memory_order_relaxed);
    result.last_target_chunks = last_target_chunks.load(std::memory_order_relaxed);
    result.last_policy = static_cast<SchedulerPolicy>(last_policy.load(std::memory_order_relaxed));
    for (size_t i = 0; i < tasks_per_thread.size(); ++i) {
        result.tasks_per_thread[i] = tasks_per_thread[i].load(std::memory_order_relaxed);
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
    : mPrefillParams(SchedulerPolicy::WORK_STEAL)
    , mDecodeParams(SchedulerPolicy::DYNAMIC)
    , mDefaultExecution(1, 0)
    , mPrefillExecution(1, 0)
    , mDecodeExecution(1, 0)
    , mCurrentPhase(InferencePhase::UNKNOWN)
    , mPanicMode(false) {
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
    MNN_PRINT("[AutoTuner] Initialized. Default(threads=%d, affinity=0x%lX) Prefill(policy=%s, target_chunks=%d, threads=%d, affinity=0x%lX) Decode(policy=%s, target_chunks=%d, threads=%d, affinity=0x%lX)\n",
              mDefaultExecution.active_threads,
              mDefaultExecution.affinity_mask,
              schedulerPolicyName(mPrefillParams.policy),
              mPrefillParams.dynamic_target_chunks,
              mPrefillExecution.active_threads,
              mPrefillExecution.affinity_mask,
              schedulerPolicyName(mDecodeParams.policy),
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
        return TuningParams(SchedulerPolicy::WORK_STEAL);
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

void AutoTuner::setPrefillParams(const TuningParams& params) {
    mPrefillParams = sanitizeTuningParams(params);
    MNN_PRINT("[AutoTuner] Prefill tuning updated: policy=%s, target_chunks=%d\n",
              schedulerPolicyName(mPrefillParams.policy),
              mPrefillParams.dynamic_target_chunks);
}

void AutoTuner::setDecodeParams(const TuningParams& params) {
    mDecodeParams = sanitizeTuningParams(params);
    MNN_PRINT("[AutoTuner] Decode tuning updated: policy=%s, target_chunks=%d\n",
              schedulerPolicyName(mDecodeParams.policy),
              mDecodeParams.dynamic_target_chunks);
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
                                 int step_size,
                                 int target_chunks) {
    auto& stats = scheduleStats(phase);
    stats.op_count.fetch_add(1, std::memory_order_relaxed);
    stats.total_tasks.fetch_add(total_size, std::memory_order_relaxed);
    stats.last_total_size.store(total_size, std::memory_order_relaxed);
    stats.last_step_size.store(std::max(1, step_size), std::memory_order_relaxed);
    stats.last_active_threads.store(std::max(1, active_threads), std::memory_order_relaxed);
    stats.last_target_chunks.store(std::max(0, target_chunks), std::memory_order_relaxed);
    stats.last_policy.store(static_cast<int>(policy), std::memory_order_relaxed);
}

void AutoTuner::noteThreadTasks(InferencePhase phase, int thread_id, int task_count) {
    if (thread_id < 0 || thread_id >= MNN_MAX_SCHEDULER_THREADS || task_count <= 0) {
        return;
    }
    scheduleStats(phase).tasks_per_thread[thread_id].fetch_add(task_count, std::memory_order_relaxed);
}

void AutoTuner::notePrefillLocalPop(bool success) {
    auto& stats = scheduleStats(InferencePhase::PREFILL);
    stats.local_pop_calls.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        stats.local_pop_success.fetch_add(1, std::memory_order_relaxed);
    }
}

void AutoTuner::notePrefillStealAttempt() {
    scheduleStats(InferencePhase::PREFILL).steal_attempts.fetch_add(1, std::memory_order_relaxed);
}

void AutoTuner::notePrefillStealResult(bool success, int task_count, int cas_retries) {
    auto& stats = scheduleStats(InferencePhase::PREFILL);
    if (success) {
        stats.steal_success.fetch_add(1, std::memory_order_relaxed);
        stats.stolen_tasks.fetch_add(task_count, std::memory_order_relaxed);
        stats.total_chunks.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats.steal_empty.fetch_add(1, std::memory_order_relaxed);
    }
    if (cas_retries > 0) {
        stats.steal_cas_retries.fetch_add(cas_retries, std::memory_order_relaxed);
    }
}

void AutoTuner::noteDecodeDynamicClaim(bool success, int task_count) {
    auto& stats = scheduleStats(InferencePhase::DECODE);
    stats.dynamic_claim_calls.fetch_add(1, std::memory_order_relaxed);
    if (success) {
        stats.dynamic_claim_success.fetch_add(1, std::memory_order_relaxed);
        stats.dynamic_claim_tasks.fetch_add(task_count, std::memory_order_relaxed);
        stats.total_chunks.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats.dynamic_claim_empty.fetch_add(1, std::memory_order_relaxed);
    }
}

void AutoTuner::notePrefillThreadStats(int thread_id, const PrefillWorkStealThreadStats& localStats) {
    if (thread_id < 0 || thread_id >= MNN_MAX_SCHEDULER_THREADS) {
        return;
    }
    auto& stats = scheduleStats(InferencePhase::PREFILL);
    stats.local_pop_calls.fetch_add(localStats.local_pop_calls, std::memory_order_relaxed);
    stats.local_pop_success.fetch_add(localStats.local_pop_success, std::memory_order_relaxed);
    stats.steal_attempts.fetch_add(localStats.steal_attempts, std::memory_order_relaxed);
    stats.steal_success.fetch_add(localStats.steal_success, std::memory_order_relaxed);
    stats.steal_empty.fetch_add(localStats.steal_empty, std::memory_order_relaxed);
    stats.steal_cas_retries.fetch_add(localStats.steal_cas_retries, std::memory_order_relaxed);
    stats.stolen_tasks.fetch_add(localStats.stolen_tasks, std::memory_order_relaxed);
    stats.total_chunks.fetch_add(localStats.local_pop_success + localStats.steal_success, std::memory_order_relaxed);
    stats.tasks_per_thread[thread_id].fetch_add(localStats.tasks_executed, std::memory_order_relaxed);
}

void AutoTuner::noteDecodeDynamicThreadStats(int thread_id, const DecodeDynamicThreadStats& localStats) {
    if (thread_id < 0 || thread_id >= MNN_MAX_SCHEDULER_THREADS) {
        return;
    }
    auto& stats = scheduleStats(InferencePhase::DECODE);
    stats.dynamic_claim_calls.fetch_add(localStats.claim_calls, std::memory_order_relaxed);
    stats.dynamic_claim_success.fetch_add(localStats.claim_success, std::memory_order_relaxed);
    stats.dynamic_claim_empty.fetch_add(localStats.claim_empty, std::memory_order_relaxed);
    stats.dynamic_claim_tasks.fetch_add(localStats.claimed_tasks, std::memory_order_relaxed);
    stats.total_chunks.fetch_add(localStats.claim_success, std::memory_order_relaxed);
    stats.tasks_per_thread[thread_id].fetch_add(localStats.tasks_executed, std::memory_order_relaxed);
}

PhaseScheduleStatsSnapshot AutoTuner::getScheduleStats(InferencePhase phase) const {
    return scheduleStats(phase).snapshot();
}

std::string AutoTuner::formatScheduleStats(InferencePhase phase) const {
    const auto snapshot = getScheduleStats(phase);
    std::ostringstream stream;
    stream << "policy=" << schedulerPolicyName(snapshot.last_policy)
           << " ops=" << snapshot.op_count
           << " total=" << snapshot.total_tasks
           << " step=" << snapshot.last_step_size
           << " threads=" << snapshot.last_active_threads;
    if (phase == InferencePhase::PREFILL) {
        if (snapshot.last_policy == SchedulerPolicy::STATIC) {
            stream << " static_total_tasks=" << snapshot.total_tasks;
        } else {
            stream << " ws_total_tasks=" << snapshot.total_tasks
                   << " ws_local_pop_calls=" << snapshot.local_pop_calls
                   << " ws_local_pop_success=" << snapshot.local_pop_success
                   << " ws_steal_attempts=" << snapshot.steal_attempts
                   << " ws_steal_success=" << snapshot.steal_success
                   << " ws_steal_empty=" << snapshot.steal_empty
                   << " ws_steal_cas_retries=" << snapshot.steal_cas_retries
                   << " ws_stolen_tasks=" << snapshot.stolen_tasks
                   << " ws_total_chunks=" << snapshot.total_chunks;
        }
    } else {
        stream << " dyn_total_tasks=" << snapshot.total_tasks
               << " dyn_target_chunks=" << snapshot.last_target_chunks
               << " dyn_claim_calls=" << snapshot.dynamic_claim_calls
               << " dyn_claim_success=" << snapshot.dynamic_claim_success
               << " dyn_claim_empty=" << snapshot.dynamic_claim_empty
               << " dyn_claim_tasks=" << snapshot.dynamic_claim_tasks
               << " dyn_total_chunks=" << snapshot.total_chunks;
    }
    stream << " tasks_per_thread=";
    appendThreadLoads(stream, snapshot.tasks_per_thread);
    if (scheduleInstrumentEnabled()) {
        stream << " instr=1";
    }
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
    mPrefillParams = TuningParams(SchedulerPolicy::WORK_STEAL);
    mDecodeParams = TuningParams(SchedulerPolicy::DYNAMIC);
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
