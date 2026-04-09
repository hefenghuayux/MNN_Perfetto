#!/bin/bash
# LOCAL_PKG=final_version1 \
# PREFILL_THREADS=6 \
# DECODE_THREADS=6 \
# PREFILL_CPU_IDS=2,3,4,5,6,7 \
# DECODE_CPU_IDS=2,3,6,7 \
# bash ./run_perfetto_batch.sh

# LOCAL_PKG=final_version2 \
# PREFILL_SCHED_POLICY=dynamic \
# PREFILL_DYNAMIC_BLOCKS=240 \
# DECODE_SCHED_POLICY=dynamic \
# DECODE_DYNAMIC_BLOCKS=4 \
# bash ./run_perfetto_batch.sh 

# LOCAL_PKG=final_version7 \
# PREFILL_SCHED_POLICY=guided \
# PREFILL_STATIC_RATIO=0.5 \
# PREFILL_DYNAMIC_BLOCKS=10 \
# DECODE_SCHED_POLICY=dynamic \
# DECODE_DYNAMIC_BLOCKS=4 \
# bash ./run_perfetto_batch.sh 

# LOCAL_PKG=final_version7 \
# DECODE_SCHED_POLICY=dynamic \
# DECODE_DYNAMIC_BLOCKS=4 \
# bash ./run_perfetto_batch.sh --trace --instrument

# LOCAL_PKG=/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/work_steal \
# REMOTE_DIR=/data/local/tmp/work_steal \
# KV_CACHE=true \
# PROMPT_TOKENS=512 \
# GENERATE_TOKENS=128 \
# REPEAT_COUNT=5 \
# SPLIT_PHASE_BENCH=true \
# bash ./run_perfetto_batch.sh --trace --instrument


# LOCAL_PKG=/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/work_steal_opt1 \
# REMOTE_DIR=/data/local/tmp/work_steal_opt1 \
# KV_CACHE=true \
# PROMPT_TOKENS=512 \
# GENERATE_TOKENS=128 \
# REPEAT_COUNT=5 \
# SPLIT_PHASE_BENCH=true \
# bash ./run_perfetto_batch.sh

# LOCAL_PKG=/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/work_steal_decode_chunks_eq_threads_stats  \
# REMOTE_DIR=/data/local/tmp/work_steal_decode_chunks_eq_threads_stats \
# KV_CACHE=true \
# PROMPT_TOKENS=512 \
# GENERATE_TOKENS=128 \
# REPEAT_COUNT=5 \
# SPLIT_PHASE_BENCH=true \
# bash ./run_perfetto_batch.sh
# ============================================================
# MNN LLM 性能测试自动化脚本 - 基线版本 (统一全局绑核)
# ============================================================

# 0. 参数解析
ENABLE_TRACE=false
ENABLE_INSTRUMENT=false
ENABLE_AECS_RETUNE=false
LLM_BENCH_ARGS=()

join_quoted_args() {
    local output=""
    local arg
    for arg in "$@"; do
        output+=" $(printf '%q' "$arg")"
    done
    printf '%s' "$output"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trace)
            ENABLE_TRACE=true
            shift
            ;;
        --instrument)
            ENABLE_INSTRUMENT=true
            shift
            ;;
        --aecs-retune)
            ENABLE_AECS_RETUNE=true
            shift
            ;;
        --)
            shift
            while [[ $# -gt 0 ]]; do
                LLM_BENCH_ARGS+=("$1")
                shift
            done
            ;;
        *)
            LLM_BENCH_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ "$ENABLE_TRACE" = true ]; then
    echo ">>> [模式] Perfetto Tracing 已开启 (--trace)"
else
    echo ">>> [模式] Perfetto Tracing 已关闭 (默认)"
fi

if [ "$ENABLE_INSTRUMENT" = true ]; then
    echo ">>> [模式] Schedule instrumentation 已开启 (--instrument)"
else
    echo ">>> [模式] Schedule instrumentation 已关闭 (默认)"
fi

if [ "$ENABLE_AECS_RETUNE" = true ]; then
    echo ">>> [模式] AECS 离线搜索已开启 (--aecs-retune)"
