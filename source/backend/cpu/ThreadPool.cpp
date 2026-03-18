//
//  ThreadPool.cpp
//  MNN
//
//  Created by MNN on 2019/06/30.
//

#ifdef MNN_USE_THREAD_POOL

#include "backend/cpu/ThreadPool.hpp"

#include <algorithm>
#include <string>
#include <string.h>
#include <unordered_map>

#include <MNN/MNNDefine.h>

#include "AutoTuner.hpp"
#include "ThreadPool.hpp"
#include "trace_marker_helper.h"

#define MNN_THREAD_POOL_MAX_TASKS 2

#if !defined(_WIN64) && !defined(__APPLE__) && !defined(__OpenBSD__) && (defined(__linux__) || defined(__ANDROID__))
#include <errno.h>
#include <sched.h>
#include <string.h>
#endif

namespace MNN {

static std::unordered_map<std::string, ThreadPool*> gInstances;
static std::mutex gInitMutex;

static std::string _poolKey(int numberThread, unsigned long cpuMask) {
    return std::to_string(numberThread) + "#" + std::to_string(cpuMask);
}

static std::vector<int> _coreIdsFromMask(unsigned long cpuMask) {
    std::vector<int> coreIds;
    for (int i = static_cast<int>(sizeof(cpuMask) * 8) - 1; i >= 0; --i) {
        if ((cpuMask >> i) & 1UL) {
            coreIds.push_back(i);
        }
    }
    return coreIds;
}

static void _set_thread_affinity(int core_id) {
    if (core_id < 0) {
        return;
    }
#if !defined(_WIN64) && !defined(__APPLE__) && !defined(__OpenBSD__) && (defined(__linux__) || defined(__ANDROID__))
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        MNN_PRINT("Error setting thread affinity for core %d: %s\n", core_id, strerror(errno));
    }
#else
    (void)core_id;
#endif
}

static int _pickCoreForThread(int threadIndex, unsigned long cpuMask) {
    if (cpuMask == 0) {
        return -1;
    }
    auto activeCores = _coreIdsFromMask(cpuMask);
    if (threadIndex < 0 || threadIndex >= static_cast<int>(activeCores.size())) {
        return -1;
    }
    return activeCores[threadIndex];
}

int ThreadPool::init(int numberThread, unsigned long cpuMask, ThreadPool*& threadPool) {
    if (numberThread <= 1) {
        numberThread = 1;
    }

    std::vector<int> coreIds = _coreIdsFromMask(cpuMask);
    std::lock_guard<std::mutex> lock(gInitMutex);

    auto key = _poolKey(numberThread, cpuMask);
    auto iter = gInstances.find(key);
    if (iter == gInstances.end()) {
        iter = gInstances.emplace(key, new ThreadPool(numberThread, coreIds)).first;
    }
    threadPool = iter->second;

    if (!coreIds.empty()) {
        _set_thread_affinity(coreIds.front());
    }

    if (threadPool->numberThread() < numberThread) {
        return threadPool->numberThread();
    }
    return numberThread;
}

void ThreadPool::destroy() {
    std::lock_guard<std::mutex> lock(gInitMutex);
    for (auto& entry : gInstances) {
        delete entry.second;
    }
    gInstances.clear();
}

