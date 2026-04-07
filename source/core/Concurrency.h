//
//  Concurrency.h
//  MNN
//
//  Created by MNN on 2018/07/26.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#ifndef concurrency_h
#define concurrency_h

#define LAUNCH_MULTI_THREADS_WORKLOAD 1e+5

// 用于混合调度的 trace marker 支持
#include "trace_marker_helper.h"

#ifdef MNN_FORBIT_MULTI_THREADS
#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_END() }

#elif defined(MNN_USE_THREAD_POOL)
#include "backend/cpu/ThreadPool.hpp"

#define MNN_STRINGIFY(a) #a
#define MNN_CONCURRENCY_BEGIN(__iter__, __num__)       \
    {                                                  \
        std::pair<std::function<void(int)>, int> task; \
        task.second = __num__;                         \
        task.first  = [&](int __iter__) {
#define MNN_CONCURRENCY_END()                                      \
    }                                                              \
    ;                                                              \
    auto cpuBn = (CPUBackend*)backend();                           \
    auto thrPl = cpuBn->threadPool();                              \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());           \
    }

// ===================== Phase 1: 混合调度宏 =====================
// 使用方法:
// MNN_CONCURRENCY_HYBRID_BEGIN(tId, threadNum, divides, isPrefill) {
//     // Phase 1: 执行静态私有区间
//     int staticStart = (tId == 0) ? 0 : divides[tId - 1];
//     int staticEnd = divides[tId];
//     for (int x = staticStart; x < staticEnd; ++x) {
//         processTask(x);
//     }
//     
//     // Phase 2: 抢占动态任务
//     while (cpuBn->hasDynamicTasks()) {
//         auto chunk = cpuBn->fetchDynamicChunk();
//         if (chunk.first >= chunk.second) break;
//         for (int x = chunk.first; x < chunk.second; ++x) {
//             processTask(x);
//         }
//     }
// }
// MNN_CONCURRENCY_HYBRID_END();

// __total_size__: 总任务数，用于在执行时初始化动态状态
// __step_size__: 动态任务的步长
// __policy__/__target_chunks__/__min_chunk_size__: 规划阶段产出的动态调度配置
#define MNN_CONCURRENCY_HYBRID_BEGIN(iter_, num_, divides_, total_size_, step_size_, policy_, target_chunks_, min_chunk_size_) \
    {                                                                                                                         \
        auto cpuBn = (CPUBackend*)backend();                                                                                  \
        /* 在执行时重新初始化动态状态，避免被其他算子覆盖；这里要带上完整计划，避免回退到 tuner 原始参数。 */                           \
        int __static_end__ = (divides_)[num_];                                                                                \
        cpuBn->initDynamicTaskState(__static_end__, total_size_, step_size_, policy_, num_, target_chunks_, min_chunk_size_); \
        std::pair<std::function<void(int)>, int> task;                                                                        \
        task.second = num_;                                                                                                   \
        task.first  = [&, cpuBn](int iter_) {

#define MNN_CONCURRENCY_HYBRID_END()                                   \
    }                                                                  \
    ;                                                                  \
    auto thrPl = cpuBn->threadPool();                                  \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());               \
    }

// 辅助宏: 执行静态+动态两阶段任务
// __task_func__: 任务函数，接收单个任务索引 (已废弃，性能差)
// __divides__: 任务边界数组，布局为 [0, end1, end2, ...], 即 thread i 处理 [divides[i], divides[i+1])
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)       \
    {                                                                          \
        int __static_start__ = (__divides__)[__iter__];                        \
        int __static_end__ = (__divides__)[(__iter__) + 1];                    \
        if (__static_start__ < __static_end__) {                               \
            begin_trace_marker("Worker_StaticPhase");                          \
            for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) { \
                __task_func__(__x__);                                          \
            }                                                                  \
            end_trace_marker();                                                \
        }                                                                      \
    }

