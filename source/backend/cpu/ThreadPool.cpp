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
#define MNN_THREAD_POOL_MAX_TASKS 2

// [新代码] 添加绑核所需的头文件
#if !defined(_WIN64) && !defined(__APPLE__) && !defined(__OpenBSD__) && (defined(__linux__) || defined(__ANDROID__))
#include <sched.h> // for sched_setaffinity
#include <errno.h> // for errno
#include <string.h> // for strerror
#endif
// [新代码结束]

namespace MNN {
static std::unordered_map<long int, ThreadPool*> gInstances;
static std::mutex gInitMutex;

// [修改] 绑核函数现在接收一个 int core_id，而不是一个 mask
/**
 * @brief Set thread affinity. Pin current thread to a *single* core.
 * @param[in] core_id The ID of the core to pin to. If -1, no pinning is done.
 */
static void _set_thread_affinity(int core_id) {
    // [修改] 如果 core_id < 0，我们将其视为不绑核的信号
    if (core_id < 0) {
        return;
    }
#if !defined(_WIN64) && !defined(__APPLE__) && !defined(__OpenBSD__) && (defined(__linux__) || defined(__ANDROID__))
    cpu_set_t set;
    CPU_ZERO(&set);
    // [修改] 只将这一个 core_id 添加到集合中
    CPU_SET(core_id, &set);

    // sched_setaffinity(0, ...) 0 表示“当前线程”
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        MNN_PRINT("Error setting thread affinity for core %d: %s\n", core_id, strerror(errno));
    }
#endif
}
// [修改结束]
int ThreadPool::init(int numberThread, unsigned long cpuMask, ThreadPool*& threadPool) {
    if (1 >= numberThread) {
        numberThread = 1;
    }
    
    // [新代码]
    // 1. 解析 cpuMask (如 0xf0)，将其转换为核心ID列表 (如 [4, 5, 6, 7])
    std::vector<int> core_ids;
    if (cpuMask != 0) {
        for (int i = (sizeof(cpuMask) * 8) - 1; i >= 0; --i) {
            if ((cpuMask >> i) & 1) { // 逻辑不变：检查第 i 位是否为 1
                core_ids.push_back(i); // 先放入的是大号核心 (例如 7)
            }
        }
    }
    // [新代码结束]

    std::lock_guard<std::mutex> _l(gInitMutex);

    if (gInstances.find(cpuMask) == gInstances.end()){
        // [修改] 将 *解析后的核心列表* 传递给构造函数
        gInstances[cpuMask] = new ThreadPool(numberThread, core_ids);
    }
    threadPool = gInstances[cpuMask];

    // [新代码]
    // 2. 绑定【主线程】(即 T0，调用 init 的这个线程)
    int main_thread_core = -1; // 默认不绑核
    if (!core_ids.empty()) {
        main_thread_core = core_ids[0]; // 主线程 (T0) 绑定到列表中的第一个核心
    }
    _set_thread_affinity(main_thread_core);
    // [新代码结束]

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

// [修改] 修改构造函数签名以接受核心列表
ThreadPool::ThreadPool(int numberThread, const std::vector<int>& core_ids) {
    mNumberThread = numberThread;
    mCoreIDs = core_ids; // [修改] 保存核心列表
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
        int threadIndex = i; // T1, T2, T3 ...
        mWorkers.emplace_back([this, threadIndex]() {
            
            // [新代码]
            // 3. 为每个工作线程 T_i 绑定核心 core_ids[i]
            int core_to_pin = -1; // 默认不绑核
            
            // 检查 mCoreIDs 列表是否足够长，以覆盖当前 threadIndex
            // (threadIndex 对应 T_i, 例如 T1 对应 index 1)
            if (threadIndex < mCoreIDs.size()) {
                core_to_pin = mCoreIDs[threadIndex];
            }
            // 在工作线程内部调用绑核
            _set_thread_affinity(core_to_pin);
            // [新代码结束]

            while (!mStop) {
                while (mActiveCount > 0) {
                    for (int i = 0; i < MNN_THREAD_POOL_MAX_TASKS; ++i) {
                        if (*mTasks[i].second[threadIndex]) {
                            mTasks[i].first.first(threadIndex);
                            { *mTasks[i].second[threadIndex] = false; }
                        }
                    }
                    
                    std::this_thread::yield();
                }
                std::unique_lock<std::mutex> _l(mQueueMutex);
                mCondition.wait(_l, [this] { return mStop || mActiveCount > 0; });
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
        for (int i = 0; i < task.second; ++i) {
            task.first(i);
        }
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
        for (int i = 1; i < workSize; ++i) {
            *mTasks[index].second[i] = true;
        }
    }
    mTasks[index].first.first(0);

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
}
} // namespace MNN
#endif
