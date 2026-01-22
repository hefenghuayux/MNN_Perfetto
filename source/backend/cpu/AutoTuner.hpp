//
//  AutoTuner.hpp
//  MNN
//
//  Created for MNN Heterogeneous Scheduling Optimization
//  Copyright © 2024, Alibaba Group Holding Limited
//

#ifndef AutoTuner_hpp
#define AutoTuner_hpp

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

// Cache Line 大小（字节），用于避免 False Sharing
#define MNN_CACHE_LINE_SIZE 64

namespace MNN {

/**
 * @brief 任务划分的调优参数
 */
struct TuningParams {
    float static_ratio;    // 静态部分占比 [0.0, 1.0]
    int step_size;         // 动态调度时每次抢占的任务块数（以迭代为单位）
    
    TuningParams(float ratio = 0.8f, int step = 4)
        : static_ratio(ratio), step_size(step) {}
};

/**
 * @brief 用于动态调度的原子计数器结构，强制 Cache Line 对齐避免 False Sharing
 */
struct alignas(MNN_CACHE_LINE_SIZE) DynamicTaskState {
    std::atomic<int> cursor{0};   // 动态任务的当前游标（原子抢占点）
    int end{0};                   // 动态任务结束边界
    int step_size{1};             // 每次抢占的任务块大小
    
    // 填充字节确保整个结构体占据完整的 Cache Line
    char padding[MNN_CACHE_LINE_SIZE - sizeof(std::atomic<int>) - sizeof(int) * 2];
};

/**
 * @brief AutoTuner 单例类 - 全局任务划分策略指挥官
 * 
 * Phase 1: 静态比例 + 动态缓冲的固定逻辑
 * Phase 2 (预留): 运行时自动调优（Hill Climbing）和急停开关（Panic Switch）
 */
class AutoTuner {
public:
    /**
     * @brief 获取单例实例
     */
    static AutoTuner* getInstance();
    
    /**
     * @brief 销毁单例（程序退出时调用）
     */
    static void destroy();

    /**
     * @brief 获取当前阶段的任务划分参数
     * @param is_prefill true=Prefill阶段, false=Decode阶段
     * @return TuningParams 包含 static_ratio 和 step_size
     */
    TuningParams getTuningParams(bool is_prefill) const;
    
    /**
     * @brief 设置 Prefill 阶段的调优参数
     */
    void setPrefillParams(float static_ratio, int step_size);
    
    /**
     * @brief 设置 Decode 阶段的调优参数
     */
    void setDecodeParams(float static_ratio, int step_size);
    
    /**
     * @brief 设置核心性能比（大核:中核:小核...）
     * @param ratios 性能比数组，从大核到小核排列。例如 {4, 2, 1} 表示大核是小核4倍速度
     */
    void setCoreRatios(const std::vector<int>& ratios);
    
    /**
     * @brief 获取核心性能比
     */
    const std::vector<int>& getCoreRatios() const;

    // ===================== Phase 2 预留接口 =====================
    
    /**
     * @brief [Phase 2 预留] 反馈接口 - 接收上一次推理的耗时
     * @param cost_time 上一次推理的耗时（毫秒）
     * @param is_prefill 是否为 Prefill 阶段
     * 
     * 用于未来实现梯度微调（Hill Climbing）:
     * - 记录历史耗时
     * - 计算性能梯度
     * - 自动调整 static_ratio
     */
    void feedback(float cost_time, bool is_prefill);
    
    /**
     * @brief [Phase 2 预留] 急停开关 - 紧急切换调度策略
     * @param enable true=启用急停模式
     * 
     * 急停模式下的行为:
     * - 立即切换到保守的均匀分配策略
     * - 禁用动态调度
     * - 用于检测到严重性能异常时的快速恢复
     */
    void setPanicMode(bool enable);
    
    /**
     * @brief [Phase 2 预留] 检查是否处于急停模式
     */
    bool isPanicMode() const;
    
    /**
     * @brief [Phase 2 预留] 重置调优状态
     * 
     * 清除所有历史反馈数据，恢复到初始参数
     */
    void reset();

private:
    AutoTuner();
    ~AutoTuner() = default;
    
    // 禁止拷贝和移动
    AutoTuner(const AutoTuner&) = delete;
    AutoTuner& operator=(const AutoTuner&) = delete;
    AutoTuner(AutoTuner&&) = delete;
    AutoTuner& operator=(AutoTuner&&) = delete;
    
    static AutoTuner* sInstance;
    static std::mutex sInstanceMutex;
    
    // Prefill 阶段参数（默认：静态80%，动态步长4）
    TuningParams mPrefillParams;
    
    // Decode 阶段参数（默认：全动态调度）
    TuningParams mDecodeParams;
    
    // 核心性能比（从大核到小核）
    std::vector<int> mCoreRatios;
    
    // Phase 2 预留: 急停模式标志
    std::atomic<bool> mPanicMode{false};
    
    // Phase 2 预留: 用于 Hill Climbing 的历史数据
    // std::deque<float> mPrefillHistory;
    // std::deque<float> mDecodeHistory;
    // float mLearningRate = 0.05f;
};

// ===================== 工具函数 =====================

/**
 * @brief 将任务边界对齐到 Cache Line（向下取整）
 * @param boundary 原始边界
 * @param element_size 单个元素的大小（字节）
 * @return 对齐后的边界
 * 
 * 用于防止静态/动态任务边界处的 False Sharing
 * 例如：对于 float (4字节)，Cache Line = 64字节 = 16个float
 */
inline int alignToCacheLine(int boundary, int element_size = 4) {
    int elements_per_line = MNN_CACHE_LINE_SIZE / element_size;
    return (boundary / elements_per_line) * elements_per_line;
}

/**
 * @brief 将任务边界对齐到 Cache Line（向上取整）
 */
inline int alignToCacheLineUp(int boundary, int element_size = 4) {
    int elements_per_line = MNN_CACHE_LINE_SIZE / element_size;
    return ((boundary + elements_per_line - 1) / elements_per_line) * elements_per_line;
}

} // namespace MNN

#endif /* AutoTuner_hpp */
