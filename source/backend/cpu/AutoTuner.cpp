//
//  AutoTuner.cpp
//  MNN
//
//  Created for MNN Heterogeneous Scheduling Optimization
//  Copyright © 2024, Alibaba Group Holding Limited
//

#include "AutoTuner.hpp"
#include <MNN/MNNDefine.h>

namespace MNN {

// 静态成员初始化
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
    : mPrefillParams(0.8f, 0xFFFFFFFF, 8)    // Prefill: 80% 静态，动态部分分成8块（每线程可抢约2块）
    , mDecodeParams(0.0f, 0xFFFFFFFF, 0)     // Decode: 全动态，最细粒度（step=1，逐任务抢占）
    , mCurrentPhase(InferencePhase::UNKNOWN)  // 默认未知阶段
    , mCoreRatios({4, 2, 1})     // 默认大:中:小 = 4:2:1
    , mPanicMode(false) {
    MNN_PRINT("[AutoTuner] Initialized with default params:\n");
    MNN_PRINT("  Prefill: static_ratio=%.2f, dynamic_blocks=%d, affinity=0x%lX\n", 
              mPrefillParams.static_ratio, mPrefillParams.dynamic_blocks, mPrefillParams.affinity_mask);
    MNN_PRINT("  Decode:  static_ratio=%.2f, dynamic_blocks=%d, affinity=0x%lX\n", 
              mDecodeParams.static_ratio, mDecodeParams.dynamic_blocks, mDecodeParams.affinity_mask);
}

void AutoTuner::setPhase(InferencePhase phase) {
    InferencePhase oldPhase = mCurrentPhase.exchange(phase, std::memory_order_release);
    if (oldPhase != phase) {
        const char* oldName = (oldPhase == InferencePhase::PREFILL) ? "PREFILL" : 
                              (oldPhase == InferencePhase::DECODE) ? "DECODE" : "UNKNOWN";
        const char* newName = (phase == InferencePhase::PREFILL) ? "PREFILL" : 
                              (phase == InferencePhase::DECODE) ? "DECODE" : "UNKNOWN";
        MNN_PRINT("[AutoTuner] Phase changed: %s -> %s\n", oldName, newName);
    }
}

InferencePhase AutoTuner::getPhase() const {
    return mCurrentPhase.load(std::memory_order_acquire);
}

TuningParams AutoTuner::getTuningParams() const {
    // Phase 2 预留: 急停模式下返回保守参数
    if (mPanicMode.load(std::memory_order_relaxed)) {
        // 急停模式：全静态均匀分配，禁用动态调度
        return TuningParams(1.0f, 0);
    }
    
    InferencePhase phase = mCurrentPhase.load(std::memory_order_acquire);
    
    // 根据当前阶段返回对应参数
    switch (phase) {
        case InferencePhase::PREFILL:
            return mPrefillParams;
        case InferencePhase::DECODE:
            return mDecodeParams;
        case InferencePhase::UNKNOWN:
        default:
            // 未知阶段默认使用 Prefill 参数（保守策略）
            return mPrefillParams;
    }
}

void AutoTuner::setPrefillParams(float static_ratio, int dynamic_blocks, unsigned long affinity_mask) {
    // 参数合法性检查
    if (static_ratio < 0.0f) static_ratio = 0.0f;
    if (static_ratio > 1.0f) static_ratio = 1.0f;
    if (dynamic_blocks < 0) dynamic_blocks = 0;
    
    mPrefillParams.static_ratio = static_ratio;
    mPrefillParams.dynamic_blocks = dynamic_blocks;
    mPrefillParams.affinity_mask = affinity_mask;
    
    MNN_PRINT("[AutoTuner] Prefill params updated: static_ratio=%.2f, dynamic_blocks=%d, affinity=0x%lX\n",
              static_ratio, dynamic_blocks, affinity_mask);
}

void AutoTuner::setDecodeParams(float static_ratio, int dynamic_blocks, unsigned long affinity_mask) {
    if (static_ratio < 0.0f) static_ratio = 0.0f;
    if (static_ratio > 1.0f) static_ratio = 1.0f;
    if (dynamic_blocks < 0) dynamic_blocks = 0;
    
    mDecodeParams.static_ratio = static_ratio;
    mDecodeParams.dynamic_blocks = dynamic_blocks;
    mDecodeParams.affinity_mask = affinity_mask;
    
    MNN_PRINT("[AutoTuner] Decode params updated: static_ratio=%.2f, dynamic_blocks=%d, affinity=0x%lX\n",
              static_ratio, dynamic_blocks, affinity_mask);
}

void AutoTuner::setCoreRatios(const std::vector<int>& ratios) {
    mCoreRatios = ratios;
    
    // 打印核心比例信息
    MNN_PRINT("[AutoTuner] Core ratios set to: [");
    for (size_t i = 0; i < ratios.size(); ++i) {
        MNN_PRINT("%d%s", ratios[i], (i < ratios.size() - 1) ? ":" : "");
    }
    MNN_PRINT("]\n");
}

const std::vector<int>& AutoTuner::getCoreRatios() const {
    return mCoreRatios;
}

// ===================== Phase 2 预留接口实现 =====================

void AutoTuner::feedback(float cost_time) {
    // Phase 1: 空实现
    // Phase 2 TODO: 
    // 1. 将 cost_time 添加到历史队列
    // 2. 计算性能趋势（梯度）
    // 3. 使用 Hill Climbing 微调 static_ratio
    //
    // 示例伪代码:
    // InferencePhase phase = mCurrentPhase.load(std::memory_order_acquire);
    // auto& history = (phase == InferencePhase::PREFILL) ? mPrefillHistory : mDecodeHistory;
    // history.push_back(cost_time);
    // if (history.size() >= WINDOW_SIZE) {
    //     float gradient = computeGradient(history);
    //     adjustStaticRatio(phase, gradient);
    //     history.pop_front();
    // }
    
    (void)cost_time;
}

void AutoTuner::setPanicMode(bool enable) {
    bool expected = !enable;
    if (mPanicMode.compare_exchange_strong(expected, enable, std::memory_order_release)) {
        if (enable) {
            MNN_PRINT("[AutoTuner] PANIC MODE ENABLED - Switching to conservative scheduling\n");
        } else {
            MNN_PRINT("[AutoTuner] Panic mode disabled - Resuming normal scheduling\n");
        }
    }
}

bool AutoTuner::isPanicMode() const {
    return mPanicMode.load(std::memory_order_relaxed);
}

void AutoTuner::reset() {
    // 恢复默认参数
    mPrefillParams = TuningParams(0.8f, 0xFFFFFFFF, 8);
    mDecodeParams = TuningParams(0.0f, 0xFFFFFFFF, 0);
    mPanicMode.store(false, std::memory_order_release);
    
    // Phase 2 TODO: 清除历史数据
    // mPrefillHistory.clear();
    // mDecodeHistory.clear();
    
    MNN_PRINT("[AutoTuner] Reset to default parameters\n");
}

} // namespace MNN
