#!/usr/bin/env bash
# Example:
# wsl -d Ubuntu -- bash -lc 'cd /mnt/e/workspacce/WSL2/MNN_WSL2 && THREADS=4 PROMPT_TOKENS=8 GEN_TOKENS=8 REPEAT=1 bash ./run_perfetto_batch.sh'
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_LOCAL_PKG="$PROJECT_ROOT/mnn_aecs_run"
if [[ ! -d "$DEFAULT_LOCAL_PKG" ]]; then
    DEFAULT_LOCAL_PKG="$PROJECT_ROOT/android_demo_package"
fi
LOCAL_PKG="${LOCAL_PKG:-$DEFAULT_LOCAL_PKG}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/mnn_aecs_run}"
MODEL_REMOTE="${MODEL_REMOTE:-/data/local/tmp/android_demo_package/model_dir/config.json}"
BUILD_WORK_DIR="${BUILD_WORK_DIR:-$PROJECT_ROOT/project/android/build_64_wsl}"
CONFIG_REMOTE="${CONFIG_REMOTE:-/data/misc/perfetto-configs/normal_config_30.pbtxt}"
TRACE_REMOTE_BASE="${TRACE_REMOTE_BASE:-/data/misc/perfetto-traces}"
DEST_BASE="${DEST_BASE:-$PROJECT_ROOT/perfetto_traces}"
DATE_FOLDER="$(date +"%Y%m%d")"
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"
# --- 以下为同步 llm_bench.cpp 后的默认配置 ---
BACKEND="${BACKEND:-cpu}"           # 对应 backends {0}
THREADS="${THREADS:-4}"             # 对应 threads {4}
PROMPT_TOKENS="${PROMPT_TOKENS:-512}" # 对应 nPrompt {512}
GEN_TOKENS="${GEN_TOKENS:-128}"      # 对应 nGenerate {128}
REPEAT="${REPEAT:-5}"               # 对应 nRepeat {5}
DYNAMIC_OPTION="${DYNAMIC_OPTION:-0}" # 对应 dynamicOption {0}
KV_CACHE="${KV_CACHE:-true}"        # 对齐 AECS/llm_demo prefill+decode 测量口径
AUTO_BUILD="${AUTO_BUILD:-0}"

ENABLE_TRACE=false
if [[ "${1:-}" == "--trace" ]]; then
    ENABLE_TRACE=true
    shift
fi

if [[ "$AUTO_BUILD" == "1" ]]; then
    bash "$PROJECT_ROOT/build_android_llm_bench_wsl.sh"
fi

if [[ ! -d "$LOCAL_PKG" ]]; then
    echo "Local package not found: $LOCAL_PKG" >&2
    echo "Run ./build_android_llm_bench_wsl.sh first, or set AUTO_BUILD=1." >&2
    exit 1
fi

if [[ -n "${TEST_CASES_OVERRIDE:-}" ]]; then
    IFS=';' read -r -a TEST_CASES <<< "$TEST_CASES_OVERRIDE"
else
    TEST_CASES=(
    # "6:2,3,4,5,6,7"
    # 5:3,4,5,6,7


    # 3:5,6,7
    # 2:6,7
    2:6,7:6,7
        # "1:7:7" 
    )
fi

mkdir -p "$FINAL_DEST_DIR"

echo ">>> [Init] Pushing package to device..."
adb shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
adb push "$LOCAL_PKG"/. "$REMOTE_DIR"/ > /dev/null

LATEST_BUILD_ARTIFACTS=(
    libMNN.so
    libMNN_Express.so
    libllm.so
    llm_bench
    llm_demo
)
have_latest_build=1
for artifact in "${LATEST_BUILD_ARTIFACTS[@]}"; do
    if [[ ! -f "$BUILD_WORK_DIR/$artifact" ]]; then
        have_latest_build=0
        break
    fi
done
if [[ "$have_latest_build" == "1" ]]; then
    echo ">>> [Init] Overriding package binaries from $BUILD_WORK_DIR"
    for artifact in "${LATEST_BUILD_ARTIFACTS[@]}"; do
        adb push "$BUILD_WORK_DIR/$artifact" "$REMOTE_DIR/" > /dev/null
    done
