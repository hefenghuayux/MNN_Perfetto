# 子任务 C：Prefill 三策略代表性对比

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

| 策略 | Prefill (pp512, tok/s) | Decode (tg128, tok/s) |
| --- | ---: | ---: |
| `prefill=dynamic`, `pc=240`, `pm=32` | `621.81 ± 1.26` | `99.92 ± 0.17` |
| `prefill=hybrid`, `static_ratio=0.05`, `pm=32` | `585.14 ± 0.77` | `98.97 ± 0.82` |
| `prefill=guided`, `static_ratio=0.05`, `pm=32` | `622.17 ± 1.12` | `97.63 ± 0.32` |

## 当前结论

- 在当前主优化包和这组代表性 case 下，`guided` 与 `dynamic` 的 Prefill 吞吐基本持平，`guided` 略高。
- 当前参数下的 `hybrid` 明显落后于 `dynamic/guided`，不适合作为正文主结果参数。
- 这说明后续参数敏感性应优先围绕 `guided` 展开，而不是继续在 `hybrid` 上扩大搜索。
