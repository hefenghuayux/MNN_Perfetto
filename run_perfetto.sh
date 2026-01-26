#!/bin/bash

# ============================================================
# MNN LLM 性能测试自动化脚本 - 修复版
# ============================================================

# 1. 基础路径配置
LOCAL_PKG="android_demo_package"
REMOTE_DIR="/data/local/tmp/android_demo_package"
TIMESTAMP=$(date +"%m%d%H%M%S")
LOCAL_TRACE_NAME="${TIMESTAMP}.perfetto-trace"
TRACE_FILE_REMOTE="/data/misc/perfetto-traces/${TIMESTAMP}.perfetto-trace" 
# 【注意】请确保手机上真的有这个文件，或者在脚本里添加 adb push 步骤
CONFIG_FILE="/data/misc/perfetto-configs/normal_config_30.pbtxt" 

# 2. 目标归档路径
DEST_BASE="../perfetto_traces"
DATE_FOLDER=$(date +"%Y%m%d")
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"

# 执行流程
echo ">>> [1/6] 准备归档目录: $FINAL_DEST_DIR"
mkdir -p "$FINAL_DEST_DIR"

echo ">>> [2/6] 推送测试包并设置权限..."
adb push "$LOCAL_PKG" /data/local/tmp/ > /dev/null
adb shell "chmod +x $REMOTE_DIR/llm_bench" 

# 【新增】检查配置文件是否存在
echo ">>> [Check] 检查 Perfetto 配置文件..."
if adb shell "[ -f $CONFIG_FILE ]"; then
    echo "    -> 配置文件存在。"
else
    echo "    -> [错误] 手机上找不到配置文件: $CONFIG_FILE"
    echo "    -> 请先运行: adb push your_config.pbtxt $CONFIG_FILE"
    exit 1
fi

echo ">>> [3/6] 后台启动 Perfetto 追踪..."
# 【修改】使用 nohup 防止 adb 断开后进程被杀，并去掉 > /dev/null 以便调试（如果稳定了可以加回）
adb shell "perfetto -o $TRACE_FILE_REMOTE -c $CONFIG_FILE --txt  2>&1 &"

# 等待一秒确保 Perfetto 启动成功
sleep 2


echo ">>> [4/6] 执行 LLM Benchmark (线程: 3, 核心: 5,6,7)..."
adb shell "cd $REMOTE_DIR && LD_LIBRARY_PATH=./ ./llm_bench -m ./model_dir/config.json -a cpu -t 3 -ids 5,6,7" 

echo ">>> [5/6] 等待数据写入..."
# 确保等待时间 > Config文件里的 duration_ms
sleep 15

echo ">>> [Step 4] 拉取 Trace 文件..."
adb pull "$TRACE_FILE_REMOTE" "$FINAL_DEST_DIR/$LOCAL_TRACE_NAME" 
# 只有文件存在才清理
if [ -f "$FINAL_DEST_DIR/$LOCAL_TRACE_NAME" ]; then
    echo ">>> [6/6] 清理远程文件..."
    adb shell "rm $TRACE_FILE_REMOTE"
    echo "------------------------------------------------------------"
    echo "任务完成！"
    echo "文件已保存至: $FINAL_DEST_DIR/$LOCAL_TRACE_NAME"
    echo "------------------------------------------------------------"
else
    echo "------------------------------------------------------------"
    echo "[失败] 文件拉取失败，请检查上方报错信息。"
    echo "------------------------------------------------------------"
fi