# 子任务 F：AECS retune（guided, static_ratio=0.5, min_chunk=10）

## 运行命令
```bash
LOCAL_PKG=final_version6 \
PREFILL_STATIC_RATIO=0.5 \
PREFILL_SCHED_POLICY=guided \
PREFILL_MIN_CHUNK=10 \
DECODE_SCHED_POLICY=dynamic \
DECODE_DYNAMIC_BLOCKS=4 \
bash ./run_perfetto_batch.sh --aecs-retune
```

## 本轮约束
- 设备：OnePlus / Android 15
- 包目录：`/data/local/tmp/final_version6`
- 本轮按用户要求忽略能耗结论，不将能耗写入论文结论
- `stdout` 已保存到：`logs/aecs_guided_static05_min10_20260407.txt`
- 本轮结束后 `adb` 连接断开，导致对应 `logcat` 未能在结束后完整导出；因此下述“静态标定”和“已观察到的 decode 搜索过程”来自实时抓取记录

## 1. AECS 前置：核心簇性能比测量（可用于论文机制说明）

### 1.1 representative warmup
- `cpu_ids=7,6,5,4,3,2`
- `threads=6`
- `capacities=379,379,923,923,923,867,867,1024`
- speed = `517.084 tok/s`
- time = `0.990168 s`

### 1.2 pair = `7 / 6,5`
- warmup speed = `387.330 tok/s`, time = `1.321871 s`
- coarse-anchor:
  - ratio=`1.0000`, weights=`867:867`, speed=`387.928 tok/s`
- coarse:
  - ratio=`1.0630`, weights=`922:867`, speed=`385.966 tok/s`
  - ratio=`1.1811`, weights=`1024:867`, speed=`387.923 tok/s`
  - ratio=`1.2992`, weights=`1126:867`, speed=`389.141 tok/s`
  - ratio=`1.4173`, weights=`1229:867`, speed=`387.691 tok/s`
  - ratio=`1.5354`, weights=`1331:867`, speed=`387.239 tok/s`
- fine:
  - ratio=`1.2392`, weights=`1074:867`, speed=`388.164 tok/s`
  - ratio=`1.2592`, weights=`1092:867`, speed=`387.721 tok/s`
  - ratio=`1.2792`, weights=`1109:867`, speed=`388.074 tok/s`
  - ratio=`1.3192`, weights=`1144:867`, speed=`386.493 tok/s`
  - ratio=`1.3392`, weights=`1161:867`, speed=`391.134 tok/s`
  - ratio=`1.3592`, weights=`1178:867`, speed=`386.357 tok/s`
- selected:
  - `ratio=1.3392`
  - `speed=391.134 tok/s`
  - `after 12 measurements`

### 1.3 pair = `6,5 / 4,3,2`
- warmup speed = `420.690 tok/s`, time = `1.217047 s`
- inspected ratio `0.9393`，按代码逻辑钳制到 `>=1.0`
- coarse-anchor:
  - ratio=`1.0000`, weights=`923:923`, speed=`419.818 tok/s`
- coarse:
  - ratio=`1.0333`, weights=`954:923`, speed=`419.955 tok/s`
  - ratio=`1.1272`, weights=`1040:923`, speed=`419.837 tok/s`
  - ratio=`1.2211`, weights=`1127:923`, speed=`419.275 tok/s`
- fine:
  - ratio=`1.0133`, weights=`935:923`, speed=`419.383 tok/s`
  - ratio=`1.0533`, weights=`972:923`, speed=`419.083 tok/s`
  - ratio=`1.0733`, weights=`991:923`, speed=`420.097 tok/s`
  - ratio=`1.0933`, weights=`1009:923`, speed=`419.715 tok/s`
- selected:
  - `ratio=1.0733`
  - `speed=420.097 tok/s`
  - `after 8 measurements`

### 1.4 pair = `4,3,2 / 1,0`
- 本轮跳过实测
- 直接继承上一对的比例：`1.0733`
- 原因：prefill 不使用最慢簇，代码中显式跳过该 pair 的测量

### 1.5 validation 与最终采用容量
- validation begin:
  - `calibrated_capacities=100,100,107,107,107,115,115,154`
- accepted:
  - calibrated profile speed = `517.114 tok/s`
  - baseline speed = `515.956 tok/s`
- final cluster ratios:
  - `[1.33913, 1.07477, 1.07]`
- adopted per-cpu capacities:
  - `100,100,107,107,107,115,115,154`

## 2. 已观察到的 decode 搜索过程

### 2.1 Stage1
- `7` -> `61.960 tok/s`
- `7,6` -> `73.360 tok/s`
- `7,6,5` -> `74.409 tok/s`
- `7,6,5,4` -> `92.153 tok/s`
- `7,6,5,4,3` -> `94.958 tok/s`
- `7,6,5,4,3,2` -> `93.889 tok/s`

据此，Stage1 最快候选为 `7,6,5,4,3`。

### 2.2 Stage2 中已确认的候选
- feasible:
  - `7,6,5,4,3` -> speed=`94.958`, objective=`13.970173`
  - `7,6,5,4` -> speed=`92.505`, objective=`12.496493`
  - `7,6,4,3,2` -> speed=`94.904`, objective=`13.500890`
  - `7,4,3,2` -> speed=`92.487`, objective=`13.265748`
- infeasible:
  - `6,5,4,3` -> speed=`63.414`, objective=`15.227938`
  - `6,5,4` -> speed=`57.431`, objective=`13.628867`
  - `7,6,5` -> speed=`73.895`, objective=`13.576845`
  - `6,5,4,3,2` -> speed=`63.482`, objective=`16.396062`
  - `4,3,2` -> speed=`53.432`, objective=`15.735535`
  - `6,5` -> speed=`57.293`, objective=`10.712760`
  - `7,4` -> speed=`66.071`, objective=`13.694491`
  - `7,4,3` -> speed=`72.068`, objective=`14.594892`
  - `6,4,3,2` -> speed=`63.659`, objective=`14.850910`

说明：当前代码在 decode Stage2 中以 speed floor 内可行候选的最小 objective 作为最终选择准则；本轮又按用户要求忽略能耗结论，因此这里 objective 实际等价于启发式能量项。

## 3. 当前能确认到的结论边界
- 已经确认：本轮静态校准完成且通过验证。
- 已经确认：本轮 decode Stage1 最快候选是 `7,6,5,4,3`。
- 已经确认：Stage2 至少存在 4 个可行候选，其中已观察到的最小 objective 是 `7,6,5,4` 的 `12.496493`。
- 仍未直接抓到：
  - `Fastest decode candidate=...`
  - `Selected decode candidate=...`
  - `Feasible decode candidates: ...`
  - `All decode candidates: ...`
  - `Final execution plan ...`
  - `[llm_bench] Final result ...`
- 原因：`llm_bench` 结束后设备 `adb` 立即断连，当前轮次的 `logcat` 导出文件为空。

## 4. 论文使用建议
- 可以直接写入论文的部分：
  - 核心簇性能比测量流程
  - `final cluster ratios=[1.33913, 1.07477, 1.07]`
  - `adopted per-cpu capacities=100,100,107,107,107,115,115,154`
  - “最慢簇 pair 跳过测量、继承上一级比例”的机制说明
- 暂不建议直接写入论文的部分：
  - 本轮最终 decode 选中的核组合
  - 本轮最终正式吞吐结果
- 要写这两项，需要在设备重新连上后补抓一次最终日志，或者重跑一轮并立即保存 `logcat` / `tmp/aecs_cache.json`。
