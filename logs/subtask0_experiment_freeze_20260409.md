# 子任务0：第4章实验口径冻结（work_steal 当前版本）

## 1. 平台信息

- 测试日期：2026-04-09
- 手机品牌：OnePlus
- 设备型号：`PKG110`
- 设备代号：`OP5D2BL1`
- SoC：`SM8650`
- Android 版本：`15`
- 系统增量版本：`V.46ea308-2771c91-2776590`

### CPU 拓扑

根据 `/sys/devices/system/cpu/cpufreq/policy*` 实测：

- `policy0`：CPU `0,1`，`2265600` kHz
- `policy2`：CPU `2,3,4`，`3148800` kHz
- `policy5`：CPU `5,6`，`2956800` kHz
- `policy7`：CPU `7`，`3302400` kHz

论文可按 4 簇记录：

- little：`0,1`
- mid：`2,3,4`
- big：`5,6`
- prime：`7`

## 2. 软件与包版本

- 仓库路径：`/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise`
- Git 分支：`codex/prefill-weighted-worksteal-decode-dynamic`
- 当前 HEAD：`956912720b677f3c87012b245752453bd2163689`
- 工作区状态：dirty，本轮实验基于 2026-04-09 当前工作区与本地打包结果

### 当前优化版测试包

- 本地包目录：`/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/work_steal_decode_blocks_current`
- 手机包目录：`/data/local/tmp/work_steal_decode_blocks_current`
- 可执行程序：`llm_bench`
- `llm_bench --help` 已确认：
  - scheduler 固定为 `prefill=work_steal`
  - scheduler 固定为 `decode=dynamic`

### 编译口径

来自 `build_and_move.sh` 当前配置：

- `-DCMAKE_BUILD_TYPE=Release`
- `-DMNN_BUILD_BENCHMARK=ON`
- `-DMNN_LOW_MEMORY=true`
- `-DMNN_CPU_WEIGHT_DEQUANT_GEMM=true`
- `-DMNN_BUILD_LLM=true`
- `-DMNN_SUPPORT_TRANSFORMER_FUSE=true`
- `-DMNN_ARM82=true`
- `-DMNN_USE_LOGCAT=true`
- `-DMNN_BUILD_DEMO=ON`

来自 `CMakeLists.txt` 默认配置：

- `MNN_USE_THREAD_POOL=ON`
- `MNN_OPENMP=OFF`

### 模型配置

来自 `work_steal_decode_blocks_current/model_dir/config.json`：

- 模型配置文件：`./model_dir/config.json`
- 模型主文件：`llm.mnn`
- 权重文件：`llm.mnn.weight`
- backend：`cpu`
- precision：`low`
- memory：`low`
- 配置默认线程数：`4`

## 3. 固定运行口径

### 主实验输入

- `prompt=512`
- `decode=128`

### 补充输入预留

后续补充组若需要验证趋势稳定性，优先使用：

- `prompt=256`，`decode=64`
- `prompt=1024`，`decode=128`

### 固定运行参数

- `-a cpu`
- `-kv true`
- `--split-phase-bench`
- `-rep 5`
- `-mmp` 不传，保持默认 `0`，即当前口径为 `mmap=false`
- `--decode-dynamic-blocks` 默认不显式传参，保持程序默认口径

## 4. 重复、warmup 与环境记录

- 每组实验默认 `1` 次 warmup + `5` 次测量
- 关键结论组后续补测到 `10` 次
- 所有结果默认报告：
  - `mean`
  - `std`
- 关键组可追加：
  - `median`
  - `p95`

### 本轮冻结时环境快照

来自 `adb shell dumpsys battery` 与 thermal zone：

- 电量：`100%`
- 供电状态：`USB powered=true`
- 电池温度：`28.3 C`
- CPU 相关 thermal zone 观测范围：约 `28.9 C` 到 `32.0 C`

后续执行规则：

- 开始正式测量前保持手机空闲
- 若温度明显上升，暂停到接近上述区间再继续
- 同一小节实验尽量在相近电量与温度区间内连续完成

## 5. 日志与命名规则

本轮开始统一放在 `logs/` 下，按子任务单独存档：

- 子任务说明：`logs/subtaskX_*.md`
- 原始 stdout/logcat：`logs/subtaskX_*_raw_YYYYMMDD.txt`
- 若包含多 case 汇总：`logs/subtaskX_*_summary_YYYYMMDD.md`
- 若包含 trace：放在 `perfetto_traces/YYYYMMDD/`，文件名写入线程数、绑核集合、阶段信息

case 命名统一写明：

- 线程数
- 是否绑核
- 绑核集合
- 若分阶段，写明 `prefill` 与 `decode` 的线程数和 CPU ids

## 6. 当前已冻结的论文口径

从本文件起，第4章后续实验默认共用以下公共条件：

- 平台：OnePlus `PKG110` / `SM8650` / Android `15`
- 主输入：`512/128`
- 调度实现：`prefill=work_steal`，`decode=dynamic`
- 重复次数：`1` warmup + `5` measurement
- 汇报口径：`mean + std`
- 当前优化版包：`work_steal_decode_blocks_current`

除非后续实验明确声明新增变量，否则不再单独改这些基础条件。