ThreadPool::ThreadPool(int numberThread, const std::vector<int>& core_ids)
    : mCoreIDs(core_ids)
    , mNumberThread(numberThread) {
    mPhaseDispatchWidth.store(mNumberThread, std::memory_order_relaxed);
    mTaskAvailable.resize(MNN_THREAD_POOL_MAX_TASKS);
    mTasks.resize(MNN_THREAD_POOL_MAX_TASKS);
    for (int t = 0; t < static_cast<int>(mTasks.size()); ++t) {
        mTaskAvailable[t] = true;
        for (int i = 0; i < mNumberThread; ++i) {
            mTasks[t].second.emplace_back(new std::atomic_bool{false});
        }
    }

    for (int i = 1; i < mNumberThread; ++i) {
        int threadIndex = i;
        mWorkers.emplace_back([this, threadIndex]() {
            unsigned long currentBoundMask = 0;
            while (!mStop.load(std::memory_order_relaxed)) {
                int dispatchWidth = mPhaseDispatchWidth.load(std::memory_order_relaxed);
                while (!mStop.load(std::memory_order_relaxed) &&
                       mActiveCount.load(std::memory_order_acquire) > 0 &&
                       threadIndex < dispatchWidth) {
                    unsigned long globalMask = AutoTuner::getInstance()->getFastAffinityMask();
                    if (globalMask != currentBoundMask && globalMask != 0) {
                        currentBoundMask = globalMask;
                        _set_thread_affinity(_pickCoreForThread(threadIndex, globalMask));
                    }
                    for (int taskIndex = 0; taskIndex < MNN_THREAD_POOL_MAX_TASKS; ++taskIndex) {
                        if (*mTasks[taskIndex].second[threadIndex]) {
                            begin_trace_marker("Worker_Work");
                            mTasks[taskIndex].first.first(threadIndex);
                            end_trace_marker();
                            *mTasks[taskIndex].second[threadIndex] = false;
                        }
                    }
                    begin_trace_marker("Worker_IdleSpin");
                    std::this_thread::yield();
                    end_trace_marker();
                    dispatchWidth = mPhaseDispatchWidth.load(std::memory_order_relaxed);
                }

                begin_trace_marker("Wait_Idle_Lock");
                std::unique_lock<std::mutex> lock(mQueueMutex);
                end_trace_marker();
                begin_trace_marker("Worker_WaitOnCondition");
                mCondition.wait(lock, [this, threadIndex]() {
                    return mStop.load(std::memory_order_relaxed) ||
                           (mActiveCount.load(std::memory_order_acquire) > 0 &&
                            threadIndex < mPhaseDispatchWidth.load(std::memory_order_relaxed));
                });
                end_trace_marker();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mStop.store(true, std::memory_order_release);
    }
    mCondition.notify_all();
    for (auto& worker : mWorkers) {
        worker.join();
    }
    for (auto& task : mTasks) {
        for (auto* done : task.second) {
            delete done;
        }
    }
}

int ThreadPool::acquireWorkIndex() {
    std::lock_guard<std::mutex> lock(mQueueMutex);
    for (int i = 0; i < MNN_THREAD_POOL_MAX_TASKS; ++i) {
        if (mTaskAvailable[i]) {
            mTaskAvailable[i] = false;
            return i;
        }
    }
    return -1;
}

void ThreadPool::releaseWorkIndex(int index) {
    if (index < 0 || index >= MNN_THREAD_POOL_MAX_TASKS) {
        return;
    }
    std::lock_guard<std::mutex> lock(mQueueMutex);
    mTaskAvailable[index] = true;
}

void ThreadPool::active() {
    int dispatchWidth = AutoTuner::getInstance()->getActiveThreadCount();
    if (dispatchWidth < 1) {
        dispatchWidth = 1;
    }
    dispatchWidth = std::min(dispatchWidth, mNumberThread);
    {
        begin_trace_marker("Wait_Main_Active_Lock");
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mPhaseDispatchWidth.store(dispatchWidth, std::memory_order_relaxed);
        mActiveCount.fetch_add(1, std::memory_order_release);
        end_trace_marker();
    }

    unsigned long globalMask = AutoTuner::getInstance()->getFastAffinityMask();
    if (globalMask != 0) {
        _set_thread_affinity(_pickCoreForThread(0, globalMask));
    }

    begin_trace_marker("Main_Notify_All");
    mCondition.notify_all();
    end_trace_marker();
}

void ThreadPool::deactive() {
    mActiveCount.fetch_sub(1, std::memory_order_release);
}

void ThreadPool::enqueue(TASK&& task, int index) {
    if (task.second <= 1 || index < 0) {
        for (int i = 0; i < task.second; ++i) {
            task.first(i);
        }
        return;
    }
    enqueueInternal(std::move(task), index);
}

void ThreadPool::enqueueInternal(TASK&& task, int index) {
    if (mActiveCount.load(std::memory_order_acquire) == 0) {
        begin_trace_marker("Pool_Inactive_Run_On_Main");
        for (int i = 0; i < task.second; ++i) {
            task.first(i);
        }
        end_trace_marker();
        return;
    }

    auto taskFunc = std::move(task.first);
    int logicalWorkSize = task.second;
    int dispatchWidth = std::min({logicalWorkSize,
                                  mPhaseDispatchWidth.load(std::memory_order_relaxed),
                                  mNumberThread});
    if (dispatchWidth < 1) {
        dispatchWidth = 1;
    }

    if (logicalWorkSize > dispatchWidth) {
        mTasks[index].first = std::make_pair(
            [taskFunc, logicalWorkSize, dispatchWidth](int tId) {
                for (int logicalId = tId; logicalId < logicalWorkSize; logicalId += dispatchWidth) {
                    taskFunc(logicalId);
                }
            },
            dispatchWidth);
    } else {
        mTasks[index].first = std::make_pair(std::move(taskFunc), logicalWorkSize);
        dispatchWidth = logicalWorkSize;
    }

    begin_trace_marker("Task_Setup");
    for (int i = 1; i < dispatchWidth; ++i) {
        *mTasks[index].second[i] = true;
    }
    end_trace_marker();

    begin_trace_marker("MainThread_Work");
    mTasks[index].first.first(0);
    end_trace_marker();

    begin_trace_marker("MainThread_Wait");
    bool complete = true;
    do {
        complete = true;
        for (int i = 1; i < dispatchWidth; ++i) {
            if (*mTasks[index].second[i]) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            std::this_thread::yield();
        }
    } while (!complete);
    end_trace_marker();
}

} // namespace MNN

#endif
