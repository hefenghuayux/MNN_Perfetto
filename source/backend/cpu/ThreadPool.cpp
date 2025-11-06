//
//  ThreadPool.cpp
//  MNN
//
//  Created by MNN on 2019/06/30.
//  Copyright © 2018, Alibaba Group Holding Limited
//
#ifdef MNN_USE_THREAD_POOL
#include "backend/cpu/ThreadPool.hpp"
#include <string.h>
#include <unordered_map>
#include <MNN/MNNDefine.h>
#include "ThreadPool.hpp"
#include <android/trace.h> // [保留] 核心 ATrace API
#define MNN_THREAD_POOL_MAX_TASKS 2

// [移除] ScopedTrace 类和 TRACE_SCOPE 宏的定义已被移除

namespace MNN {
static std::unordered_map<long int, ThreadPool*> gInstances;
static std::mutex gInitMutex;
int ThreadPool::init(int numberThread, unsigned long cpuMask, ThreadPool*& threadPool) {
    if (1 >= numberThread) {
        numberThread = 1;
    }
    std::lock_guard<std::mutex> _l(gInitMutex);

    if (gInstances.find(cpuMask) == gInstances.end()){
        gInstances[cpuMask] = new ThreadPool(numberThread);
    }
    threadPool = gInstances[cpuMask];
    if (gInstances[cpuMask]->numberThread() < numberThread){
        return gInstances[cpuMask]->numberThread();
    }
    return numberThread;
}

void ThreadPool::destroy() {
    std::lock_guard<std::mutex> _l(gInitMutex);
    for (auto i= gInstances.begin(); i != gInstances.end(); i++){
        if (i->second){
            delete i->second;
        }
    }
    gInstances.clear();
}

ThreadPool::ThreadPool(int numberThread) {
    mNumberThread = numberThread;
    mActiveCount  = 0;
    mTaskAvailable.resize(MNN_THREAD_POOL_MAX_TASKS);
    mTasks.resize(MNN_THREAD_POOL_MAX_TASKS);
    for (int t = 0; t < mTasks.size(); ++t) {
        mTaskAvailable[t] = true;
        for (int i = 0; i < mNumberThread; ++i) {
            mTasks[t].second.emplace_back(new std::atomic_bool{false});
        }
    }
    for (int i = 1; i < mNumberThread; ++i) {
        int threadIndex = i;
        mWorkers.emplace_back([this, threadIndex]() {
            while (!mStop) {
                while (mActiveCount > 0) {
                    for (int i = 0; i < MNN_THREAD_POOL_MAX_TASKS; ++i) {
                        if (*mTasks[i].second[threadIndex]) {
                            // [修改] 替换 TRACE_SCOPE("Worker_Work")
                            // "Worker_Work" 追踪实际计算
                            ATrace_beginSection("Worker_Work");
                            mTasks[i].first.first(threadIndex);
                            ATrace_endSection();
                            
                            { *mTasks[i].second[threadIndex] = false; }
                        }
                    }
                    
                    // [修改] 替换 TRACE_SCOPE("Worker_IdleSpin")
                    // "Worker_IdleSpin" 追踪空转
                    ATrace_beginSection("Worker_IdleSpin");
                    std::this_thread::yield();
                    ATrace_endSection();
                }
                std::unique_lock<std::mutex> _l(mQueueMutex);
                // 3. 追踪线程的休眠等待
                // [修改] 替换 TRACE_SCOPE("Worker_WaitOnCondition")
                ATrace_beginSection("Worker_WaitOnCondition");
                mCondition.wait(_l, [this] { return mStop || mActiveCount > 0; });
                ATrace_endSection();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> _l(mQueueMutex);
        mStop = true;
    }
    mCondition.notify_all();
    for (auto& worker : mWorkers) {
        worker.join();
    }
    for (auto& task : mTasks) {
        for (auto c : task.second) {
            delete c;
        }
    }
}

int ThreadPool::acquireWorkIndex() {
    std::lock_guard<std::mutex> _l(mQueueMutex);
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
    std::lock_guard<std::mutex> _l(mQueueMutex);
    mTaskAvailable[index] = true;
}

void ThreadPool::active() {
    {
        std::lock_guard<std::mutex> _l(mQueueMutex);
        mActiveCount++;
    }
    mCondition.notify_all();
}
void ThreadPool::deactive() {
    mActiveCount--;
}

void ThreadPool::enqueue(TASK&& task, int index) {
    if (1 >= task.second || 0 > index) {
        for (int i = 0; i < task.second; ++i) {
            task.first(i);
        }
        return;
    }
    enqueueInternal(std::move(task), index);
}
void ThreadPool::enqueueInternal(TASK&& task, int index) {
    if (mActiveCount == 0) {
        // [修改] 替换 TRACE_SCOPE("Pool_Inactive_Run_On_Main")
        ATrace_beginSection("Pool_Inactive_Run_On_Main");
        for (int i = 0; i < task.second; ++i) {
            task.first(i);
        }
        ATrace_endSection();
        return;
    }
    int workSize = task.second;
    if (workSize > mNumberThread) {
        mTasks[index].first = std::make_pair(
            [workSize, &task, this](int tId) {
                for (int v = tId; v < workSize; v += mNumberThread) {
                    task.first(v);
                }
            },
            mNumberThread);
        workSize = mNumberThread;
    } else {
        mTasks[index].first = std::move(task);
    }
    {
        // (可选) 追踪任务分发的开销
        // [修改] 替换 TRACE_SCOPE("Task_Setup")
        ATrace_beginSection("Task_Setup");
        for (int i = 1; i < workSize; ++i) {
            *mTasks[index].second[i] = true;
        }
        ATrace_endSection();
    }
    // 1. 追踪主线程（T0）的实际工作时间
    // [修改] 替换 TRACE_SCOPE("MainThread_Work")
    ATrace_beginSection("MainThread_Work");
    mTasks[index].first.first(0);
    ATrace_endSection();
    
    // 2. 追踪主线程的“忙等”同步时间
    // [修改] 替换 TRACE_SCOPE("MainThread_Wait")
    ATrace_beginSection("MainThread_Wait");
    bool complete = true;
    do {
        complete = true;
        for (int i = 1; i < workSize; ++i) {
            if (*mTasks[index].second[i]) {
                complete = false;
                break;
            }
        }
        std::this_thread::yield();
    } while (!complete);
    ATrace_endSection();
}
} // namespace MNN
#endif