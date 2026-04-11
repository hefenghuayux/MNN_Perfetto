# Prefill static vs work\_steal（2026-04-11）

## 实验口径

- 测试包：`/data/local/tmp/mnn_aecs_run_20260411_191830_pool8_default`
- 脚本入口：`run_perfetto_batch.sh`
- 使用的 case：
  - `8:0,1,2,3,4,5,6,7:2,3,4,5,6,7:7:6:1`
- 解释：
  - pool 线程池：`8`，绑定 `0-7`
  - real prefill：`6` 线程，绑定 `2,3,4,5,6,7`
  - real decode：`1` 线程，绑定 `7`
- workload：
  - `KV_CACHE=true`
  - `PROMPT_TOKENS=512`
  - `GENERATE_TOKENS=128`
  - `REPEAT_COUNT=5`
  - `SPLIT_PHASE_BENCH=true`
- `decode_prime` 使用包默认值：开启

## 对比组

1. `PREFILL_POLICY=work_steal`
2. `PREFILL_POLICY=static`

Decode 保持同一实现：

- `decode=dynamic`

## 结果

- `work_steal`：
  - `pp512 = 817.67 ± 2.95 ms`
  - `tg128 = 557.38 ± 1.55 ms`
  - `decode tok/s = 229.65`
  - `E2E = 1375.05 ± 3.27 ms`

- `static`：
  - `pp512 = 962.66 ± 2.05 ms`
  - `tg128 = 455.58 ± 2.01 ms`
  - `decode tok/s = 280.97`
  - `E2E = 1418.24 ± 2.04 ms`

## 直接结论

1. 在这组相同绑定与相同 phase 线程数下，`work_steal` 对 Prefill 有明显提升。
2. 相比 `static`，`work_steal` 的 Prefill 时间下降约 `15.06%`，Prefill 吞吐从 `531.86 tok/s` 提升到 `626.18 tok/s`，提升约 `17.73%`。
3. 但这组配置下，Decode 反而是 `static` 更快，因此 `work_steal` 的优势主要集中在 Prefill，而不是 Decode。
4. 端到端时延仍由 `work_steal` 略优：`1418.24 -> 1375.05 ms`，下降约 `3.05%`。

## 调度统计

`work_steal` 的 5 次测量均值：

- `local_pop_success = 15540.0`
- `steal_attempts = 1337.2`
- `steal_success = 1245.6`
- `steal_empty = 91.6`
- `steal_cas_retries = 116.0`
- `stolen_tasks = 4075.2`

说明：

- Prefill 期间确实存在稳定的 stealing 行为，而不是只停留在初始静态划分。
- 这组结果可直接作为“work stealing 已真实发生”的日志证据。

## 正文建议写法

建议把这组结果写成：

> 在相同 pool、相同 phase 绑核和相同 phase 线程数下，将 Prefill 从静态划分切换为 `work_steal` 后，Prefill 时延显著下降，说明 `work_steal` 能有效改善 Prefill 阶段的任务组织与尾部拖延。但由于当前 Decode 仍使用独立的 dynamic 路径，因此该收益主要集中在 Prefill 侧，而不会自动转化为 Decode 吞吐同步提升。

## 当前局限

- 当前这组结果是在 `decode_prime` 默认开启的包上测得，后续若正文要完全隔离 Priming 影响，可以再补一轮 `--decode-prime` 关闭的对照。
- 这组结果已经足够支撑“Prefill static vs work_steal”的主结论，但若要把“尾部缓解”写得更强，还需要后续补 Perfetto 轨迹或线程结束时间分布图。
