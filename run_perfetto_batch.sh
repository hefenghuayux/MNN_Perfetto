#!/usr/bin/env bash
# Example:
# wsl -d Ubuntu -- bash -lc 'cd ~/MNN_WSL2 && AUTO_BUILD=1 ENABLE_TRACE=0 bash ./run_perfetto_batch.sh'
#
# Scheduler sweep override format:
# SCHED_CASES_OVERRIDE='dynamic:auto:auto:auto:auto;hybrid:0.05:0.05:8:4;guided:0.05:0.02:auto:auto'
# Field order: policy:prefill_static:decode_static:prefill_chunks:decode_chunks

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_WORK_DIR="${BUILD_WORK_DIR:-$PROJECT_ROOT/project/android/build_64_wsl}"
PACKAGE_PREFIX="${PACKAGE_PREFIX:-mnn_aecs_run}"
PACKAGE_METADATA_FILE="${PACKAGE_METADATA_FILE:-$BUILD_WORK_DIR/last_android_package.env}"
TRACE_REMOTE_BASE="${TRACE_REMOTE_BASE:-/data/misc/perfetto-traces}"
DEST_BASE="${DEST_BASE:-$PROJECT_ROOT/perfetto_traces}"
DATE_FOLDER="${DATE_FOLDER:-$(date +"%Y%m%d")}"
RUN_STAMP="${RUN_STAMP:-$(date +"%Y%m%d_%H%M%S")}"
BACKEND="${BACKEND:-cpu}"
THREADS="${THREADS:-4}"
PROMPT_TOKENS="${PROMPT_TOKENS:-512}"
GEN_TOKENS="${GEN_TOKENS:-128}"
REPEAT="${REPEAT:-5}"
DYNAMIC_OPTION="${DYNAMIC_OPTION:-0}"
KV_CACHE="${KV_CACHE:-true}"
AUTO_BUILD="${AUTO_BUILD:-0}"
PUSH_PACKAGE_TO_DEVICE="${PUSH_PACKAGE_TO_DEVICE:-0}"
SYNC_LATEST_BUILD_ARTIFACTS="${SYNC_LATEST_BUILD_ARTIFACTS:-0}"
CONFIG_REMOTE="${CONFIG_REMOTE:-/data/misc/perfetto-configs/normal_config_30.pbtxt}"
LLM_BENCH_EXTRA_ARGS="${LLM_BENCH_EXTRA_ARGS:-}"
DEFAULT_PREFILL_IDS="${DEFAULT_PREFILL_IDS:-6,7}"
DEFAULT_DECODE_IDS="${DEFAULT_DECODE_IDS:-$DEFAULT_PREFILL_IDS}"

normalize_sched_value() {
    local value="${1:-auto}"
    if [[ -z "$value" ]]; then
        echo "auto"
    else
        echo "$value"
    fi
}

sanitize_token() {
    local value
    value="$(normalize_sched_value "$1")"
    value="${value//,/_}"
    value="${value//./p}"
    value="${value//-/_}"
    value="${value// /_}"
    if [[ -z "$value" ]]; then
        value="auto"
    fi
    printf '%s' "$value"
}

one_line() {
    printf '%s' "$1" | tr '\r\n\t' '   ' | sed 's/[[:space:]]\+/ /g; s/^ //; s/ $//'
}

read_metadata_value() {
    local key="$1"
    if [[ ! -f "$PACKAGE_METADATA_FILE" ]]; then
        return 0
    fi
    sed -n "s/^${key}=//p" "$PACKAGE_METADATA_FILE" | head -n 1
}

print_sync_hint() {
    if [[ -n "${LOCAL_PKG:-}" ]]; then
        echo "Hint: sync package manually or set PUSH_PACKAGE_TO_DEVICE=1"
        echo "  adb shell \"rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'\""
        echo "  adb push \"$LOCAL_PKG\"/. \"$REMOTE_DIR\"/"
    else
        echo "Hint: set LOCAL_PKG=/path/to/local/package and rerun with PUSH_PACKAGE_TO_DEVICE=1"
    fi
}