else
    echo ">>> [模式] AECS 离线搜索已关闭 (默认)"
fi

EXTRA_BENCH_ARGS=""
if [ ${#LLM_BENCH_ARGS[@]} -gt 0 ]; then
    EXTRA_BENCH_ARGS=$(join_quoted_args "${LLM_BENCH_ARGS[@]}")
    echo ">>> [参数] 透传 llm_bench 参数: ${LLM_BENCH_ARGS[*]}"
fi

# 0.5 执行参数（调度策略在 llm_bench 内固定为 prefill=work_steal, decode=dynamic）
PREFILL_THREADS="${PREFILL_THREADS:-6}"
DECODE_THREADS="${DECODE_THREADS:-6}"
PREFILL_CPU_IDS="${PREFILL_CPU_IDS:-}"
DECODE_CPU_IDS="${DECODE_CPU_IDS:-}"
SPLIT_PHASE_BENCH="${SPLIT_PHASE_BENCH:-true}"
DECODE_DYNAMIC_BLOCKS="${DECODE_DYNAMIC_BLOCKS:-}"

# 基线 workload 默认与手工最优回归保持一致。
KV_CACHE="${KV_CACHE:-true}"
PROMPT_TOKENS="${PROMPT_TOKENS:-512}"
GENERATE_TOKENS="${GENERATE_TOKENS:-128}"
REPEAT_COUNT="${REPEAT_COUNT:-5}"
DYNAMIC_OPTION="${DYNAMIC_OPTION:-0}"

SCRIPT_SCHED_ARGS=()
SCRIPT_FEATURE_ARGS=()

append_sched_arg() {
    local key="$1"
    local value="$2"
    if [[ -n "$value" ]]; then
        SCRIPT_SCHED_ARGS+=("$key" "$value")
    fi
}

count_csv_items() {
    local csv="$1"
    if [[ -z "$csv" ]]; then
        echo 0
        return
    fi
    local count=1
    local rest="$csv"
    while [[ "$rest" == *,* ]]; do
        rest="${rest#*,}"
        ((count++))
    done
    echo "$count"
}

if [ "$ENABLE_AECS_RETUNE" != true ]; then
    append_sched_arg "--prefill-cpu-ids" "$PREFILL_CPU_IDS"
    append_sched_arg "--decode-cpu-ids" "$DECODE_CPU_IDS"
    append_sched_arg "--decode-dynamic-blocks" "$DECODE_DYNAMIC_BLOCKS"
fi

SCRIPT_SCHED_ARGS_STR=""
if [ ${#SCRIPT_SCHED_ARGS[@]} -gt 0 ]; then
    SCRIPT_SCHED_ARGS_STR=$(join_quoted_args "${SCRIPT_SCHED_ARGS[@]}")
    echo ">>> [参数] 脚本默认绑核参数: ${SCRIPT_SCHED_ARGS[*]}"
fi

if [ "$SPLIT_PHASE_BENCH" = true ]; then
    SCRIPT_FEATURE_ARGS+=("--split-phase-bench")
fi

if [ "$ENABLE_AECS_RETUNE" = true ]; then
    # 只有显式开启时才运行 AECS，并强制重新搜索，默认完全不走 AECS 路径。
    SCRIPT_FEATURE_ARGS+=("--prefill-auto-bind" "--decode-aecs" "--force-retune")
fi

SCRIPT_FEATURE_ARGS_STR=""
if [ ${#SCRIPT_FEATURE_ARGS[@]} -gt 0 ]; then
    SCRIPT_FEATURE_ARGS_STR=$(join_quoted_args "${SCRIPT_FEATURE_ARGS[@]}")
    echo ">>> [参数] 脚本功能参数: ${SCRIPT_FEATURE_ARGS[*]}"
fi

REMOTE_BENCH_PREFIX=""
if [ "$ENABLE_TRACE" = true ] || [ "$ENABLE_INSTRUMENT" = true ]; then
    REMOTE_BENCH_PREFIX="export MNN_ENABLE_TRACE_MARKER=1; export MNN_ENABLE_SCHEDULE_INSTRUMENT=1; "
fi

# 1. 基础配置
LOCAL_PKG="${LOCAL_PKG:-final_version1}"
REMOTE_DIR="${REMOTE_DIR:-/data/local/tmp/${LOCAL_PKG}}"
TRACE_FILE_REMOTE="/data/misc/perfetto-traces/temp_trace.perfetto-trace"
# 【注意】确保此 Config 的 duration_ms 足够长 (例如 60000ms)，我们会手动提前结束它
CONFIG_FILE="/data/misc/perfetto-configs/normal_config_30.pbtxt" 

DEST_BASE="../perfetto_traces"
DATE_FOLDER=$(date +"%Y%m%d")
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"



if [ "$ENABLE_TRACE" = true ]; then
    mkdir -p "$FINAL_DEST_DIR"
fi

# 2. 推送与权限
# echo ">>> [Init] 推送测试包..."
# adb push "$LOCAL_PKG" /data/local/tmp/ > /dev/null 2>&1
# adb shell "chmod +x $REMOTE_DIR/llm_bench" 
adb shell "killall -9 perfetto > /dev/null 2>&1"

# ---------------------------------------------------------
# 测试用例定义
# 格式:
#   "线程数:核心列表"
#   或 "线程数:核心列表:decode核心列表" (prefill 绑核=核心列表, decode 绑核=第三段)
#   或 "线程数:核心列表:prefill核心列表:decode核心列表"
#   或 "线程数:核心列表:prefill核心列表:decode核心列表:prefill线程:decode线程"
# 示例:
#   "4:4,5,6,7"                    -> -t 4, phase线程默认跟随 -t
#   "6:2,3,4,5,6,7:2,3,4,7"        -> -t 6, -pt 6 -dt 4, pids=2,3,4,5,6,7 dids=2,3,4,7
#   "6:2,3,4,5,6,7:2,3,4,5,7:3,4,5" -> -t 6, pids=2,3,4,5,7 dids=3,4,5
#   "6:2,3,4,5,6,7:2,3,4,5,7:3,4,5:5:3" -> 显式 -pt 5 -dt 3
# ---------------------------------------------------------
TEST_CASES=(
    # 7:2,3,4,5,6,7
    "6:2,3,4,5,6,7:2,3,6,7"
    # 5:2,3,4,6,7
    
    # "4:4,5,6,7" 
    # 3:5,6,7
    # 2:6,7
    # 2:5,6
    # 1:7
)

for case in "${TEST_CASES[@]}"; do
    # 解析参数:
    # threads:ids[:decode_ids]
    # threads:ids[:prefill_ids:decode_ids]
    # threads:ids[:prefill_ids:decode_ids:prefill_threads:decode_threads]
    IFS=":" read -r threads ids field3 field4 field5 field6 <<< "$case"

    case_prefill_ids=""
    case_decode_ids=""
    case_prefill_threads=""
    case_decode_threads=""

    if [[ -n "$field3" ]]; then
        if [[ -z "$field4" ]]; then
            # 三段格式: threads:ids:decode_ids
            # 语义: -pt 跟随 -t，-dt 按 decode 核心数自动推导，pids 使用全局 ids。
            case_prefill_ids="$ids"
            case_decode_ids="$field3"
            case_prefill_threads="$threads"
            case_decode_threads=$(count_csv_items "$case_decode_ids")
        else
            # 四段或六段格式: threads:ids:prefill_ids:decode_ids[:pt:dt]
            case_prefill_ids="$field3"
            case_decode_ids="$field4"
            case_prefill_threads="$field5"
            case_decode_threads="$field6"
        fi
    fi

    prefill_threads="${case_prefill_threads:-$PREFILL_THREADS}"
    decode_threads="${case_decode_threads:-$DECODE_THREADS}"

    # 未显式配置 phase 线程时，优先按 phase 绑核列表长度自动推导
    if [[ -z "$prefill_threads" && -n "$case_prefill_ids" ]]; then
        prefill_threads=$(count_csv_items "$case_prefill_ids")
    fi
    if [[ -z "$decode_threads" && -n "$case_decode_ids" ]]; then
        decode_threads=$(count_csv_items "$case_decode_ids")
    fi

    if [[ -z "$prefill_threads" ]]; then
        prefill_threads="$threads"
    fi
    if [[ -z "$decode_threads" ]]; then
        decode_threads="$threads"
    fi

    PHASE_THREAD_ARGS_STR=$(join_quoted_args "-pt" "$prefill_threads" "-dt" "$decode_threads")
    CASE_PHASE_CPU_ARGS_STR=""
    if [ "$ENABLE_AECS_RETUNE" != true ]; then
        if [[ -n "$case_prefill_ids" ]]; then
            CASE_PHASE_CPU_ARGS_STR+=" $(printf '%q' "--prefill-cpu-ids") $(printf '%q' "$case_prefill_ids")"
        fi
        if [[ -n "$case_decode_ids" ]]; then
            CASE_PHASE_CPU_ARGS_STR+=" $(printf '%q' "--decode-cpu-ids") $(printf '%q' "$case_decode_ids")"
        fi
    fi
    
    TIMESTAMP=$(date +"%H%M%S")
    # 生成文件名：包含全局绑核信息
    IDS_NAME=${ids//,/_}
    LOCAL_TRACE_NAME="${TIMESTAMP}_${threads}T_IDS_${IDS_NAME}.perfetto-trace"
    
    echo "============================================================"
    echo "正在运行: 线程=$threads"
    echo "Prefill线程: $prefill_threads | Decode线程: $decode_threads"
    echo "全局绑核 Ids: $ids"
    echo "测试包目录: $REMOTE_DIR"
    if [ "$ENABLE_AECS_RETUNE" = true ]; then
        echo "Prefill绑核 Ids: <AECS 搜索开启，已跳过传参> | Decode绑核 Ids: <AECS 搜索开启，已跳过传参>"
    elif [[ -n "$case_prefill_ids" || -n "$case_decode_ids" ]]; then
        echo "Prefill绑核 Ids: ${case_prefill_ids:-<默认>} | Decode绑核 Ids: ${case_decode_ids:-<默认>}"
    fi
    echo "============================================================"

    # 步骤 1: 启动 Perfetto
    if [ "$ENABLE_TRACE" = true ]; then
        echo ">>> [Step 1] 启动 Perfetto..."
        adb shell "rm $TRACE_FILE_REMOTE > /dev/null 2>&1"
        adb shell "nohup perfetto -o $TRACE_FILE_REMOTE -c $CONFIG_FILE --txt > /dev/null 2>&1 &"
        sleep 2
    fi

    # 步骤 2: 运行 llm_bench
    # 使用基线参数 -t 和 -ids
    echo ">>> [Step 2] 运行 llm_bench..."
    adb shell "cd $REMOTE_DIR && ${REMOTE_BENCH_PREFIX}LD_LIBRARY_PATH=./ ./llm_bench \
        -m ./model_dir/config.json \
        -a cpu \
        -kv $KV_CACHE \
        -p $PROMPT_TOKENS \
        -n $GENERATE_TOKENS \
        -rep $REPEAT_COUNT \
        -dyo $DYNAMIC_OPTION \
        -t $threads \
        -ids $ids${PHASE_THREAD_ARGS_STR}${SCRIPT_SCHED_ARGS_STR}${CASE_PHASE_CPU_ARGS_STR}${SCRIPT_FEATURE_ARGS_STR}${EXTRA_BENCH_ARGS}"

        
        
         
        
    # 步骤 3, 4, 5: 停止/等待/拉取
    if [ "$ENABLE_TRACE" = true ]; then
        echo ">>> [Step 3] 停止 Perfetto..."
        adb shell "pkill -INT perfetto"

        echo ">>> [Step 4] 等待写入..."
        WAIT_COUNT=0
        while adb shell "pidof perfetto > /dev/null"; do
            sleep 1
            ((WAIT_COUNT++))
            [ $WAIT_COUNT -gt 20 ] && break
        done

        echo ">>> [Step 5] 拉取 Trace..."
        adb pull "$TRACE_FILE_REMOTE" "$FINAL_DEST_DIR/$LOCAL_TRACE_NAME"
        adb shell "rm $TRACE_FILE_REMOTE > /dev/null 2>&1"
    fi
    
    echo ">>> [Done] 本轮结束。"
    echo ""
done
