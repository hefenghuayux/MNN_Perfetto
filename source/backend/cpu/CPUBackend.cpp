//  ConvInt8TiledExecutor.cpp
//  MNN
//
//  Created by MNN on 2019/5/17.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "ConvInt8TiledExecutor.hpp"
#include "ConvolutionTiledExecutor.hpp"
#include "core/Macro.h"
#include "core/BufferAllocator.hpp"

#include <math.h>
#include "backend/cpu/CPUBackend.hpp"
#include "core/Concurrency.h"
#include "core/TensorUtils.hpp"


#define QUANT_INFO_BYTES 4
#define WEIGHT_ONLINE_REORDER 8
namespace MNN {

ConvInt8TiledExecutor::ConvInt8TiledExecutor(Backend* backend, const Op* op): CPUConvolution(op->main_as_Convolution2D()->common(), backend) {}

ConvInt8TiledExecutor::ConvInt8TiledExecutor(Backend* backend, const Op* op, std::shared_ptr<ResourceInt8> res): CPUConvolution(op->main_as_Convolution2D()->common(), backend), mResourceInt8(res) {
    if (!res->mDynamicQuant) {
        mMutableResource.reset(new MutableResourceInt8(res, backend));
        mValid = mMutableResource->mValid;
    }
}

ConvInt8TiledExecutor::~ConvInt8TiledExecutor() {
    // Do nothing
}

bool ConvInt8TiledExecutor::onClone(Backend* bn, const Op* op, Execution** dst) {
    return false;
}

ErrorCode ConvInt8TiledExecutor::onResize(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    if (nullptr != mMutableResource) {
        mMutableResource->updateInputOutputScale(TensorUtils::getQuantInfo(inputs[0]), TensorUtils::getQuantInfo(outputs[0]));
    }
    CPUConvolution::onResize(inputs, outputs);
    ConvolutionTiledExecutor::setIm2ColParameter(mIm2ColParamter, mCommon, inputs[0], outputs[0], mPadX, mPadY, static_cast<CPUBackend*>(backend())->functions(), static_cast<CPUBackend*>(backend())->int8Functions());
    return NO_ERROR;
}
void CPUBackend::computeDivideSizes(int size, int* dst, float avgDiv) const {
    if (mGroupWithComputeRate.size() <= 1 || (avgDiv > 0 && avgDiv < mComputeI)) {
        // Avg divide
        int length = UP_DIV(size, mThreadNumber);
        int cur = length;
        for (int i=0; i<mThreadNumber; ++i) {
            dst[i] = cur;
            cur = cur + length;
            cur = ALIMIN(cur, size);
        }
        return;
    }

    int cur = 0;
    int curPos = 0;
    for (auto& group : mGroupWithComputeRate) {
        int currentGroupTotal = (int)(ceilf((float)size*group.first));
        int length = UP_DIV(currentGroupTotal, group.second);
        for (int i=0; i<group.second; ++i) {
            cur = cur + length;
            cur = ALIMIN(cur, size);
            dst[curPos+i] = cur;
        }
        curPos += group.second;
    }
}

void CPURuntime::_bindCPUCore() const {
    if (mCpuIds.empty()) {
        return;
    }
    auto tid = MNNGetCurrentPid();
    if (tid == mCurrentTID) {
        return;
    }
    mCurrentTID = tid;
    // Bind CPU Core
    std::vector<std::pair<const int*, int>> lockCPUIndexes(mThreadNumber);
    for (int v=0; v<mThreadNumber; ++v) {
        lockCPUIndexes[v] = std::make_pair(mCpuIds.data(), mCpuIds.size());
    }
    // Set CPU Affinity
#ifdef _OPENMP
    auto threadsNumber = mThreadNumber;
    std::vector<int> result(threadsNumber, 0);
#pragma omp parallel for
    for (int i = 0; i < threadsNumber; ++i) {
        result[i] = MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
    }
#endif
#ifdef MNN_USE_THREAD_POOL
    if (nullptr != mThreadPool) {
        mThreadPool->active();
        ThreadPool::TASK task = std::make_pair([&](int i) {
            MNNSetSchedAffinity(lockCPUIndexes[i].first, lockCPUIndexes[i].second);
        }, mThreadNumber);
        mThreadPool->enqueue(&task, mTaskIndex);
        mThreadPool->deactive();
    }
#endif
}
void CPURuntime::_resetThreadPool() const {
    mThreadNumber = std::max(1, mThreadNumber);
    mThreadNumber = std::min(mThreadNumber, MAX_THREAD_NUMBER);
#ifdef MNN_USE_THREAD_POOL
    if (mThreadNumber > 1) {
        mThreadNumber = ALIMIN(ThreadPool::init(mThreadNumber, mCpuMask, mThreadPool), mThreadNumber);
    }
#endif
    // Reset tid to rebind cpu if necessary
    mCurrentTID = 0;
}
void CPURuntime::_validateCpuIds() const{
    bool valid = true;

    do {
        if (mCpuIds.empty()) {
            valid = false;
            break;
        }

        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.empty()) {
            valid = false;
            break;
        }

        std::unordered_map<int, bool> cpuLittleMap;
        for (auto id : cpuInfo->groups[0].ids) {
            cpuLittleMap[id] = true;
        }
        for (size_t i = 1; i < cpuInfo->groups.size(); i++) {
            for (auto id : cpuInfo->groups[i].ids) {
                cpuLittleMap[id] = false;
            }
        }

        if (cpuLittleMap.find(mCpuIds[0]) == cpuLittleMap.end()) {
            MNN_ERROR("CPU ID %d is not valid. CpuIds will not be used.\n", mCpuIds[0]);
            valid = false;
            break;
        }

        auto cpuLittle = cpuLittleMap[mCpuIds[0]];
        for (size_t i = 1; i < mCpuIds.size(); i++) {
            if (cpuLittleMap.find(mCpuIds[i]) == cpuLittleMap.end()) {
                MNN_ERROR("CPU ID %d is not valid. CpuIds will not be used.\n", mCpuIds[i]);
                valid = false;
                break;
            }
            // Using the same group of CPU cores helps maximize multi thread performance.
            // Mixing little cores with others can lead to significant performance degradation, so it is strictly prohibited.
            // Even on architectures with more than two clusters, when little cores are not involved,
            // it's still strongly recommended to avoid cross-cluster usage between different big core groups.
            if (cpuLittleMap[mCpuIds[i]] != cpuLittle) {
                MNN_ERROR("CPU ID %d and %d are not from the same group. CpuIds will not be used.\n", mCpuIds[0], mCpuIds[i]);
                valid = false;
                break;
            }
        }
    } else {
        for (int i = 0; i < outputCount; ++i) {
            float accum = 0.f;
            auto ocOutside = i / HP;
            auto ocInside = i % HP;
            for (int j = 0; j < blockNum; ++j) {
                int index = i * blockNum + j;
                int srcSumIndex = ocOutside * blockNum * HP + j * HP + ocInside; // ikernelsum: [hU,blocknum,hP]
                alphaPtr[j * ocUp4 + i] = quanInfoPtr[index];
                biasPtr[j * ocUp4 + i] = (float)originOffset * quanInfoPtr[index];
                if (realInt4OrInt8) {
                    accum += (ikernelSum[srcSumIndex] * quanInfoPtr[index] + blockSize * biasPtr[j * ocUp4 + i]);
                } else {
                    accum += ((ikernelSum[srcSumIndex]  - blockSize * 8) * quanInfoPtr[index]);
                }
                if (blockQuantInput) {
                    int dstSumIndex = ocOutside * blockNum * HP + j * HP + ocInside;
                    weightKernelSum[dstSumIndex] = accum;
                    accum = 0;
                }
            }
            if (!blockQuantInput) {
                weightKernelSum[i] = accum;
            }
        }
    }
}

static inline void calculateSmeNeonWorkDivision(int& ocMain, int& ocBranch, std::vector<int>& divides, int oc, int threads, int pack, int planeSize, int divisionRatio, int smeCores) {
    // workload
    auto ocDivPack = UP_DIV(oc, pack);
    auto workUnit = UP_DIV(ocDivPack, divisionRatio * smeCores + 1 * (threads - smeCores));
    int calOcMain = ALIMIN(ROUND_UP(workUnit * pack * smeCores * divisionRatio, GEMM_INT8_UNIT_SME2_128), oc);
    if (calOcMain <= ocMain) { // The purpose of this function is to increase the value of ocMain.
        return;
    }
    ocMain = calOcMain;
    ocBranch = oc - ocMain;
    divides.assign(threads + 1, ocDivPack);
    divides[0] = 0;

   // runtime UNIT for different core and different process(prefill or decode)
   auto rtUnit4Sme = planeSize == 1? GEMM_INT8_UNIT_SME2_128 : GEMM_INT8_UNIT_SME2;
    // mOcMain
    auto ocPerSmeCore = ALIMIN(UP_DIV(UP_DIV(ROUND_UP(ocMain, pack), rtUnit4Sme), smeCores) * (rtUnit4Sme / pack), UP_DIV(ocMain, pack));
    for (int i = 0; i < smeCores; ++i) {
        divides[i + 1] = ALIMIN(divides[i] + ocPerSmeCore, UP_DIV(ocMain, pack));
    }

    // ocRemain
    if (ocBranch > 0) {
        auto ocPerNeonCore = UP_DIV(UP_DIV(ROUND_UP(ocBranch, pack), GEMM_INT8_UNIT_ARM82), threads - smeCores) * (GEMM_INT8_UNIT_ARM82 / pack);
        for (int i = smeCores + 1; i < threads + 1; ++i) {
            divides[i] = ALIMIN(divides[i - 1] + ocPerNeonCore, ocDivPack);
        }
    }
}

