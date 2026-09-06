# QR Grid Normalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode all 34 QR samples exactly while keeping average latency at or below 0.5 seconds and P95 at or below 0.8 seconds.

**Architecture:** Keep ZXing's ordinary decode as the fast path. On failure, run one Version 1–10 coarse-to-fine grid normalizer and decode its single best canonical image; if normalization confidence is insufficient, make exactly one `try-harder` attempt on the original image.

**Tech Stack:** C++11, existing ZXing core, lodepng/jpgd, Python 3 standard library for regression and latency measurement, CMake.

---

## File Structure

- Create `cli/src/QRGridNormalizer.h`: minimal mutable-image input and normalization API.
- Create `cli/src/QRGridNormalizer.cpp`: thresholding, geometry, fixed structures, candidate search, refinement, and rendering.
- Modify `cli/src/ImageReaderSource.h`: replace orientation-specific repair overload with one normalized-image factory.
- Modify `cli/src/ImageReaderSource.cpp`: retain image loading; delegate repair to `QRGridNormalizer` and remove embedded repair/search code.
- Modify `cli/src/main.cpp`: bounded normal → normalized → optional original `try-harder` flow.
- Create `scripts/verify_qr_repair.py`: exact 34-image manifest and latency assertions.
- Modify `scripts/verify_qr_damage_samples.sh`: compatibility wrapper for the new verifier.
- Modify `docs/qr-finder-repair-summary.md`: describe the final bounded pipeline and measured results.
- Modify `docs/qr-general-repair-build.md`: update build and verification commands.

## Task 1: Establish the failing correctness and latency regression

**Files:**
- Create: `scripts/verify_qr_repair.py`
- Modify: `scripts/verify_qr_damage_samples.sh`

- [ ] **Step 1: Add the exact image manifest and verifier**

Create a Python 3 runner with this manifest and behavior:

```python
#!/usr/bin/env python3
import argparse
import math
import pathlib
import subprocess
import sys
import time

SAMPLES = {
    **{f"version1/123{suffix}.png": "123"
       for suffix in ["", "_destroy1", "_destroy2", "_destroy3", "_destroy4",
                      "_destroy5", "_destroy6", "_destroy7", "_destroy8"]},
    **{f"version5/{index}.png": "version5" for index in range(1, 6)},
    **{f"version10/{index}.png": "version10" for index in range(1, 5)},
    "print/1.png": "333",
    **{f"print/{index}.png": "123" for index in range(2, 9)},
    **{f"Rotated/123_rotated{index}.png": "123" for index in range(1, 5)},
    "Rotated/rotated5.png": "version5",
    "Rotated/rotated6.png": "version5",
    "Rotated/rotated7.png": "version10",
    "Rotated/rotated8.png": "version10",
}

def decoded_text(stdout):
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    return lines[-1] if lines else ""

def percentile95(values):
    return sorted(values)[max(0, math.ceil(len(values) * 0.95) - 1)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/zxing")
    parser.add_argument("--average", type=float, default=0.5)
    parser.add_argument("--p95", type=float, default=0.8)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("samples", nargs="*")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    selected = args.samples or list(SAMPLES)
    durations, failures = [], []
    for relative in selected:
        started = time.perf_counter()
        try:
            run = subprocess.run(
                [str(root / args.binary), str(root / "qr_fig" / relative)],
                text=True, capture_output=True, timeout=args.timeout)
            actual = decoded_text(run.stdout)
        except subprocess.TimeoutExpired:
            actual = "<timeout>"
        elapsed = time.perf_counter() - started
        durations.append(elapsed)
        expected = SAMPLES[relative]
        print(f"{relative}: {actual or '<empty>'} ({elapsed:.3f}s)")
        if actual != expected:
            failures.append(f"{relative}: expected {expected}, got {actual or '<empty>'}")
    average = sum(durations) / len(durations)
    p95 = percentile95(durations)
    print(f"summary: {len(selected) - len(failures)}/{len(selected)}, "
          f"average={average:.3f}s, p95={p95:.3f}s")
    failures += [] if average <= args.average else [f"average {average:.3f}s > {args.average:.3f}s"]
    failures += [] if p95 <= args.p95 else [f"p95 {p95:.3f}s > {args.p95:.3f}s"]
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Keep the shell entry point**

Replace the stale root-level sample list with:

```bash
#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "${repo_root}/scripts/verify_qr_repair.py" "$@"
```

- [ ] **Step 3: Verify RED**

Run:

```bash
python3 scripts/verify_qr_repair.py \
  version1/123_destroy5.png version1/123_destroy6.png
