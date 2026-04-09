# work_steal 版本复测清单

生成时间：2026-04-09
适用前提：Prefill 调度策略已经从原先的 `guided/dynamic/hybrid` 实现切换到 `work_steal`。

## 一、仍然可用的数据

以下结果仍可用于论文，原因是它们基于 `baseline` 口径，且已按 `run_perfetto_batch_base.sh` 重测，不依赖当前优化包中的 Prefill 调度实现：

1. 基线线程数与绑核锚点表
   - 文件：`subtaskB_baseline_anchor_20260403.md`
   - 可保留结论：
     - `baseline_clean` 的 Prefill 峰值仍在 `6` 线程附近。
     - `baseline_clean` 的 Decode 最优点在 `5` 线程附近。
2. “4线程不绑核”口径澄清
   - 可保留结论：`taskset F0` 不能等同于“默认调度无 taskset”。

## 二、必须重测的数据与图表

### 1. Prefill 主结果图表

全部重测：
- 原始 MNN / 优化策略对比表
- Prefill 吞吐主结果图
- Prefill 长尾与负载均衡相关结论

原因：Prefill 调度实现已换成 `work_steal`，旧的 `guided/dynamic/hybrid` 性能对比已不再代表当前版本。

### 2. Prefill 参数敏感性

全部重测：
- `static_ratio`
- `dynamic_blocks`
- `min_chunk`
- 任何基于 `guided` 或 `hybrid` 的参数扫描

原因：这些参数结论建立在旧 Prefill 实现上，当前实现变为 `work_steal` 后，参数空间和最优点可能已经改变，部分参数甚至不再有原先含义。

### 3. Decode 与阶段解耦实验

建议全部重测：
- Decode 线程数扫描
- Decode 独立绑核扫描
- 统一配置 vs 阶段解耦表
- Prefill/Decode 联合吞吐结论

原因：即使 Decode 策略本身没改，联合吞吐实验中的 Prefill 端已经变化，原结论中的“几乎不损失 Prefill”“总吞吐权衡关系”都需要重新验证。

### 4. Perfetto / 机制分析图

全部重抓：
- Prefill 负载分布图
- claim/retry 开销图
- 代表性 trace
- 任何 `focus_claim_20260407`、`sweep_prefill_instrument_20260407`、`guided_op_dist_20260407` 生成的图表

原因：这些图直接刻画旧调度器行为，换成 `work_steal` 后已经不再对应当前实现。

### 5. AECS 相关实验

全部重测：
- AECS 前置静态标定
- AECS 搜索成本
- AECS 复现一致性
- AECS 最终选核与正式吞吐

原因：AECS 的 Prefill 评估、静态标定和最终候选选择都依赖当前 Prefill 性能面，旧 AECS 结果不能迁移到 `work_steal` 版本。

## 三、目前归档中已剔除的数据

以下内容已经从“当前可用结果归档”中移除：
- 所有 `guided/dynamic/hybrid` Prefill 扫描目录
- 旧 AECS 中间记录
- 旧 trace / notrace verify / compare 系列优化包日志
- 旧阶段解耦正式结论

这些文件原始副本仍保留在工程 `logs/` 目录，仅不再作为“当前可用数据包”的组成部分。

## 四、当前写论文时的使用原则

1. 正文正式数值，当前只引用基于 `baseline` 的结果。
2. 所有依赖旧优化包 Prefill 调度实现的结果，先不写入正文结论。
3. 如果需要写当前优化版，只能等 `work_steal` 版本的新实验完成后再回填。
