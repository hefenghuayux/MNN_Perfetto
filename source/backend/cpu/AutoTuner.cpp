//
//  AutoTuner.cpp
//  MNN
//
//  Created for MNN heterogeneous scheduling optimization.
//

#include "AutoTuner.hpp"

#include <algorithm>

#include <MNN/MNNDefine.h>

namespace MNN {

AutoTuner* AutoTuner::sInstance = nullptr;
std::mutex AutoTuner::sInstanceMutex;

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
    : mPrefillParams(0.0f, 60)
    , mDecodeParams(0.9f, 2)
    , mDefaultExecution(1, 0)
    , mPrefillExecution(1, 0)
    , mDecodeExecution(1, 0)
    , mCurrentPhase(InferencePhase::UNKNOWN)
    , mCoreRatios({4, 2, 1})
    , mPanicMode(false) {
    refreshFallbackExecutionState();
    updateFastPhaseState(InferencePhase::UNKNOWN);
    MNN_PRINT("[AutoTuner] Initialized. Default(threads=%d, affinity=0x%lX) Prefill(static=%.2f, blocks=%d, threads=%d, affinity=0x%lX) Decode(static=%.2f, blocks=%d, threads=%d, affinity=0x%lX)\n",
              mDefaultExecution.active_threads,
              mDefaultExecution.affinity_mask,
              mPrefillParams.static_ratio,
              mPrefillParams.dynamic_blocks,
              mPrefillExecution.active_threads,
              mPrefillExecution.affinity_mask,
              mDecodeParams.static_ratio,
              mDecodeParams.dynamic_blocks,
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
        return TuningParams(1.0f, 0);
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
    if (static_ratio < 0.0f) {
        static_ratio = 0.0f;
    }
    if (static_ratio > 1.0f) {
        static_ratio = 1.0f;
    }
    if (dynamic_blocks < 0) {
        dynamic_blocks = 0;
    }

    mPrefillParams.static_ratio = static_ratio;
    mPrefillParams.dynamic_blocks = dynamic_blocks;
    MNN_PRINT("[AutoTuner] Prefill tuning updated: static_ratio=%.2f, dynamic_blocks=%d\n",
              static_ratio,
              dynamic_blocks);
}

void AutoTuner::setDecodeParams(float static_ratio, int dynamic_blocks) {
    if (static_ratio < 0.0f) {
        static_ratio = 0.0f;
    }
    if (static_ratio > 1.0f) {
        static_ratio = 1.0f;
    }
    if (dynamic_blocks < 0) {
        dynamic_blocks = 0;
    }

    mDecodeParams.static_ratio = static_ratio;
    mDecodeParams.dynamic_blocks = dynamic_blocks;
    MNN_PRINT("[AutoTuner] Decode tuning updated: static_ratio=%.2f, dynamic_blocks=%d\n",
              static_ratio,
              dynamic_blocks);
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

void AutoTuner::setCoreRatios(const std::vector<int>& ratios) {
    mCoreRatios = ratios;
    MNN_PRINT("[AutoTuner] Core ratios set to: [");
    for (size_t i = 0; i < ratios.size(); ++i) {
        MNN_PRINT("%d%s", ratios[i], (i + 1 < ratios.size()) ? ":" : "");
    }
    MNN_PRINT("]\n");
}

const std::vector<int>& AutoTuner::getCoreRatios() const {
    return mCoreRatios;
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
    mPrefillParams = TuningParams(0.8f, 8);
    mDecodeParams = TuningParams(0.0f, 0);
    mDefaultExecution = ExecutionParams(1, 0);
    mPrefillExecution = ExecutionParams(1, 0);
    mDecodeExecution = ExecutionParams(1, 0);
    mCurrentPhase.store(InferencePhase::UNKNOWN, std::memory_order_release);
    mPanicMode.store(false, std::memory_order_release);
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

} // namespace MNN
