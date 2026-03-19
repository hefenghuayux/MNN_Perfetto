//
//  ThreadPool.hpp
//  MNN
//
//  Created by MNN on 2019/06/30.
//

#ifndef CPU_INTHREADPOOL_H
#define CPU_INTHREADPOOL_H

#ifdef MNN_USE_THREAD_POOL

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <MNN/MNNDefine.h>

namespace MNN {

class MNN_PUBLIC ThreadPool {
public:
    typedef std::pair<std::function<void(int)>, int> TASK;

    int numberThread() const {
        return mNumberThread;
    }

    void enqueue(TASK&& task, int index);
    void active();
    void deactive();

    int acquireWorkIndex();
    void releaseWorkIndex(int index);

    static int init(int numberThread, unsigned long cpuMask, ThreadPool*& threadPool);
    static void destroy();

private:
    void enqueueInternal(TASK&& task, int index);

    ThreadPool(int numberThread, const std::vector<int>& core_ids);
    ~ThreadPool();

    std::vector<std::thread> mWorkers;
    std::vector<bool> mTaskAvailable;
    std::atomic<bool> mStop{false};

    std::vector<std::pair<TASK, std::vector<std::atomic_bool*>>> mTasks;
    std::condition_variable mCondition;
    std::mutex mQueueMutex;

    std::vector<int> mCoreIDs;
    int mNumberThread = 0;
    std::atomic_int mActiveCount{0};
    std::atomic_int mPhaseDispatchWidth{1};
};

} // namespace MNN

#endif
#endif
