# 子任务 D：阶段解耦正式结果

生成时间：2026-04-03

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
| `统一 6/6 + 统一 ids=2,3,4,5,6,7` | `616.18 ± 0.14` | `98.68 ± 0.24` |
| `Prefill 6 / Decode 5 + 统一 ids=2,3,4,5,6,7` | `615.02 ± 0.64` | `100.80 ± 0.27` |
| `Prefill 6 / Decode 5 + Prefill ids=2,3,4,5,6,7 / Decode ids=3,4,5,6,7` | `615.09 ± 0.57` | `101.07 ± 0.53` |

## 当前结论

- 只调整 Decode 线程数，从 `6` 改为 `5`，即可在几乎不损失 Prefill 的情况下把 Decode 提升约 `2.12 tok/s`。
- 在此基础上再加独立 Decode affinity，Decode 还有小幅提升，但增益明显小于“先把 Decode 线程数从 6 降到 5”。
- 这说明第 4.3.2 小节可以把“独立线程数”写成主收益来源，把“独立绑核”写成次级增益。
