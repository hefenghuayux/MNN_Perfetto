//
//  AutoTuner.hpp
//  MNN
//
//  Created for MNN heterogeneous scheduling optimization.
//
#include <MNN/MNNDefine.h>
#ifndef AutoTuner_hpp
#define AutoTuner_hpp

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#define MNN_CACHE_LINE_SIZE 64
#define MNN_MAX_SCHEDULER_THREADS 32

namespace MNN {

enum class InferencePhase {
    PREFILL = 0,
    DECODE = 1,
    UNKNOWN = -1
};

enum class SchedulerPolicy {
    DYNAMIC = 0,
    WORK_STEAL = 1
};

MNN_PUBLIC const char* schedulerPolicyName(SchedulerPolicy policy);

struct TuningParams {
    SchedulerPolicy policy;
    int dynamic_blocks;
    int dynamic_target_chunks;

    explicit TuningParams(SchedulerPolicy scheduler_policy = SchedulerPolicy::DYNAMIC,
                          int blocks = 0,
                          int target_chunks = 0)
        : policy(scheduler_policy),
          dynamic_blocks(blocks),
          dynamic_target_chunks(target_chunks > 0 ? target_chunks : blocks) {}
};

struct alignas(MNN_CACHE_LINE_SIZE) PrefillWorkStealThreadStats {
    long long local_pop_calls = 0;
    long long local_pop_success = 0;
    long long steal_attempts = 0;
    long long steal_success = 0;
    long long steal_empty = 0;
    long long steal_cas_retries = 0;
    long long stolen_tasks = 0;
    long long tasks_executed = 0;
};

struct alignas(MNN_CACHE_LINE_SIZE) DecodeDynamicThreadStats {
    long long claim_calls = 0;
    long long claim_success = 0;
    long long claim_empty = 0;
    long long claimed_tasks = 0;
    long long tasks_executed = 0;
    long long padding[3] = {0, 0, 0};
};

struct ExecutionParams {
    int active_threads;
    unsigned long affinity_mask;

    ExecutionParams(int threads = 1, unsigned long mask = 0)
        : active_threads(threads), affinity_mask(mask) {}
};

struct alignas(MNN_CACHE_LINE_SIZE) DynamicTaskState {
    std::atomic<int> cursor{0};
    int end{0};
    int step_size{1};
    int active_threads{1};
    int target_chunks{0};
    SchedulerPolicy policy{SchedulerPolicy::DYNAMIC};
    std::array<DecodeDynamicThreadStats, MNN_MAX_SCHEDULER_THREADS> thread_stats{};
};

struct alignas(MNN_CACHE_LINE_SIZE) WorkStealQueueSlot {
    std::atomic<uint64_t> bounds{0};
    uint8_t padding[MNN_CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)]{};
};

struct alignas(MNN_CACHE_LINE_SIZE) PrefillWorkStealState {
    std::atomic<uint64_t> non_empty_mask{0};
    uint8_t mask_padding[MNN_CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)]{};
    int total_size{0};
    int step_size{1};
    int active_threads{1};
    std::array<uint32_t, MNN_MAX_SCHEDULER_THREADS> local_steps{};
    std::array<WorkStealQueueSlot, MNN_MAX_SCHEDULER_THREADS> queues{};
    std::array<PrefillWorkStealThreadStats, MNN_MAX_SCHEDULER_THREADS> thread_stats{};
    std::array<std::array<uint8_t, MNN_MAX_SCHEDULER_THREADS>, MNN_MAX_SCHEDULER_THREADS> victim_order{};
};

struct PhaseScheduleStatsSnapshot {
    long long op_count = 0;
    long long total_tasks = 0;
    long long total_chunks = 0;
    long long local_pop_calls = 0;
    long long local_pop_success = 0;
    long long steal_attempts = 0;
    long long steal_success = 0;
    long long steal_empty = 0;
    long long steal_cas_retries = 0;
    long long stolen_tasks = 0;
    long long dynamic_claim_calls = 0;
    long long dynamic_claim_success = 0;
    long long dynamic_claim_empty = 0;
    long long dynamic_claim_tasks = 0;
    int last_total_size = 0;
    int last_step_size = 1;
    int last_active_threads = 1;
    int last_target_chunks = 0;
    SchedulerPolicy last_policy = SchedulerPolicy::DYNAMIC;
    std::array<long long, MNN_MAX_SCHEDULER_THREADS> tasks_per_thread{};
};

