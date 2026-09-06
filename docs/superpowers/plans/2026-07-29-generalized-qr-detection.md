# Generalized QR Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace single-ROI hard gating with bounded QR-model-driven full-image detection that decodes all 38 real samples and at least 95% of an untouched 100-image polluted-background holdout.

**Architecture:** Existing ROI, paper, and run-histogram detectors become non-authoritative candidate sources. A fixed-size detection layer searches Version 1–10 QR structure over position and module scale, retains eight distinct candidates, refines them on the original image, and lets ZXing validate at most three canonical reconstructions.

**Tech Stack:** C++11, existing ZXing core, lodepng/jpgd, Python 3 plus test-only OpenCV/Numpy for deterministic synthetic image generation.

---

## File Structure

- Modify `cli/src/QRGridNormalizer.h`: expose a bounded normalized-candidate result API.
- Modify `cli/src/QRGridNormalizer.cpp`: detection layer, multi-region proposals, module hints, global QR-model search, continuous refinement, and candidate rendering.
- Modify `cli/src/ImageReaderSource.h`: expose creation of up to three normalized luminance sources.
- Modify `cli/src/ImageReaderSource.cpp`: load once, normalize once, construct bounded candidate sources.
- Modify `cli/src/main.cpp`: decode normalized candidates in order before the one original `try-harder` fallback.
- Modify `scripts/verify_qr_repair.py`: add `print/12.png` to the real manifest.
- Create `scripts/verify_qr_generalization.py`: deterministic positive/negative generation, decode verification, and latency report.
- Modify `docs/qr-finder-repair-summary.md`: document model-driven search and final evidence.
- Modify `docs/qr-general-repair-build.md`: document real and synthetic verification commands.

Do not commit unless the user explicitly requests it.

## Task 1: Establish real and generated failing regressions

**Files:**
- Modify: `scripts/verify_qr_repair.py`
- Create: `scripts/verify_qr_generalization.py`

- [ ] **Step 1: Add the 38th real sample**

Extend the print manifest:

```python
"print/1.png": "333",
**{f"print/{index}.png": "123" for index in range(2, 13)},
```

- [ ] **Step 2: Verify the real RED case**

Run:

```bash
cmake --build build --target zxing -j4
python3 scripts/verify_qr_repair.py print/12.png
```

Expected: exit 1, `print/12.png: expected 123, got decoding failed`.

- [ ] **Step 3: Create deterministic generalization cases**

Implement `scripts/verify_qr_generalization.py` around these explicit types and defaults:

```python
@dataclasses.dataclass(frozen=True)
class Source:
    path: str
    payload: str

POSITIVE_SOURCES = (
    Source("version1/123.png", "123"),
    Source("print/4.png", "123"),
    Source("print/8.png", "123"),
    Source("version5/1.png", "version5"),
    Source("version10/1.png", "version10"),
)

BASELINE_SEED = 20260728
TUNING_SEED = 20260729
HOLDOUT_SEED = 20260730
DEFAULT_HOLDOUT_COUNT = 100
DEFAULT_NEGATIVE_COUNT = 50
```

The generator must:

1. load a source through OpenCV;
2. resize it to a bounded patch only if the resized patch itself decodes;
3. place the complete patch on a larger canvas;
4. record the patch rectangle as a protected region;
5. draw bars, squares, circles, text-like segments, gradients, noise, and mild
   blur only outside that protected rectangle;
6. invoke `build/zxing` and compare exact payload;
7. create nested-square, checker, bar, and random-line negatives without QR;
8. use `TemporaryDirectory`, leaving no generated files in the repository.

Use this CLI:

```python
parser.add_argument("--binary", default="build/zxing")
parser.add_argument("--seed", type=int, default=HOLDOUT_SEED)
parser.add_argument("--positive-count", type=int, default=DEFAULT_HOLDOUT_COUNT)
parser.add_argument("--negative-count", type=int, default=DEFAULT_NEGATIVE_COUNT)
parser.add_argument("--minimum-positive-rate", type=float, default=0.95)
parser.add_argument("--average", type=float, default=0.5)
parser.add_argument("--p95", type=float, default=0.8)
```

Fail when positive accuracy, negative false positives, average, or P95 breaches
its limit. Print the seed and aggregate counts so every run is reproducible.

- [ ] **Step 4: Verify generated RED**

Run:

```bash
python3 scripts/verify_qr_generalization.py \
  --seed 20260728 --positive-count 100 --negative-count 50
```

Expected: exit 1 because the current single-ROI implementation is below 95%
on polluted holdouts. Record the exact baseline without changing the seed.

## Task 2: Replace one ROI with bounded candidate sources

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Confirm ROI hard-gate behavior remains RED**

Run:

```bash
python3 scripts/verify_qr_repair.py print/12.png
```

Expected: fail before changing production code.

- [ ] **Step 2: Add candidate-source data structures**

Add private structures:

```cpp
struct SearchRegion {
  Bounds bounds;
  float priority;
};

struct ScaleHint {
  float moduleSize;
  float support;
};

struct DetectionLayer {
  BinaryImage image;
  float sourceScaleX;
  float sourceScaleY;
};
```

