# 子任务 C：Guided 参数敏感性

生成时间：2026-04-03

测试包：`/data/local/tmp/mnn_aecs_run_20260330_191429`

统一 workload：

- `prompt=512`
- `decode=128`
- `kv=false`
- `rep=3`
- `--split-phase-bench`
- `-t 6 -ids 2,3,4,5,6,7`
- Decode 固定：`--decode-sched-policy dynamic`

## 结果

| 配置 | Prefill (pp512, tok/s) | Decode (tg128, tok/s) |
| --- | ---: | ---: |
| `guided, static_ratio=0.02, min_chunk=32` | `619.84 ± 0.46` | `98.80 ± 0.73` |
| `guided, static_ratio=0.05, min_chunk=32` | `620.89 ± 1.05` | `98.72 ± 0.83` |
| `guided, static_ratio=0.08, min_chunk=32` | `621.23 ± 0.27` | `99.03 ± 0.24` |
| `guided, static_ratio=0.05, min_chunk=16` | `620.49 ± 0.20` | `92.29 ± 5.22` |
| `guided, static_ratio=0.05, min_chunk=64` | `584.45 ± 12.60` | `83.68 ± 1.39` |

## 当前结论

- `static_ratio=0.02-0.08` 区间对这组 case 的 Prefill 吞吐影响很小，`0.05` 可作为正文默认值继续使用。
- `min_chunk=32` 明显比 `64` 更稳，且不会像 `16` 一样把 Decode 波动放大。
- 本轮后续实验固定 `guided + static_ratio=0.05 + min_chunk=32` 作为 Prefill 主策略。