#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__)                  \
    {                                                                          \
        if ((__cpuBn__)->hasDynamicTasks()) {                                  \
            begin_trace_marker("Worker_DynamicPhase");                         \
            do {                                                               \
                auto __chunk__ = (__cpuBn__)->fetchDynamicChunk();             \
                if (__chunk__.first >= __chunk__.second) break;                \
                for (int __x__ = __chunk__.first; __x__ < __chunk__.second; ++__x__) { \
                    __task_func__(__x__);                                      \
                }                                                              \
            } while ((__cpuBn__)->hasDynamicTasks());                          \
            end_trace_marker();                                                \
        }                                                                      \
    }

// ===================== 批量处理版本 (推荐使用) =====================
// 这些宏传递区间 [start, end)，让 Kernel 保持批量处理优化
// __range_func__: 区间处理函数，签名为 void(int start, int end)
//
// 使用示例:
// auto processRange = [&](int start, int end) {
//     int count = end - start;  // 批量处理 count 个任务
//     mGemmKernel(..., count, ...);
// };
// MNN_HYBRID_STATIC_RANGE(tId, mDivides.data(), processRange);
// MNN_HYBRID_DYNAMIC_RANGE(cpuBn, processRange);

#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__)        \
    {                                                                          \
        int __static_start__ = (__divides__)[__iter__];                        \
        int __static_end__ = (__divides__)[(__iter__) + 1];                    \
        if (__static_start__ < __static_end__) {                               \
            begin_trace_marker("Worker_StaticPhase");                          \
            __range_func__(__static_start__, __static_end__);                  \
            end_trace_marker();                                                \
        }                                                                      \
    }

#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__)                   \
    {                                                                          \
        if ((__cpuBn__)->hasDynamicTasks()) {                                  \
            begin_trace_marker("Worker_DynamicPhase");                         \
            do {                                                               \
                auto __chunk__ = (__cpuBn__)->fetchDynamicChunk();             \
                if (__chunk__.first >= __chunk__.second) break;                \
                __range_func__(__chunk__.first, __chunk__.second);             \
            } while ((__cpuBn__)->hasDynamicTasks());                          \
            end_trace_marker();                                                \
        }                                                                      \
    }

// ===================== 混合调度宏结束 =====================

#else
// iOS / OSX / Windows / Other: 非线程池模式的后备混合调度宏
// 在这些平台上，混合调度退化为普通的静态分配

#define MNN_CONCURRENCY_HYBRID_BEGIN(iter_, num_, divides_, total_size_, step_size_, policy_, target_chunks_, min_chunk_size_) \
    for (int iter_ = 0; iter_ < num_; iter_++) {

#define MNN_CONCURRENCY_HYBRID_END() }

// 后备实现：仅执行静态部分，无动态抢占 (单任务版本)
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)       \
    {                                                                          \
        int __static_start__ = (__divides__)[__iter__];                        \
        int __static_end__ = (__divides__)[(__iter__) + 1];                    \
        for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) {  \
            __task_func__(__x__);                                              \
        }                                                                      \
    }

#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__) \
    { /* 非线程池模式下不支持动态抢占 */ }

// 后备实现：批量处理版本 (推荐)
#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__)        \
    {                                                                          \
        int __static_start__ = (__divides__)[__iter__];                        \
        int __static_end__ = (__divides__)[(__iter__) + 1];                    \
        if (__static_start__ < __static_end__) {                               \
            __range_func__(__static_start__, __static_end__);                  \
        }                                                                      \
    }

#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__) \
    { /* 非线程池模式下不支持动态抢占 */ }

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <stddef.h>

#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) \
dispatch_apply(__num__, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^(size_t __iter__) {
#define MNN_CONCURRENCY_END() \
    (void)(backend()); \
    });

// Windows
#elif defined(_MSC_VER)
#include <omp.h>

#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) \
    __pragma(omp parallel for) for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_END() }
#define MNN_CONCURRENCY_BEGIN_CONDITION(__iter__, __num__, __condition__) \
    int __iter__ = 0;                                                     \
    __pragma(omp parallel for if(__condition__))                          \
    for (; __iter__ < __num__; __iter__++) {
// Android
#else
#include <omp.h>

#define MNN_STRINGIFY(a) #a
#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) \
    _Pragma("omp parallel for") for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_END() }

#endif
#endif
#endif /* concurrency_h */
