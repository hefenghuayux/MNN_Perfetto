#!/usr/bin/env bash
set -e

DEST_DIR="./related_files"
mkdir -p "$DEST_DIR"

FILES=(
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/source/core/Concurrency.h"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench.cpp"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/source/backend/cpu/compute/ConvInt8TiledExecutor.cpp"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/source/backend/cpu/AutoTuner.cpp"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/source/backend/cpu/CPUBackend.cpp"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/src/aecs_tuner.cpp"
    "/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench_aecs.cpp"
)

for f in "${FILES[@]}"; do
    cp "$f" "$DEST_DIR/"
done

echo "已复制 ${#FILES[@]} 个文件到 $DEST_DIR"
