# QRGridNormalizer 流程、框架与算法映射

本文档整理 `cli/src/QRGridNormalizer.cpp` 的代码流程与算法框架，并在每一步标出对应的实现函数。

适用范围：QR Version 1–10；不依赖 OpenCV；在原图 ZXing 解码失败后做一次网格归一化，输出带 quiet zone 的规范模块图。

公开 API：

| API | 作用 |
|-----|------|
| `CanNormalizeQR` | 内存/尺寸安全检查（`CheckedImageSize`） |
| `NormalizeQRCandidates` | 主入口：二值化 → 粗到细搜索 → 渲染最多 3 张候选图 |
| `NormalizeQR` | 仅取最好一张，写回 `MutableImage` |

调用链：`ImageReaderSource` → 上述 API；CLI 路径仍是「原图解码失败 → 归一化再解码 → 必要时 try-harder」。

核心思想：**不修像素语义，而是估计 QR 网格几何，把固定模式写死、数据模块采样，输出 canonical 位图**。

---

## 1. 整体框架（分层）

```text
Public API
  NormalizeQRCandidates / NormalizeQR
        │
        ▼
L1 表示层
  BuildBinary / FinishBinary
  BuildDetectionLayer
  BinaryImage + integral
        │
        ▼
L2 几何先验
  FindQRRegions
  CollectModuleHints
  MeasureBorder / FindPaperCorners
  CollectModelAxes / PCA
        │
        ▼
L3 三路网格假设
  SearchQRModel          (model 扫描)
  CollectAxisCandidates  (轴对齐 + PCA)
  CollectPaperCandidates (纸张四边形)
        │
        ▼
L4 融合与精修
  FairCoarseCandidates
  RefineTop / RefineSteps
  HasAbsoluteEvidence
        │
        ▼
L5 采样与渲染
  ScoreGrid / FixedScore
  RenderNormalized
```

### 关键数据结构

| 类型 | 含义 |
|------|------|
| `BinaryImage` | 灰度、alpha、黑白掩码、积分图、Otsu 阈值 |
| `DetectionLayer` | 最长边 ≤256 的检测用缩略图 + 回源尺度 |
| `SearchRegion` | ROI 包围盒 + 优先级 |
| `ScaleHint` | 模块像素尺寸假设 + 支持度 |
| `GridCandidate` | 四角、version、orientation、score |
| `FixedModule` | 固定模块 `(x, y, black, group)` |
| `AxisSource` / `ModelAxes` | 轴对齐源 / 扫描轴方向 |
| `AffineHypothesis` | 中心 + 正交基 + 跨度（PCA/投影） |

**网格参数化**：任意模块坐标 `(moduleX, moduleY)` 由四角双线性插值得到像素位置（`GridPoint`）。  
`corner[0..3]` 约等于 TL→TR→BR→BL（可由 orientation 循环）。

---

## 2. 主流程（算法管线 ↔ 函数）

入口：

```text
NormalizeQRCandidates
  → BuildBinary
  → FindRefinedCandidates
  → RenderCandidates
```

| 步骤 | 算法意图 | 函数 |
|------|----------|------|
| 0. 可行性 | 像素/工作集上限 | `CheckedImageSize` / `CanNormalizeQR` |
| 1. 二值化 + 积分图 | 灰度、Otsu、black、summed-area | `BuildBinary` → `Luminance` / `Opacity` / `OtsuThreshold` / `FinishBinary`；矩形查询 `RegionSum` / `IntegralAt` |
| 2. 检测层 | 大图缩到长边 256，降 ROI/模型扫描成本 | `BuildDetectionLayer` + `AreaAverage`；`DetectionToSource` / 映射自检 `DetectionMappingRoundTrips` |
| 3. 边框语义 | 黑底/透明底 vs 白纸 | `MeasureBorder` → `CountBorderRing` |
| 4. ROI | 暗色连通区 → 最多 6 个 ROI + 全图 | `FindQRRegions`：`BuildDarkRoiGrid` → `FloodRoi` → `RoiScore` / `RoiTextureScore` → `AddDistinctRegion` |
| 5. 模块尺寸 hint | 黑 run 直方图峰（全图/ROI/4×4 瓦片） | `CollectModuleHints` → `AppendRunHints` → `RunHistogram` / `FoldedRunSupport` / `MergeScaleHint` |
| 6a. Model 路 | 缩略层滑窗 + finder/timing 预筛 | 见 §3.1 |
| 6b. Axis 路 | 暗内容外接框 + 可选 PCA 旋转 | 见 §3.2 |
| 6c. Paper 路 | 仅 darkCanvas | 见 §3.3 |
| 7. 融合 | 三路上各保座 + Top-8 | `FairCoarseCandidates` / `MergeCoarseCandidates` / `ReserveCandidateSource` |
| 8. 精修 | 平移/缩放/旋转/角点爬山 | `RefineTop` → `RefineCandidate` → `RefineSteps` |
| 9. 准入 | score≥0.72 绝对证据；渲染需覆盖与阈值 | `HasAbsoluteEvidence`、`RenderableCandidate`、`FootprintCovered` |
| 10. 渲染 | 固定模块写死；数据模块采样；8×8 + 4 模块 quiet | `RenderNormalized` + `SampleGridBlack` |

