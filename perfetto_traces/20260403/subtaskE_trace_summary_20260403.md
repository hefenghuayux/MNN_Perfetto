# 子任务 E：Perfetto 聚合统计

生成时间：2026-04-03

## unified_t6_ids_234567

### prefill

| category | count | total_duration_ms |
| --- | ---: | ---: |
| MainThread_Work | 2752 | 1358.916 |
| Worker_Work | 13272 | 7433.984 |
| MainThread_Wait | 2752 | 312.285 |
| Worker_IdleSpin | 235341 | 759.193 |

### decode

| category | count | total_duration_ms |
| --- | ---: | ---: |
| MainThread_Work | 100104 | 2490.267 |
| Worker_Work | 462336 | 12995.947 |
| MainThread_Wait | 100104 | 1350.299 |
| Worker_IdleSpin | 2119063 | 9034.139 |

## split_t6_d5_prefill_234567_decode_34567

### prefill

| category | count | total_duration_ms |
| --- | ---: | ---: |
| MainThread_Work | 2752 | 1358.467 |
| Worker_Work | 13272 | 7439.828 |
| MainThread_Wait | 2752 | 313.367 |
| Worker_IdleSpin | 233090 | 770.803 |

### decode

| category | count | total_duration_ms |
| --- | ---: | ---: |
| MainThread_Work | 100104 | 2529.616 |
| Worker_Work | 375132 | 9940.368 |
| MainThread_Wait | 100104 | 907.428 |
| Worker_IdleSpin | 1674200 | 6313.979 |

## 关键结论

- decode 阶段 `MainThread_Wait` 由 1350.299 ms 降到 907.428 ms，减少 442.871 ms（32.80%）。
- decode 阶段 `Worker_IdleSpin` 由 9034.139 ms 降到 6313.979 ms，减少 2720.160 ms（30.11%）。
- decode 阶段 `MainThread_Work` 从 2490.267 ms 增至 2529.616 ms，增加 39.349 ms。
- decode 阶段 `Worker_Work` 从 12995.947 ms 变为 9940.368 ms，变化 -3055.579 ms。
- 因 trace 打点会显著拉低 decode 吞吐，这些数字只用于机制分析，不作为正式性能表数据。
