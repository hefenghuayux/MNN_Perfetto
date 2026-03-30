#!/usr/bin/env bash
# wsl -d Ubuntu -- bash -lc 'cd ~/MNN_WSL2 && CLEAN_BUILD=1 bash ./build_android_llm_bench_wsl.sh'

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_WORK_DIR="${BUILD_WORK_DIR:-$PROJECT_ROOT/project/android/build_64_wsl}"
MODEL_SOURCE_DIR="${MODEL_SOURCE_DIR:-$PROJECT_ROOT/model_dir}"
BUILD_STAMP="${BUILD_STAMP:-$(date +"%Y%m%d_%H%M%S")}"
PACKAGE_PREFIX="${PACKAGE_PREFIX:-mnn_aecs_run}"
PACKAGE_NAME="${PACKAGE_NAME:-${PACKAGE_PREFIX}_${BUILD_STAMP}}"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/$PACKAGE_NAME}"
PACKAGE_METADATA_FILE="${PACKAGE_METADATA_FILE:-$BUILD_WORK_DIR/last_android_package.env}"
REMOTE_PACKAGE_DIR="${REMOTE_PACKAGE_DIR:-/data/local/tmp/$PACKAGE_NAME}"
MODEL_REMOTE_PATH="${MODEL_REMOTE_PATH:-$REMOTE_PACKAGE_DIR/model_dir/config.json}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-29}"
ANDROID_NATIVE_API_LEVEL="${ANDROID_NATIVE_API_LEVEL:-android-21}"
JOBS="${JOBS:-$(nproc)}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
MNN_BUILD_TEST="${MNN_BUILD_TEST:-0}"
MNN_OPENCL="${MNN_OPENCL:-0}"
RUN_PERFETTO_AFTER_BUILD="${RUN_PERFETTO_AFTER_BUILD:-1}"
ADB_PUSH_AFTER_BUILD="${ADB_PUSH_AFTER_BUILD:-1}"
NDK_SHIM_ROOT="${NDK_SHIM_ROOT:-$HOME/.cache/codex-ndk-shims}"
NDK_CANDIDATES=(
    "${ANDROID_NDK:-}"
    "$HOME/Android/ndk/android-ndk-r23c"
    "/home/hefeng/Downloads/environment/Android_NDK/android-ndk-r25c"
    "/mnt/c/Microsoft/AndroidNDK/android-ndk-r23c"
    "/mnt/c/Users/14609/AppData/Local/Android/Sdk/ndk/25.2.9519653"
)

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        exit 1
    fi
}

cmake_bool() {
    case "${1,,}" in
        1|on|true|yes) echo "ON" ;;
        0|off|false|no|"") echo "OFF" ;;
        *)
            echo "Invalid boolean value: $1" >&2
            exit 1
            ;;
    esac
}

detect_existing_generator() {
    if [[ -f "$BUILD_WORK_DIR/build.ninja" ]]; then
        echo "Ninja"
    elif [[ -f "$BUILD_WORK_DIR/Makefile" ]]; then
        echo "Unix Makefiles"
    fi
}

select_build_generator() {
    if [[ -n "${BUILD_GENERATOR:-}" ]]; then
        echo "$BUILD_GENERATOR"
        return 0
    fi

    if command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1; then
        echo "Ninja"
        return 0
    fi

    echo "Unix Makefiles"
}

require_cmd cmake
require_cmd c++
require_cmd sed