`FindRefinedCandidates` 顺序：

```text
MeasureBorder
  → BuildDetectionLayer
  → FindQRRegions + CollectModuleHints
  → SearchQRModel → SourceModelCandidates   // 缩略 → 原图重打分
  → FindAxisSources + CollectAxisCandidates
  → [可选] FindPaperCorners + CollectPaperCandidates
  → FairCoarseCandidates
  → RefineTop
```

---

## 3. 三路候选生成

### 3.1 Model 路径（结构扫描，最重）

**目标**：在估计的 module 尺寸与轴方向上扫描网格原点，用 sparse finder/timing 快速筛，再用固定模式 + 稀疏纯度粗打分。

| 子步骤 | 算法 | 函数 |
|--------|------|------|
| 轴集合 | 轴对齐 + 区域 PCA 旋转轴（最多 3） | `CollectModelAxes` ← `RegionAngleEvidence` ← `SampleForeground` + `EstimateOrientation` + `PcaAngleHypotheses` |
| 按角扫描 | version 1–10 × module hints | `SearchModelAngle` |
| 版本适应 | 码边不过长边 | `ModelVersionFits` |
| 原点范围 | 旋转矩形外接，留采样半径 | `ModelOriginRange` / `ModelOriginBounds` |
| 步长 | module/2，面积预算约 900/2400 点 | `ModelScanStride` |
| 原点预筛 | finder 核 + timing 稀疏一致性 | `ScoreModelOrigin` → `QuickModelPrescore` → `QuickFinder*` / `QuickTiming*` / `QuickBlack` |
| 种子合并 | 区域保留 + 全局 Top | `ReserveRegionSeeds`、`MergeModelSeeds`、`KeepModelSeed` |
| 4 朝向打分 | 仅种子通过后再做 | `ScoreSeedOrientations` → `MakeAffineGrid` + `CoarseModelScore` |
| 角序策略 | 先 angle=0；弱则 reservedCentral；仍弱再其余 | `SearchQRModel` |
| 回源重分 | 角点乘 scale，在原图 `ScoreGrid` | `SourceModelCandidates` + `DetectionToSource` |

**预筛设计要点**（`QuickModelPrescore`）：三个 finder 分开计分并排序，**单 finder 不能独占**，与最终 `FixedScore` 的 finder 权重封顶思路一致。

### 3.2 Axis 路径（轴对齐 + 条件 PCA）

| 子步骤 | 算法 | 函数 |
|--------|------|------|
| 搜索源 | 各 ROI 映射到源图暗外接框；全图作 canvas anchor | `FindAxisSources` ← `FindDarkBounds` ← `SupportedExtent` / `RegionSum` |
| 模块估计 | run 直方图基频 | `EstimateModuleSize` |
| 版本剪枝 | 期望边长 ≈ module×(17+4v) 相对 ROI 侧长 | `ScoreAxisBounds` + `CandidateVersions` |
| 候选网格 | 内容锚定 / quiet 锚定；scale/phase/orientation 离散 | `AxisCandidates` → `MakeAxisGrid` |
| 粗评 | 先粗后精，保留 Top | `ScoreCandidates` ← `CoarseModelScore` 再 `ScoreGrid` |
| 早停 | 局部领先强时跳过全图画布与 PCA | `CollectAxisCandidates` / `ScoreAxisSource`；`HasDistinctScoreLead` |
| 旋转补充 | PCA 置信高且投影明显旋转时 | `NeedsRotationSearch` → `ScorePcaBounds` → `PcaCandidates` → `MakeAffineGrid` |