static inline void _getProportions(int totalProp, int& intensiveProp, int& lightProp) {
    // compute the proportions of different kernels
    lightProp = totalProp % 8;
    intensiveProp = totalProp / 8 % 8;
    if (lightProp == 0 && intensiveProp == 0) {
        // pass
        // Don't use mixed kernels
    } else if (lightProp == 0) {
        lightProp = 1;
    } else if (intensiveProp == 0) {
        intensiveProp = 6;
    } else if (lightProp > intensiveProp) {
        lightProp = 1;
    }
}

static inline void _computeDivides4Sme(std::vector<int>& divides, int threads, int smeCoreNums, int size) {
    divides.resize(threads + 1);
    divides[0] = 0;
    auto length = UP_DIV(size, smeCoreNums);
    auto cur = length;
    for (int i = 1; i < smeCoreNums + 1; ++i) {
        divides[i] = cur;
        cur = ALIMIN(cur + length, size);
    }
}

static inline void _updateMixedKernelFlag(bool &mixedKernel, bool &onlineReorderWeightSme, int threads, int eP, bool isDynamciQuant, bool postiveBothProp) {
    mixedKernel = false;
    if (threads >= 4 && eP == GEMM_INT8_DST_XUNIT_SME2 && isDynamciQuant && postiveBothProp) {
        mixedKernel = true;
        onlineReorderWeightSme = true;
    }
}

DenseConvInt8TiledExecutor::DenseConvInt8TiledExecutor(Backend* backend, const Op* op, std::shared_ptr<ConvolutionCommon::Int8Common> quanCommon, bool isDynamicQuant) : ConvInt8TiledExecutor(backend, op) {
    // convolution info
    auto convOp = op->main_as_Convolution2D();
    int kernelCount = mCommon->kernelX() * mCommon->kernelY();
    int oc = convOp->common()->outputCount();
    int ic = convOp->common()->inputCount();
    bool asyWeight = quanCommon ? quanCommon->asymmetric : false;

    mOcBranch = 0;
    mOcMain = oc;

    int blockNum = 1;
    int inputBlockNum = 1;
    if (quanCommon) {
        int dequantCnt = quanCommon->alphaSize;
        if (quanCommon->asymmetric) {
            dequantCnt /= 2;
        }
        blockNum = dequantCnt / oc;
    }
    mBlockNum = blockNum;

    // backend info
    auto core = static_cast<CPUBackend*>(backend)->int8Functions();
    auto gcore = static_cast<CPUBackend*>(backend)->functions();
    const int threads = static_cast<CPUBackend*>(backend)->threadNumber();
    const int pack = gcore->pack;

    // runtime hint
    auto option = static_cast<CPUBackend*>(backend)->getRuntime()->hint().dynamicQuantOption;
    auto weightOnlineReorderOption = WEIGHT_ONLINE_REORDER & option;
    auto inputBlockQuantOption = option % WEIGHT_ONLINE_REORDER;
    if (inputBlockQuantOption == 2) {
        inputBlockNum = blockNum;
    }

    _getProportions(static_cast<CPUBackend*>(backend)->getRuntime()->hint().divisionRatio, mRatioPrefill, mRatioDecode);
    mSmeCores = gcore->smeCoreNumber;

    mRelatedFunctions = *(static_cast<CPUBackend*>(backend)->int8GemmFunctions());
    mArm82Functions = gcore->arm82MatmulRelatedFunctions;

    int UNITMain, SRC_UNITMain, DST_XUNITMain;
    int UNITBranch = 0; int SRC_UNITBranch = 0, DST_XUNITBranch = 0;
    mRelatedFunctions.MNNGetGemmUnit(&UNITMain, &SRC_UNITMain, &DST_XUNITMain);

    if (mArm82Functions.MNNGetGemmUnit != nullptr) { // exclude cpu does not support arm82
        mArm82Functions.MNNGetGemmUnit(&UNITBranch, &SRC_UNITBranch, &DST_XUNITBranch);
    }
    if (hint().midMemoryPath.size() > 0) {
        if (mDynamicMmap.empty()) {
            // Only support set featuremap dir once
            mDynamicMmap.resize(2);
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().midMemoryPath.c_str(), "", "dynamic");
            for (auto& buf : mDynamicMmap) {
                buf.root = mmapMem;
            }
        }
    }
    if (hint().weightMemoryPath.size() > 0) {
        // forward_type, precision_type, memory_type, power_type
        std::string prefix = "0_0_0_0_";
        prefix[2] += mPrecision;
        prefix[4] += mMemory;
        prefix[6] += mPower;
        // prefix += hint().modelUUID + "_";
        bool autoRemove = true;
        if (hint().useCachedMmap) {
            autoRemove = false;
            std::string fileName = MNNFilePathConcat(hint().weightMemoryPath, prefix + "sync.static");
            const_cast<RuntimeHint&>(hint()).useCachedMmap += MNNFileExist(fileName.c_str());
        }
        if (nullptr == mStaticAllocatorMMap.get()) {
            // Only support set weightmap dir once
            mStaticAllocatorRaw = mStaticAllocator;
            auto mmapMem = BufferAllocator::Allocator::createMmap(hint().weightMemoryPath.c_str(), prefix.c_str(), "static", autoRemove);
            size_t mmapSize = static_cast<size_t>(hint().mmapFileSize) * 1024 * 1024;
            mStaticAllocator.reset(new EagerBufferAllocator(mmapMem, 32, mmapSize));
            mStaticAllocatorMMap = mStaticAllocator;
        }
    }
    auto precision = mPrecision;
    auto memory = mMemory;
    size_t flags = mFlags;
    if (nullptr != origin) {
        auto cpuBn = static_cast<CPUBackend*>(origin);
        mSharedDmaInfo = cpuBn->mDmaInfo;
    }
    if (nullptr != config) {
        precision = config->precision;
        flags = config->flags;
        memory = config->memory;
    }
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend was created by runtime:%p\n", this);
#endif
    CPUBackend* res = nullptr;
    auto initThreadNumber = hint().initThreadNumber;
    do {
#ifdef MNN_USE_ARMV82
        auto core = MNNGetCoreFunctions();
        if (core->supportFp16arith && precision == BackendConfig::Precision_Low) {
            res = new Arm82Backend(this, memory);
            res->mRelatedFunctions = &(res->functions()->int8MatmulRelatedFunctions);
            break;
        }
#endif
#ifdef MNN_SUPPORT_BF16
        if (precision == BackendConfig::Precision_Low_BF16 && BF16Functions::get()) {
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU_EXTENSION);
            res->mCoreFunctions = BF16Functions::get();
            break;
        }
#endif
        if (flags == MNN_CPU_USE_DEFAULT_BACKEND) {
            // Default don't use multi-thread init
            res = new CPUBackend(this, precision, memory, MNN_FORWARD_CPU);
            break;
        }
#ifdef MNN_USE_SSE
            if (target->mUseConvQuan) {
                for (int ks = 0; ks < oc; ++ks) {
                    target->mOriginBias->host<int32_t>()[ks] -= 128 * ((float*)kernelsum.get())[ks];
                }
            }
