//
//  CPUBackend.hpp
//  MNN
//
//  Created by MNN on 2018/07/06.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#ifndef CPUBackend_hpp
#define CPUBackend_hpp

#include <map>
#include <memory>
#include <MNN/AutoTime.hpp>
#include "core/Backend.hpp"
#include "core/Execution.hpp"
#include "core/BufferAllocator.hpp"
#include "MNN_generated.h"

#ifdef MNN_USE_THREAD_POOL
#include "ThreadPool.hpp"
#endif
#include "AutoTuner.hpp"

namespace MNN {
class WorkerThread;
class CPUResizeCache;
class CPURuntime : public Runtime {
public:
    struct DynamicAllocator {
        std::shared_ptr<BufferAllocator> mDynamicAllocator;
        std::shared_ptr<BufferAllocator> mDynamicAllocatorBackup;
        BufferAllocator* mCurrentDynamicAllocator = nullptr;
        std::vector<std::shared_ptr<CPUResizeCache>> mCacheGroup;
    };
    friend class CPUBackend;
    CPURuntime(const Backend::Info& info);
    virtual ~ CPURuntime();
    int onGetRuntimeStatus(RuntimeStatus statusEnum) const override;
    virtual Backend* onCreate(const BackendConfig* config, Backend* origin) const override;
    virtual void onReset(int numberThread, const BackendConfig* config, bool full) override;
    virtual void onGabageCollect(int level) override;
    virtual float onGetMemoryInMB() override;
    virtual CompilerType onGetCompilerType() const override {
        return Compiler_Loop;
    }
    virtual void onConcurrencyBegin() const override;
    virtual void onConcurrencyEnd() const override;
    virtual bool onCheckInfo(Backend::Info& info) const override;

    SingleBufferWithAllocator* buffer(int index) const;
    BufferAllocator* createDynamicBufferAlloctor(int index) const;

private:
    void _bindCPUCore() const;
    void _resetThreadPool() const;
    void _validateCpuIds() const;
    mutable std::shared_ptr<EagerBufferAllocator> mStaticAllocator;
    mutable int mThreadNumber;
    mutable std::vector<int> mCpuIds;
    mutable unsigned long mCpuMask;
#ifdef MNN_USE_THREAD_POOL
    mutable ThreadPool* mThreadPool = nullptr;
#endif
#ifdef MNN_USE_THREAD_POOL
    mutable int mTaskIndex = -1;
    mutable int mThreadOpen = 0;
#endif
    BackendConfig::MemoryMode mMemory;
    BackendConfig::PowerMode mPower;
    BackendConfig::PrecisionMode mPrecision;

    // Backend features
    // CPU features
    static Backend*(*gExtraCreate)(const Runtime* runtime);
    size_t mFlags = 0;
    mutable int mCurrentTID = 0;
    mutable std::vector<SingleBufferWithAllocator> mDynamic;
    mutable std::vector<SingleBufferWithAllocator> mDynamicMmap;
    mutable std::shared_ptr<DynamicAllocator> mSharedDmaInfo;
    mutable std::shared_ptr<EagerBufferAllocator> mStaticAllocatorRaw;
    mutable std::shared_ptr<EagerBufferAllocator> mStaticAllocatorMMap;
};
struct CoreFunctions;
struct CoreInt8Functions;
struct MatmulRelatedFunctions;

class CPUMemObj : public Backend::MemObj {
public:
    CPUMemObj(BufferAllocator* allocator, MemChunk chunk, int size) : mAllocator(allocator), mChunk(chunk), mSize(size) {}
    virtual ~ CPUMemObj() {
        if (mAllocator) {
            mAllocator->free(mChunk);
        }
    }
    virtual MemChunk chunk() {
        return mChunk;
    }
    inline int getSize() const {
        return mSize;
    }
private:
    BufferAllocator* mAllocator;
    MemChunk mChunk;
    int mSize;
};

struct DivideSchedulePlan {
    int total_size = 0;
    int total_static = 0;
    int dynamic_size = 0;
    int step_size = 1;
    int target_chunks = 0;
    int theoretical_dynamic_chunks = 0;
    int active_threads = 1;
    int min_chunk_size = 1;
    SchedulerPolicy policy = SchedulerPolicy::DYNAMIC;
};

class CPUBackend : public Backend {
public:
    CPUBackend(const CPURuntime* runtime, BackendConfig::PrecisionMode precision, BackendConfig::MemoryMode memory, MNNForwardType type = MNN_FORWARD_CPU, size_t flags = 0);
    virtual ~CPUBackend();

    // Return sizeDivide, scheduleNumber aligned memory
    std::pair<int, int> multiThreadDivide(int size) const;
    virtual bool onSelectDynamicAllocator(int index, int maxIndex) override;
    // dividedSize's length should be larger than threadNumber
    void computeDivideSizes(int size, int* dst, float computeI = 0.f) const;
    
    /**
     * @brief [Phase 1] 混合调度版本的任务划分
     * @param size 总任务数
     * @param dst 输出数组，存储每个线程的任务边界（累积形式）
     * @param computeI 用于判断是否启用异构分配的阈值
     * 
     * 从 AutoTuner 获取当前阶段参数（不再需要传递 is_prefill）
     * 逻辑：
     * 1. 从 AutoTuner 获取 static_ratio, affinity_mask 和 dynamic_blocks
     * 2. 静态部分按 mGroupWithComputeRate 性能比分配
     * 3. 动态部分通过原子计数器竞争获取
     * 
     * @return pair<total_size, step> 用于执行时初始化动态状态
     */
    DivideSchedulePlan computeDivideSizesHybrid(int size, int* dst, float computeI = 0.f) const;
    
