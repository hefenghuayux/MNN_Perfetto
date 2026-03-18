//
//  AutoTuner.hpp
//  MNN
//
//  Created for MNN heterogeneous scheduling optimization.
//
#include <MNN/MNNDefine.h>
#ifndef AutoTuner_hpp
#define AutoTuner_hpp

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#define MNN_CACHE_LINE_SIZE 64

namespace MNN {

enum class InferencePhase {
    PREFILL = 0,
    DECODE = 1,
    UNKNOWN = -1
};

struct TuningParams {
    float static_ratio;
    int dynamic_blocks;

    TuningParams(float ratio = 0.8f, int blocks = 8)
        : static_ratio(ratio), dynamic_blocks(blocks) {}
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
    char padding[MNN_CACHE_LINE_SIZE - sizeof(std::atomic<int>) - sizeof(int) * 2];
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

    void setPrefillParams(float static_ratio, int dynamic_blocks);
    void setDecodeParams(float static_ratio, int dynamic_blocks);

    void setDefaultExecution(int active_threads, unsigned long affinity_mask = 0);
    void setPrefillExecution(int active_threads, unsigned long affinity_mask = 0);
    void setDecodeExecution(int active_threads, unsigned long affinity_mask = 0);

    void setCoreRatios(const std::vector<int>& ratios);
    const std::vector<int>& getCoreRatios() const;

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
    std::vector<int> mCoreRatios;
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