**PCA 角度如何生成**：

1. `EstimateOrientation`：前景协方差主轴  
2. `PcaAngleHypotheses`：0–80° 最小包围面积 + ±3°  
3. `ProjectAtAngle`：得到 center / span  

### 3.3 Paper 路径（旋转黑底/透明底）

仅当 `border.darkCanvas` 为真。

| 子步骤 | 算法 | 函数 |
|--------|------|------|
| 纸像素 | 亮灰度或高 alpha | `IsPaperPixel` |
| 最大连通域 | 下采样网格 flood | `BuildPaperGrid` → `FloodPaper` → `LargestPaperComponent` |
| 四角 | 16 方向 support 点 C(n,4) 最大合法面积四边形 | `DirectionalSupports` → `OrderQuad` → `ValidPaperQuad` / `QuadArea`；入口 `FindPaperCorners` |
| 网格假设 | quiet∈{2,3,4,5}、4 朝向、部分角扰动 | `CollectPaperCandidates` → `RotatedCandidates` → `MakeQuadGrid` / `PerturbPaperCorner` / `QuadPoint` |

---

## 4. 打分与固定结构

### 4.1 固定模块图

`BuildFixedModules(version)`：

| 结构 | 函数 | group |
|------|------|-------|
| 3 个 finder + 白 separator | `AddFinders` / `AddFinder` / `AddSeparator` | 0, 1, 2 |
| 时序 | `AddTiming` | 3 |
| dark module | `SetFixed(8, dim-8)` | 4 |
| alignment | `AddAlignments` / `AddAlignment` | 4 |
| version bits (v≥7) | `AddVersionInformation` / `VersionBits` | 4 |

### 4.2 采样

| 场景 | 方法 | 函数 |
|------|------|------|
| 轴对齐矩形 | 积分图均值 | `SampleBlack(..., axisAligned=true)` |
| 斜/透视 | 中心邻域点 | `DirectBlack` / `ContinuousBlack`；`SampleGridBlack` |
| 粗模型快速查 | 整数矩形 `RegionSum` | `QuickBlack` |

模块中心：`GridPoint(grid, x, y)` 四角透视插值。

### 4.3 分数公式

**固定模式** `FixedScore`：

- 每 group 平均 agreement（黑期望 `black`、白期望 `1-black`）× coverage  
- finder 三组**排序后**：

```text
0.35 * best + 0.15 * 2nd + 0.05 * 3rd + 0.30 * timing + 0.15 * other
```

损坏 1–2 个 finder 仍可存活，完整 finder 不能碾压整盘。

**纯度** `GridPurityScore` / `PurityScore` / `SparsePurityScore`：

- 高对比（离 0.5 远）、黑白平衡、邻模块跳变  
- 粗搜 step=4 稀疏；精评 step=2  

**合成**：

| 级别 | 函数 | 权重 |
|------|------|------|
| 粗 | `CoarseModelScore` | 0.88 fixed + 0.12 sparse purity；coverage ≥ 0.98 |
| 精 | `ScoreGrid` | 0.80 fixed + 0.20 full purity |

不在图内或覆盖不足 → `score = -1`（`FootprintCovered`）。

### 4.4 候选保留与置信度

| 机制 | 函数 | 阈值 |
|------|------|------|
| 去重 + 限顶 | `EquivalentGrid`、`KeepLimited` / `KeepBest` | 通常 Top 8 |
| 明显领先 | `HasDistinctScoreLead` | score ≥ 0.72 且 margin ≥ 0.004 |
| 可渲染绝对证据 | `HasAbsoluteEvidence` | best ≥ 0.72 |
| 可输出 | `RenderableCandidate` | ≥ 0.68 + 完全 footprint |

---

## 5. 精修与输出

### 5.1 精修 `RefineSteps`

多阶段步长 `0.5 / 0.2 / 0.1` × `GridModuleSize`：

| 自由度 | 函数 |
|--------|------|
| 平移 | `TranslateGrid` |
| 沿边缩放 | `ScaleGrid` |
| 绕中心微旋（非轴对齐时） | `RotateGrid` |
| 角点（透视或强制） | `MoveCorner`；触发：`HasPerspectiveEvidence` / `NeedsAxisCornerRefinement` / `forceCorners` |
| 接受改进 | `PreferScored` → `ScoreGrid` |

