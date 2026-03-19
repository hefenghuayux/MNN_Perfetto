import subprocess  # 用于在 Python 中执行系统命令（如 adb shell）
import re          # 正则表达式模块，用于从文本中提取性能数据
import time        # 时间模块，用于在测试间隙进行等待（冷却）
import statistics  # 统计模块，用于计算平均值

# ================= 配置区域 =================
ADB_PATH = "adb"  # adb 命令的路径，确保 adb 在系统环境变量中
REMOTE_WORK_DIR = "/data/local/tmp/android_demo_package"  # 手机上存放测试程序和模型的目录
REMOTE_MODEL_DIR = "./model_dir/config.json"              # 模型配置文件的相对路径
BIN_NAME = "./llm_bench"                                  # 测试二进制文件的名称

# 核心掩码参考 (Snapdragon 8 Gen 3)
# 0-1: Small (小核), 2-6: Big (大核), 7: Super (超大核)
# ===========================================

def run_adb_command(cmd):
    """在手机上执行 Shell 命令"""
    # 构造完整的 ADB 命令：先 cd 到工作目录，再执行具体指令
    full_cmd = f"{ADB_PATH} shell \"cd {REMOTE_WORK_DIR} && {cmd}\""
    print(f"Executing: {cmd}")  # 打印当前正在执行的命令，方便调试
    try:
        # subprocess.run 执行命令
        # shell=True: 通过 shell 执行
        # check=True: 如果命令返回非零状态码（出错），抛出异常
        # stdout/stderr=subprocess.PIPE: 捕获标准输出和错误输出
        result = subprocess.run(full_cmd, shell=True, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return result.stdout  # 返回命令的标准输出内容
    except subprocess.CalledProcessError as e:
        # 如果命令执行出错，打印错误信息
        print(f"Error: {e}")
        return None

def parse_mnn_output(output):
    """解析 llm_bench 输出的表格数据"""
    prefill_speed = 0.0  # 初始化预填充速度
    decode_speed = 0.0   # 初始化解码速度
    lines = output.split('\n')  # 按行分割输出内容
    for line in lines:
        # 匹配 Prefill 阶段 (pp512 表示 prompt processing 512 tokens)
        if "pp512" in line or "prefill" in line.lower():
            # 使用正则提取 "±" 之前的数字，即平均速度
            match = re.search(r'\|\s+([\d\.]+)\s+±', line)
            if match: prefill_speed = float(match.group(1))
        # 匹配 Decode 阶段 (tg128 表示 text generation 128 tokens)
        elif "tg128" in line or "decode" in line.lower():
            match = re.search(r'\|\s+([\d\.]+)\s+±', line)
            if match: decode_speed = float(match.group(1))
    return prefill_speed, decode_speed

def run_custom_test(threads, ids_list, desc):
    """
    运行自定义绑核测试的主逻辑
    threads: 线程数 (int)
    ids_list: 核心ID列表字符串 (str), 例如 "4,5,6,7"
    desc: 测试描述，用于区分不同的测试场景
    """
    print(f"\n[Custom Test] {desc}")
    print(f"Configuration: Threads={threads}, IDs=[{ids_list}]")
    
    # 构造 llm_bench 的运行命令
    # LD_LIBRARY_PATH=. : 指定动态库加载路径为当前目录
    # -m: 模型配置
    # -a cpu: 指定后端为 CPU
    # -t: 线程数
    # -ids: 指定绑定的核心 ID 列表 (这是本脚本测试的核心变量)
    cmd = f"LD_LIBRARY_PATH=. {BIN_NAME} -m {REMOTE_MODEL_DIR} -a cpu -t {threads} -ids {ids_list}"
    
    p_speeds = []  # 存储多次运行的 Prefill 速度
    d_speeds = []  # 存储多次运行的 Decode 速度
    
    # 循环运行 3 次，以取平均值减少误差
    for i in range(3):
        output = run_adb_command(cmd)  # 执行 ADB 命令
        if output:
            # 解析输出结果
            p_s, d_s = parse_mnn_output(output)
            if p_s > 0: p_speeds.append(p_s)
            if d_s > 0: d_speeds.append(d_s)
        time.sleep(1) # 每次运行后休眠 1 秒，让 CPU 稍微冷却，避免过热降频影响结果
        
    # 计算平均值，如果列表为空则为 0
    avg_p = statistics.mean(p_speeds) if p_speeds else 0
    avg_d = statistics.mean(d_speeds) if d_speeds else 0
    
    # 打印最终的测试结果
    print(f"Result -> Prefill: {avg_p:.2f} t/s | Decode: {avg_d:.2f} t/s")
    return avg_p, avg_d

if __name__ == "__main__":
    # 脚本入口
    print("!!! 请确保已运行定频脚本 !!!\n") # 提示用户定频，保证测试准确性

    # ---------------------------------------------------------
    # 场景 1: 用户原始请求的顺序
    # ids="4,5,6,7"
    # 根据 MNN 逻辑，列表第一个元素给主线程。
    # 这里主线程 (T0) 绑定 Core 4 (大核)，其他线程绑定 5,6,7。
    # ---------------------------------------------------------
    run_custom_test(
        threads=4, 
        ids_list="4,5,6,7", 
        desc="User Request: IDs 4,5,6,7 (T0->Core4, T3->Core7)"
    )

    # ---------------------------------------------------------
    # 场景 2: 优化后的建议顺序
    # ids="7,6,5,4"
    # 这里主线程 (T0) 绑定 Core 7 (超大核)，其他线程绑定 6,5,4。
    # 理论上主线程负责任务分发和同步，绑定在最强的核心上收益最大。
    # ---------------------------------------------------------
    run_custom_test(
        threads=4, 
        ids_list="7,6,5,4", 
        desc="Optimized: IDs 7,6,5,4 (T0->Core7, Others->Big)"
    )
    
    print("\nTest Finished.")