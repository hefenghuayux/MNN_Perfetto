set -e
ABI="arm64-v8a"
OPENMP="OFF"
VULKAN="ON"
OPENCL="ON"
OPENGL="OFF"
RUN_LOOP=10
FORWARD_TYPE=0
CLEAN=""
PUSH_MODEL=""

# 【新增变量】默认 Mask 为 0 (不绑核)
CPU_MASK="0"
# 【新增变量】默认线程数 4 (为了补齐参数位置，必须指定一个显式值)
THREAD=4

WORK_DIR=`pwd`
BUILD_DIR=build
BENCHMARK_MODEL_DIR=$WORK_DIR/../models
ANDROID_DIR=/data/local/tmp

function usage() {
    echo "-32\tBuild 32bit."
    echo "-c\tClean up build folders."
    echo "-p\tPush models to device"
    # 【新增帮助信息】
    echo "-m\tSet CPU Affinity Mask (Hex string, e.g. f0)"
    echo "-t\tSet CPU Thread number (Default 4)"
}
function die() {
    echo $1
    exit 1
}

function clean_build() {
    echo $1 | grep "$BUILD_DIR\b" > /dev/null
    if [[ "$?" != "0" ]]; then
        die "Warnning: $1 seems not to be a BUILD folder."
    fi
    rm -rf $1
    mkdir $1
}

function build_android_bench() {
    if [ "-c" == "$CLEAN" ]; then
        clean_build $BUILD_DIR
    fi
    mkdir -p build
    cd $BUILD_DIR
    cmake ../../../ \
          -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
          -DCMAKE_BUILD_TYPE=Release \
          -DANDROID_ABI="${ABI}" \
          -DANDROID_STL=c++_static \
          -DANDROID_NATIVE_API_LEVEL=android-21  \
          -DMNN_USE_LOGCAT:BOOL=OFF \
          -DMNN_VULKAN:BOOL=$VULKAN \
          -DMNN_OPENCL:BOOL=$OPENCL \
          -DMNN_OPENMP:BOOL=$OPENMP \
          -DMNN_OPENGL:BOOL=$OPENGL \
          -DMNN_ARM82:BOOL=ON \
          -DMNN_BUILD_BENCHMARK:BOOL=ON \
          -DMNN_BUILD_FOR_ANDROID_COMMAND=true \
          -DNATIVE_LIBRARY_OUTPUT=.
    make -j8 benchmark.out timeProfile.out
}

function bench_android() {
    build_android_bench
    find . -name "*.so" | while read solib; do
        adb push $solib  $ANDROID_DIR
    done
    adb push benchmark.out $ANDROID_DIR
    adb push timeProfile.out $ANDROID_DIR
    adb shell  "su -c 'chmod 0777 $ANDROID_DIR/benchmark.out'"

    if [ "" != "$PUSH_MODEL" ]; then
        adb shell "rm -rf $ANDROID_DIR/benchmark_models"
        adb push $BENCHMARK_MODEL_DIR $ANDROID_DIR/benchmark_models
    fi
    adb shell "cat /proc/cpuinfo > $ANDROID_DIR/benchmark.txt"
    adb shell "echo >> $ANDROID_DIR/benchmark.txt"
    adb shell "echo Build Flags: ABI=$ABI  OpenMP=$OPENMP Vulkan=$VULKAN OpenCL=$OPENCL >> $ANDROID_DIR/benchmark.txt"
# ---------------------------------------------------------
    # 【核心修改】使用 su -c 强制唤醒大核并锁定频率
    # ---------------------------------------------------------
    echo "[-INFO-] Waking up CPUs with Root permission..."
    
    # 遍历核心 4, 5, 6, 7 (对应你的 Mask f0)
    for i in 4 5 6 7; do
        # 1. 强制上线 (Online)
        # 注意单引号和双引号的嵌套：外层双引号由本地 shell 解析，内层单引号传给 Android 的 su
        adb shell "su -c 'echo 1 > /sys/devices/system/cpu/cpu$i/online'"
        
        # 2. 设置为性能模式 (Performance)
        adb shell "su -c 'echo performance > /sys/devices/system/cpu/cpu$i/cpufreq/scaling_governor'"
    done
    
    # 等待 2 秒让 CPU 状态稳定
    sleep 2
    # ---------------------------------------------------------
    # 【关键修改】CPU Benchmark
    # 原脚本只传了前4个参数，现在我们需要填满到第12个参数(Mask)
    # 参数顺序: [Models] [Loop] [Warmup] [Forward] [Thread] [Precision] [Sparsity] [Block] [Quant] [Kleidi] [Mask]
    # 我们填入默认值: Precision=2(Normal), Sparsity=0.0, Block=1, Quant=0, Kleidi=0
    echo "Running CPU Benchmark with Mask: $CPU_MASK, Thread: $THREAD"
    adb shell "su -c 'LD_LIBRARY_PATH=$ANDROID_DIR  $ANDROID_DIR/benchmark.out $ANDROID_DIR/benchmark_models $RUN_LOOP 5 $FORWARD_TYPE $THREAD 2 0.0 1 0 0 $CPU_MASK' 2>$ANDROID_DIR/benchmark.err >> $ANDROID_DIR/benchmark.txt"
    
    # #benchmark  Vulkan (通常不绑核，保持原样或按需修改)
    # adb shell "LD_LIBRARY_PATH=$ANDROID_DIR $ANDROID_DIR/benchmark.out $ANDROID_DIR/benchmark_models $RUN_LOOP 5 7 2>$ANDROID_DIR/benchmark.err >> $ANDROID_DIR/benchmark.txt"
    # #benchmark OpenCL
    # adb shell "LD_LIBRARY_PATH=$ANDROID_DIR $ANDROID_DIR/benchmark.out $ANDROID_DIR/benchmark_models 100 20 3 2>$ANDROID_DIR/benchmark.err >> $ANDROID_DIR/benchmark.txt"
}
while [ "$1" != "" ]; do
    case $1 in
        -32)
            shift
            ABI="armeabi-v7a"
            ;;
        -c)
            shift
            CLEAN="-c"
            ;;
        -p)
            shift
            PUSH_MODEL="-p"
            ;;
        -m)
            shift
            CPU_MASK=$1
            shift  # 【新增】必须加这一行，吃掉具体的 Mask 值 (如 c0)
            ;;
        -t)
            shift
            THREAD=$1
            shift  # 【新增】必须加这一行，吃掉具体的线程数值
            ;;
        *)
            usage
            exit 1
    esac
done

bench_android
adb pull $ANDROID_DIR/benchmark.txt .
cat benchmark.txt