```

Expected: non-zero exit; both payload checks fail or exceed the latency limit.

- [ ] **Step 4: Record the complete baseline**

Run:

```bash
python3 scripts/verify_qr_repair.py --timeout 2
```

Expected: non-zero exit with current correctness failures/timeouts and latency above the accepted thresholds.

## Task 2: Introduce the normalizer boundary and bounded fallback flow

**Files:**
- Create: `cli/src/QRGridNormalizer.h`
- Create: `cli/src/QRGridNormalizer.cpp`
- Modify: `cli/src/ImageReaderSource.h`
- Modify: `cli/src/ImageReaderSource.cpp`
- Modify: `cli/src/main.cpp`

- [ ] **Step 1: Define the wished-for normalization API**

Create:

```cpp
#ifndef ZXING_QR_GRID_NORMALIZER_H
#define ZXING_QR_GRID_NORMALIZER_H

#include <zxing/common/Array.h>

namespace qrrepair {

struct MutableImage {
  zxing::ArrayRef<char> pixels;
  int width;
  int height;
  int components;
};

bool NormalizeQR(MutableImage& image);

}

#endif
```

- [ ] **Step 2: Verify the new API is not implemented**

Temporarily include the header from `ImageReaderSource.cpp`, call
`qrrepair::NormalizeQR`, and run:

```bash
cmake --build build -j4
```

Expected: link failure for undefined `qrrepair::NormalizeQR`.

- [ ] **Step 3: Add the minimal implementation boundary**

Create `QRGridNormalizer.cpp` with the namespace, private data types, and a
temporary `false` result:

```cpp
#include "QRGridNormalizer.h"

namespace qrrepair {

bool NormalizeQR(MutableImage& image) {
  return image.pixels && image.width > 0 && image.height > 0 &&
      image.components > 0 ? false : false;
}

}
```

- [ ] **Step 4: Replace orientation-specific image factories**

Expose only:

```cpp
static zxing::Ref<LuminanceSource> create(std::string const& filename);
static zxing::Ref<LuminanceSource> createNormalized(
    std::string const& filename, bool& normalized);
```

Move shared PNG/JPEG loading into a private factory helper represented by a
small `LoadedImage` struct. `createNormalized` calls `NormalizeQR`, copies the
possibly replaced pixels/size back, and sets `normalized` to its return value.
Delete `forcedDeskewOrientation` and all embedded normalization/finder-repair
helpers from `ImageReaderSource.cpp`.

- [ ] **Step 5: Implement the bounded CLI state machine**

Keep the original source alive and use:

```cpp
attempt_decode(source, false);
if (hresult != 0 && gresult != 0) {
  bool normalized = false;
  Ref<LuminanceSource> repaired =
      ImageReaderSource::createNormalized(filename, normalized);
  if (normalized) {
    attempt_decode(repaired, false);
  } else {
    attempt_decode(source, true);
  }
}
```

Change the decode lambda to accept the source explicitly and remove the current
orientation loop. Remove the `read_image` catch block that silently retries
without `try-harder`; each call must perform exactly the requested hint mode.

- [ ] **Step 6: Build and verify bounded failure**

Run:

```bash
cmake --build build -j4
python3 scripts/verify_qr_repair.py --timeout 2 version1/123_destroy5.png
```

Expected: build passes; the image still fails (normalizer is intentionally
empty), but terminates within the verifier timeout instead of entering repeated
orientation searches.

## Task 3: Add one-pass binary preprocessing and deterministic QR structures

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Use the focused image regression as the failing test**

Run:

```bash
python3 scripts/verify_qr_repair.py --timeout 2 \
  version1/123_destroy5.png version5/2.png version10/2.png
```

Expected: RED because the normalizer still returns `false`.

- [ ] **Step 2: Add compact private image types**

Add:

```cpp
struct PointF { float x, y; };
struct BinaryImage {
  int width, height;
  unsigned char threshold;
  std::vector<unsigned char> gray;
  std::vector<unsigned char> black;
  std::vector<int> integral;
};
struct Bounds { int left, top, right, bottom; };
struct FixedModule { short x, y; unsigned char black, group; };
struct GridCandidate {
  PointF corner[4];
  float score;
  int version;
  int orientation;
};
```

No helper may take more than five arguments. Image dimensions and buffers travel
inside these structures.

- [ ] **Step 3: Implement grayscale, Otsu, and summed-area preprocessing**

Implement these complete units:

```cpp
unsigned char Luminance(const MutableImage& image, int x, int y);
unsigned char OtsuThreshold(const std::vector<unsigned char>& gray);
BinaryImage BuildBinary(const MutableImage& image);
int RegionSum(const BinaryImage& image, Bounds region);
```

`BuildBinary` makes one grayscale pass, computes the 256-bin Otsu threshold,
fills `black`, and then fills a `(width + 1) * (height + 1)` integral buffer.
Clamp the chosen threshold to `[48, 208]` to avoid classifying an almost uniform
canvas as QR content.

- [ ] **Step 4: Build Version 1–10 fixed modules**

Implement:

```cpp
void SetFixed(std::vector<FixedModule>& modules, int x, int y,
              bool black, unsigned char group);