else
    echo ">>> [Init] Build artifacts not complete in $BUILD_WORK_DIR, using package binaries as-is"
fi

adb shell "mkdir -p '$REMOTE_DIR/tmp'; chmod +x '$REMOTE_DIR/llm_bench' >/dev/null 2>&1 || true; chmod +x '$REMOTE_DIR/llm_demo' >/dev/null 2>&1 || true"
adb shell "test -f '$MODEL_REMOTE'"

if [[ "$ENABLE_TRACE" == "true" ]]; then
    adb shell "test -f '$CONFIG_REMOTE'"
fi

adb shell "killall -9 perfetto >/dev/null 2>&1 || true"

for case_item in "${TEST_CASES[@]}"; do
    IFS=":" read -r threads p_ids d_ids <<< "$case_item"
    timestamp="$(date +"%H%M%S")"
    p_name="${p_ids//,/_}"
    d_name="${d_ids//,/_}"
    trace_remote="$TRACE_REMOTE_BASE/${timestamp}_${threads}T_P${p_name}_D${d_name}.perfetto-trace"
    trace_local="$FINAL_DEST_DIR/$(basename "$trace_remote")"
    perfetto_pid=""

    echo "============================================================"
    echo "Running case: threads=$threads"
    echo "Prefill IDs: ${p_ids:-none}"
    echo "Decode IDs : ${d_ids:-none}"
    echo "============================================================"

    if [[ "$ENABLE_TRACE" == "true" ]]; then
        echo ">>> [Step 1] Starting Perfetto..."
        perfetto_pid="$(adb shell "rm -f '$trace_remote'; perfetto --txt -c '$CONFIG_REMOTE' -o '$trace_remote' --background-wait" | tr -d '\r' | tail -n 1)"
        if [[ ! "$perfetto_pid" =~ ^[0-9]+$ ]]; then
            echo "Failed to start Perfetto, got PID: $perfetto_pid" >&2
            exit 1
        fi
        echo ">>> [Trace] Perfetto PID: $perfetto_pid"
        sleep 2
    fi

    cmd="cd $REMOTE_DIR && LD_LIBRARY_PATH=. ./llm_bench -m '$MODEL_REMOTE' -a $BACKEND -t $threads -kv $KV_CACHE -p $PROMPT_TOKENS -n $GEN_TOKENS -rep $REPEAT -dyo $DYNAMIC_OPTION"
    
    if [[ -n "${p_ids:-}" && "$p_ids" != "none" ]]; then
        # 自动计算 p_ids 里用逗号隔开的核心数量
        p_count=$(IFS=','; set -- $p_ids; echo $#)
        cmd+=" -pids $p_ids -pt $p_count"
    fi
    
    if [[ -n "${d_ids:-}" && "$d_ids" != "none" ]]; then
        # 自动计算 d_ids 里用逗号隔开的核心数量
        d_count=$(IFS=','; set -- $d_ids; echo $#)
        cmd+=" -dids $d_ids -dt $d_count"
    fi

    echo ">>> [Step 2] Running llm_bench..."
    adb shell "$cmd"

    if [[ "$ENABLE_TRACE" == "true" ]]; then
        echo ">>> [Step 3] Stopping Perfetto..."
        adb shell "kill -TERM $perfetto_pid >/dev/null 2>&1 || true"

        echo ">>> [Step 4] Waiting for trace flush..."
        wait_count=0
        until adb shell "test -f '$trace_remote'" >/dev/null 2>&1; do
            sleep 1
            wait_count=$((wait_count + 1))
            if [[ "$wait_count" -gt 30 ]]; then
                echo "Timed out waiting for trace file: $trace_remote" >&2
                exit 1
            fi
        done
        sleep 2

        echo ">>> [Step 5] Pulling trace to $trace_local"
        adb pull "$trace_remote" "$trace_local" > /dev/null
        adb shell "rm -f '$trace_remote'"
    fi

    echo ">>> [Done] Case finished."
    echo

done

if [[ "$ENABLE_TRACE" == "true" ]]; then
    echo "Trace output directory: $FINAL_DEST_DIR"
fi