#endif
        }
        /* 2. compute and order dequant scale&bias */
        bool notConvertInt4ToInt8 = true;
        if (target->mWeightBits == 4 && !fastReadWeight) {
            notConvertInt4ToInt8 = false;
        }
        int32_t paramsKernelSum[2] = {blockNum, inputBlockNum * ROUND_UP(oc, UNIT)};
        float* weightKernelSum = (float*)addressPtr[2];
        float* quanScalePtr = (float*)addressPtr[3];
        _computeReorderQuantInfo(weightKernelSum, paramsKernelSum, (inputBlockQuantOption == 2), target->mWeightBits == 4, asyWeight, quanScalePtr, oc, kernelCount * ic, pack, reorderedQuantInfo, (float*)kernelsum.get(), UNIT, notConvertInt4ToInt8);
        /* 3. put weight and quantInfo together */
        int32_t params[6] = {shape[0], shape[1], shape[2], shape[3], shape[4], ROUND_UP(oc, pack)};
        int8_t* weightInt8 = addressPtr[1];

        ConvInt8TiledExecutor::packWeightAndQuantInfo(weightInt8, (int8_t*)weightReordered.get(), reorderedQuantInfo.get(), params, QUANT_INFO_BYTES);

        return 0;
    };

    auto function = [=]() -> int {
        bool fastReadWeight = (kernelCount == 1 && ROUND_UP(ocMain, UNITMain) == ocMain && ROUND_UP(ic, SRC_UNITMain) == ic);
        weightSummerFuncion sumFunc = funcsMain.MNNSumWeightInt8;
        if (mOnlineReorderWeightSme) {
            sumFunc = funcsMain.MNNSumWeightInt8SmeHp128;
        }

        int8_t* addressPtr[4];
        addressPtr[0] = quanCommon? quanCommon->weight.get() : (int8_t*)convOp->symmetricQuan()->weight()->data();
        addressPtr[1] = target->mWeightInt8->host<int8_t>();
        addressPtr[2] = target->mWeightKernelSum->host<int8_t>();
        addressPtr[3] = quanCommon? (int8_t*) quanCommon->alpha.get() : (int8_t*)convOp->symmetricQuan()->scale()->data();

        reorderFunc(funcsMain, shapeMain, UNITMain, SRC_UNITMain, DST_XUNITMain, weightlenMain, scaleSizeMain, ocMain, 0, fastReadWeight, addressPtr, sumFunc);

        if (ocBranch > 0) {
            // update the address of weight source, weight destination, weight kernel sum and weight scale
            addressPtr[0] += (target->mWeightBits == 4 ? ocMain * ic * kernelCount / 2 : ocMain * ic * kernelCount); // ocMain%2==0, so divides 2 directly
            addressPtr[1] += (weightlenMain + quantlenMain);
            addressPtr[2] += ROUND_UP(ocMain, UNITMain) * inputBlockNum * QUANT_INFO_BYTES;
            addressPtr[3] += (quanCommon->asymmetric ? 2 * ocMain * blockNum * QUANT_INFO_BYTES : ocMain * blockNum * QUANT_INFO_BYTES);
            sumFunc = funcsBranch.MNNSumWeightInt8;

            fastReadWeight = (kernelCount == 1 && ROUND_UP(ocBranch, UNITMain) == ocBranch && ROUND_UP(ic, SRC_UNITMain) == ic);
            reorderFunc(funcsBranch, shapeBranch, UNITBranch, SRC_UNITBranch, DST_XUNITBranch, weightlenBranch, scaleSizeBranch, ocBranch, 1, fastReadWeight, addressPtr, sumFunc);
        }
    }

    return 0;
}

void CPURuntime::onGabageCollect(int level) {
    mStaticAllocator->release(false);
    if (nullptr != mStaticAllocatorMMap) {
        mStaticAllocatorMMap->release(false);
    }
    if (level >= 100) {
        for (auto& buf : mDynamic) {
            buf.release();
        }
    }
}


void CPURuntime::onConcurrencyBegin() const {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex < 0 && nullptr != mThreadPool) {
        mTaskIndex = mThreadPool->acquireWorkIndex();
    }
    if (mTaskIndex >= 0) {
        // mThreadOpen 0 -> 1, active ThreadPool
        // For next onConcurrencyBegin, will only add mThreadOpen
        if (0 == mThreadOpen) {
            mThreadPool->active();
        }
        mThreadOpen++;
    }
#else
#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(mThreadNumber);
#endif
#endif
    _bindCPUCore();
}

void CPURuntime::onConcurrencyEnd() const {
#ifdef MNN_USE_THREAD_POOL
    if (mTaskIndex >= 0) {
        mThreadOpen--;
        mThreadOpen = mThreadOpen < 0 ? 0 : mThreadOpen;
        if (0 == mThreadOpen) {
            mThreadPool->releaseWorkIndex(mTaskIndex);
            mThreadPool->deactive();
            mTaskIndex = -1;
        }
    }
#endif
}

std::map<OpType, CPUBackend::Creator*>* CPUBackend::gCreator = nullptr;
void CPUBackend::initCreatorMap() {
    gCreator = new std::map<OpType, CPUBackend::Creator*>;
}