    /**
     * @brief 初始化动态任务调度状态
     * @param static_end 静态任务结束边界
     * @param total_size 总任务数
     * @param step_size 每次抢占的步长
     */
    void initDynamicTaskState(int static_end,
                              int total_size,
                              int step_size,
                              SchedulerPolicy policy = SchedulerPolicy::DYNAMIC,
                              int active_threads = 0,
                              int target_chunks = 0,
                              int min_chunk_size = 0) const;
    
    /**
     * @brief 抢占下一个动态任务块
     * @return pair<start, end> 任务区间，若 start >= end 表示任务已耗尽
     */
    std::pair<int, int> fetchDynamicChunk() const;
    
    /**
     * @brief 检查是否还有剩余动态任务
     */
    bool hasDynamicTasks() const;

public:
    virtual MemObj* onAcquire(const Tensor* nativeTensor, StorageType storageType) override;
    virtual bool onClearBuffer() override;
    virtual void onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const override;

    virtual Execution* onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op) override;

    virtual void onExecuteBegin() const override;
    virtual void onExecuteEnd() const override;
    virtual void* onMapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* srcTensor) override;

    virtual bool onUnmapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* dstTensor, void* mapPtr) override;

    virtual void onResizeBegin() override;
    virtual ErrorCode onResizeEnd() override;

    const CoreFunctions* functions() const {
        return mCoreFunctions;
    }
    
    const MatmulRelatedFunctions* int8GemmFunctions() const {
        return mRelatedFunctions;
    }
    // Return element size for Tensor, conside pack
    size_t getTensorSize(const Tensor* tensor, bool multiBytes = false) const;
    const CoreInt8Functions* int8Functions() const {
        return mInt8CoreFunctions;
    }
    void _resetDynamicMemory() const;
public:
    class Creator {
    public:
        virtual Execution* onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                    const MNN::Op* op, Backend* backend) const = 0;
    };

    static bool addCreator(OpType t, Creator* c);

    inline int threadNumber() const {
        return mThreadNumber;
    }

    BufferAllocator* getBufferAllocator(bool defer_allocator = true) const {
        return mDmaInfo->mCurrentDynamicAllocator;
    }

    BackendConfig::MemoryMode memoryMode() const {
        return mMemory;
    }
    BackendConfig::PrecisionMode precisionMode() const {
        return mPrecisionMode;
    }
    CPUResizeCache* getCache() const {
        return mCache;
    }

    virtual const Runtime* getRuntime() override;

#ifdef MNN_USE_THREAD_POOL
    inline int taskIndex() const {return mRuntime->mTaskIndex;}
    inline ThreadPool* threadPool() const {return mRuntime->mThreadPool;}
#endif
    static void initCreatorMap();
    static size_t getBytes(const Backend* backend, const Tensor* output);
    static DataType getDataType(const Tensor* tensor);
    friend class CPURuntime;
    void enqueueTask(std::function<int()>&& task);

protected:
    MemObj* allocBuffer(size_t size, Tensor* dest,  StorageType storageType);
    CoreFunctions* mCoreFunctions;
    CoreInt8Functions* mInt8CoreFunctions;
    const MatmulRelatedFunctions* mRelatedFunctions;
private:
    mutable std::shared_ptr<WorkerThread> mInitWorkQueue;
    mutable int mThreadNumber = 1;
    float mComputeI = 0.f;
    
    // ===== 动态调度相关成员（Cache Line 对齐避免 False Sharing）=====
    mutable DynamicTaskState mDynamicState;

    std::shared_ptr<CPURuntime::DynamicAllocator> mDmaInfo;
    CPURuntime* mRuntime;
    BackendConfig::PrecisionMode mPrecisionMode;
    BackendConfig::MemoryMode mMemory;
    static std::map<OpType, CPUBackend::Creator*>* gCreator;
    CPUResizeCache* mCache;
};
/** execution cast wrapper. insert tensor cast dynamic. */
class CastWrapExecution : public Execution {
public:
    CastWrapExecution(Backend* backend, DataType runT)
                    : Execution(backend), mRunType(runT) {}
    virtual ErrorCode onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) override;
private:
    DataType mRunType;
};


#define REGISTER_CPU_OP_CREATOR(name, opType)     \
    void ___##name##__##opType##__() {            \
        static name _temp;\
        CPUBackend::addCreator(opType, &_temp); \
    }

#ifdef MNN_SUPPORT_DEPRECATED_OP
#define REGISTER_CPU_OP_CREATOR_OLD(name, opType)     \
    void ___##name##__##opType##__() {            \
        static name _temp;\
        CPUBackend::addCreator(opType, &_temp); \
    }

#else
#define REGISTER_CPU_OP_CREATOR_OLD(name, opType)     \
    void ___##name##__##opType##__() {            \
    }
#endif

#define REGISTER_CPU_OP_CREATOR_RENDER(name, opType)     \
    void ___##name##__##opType##__() {            \
        static name _temp;\
        CPUBackend::addCreator(opType, &_temp); \
    }

#define REGISTER_CPU_OP_CREATOR_TRANSFORMER(name, opType)     \
    void ___##name##__##opType##__() {            \
        static name _temp;\
        CPUBackend::addCreator(opType, &_temp); \
    }

} // namespace MNN

#endif /* CPUBackend_hpp */
