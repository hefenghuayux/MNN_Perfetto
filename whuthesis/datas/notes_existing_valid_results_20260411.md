# 当前 `work_steal` 版本已可直接引用的结果

## 可直接写入正文

### 1. baseline 锚点表

- 文件：`tab_baseline_anchor_20260411.tex`
- 来源：`logs/subtaskB_baseline_anchor_20260403.md`
- 使用方式：
  - 作为 baseline 章节或对照表的锚点
  - 支撑“baseline 下 Prefill 更偏向 6 线程、Decode 更偏向 5 线程”的初步观察

### 2. 当前 `work_steal` 版本的绑核 vs 不绑核

- 文件：
  - `ws_pin_vs_unpin_20260411.tsv`
  - `tab_ws_pin_vs_unpin_20260411.tex`
- 可支撑结论：
  - 绑核收益依赖线程数
  - 4 线程时收益最显著
  - 5/6 线程时收益减弱，不能写成“绑核绝对优于不绑核”

### 3. 当前 `work_steal` 版本的全局线程数扫描

- 文件：
  - `ws_thread_sweep_global_20260411.tsv`
  - `tab_ws_thread_sweep_global_20260411.tex`
- 可支撑结论：
  - 在固定绑核 `2,3,4,5,6,7` 下，Prefill 随线程数增加持续改善
  - 当前观测点中，Prefill 最优线程数至少不低于 6
  - Decode 在 5-6 线程间已接近平台区

### 4. Prefill static vs `work_steal` 主对比

- 文件：
  - `static_vs_worksteal_20260411.tsv`
  - `static_vs_worksteal_stats_20260411.tsv`
  - `tab_static_vs_worksteal_20260411.tex`
  - `static_vs_worksteal_notes_20260411.md`
- 当前可支撑结论：
  - 在相同 pool、相同 phase 绑核、相同 phase 线程数下，`work_steal` 的 Prefill 明显优于 `static`
  - 当前这组配置中，收益主要集中在 Prefill 侧，Decode 不会自动同步提升
  - 端到端仍由 `work_steal` 略优

## 当前只能过渡引用、不能当作最终完成结果的部分

### 1. Prefill phase 独立线程扫描

- 当前包 `work_steal_decode_blocks_current` 在 `-pt 2`、`-pt 3` 时会触发 `Segmentation fault`
- 因此，现阶段只能引用“固定绑核 + 全局线程数扫描”结果
- 不能写成“Prefill phase 独立线程扫描已完整完成”

### 2. TTFT

- 第4章计划保留 TTFT
- 但当前已迁移到 `datas` 的现成结果文件里，统一主表尚未包含 TTFT 一列
- 后续实验补采 TTFT 后，再统一更新正式正文表

### 3. 真实性能比 vs 经验比

- 旧 guided/hybrid 结果不能直接迁移到当前 `work_steal` 版本
- 必须以后续 `work_steal` 版本的标定和对照实验为准

### 4. 高强度“尾部缓解”证据仍待补齐

- 当前已经有 `static vs work_steal` 的主对比表
- 但如果正文要把“尾部缓解”写得更强，仍建议后续补 Perfetto 轨迹或线程结束时间分布图

## 后续需要用户参与的子任务

### 必须你参与

1. 严格 Prefill phase 独立线程扫描  
原因：`-pt 2/3` 当前崩溃，需要重新打包或修复后重测。

2. 若后续“分阶段绑核”“经验比 vs 实测性能比”“static vs work_steal”缺少可执行手机包  
原因：需要你参与重新打包对应对照包。

### 当前不需要你参与

1. `datas` 目录下的结果迁移与表格整理
2. 基于现有有效结果的正文表与说明文档生成
3. 后续不依赖新打包包体的结果提炼与文字改写

## 对正文写法的建议

- baseline 结论和当前 `work_steal` 结论要严格分开写，避免混口径。
- 当前 `work_steal` 版本关于“绑核有效”的结论，应写成“低线程数下收益明显，高线程数下收益减弱”。
- 当前 Prefill 线程数实验只能先写成“Prefill 最优线程数至少不低于 6”，不要写成“phase 独立扫描已完整证明 6 线程最优”。