No function introduced by this task may exceed five parameters or 100 lines.

- [ ] **Step 3: Return multiple occupancy regions**

Replace `Bounds FindQRRegion(...)` with:

```cpp
std::vector<SearchRegion> FindQRRegions(const BinaryImage& image);
```

Return up to six distinct component regions sorted by generic priority. Always
append the full image as the lowest-priority region. Density, squareness, and
texture may prioritize a region but may never remove the full-image path.

Use IoU above `0.75` to merge duplicate regions. Preserve dark-paper geometry
as a separate candidate source.

- [ ] **Step 4: Build a fixed-size detection layer**

Implement:

```cpp
DetectionLayer BuildDetectionLayer(const BinaryImage& source);
PointF DetectionToSource(const DetectionLayer& layer, PointF point);
Bounds DetectionToSource(const DetectionLayer& layer, Bounds bounds);
```

Set the detection longest side to at most 320 pixels. Downsample grayscale and
opacity with area averaging, recompute the Otsu binary map and integral image,
and store exact X/Y source scale factors.

- [ ] **Step 5: Verify candidate-source behavior**

Build and run:

```bash
cmake --build build --target zxing -j4
python3 scripts/verify_qr_repair.py \
  print/9.png print/11.png print/12.png
```

Expected at this intermediate step: `9.png` and `11.png` remain `123`;
`12.png` may remain RED because model search is Task 3.

## Task 3: Add QR-model-driven global coarse search

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Confirm model-search acceptance remains RED**

Run:

```bash
python3 scripts/verify_qr_repair.py print/12.png
```

Expected: fail.

- [ ] **Step 2: Generate global and tiled module hints**

Implement:

```cpp
std::vector<ScaleHint> CollectModuleHints(
    const DetectionLayer& layer,
    const std::vector<SearchRegion>& regions);
```

Collect black-run peaks from:

- the complete detection image;
- each proposed region;
- a fixed `4x4` tile grid.

Fold supported integer multiples to their fundamental. Merge hints within
`0.35` detection pixels, retain the eight strongest, and discard sizes below
`1.25` pixels.

- [ ] **Step 3: Add sparse model scoring**

Implement:

```cpp
float CoarseModelScore(
    const BinaryImage& image,
    GridCandidate& grid,
    const std::vector<FixedModule>& fixed);

std::vector<GridCandidate> SearchQRModel(
    const DetectionLayer& layer,
    const std::vector<SearchRegion>& regions,
    const std::vector<ScaleHint>& hints);
```

For every Version 1–10 and compatible module hint:

1. reject dimensions that do not fit the detection image;
2. scan origins over the full image at `max(1, round(module/2))`;
3. scan high-priority regions first but continue through the full image;
4. sample module inner centers from the integral image;
5. score finder groups separately and combine the strongest surviving finder
   evidence with timing, alignment, dark-module, and sparse purity scores;
6. call the existing deterministic `KeepBest`, retaining eight distinct
   version/orientation/grid hypotheses.

Use early rejection after timing and finder prescores. Do not create a candidate
loop proportional to original source pixels; all scanning occurs on the
320-pixel detection layer.

- [ ] **Step 4: Map coarse candidates to source coordinates**

Implement:

```cpp
GridCandidate DetectionToSource(
    const DetectionLayer& layer,
    GridCandidate candidate);
```

Scale all four corners by the stored X/Y factors without changing version,
orientation, or score.

- [ ] **Step 5: Integrate model candidates without deleting existing sources**

In `NormalizeQR`, combine:

```cpp
coarseFromRoi
coarseFromPaper
coarseFromModel
```

through one `KeepBest` pool. No source may bypass the common distinct-candidate
ranking.

- [ ] **Step 6: Verify GREEN for the motivating real sample**

Run:

```bash
cmake --build build --target zxing -j4
python3 scripts/verify_qr_repair.py \
  print/9.png print/10.png print/11.png print/12.png
```

Expected: `9.png`, `11.png`, and `12.png` decode as `123`. `10.png` may remain
the known finder-origin failure until a separately approved origin-search
change; however the final 38/38 gate requires resolving it through the
model-driven search rather than a filename-specific rule.

## Task 4: Refine continuous grids and decode bounded Top-K candidates

**Files:**
- Modify: `cli/src/QRGridNormalizer.h`
- Modify: `cli/src/QRGridNormalizer.cpp`
- Modify: `cli/src/ImageReaderSource.h`
- Modify: `cli/src/ImageReaderSource.cpp`
- Modify: `cli/src/main.cpp`

- [ ] **Step 1: Define normalized-candidate results**

Add:

```cpp
struct NormalizedImage {
  zxing::ArrayRef<char> pixels;
  int width;
  int height;
  int components;
};

std::vector<NormalizedImage> NormalizeQRCandidates(
    const MutableImage& image, int maximumCandidates);
```

Keep `bool NormalizeQR(MutableImage&)` as a compatibility wrapper that uses the
first candidate.