int VersionBits(int version);
std::vector<FixedModule> BuildFixedModules(int version);
```

`BuildFixedModules` adds three 7×7 finders, one-module white separators, timing
rows/columns, the dark module, non-overlapping alignment patterns from ZXing's
`Version::getAlignmentPatternCenters`, and both 18-bit version-information
areas for Versions 7–10. `VersionBits` computes BCH remainder using generator
`0x1f25`. Use groups `0..2` for finders, `3` for timing, and `4` for all other
deterministic structures.

- [ ] **Step 5: Keep functions within the limits**

Run a source check:

```bash
python3 - <<'PY'
from pathlib import Path
text = Path("cli/src/QRGridNormalizer.cpp").read_text().splitlines()
assert not any("TODO" in line or "TBD" in line for line in text)
print(f"{len(text)} source lines ready for function-level review")
PY
```

Expected: pass. Manually split any implementation function approaching 100
lines before continuing.

## Task 4: Implement fast axis-aligned coarse-to-fine reconstruction

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Confirm the axis-aligned failures**

Run:

```bash
python3 scripts/verify_qr_repair.py --timeout 2 \
  version1/123_destroy5.png version1/123_destroy6.png \
  version5/2.png version10/4.png
```

Expected: RED.

- [ ] **Step 2: Add content bounds and module-size estimation**

Implement:

```cpp
Bounds FindDarkBounds(const BinaryImage& image);
std::vector<int> RunHistogram(const BinaryImage& image, Bounds bounds);
float EstimateModuleSize(const BinaryImage& image, Bounds bounds);
std::vector<int> CandidateVersions(float moduleSize, Bounds bounds);
```

Count horizontal and vertical black runs inside the content bounds, fold strong
integer-multiple peaks back to their smallest supported fundamental, and rank
Versions 1–10 by agreement between run size and `max(span)/(17 + 4*version)`.
Keep at most three versions when the estimate is reliable; retain all ten only
when no run peak has enough support.

- [ ] **Step 3: Add constant-cost grid sampling and scoring**

Implement:

```cpp
PointF GridPoint(const GridCandidate& grid, float x, float y);
float SampleBlack(const BinaryImage& image, PointF point, float radius);
float FixedScore(const BinaryImage& image, const GridCandidate& grid,
                 const std::vector<FixedModule>& fixed);
float PurityScore(const BinaryImage& image, const GridCandidate& grid);
float ScoreGrid(const BinaryImage& image, GridCandidate& grid,
                const std::vector<FixedModule>& fixed);
```

`FixedScore` computes finder groups separately, uses the best and second-best
finder scores, and combines them with timing/other structures. `PurityScore`
samples module centers on a stride of two. The final score is 80% deterministic
structure and 20% purity. Sampling uses the integral image for axis-aligned
regions and five direct grayscale samples for rotated regions.

- [ ] **Step 4: Generate bounded candidates**

Implement:

```cpp
std::vector<GridCandidate> AxisCandidates(
    const BinaryImage& image, Bounds bounds, int version, float moduleSize);
void KeepBest(std::vector<GridCandidate>& best, GridCandidate candidate);
GridCandidate RefineCandidate(
    const BinaryImage& image, GridCandidate candidate,
    const std::vector<FixedModule>& fixed);
```

Generate anchors from both content bounds and the full canvas with four-module
quiet-zone assumptions. Search origin offsets in module increments only around
those anchors, three scale values (`m - 0.5`, `m`, `m + 0.5`), and four QR
orientations. Keep eight candidates globally. Refine each with coordinate
descent steps of 0.5 and 0.2 module over origin and scale. Candidate counts must
be fixed and independent of pixel dimensions.

- [ ] **Step 5: Render the canonical module image**

Implement:

```cpp
zxing::ArrayRef<char> RenderNormalized(
    const BinaryImage& image, const GridCandidate& grid,
    const std::vector<FixedModule>& fixed, int& outputSize);
