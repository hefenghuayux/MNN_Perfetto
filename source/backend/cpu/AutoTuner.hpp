#include <MNN/MNNDefine.h>
#ifndef AutoTuner_hpp
#define AutoTuner_hpp

#include <atomic>
#include <mutex>

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

struct alignas(64) DynamicTaskState {
    std::atomic<int> cursor{0};
    int end{0};
    int step_size{1};
    int min_step_size{1};
    int active_threads{1};
    int target_chunks{1};
    SchedulerPolicy policy{SchedulerPolicy::DYNAMIC};
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
    TuningParams getPrefillParams() const {
        return mPrefillParams;
    }
    TuningParams getDecodeParams() const {
        return mDecodeParams;
    }

    void setPrefillParams(float static_ratio, int dynamic_blocks);
    void setDecodeParams(float static_ratio, int dynamic_blocks);
    void setPrefillParams(const TuningParams& params);
    void setDecodeParams(const TuningParams& params);

    void setDefaultExecution(int active_threads, unsigned long affinity_mask = 0);
    void setPrefillExecution(int active_threads, unsigned long affinity_mask = 0);
    void setDecodeExecution(int active_threads, unsigned long affinity_mask = 0);

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
};

} // namespace MNN

#endif /* AutoTuner_hpp */
