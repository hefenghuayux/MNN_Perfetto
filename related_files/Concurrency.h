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

// ===================== Prefill/Decode 调度宏 =====================
// prefill: 纯 work-steal
// decode:  纯 dynamic
#define MNN_CONCURRENCY_PREFILL_WORKSTEAL_BEGIN(iter_, num_, divides_, total_size_, step_size_) \
    {                                                                                             \
        auto cpuBn = (CPUBackend*)backend();                                                      \
        cpuBn->initPrefillWorkStealState((divides_), (num_), (total_size_), (step_size_));       \
        std::pair<std::function<void(int)>, int> task;                                            \
        task.second = (num_);                                                                     \
        task.first  = [&, cpuBn](int iter_) {

#define MNN_CONCURRENCY_PREFILL_WORKSTEAL_END()                 \
    }                                                           \
    ;                                                           \
    auto thrPl = cpuBn->threadPool();                           \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());        \
    cpuBn->flushPrefillWorkStealStats();                        \
    }

#define MNN_CONCURRENCY_DECODE_DYNAMIC_BEGIN(iter_, num_, total_size_, step_size_) \
    {                                                                                \
        auto cpuBn = (CPUBackend*)backend();                                         \
        cpuBn->initDecodeDynamicState((total_size_), (step_size_), (num_));          \
        std::pair<std::function<void(int)>, int> task;                               \
        task.second = (num_);                                                        \
        task.first  = [&, cpuBn](int iter_) {

#define MNN_CONCURRENCY_DECODE_DYNAMIC_END()                   \
    }                                                          \
    ;                                                          \
    auto thrPl = cpuBn->threadPool();                          \
    thrPl->enqueue(std::move(task), cpuBn->taskIndex());       \
    cpuBn->flushDecodeDynamicStats();                          \
    }

#define MNN_PREFILL_WORKSTEAL_RANGE(__cpuBn__, __iter__, __range_func__) \
    do {                                                                  \
        while (true) {                                                    \
            auto __chunk__ = (__cpuBn__)->fetchPrefillWorkStealChunk((int)(__iter__)); \
            if (__chunk__.first >= __chunk__.second) {                    \
                break;                                                     \
            }                                                              \
            __range_func__(__chunk__.first, __chunk__.second);            \
        }                                                                  \
    } while (0)

#define MNN_DECODE_DYNAMIC_RANGE(__cpuBn__, __iter__, __range_func__) \
    do {                                                               \
        while (true) {                                                 \
            auto __chunk__ = (__cpuBn__)->fetchDecodeDynamicChunk((int)(__iter__)); \
            if (__chunk__.first >= __chunk__.second) {                 \
                break;                                                 \
            }                                                          \
            __range_func__(__chunk__.first, __chunk__.second);         \
        }                                                              \
    } while (0)

#else
// iOS / OSX / Windows / Other: 非线程池模式后备实现
#define MNN_CONCURRENCY_PREFILL_WORKSTEAL_BEGIN(iter_, num_, divides_, total_size_, step_size_) \
    {                                                                                             \
        auto cpuBn = (CPUBackend*)backend();                                                      \
        cpuBn->initPrefillWorkStealState((divides_), (num_), (total_size_), (step_size_));       \
        for (int iter_ = 0; iter_ < (num_); ++iter_) {

#define MNN_CONCURRENCY_PREFILL_WORKSTEAL_END() \
        }                                       \
        cpuBn->flushPrefillWorkStealStats();    \
    }

#define MNN_CONCURRENCY_DECODE_DYNAMIC_BEGIN(iter_, num_, total_size_, step_size_) \
    {                                                                                \
        auto cpuBn = (CPUBackend*)backend();                                         \
        cpuBn->initDecodeDynamicState((total_size_), (step_size_), (num_));          \
        for (int iter_ = 0; iter_ < (num_); ++iter_) {

#define MNN_CONCURRENCY_DECODE_DYNAMIC_END() \
        }                                    \
        cpuBn->flushDecodeDynamicStats();    \
    }

#define MNN_PREFILL_WORKSTEAL_RANGE(__cpuBn__, __iter__, __range_func__) \
    do {                                                                  \
        while (true) {                                                    \
            auto __chunk__ = (__cpuBn__)->fetchPrefillWorkStealChunk((int)(__iter__)); \
            if (__chunk__.first >= __chunk__.second) {                    \
                break;                                                     \
            }                                                              \
            __range_func__(__chunk__.first, __chunk__.second);            \
        }                                                                  \
    } while (0)

#define MNN_DECODE_DYNAMIC_RANGE(__cpuBn__, __iter__, __range_func__) \
    do {                                                               \
        while (true) {                                                 \
            auto __chunk__ = (__cpuBn__)->fetchDecodeDynamicChunk((int)(__iter__)); \
            if (__chunk__.first >= __chunk__.second) {                 \
                break;                                                 \
            }                                                          \
            __range_func__(__chunk__.first, __chunk__.second);         \
        }                                                              \
    } while (0)

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