```

Use an 8-pixel module scale and four-module quiet zone. Force every
deterministic module from `BuildFixedModules`; sample all remaining modules by
inner-region majority. Return RGBA with opaque alpha.

- [ ] **Step 6: Complete the axis-aligned NormalizeQR path**

`NormalizeQR` preprocesses once, evaluates the shortlisted versions, refines the
top candidates, requires both a minimum score and a best-versus-runner-up
margin, renders once, and replaces `MutableImage` only on success.

- [ ] **Step 7: Verify GREEN for axis-aligned images**

Run:

```bash
cmake --build build -j4
python3 scripts/verify_qr_repair.py --timeout 2 \
  version1/123_destroy5.png version1/123_destroy6.png \
  version5/2.png version10/4.png
```

Expected: 4/4 exact payloads and both latency thresholds pass.

## Task 5: Add analytical rotation and mild-perspective geometry

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Confirm rotated failures**

Run:

```bash
python3 scripts/verify_qr_repair.py --timeout 2 \
  Rotated/123_rotated1.png Rotated/123_rotated4.png \
  Rotated/rotated5.png Rotated/rotated8.png
```

Expected: RED for at least the dark-canvas rotated samples.

- [ ] **Step 2: Add the PCA rotation gate**

Implement:

```cpp
struct Orientation { PointF center, axisX, axisY; float confidence; };
Orientation EstimateOrientation(const BinaryImage& image, Bounds bounds);
bool NeedsRotationSearch(const Orientation& orientation);
```

Compute centroid and covariance in one pass over foreground samples. Confidence
is the normalized eigenvalue difference. Treat PCA as a gate only; do not use
its angle when confidence is low.

- [ ] **Step 3: Find dark-canvas paper corners analytically**

Implement:

```cpp
bool HasDarkBorder(const BinaryImage& image);
bool FindPaperCorners(const BinaryImage& image, PointF corners[4]);
std::vector<GridCandidate> RotatedCandidates(
    const BinaryImage& image, const PointF paper[4], int version);
```

For dark-border inputs, collect bright paper pixels and obtain four corners from
the extrema of `x+y` and `x-y`. Build QR corners by bilinearly insetting the
paper quadrilateral by approximately four modules. Search only a fixed set of
insets (`2..6` modules), small corner perturbations, and four orientations.
This replaces the current 180-degree scan over every foreground point.

- [ ] **Step 4: Refine four corners**

Extend `RefineCandidate` with bounded coordinate descent over the four corners.
Use coarse ±0.5-module then fine ±0.2-module adjustments, retaining a change
only when `ScoreGrid` improves. This accounts for the photographed samples'
mild perspective without OpenCV.

- [ ] **Step 5: Verify GREEN for rotated and photographed images**

Run:

```bash
cmake --build build -j4
python3 scripts/verify_qr_repair.py --timeout 2 \
  Rotated/123_rotated1.png Rotated/123_rotated2.png \
  Rotated/123_rotated3.png Rotated/123_rotated4.png \
  Rotated/rotated5.png Rotated/rotated6.png \
  Rotated/rotated7.png Rotated/rotated8.png \
  print/1.png print/4.png print/8.png
```

Expected: 11/11 exact payloads and both latency thresholds pass.

## Task 6: Complete validation, limit checks, and documentation

**Files:**
- Modify: `docs/qr-finder-repair-summary.md`
- Modify: `docs/qr-general-repair-build.md`

- [ ] **Step 1: Run the complete correctness/performance gate**

Run:

```bash
python3 scripts/verify_qr_repair.py
```

Expected: `34/34`, average at most `0.500s`, P95 at most `0.800s`.

- [ ] **Step 2: Run build and available project tests**

Run:

```bash
cmake --build build -j4
if [[ -x build/testrunner ]]; then ./build/testrunner; fi
```

Expected: successful build and no test failure.

- [ ] **Step 3: Check function signatures and lengths**

Review every function changed or added in:

```text
cli/src/QRGridNormalizer.cpp
cli/src/ImageReaderSource.cpp
cli/src/main.cpp
```

Expected: no function over 100 lines and no function with more than five
parameters. Split helpers by responsibility if a limit is exceeded.

- [ ] **Step 4: Check diagnostics**

Run IDE diagnostics for all changed C++ and Python files.

Expected: no newly introduced warning or error.

- [ ] **Step 5: Update documentation with measured evidence**

Replace stale root-level image paths and old 20–180 second behavior. Document
the Version 1–10 scope, one-pass preprocessing, coarse-to-fine scoring, the
single low-confidence `try-harder` fallback, exact 34-image output, average, and
P95 from Step 1.

- [ ] **Step 6: Review the final diff**

Run:

```bash
git diff --check
git status --short
git diff -- cli/src scripts docs
```

Expected: no whitespace errors; only task-related source, verifier, and
documentation changes are included. Do not commit unless the user explicitly
requests a commit.
