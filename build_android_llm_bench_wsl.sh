#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_WORK_DIR="${BUILD_WORK_DIR:-$PROJECT_ROOT/project/android/build_64_wsl}"
MODEL_SOURCE_DIR="${MODEL_SOURCE_DIR:-$PROJECT_ROOT/model_dir}"
PACKAGE_NAME="${PACKAGE_NAME:-android_demo_package}"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/$PACKAGE_NAME}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-29}"
ANDROID_NATIVE_API_LEVEL="${ANDROID_NATIVE_API_LEVEL:-android-21}"
JOBS="${JOBS:-$(nproc)}"
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

require_cmd cmake
require_cmd make
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

    cp "$BUILD_WORK_DIR/libMNN.so" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/libMNN_Express.so" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/libllm.so" "$OUTPUT_DIR/"
    if [[ -f "$BUILD_WORK_DIR/libMNN_CL.so" ]]; then cp "$BUILD_WORK_DIR/libMNN_CL.so" "$OUTPUT_DIR/"; fi
    cp "$BUILD_WORK_DIR/llm_demo" "$OUTPUT_DIR/"
    cp "$BUILD_WORK_DIR/llm_bench" "$OUTPUT_DIR/"
    cp "$MODEL_SOURCE_DIR"/* "$OUTPUT_DIR/model_dir/"
}

NDK_PATH="$(find_ndk || true)"
if [[ -z "$NDK_PATH" ]]; then
    echo "Android NDK not found in known locations." >&2
    exit 1
fi

NDK_PATH="$(prepare_ndk_for_wsl "$NDK_PATH")"
export ANDROID_NDK="$NDK_PATH"
echo "Using Android NDK: $ANDROID_NDK"

if [[ "${CLEAN_BUILD:-1}" == "1" ]]; then
    rm -rf "$BUILD_WORK_DIR"
fi
mkdir -p "$BUILD_WORK_DIR"

cmake -S "$PROJECT_ROOT" -B "$BUILD_WORK_DIR" \
    -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_USE_LEGACY_TOOLCHAIN_FILE=true \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_STL=c++_static \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DANDROID_NATIVE_API_LEVEL="$ANDROID_NATIVE_API_LEVEL" \
    -DMNN_BUILD_BENCHMARK=ON \
    -DMNN_USE_SSE=OFF \
    -DMNN_BUILD_TEST=ON \
    -DMNN_BUILD_FOR_ANDROID_COMMAND=true \
    -DNATIVE_LIBRARY_OUTPUT=. \
    -DNATIVE_INCLUDE_OUTPUT=. \
    -DMNN_LOW_MEMORY=true \
    -DMNN_CPU_WEIGHT_DEQUANT_GEMM=true \
    -DMNN_BUILD_LLM=true \
    -DMNN_SUPPORT_TRANSFORMER_FUSE=true \
    -DMNN_ARM82=true \
    -DMNN_OPENCL=true \
    -DMNN_USE_LOGCAT=true \
    -DMNN_BUILD_DEMO=ON \
    -DCMAKE_CXX_STANDARD=17

cmake --build "$BUILD_WORK_DIR" -- -j"$JOBS"

for required in libMNN.so libMNN_Express.so libllm.so llm_bench llm_demo; do
    if [[ ! -f "$BUILD_WORK_DIR/$required" ]]; then
        echo "Missing build artifact: $BUILD_WORK_DIR/$required" >&2
        exit 1
    fi
done

for model_file in config.json llm.mnn llm.mnn.weight llm_config.json tokenizer.txt; do
    if [[ ! -f "$MODEL_SOURCE_DIR/$model_file" ]]; then
        echo "Missing model file: $MODEL_SOURCE_DIR/$model_file" >&2
        exit 1
    fi
done

copy_outputs

echo "Package prepared: $OUTPUT_DIR"