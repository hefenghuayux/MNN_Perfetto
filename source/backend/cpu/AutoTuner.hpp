#include <MNN/MNNDefine.h>
#ifndef AutoTuner_hpp
#define AutoTuner_hpp

#include <array>
#include <atomic>
#include <mutex>
#include <string>

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
    HYBRID = 1,
    GUIDED = 2
};

MNN_PUBLIC const char* schedulerPolicyName(SchedulerPolicy policy);

struct TuningParams {
    float static_ratio;
    int dynamic_blocks;
    int dynamic_target_chunks;
    SchedulerPolicy policy;
    int min_chunk_size;

    TuningParams(float ratio = 0.0f,
                 int blocks = 0,
                 int target_chunks = 0,
                 SchedulerPolicy scheduler_policy = SchedulerPolicy::DYNAMIC,
                 int min_chunk = 1)
        : static_ratio(ratio),
          dynamic_blocks(blocks),
          dynamic_target_chunks(target_chunks > 0 ? target_chunks : blocks),
          policy(scheduler_policy),
          min_chunk_size(min_chunk > 0 ? min_chunk : 1) {
    }
};

struct ExecutionParams {
    int active_threads;
    unsigned long affinity_mask;

    ExecutionParams(int threads = 1, unsigned long mask = 0)
        : active_threads(threads),
          affinity_mask(mask) {
    }
};

struct alignas(MNN_CACHE_LINE_SIZE) DynamicTaskState {
    std::atomic<int> cursor{0};
    int end{0};
    int step_size{1};
    int min_step_size{1};
    int active_threads{1};
    int target_chunks{1};
    SchedulerPolicy policy{SchedulerPolicy::DYNAMIC};
};

struct PhaseScheduleStatsSnapshot {
    long long op_count = 0;
    long long total_tasks = 0;
    long long total_static_tasks = 0;
    long long total_dynamic_tasks = 0;
    long long total_step_size = 0;
    long long step_samples = 0;
    long long total_target_chunks = 0;
    long long target_chunk_samples = 0;
    long long theoretical_dynamic_chunks = 0;
    long long actual_dynamic_chunks = 0;
    int last_total_size = 0;
    int last_total_static = 0;
    int last_dynamic_size = 0;
    int last_step_size = 1;
    int last_target_chunks = 0;
    int last_active_threads = 1;
    int last_min_chunk_size = 1;
    SchedulerPolicy last_policy = SchedulerPolicy::DYNAMIC;
    std::array<long long, MNN_MAX_SCHEDULER_THREADS> static_tasks_per_thread{};
    std::array<long long, MNN_MAX_SCHEDULER_THREADS> dynamic_tasks_per_thread{};
};

struct PhaseScheduleStats {
    std::atomic<long long> op_count{0};
    std::atomic<long long> total_tasks{0};
    std::atomic<long long> total_static_tasks{0};
    std::atomic<long long> total_dynamic_tasks{0};
    std::atomic<long long> total_step_size{0};
    std::atomic<long long> step_samples{0};
    std::atomic<long long> total_target_chunks{0};
    std::atomic<long long> target_chunk_samples{0};
    std::atomic<long long> theoretical_dynamic_chunks{0};
    std::atomic<long long> actual_dynamic_chunks{0};
    std::atomic<int> last_total_size{0};
    std::atomic<int> last_total_static{0};
    std::atomic<int> last_dynamic_size{0};
    std::atomic<int> last_step_size{1};
    std::atomic<int> last_target_chunks{0};
    std::atomic<int> last_active_threads{1};
    std::atomic<int> last_min_chunk_size{1};
    std::atomic<int> last_policy{static_cast<int>(SchedulerPolicy::DYNAMIC)};
    std::array<std::atomic<long long>, MNN_MAX_SCHEDULER_THREADS> static_tasks_per_thread;
    std::array<std::atomic<long long>, MNN_MAX_SCHEDULER_THREADS> dynamic_tasks_per_thread;

    PhaseScheduleStats();
    void reset();
    PhaseScheduleStatsSnapshot snapshot() const;
};

class MNN_PUBLIC AutoTuner {
public:
    static AutoTuner* getInstance();
    static void destroy();

    void setPhase(InferencePhase phase);
    InferencePhase getPhase() const;

    unsigned long getFastAffinityMask() const {
        return mCurrentAffinityMask.load(std::memory_order_relaxed);
    }

    int getActiveThreadCount() const {
        const int threads = mCurrentActiveThreadCount.load(std::memory_order_relaxed);
        return threads > 0 ? threads : 1;
    }

    TuningParams getTuningParams() const;
    TuningParams getPrefillParams() const;
    TuningParams getDecodeParams() const;

    void setPrefillParams(float static_ratio, int dynamic_blocks);
    void setDecodeParams(float static_ratio, int dynamic_blocks);
    void setPrefillParams(const TuningParams& params);
    void setDecodeParams(const TuningParams& params);

    void setDefaultExecution(int active_threads, unsigned long affinity_mask = 0);
    void setPrefillExecution(int active_threads, unsigned long affinity_mask = 0);
    void setDecodeExecution(int active_threads, unsigned long affinity_mask = 0);

    void resetScheduleStats(InferencePhase phase);
    void noteSchedulePlan(InferencePhase phase,
                          SchedulerPolicy policy,
                          int active_threads,
                          int total_size,
                          int total_static,
                          int dynamic_size,
                          int step_size,
                          int target_chunks,
                          int theoretical_dynamic_chunks,
                          int min_chunk_size);
    void noteStaticRange(InferencePhase phase, int thread_id, int start, int end);
    void noteDynamicRange(InferencePhase phase, int thread_id, int start, int end);
    PhaseScheduleStatsSnapshot getScheduleStats(InferencePhase phase) const;
    std::string formatScheduleStats(InferencePhase phase) const;

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
    PhaseScheduleStats mPrefillScheduleStats;
    PhaseScheduleStats mDecodeScheduleStats;
};

} // namespace MNN

#endif /* AutoTuner_hpp */