bool CPUBackend::addCreator(OpType t, Creator* c) {
    auto map = gCreator;
    if (map->find(t) != map->end()) {
        MNN_PRINT("Error: %d type has be added\n", t);
        return false;
    }
    map->insert(std::make_pair(t, c));
    return true;
}
BufferAllocator* CPURuntime::createDynamicBufferAlloctor(int index) const {
    if (hint().memoryAllocatorType == Runtime::Allocator_Defer) {
        return new DeferBufferAllocator(buffer(index));
    }
    if (nullptr != mStaticAllocatorRaw.get()) {
        return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocatorRaw.get()));
    }
    return new EagerBufferAllocator(BufferAllocator::Allocator::createRecurse(mStaticAllocator.get()));
}
CPUBackend::CPUBackend(const CPURuntime* runtime, BackendConfig::PrecisionMode precision, BackendConfig::MemoryMode memory, MNNForwardType type, size_t flags) : Backend(type) {
#ifdef LOG_VERBOSE
    MNN_PRINT("cpu backend create\n");
#endif
    mMemory = memory;
    mRuntime = const_cast<CPURuntime*>(runtime);
    auto core = MNNGetCoreFunctions();
    mThreadNumber = mRuntime->mThreadNumber;
    mRelatedFunctions = &core->int8MatmulRelatedFunctions;

    // Compute Group Rate
    do {
        if (mThreadNumber <= 1 || mRuntime->mPower == BackendConfig::Power_Low) {
            break;
        }
        auto rate = mRuntime->hint().cpuDecreaseRate;
        if (rate >= 100 || rate <= 0) {
            break;
        }
        auto cpuInfo = MNNGetCPUInfo();
        if (cpuInfo->groups.size() < 2) {
            break;
        }
    }

    if (!mSplitByOc) {
        mThreadNums = ALIMIN(threads, mTileCount);
        if (threads >= 4&&DST_XUNIT==GEMM_INT8_DST_XUNIT_SME2&&mResourceInt8->mDynamicQuant&&!mMixedKernel) {
            _computeDivides4Sme(mDivides, threads, mSmeCores, mTileCount);
        } else {
            mComputeI = 7.f;
        }
        mGroupWithComputeRate.clear();
        float decreaseRate = (float)(rate) / 100.0f;
        int validCpuSize = (int)(cpuInfo->groups[cpuInfo->groups.size()-1].ids.size());
        int groupIndex = (int)cpuInfo->groups.size()-2;
        validCpuSize = ALIMIN(validCpuSize, mThreadNumber);
        float totalComputeRate = 1.0f * validCpuSize;
        mGroupWithComputeRate.emplace_back(std::make_pair(totalComputeRate, validCpuSize));
        float currentRate = 1.0f;
        while (validCpuSize < mThreadNumber && groupIndex >= 0) {
            auto& group = cpuInfo->groups[groupIndex];
            int selectSize = ALIMIN(mThreadNumber - validCpuSize, (int)group.ids.size());
            validCpuSize += group.ids.size();
            currentRate *= decreaseRate;
            totalComputeRate += currentRate * selectSize;
            mGroupWithComputeRate.emplace_back(std::make_pair(currentRate * selectSize, selectSize));
            groupIndex--;
        }
        for (auto& g : mGroupWithComputeRate) {
            g.first = g.first / totalComputeRate;
        }
    } while (false);
    auto dynamicAlloc = mRuntime->mSharedDmaInfo;
    if (nullptr == dynamicAlloc.get()) {
        mDmaInfo.reset(new CPURuntime::DynamicAllocator);
        mDmaInfo->mDynamicAllocator.reset(mRuntime->createDynamicBufferAlloctor(0));
        mDmaInfo->mCurrentDynamicAllocator = mDmaInfo->mDynamicAllocator.get();
        mDmaInfo->mCacheGroup.resize(MNN_CPU_MAX_BUFFER_INDEX);
        for (int i=0; i<mDmaInfo->mCacheGroup.size(); ++i) {
            mDmaInfo->mCacheGroup[i].reset(new CPUResizeCache);
        }
    } else {
        mTempIm2ColBuffer.reset(Tensor::createDevice<int8_t>({mTileCount, colBufferSize * im2colBytes}));
        mTempSrcSum = bufferAlloc->alloc(mTileCount * mBlockNum * DST_XUNIT * mIm2ColCount * QUANT_INFO_BYTES);
    }

    mAccumBuffer.reset(Tensor::createDevice<int32_t>({threads, DST_XUNIT * ALIMAX(UNIT, gcore->pack)}));

    auto success = backend()->onAcquireBuffer(mTempIm2ColBuffer.get(), Backend::DYNAMIC);
    success &= backend()->onAcquireBuffer(mAccumBuffer.get(), Backend::DYNAMIC);
    if (!success || mBlitInfo.invalid() || mTempSrcSum.invalid()) {
        return OUT_OF_MEMORY;
    }
    if (false == mResourceInt8->mDynamicQuant && false == m4BitPtq) {
        bufferAlloc->free(mBlitInfo);
        bufferAlloc->free(mTempSrcSum);
        backend()->onReleaseBuffer(mTempIm2ColBuffer.get(), Backend::DYNAMIC);
        if (mBatchQuantInfo.get()) {
            backend()->onReleaseBuffer(mBatchQuantInfo.get(), Backend::DYNAMIC);
        }
        backend()->onReleaseBuffer(mAccumBuffer.get(), Backend::DYNAMIC);
        return NO_ERROR;
    }

#ifdef MNN_LOW_MEMORY
    if (!mMixedKernel) { // Dynamic Quant kernels, use single gemm kernel.
        mGemmKernel = mRelatedFunctions.Int8GemmKernel;
        if (mOnlineReorderWeightSme && planeSize == 1) {
            mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP32_DecodeMax;
        }
        if (mResourceInt8->mWeightBits == 4) {
            mGemmKernel = mRelatedFunctions.Int8GemmKernel_W4;
            if (mOnlineReorderWeightSme && planeSize == 1) {
                mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP32_DecodeMax;
            }
        }
        mQuantFunc = core->MNNFloat2Int8;
        if (gcore->bytes == 2 && gcore->pack == 8) {
            mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP16;
            if (mOnlineReorderWeightSme && planeSize == 1) {
                mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP16_DecodeMax;
            }
            if (mResourceInt8->mWeightBits == 4) {
                mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP16;
                if (mOnlineReorderWeightSme && planeSize == 1) {
                    mGemmKernel = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP16_DecodeMax;
                }
            }
            mQuantFunc = core->DynamicQuanInput_ARM82;
            mQuantAndReorderFunc = core->DynamicQuanInputAndReorder_ARM82;

        }
        // A axisSum kernel
    } else { // use sme and neon gemmInt8
        // Fp32
        if (planeSize == 1) { // Decode
            mGemmKernels.push_back(mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP32_DecodeMax);
            mGemmKernels.push_back(mArm82Functions.Int8GemmKernel);
            if (mResourceInt8->mWeightBits == 4) {
                mGemmKernels[0] = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP32_DecodeMax;
                mGemmKernels[1] = mArm82Functions.Int8GemmKernel_W4;
            }
        } else { // Prefill
            mGemmKernels.push_back(mRelatedFunctions.Int8GemmKernel);
            mGemmKernels.push_back(mArm82Functions.Int8GemmKernel);
            if (mResourceInt8->mWeightBits == 4) {
                mGemmKernels[0] = mRelatedFunctions.Int8GemmKernel_W4;
                mGemmKernels[1] = mArm82Functions.Int8GemmKernel_W4;
            }
        }
        mQuantFunc = core->MNNFloat2Int8;

        // fp16
        if (gcore->bytes == 2 && gcore->pack == 8) {
            if (planeSize == 1) { // Decode
                mGemmKernels[0] = mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP16_DecodeMax;
                mGemmKernels[1] = mArm82Functions.MNNGemmInt8AddBiasScale_Unit_FP16;
                if (mResourceInt8->mWeightBits == 4) {
                    mGemmKernels[0] = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP16_DecodeMax;
                    mGemmKernels[1] = mArm82Functions.MNNGemmInt8AddBiasScale_w4_Unit_FP16;
                }
            } else { // Prefill
                mGemmKernels[0] = mRelatedFunctions.MNNGemmInt8AddBiasScale_Unit_FP16;
                mGemmKernels[1] = mArm82Functions.MNNGemmInt8AddBiasScale_Unit_FP16;
                if (mResourceInt8->mWeightBits == 4) {
                    mGemmKernels[0] = mRelatedFunctions.MNNGemmInt8AddBiasScale_w4_Unit_FP16;
                    mGemmKernels[1] = mArm82Functions.MNNGemmInt8AddBiasScale_w4_Unit_FP16;
                }
            }
            mQuantFunc = core->DynamicQuanInput_ARM82;
            mQuantAndReorderFunc = core->DynamicQuanInputAndReorder_ARM82;
        }
        // A axisSum kernel
    }

    mInputBlockNum = (inputBlockQuantOption == 2) ? mBlockNum : 1;
    bool symmetricQuant = (inputBlockQuantOption != 2 && mUseBatchQuan) ? true : false;

    int size = 0;
    if (!mUseBatchQuan) { // single quant
        if (mSplitByOc) {
            size = 2 * mInputBlockNum * ALIMIN(DST_XUNIT, planeSize) * QUANT_INFO_BYTES;
        } else {
            size = 2 * mInputBlockNum * mIm2ColCount * DST_XUNIT * QUANT_INFO_BYTES;
        }
    }
    if (mUseBatchQuan) {
        if (mIm2ColBasedInt8) {
            size = 2 * mInputBlockNum * inputPlane * QUANT_INFO_BYTES;
        } else if (!mSplitByOc){ // only threads buffer needed by this case
            size = 2 * mInputBlockNum * mIm2ColCount * DST_XUNIT * QUANT_INFO_BYTES;
        } else {
            size = 2 * mInputBlockNum * planeSize * QUANT_INFO_BYTES;
        }
    }
    if (symmetricQuant) { // symmetric quant
        size /= 2;
    }

    if (false == m4BitPtq) {
        if (!mIm2ColBasedInt8 && !mSplitByOc) {
            mBatchQuantInfo.reset(Tensor::createDevice<int8_t>({threads, size}));
        } else {
            mBatchQuantInfo.reset(Tensor::createDevice<int8_t>({1, size})); // keep dimensions=2!
        }
        success &= backend()->onAcquireBuffer(mBatchQuantInfo.get(), Backend::DYNAMIC);
    }

    // Dynamic quant.
    // set im2col tensor info
    if (mIm2ColBasedInt8) {
        mQuantInput.reset((Tensor::createDevice<int8_t>({batch, mIm2ColParamter.ih, mIm2ColParamter.iw, ROUND_UP(inC, gcore->pack)})));
    } else if (!mSplitByOc){
        mQuantInput.reset((Tensor::createDevice<int8_t>({threads, colBufferSize * 1})));
    } else {
        mQuantInput.reset((Tensor::createDevice<int8_t>({mTileCount, colBufferSize * 1})));
    }
    success &= backend()->onAcquireBuffer(mQuantInput.get(), Backend::DYNAMIC);

    // set compute buffer
    int tempSize = threads * 2 * mInputBlockNum * inputPlane;
    if (!mIm2ColBasedInt8) {
        if (!mSplitByOc) {
            tempSize = threads * 2 * mInputBlockNum * DST_XUNIT * mIm2ColCount;
        } else {
            tempSize = threads * 2 * mInputBlockNum * ROUND_UP(planeSize, DST_XUNIT);
        }
    }
    if (symmetricQuant) { // symmetric batch quant.
        tempSize /= 2;
    }
    mSizeInputBlockQuant = tempSize / threads;
    mTempMaxMinValueBuffer = bufferAlloc->alloc(tempSize * gcore->bytes);
    mQScaleZero = bufferAlloc->alloc(tempSize * QUANT_INFO_BYTES);

    if (mQScaleZero.invalid()) {
        return OUT_OF_MEMORY;
    }
    if (mOnlineReorderWeightSme && planeSize > 1) { // only prefill need
        int ocProcessedBySme = mOcMain;
        int ocProcessedByNeon = 0;
        if (mMixedKernel && mRatioDecode != mRatioPrefill) {
            auto workUnit = UP_DIV(outC4, mRatioPrefill * mSmeCores + 1 * (threads - mSmeCores));
            ocProcessedBySme = ALIMIN(ROUND_UP(workUnit * pack * mSmeCores * mRatioPrefill, GEMM_INT8_UNIT_SME2_128), outC);
            ocProcessedBySme = ALIMAX(ocProcessedBySme, mOcMain);
            ocProcessedByNeon = outC - ocProcessedBySme;
        }
        int weightlenSme = ROUND_UP(ocProcessedBySme, GEMM_INT8_UNIT_SME2_128) * mBlockNum * ROUND_UP(ic / mBlockNum, SRC_UNIT) * kernelCount;
        int weightlenNeon = ROUND_UP(ocProcessedByNeon, 8) * mBlockNum * ROUND_UP(ic / mBlockNum, SRC_UNIT) * kernelCount;
        if (mResourceInt8->mWeightBits == 4) {
            weightlenSme /= 2;
            weightlenNeon /= 2;
        }
        int scalebiasLenSme = 2 * mBlockNum * ROUND_UP(ocProcessedBySme, GEMM_INT8_UNIT_SME2_128) * QUANT_INFO_BYTES;
        int scalebiasLenNeon = 2 * mBlockNum * ROUND_UP(ocProcessedByNeon, 8) * QUANT_INFO_BYTES;


        mWeight4Prefill = bufferAlloc->alloc(weightlenSme + scalebiasLenSme + weightlenNeon + scalebiasLenNeon);
        if (mWeight4Prefill.invalid()) {
            return OUT_OF_MEMORY;
        }
        if (mInputBlockNum > 1) { // only in this case, need to use weight_kernel_sum
            mWeightKernelSum4Prefill = bufferAlloc->alloc(ROUND_UP(outC, GEMM_INT8_UNIT_SME2_128) * mBlockNum * sizeof(float));
            if (mWeightKernelSum4Prefill.invalid()) {
                return OUT_OF_MEMORY;
            }
        }
    }
    mToFuseInputbias2Bias = (!mUseBatchQuan && inputBlockQuantOption != 2) ? true : false;
    if (mToFuseInputbias2Bias) { // input data has only one bias&scale
        if (mIm2ColBasedInt8) {
            mBiasBufferFusedInputzero = bufferAlloc->alloc(ROUND_UP(outC, UNIT) * QUANT_INFO_BYTES); // should be UP_DIV(oc, UNIT),not UP_DIV(oc, pack)
        } else {
            mBiasBufferFusedInputzero = bufferAlloc->alloc(threads * ROUND_UP(outC, UNIT) * QUANT_INFO_BYTES);
        }
        if (mBiasBufferFusedInputzero.invalid()) {
            return OUT_OF_MEMORY;
        }
    }

void CPUBackend::enqueueTask(std::function<int()>&& task) {
    if (mInitWorkQueue != nullptr) {
        mInitWorkQueue->postTask(std::move(task));
    } else {
        task();
    }
}

Backend::MemObj* CPUBackend::onAcquire(const MNN::Tensor* nativeTensorConst, StorageType storageType) {
    if (nativeTensorConst == nullptr) {
        return nullptr;
    }
    //FUNC_PRINT_ALL(nativeTensorConst, p);
    auto nativeTensor = (Tensor*)nativeTensorConst;
    auto size = getTensorSize(nativeTensor, true);
    return allocBuffer(size, nativeTensor, storageType);
}

static OpType _getRealOpType(OpType opType) {
    switch (opType) {
        case OpType_Convolution:
            return OpType_ConvInt8;
        case OpType_ConvolutionDepthwise:
            return OpType_DepthwiseConvInt8;
        default:
            return opType;
    }
}
void* CPUBackend::onMapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* srcTensor) {
    if (static_cast<int>(getBytes(this, srcTensor)) != srcTensor->getType().bytes()) {
        return nullptr;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat) != dtype) {
        return nullptr;
    }
    _resetDynamicMemory();
    if (mRuntime->pCurrentStatus != NO_ERROR) {
        // Out of memory
        return nullptr;
    }
    return srcTensor->host<void>();
}