find_ndk() {
    local candidate
    for candidate in "${NDK_CANDIDATES[@]}"; do
        if [[ -n "$candidate" && -f "$candidate/build/cmake/android.toolchain.cmake" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

prepare_ndk_for_wsl() {
    local ndk_root="$1"
    local shim_root
    local item

    if [[ -f "$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
        echo "$ndk_root"
        return 0
    fi

    if [[ ! -f "$ndk_root/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe" ]]; then
        echo "$ndk_root"
        return 0
    fi

    shim_root="$NDK_SHIM_ROOT/$(basename "$ndk_root")"
    if [[ "${CLEAN_NDK_SHIM:-0}" == "1" ]]; then
        rm -rf "$shim_root"
    fi

    if [[ ! -f "$shim_root/build/cmake/android-legacy.toolchain.cmake" ]]; then
        rm -rf "$shim_root"
        mkdir -p "$shim_root"
        cp "$ndk_root/source.properties" "$shim_root/"
        cp -a "$ndk_root/build" "$shim_root/"
        for item in meta prebuilt python shader-tools simpleperf sources toolchains platforms ndk-build ndk-gdb ndk-lldb ndk-which NOTICE NOTICE.txt README.md README.txt; do
            if [[ -e "$ndk_root/$item" ]]; then
                ln -s "$ndk_root/$item" "$shim_root/$item"
            fi
        done
        sed -i 's/set(ANDROID_HOST_TAG linux-x86_64)/set(ANDROID_HOST_TAG windows-x86_64)\n  set(ANDROID_TOOLCHAIN_SUFFIX .exe)/' "$shim_root/build/cmake/android-legacy.toolchain.cmake"
    fi

    echo "$shim_root"
}

copy_outputs() {
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR/model_dir"
    if [[ -d "$MODEL_SOURCE_DIR" ]]; then
        find "$MODEL_SOURCE_DIR" -maxdepth 1 -type f -exec cp -t "$OUTPUT_DIR/model_dir" {} +
        echo ">>> Model files copied to package directory."
    else
        echo ">>> [Warning] MODEL_SOURCE_DIR not found, skipping model files."
    fi
    cp "$BUILD_WORK_DIR/libMNN.so" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/libMNN_Express.so" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/libllm.so" "$OUTPUT_DIR/"
    if [[ -f "$BUILD_WORK_DIR/libMNN_CL.so" ]]; then cp "$BUILD_WORK_DIR/libMNN_CL.so" "$OUTPUT_DIR/"; fi
    cp "$BUILD_WORK_DIR/llm_demo" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/llm_bench" "$OUTPUT_DIR/"
}

write_package_metadata() {
    mkdir -p "$(dirname "$PACKAGE_METADATA_FILE")"
    cat > "$PACKAGE_METADATA_FILE" <<EOF
PACKAGE_NAME=$PACKAGE_NAME
OUTPUT_DIR=$OUTPUT_DIR
REMOTE_DIR=$REMOTE_PACKAGE_DIR
MODEL_REMOTE=$MODEL_REMOTE_PATH
BUILD_STAMP=$BUILD_STAMP
EOF
}

push_package_to_device() {
    local remote_parent
    local remote_basename
    local local_basename

    remote_parent="$(dirname "$REMOTE_PACKAGE_DIR")"
    remote_basename="$(basename "$REMOTE_PACKAGE_DIR")"
    local_basename="$(basename "$OUTPUT_DIR")"

    echo ">>> [ADB] Pushing package: $OUTPUT_DIR -> $REMOTE_PACKAGE_DIR"
    adb shell "mkdir -p $remote_parent && rm -rf $REMOTE_PACKAGE_DIR"
    adb push "$OUTPUT_DIR" "$remote_parent/"

    if [[ "$local_basename" != "$remote_basename" ]]; then
        adb shell "rm -rf $REMOTE_PACKAGE_DIR && mv $remote_parent/$local_basename $REMOTE_PACKAGE_DIR"
    fi

    adb shell "chmod +x $REMOTE_PACKAGE_DIR/llm_bench $REMOTE_PACKAGE_DIR/llm_demo"
}

NDK_PATH="$(find_ndk || true)"
if [[ -z "$NDK_PATH" ]]; then
    echo "Android NDK not found in known locations." >&2
    exit 1
fi

NDK_PATH="$(prepare_ndk_for_wsl "$NDK_PATH")"
export ANDROID_NDK="$NDK_PATH"
echo "Using Android NDK: $ANDROID_NDK"

BUILD_GENERATOR="$(select_build_generator)"
if [[ "$BUILD_GENERATOR" == "Unix Makefiles" ]]; then
    require_cmd make
elif [[ "$BUILD_GENERATOR" == "Ninja" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        CMAKE_MAKE_PROGRAM_OVERRIDE="$(command -v ninja)"
    elif command -v ninja-build >/dev/null 2>&1; then
        CMAKE_MAKE_PROGRAM_OVERRIDE="$(command -v ninja-build)"
    else
        echo "Ninja generator selected but ninja was not found." >&2
        exit 1
    fi
else
    echo "Unsupported build generator: $BUILD_GENERATOR" >&2
    exit 1
fi

EXISTING_GENERATOR="$(detect_existing_generator)"
CLEAN_BUILD_CMAKE="$(cmake_bool "$CLEAN_BUILD")"
RUN_PERFETTO_AFTER_BUILD_CMAKE="$(cmake_bool "$RUN_PERFETTO_AFTER_BUILD")"
ADB_PUSH_AFTER_BUILD_CMAKE="$(cmake_bool "$ADB_PUSH_AFTER_BUILD")"

if [[ "$CLEAN_BUILD_CMAKE" == "ON" ]]; then
    rm -rf "$BUILD_WORK_DIR"
elif [[ -n "$EXISTING_GENERATOR" && "$EXISTING_GENERATOR" != "$BUILD_GENERATOR" ]]; then
    echo ">>> Build generator changed ($EXISTING_GENERATOR -> $BUILD_GENERATOR), recreating build directory."
    rm -rf "$BUILD_WORK_DIR"
fi
mkdir -p "$BUILD_WORK_DIR"

MNN_BUILD_TEST_CMAKE="$(cmake_bool "$MNN_BUILD_TEST")"
MNN_OPENCL_CMAKE="$(cmake_bool "$MNN_OPENCL")"

CMAKE_ARGS=(
    -S "$PROJECT_ROOT"
    -B "$BUILD_WORK_DIR"
    -G "$BUILD_GENERATOR"
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
    -DANDROID_USE_LEGACY_TOOLCHAIN_FILE=true
    -DCMAKE_BUILD_TYPE=Release
    -DANDROID_ABI="$ANDROID_ABI"
    -DANDROID_STL=c++_static
    -DANDROID_PLATFORM="$ANDROID_PLATFORM"
    -DANDROID_NATIVE_API_LEVEL="$ANDROID_NATIVE_API_LEVEL"
    -DMNN_BUILD_BENCHMARK=ON
    -DMNN_USE_SSE=OFF
    -DMNN_BUILD_TEST="$MNN_BUILD_TEST_CMAKE"
    -DMNN_BUILD_FOR_ANDROID_COMMAND=true
    -DNATIVE_LIBRARY_OUTPUT=.
    -DNATIVE_INCLUDE_OUTPUT=.
    -DMNN_LOW_MEMORY=true
    -DMNN_CPU_WEIGHT_DEQUANT_GEMM=true
    -DMNN_BUILD_LLM=true
    -DMNN_SUPPORT_TRANSFORMER_FUSE=true
    -DMNN_ARM82=true
    -DMNN_OPENCL="$MNN_OPENCL_CMAKE"
    -DMNN_USE_LOGCAT=true
    -DMNN_BUILD_DEMO=ON
    -DCMAKE_CXX_STANDARD=17
)

if [[ -n "${CMAKE_MAKE_PROGRAM_OVERRIDE:-}" ]]; then
    CMAKE_ARGS+=(-DCMAKE_MAKE_PROGRAM="$CMAKE_MAKE_PROGRAM_OVERRIDE")
fi

if [[ -n "${BUILD_TARGETS:-}" ]]; then
    read -r -a BUILD_TARGETS_ARRAY <<< "$BUILD_TARGETS"
else
    BUILD_TARGETS_ARRAY=(llm_bench llm_demo)
fi

echo "Using build generator: $BUILD_GENERATOR"
echo "Incremental build enabled: $([[ "$CLEAN_BUILD_CMAKE" == "ON" ]] && echo no || echo yes)"
echo "MNN_BUILD_TEST: $MNN_BUILD_TEST_CMAKE"
echo "MNN_OPENCL: $MNN_OPENCL_CMAKE"
echo "Build targets: ${BUILD_TARGETS_ARRAY[*]}"

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_WORK_DIR" --parallel "$JOBS" --target "${BUILD_TARGETS_ARRAY[@]}"

for required in libMNN.so libMNN_Express.so libllm.so llm_bench llm_demo; do
    if [[ ! -f "$BUILD_WORK_DIR/$required" ]]; then
        echo "Missing build artifact: $BUILD_WORK_DIR/$required" >&2
        exit 1
    fi
done

for model_file in config.json llm.mnn llm_config.json tokenizer.txt; do
    if [[ ! -f "$MODEL_SOURCE_DIR/$model_file" ]]; then
        echo "Missing model file: $MODEL_SOURCE_DIR/$model_file" >&2
        exit 1
    fi
done

copy_outputs
write_package_metadata

if [[ "$ADB_PUSH_AFTER_BUILD_CMAKE" == "ON" ]]; then
    require_cmd adb
    push_package_to_device
else
    echo ">>> [Skip] ADB_PUSH_AFTER_BUILD=$ADB_PUSH_AFTER_BUILD, skipping adb push"
fi

echo "Package prepared: $OUTPUT_DIR"
echo "Remote package dir: $REMOTE_PACKAGE_DIR"
echo "Remote model path : $MODEL_REMOTE_PATH"
echo "Package metadata  : $PACKAGE_METADATA_FILE"

# --- 编译完成后自动运行 run_perfetto_batch.sh ---
PERFETTO_SCRIPT="$PROJECT_ROOT/run_perfetto_batch.sh"

if [[ "$RUN_PERFETTO_AFTER_BUILD_CMAKE" == "ON" && -f "$PERFETTO_SCRIPT" ]]; then
    echo ">>> [Auto Run] Starting run_perfetto_batch.sh..."
    LOCAL_PKG="$OUTPUT_DIR" \
    REMOTE_DIR="$REMOTE_PACKAGE_DIR" \
    MODEL_REMOTE="$MODEL_REMOTE_PATH" \
    PACKAGE_METADATA_FILE="$PACKAGE_METADATA_FILE" \
    bash "$PERFETTO_SCRIPT"
elif [[ "$RUN_PERFETTO_AFTER_BUILD_CMAKE" != "ON" ]]; then
    echo ">>> [Skip] RUN_PERFETTO_AFTER_BUILD=$RUN_PERFETTO_AFTER_BUILD, skipping run_perfetto_batch.sh"
else
    echo ">>> [Warning] run_perfetto_batch.sh not found at $PERFETTO_SCRIPT"
fi
