# 子任务 D：阶段解耦预结果（热态，仅供参考）

生成时间：2026-04-03

说明：

- 本轮结果采集时手机已连续跑多轮实验。
- 采集后检查到多个热区约 `43-46.5C`，CPU 当前频率偏低。
- 因此本文件只保留趋势，不作为正文正式结果。

测试包：`/data/local/tmp/mnn_aecs_run_20260330_191429`

统一 workload：

- `prompt=512`
- `decode=128`
- `kv=false`
- `rep=3`
- `--split-phase-bench`
- Prefill 固定：`guided + static_ratio=0.05 + min_chunk=32`
- Decode 固定：`dynamic`

## 结果

| 配置 | Prefill (pp512, tok/s) | Decode (tg128, tok/s) |
| --- | ---: | ---: |
| `统一 6/6 + 统一 ids=2,3,4,5,6,7` | `548.73 ± 0.85` | `80.10 ± 0.27` |
| `Prefill 6 / Decode 5 + 统一 ids=2,3,4,5,6,7` | `550.05 ± 0.84` | `82.17 ± 0.38` |
| `Prefill 6 / Decode 5 + Prefill ids=2,3,4,5,6,7 / Decode ids=3,4,5,6,7` | `549.68 ± 1.65` | `81.66 ± 0.27` |

## 仅可保留的趋势

- 在热态下，仅把 Decode 线程数从 `6` 降到 `5` 仍然带来小幅收益。
- 再叠加独立 Decode affinity，在这轮热态数据里没有继续拉开明显差距。
- 正式结果必须在手机降温后重测。
