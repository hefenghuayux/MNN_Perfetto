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
    : mPrefillParams(0.8f, 4)    // Prefill: 80% 静态，步长4
    , mDecodeParams(0.0f, 1)     // Decode: 全动态，步长1
    , mCoreRatios({4, 2, 1})     // 默认大:中:小 = 4:2:1
    , mPanicMode(false) {
    MNN_PRINT("[AutoTuner] Initialized with default params:\n");
    MNN_PRINT("  Prefill: static_ratio=%.2f, step_size=%d\n", 
              mPrefillParams.static_ratio, mPrefillParams.step_size);
    MNN_PRINT("  Decode:  static_ratio=%.2f, step_size=%d\n", 
              mDecodeParams.static_ratio, mDecodeParams.step_size);
}

TuningParams AutoTuner::getTuningParams(bool is_prefill) const {
    // Phase 2 预留: 急停模式下返回保守参数
    if (mPanicMode.load(std::memory_order_relaxed)) {
        // 急停模式：全静态均匀分配，禁用动态调度
        return TuningParams(1.0f, 0);
    }
    
    return is_prefill ? mPrefillParams : mDecodeParams;
}

void AutoTuner::setPrefillParams(float static_ratio, int step_size) {
    // 参数合法性检查
    if (static_ratio < 0.0f) static_ratio = 0.0f;
    if (static_ratio > 1.0f) static_ratio = 1.0f;
    if (step_size < 1) step_size = 1;
    
    mPrefillParams.static_ratio = static_ratio;
    mPrefillParams.step_size = step_size;
    
    MNN_PRINT("[AutoTuner] Prefill params updated: static_ratio=%.2f, step_size=%d\n",
              static_ratio, step_size);
}

void AutoTuner::setDecodeParams(float static_ratio, int step_size) {
    if (static_ratio < 0.0f) static_ratio = 0.0f;
    if (static_ratio > 1.0f) static_ratio = 1.0f;
    if (step_size < 1) step_size = 1;
    
    mDecodeParams.static_ratio = static_ratio;
    mDecodeParams.step_size = step_size;
    
    MNN_PRINT("[AutoTuner] Decode params updated: static_ratio=%.2f, step_size=%d\n",
              static_ratio, step_size);
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

void AutoTuner::feedback(float cost_time, bool is_prefill) {
    // Phase 1: 空实现
    // Phase 2 TODO: 
    // 1. 将 cost_time 添加到历史队列
    // 2. 计算性能趋势（梯度）
    // 3. 使用 Hill Climbing 微调 static_ratio
    //
    // 示例伪代码:
    // auto& history = is_prefill ? mPrefillHistory : mDecodeHistory;
    // history.push_back(cost_time);
    // if (history.size() >= WINDOW_SIZE) {
    //     float gradient = computeGradient(history);
    //     adjustStaticRatio(is_prefill, gradient);
    //     history.pop_front();
    // }
    
    (void)cost_time;
    (void)is_prefill;
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
    mPrefillParams = TuningParams(0.8f, 4);
    mDecodeParams = TuningParams(0.0f, 1);
    mPanicMode.store(false, std::memory_order_release);
    
    // Phase 2 TODO: 清除历史数据
    // mPrefillHistory.clear();
    // mDecodeHistory.clear();
    
    MNN_PRINT("[AutoTuner] Reset to default parameters\n");
}

} // namespace MNN
