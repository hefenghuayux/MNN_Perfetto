#!/bin/bash

# ============================================================
# MNN LLM 性能测试自动化脚本 - 批量测试版
# ============================================================

# 1. 基础路径配置
LOCAL_PKG="android_demo_package"
REMOTE_DIR="/data/local/tmp/android_demo_package"
# 远程 Trace 临时文件路径
TRACE_FILE_REMOTE="/data/misc/perfetto-traces/temp_trace.perfetto-trace" 
# 请确保此 Config 文件设置了合理的时长 (例如 duration_ms: 10000 或更长)
CONFIG_FILE="/data/misc/perfetto-configs/normal_config_60.pbtxt" 

# 2. 目标归档路径
DEST_BASE="../perfetto_traces"
DATE_FOLDER=$(date +"%Y%m%d")
FINAL_DEST_DIR="$DEST_BASE/$DATE_FOLDER"

# 创建归档目录
echo ">>> [Init] 准备归档目录: $FINAL_DEST_DIR"
mkdir -p "$FINAL_DEST_DIR"

# 3. 推送测试包 (只推一次)
echo ">>> [Init] 推送测试包并设置权限..."
adb push "$LOCAL_PKG" /data/local/tmp/ > /dev/null 2>&1
adb shell "chmod +x $REMOTE_DIR/llm_bench" 

# ============================================================
# 定义测试序列
# 格式: "线程数:绑核ID列表"
# 逻辑: 从 2 线程(绑6,7) 逐步增加到 8 线程(绑0-7)
# ============================================================
TEST_CASES=(
    "2:6,7"
    "3:5,6,7"
    "4:4,5,6,7"
    "5:3,4,5,6,7"
    "6:2,3,4,5,6,7"
    "7:1,2,3,4,5,6,7"
    "8:0,1,2,3,4,5,6,7"
)

# ============================================================
# 循环执行测试
# ============================================================
for case in "${TEST_CASES[@]}"; do
    # 解析参数 (利用 IFS 分割字符串)
    IFS=":" read -r threads core_ids <<< "$case"
    
    # 生成带描述的文件名: 时间戳_线程数T_核心IDs.trace
    TIMESTAMP=$(date +"%H%M%S")
    # 将逗号替换为下划线以便用于文件名 (例如 6_7)
    FILENAME_IDS=${core_ids//,/_}
    LOCAL_TRACE_NAME="${TIMESTAMP}_${threads}T_ids${FILENAME_IDS}.perfetto-trace"
    
    echo "============================================================"
    echo "正在运行测试: 线程数=$threads, 绑核=$core_ids"
    echo "============================================================"

    # 1. 后台启动 Perfetto
    echo ">>> [Step 1] 启动 Perfetto..."
    # 使用 nohup 或后台运行，确保不阻塞脚本
    adb shell "perfetto -o $TRACE_FILE_REMOTE -c $CONFIG_FILE --txt" > /dev/null 2>&1 &
    
    # 2. 稍微等待 Perfetto 初始化 (防止 benchmark 先于 trace 跑完)
    sleep 1

    # 3. 执行 Benchmark
    echo ">>> [Step 2] 运行 llm_bench..."
    adb shell "cd $REMOTE_DIR && LD_LIBRARY_PATH=./ ./llm_bench -m ./model_dir/config.json -a cpu -t $threads -ids $core_ids"

    # 4. 等待数据写入 (根据 Config 文件的 duration，可能需要调整这里的 sleep)
    # 建议：如果 Config 是固定时长模式(如30s)，这里 sleep 时间应与 Config 时长接近
    echo ">>> [Step 3] 等待 Trace 数据落盘..."
   sleep 5
    
    # 5. 拉取文件
    echo ">>> [Step 4] 拉取 Trace 文件: $LOCAL_TRACE_NAME"
    adb pull "$TRACE_FILE_REMOTE" "$FINAL_DEST_DIR/$LOCAL_TRACE_NAME" 

    # 6. (可选) 清理远程临时 trace，防止空间占满
    adb shell "rm $TRACE_FILE_REMOTE"
    
    echo ">>> [Done] 本轮测试完成。"
    echo ""
done

echo "------------------------------------------------------------"
echo "所有任务完成！文件已保存至: $FINAL_DEST_DIR"
echo "------------------------------------------------------------"