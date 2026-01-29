#!/bin/bash

# ==========================================
# 配置区域
# ==========================================
EVENTS="instructions,cpu-cycles,L1-dcache-loads,L1-dcache-load-misses,LLC-loads,LLC-load-misses"
MODEL_CONFIG="./model_dir/config.json"
LIB_PATH="./"

# ==========================================
# 函数: 运行测试
# 参数 1: 线程数 (-t)
# 参数 2: 额外的应用参数 (如 -ids 绑核参数，可为空)
# 参数 3: 描述文本
# ==========================================
run_test() {
    THREADS=$1        # 接收线程数
    EXTRA_ARGS=$2     # 接收额外参数 (如 -ids ...)
    DESC=$3           # 接收描述文本

    echo "--------------------------------------------------"
    echo "开始测试: $DESC"
    echo "配置: 线程=$THREADS"
    
    if [ -n "$EXTRA_ARGS" ]; then
        echo "参数: $EXTRA_ARGS"
    else
        echo "参数: 无 (使用默认调度)"
    fi
    echo "--------------------------------------------------"

    # 1. 启动 llm_bench (后台运行)
    # 直接执行程序，将 EXTRA_ARGS 拼接到末尾
    LD_LIBRARY_PATH=$LIB_PATH ./llm_bench -m $MODEL_CONFIG -a cpu -t $THREADS $EXTRA_ARGS &
    
    # 2. 捕获进程 ID (PID)
    pid=$!
    echo "进程已启动, PID: $pid"

    # 3. 预热 (Warmup)
    sleep 2

    # 4. 运行 Simpleperf 统计
    echo "正在采集 Cache 数据 (持续 10 秒)..."
    simpleperf stat -e $EVENTS -p $pid --duration 10

    # 5. 清理进程
    echo "停止进程 $pid..."
    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    echo "完成: $DESC"
    echo -e "\n"
}

# ==========================================
# 第一轮: 4 线程 (基准)
# 参数2为空，不指定 -ids，由系统/MNN自动调度
# ==========================================
run_test 4 "" "4线程-基准测试"