bool CPUBackend::onUnmapTensor(Tensor::MapType mtype, Tensor::DimensionType dtype, const Tensor* dstTensor, void* mapPtr) {
    if (static_cast<int>(getBytes(this, dstTensor)) != dstTensor->getType().bytes()) {
        return false;
    }
    if (OpCommonUtils:: convertDimType(TensorUtils::getDescribe(dstTensor)->dimensionFormat) != dtype) {
        return false;
    }
    return true;
}

size_t CPUBackend::getTensorSize(const Tensor* tensor, bool multiBytes) const {
    auto core = mCoreFunctions;
    size_t dataSize = 1;
    auto des = TensorUtils::getDescribe(tensor);
    for (int i = 0; i < tensor->dimensions(); i++) {
        size_t currentDimSize = tensor->length(i);
        if (des->dimensionFormat == MNN_DATA_FORMAT_NC4HW4 && 1 == i) {
            currentDimSize = UP_DIV(currentDimSize, core->pack) * core->pack;
        }
        dataSize *= currentDimSize;
    }
    if (multiBytes) {
        size_t bytes = getBytes(this, tensor);
        return dataSize * bytes;
    }
    return dataSize;
}

size_t CPUBackend::getBytes(const Backend* backend, const Tensor* output) {
    size_t bytes = output->getType().bytes();
    auto core = static_cast<const CPUBackend*>(backend)->functions();
    auto quant = TensorUtils::getDescribe(output)->quantAttr.get();
    if (output->getType().code == halide_type_float) {
        bytes = core->bytes;
    }
    if (nullptr != quant && TensorUtils::getDescribe(output)->applyQuant) {
        bytes = 1;
    }
    return bytes;
}

DataType CPUBackend::getDataType(const Tensor* tensor) {
    auto des = TensorUtils::getDescribe(tensor);
    if (nullptr == des->quantAttr.get() || (!des->applyQuant)) {
        return DataType_DT_FLOAT;
    }
    return des->quantAttr->type;
}

/// get execution
Execution* CPUBackend::onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op) {
    /**
     BatchNorm it will be converted to scale
     for model convert, don't print error log
     */
    if (op->type() == OpType_BatchNorm) {
        return nullptr;
    }
    auto opType = op->type();
    if (outputs.size() > 0 && inputs.size() > 0) {
        bool outputQuant = TensorUtils::getDescribe(outputs[0])->quantAttr != nullptr && TensorUtils::getDescribe(outputs[0])->quantAttr->type == DataType_DT_INT8;
        bool inputQuant = TensorUtils::getDescribe(inputs[0])->quantAttr != nullptr && TensorUtils::getDescribe(inputs[0])->quantAttr->type == DataType_DT_INT8;
        if (inputQuant && outputQuant) {
            opType = _getRealOpType(opType);
        }
    }

    // TODO: rm this convert when merge diff datatyoe of op
    auto map  = gCreator;
    auto iter = map->find(opType);
    if (iter == map->end() ) {
        MNN_PRINT("Don't support type [%s]\n", MNN::EnumNameOpType(op->type()));
        return nullptr;
    }
    Execution* exe = nullptr;
    bool needCast = false;
    if (exe == nullptr) {
        exe = iter->second->onCreate(inputs, outputs, op, this);
    }
    return exe;
}
const Runtime* CPUBackend::getRuntime() {
    return mRuntime;
}

bool CPUBackend::onClearBuffer() {
    if (nullptr != mRuntime->mStaticAllocatorRaw.get()) {
        mRuntime->mStaticAllocator->sync();
        mRuntime->mStaticAllocator = mRuntime->mStaticAllocatorRaw;
        mRuntime->mStaticAllocatorRaw = nullptr;
    }
    mCache->reset();
    mDmaInfo->mCurrentDynamicAllocator->release(true);
    return true;
}

std::pair<int, int> CPUBackend::multiThreadDivide(int size) const {
    int sizeDivide = size / threadNumber();
    sizeDivide = UP_DIV(sizeDivide, mCoreFunctions->pack) * mCoreFunctions->pack;
    int scheduleNumber = 1;
    if (sizeDivide > 0) {
        scheduleNumber = UP_DIV(size, sizeDivide);
    }
    return std::make_pair(sizeDivide, scheduleNumber);
}
void CPUBackend::onCopyBuffer(const Tensor* srcTensor, const Tensor* dstTensor) const {
    _resetDynamicMemory();
    if (mRuntime->pCurrentStatus != NO_ERROR) {
        // Out of memory
        return;
    }
    auto& srcBuffer = srcTensor->buffer();
    auto& dstBuffer = dstTensor->buffer();
    if (srcBuffer.dimensions != dstBuffer.dimensions ) {
        if (srcBuffer.dim[srcBuffer.dimensions - 1].extent != 1 && dstBuffer.dim[dstBuffer.dimensions - 1].extent != 1) {
            MNN_ERROR("srcBuffer dimension not equal to dstBuffer, can't copy buffer\n");
        }
    }
    if (srcTensor->getDimensionType() == dstTensor->getDimensionType()) {
        for (int i = 0; i < srcBuffer.dimensions; ++i) {
            MNN_ASSERT(srcBuffer.dim[i].extent <= dstBuffer.dim[i].extent);
        }
    }
    if (nullptr == srcBuffer.host || nullptr == dstBuffer.host) {
        return;
    }
    std::unique_ptr<Tensor> wrapTensor;
    if (getDataType(srcTensor) != getDataType(dstTensor)) {
        auto dimType =  OpCommonUtils::convertDimType(TensorUtils::getDescribe(srcTensor)->dimensionFormat);
        auto convertType = CPUCastCreator::FlOAT_TO_INT8;
        if (getDataType(srcTensor) == DataType_DT_INT8) {
            convertType = CPUCastCreator::INT8_TO_FlOAT;
        }
        wrapTensor.reset(Tensor::createDevice(srcTensor->shape(), dstTensor->getType(), dimType));
        auto dstType = getDataType(dstTensor);
        if (dstType != DataType_DT_FLOAT) {
            wrapTensor->setType(dstType);
        }
        wrapTensor->buffer().host = (uint8_t*)MNNMemoryAllocAlign(getTensorSize(wrapTensor.get()) * wrapTensor->getType().bytes(), MNN_MEMORY_ALIGN_DEFAULT);

#ifdef LOG_VERBOSE
        MNN_PRINT("CPU backend copy tensor ptr:%p -> ptr:%p hostPtr:%p -> %p, format %d -> %d, dims: [",
        srcTensor, dstTensor, srcTensor->host<void>(), dstTensor->host<void>(), TensorUtils::getDescribe(srcTensor)->dimensionFormat, TensorUtils::getDescribe(dstTensor)->dimensionFormat);
        for (int i=0; i<srcTensor->dimensions(); ++i) {
            MNN_PRINT("%d ", srcTensor->length(i));
        }
        MNN_PRINT("]\n");
#endif

        TensorUtils::getDescribe(wrapTensor.get())->memoryType = Tensor::InsideDescribe::MEMORY_HOST;
        auto code = CPUCastCreator::cast(srcTensor, wrapTensor.get(), this, convertType);
        if (NO_ERROR != code) {
            MNN_ERROR("Error in CPUBackend::onCopyBuffer:cast\n");
        }
    }

    if (ocPreserve) {
        memcpy(dst + huDst * strideDst, src + 4 * huDst * strideSrc, ROUND_UP(ocPreserve, hpSrc) * blockNum * sizeof(float));
    }
}