- [ ] **Step 2: Strengthen continuous refinement**

Extend refinement stages from:

```cpp
const float stages[2] = {0.5f, 0.2f};
```

to:

```cpp
const float stages[3] = {0.5f, 0.2f, 0.1f};
```

At each stage test both signs from an immutable baseline for:

- X/Y translation;
- independent X/Y module scale;
- rotation for non-axis candidates;
- corner offsets when perspective evidence is present.

Use continuous inner-region sampling for all candidates. Axis candidates must
not change behavior based on an integer pixel phase or a specific whole-image
resize ratio.

- [ ] **Step 3: Render up to three distinct candidates**

After refinement and confidence checks:

```cpp
const int count = std::min(
    maximumCandidates, static_cast<int>(refined.size()));
```

Render at most `count` candidates, skipping candidates equivalent under the
existing version/orientation/sub-module rule.

- [ ] **Step 4: Construct candidate luminance sources**

Expose:

```cpp
static std::vector<zxing::Ref<LuminanceSource> >
createNormalizedCandidates(std::string const& filename, int maximumCandidates);
```

Load the file once. Convert each `NormalizedImage` into an
`ImageReaderSource`. Keep the current single-candidate factory as a compatibility
wrapper if needed by external callers.

- [ ] **Step 5: Decode candidates in bounded order**

In `main.cpp`, use:

```cpp
std::vector<Ref<LuminanceSource> > candidates =
    ImageReaderSource::createNormalizedCandidates(filename, 3);
for (size_t i = 0;
     i < candidates.size() && hybridResult != 0 && globalResult != 0;
     ++i) {
  attempt_decode(candidates[i], try_harder);
}
if (hybridResult != 0 && globalResult != 0 && !try_harder) {
  attempt_decode(source, true);
}
```

There must be no orientation reload loop and no more than three normalized
decode attempts.

- [ ] **Step 6: Verify all real samples**

Run:

```bash
cmake --build build --target zxing -j4
python3 scripts/verify_qr_repair.py
```

Expected: `38/38`, exact payloads, average at most `0.5s`, P95 at most `0.8s`.

## Task 5: Pass untouched generalization and negative gates

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`
- Modify only if a test defect exists: `scripts/verify_qr_generalization.py`

- [ ] **Step 1: Run the tuning set**

Run:

```bash
python3 scripts/verify_qr_generalization.py \
  --seed 20260729 --positive-count 100 --negative-count 50
```

Expected: exact report of positive accuracy, false positives, average, and P95.

- [ ] **Step 2: Fix only general QR-evidence defects**

If the tuning set misses the gate, classify failures by:

- no module hint;
- no coarse model candidate;
- correct coarse candidate pruned;
- refinement lost the grid;
- normalized candidate failed decode;
- negative false positive.

For each category, add a focused deterministic generated regression before
changing code. Do not add filename, fixed crop, fixed resize, or holdout-specific
conditions.

- [ ] **Step 3: Freeze constants and run untouched holdout**

Run exactly once after tuning:

```bash
python3 scripts/verify_qr_generalization.py \
  --seed 20260730 --positive-count 100 --negative-count 50
```

Expected:

```text
positive: >=95/100
negative false positives: 0/50
average: <=0.5000s
p95: <=0.8000s
```

If it fails, report the failure before inspecting individual holdout images.
Do not tune against individual holdout cases.

- [ ] **Step 4: Re-run real regression after generalization tuning**

Run:

```bash
python3 scripts/verify_qr_repair.py
```

Expected: `38/38` with both latency gates passing.

## Task 6: Final limits, review, and documentation

**Files:**
- Modify: `docs/qr-finder-repair-summary.md`
- Modify: `docs/qr-general-repair-build.md`

- [ ] **Step 1: Run fresh verification**

Run:

```bash
cmake --build build --target zxing -j4
python3 scripts/verify_qr_repair.py
python3 scripts/verify_qr_generalization.py \
  --seed 20260730 --positive-count 100 --negative-count 50
git diff --check
```

Expected: successful build, 38/38 real, holdout at least 95/100, zero negative
decodes, and both latency gates passing.

- [ ] **Step 2: Check coding constraints**

Review every changed function in:

```text
cli/src/QRGridNormalizer.cpp
cli/src/ImageReaderSource.cpp
cli/src/main.cpp
```

Expected: at most five parameters and at most 100 physical lines per function.

- [ ] **Step 3: Check diagnostics and generated files**

Run IDE diagnostics for all changed C++ and Python files. Confirm no generated
positive/negative images, `__pycache__`, or `.pyc` files remain in the
repository.

- [ ] **Step 4: Update documentation with measured evidence**

Document:

- ROI as a non-authoritative candidate source;
- fixed-size QR-model search;
- continuous source-image refinement;
- bounded Top-K decode validation;
- exact 38-image output;
- tuning/holdout seeds and measured positive/negative results;
- final average and P95.

- [ ] **Step 5: Final integrated review**

Review all task files against
`docs/superpowers/specs/2026-07-29-generalized-qr-detection-design.md`.
Do not commit unless explicitly requested.
