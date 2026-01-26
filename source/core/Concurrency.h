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

#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __is_prefill__) \
    {                                                                                \
        auto cpuBn = (CPUBackend*)backend();                                         \
        std::pair<std::function<void(int)>, int> task;                               \
        task.second = __num__;                                                       \
        task.first  = [&, cpuBn](int __iter__) {

#define MNN_CONCURRENCY_HYBRID_END()                                   \
    }                                                                  \
    ;                                                                  \
    auto thrPl = cpuBn->threadPool();                                  \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());               \
    }

// 辅助宏: 执行静态+动态两阶段任务
// __task_func__: 任务函数，接收单个任务索引
// __divides__: 任务边界数组，布局为 [0, end1, end2, ...], 即 thread i 处理 [divides[i], divides[i+1])
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)       \
    {                                                                          \
        begin_trace_marker("Worker_StaticPhase");                              \
        int __static_start__ = (__divides__)[__iter__];                        \
        int __static_end__ = (__divides__)[(__iter__) + 1];                    \
        for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) {  \
            __task_func__(__x__);                                              \
        }                                                                      \
        end_trace_marker();                                                    \
    }

#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__)                  \
    {                                                                          \
        begin_trace_marker("Worker_DynamicPhase");                             \
        while ((__cpuBn__)->hasDynamicTasks()) {                               \
            auto __chunk__ = (__cpuBn__)->fetchDynamicChunk();                 \
            if (__chunk__.first >= __chunk__.second) break;                    \
            for (int __x__ = __chunk__.first; __x__ < __chunk__.second; ++__x__) { \
                __task_func__(__x__);                                          \
            }                                                                  \
        }                                                                      \
        end_trace_marker();                                                    \
    }

// ===================== 混合调度宏结束 =====================

#else
// iOS / OSX / Windows / Other: 非线程池模式的后备混合调度宏
// 在这些平台上，混合调度退化为普通的静态分配

#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __is_prefill__) \
    for (int __iter__ = 0; __iter__ < __num__; __iter__++) {

#define MNN_CONCURRENCY_HYBRID_END() }

// 后备实现：仅执行静态部分，无动态抢占
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
