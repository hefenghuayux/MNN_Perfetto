#!/bin/bash

# ============================================================
# MNN LLM 性能测试自动化脚本 - 自动归档版本
# ============================================================

# 1. 基础路径配置
LOCAL_PKG="android_demo_package"
REMOTE_DIR="/data/local/tmp/android_demo_package"
TRACE_FILE_REMOTE="/data/misc/perfetto-traces/llm_bench_atrace1.perfetto-trace" 
CONFIG_FILE="/data/misc/perfetto-configs/normal_config.pbtxt" 

# 2. 目标归档路径 (Windows 格式在 Bash 中建议使用正斜杠)
DEST_BASE="../perfetto_traces"
DATE_FOLDER=$(date +"%Y%m%d")
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"

# 3. 生成文件名时间戳 (格式如: 0119104455)
TIMESTAMP=$(date +"%m%d%H%M%S")
LOCAL_TRACE_NAME="${TIMESTAMP}.perfetto-trace"

# 执行流程
echo ">>> [1/6] 准备归档目录: $FINAL_DEST_DIR"
mkdir -p "$FINAL_DEST_DIR"

echo ">>> [2/6] 推送测试包并设置权限..."
# adb shell "find /data/local/tmp/android_demo_package -mindepth 1 ! -path '*/llm.mnn.weight' -delete"
adb push "$LOCAL_PKG" /data/local/tmp/ 
adb shell "chmod +x $REMOTE_DIR/llm_bench" 

echo ">>> [3/6] 后台启动 Perfetto 追踪..."
adb shell "perfetto -o $TRACE_FILE_REMOTE -c $CONFIG_FILE --txt" > /dev/null 2>&1 &

echo ">>> [4/6] 执行 LLM Benchmark (线程: 4, 核心: 4,5,6,7)..."
adb shell "cd $REMOTE_DIR && LD_LIBRARY_PATH=./ ./llm_bench -m ./model_dir/config.json -a cpu -t 4 -ids 4,5,6,7" 

echo ">>> [5/6] 等待数据写入并拉取文件..."
sleep 10
adb pull "$TRACE_FILE_REMOTE" "./$LOCAL_TRACE_NAME" 

echo ">>> [6/6] 移动文件到日期文件夹..."
mv "./$LOCAL_TRACE_NAME" "$FINAL_DEST_DIR/"

echo "------------------------------------------------------------"
echo "任务完成！"
echo "文件已保存至: $FINAL_DEST_DIR/$LOCAL_TRACE_NAME"
echo "------------------------------------------------------------"