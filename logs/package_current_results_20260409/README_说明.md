# 当前可用数据说明

生成时间：2026-04-09
工程目录：`/home/hefeng/MNN_last/MNN_Perfetto_hybrid_stepwise`

## 1. 这份归档现在表示什么

这份归档已经按当前代码状态重新筛过一遍。

当前前提是：Prefill 调度策略已经切换为 `work_steal`。因此，凡是依赖旧 `guided/dynamic/hybrid` Prefill 行为测出来的优化包结果，都不再视为“当前可用数据”。

这份归档现在只保留：
- 仍可用于论文撰写的基线类结果；
- 一份新的 `work_steal` 复测清单；
- 不再混入旧优化包的性能结论。

## 2. 当前可直接用于论文的数据

### 2.1 baseline 线程数与绑核锚点

文件：`docs/subtaskB_baseline_anchor_20260403.md`

当前仍可直接使用的 baseline 结论（以 `run_perfetto_batch_base.sh` 重测结果为准）：
- `baseline_clean` 的 Prefill 峰值在 `6` 线程附近。
- `baseline_clean` 的 Decode 最优点在 `5` 线程附近。
- “默认调度无 taskset”和 `taskset F0` 不是同一口径，不能混写成“不绑核”。

如果你现在要先写论文，这一部分可以直接作为“基线性能与实验口径”章节内容。

## 3. 当前不能再直接使用的数据

以下类型的结果已经不再纳入当前可用归档：
- 旧 Prefill 策略对比结果
- 旧 Guided 参数敏感性结果
- 旧阶段解耦正式结果
- 旧 Perfetto / claim / 负载分布分析结果
- 旧 AECS 搜索、选核与吞吐结果

原因统一为：这些结果建立在旧 Prefill 实现之上，而当前实现已经切到 `work_steal`。

## 4. 目前需要重测的内容

详见：`docs/work_steal_remeasure_checklist_20260409.md`

简要分为五类：
1. Prefill 主结果图表
2. Prefill 参数敏感性
3. Decode 与阶段解耦实验
4. Perfetto / 机制分析图
5. AECS 全部实验

## 5. 本归档当前包含的文件

- `docs/subtaskB_baseline_anchor_20260403.md`
  - 当前仍可直接引用的 baseline 锚点结果
- `docs/work_steal_remeasure_checklist_20260409.md`
  - 当前版本必须重测的实验清单

## 6. 说明

- 本归档已经清除“当前用不了的数据和结果”。
- 被移除的只是归档副本，不是工程原始 `logs/` 目录里的历史文件。
- 如果后续完成 `work_steal` 版本复测，应以新结果重新生成一份可用归档。
