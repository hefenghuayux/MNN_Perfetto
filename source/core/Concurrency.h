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
#include "../utils/trace_marker_helper.h"

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

#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __total_size__, __step_size__, __policy__, __target_chunks__, __min_chunk_size__) \
    {                                                                                                                                                  \
        auto cpuBn = (CPUBackend*)backend();                                                                                                           \
        int __static_end__ = (__divides__)[__num__];                                                                                                   \
        cpuBn->initDynamicTaskState(__static_end__, __total_size__, __step_size__, __policy__, __num__, __target_chunks__, __min_chunk_size__);      \
        std::pair<std::function<void(int)>, int> task;                                                                                                 \
        task.second = __num__;                                                                                                                         \
        task.first  = [&, cpuBn](int __iter__) {

#define MNN_CONCURRENCY_HYBRID_END()                      \
    }                                                     \
    ;                                                     \
    auto thrPl = cpuBn->threadPool();                     \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());  \
    }

#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)      \
    {                                                                         \
        int __static_start__ = (__divides__)[__iter__];                       \
        int __static_end__ = (__divides__)[(__iter__) + 1];                   \
        if (__static_start__ < __static_end__) {                              \
            begin_trace_marker("Worker_StaticPhase");                         \
            for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) { \
                __task_func__(__x__);                                         \
            }                                                                 \
            end_trace_marker();                                               \
        }                                                                     \
    }

#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__)                  \
    {                                                                          \
        if ((__cpuBn__)->hasDynamicTasks()) {                                  \
            begin_trace_marker("Worker_DynamicPhase");                         \
            while ((__cpuBn__)->hasDynamicTasks()) {                           \
                auto __chunk__ = (__cpuBn__)->fetchDynamicChunk();             \
                if (__chunk__.first >= __chunk__.second) {                     \
                    break;                                                     \
                }                                                              \
                for (int __x__ = __chunk__.first; __x__ < __chunk__.second; ++__x__) { \
                    __task_func__(__x__);                                      \
                }                                                              \
            }                                                                  \
            end_trace_marker();                                                \
        }                                                                      \
    }

#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__) \
    {                                                                   \
        int __static_start__ = (__divides__)[__iter__];                 \
        int __static_end__ = (__divides__)[(__iter__) + 1];             \
        if (__static_start__ < __static_end__) {                        \
            begin_trace_marker("Worker_StaticPhase");                   \
            __range_func__(__static_start__, __static_end__);           \
            end_trace_marker();                                         \
        }                                                               \
    }

#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__)             \
    {                                                                   \
        if ((__cpuBn__)->hasDynamicTasks()) {                           \
            begin_trace_marker("Worker_DynamicPhase");                  \
            while ((__cpuBn__)->hasDynamicTasks()) {                    \
                auto __chunk__ = (__cpuBn__)->fetchDynamicChunk();      \
                if (__chunk__.first >= __chunk__.second) {              \
                    break;                                              \
                }                                                       \
                __range_func__(__chunk__.first, __chunk__.second);      \
            }                                                           \
            end_trace_marker();                                         \
        }                                                               \
    }

#else
// iOS / OSX
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <stddef.h>

#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __total_size__, __step_size__, __policy__, __target_chunks__, __min_chunk_size__) \
    for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_HYBRID_END() }
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)      \
    {                                                                         \
        int __static_start__ = (__divides__)[__iter__];                       \
        int __static_end__ = (__divides__)[(__iter__) + 1];                   \
        for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) { \
            __task_func__(__x__);                                             \
        }                                                                     \
    }
#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__) {}
#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__) \
    {                                                                   \
        int __static_start__ = (__divides__)[__iter__];                 \
        int __static_end__ = (__divides__)[(__iter__) + 1];             \
        if (__static_start__ < __static_end__) {                        \
            __range_func__(__static_start__, __static_end__);           \
        }                                                               \
    }
#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__) {}

#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) \
dispatch_apply(__num__, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^(size_t __iter__) {
#define MNN_CONCURRENCY_END() \
    (void)(backend()); \
    });

// Windows
#elif defined(_MSC_VER)
#include <omp.h>

#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __total_size__, __step_size__, __policy__, __target_chunks__, __min_chunk_size__) \
    for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_HYBRID_END() }
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)      \
    {                                                                         \
        int __static_start__ = (__divides__)[__iter__];                       \
        int __static_end__ = (__divides__)[(__iter__) + 1];                   \
        for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) { \
            __task_func__(__x__);                                             \
        }                                                                     \
    }
#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__) {}
#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__) \
    {                                                                   \
        int __static_start__ = (__divides__)[__iter__];                 \
        int __static_end__ = (__divides__)[(__iter__) + 1];             \
        if (__static_start__ < __static_end__) {                        \
            __range_func__(__static_start__, __static_end__);           \
        }                                                               \
    }
#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__) {}

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
#define MNN_CONCURRENCY_HYBRID_BEGIN(__iter__, __num__, __divides__, __total_size__, __step_size__, __policy__, __target_chunks__, __min_chunk_size__) \
    for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_HYBRID_END() }
#define MNN_HYBRID_EXECUTE_STATIC(__iter__, __divides__, __task_func__)      \
    {                                                                         \
        int __static_start__ = (__divides__)[__iter__];                       \
        int __static_end__ = (__divides__)[(__iter__) + 1];                   \
        for (int __x__ = __static_start__; __x__ < __static_end__; ++__x__) { \
            __task_func__(__x__);                                             \
        }                                                                     \
    }
#define MNN_HYBRID_EXECUTE_DYNAMIC(__cpuBn__, __task_func__) {}
#define MNN_HYBRID_STATIC_RANGE(__iter__, __divides__, __range_func__) \
    {                                                                   \
        int __static_start__ = (__divides__)[__iter__];                 \
        int __static_end__ = (__divides__)[(__iter__) + 1];             \
        if (__static_start__ < __static_end__) {                        \
            __range_func__(__static_start__, __static_end__);           \
        }                                                               \
    }
#define MNN_HYBRID_DYNAMIC_RANGE(__cpuBn__, __range_func__) {}

#define MNN_CONCURRENCY_BEGIN(__iter__, __num__) \
    _Pragma("omp parallel for") for (int __iter__ = 0; __iter__ < __num__; __iter__++) {
#define MNN_CONCURRENCY_END() }

#endif
#endif
#endif /* concurrency_h */
