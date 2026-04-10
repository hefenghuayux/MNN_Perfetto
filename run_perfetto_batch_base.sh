#!/bin/bash

# ============================================================
# MNN LLM 性能测试自动化脚本 - 基线版本 (统一全局绑核)
# ============================================================

# 0. 参数解析
ENABLE_TRACE=false
if [[ "$1" == "--trace" ]]; then
    ENABLE_TRACE=true
    echo ">>> [模式] Perfetto Tracing 已开启 (--trace)"
else
    echo ">>> [模式] Perfetto Tracing 已关闭 (默认)"
fi

REMOTE_BENCH_PREFIX=""
if [ "$ENABLE_TRACE" = true ]; then
    REMOTE_BENCH_PREFIX="export MNN_ENABLE_TRACE_MARKER=1; export MNN_ENABLE_HYBRID_INSTRUMENT=1; "
    echo ">>> [模式] Trace marker 与 Hybrid instrumentation 已开启"
fi

# 1. 基础配置
LOCAL_PKG="baseline"
REMOTE_DIR="/data/local/tmp/baseline"
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
# 格式: "线程数" 或 "线程数:核心列表"
# 示例:
#   "4"         -> 4线程，不绑核
#   "4:4,5,6,7" -> 4线程，全局绑在 4,5,6,7 核心上
# ---------------------------------------------------------
TEST_CASES=(
    # "4"
    "4:4,5,6,7"
    # "5"
    # "5:3,4,5,6,7"
    # "6"
    # "6:2,3,4,5,6,7"
)

for case in "${TEST_CASES[@]}"; do
    # 解析两段参数
    IFS=":" read -r threads ids <<< "$case"
    
    TIMESTAMP=$(date +"%H%M%S")
    if [ -n "$ids" ]; then
        # 生成文件名：包含全局绑核信息
        IDS_NAME=${ids//,/_}
        LOCAL_TRACE_NAME="${TIMESTAMP}_${threads}T_IDS_${IDS_NAME}.perfetto-trace"
    else
        # 不绑核场景标识
        LOCAL_TRACE_NAME="${TIMESTAMP}_${threads}T_NO_IDS.perfetto-trace"
    fi
    
    echo "============================================================"
    echo "正在运行: 线程=$threads"
    if [ -n "$ids" ]; then
        echo "全局绑核 Ids: $ids"
    else
        echo "全局绑核 Ids: 无 (不绑核)"
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
    # 使用基线参数 -t；只有设置了 ids 才追加 -ids
    echo ">>> [Step 2] 运行 llm_bench..."
    if [ -n "$ids" ]; then
        adb shell "cd $REMOTE_DIR && ${REMOTE_BENCH_PREFIX}LD_LIBRARY_PATH=./ ./llm_bench \
            -m ./model_dir/config.json \
            -a cpu \
            -t $threads \
            -ids $ids"
    else
        adb shell "cd $REMOTE_DIR && ${REMOTE_BENCH_PREFIX}LD_LIBRARY_PATH=./ ./llm_bench \
            -m ./model_dir/config.json \
            -a cpu \
            -t $threads"
    fi

        
        
         
        
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