`RefineTop`：Top-8 先不强制角；若最优仍轴对齐但方正性/分数不够，再开角精修。

### 5.2 渲染 `RenderNormalized`

1. 建 `fixedValues[dim²]`，来自 `BuildFixedModules`  
2. 数据模块：`SampleGridBlack` ≥ 0.5 → 黑  
3. 固定位：强制标准黑白（修复 finder/timing/alignment）  
4. 每模块 8×8 像素，四周围 **4 模块宽** 白 quiet zone  
5. 输出 RGBA，`NormalizedImage`  

---

## 6. 模块间调用关系

```text
NormalizeQRCandidates
 └─ BuildBinary
 └─ FindRefinedCandidates
 │   ├─ MeasureBorder
 │   ├─ BuildDetectionLayer
 │   ├─ FindQRRegions
 │   ├─ CollectModuleHints
 │   ├─ SearchQRModel
 │   │   ├─ CollectModelAxes
 │   │   ├─ SearchModelAngle
 │   │   ├─ QuickModelPrescore / CoarseModelScore
 │   │   └─ SourceModelCandidates (ScoreGrid @ source)
 │   ├─ FindAxisSources
 │   ├─ CollectAxisCandidates
 │   │   └─ ScoreAxisSource
 │   │       ├─ ScoreAxisBounds
 │   │       └─ ScorePcaBounds (条件)
 │   ├─ FindPaperCorners? ── CollectPaperCandidates
 │   ├─ FairCoarseCandidates
 │   └─ RefineTop ── RefineCandidate ── RefineSteps
 └─ RenderCandidates ── RenderNormalized
```

---

## 7. 算法特性一览

1. **粗到细 / 多假设融合**：Model（结构扫）+ Axis（矩形）+ Paper（四边形）→ 公平占坑 → 局部爬山。  
2. **检测与评分分辨率分离**：滑窗在 ≤256 层；最终分与渲染在全分辨率。  
3. **PCA 是 gate，不是真理**：低置信跳过旋转；轴对齐已够强则省 PCA。  
4. **稀疏固定模式优先**：finder 分权 + timing + purity，适配局部污损。  
5. **确定性、有界搜索**：步长预算、hint Top-3、候选上限、分数门槛；无输出给 ZXing。  
6. **能力边界**：仅 V1–10；不做曲面扭曲；超过 Reed-Solomon 能力的数据损坏不保证恢复。  

---

## 8. 按职责索引（便于读源码）

| 职责 | 主要函数（约行号） |
|------|--------------------|
| 尺寸安全 | `CheckedImageSize` ~199 |
| 二值化/积分 | `BuildBinary` / `FinishBinary` / `OtsuThreshold` ~287–376 |
| 检测层 | `BuildDetectionLayer` ~416 |
| 固定模式 | `BuildFixedModules` 族 ~557–687 |
| ROI | `FindQRRegions` ~716–908 |
| 暗边界 | `FindDarkBounds` ~910 |
| 朝向/PCA | `EstimateOrientation` / `PcaAngleHypotheses` ~978–1074 |
| 纸张 | `MeasureBorder` / `FindPaperCorners` ~1206–1446 |
| 模块 hint | `CollectModuleHints` / `EstimateModuleSize` ~1448–1621 |
| 采样/打分 | `SampleGridBlack` / `FixedScore` / `ScoreGrid` ~1697–1952 |
| 候选生成 | `AxisCandidates` / `PcaCandidates` / `RotatedCandidates` ~1954–2130 |
| 模型扫 | `QuickModelPrescore` / `SearchQRModel` ~2212–2699 |
| 融合/精修/渲染 | `FairCoarseCandidates` / `Refine*` / `Render*` / `FindRefinedCandidates` ~2722–3262 |
| 导出 API | 文末 ~3266–3299 |

---

## 9. 相关设计文档

- `docs/superpowers/specs/2026-07-25-qr-grid-normalization-design.md` — 归一化设计目标与管线约束  
- `docs/superpowers/plans/2026-07-25-qr-grid-normalization.md` — 实现计划  
- `docs/qr-general-repair-build.md` — 构建与回归验证  

调试环境变量（实现内）：

- `ZXING_QR_STAGE_MS`：打印各阶段耗时  
- `ZXING_QR_SEARCH_STATS`：打印 model 搜索与候选统计  