struct PhaseScheduleStats {
    std::atomic<long long> op_count{0};
    std::atomic<long long> total_tasks{0};
    std::atomic<long long> total_chunks{0};
    std::atomic<long long> local_pop_calls{0};
    std::atomic<long long> local_pop_success{0};
    std::atomic<long long> steal_attempts{0};
    std::atomic<long long> steal_success{0};
    std::atomic<long long> steal_empty{0};
    std::atomic<long long> steal_cas_retries{0};
    std::atomic<long long> stolen_tasks{0};
    std::atomic<long long> dynamic_claim_calls{0};
    std::atomic<long long> dynamic_claim_success{0};
    std::atomic<long long> dynamic_claim_empty{0};
    std::atomic<long long> dynamic_claim_tasks{0};
    std::atomic<int> last_total_size{0};
    std::atomic<int> last_step_size{1};
    std::atomic<int> last_active_threads{1};
    std::atomic<int> last_target_chunks{0};
    std::atomic<int> last_policy{static_cast<int>(SchedulerPolicy::DYNAMIC)};
    std::array<std::atomic<long long>, MNN_MAX_SCHEDULER_THREADS> tasks_per_thread;

    PhaseScheduleStats();
    void reset();
    PhaseScheduleStatsSnapshot snapshot() const;
};

class MNN_PUBLIC AutoTuner {
public:
    static AutoTuner* getInstance();
    static void destroy();

    void setPhase(InferencePhase phase);

    unsigned long getFastAffinityMask() const {
        return mCurrentAffinityMask.load(std::memory_order_relaxed);
    }

    int getActiveThreadCount() const {
        int threads = mCurrentActiveThreadCount.load(std::memory_order_relaxed);
        return threads > 0 ? threads : 1;
    }

    InferencePhase getPhase() const;
    TuningParams getTuningParams() const;

    void setPrefillParams(const TuningParams& params);
    void setDecodeParams(const TuningParams& params);

    void setDefaultExecution(int active_threads, unsigned long affinity_mask = 0);
    void setPrefillExecution(int active_threads, unsigned long affinity_mask = 0);
    void setDecodeExecution(int active_threads, unsigned long affinity_mask = 0);

    void setCoreCapacities(const std::vector<int>& capacities);
    const std::vector<int>& getCoreCapacities() const;
    void setCoreRatios(const std::vector<int>& ratios);
    const std::vector<int>& getCoreRatios() const;

    void resetScheduleStats(InferencePhase phase);
    void noteSchedulePlan(InferencePhase phase,
                          SchedulerPolicy policy,
                          int active_threads,
                          int total_size,
                          int step_size,
                          int target_chunks = 0);
    void noteThreadTasks(InferencePhase phase, int thread_id, int task_count);
    void notePrefillLocalPop(bool success);
    void notePrefillStealAttempt();
    void notePrefillStealResult(bool success, int task_count, int cas_retries);
    void noteDecodeDynamicClaim(bool success, int task_count);
    void notePrefillThreadStats(int thread_id, const PrefillWorkStealThreadStats& stats);
    void noteDecodeDynamicThreadStats(int thread_id, const DecodeDynamicThreadStats& stats);
    PhaseScheduleStatsSnapshot getScheduleStats(InferencePhase phase) const;
    std::string formatScheduleStats(InferencePhase phase) const;

    void feedback(float cost_time);
    void setPanicMode(bool enable);

    TuningParams getDecodeParams() const;
    TuningParams getPrefillParams() const;
    bool isPanicMode() const;
    void reset();

private:
    AutoTuner();
    ~AutoTuner() = default;

    AutoTuner(const AutoTuner&) = delete;
    AutoTuner& operator=(const AutoTuner&) = delete;
    AutoTuner(AutoTuner&&) = delete;
    AutoTuner& operator=(AutoTuner&&) = delete;

    void updateFastPhaseState(InferencePhase phase);
    void refreshFallbackExecutionState();
    PhaseScheduleStats& scheduleStats(InferencePhase phase);
    const PhaseScheduleStats& scheduleStats(InferencePhase phase) const;

private:
    static AutoTuner* sInstance;
    static std::mutex sInstanceMutex;

    std::atomic<unsigned long> mCurrentAffinityMask{0};
    std::atomic<int> mCurrentActiveThreadCount{1};
    std::atomic<unsigned long> mFallbackAffinityMask{0};
    std::atomic<int> mFallbackActiveThreadCount{1};

    TuningParams mPrefillParams;
    TuningParams mDecodeParams;
    ExecutionParams mDefaultExecution;
    ExecutionParams mPrefillExecution;
    ExecutionParams mDecodeExecution;

    std::atomic<InferencePhase> mCurrentPhase;
    std::vector<int> mCoreCapacities;
    PhaseScheduleStats mPrefillScheduleStats;
    PhaseScheduleStats mDecodeScheduleStats;
    std::atomic<bool> mPanicMode{false};
};

inline int alignToCacheLine(int boundary, int element_size = 4) {
    int elements_per_line = MNN_CACHE_LINE_SIZE / element_size;
    return (boundary / elements_per_line) * elements_per_line;
}

inline int alignToCacheLineUp(int boundary, int element_size = 4) {
    int elements_per_line = MNN_CACHE_LINE_SIZE / element_size;
    return ((boundary + elements_per_line - 1) / elements_per_line) * elements_per_line;
}

} // namespace MNN

#endif /* AutoTuner_hpp */