ErrorCode DenseConvInt8TiledExecutor::onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    const auto input = inputs[0];
    auto output      = outputs[0];
    auto core = static_cast<CPUBackend*>(backend())->int8Functions();
    auto gcore = static_cast<CPUBackend*>(backend())->functions();
    auto dynamicOption = static_cast<CPUBackend*>(backend())->getRuntime()->hint().dynamicQuantOption % 8;

    int UNIT = mGemmUnits[0];
    int SRC_UNIT = mGemmUnits[1];
    int DST_XUNIT = mGemmUnits[2];
    auto blitProc = mRelatedFunctions.MNNPackC4Int8ForMatMul_A;
    const int plane                  = output->batch() * mIm2ColParamter.oh * mIm2ColParamter.ow;
    const int batch                  = input->batch();
    const int PackUnit               = gcore->pack;
    const int dstZStep               = plane * PackUnit;
    const int ocDiv4                 = UP_DIV(output->channel(), PackUnit);
    const int ocUp4                  = ROUND_UP(output->channel(), PackUnit);
    const int ocUpHp                 = ROUND_UP(output->channel(), UNIT);
    const auto kernelCountUnit       = mIm2ColParamter.kernelCountUnit;
    const auto unitColBufferSize     = kernelCountUnit * DST_XUNIT * SRC_UNIT * sizeof(int8_t);
    const auto colBufferSize         = unitColBufferSize * mIm2ColCount;
    auto dstBytes                    = static_cast<CPUBackend*>(backend())->getBytes(backend(), output);
    const int blockL                 = kernelCountUnit / mBlockNum; // source depthQuad for each block.
    const int kxky                   = mIm2ColParamter.kernelX * mIm2ColParamter.kernelY;
    const int blocklu                = blockL / kxky;                     // UP_DIV(ic,src_unit) per block
    const int oc                     = output->channel();
    const int ic                     = input->channel();
    float weightBytes                = 1.f;
    int weightStepY                  = weightBytes * (UNIT * SRC_UNIT);
    int inputPlane                   = batch * input->width() * input->height();

    auto im2colPtr           = mTempIm2ColBuffer->host<int8_t>();
    if (SRC_UNIT > PackUnit) {
        memset(im2colPtr, 0, mTempIm2ColBuffer->size());
    }
    auto weightDataPtr = mResourceInt8->mWeightInt8->host<int8_t>();
    auto srcKernelSumPtr     = (int8_t*)mTempSrcSum.ptr();
    auto im2colSrc = input->host<uint8_t>();
    auto outputDataPtr = output->host<int8_t>();
    uint8_t* biasPtr = nullptr;
    int32_t inputZeroPoint = 0;
    int im2colBytes = mIm2ColBasedInt8 == true ? 1 : gcore->bytes;

    // Additional Adjustments for 4Bit Ptq model
    if (m4BitPtq) {
        outputDataPtr = (int8_t*)mTempOutput.ptr();
        dstBytes = gcore->bytes;
    }

    if (nullptr != mMutableResource.get()) {
        biasPtr       = mMutableResource->mBiasFloat->host<uint8_t>();
        inputZeroPoint  = mMutableResource->mInputZeroPoint;
        if (mBatchQuantInfo.get()) {
            float scalein = TensorUtils::getQuantInfo(inputs[0])[0];
            float scaleou = TensorUtils::getQuantInfo(outputs[0])[0];
            if (true == m4BitPtq) {
                scaleou = 1;
            }
            auto scaleX = scalein / scaleou;
            for (int i = 0; i < DST_XUNIT; ++i) {
                mBatchQuantInfo->host<float>()[i] = scaleX;
            }
        }
        switch (otype) {
            case OpType_Convolution:
            case OpType_ConvolutionDepthwise:
                if (inputs.size() > 1) {
                    return false;
                }
                if (op->main_as_Convolution2D() && op->main_as_Convolution2D()->weight() != nullptr) {
                    return false;
                } else {
                    return true;
                }
            case OpType_ConvInt8:
            case OpType_DepthwiseConvInt8:
                return true;
                // case OpType_Eltwise:
            case OpType_Raster:
            {
                for (auto input : inputs) {
                    if (TensorUtils::getDescribe(input)->quantAttr->scale != TensorUtils::getDescribe(outputs[0])->quantAttr->scale || TensorUtils::getDescribe(input)->quantAttr->zero != TensorUtils::getDescribe(outputs[0])->quantAttr->zero) {
                        return false;
                    }
                    if (TensorUtils::getDescribe(input)->quantAttr.get() && TensorUtils::getDescribe(outputs[0])->quantAttr.get() && (TensorUtils::getDescribe(input)->quantAttr.get()->scale == 0 || TensorUtils::getDescribe(outputs[0])->quantAttr.get()->scale == 0)) {
                        return false;
                    }
                }
                return true;
            }
            case OpType_Pooling:
                if (TensorUtils::getDescribe(inputs[0])->quantAttr->scale != TensorUtils::getDescribe(outputs[0])->quantAttr->scale || TensorUtils::getDescribe(inputs[0])->quantAttr->zero != TensorUtils::getDescribe(outputs[0])->quantAttr->zero) {
                    return false;
                }
                if (op->main_as_Pool() && op->main_as_Pool()->type() == PoolType_MAXPOOL ) {
                    return true;
                } else if (op->main_as_Pool() && op->main_as_Pool()->type() == PoolType_AVEPOOL) {
                    return true;
                } else {
                    return false;
                }
            case OpType_Softmax:
                return true;
            case OpType_LayerNorm:
                return true;
#ifdef MNN_SUPPORT_QUANT_EXTEND
            case OpType_ReLU:
                if (TensorUtils::getDescribe(inputs[0])->quantAttr.get() != TensorUtils::getDescribe(outputs[0])->quantAttr.get()) {
                    return false;
                }
            }
        }

        if (dropBranch == 0) { // If dropBranch == 0, it means that the arrangement of the weights processed by the Arm82 architecture remains unchanged.
            // copy
            memcpy(mWeightKernelSum4Prefill.ptr() + kernelSumMainSize, mResourceInt8->mWeightKernelSum->host<uint8_t>() + kernelSumMainSize, kernelSumBranchSize);
        }

        weightDataPtr = (int8_t*)mWeight4Prefill.ptr();
    }
