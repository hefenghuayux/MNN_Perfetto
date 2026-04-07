# 子任务 F：AECS 复现实验阻塞记录

生成时间：2026-04-03

## 目标

按论文提纲 4.4 补做 AECS 相关实验：
- 离线标定搜索成本
- Decode 速度/能耗对比
- 复现一致性与偏差来源

## 已完成核查

### 1. 手机侧 AECS 入口确认

测试包：`/data/local/tmp/mnn_aecs_run_20260330_191429`

`llm_bench --help` 已确认支持以下 AECS 参数：
- `--prefill-auto-bind`
- `--decode-aecs`
- `--force-retune`
- `--aecs-cache-file`
- `--decode-search-tokens`
- `--aecs-warmup-runs`
- `--aecs-measure-runs`

### 2. `kv=false` 口径下的参数语义核查

本地代码显示：当 `kv=false` 时，`llm_bench` 的参数实例生成逻辑不会把 `-p` 和 `-n` 直接合成一条联合 workload，而是先生成：
- `prompt-only` 实例
- `decode-only` 实例
- 最后才处理 `-pg`

相关代码位置：
- [llm_bench.cpp:770](/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench.cpp:770)
- [llm_bench.cpp:811](/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench.cpp:811)
- [llm_bench.cpp:850](/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench.cpp:850)
- [llm_bench.cpp:913](/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise/transformers/llm/engine/demo/llm_bench.cpp:913)

这会导致 AECS 在第一条 `decode=0` 的实例上先启动 tuning。

### 3. `kv=false` 路径的实际复现

命令示例（已实际执行）：
```bash
adb shell 'cd /data/local/tmp/mnn_aecs_run_20260330_191429 && \
LD_LIBRARY_PATH=./ ./llm_bench \
  -m ./model_dir/config.json -a cpu \
  -t 6 -pt 6 -dt 5 -ids 2,3,4,5,6,7 \
  -p 64 -n 16 -rep 1 -kv false \
  --prefill-auto-bind --decode-aecs --force-retune \
  --aecs-cache-file tmp/aecs_cache.json'
```

关键 logcat 事实：
- `Start tuning model=... prompt=64 decode=0`
- 随后立即 `Fatal signal 11 (SIGSEGV)`

这说明当前打包二进制下，`AECS + kv=false` 无法作为正式实验口径运行。

### 4. `kv=true` 路径的实际复现

命令示例（已实际执行）：
```bash
adb shell 'cd /data/local/tmp/mnn_aecs_run_20260330_191429 && \
LD_LIBRARY_PATH=./ ./llm_bench \
  -m ./model_dir/config.json -a cpu \
  -t 6 -pt 6 -dt 5 -ids 2,3,4,5,6,7 \
  -p 64 -n 16 -rep 1 -kv true \
  --sched-policy dynamic \
  --prefill-sched-policy guided \
  --prefill-static-ratio 0.05 \
  --prefill-dynamic-blocks 240 \
  --prefill-min-chunk 32 \
  --prefill-auto-bind --decode-aecs --force-retune \
  --decode-search-tokens 16 \
  --aecs-warmup-runs 1 --aecs-measure-runs 1 \
  --aecs-cache-file tmp/aecs_cache.json'
```

关键 logcat 事实：
- `Start tuning model=... prompt=64 decode=16`
- `Cache miss`
- `Prefill search start`
- `warmup representative candidate=7,6,5,4,3,2 threads=6 before exhaustive search`
- 热状态检查后立即 `Fatal signal 11 (SIGSEGV)`

说明即便避开 `kv=false` 的实例生成问题，当前包的 AECS 调优路径本身仍会在第一次 Prefill 候选测量前后崩溃。

### 5. 缓存与能耗现状

- `tmp/aecs_cache.json` 始终未生成，说明离线标定未完成。
- logcat 显示功耗来源为 `dumpsys battery fallback`。
- 同时出现 `External power is attached`，说明当前接电状态下电池侧能耗读数不适合作为正式能耗结果。

## 当前结论

### 可以确认的结论

- 当前手机上的 `mnn_aecs_run_20260330_191429` 包无法完成 AECS tuning。
- `AECS + kv=false` 口径存在实例生成逻辑与 tuning 入口不匹配的问题。
- `AECS + kv=true` 口径也会在 tuning 初期触发原生崩溃。
- 因此，论文 4.4 的正式表格现在不能直接补做，至少需要先解决 AECS 运行期稳定性。

### 当前不能给出的结果

- 离线标定搜索耗时
- AECS 最终选出的 decode 候选核组合
- cache hit / cache miss 对比
- 正式 decode 能耗/速度对比表

## 后续可执行路径

### 我可以继续做的

1. 定位当前 AECS 崩溃点，给出最小修复建议或直接修复代码。
2. 你确认后，我可以重新编译、推包到手机，并重新跑完整的 4.4 实验。
3. 如果你暂时不想改 AECS 代码，我可以先把论文里 4.2 / 4.3 相关图表和结论文本继续补齐。

### 需要你参与的

1. 如果要做正式能耗实验，建议你先确认手机是否需要断开外接电源，以及你是否接受 `dumpsys battery` 作为测量口径。
2. 如果你希望我继续修 AECS 并重编译，需要你接受这一阶段会进入代码修改与重新部署流程。
