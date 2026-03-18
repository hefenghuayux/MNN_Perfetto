#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="${PACKAGE_DIR:-$PROJECT_ROOT/linux_llm_bench_package}"
MODEL_DIR="${MODEL_DIR:-$PACKAGE_DIR/model_dir}"
THREADS="${THREADS:-$(nproc)}"
BACKEND="${BACKEND:-cpu}"
PROMPT_TOKENS="${PROMPT_TOKENS:-512}"
GEN_TOKENS="${GEN_TOKENS:-128}"
REPEAT="${REPEAT:-5}"
DYNAMIC_OPTION="${DYNAMIC_OPTION:-0}"

if [[ ! -x "$PACKAGE_DIR/llm_bench" ]]; then
    echo "llm_bench not found in $PACKAGE_DIR. Run ./build_llm_bench_wsl.sh first." >&2
    exit 1
fi

model_arg=""
if [[ -f "$MODEL_DIR/config.json" ]]; then
    model_arg="$MODEL_DIR/config.json"
elif [[ -f "$MODEL_DIR/llm.mnn" ]]; then
    model_arg="$MODEL_DIR/llm.mnn"
else
    echo "No model entry found in $MODEL_DIR" >&2
    exit 1
fi

args=(
    -m "$model_arg"
    -a "$BACKEND"
    -t "$THREADS"
    -p "$PROMPT_TOKENS"
    -n "$GEN_TOKENS"
    -rep "$REPEAT"
    -dyo "$DYNAMIC_OPTION"
)

if [[ -n "${CPU_IDS:-}" ]]; then
    args+=(-ids "$CPU_IDS")
fi
if [[ -n "${PREFILL_CPU_IDS:-}" ]]; then
    args+=(-pids "$PREFILL_CPU_IDS")
fi
if [[ -n "${DECODE_CPU_IDS:-}" ]]; then
    args+=(-dids "$DECODE_CPU_IDS")
fi

if [[ -f "$MODEL_DIR/config.json" && ! -f "$MODEL_DIR/llm.mnn.weight" ]]; then
    echo "Warning: $MODEL_DIR/config.json references llm.mnn.weight, but that file is missing." >&2
    echo "If loading fails, rerun with MODEL_DIR pointing to a complete exported model, or edit config.json." >&2
fi

cd "$PACKAGE_DIR"
export LD_LIBRARY_PATH="$PACKAGE_DIR:${LD_LIBRARY_PATH:-}"

echo "Running: ./llm_bench ${args[*]}"
./llm_bench "${args[@]}" "$@"
