#!/bin/bash

# ============================================================
# MNN LLM 性能测试自动化脚本 - 智能同步修复版
# ============================================================

# 1. 基础配置
LOCAL_PKG="android_demo_package"
REMOTE_DIR="/data/local/tmp/android_demo_package"
TRACE_FILE_REMOTE="/data/misc/perfetto-traces/temp_trace.perfetto-trace"
# 【注意】确保此 Config 的 duration_ms 足够长 (例如 60000ms)，我们会手动提前结束它
CONFIG_FILE="/data/misc/perfetto-configs/normal_config_30.pbtxt" 

DEST_BASE="../perfetto_traces"
DATE_FOLDER=$(date +"%Y%m%d")
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"

mkdir -p "$FINAL_DEST_DIR"

# 2. 推送与权限
echo ">>> [Init] 推送测试包..."
adb push "$LOCAL_PKG" /data/local/tmp/ > /dev/null 2>&1
adb shell "chmod +x $REMOTE_DIR/llm_bench" 

# 清理可能残留的 perfetto 进程（防止上次异常退出导致的残留）
adb shell "killall -9 perfetto > /dev/null 2>&1"

TEST_CASES=(
    # "1:1"
    # "2:6,7"
    # "3:5,6,7"
    "4:4,5,6,7"
    # "5:3,4,5,6,7"
    # "6:2,3,4,5,6,7"
    # "7:1,2,3,4,5,6,7"
    # "8:0,1,2,3,4,5,6,7"
)
for case in "${TEST_CASES[@]}"; do
    IFS=":" read -r threads core_ids <<< "$case"
    TIMESTAMP=$(date +"%H%M%S")
    FILENAME_IDS=${core_ids//,/_}
    LOCAL_TRACE_NAME="${TIMESTAMP}_${threads}T_ids${FILENAME_IDS}.perfetto-trace"
    
    echo "============================================================"
    echo "正在运行: 线程=$threads, 绑核=$core_ids"
    echo "============================================================"

    # ---------------------------------------------------------
    # 步骤 1: 启动 Perfetto (后台模式)
    # ---------------------------------------------------------
    echo ">>> [Step 1] 启动 Perfetto..."
    # 使用 nohup 确保 adb 断开后手机端进程不挂，--background 也是好习惯
    # 注意：我们先删除旧的远程文件，确保干净
    adb shell "rm $TRACE_FILE_REMOTE > /dev/null 2>&1"
    adb shell "nohup perfetto -o $TRACE_FILE_REMOTE -c $CONFIG_FILE --txt > /dev/null 2>&1 &"
    
    # 关键：等待 Perfetto 真正启动。通过检测进程是否存在来确认。
    echo "    -> 等待 Perfetto 初始化..."
    for i in {1..10}; do
        if adb shell "pidof perfetto > /dev/null"; then
            echo "    -> Perfetto 已运行 (PID Check OK)"
            break
        fi
        sleep 0.5
    done
    sleep 1 # 再多给1秒 buffer，确保 tracing start

    # ---------------------------------------------------------
    # 步骤 2: 运行 Benchmark
    # ---------------------------------------------------------
    echo ">>> [Step 2] 运行 llm_bench..."
    adb shell "cd $REMOTE_DIR && LD_LIBRARY_PATH=./ ./llm_bench -m ./model_dir/config.json -a cpu -t $threads -ids $core_ids"

    # ---------------------------------------------------------
    # 步骤 3: 优雅停止 Perfetto (关键修复)
    # ---------------------------------------------------------
    echo ">>> [Step 3] Benchmark 完成，正在停止 Perfetto..."
    # 发送 SIGINT (Ctrl+C的效果)，Perfetto 收到后会停止采集并 Flush 数据到磁盘
    # 只有 SIGINT 才能保证数据完整写入，千万不要用 kill -9
    adb shell "pkill -INT perfetto"

    # ---------------------------------------------------------
    # 步骤 4: 等待写入完成 (自旋锁)
    # ---------------------------------------------------------
    echo ">>> [Step 4] 等待数据写入磁盘..."
    WAIT_COUNT=0
    # 循环检查 perfetto 进程是否消失
    while adb shell "pidof perfetto > /dev/null"; do
        sleep 1
        ((WAIT_COUNT++))
        echo "    -> 等待 Perfetto 退出... (${WAIT_COUNT}s)"
        if [ $WAIT_COUNT -gt 20 ]; then
            echo "    -> [警告] Perfetto 退出超时，强制清理！"
            adb shell "killall -9 perfetto"
            break
        fi
    done

    # ---------------------------------------------------------
    # 步骤 5: 拉取文件
    # ---------------------------------------------------------
    echo ">>> [Step 5] 拉取 Trace: $LOCAL_TRACE_NAME"
    # 检查远程文件大小，确保不是空文件
    FILE_SIZE=$(adb shell "stat -c %s $TRACE_FILE_REMOTE 2>/dev/null | tr -d '\r'")
    
    if [ "$FILE_SIZE" -gt 0 ]; then
        adb pull "$TRACE_FILE_REMOTE" "$FINAL_DEST_DIR/$LOCAL_TRACE_NAME"
        # 只有拉取成功才清理
        if [ $? -eq 0 ]; then
            adb shell "rm $TRACE_FILE_REMOTE"
        fi
    else
        echo ">>> [错误] 远程 Trace 文件不存在或为空，本轮失败。"
    fi
    
    echo ">>> [Done] 本轮结束。"
    echo ""
done

echo "所有任务完成: $FINAL_DEST_DIR"