#endif
    if (mResourceInt8->mWeightBits == 4) {
        weightBytes   = 0.5;
        weightStepY /= 2;
    }
    int blockunit = ocUp4 * 2 * QUANT_INFO_BYTES + blockL * weightStepY * UP_DIV(output->channel(), UNIT);
    auto inputchannel = input->channel();
    SumByAxisParams sumParams;
    sumParams.oneScale = (mUseBatchQuan || dynamicOption == 2) ? 0 : 1;
    sumParams.SRC_UNIT = SRC_UNIT;
    sumParams.blockNum = mBlockNum;
    sumParams.DST_XUNIT = DST_XUNIT;
    sumParams.unitColBufferSize = unitColBufferSize;
    sumParams.kernelCountUnitDouble = kernelCountUnit;
    sumParams.valid = inputchannel % SRC_UNIT;
    sumParams.kernelxy = kxky;
    sumParams.LU = UP_DIV(inputchannel, SRC_UNIT);
    sumParams.inputBlock = (mInputBlockNum > 1) ? 1 : 0;
    std::vector<float> fakeInputScales(DST_XUNIT, 1.f);

    auto tileSplitFunction = [&](int tId, int eStartIndex, int eEndIndex, int estep) {
        auto ocDivThread = ocDiv4;
        float* reluPtr = mResourceInt8->mReluThreshold.data();
        float* accumbuff = nullptr;
        uint8_t* inputScale = nullptr;
        uint8_t* inputBias = nullptr;
        uint8_t* ptrInputScale = nullptr;
        uint8_t* ptrInputBias = nullptr;
        if (mBatchQuantInfo.get()) {
            if (mIm2ColBasedInt8) {
                inputScale = mBatchQuantInfo->host<uint8_t>();
                ptrInputScale = inputScale;
            }

            if (dynamicOption == 2 && mUseBatchQuan && mIm2ColBasedInt8) {
                inputBias = inputScale + mBatchQuantInfo->stride(0) / 2;
                ptrInputBias = inputBias;
            }
        } else {
            inputScale = (uint8_t*)fakeInputScales.data();
            ptrInputScale = inputScale;
        }
        if (mBlockNum > 1) {
            accumbuff = reinterpret_cast<float*>(mAccumBuffer->host<int8_t>() + tId * mAccumBuffer->stride(0) * sizeof(int32_t));
        }
        float* ptrY = nullptr;
        if (dstBytes != 1) {
            ptrY = (mOnlineReorderWeightSme && mInputBlockNum > 1) ? (float*)mWeightKernelSum4Prefill.ptr() : mResourceInt8->mWeightKernelSum->host<float>();
        }
        QuanPostTreatParameters quanParam;
        quanParam.blockNum = mBlockNum;
        int32_t indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        quanParam.indices = indices;
        if (dstBytes != 1) {
            quanParam.useInt8 = 0;
            quanParam.fp32minmax = reluPtr;
#ifdef MNN_USE_SSE
            if (!mBatchQuantInfo.get()) {
                quanParam.weightKernelSum = nullptr;
            }
#endif
        } else {
            quanParam.maxValue = mMutableResource->mClampMax;
            if (mResourceInt8->mRelu) {
                quanParam.minValue = mMutableResource->mOutputZeroPoint;
            } else {
                quanParam.minValue = mMutableResource->mClampMin;
            }
        }
        auto weightPtrTid = weightDataPtr;
        quanParam.weightKernelSum = ptrY;
        quanParam.biasFloat = reinterpret_cast<float*>(biasPtr);
        auto im2colDstThread        = im2colPtr + tId * mTempIm2ColBuffer->stride(0);
        auto srcPtr     = (int8_t const **)(mBlitInfo.ptr() + tId * mBlitInfoStride.first);
        auto el         = (int32_t *)(srcPtr + mBlitInfoStride.second);
        auto xKernelSumPtrTid = reinterpret_cast<float*>(srcKernelSumPtr + tId * mBlockNum * DST_XUNIT * mIm2ColCount * QUANT_INFO_BYTES);

        int32_t info[5];
        info[1] = mIm2ColParamter.iw * mIm2ColParamter.ih * batch;
        info[2] = static_cast<int32_t>(unitColBufferSize);
        info[3] = mIm2ColParamter.strideX;
        for (int tIndex = eStartIndex; tIndex < eEndIndex; tIndex += estep) {
            const int xIndexStart  = tIndex * DST_XUNIT * mIm2ColCount;
            auto outputInTilePtr = outputDataPtr + xIndexStart * PackUnit * dstBytes;
            int realDstCount = ALIMIN(plane - xIndexStart, DST_XUNIT * mIm2ColCount);
            ptrInputScale = (mUseBatchQuan && mIm2ColBasedInt8) ? (inputScale + xIndexStart * mInputBlockNum * QUANT_INFO_BYTES) : inputScale;
            ptrInputBias = (inputBias != nullptr) ? (inputBias + xIndexStart * mInputBlockNum * QUANT_INFO_BYTES) : inputBias;
            // im2col
            auto im2colDst = im2colDstThread;
            auto res = ConvolutionTiledExecutor::turnIm2ColToBlitInfo((const float**)srcPtr, el, xIndexStart, realDstCount, mIm2ColParamter, (uint8_t*)im2colSrc, im2colBytes);
            int number = res.first;
            bool needZero = res.second;
            if (needZero && mIm2ColBasedInt8) {
#ifdef MNN_USE_SSE
                ::memset(im2colDst, inputZeroPoint + 128, colBufferSize);
#else
                ::memset(im2colDst, inputZeroPoint, colBufferSize);
#endif
            }
            info[0] = number;
            info[4] = realDstCount;
            if (mIm2ColBasedInt8 && number > 0) {
                blitProc(im2colDst, srcPtr, info, el);
            }
#ifdef MNN_LOW_MEMORY
            if (!mIm2ColBasedInt8) {
                if (needZero) {
                    ::memset(im2colDst, 0, mTempIm2ColBuffer->stride(0));
                }
                if (number > 0) {
                    if (SRC_UNIT > PackUnit && !needZero) {
                        memset(im2colDst, 0, mTempIm2ColBuffer->stride(0));
                    }
                    info[2] = realDstCount;
                    mRelatedFunctions.MNNGeneralIm2Col((float*)im2colDst, (float const**)srcPtr, info, el, SRC_UNIT, PackUnit); // im2colDst: [lu, realDstCount, lp]
                }
                ptrInputScale = mBatchQuantInfo->host<uint8_t>() + tId * mBatchQuantInfo->stride(0);
                if (dynamicOption == 2) {
                    ptrInputBias = ptrInputScale + mBatchQuantInfo->stride(0) / 2;
                    BatchAsyDynamicQuant((uint8_t*)im2colDst, inputZeroPoint, ptrInputScale, kernelCountUnit, realDstCount, SRC_UNIT, 1, mQuantInput->host<int8_t>() + tId * mQuantInput->stride(0), ptrInputBias, tId);
                } else if (mUseBatchQuan) {
                    BatchSymDynamicQuant((uint8_t*)im2colDst, inputZeroPoint, ptrInputScale, kernelCountUnit, realDstCount, SRC_UNIT, 1, mQuantInput->host<int8_t>() + tId * mQuantInput->stride(0), tId);
                } else {
                    auto maxMinPtr = mTempMaxMinValueBuffer.ptr() + tId * 2 * gcore->bytes;
                    ptrInputBias = ptrInputScale + mBatchQuantInfo->stride(0) / 2;
                    BatchAsyDynamicQuant((uint8_t*)im2colDst, inputZeroPoint, ptrInputScale, kernelCountUnit, realDstCount, SRC_UNIT, 1, mQuantInput->host<int8_t>() + tId * mQuantInput->stride(0), ptrInputBias, tId);
                    quanParam.biasFloat = (float*)(mBiasBufferFusedInputzero.ptr() + tId * ocUpHp * QUANT_INFO_BYTES);
                }
                im2colDst = mQuantInput->host<int8_t>() + tId * mQuantInput->stride(0);
            }
            if (mBlockNum > 1 && kxky > 1) {
                auto eU = UP_DIV(realDstCount, DST_XUNIT); // eU <= mIm2ColCount
                auto reorderBuffer = mReorderBuffer.ptr() + tId * colBufferSize;
                for (int k = 0; k < eU; ++k) {
                    int inside  = blocklu * SRC_UNIT * ALIMIN(realDstCount - k * DST_XUNIT, DST_XUNIT);
                    auto dstbuffer = reorderBuffer + k * unitColBufferSize;
                    auto srcbuffer = im2colDst + k * unitColBufferSize;
                    for (int i = 0; i < mBlockNum; ++i) {
                        for (int j = 0; j < kxky; ++j) {
                            memcpy(dstbuffer + i * kxky * inside + j * inside, srcbuffer + i * inside + j * mBlockNum * inside, inside);
                        }
                    }
                }
                im2colDst = (int8_t*)reorderBuffer;
            }
#endif
            if (mResourceInt8->mWeightAsymmetricQuant) {
                MNN_ASSERT(mBatchQuantInfo.get() && mBatchQuantInfo->host<float>());
                mRelatedFunctions.MNNSumByAxisLForMatmul_A(xKernelSumPtrTid, im2colDst, (float*)ptrInputScale, realDstCount, sumParams);
            } else {
                memset(xKernelSumPtrTid, 0, mBlockNum * DST_XUNIT * mIm2ColCount * QUANT_INFO_BYTES);
            }
            auto ptrX = xKernelSumPtrTid;
            do {
                int step = ALIMIN(DST_XUNIT, realDstCount);
                quanParam.inputScale = (float*)ptrInputScale;
                quanParam.inputBias = (float*)ptrInputBias;
                if (mBlockNum > 1) {
                    memset(accumbuff, 0, UNIT * 4 * DST_XUNIT);
                    quanParam.accumBuffer = accumbuff;
                }
                quanParam.srcKernelSum = ptrX;
                mGemmKernel(outputInTilePtr, im2colDst, weightPtrTid, blockL, dstZStep * dstBytes, ocDivThread, &quanParam, step);
                ptrX += (step * mBlockNum);
                realDstCount-=step;
                outputInTilePtr += DST_XUNIT * PackUnit * dstBytes;
                im2colDst += unitColBufferSize;
                ptrInputScale = mUseBatchQuan ? (ptrInputScale + step * mInputBlockNum * QUANT_INFO_BYTES) : ptrInputScale;
                ptrInputBias = (ptrInputBias != nullptr) ? (ptrInputBias + step * mInputBlockNum * QUANT_INFO_BYTES) : ptrInputBias;
            } while(realDstCount > 0);
        }
    };
    auto ocSplitFunction = [&](int threads) { // Thread split by OC
        auto im2colDst           = mTempIm2ColBuffer->host<int8_t>();
        auto srcPtr     = (int8_t const **)(mBlitInfo.ptr());
        auto el         = (int32_t *)(srcPtr + mBlitInfoStride.second);
        auto xKernelSumPtr = reinterpret_cast<float*>(mTempSrcSum.ptr());

        auto eU = UP_DIV(plane, DST_XUNIT);
        int32_t info[5];
        info[1] = mIm2ColParamter.iw * mIm2ColParamter.ih * batch;
        info[2] = static_cast<int32_t>(unitColBufferSize);
        info[3] = mIm2ColParamter.strideX;

        float* reluPtr = mResourceInt8->mReluThreshold.data();
        if (mIm2ColBasedInt8) { // im2col
            auto res = ConvolutionTiledExecutor::turnIm2ColToBlitInfo((const float**)srcPtr, el, 0, plane, mIm2ColParamter, (uint8_t*)im2colSrc, im2colBytes);
            int number = res.first;
            bool needZero = res.second;
            if (needZero) {
#ifdef MNN_USE_SSE
                ::memset(im2colDst, inputZeroPoint + 128, mTempIm2ColBuffer->size());
#else
                ::memset(im2colDst, inputZeroPoint, mTempIm2ColBuffer->size());
#endif
            }
            info[0] = number;
            info[4] = plane;
            if (number > 0) {
                blitProc(im2colDst, srcPtr, info, el);
            }
        }
#ifdef MNN_LOW_MEMORY
        if (false == mIm2ColBasedInt8) {
            int realDstCount = plane;
            int start = 0;
            auto ptrInputscale = mBatchQuantInfo->host<uint8_t>();
            auto ptrInputbias = ptrInputscale + mBatchQuantInfo->stride(0) / 2;
            auto int8Ptr = mQuantInput->host<int8_t>();
            int sizePacked = 0;
            auto im2colDstTmp = im2colDst;
            while (realDstCount > 0) {
                int work = std::min(realDstCount, DST_XUNIT);
                sizePacked += (work * SRC_UNIT * kernelCountUnit);
                auto res = ConvolutionTiledExecutor::turnIm2ColToBlitInfo((const float**)srcPtr, el, start, work, mIm2ColParamter, (uint8_t*)im2colSrc, im2colBytes);
                int number = res.first;
                bool needZero = res.second;
                if (needZero) {
                    ::memset(im2colDstTmp, 0, unitColBufferSize * gcore->bytes);
                }
                info[0] = number;
                info[2] = work;
                if (number > 0) { // im2col
                    mRelatedFunctions.MNNGeneralIm2Col((float*)im2colDstTmp, (float const**)srcPtr, info, el, SRC_UNIT, PackUnit); // im2colDst: [lu, realDstCount, lp]
                }
                if (mUseBatchQuan || dynamicOption == 2) {
                    if (dynamicOption == 2) {
                        BatchAsyDynamicQuant((uint8_t*)im2colDstTmp, inputZeroPoint, ptrInputscale, kernelCountUnit, work, SRC_UNIT, 1, int8Ptr, ptrInputbias, 0);
                        ptrInputbias += (mInputBlockNum * work * sizeof(int32_t));
                    } else {
                        BatchSymDynamicQuant((uint8_t*)im2colDstTmp, inputZeroPoint, ptrInputscale, kernelCountUnit, work, SRC_UNIT, 1, int8Ptr, 0);
                    }
                    ptrInputscale += (mInputBlockNum * work * sizeof(int32_t));
                    int8Ptr += unitColBufferSize;
                }
                realDstCount -= work;
                start += work;
                im2colDstTmp += (unitColBufferSize * gcore->bytes);
            }
            if (!mUseBatchQuan && dynamicOption != 2) {
                BatchAsyDynamicQuant((uint8_t*)im2colDst, inputZeroPoint, ptrInputscale, kernelCountUnit, plane, SRC_UNIT, 1, mQuantInput->host<int8_t>(), ptrInputscale + plane * mInputBlockNum* QUANT_INFO_BYTES, 0);
            }
            im2colDst = mQuantInput->host<int8_t>();
        }
        if (mBlockNum > 1 && kxky > 1) {
            for (int k = 0; k < eU; ++k) {
                int inside  = blocklu * SRC_UNIT * ALIMIN(DST_XUNIT, plane - k * DST_XUNIT);
                auto dstbuffer = mReorderBuffer.ptr() + k * unitColBufferSize;
                auto srcbuffer = im2colDst + k * unitColBufferSize;
                for (int i = 0; i < mBlockNum; ++i) {
                    for (int j = 0; j < kxky; ++j) {
                        memcpy(dstbuffer + i * kxky * inside + j * inside, srcbuffer + i * inside + j * mBlockNum * inside, inside);
                    }
                }
            }
            im2colDst = (int8_t*)mReorderBuffer.ptr();
        }
#endif
        if (mResourceInt8->mWeightAsymmetricQuant) {
            MNN_ASSERT(mBatchQuantInfo.get() && mBatchQuantInfo->host<float>());
            mRelatedFunctions.MNNSumByAxisLForMatmul_A(xKernelSumPtr, im2colDst, mBatchQuantInfo->host<float>(), plane, sumParams);
        } else {
            memset(xKernelSumPtr, 0, mTileCount * mBlockNum * DST_XUNIT * mIm2ColCount * QUANT_INFO_BYTES);
        }

        MNN_CONCURRENCY_BEGIN(tId, threads) {
            int ocIndex = PackUnit * mDivides[tId];
            auto ocDivThread = ALIMIN(mDivides[tId + 1] - mDivides[tId], ocDiv4 - mDivides[tId]);

            if (ocIndex < ocUp4 && ocDivThread > 0) {
                decltype(mGemmKernel) gemmInt8;
                if (mMixedKernel) {
                    gemmInt8 = tId < mSmeCores ? mGemmKernels[0] : mGemmKernels[1];
                } else {
                    gemmInt8 = mGemmKernel;
                }
                auto im2colDstThread = im2colDst;
                float* ptrY = nullptr;
                if (dstBytes != 1) {
                    float* wkernelSum = (mOnlineReorderWeightSme && mInputBlockNum > 1 && plane > 1) ? (float*)mWeightKernelSum4Prefill.ptr() : mResourceInt8->mWeightKernelSum->host<float>();
                    ptrY = wkernelSum + ocIndex * mInputBlockNum;
                }
                QuanPostTreatParameters quanParam;
                quanParam.blockNum = mBlockNum;
                quanParam.weightKernelSum = ptrY;
                quanParam.biasFloat = reinterpret_cast<float*>(biasPtr + ocIndex * 4);
                int32_t indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
                quanParam.indices = indices;
                if (dstBytes != 1) {
                    quanParam.useInt8 = 0;
                    quanParam.fp32minmax = reluPtr;
#ifdef MNN_USE_SSE
                    if (!mBatchQuantInfo.get()) {
                        quanParam.weightKernelSum = nullptr;
                    }
#endif
                } else {
                    quanParam.maxValue = mMutableResource->mClampMax;
                    if (mResourceInt8->mRelu) {
                        quanParam.minValue = mMutableResource->mOutputZeroPoint;
                    } else {
                        quanParam.minValue = mMutableResource->mClampMin;
                    }
                }
                uint8_t* inputScale = nullptr; // input scale for batch dynamic quant.
                uint8_t* inputBias = nullptr;
                float* accumbuff = nullptr;
                if (mBatchQuantInfo.get()) {
                    inputScale = mBatchQuantInfo->host<uint8_t>();
                    if (dynamicOption == 2) {
                        inputBias = inputScale + mInputBlockNum * plane * QUANT_INFO_BYTES;
                    }
                } else {
                    inputScale = (uint8_t*)fakeInputScales.data();
                }
                if (mBlockNum > 1) {
                    accumbuff = reinterpret_cast<float*>(mAccumBuffer->host<int8_t>() + tId * mAccumBuffer->stride(0) * sizeof(int32_t));
                }

                auto outputInTilePtr = outputDataPtr + ocIndex * plane * dstBytes;

                auto weightSrc = weightDataPtr;
                if (tId >= mSmeCores && dropBranch == 0 && mMixedKernel) {
                    weightSrc = mResourceInt8->mWeightInt8->host<int8_t>();
                }

                auto weightPtrTid = weightSrc + static_cast<int32_t>(ocIndex * mBlockNum * blockL * SRC_UNIT * weightBytes + ocIndex * 2 * mBlockNum * QUANT_INFO_BYTES);

                int realDstCount = plane;
                auto ptrX = xKernelSumPtr;
                do {
                    int step = ALIMIN(DST_XUNIT, realDstCount);
                    quanParam.inputScale = (float*)inputScale;
                    quanParam.inputBias = (float*)inputBias;
                    quanParam.srcKernelSum = ptrX;
                    if (mBlockNum > 1) {
                        memset(accumbuff, 0, UNIT * 4 * DST_XUNIT);
                        quanParam.accumBuffer = accumbuff;
                    }
                    gemmInt8(outputInTilePtr, im2colDstThread, weightPtrTid, blockL, dstZStep * dstBytes, ocDivThread, &quanParam, step);
                    ptrX += (step * mBlockNum);
                    realDstCount-=step;
                    outputInTilePtr += DST_XUNIT * PackUnit * dstBytes;
                    im2colDstThread += unitColBufferSize;
                    inputScale = mUseBatchQuan ? (inputScale + mInputBlockNum * step * QUANT_INFO_BYTES) : inputScale;
                    inputBias = (inputBias != nullptr) ? (inputBias + mInputBlockNum * step * QUANT_INFO_BYTES) : inputBias;
                } while(realDstCount > 0);
            }
        }
        MNN_CONCURRENCY_END();

    };
    if (!mSplitByOc) {
        MNN_CONCURRENCY_BEGIN(tId, threads) {
            if (mDivides[tId + 1] - mDivides[tId] > 0) {
                tileSplitFunction((int)tId, mDivides[tId], mDivides[tId + 1], 1);
            }
        }
        MNN_CONCURRENCY_END();
    } else {
        ocSplitFunction(threads);
    }
    if (m4BitPtq) {
        std::vector<float> outputQuantScale(PackUnit);
        float s = TensorUtils::getQuantInfo(outputs[0])[0] == 0 ? 0 : 1.f / TensorUtils::getQuantInfo(outputs[0])[0];
        for (int i = 0; i < PackUnit; ++i) {
            outputQuantScale[i] = s;
        }
        float zero_ = TensorUtils::getQuantInfo(outputs[0])[1];
        mQuantFunc((float*)mTempOutput.ptr(), output->host<int8_t>(), plane * ocDiv4, outputQuantScale.data(), mResourceInt8->mClampMin, mResourceInt8->mClampMax, &zero_, 0);
    }
    return NO_ERROR;
}
} // namespace MNN
