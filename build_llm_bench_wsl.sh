#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build_wsl2}"
PACKAGE_DIR="${PACKAGE_DIR:-$PROJECT_ROOT/linux_llm_bench_package}"
MODEL_SOURCE_DIR="${MODEL_SOURCE_DIR:-$PROJECT_ROOT/model_dir}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(nproc)}"

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing required command: $1" >&2
        echo "Install dependencies in WSL2 first:" >&2
        echo "  sudo apt update && sudo apt install -y build-essential cmake git" >&2
        exit 1
    fi
}

require_cmd cmake
require_cmd c++

cmake_args=(
    -S "$PROJECT_ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_CXX_STANDARD=17
    -DMNN_LOW_MEMORY=true
    -DMNN_CPU_WEIGHT_DEQUANT_GEMM=true
    -DMNN_BUILD_LLM=true
    -DMNN_SUPPORT_TRANSFORMER_FUSE=true
    -DMNN_BUILD_DEMO=ON
)

if [[ "${MNN_AVX512:-auto}" == "auto" ]]; then
    if grep -qw avx512f /proc/cpuinfo 2>/dev/null; then
        cmake_args+=(-DMNN_AVX512=true)
        echo "AVX512 detected, enabling -DMNN_AVX512=true"
    fi
elif [[ "${MNN_AVX512}" == "1" || "${MNN_AVX512}" == "true" ]]; then
    cmake_args+=(-DMNN_AVX512=true)
fi

echo "Configuring build in $BUILD_DIR"
cmake "${cmake_args[@]}"

echo "Building llm_bench and llm_demo"
cmake --build "$BUILD_DIR" --target llm_bench llm_demo -j"$JOBS"

echo "Preparing runtime package in $PACKAGE_DIR"
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR/model_dir"

copy_if_exists() {
    local src="$1"
    if [[ -f "$src" ]]; then
        cp "$src" "$PACKAGE_DIR/"
    fi
}

copy_if_exists "$BUILD_DIR/llm_bench"
copy_if_exists "$BUILD_DIR/llm_demo"
copy_if_exists "$BUILD_DIR/libMNN.so"
copy_if_exists "$BUILD_DIR/libMNN_Express.so"
copy_if_exists "$BUILD_DIR/libllm.so"
copy_if_exists "$BUILD_DIR/libMNNOpenCV.so"
copy_if_exists "$BUILD_DIR/libMNN_CL.so"

if [[ -d "$MODEL_SOURCE_DIR" ]]; then
    cp -a "$MODEL_SOURCE_DIR/." "$PACKAGE_DIR/model_dir/"
else
    echo "Warning: model directory not found at $MODEL_SOURCE_DIR" >&2
fi

cat <<EOF

Build complete.
Runtime package: $PACKAGE_DIR

Run inside WSL2:
  cd "$PACKAGE_DIR"
  LD_LIBRARY_PATH=. ./llm_bench -m ./model_dir/config.json -a cpu -t \$(nproc)

If config.json expects an external weight file that is not present, try:
  LD_LIBRARY_PATH=. ./llm_bench -m ./model_dir/llm.mnn -a cpu -t \$(nproc)
EOF
