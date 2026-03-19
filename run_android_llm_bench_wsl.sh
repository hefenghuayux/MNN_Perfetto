#!/usr/bin/env bash

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_NAME="${PACKAGE_NAME:-android_demo_package}"
LOCAL_PKG="${LOCAL_PKG:-$PROJECT_ROOT/$PACKAGE_NAME}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/$PACKAGE_NAME}"
THREADS="${THREADS:-4}"
PROMPT_TOKENS="${PROMPT_TOKENS:-32}"
GEN_TOKENS="${GEN_TOKENS:-32}"
REPEAT="${REPEAT:-1}"
BACKEND="${BACKEND:-cpu}"
DYNAMIC_OPTION="${DYNAMIC_OPTION:-0}"

if [[ ! -d "$LOCAL_PKG" ]]; then
    echo "Local package not found: $LOCAL_PKG" >&2
    echo "Run ./build_android_llm_bench_wsl.sh first." >&2
    exit 1
fi

adb push "$LOCAL_PKG" /data/local/tmp/
adb shell "chmod +x $REMOTE_DIR/llm_bench $REMOTE_DIR/llm_demo"

CMD="cd $REMOTE_DIR && LD_LIBRARY_PATH=. ./llm_bench -m ./model_dir/config.json -a $BACKEND -t $THREADS -p $PROMPT_TOKENS -n $GEN_TOKENS -rep $REPEAT -dyo $DYNAMIC_OPTION"
if [[ -n "${CPU_IDS:-}" ]]; then
    CMD+=" -ids $CPU_IDS"
fi
if [[ -n "${PREFILL_CPU_IDS:-}" ]]; then
    CMD+=" -pids $PREFILL_CPU_IDS"
fi
if [[ -n "${DECODE_CPU_IDS:-}" ]]; then
    CMD+=" -dids $DECODE_CPU_IDS"
fi

echo "Running on device: $CMD"
adb shell "$CMD"