find_latest_package_dir() {
    local candidate=""
    candidate="$(find "$PROJECT_ROOT" -maxdepth 1 -mindepth 1 -type d -name "${PACKAGE_PREFIX}_*" | sort | tail -n 1)"
    if [[ -n "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
    fi
    if [[ -d "$PROJECT_ROOT/mnn_aecs_run" ]]; then
        printf '%s\n' "$PROJECT_ROOT/mnn_aecs_run"
        return 0
    fi
    if [[ -d "$PROJECT_ROOT/android_demo_package" ]]; then
        printf '%s\n' "$PROJECT_ROOT/android_demo_package"
        return 0
    fi
    return 1
}

ENABLE_TRACE=false
if [[ "${1:-}" == "--trace" ]]; then
    ENABLE_TRACE=true
    shift
fi

LOCAL_PKG_EXPLICIT=0
REMOTE_DIR_EXPLICIT=0
MODEL_REMOTE_EXPLICIT=0
if [[ -n "${LOCAL_PKG:-}" ]]; then
    LOCAL_PKG_EXPLICIT=1
fi
if [[ -n "${REMOTE_DIR:-}" ]]; then
    REMOTE_DIR_EXPLICIT=1
fi
if [[ -n "${MODEL_REMOTE:-}" ]]; then
    MODEL_REMOTE_EXPLICIT=1
fi

if [[ "$AUTO_BUILD" == "1" ]]; then
    RUN_PERFETTO_AFTER_BUILD=0 PACKAGE_METADATA_FILE="$PACKAGE_METADATA_FILE" bash "$PROJECT_ROOT/build_android_llm_bench_wsl.sh"
fi

METADATA_PACKAGE_NAME="$(read_metadata_value PACKAGE_NAME || true)"
METADATA_OUTPUT_DIR="$(read_metadata_value OUTPUT_DIR || true)"
METADATA_REMOTE_DIR="$(read_metadata_value REMOTE_DIR || true)"
METADATA_MODEL_REMOTE="$(read_metadata_value MODEL_REMOTE || true)"
METADATA_BUILD_STAMP="$(read_metadata_value BUILD_STAMP || true)"

if [[ "$REMOTE_DIR_EXPLICIT" != "1" ]]; then
    if [[ -n "$METADATA_REMOTE_DIR" ]]; then
        REMOTE_DIR="$METADATA_REMOTE_DIR"
    elif [[ -n "$METADATA_PACKAGE_NAME" ]]; then
        REMOTE_DIR="/data/local/tmp/$METADATA_PACKAGE_NAME"
    else
        fallback_local_pkg="$(find_latest_package_dir || true)"
        if [[ -n "$fallback_local_pkg" ]]; then
            REMOTE_DIR="/data/local/tmp/$(basename "$fallback_local_pkg")"
        else
            REMOTE_DIR="/data/local/tmp/$PACKAGE_PREFIX"
        fi
    fi
fi

if [[ "$LOCAL_PKG_EXPLICIT" != "1" ]]; then
    if [[ -n "$METADATA_OUTPUT_DIR" && -d "$METADATA_OUTPUT_DIR" ]]; then
        LOCAL_PKG="$METADATA_OUTPUT_DIR"
    else
        LOCAL_PKG="$(find_latest_package_dir || true)"
    fi
fi

PACKAGE_NAME="$(basename "$REMOTE_DIR")"
if [[ "$MODEL_REMOTE_EXPLICIT" != "1" ]]; then
    if [[ -n "$METADATA_MODEL_REMOTE" && -n "$METADATA_REMOTE_DIR" && "$REMOTE_DIR" == "$METADATA_REMOTE_DIR" ]]; then
        MODEL_REMOTE="$METADATA_MODEL_REMOTE"
    else
        MODEL_REMOTE="$REMOTE_DIR/model_dir/config.json"
    fi
fi

RUN_LABEL="${RUN_LABEL:-${RUN_STAMP}_${PACKAGE_NAME}}"
FINAL_DEST_DIR="${FINAL_DEST_DIR:-$DEST_BASE/$DATE_FOLDER/$RUN_LABEL}"
LOG_DIR="$FINAL_DEST_DIR/logs"
SUMMARY_TSV="$FINAL_DEST_DIR/summary.tsv"
SUMMARY_MD="$FINAL_DEST_DIR/summary.md"
BEST_TXT="$FINAL_DEST_DIR/best_by_case.txt"
RUN_INFO="$FINAL_DEST_DIR/run_info.txt"
mkdir -p "$FINAL_DEST_DIR" "$LOG_DIR"

if [[ -n "${TEST_CASES_OVERRIDE:-}" ]]; then
    IFS=';' read -r -a TEST_CASES <<< "$TEST_CASES_OVERRIDE"
else
    TEST_CASES=(
        "${THREADS}:${DEFAULT_PREFILL_IDS}:${DEFAULT_DECODE_IDS}"
    )
fi

SCHED_CASES=()
if [[ -n "${SCHED_CASES_OVERRIDE:-}" ]]; then
    IFS=';' read -r -a RAW_SCHED_CASES <<< "$SCHED_CASES_OVERRIDE"
    for sched_case in "${RAW_SCHED_CASES[@]}"; do
        [[ -z "$sched_case" ]] && continue
        IFS=':' read -r policy prefill_static decode_static prefill_chunks decode_chunks <<< "$sched_case"
        policy="${policy:-dynamic}"
        prefill_static="$(normalize_sched_value "$prefill_static")"
        decode_static="$(normalize_sched_value "$decode_static")"
        prefill_chunks="$(normalize_sched_value "$prefill_chunks")"
        decode_chunks="$(normalize_sched_value "$decode_chunks")"
        SCHED_CASES+=("${policy}:${prefill_static}:${decode_static}:${prefill_chunks}:${decode_chunks}")
    done
else
    IFS=',' read -r -a SCHED_POLICIES_ARRAY <<< "${SCHED_POLICIES:-dynamic}"
    IFS=',' read -r -a PREFILL_STATIC_ARRAY <<< "${PREFILL_STATIC_RATIOS:-auto}"
    IFS=',' read -r -a DECODE_STATIC_ARRAY <<< "${DECODE_STATIC_RATIOS:-auto}"
    IFS=',' read -r -a PREFILL_CHUNK_ARRAY <<< "${PREFILL_DYNAMIC_BLOCKS:-auto}"
    IFS=',' read -r -a DECODE_CHUNK_ARRAY <<< "${DECODE_DYNAMIC_BLOCKS:-auto}"

    [[ ${#SCHED_POLICIES_ARRAY[@]} -eq 0 || -z "${SCHED_POLICIES_ARRAY[0]}" ]] && SCHED_POLICIES_ARRAY=(dynamic)
    [[ ${#PREFILL_STATIC_ARRAY[@]} -eq 0 || -z "${PREFILL_STATIC_ARRAY[0]}" ]] && PREFILL_STATIC_ARRAY=(auto)
    [[ ${#DECODE_STATIC_ARRAY[@]} -eq 0 || -z "${DECODE_STATIC_ARRAY[0]}" ]] && DECODE_STATIC_ARRAY=(auto)
    [[ ${#PREFILL_CHUNK_ARRAY[@]} -eq 0 || -z "${PREFILL_CHUNK_ARRAY[0]}" ]] && PREFILL_CHUNK_ARRAY=(auto)
    [[ ${#DECODE_CHUNK_ARRAY[@]} -eq 0 || -z "${DECODE_CHUNK_ARRAY[0]}" ]] && DECODE_CHUNK_ARRAY=(auto)

    for policy in "${SCHED_POLICIES_ARRAY[@]}"; do
        policy="${policy:-dynamic}"
        for prefill_static in "${PREFILL_STATIC_ARRAY[@]}"; do
            for decode_static in "${DECODE_STATIC_ARRAY[@]}"; do
                for prefill_chunks in "${PREFILL_CHUNK_ARRAY[@]}"; do
                    for decode_chunks in "${DECODE_CHUNK_ARRAY[@]}"; do
                        SCHED_CASES+=("${policy}:$(normalize_sched_value "$prefill_static"):$(normalize_sched_value "$decode_static"):$(normalize_sched_value "$prefill_chunks"):$(normalize_sched_value "$decode_chunks")")
                    done
                done
            done
        done
    done
fi

if [[ ${#SCHED_CASES[@]} -eq 0 ]]; then
    echo "No scheduler cases configured." >&2
    exit 1
fi

{
    echo "run_label=$RUN_LABEL"
    echo "run_stamp=$RUN_STAMP"
    echo "package_name=$PACKAGE_NAME"
    echo "remote_dir=$REMOTE_DIR"
    echo "model_remote=$MODEL_REMOTE"
    echo "enable_trace=$ENABLE_TRACE"
    echo "push_package_to_device=$PUSH_PACKAGE_TO_DEVICE"
    echo "build_work_dir=$BUILD_WORK_DIR"
    echo "metadata_file=$PACKAGE_METADATA_FILE"
    echo "metadata_build_stamp=${METADATA_BUILD_STAMP:-unknown}"
    echo "sync_latest_build_artifacts=$SYNC_LATEST_BUILD_ARTIFACTS"
    echo "backend=$BACKEND"
    echo "threads_default=$THREADS"
    echo "prompt_tokens=$PROMPT_TOKENS"
    echo "gen_tokens=$GEN_TOKENS"
    echo "repeat=$REPEAT"
    echo "dynamic_option=$DYNAMIC_OPTION"
    echo "kv_cache=$KV_CACHE"
    echo "llm_bench_extra_args=$LLM_BENCH_EXTRA_ARGS"
    echo "local_pkg=${LOCAL_PKG:-unknown}"
    echo "test_cases=${TEST_CASES[*]}"
    echo "scheduler_cases=${SCHED_CASES[*]}"
} > "$RUN_INFO"

printf 'run_label\tpackage_name\tremote_dir\tthreads\tprefill_ids\tdecode_ids\tpolicy\tprefill_static\tdecode_static\tprefill_chunks\tdecode_chunks\tprefill_tok_s\tprefill_tok_s_stdev\tdecode_tok_s\tdecode_tok_s_stdev\tprefill_sched\tdecode_sched\tlog_file\ttrace_file\n' > "$SUMMARY_TSV"

echo ">>> [Init] Package name  : $PACKAGE_NAME"
echo ">>> [Init] Remote dir    : $REMOTE_DIR"
echo ">>> [Init] Model remote  : $MODEL_REMOTE"
echo ">>> [Init] Results dir   : $FINAL_DEST_DIR"
echo ">>> [Init] Scheduler cases: ${#SCHED_CASES[@]}"
echo ">>> [Init] Push package  : $PUSH_PACKAGE_TO_DEVICE"
if [[ "$PUSH_PACKAGE_TO_DEVICE" == "1" ]]; then
    if [[ -z "${LOCAL_PKG:-}" || ! -d "$LOCAL_PKG" ]]; then
        echo "Local package not found: ${LOCAL_PKG:-<unset>}" >&2
        echo "Run ./build_android_llm_bench_wsl.sh first, or set LOCAL_PKG explicitly." >&2
        exit 1
    fi
    echo ">>> [Init] Local package : $LOCAL_PKG"
    echo ">>> [Init] Sync package to device..."
    adb shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
    adb push "$LOCAL_PKG"/. "$REMOTE_DIR"/ > /dev/null
else
    echo ">>> [Init] Using existing remote package directory (no adb push)."
fi

if [[ "$SYNC_LATEST_BUILD_ARTIFACTS" == "1" ]]; then
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
        echo ">>> [Init] Build artifacts incomplete in $BUILD_WORK_DIR, keeping packaged binaries"
    fi
else
    echo ">>> [Init] Keeping packaged binaries as-is (SYNC_LATEST_BUILD_ARTIFACTS=0)"
fi

if ! adb shell "test -x '$REMOTE_DIR/llm_bench'" >/dev/null 2>&1; then
    echo "Remote llm_bench not found: $REMOTE_DIR/llm_bench" >&2
    print_sync_hint
    exit 1
fi

adb shell "mkdir -p '$REMOTE_DIR/tmp'; chmod +x '$REMOTE_DIR/llm_bench' >/dev/null 2>&1 || true; chmod +x '$REMOTE_DIR/llm_demo' >/dev/null 2>&1 || true"
if ! adb shell "test -f '$MODEL_REMOTE'" >/dev/null 2>&1; then
    echo "Remote model config not found: $MODEL_REMOTE" >&2
    print_sync_hint
    exit 1
fi

if [[ "$ENABLE_TRACE" == "true" ]]; then
    adb shell "test -f '$CONFIG_REMOTE'"
fi

adb shell "killall -9 perfetto >/dev/null 2>&1 || true"

for case_item in "${TEST_CASES[@]}"; do
    IFS=':' read -r threads p_ids d_ids <<< "$case_item"
    p_ids="${p_ids:-none}"
    d_ids="${d_ids:-$p_ids}"
    p_name="${p_ids//,/_}"
    d_name="${d_ids//,/_}"

    for sched_item in "${SCHED_CASES[@]}"; do
        IFS=':' read -r sched_policy sched_prefill_static sched_decode_static sched_prefill_chunks sched_decode_chunks <<< "$sched_item"
        case_stamp="$(date +"%H%M%S")"
        sched_label="pol$(sanitize_token "$sched_policy")_ps$(sanitize_token "$sched_prefill_static")_ds$(sanitize_token "$sched_decode_static")_pc$(sanitize_token "$sched_prefill_chunks")_dc$(sanitize_token "$sched_decode_chunks")"
        case_name="${case_stamp}_${threads}T_P${p_name}_D${d_name}_${sched_label}"
        trace_remote="$TRACE_REMOTE_BASE/${case_name}.perfetto-trace"
        trace_local="$FINAL_DEST_DIR/$(basename "$trace_remote")"
        log_file="$LOG_DIR/${case_name}.log"
        trace_file="-"
        perfetto_pid=""

        echo "============================================================"
        echo "Running case: threads=$threads prefill_ids=$p_ids decode_ids=$d_ids"
        echo "Scheduler   : policy=$sched_policy prefill_static=$sched_prefill_static decode_static=$sched_decode_static prefill_chunks=$sched_prefill_chunks decode_chunks=$sched_decode_chunks"
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

        cmd="cd '$REMOTE_DIR' && LD_LIBRARY_PATH=. ./llm_bench -m '$MODEL_REMOTE' -a $BACKEND -t $threads -kv $KV_CACHE -p $PROMPT_TOKENS -n $GEN_TOKENS -rep $REPEAT -dyo $DYNAMIC_OPTION"
        if [[ -n "$LLM_BENCH_EXTRA_ARGS" ]]; then
            cmd+=" $LLM_BENCH_EXTRA_ARGS"
        fi
        if [[ -n "$sched_policy" && "$sched_policy" != "auto" ]]; then
            cmd+=" --sched-policy $sched_policy"
        fi
        if [[ "$sched_prefill_static" != "auto" ]]; then
            cmd+=" --prefill-static-ratio $sched_prefill_static"
        fi
        if [[ "$sched_decode_static" != "auto" ]]; then
            cmd+=" --decode-static-ratio $sched_decode_static"
        fi
        if [[ "$sched_prefill_chunks" != "auto" ]]; then
            cmd+=" --prefill-dynamic-blocks $sched_prefill_chunks"
        fi
        if [[ "$sched_decode_chunks" != "auto" ]]; then
            cmd+=" --decode-dynamic-blocks $sched_decode_chunks"
        fi
        if [[ -n "$p_ids" && "$p_ids" != "none" ]]; then
            p_count=$(IFS=','; set -- $p_ids; echo $#)
            cmd+=" -pids $p_ids -pt $p_count"
        fi
        if [[ -n "$d_ids" && "$d_ids" != "none" ]]; then
            d_count=$(IFS=','; set -- $d_ids; echo $#)
            cmd+=" -dids $d_ids -dt $d_count"
        fi

        echo ">>> [Step 2] Running llm_bench..."
        adb logcat -c
        {
            echo ">>> [Command] $cmd"
            adb shell "$cmd"
        } 2>&1 | tee "$log_file"
        device_log_file="$LOG_DIR/${case_name}.device.log"
        adb logcat -d -s MNNJNI > "$device_log_file"

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
            trace_file="$trace_local"
        fi

        final_line="$(grep -F "[llm_bench] Final result" "$device_log_file" | tail -n 1 || true)"
        prefill_sched="$(grep -F "[llm_bench][sched][PREFILL]" "$device_log_file" | tail -n 1 || true)"
        decode_sched="$(grep -F "[llm_bench][sched][DECODE]" "$device_log_file" | tail -n 1 || true)"
        prefill_tok_s="NA"
        prefill_tok_s_stdev="NA"
        decode_tok_s="NA"
        decode_tok_s_stdev="NA"
        if [[ "$final_line" =~ prefill=([^[:space:]]+)[[:space:]]\+\-[[:space:]]([^[:space:]]+)[[:space:]]tok/s[[:space:]]decode=([^[:space:]]+)[[:space:]]\+\-[[:space:]]([^[:space:]]+)[[:space:]]tok/s ]]; then
            prefill_tok_s="${BASH_REMATCH[1]}"
            prefill_tok_s_stdev="${BASH_REMATCH[2]}"
            decode_tok_s="${BASH_REMATCH[3]}"
            decode_tok_s_stdev="${BASH_REMATCH[4]}"
        else
            speed_line="$(grep '^| model_dir' "$log_file" | tail -n 1 || true)"
            if [[ "$speed_line" =~ \|[[:space:]]*([0-9.]+)[[:space:]]±[[:space:]]([0-9.]+)\<br\>[[:space:]]*([0-9.]+)[[:space:]]±[[:space:]]([0-9.]+)[[:space:]]*\|[[:space:]]*$ ]]; then
                prefill_tok_s="${BASH_REMATCH[1]}"
                prefill_tok_s_stdev="${BASH_REMATCH[2]}"
                decode_tok_s="${BASH_REMATCH[3]}"
                decode_tok_s_stdev="${BASH_REMATCH[4]}"
            fi
        fi

        prefill_sched="$(one_line "$prefill_sched")"
        decode_sched="$(one_line "$decode_sched")"
        [[ -z "$prefill_sched" ]] && prefill_sched='-'
        [[ -z "$decode_sched" ]] && decode_sched='-'

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$RUN_LABEL" "$PACKAGE_NAME" "$REMOTE_DIR" "$threads" "$p_ids" "$d_ids" \
            "$sched_policy" "$sched_prefill_static" "$sched_decode_static" "$sched_prefill_chunks" "$sched_decode_chunks" \
            "$prefill_tok_s" "$prefill_tok_s_stdev" "$decode_tok_s" "$decode_tok_s_stdev" \
            "$prefill_sched" "$decode_sched" "$log_file" "$trace_file" >> "$SUMMARY_TSV"

        echo ">>> [Done] Case finished."
        echo
    done
done

{
    echo '| threads | prefill ids | decode ids | scheduler | prefill tok/s | decode tok/s | log | trace |'
    echo '| ---: | --- | --- | --- | ---: | ---: | --- | --- |'
    awk -F '\t' 'NR > 1 {
        printf("| %s | %s | %s | %s ps=%s ds=%s pc=%s dc=%s | %s ± %s | %s ± %s | %s | %s |\n",
               $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $18, $19)
    }' "$SUMMARY_TSV"
} > "$SUMMARY_MD"

awk -F '\t' '
NR == 1 {
    next
}
{
    key = "threads=" $4 ", prefill_ids=" $5 ", decode_ids=" $6
    sched = $7 " ps=" $8 " ds=" $9 " pc=" $10 " dc=" $11
    if ($12 != "NA" && (!(key in best_prefill) || ($12 + 0) > best_prefill[key])) {
        best_prefill[key] = $12 + 0
        best_prefill_cfg[key] = sched
    }
    if ($14 != "NA" && (!(key in best_decode) || ($14 + 0) > best_decode[key])) {
        best_decode[key] = $14 + 0
        best_decode_cfg[key] = sched
    }
    keys[key] = 1
}
END {
    for (key in keys) {
        print key
        if (key in best_prefill) {
            printf("  best prefill: %.3f tok/s (%s)\n", best_prefill[key], best_prefill_cfg[key])
        } else {
            print "  best prefill: N/A"
        }
        if (key in best_decode) {
            printf("  best decode : %.3f tok/s (%s)\n", best_decode[key], best_decode_cfg[key])
        } else {
            print "  best decode : N/A"
        }
        print ""
    }
}' "$SUMMARY_TSV" > "$BEST_TXT"

echo "Summary TSV : $SUMMARY_TSV"
echo "Summary MD  : $SUMMARY_MD"
echo "Best cases  : $BEST_TXT"
echo "Run info    : $RUN_INFO"
if [[ "$ENABLE_TRACE" == "true" ]]; then
    echo "Trace output directory: $FINAL_DEST_DIR"
fi
