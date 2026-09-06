# QRGridNormalizer Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete dead code and merge redundant helpers in `QRGridNormalizer.cpp` without changing latency characteristics or algorithmic thresholds.

**Architecture:** Pure in-file refactor of the anonymous-namespace helpers in `cli/src/QRGridNormalizer.cpp`. Public API in `QRGridNormalizer.h` stays unchanged. Shared flood-fill uses an inlinable function template; pixel decode is a small helper used only by `BuildWorkingBinary`.

**Tech Stack:** C++11, existing CMake `zxing` CLI target, Python verifier scripts already in-repo.

**Spec:** `docs/superpowers/specs/2026-08-05-qr-grid-normalizer-cleanup-design.md`

---

## File Structure

- Modify: `cli/src/QRGridNormalizer.cpp` — only file with behavioral source changes.
- Unchanged: `cli/src/QRGridNormalizer.h`, `cli/src/ImageReaderSource.*`, `cli/src/main.cpp`.
- Optional later (out of acceptance): `docs/qr-grid-normalizer-framework.md` if it still names deleted wrappers.

---

### Task 1: Delete dead code

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Confirm symbols are unused**

Run from repo root:

```bash
grep -nE '\b(Luminance|Opacity|BuildBinary|HasDistinctScoreLead)\b' cli/src/QRGridNormalizer.cpp
```

Expected: only definition lines (and for `HasDistinctScoreLead`, one forward declaration ~2430 plus definition ~2581). No call sites.

- [ ] **Step 2: Delete `Luminance` and `Opacity`**

Remove the full functions currently at approximately lines 239–268:

```cpp
uint8_t Luminance(const MutableImage& image, int x, int y) {
  // ... entire body ...
}

uint8_t Opacity(const MutableImage& image, int x, int y) {
  // ... entire body ...
}
```

Leave `CompositeOverWhite` in place; `BuildWorkingBinary` still needs it.

- [ ] **Step 3: Delete `BuildBinary`**

Remove the full function currently at approximately lines 341–383:

```cpp
BinaryImage BuildBinary(const MutableImage& image) {
  // ... entire body ...
}
```

Keep `FinishBinary` and `BuildWorkingBinary`.

- [ ] **Step 4: Delete `HasDistinctScoreLead`**

Remove the forward declaration near other search helpers:

```cpp
bool HasDistinctScoreLead(const std::vector<GridCandidate>& candidates);
```

Remove the definition:

```cpp
bool HasDistinctScoreLead(const std::vector<GridCandidate>& candidates) {
  return !candidates.empty() && candidates[0].score >= 0.72f &&
      DistinctScoreMargin(candidates) >= 0.004f;
}
```

Keep `DistinctScoreMargin` and `HasAbsoluteEvidence` unchanged.

- [ ] **Step 5: Verify deletions**

```bash
grep -nE '\b(Luminance|Opacity|BuildBinary|HasDistinctScoreLead)\b' cli/src/QRGridNormalizer.cpp
```

Expected: no matches.

- [ ] **Step 6: Commit** (only if the user explicitly asked for commits)

```bash
git add cli/src/QRGridNormalizer.cpp
git commit -m "$(cat <<'EOF'
refactor: remove unused helpers from QRGridNormalizer

EOF
)"
```

---

### Task 2: Inline thin wrappers

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Replace `SparsePurityScore` / `PurityScore` call sites**

In `CoarseModelScore`, change:

```cpp
  const ScoreResult purity = SparsePurityScore(image, grid);
```

to:

```cpp
  const ScoreResult purity = GridPurityScore(image, grid, 4, 1, 0.24f);
```

In `ScoreGrid`, change:

```cpp
  const ScoreResult purityScore = PurityScore(image, grid);
```

to:

```cpp
  const ScoreResult purityScore = GridPurityScore(image, grid, 2, 0, 0.28f);
```

- [ ] **Step 2: Delete the wrappers**

Remove:

```cpp
ScoreResult PurityScore(const BinaryImage& image,
                        const GridCandidate& grid) {
  return GridPurityScore(image, grid, 2, 0, 0.28f);
}

ScoreResult SparsePurityScore(const BinaryImage& image,
                              const GridCandidate& grid) {
  return GridPurityScore(image, grid, 4, 1, 0.24f);
}
```

- [ ] **Step 3: Replace every `KeepBest` call with `KeepLimited(..., 8)`**

Exact replacements (search for `KeepBest(`):

```cpp
// ScoreSeedOrientations
KeepLimited(*scan.best, strongest, 8);

// ScoreCandidates
KeepLimited(best, shortlist[i], 8);

// SourceModelCandidates (or equivalent loop that currently KeepBest(result, candidate))
KeepLimited(result, candidate, 8);

// RefineTop
KeepLimited(refined, RefineCandidate(image, coarse[i], fixed, false), 8);
KeepLimited(
    perspective, RefineCandidate(image, coarse[i], fixed, true), 8);
KeepLimited(refined, perspective[i], 8);

// FindRefinedCandidates merge loop
KeepLimited(refined, modelRefined[i], 8);
```

- [ ] **Step 4: Delete `KeepBest`**

Remove:

```cpp
void KeepBest(std::vector<GridCandidate>& best, GridCandidate candidate) {
  KeepLimited(best, candidate, 8);
}
```

- [ ] **Step 5: Verify wrappers are gone**

```bash
grep -nE '\b(KeepBest|PurityScore|SparsePurityScore)\b' cli/src/QRGridNormalizer.cpp
```

Expected: no matches. (`GridPurityScore` and `KeepLimited` remain.)

- [ ] **Step 6: Commit** (only if the user explicitly asked for commits)

```bash
git add cli/src/QRGridNormalizer.cpp
git commit -m "$(cat <<'EOF'
refactor: inline thin scoring and candidate-keep wrappers

EOF
)"
```

---

### Task 3: Merge flood-fill into `FloodComponent`

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp` (replace `FloodRoi` / `FloodPaper`)

- [ ] **Step 1: Insert shared template above the current `FloodRoi`**

Place this immediately before where `FloodRoi` currently starts (after `BuildDarkRoiGrid`):

```cpp
template <typename Visit>
void FloodComponent(ComponentGrid& grid, int start, Visit visit) {
  std::vector<int> pending(1, start);
  grid.seen[start] = 1;
  for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
    const int index = pending[cursor];
    const int x = index % grid.width;
    const int y = index / grid.width;
    visit(index, x, y);
    const int neighbor[4] = {
        x > 0 ? index - 1 : -1,
        x + 1 < grid.width ? index + 1 : -1,
        y > 0 ? index - grid.width : -1,
        y + 1 < grid.height ? index + grid.width : -1};
    for (int side = 0; side < 4; ++side) {
      const int next = neighbor[side];
      if (next >= 0 && grid.mask[next] && !grid.seen[next]) {
        grid.seen[next] = 1;
        pending.push_back(next);
      }
    }
  }
}
```

Neighbor order must remain L, R, U, D exactly as today.

- [ ] **Step 2: Rewrite `FloodRoi` to use the template**

Replace the body with:

```cpp
void FloodRoi(ComponentGrid& grid, int start, RoiComponent& component) {
  component = RoiComponent{{grid.width, grid.height, 0, 0}};
  FloodComponent(grid, start, [&](int, int x, int y) {
    component.cells.left = std::min(component.cells.left, x);
    component.cells.top = std::min(component.cells.top, y);
    component.cells.right = std::max(component.cells.right, x + 1);
    component.cells.bottom = std::max(component.cells.bottom, y + 1);
  });
}
```

Call sites of `FloodRoi` (in `FindQRRegions`) stay unchanged.

- [ ] **Step 3: Rewrite `FloodPaper` to use the template**

Replace the body with:

```cpp
void FloodPaper(ComponentGrid& grid, int start, std::vector<int>& component) {
  component.clear();
  FloodComponent(grid, start, [&](int index, int, int) {
    component.push_back(index);
  });
}
```

Note: today's `FloodPaper` pushes `start` before the loop and uses `component` as the pending queue. The template uses a separate `pending` vector and visits start via `visit`, which appends it — final cell membership and visit order of newly discovered neighbors must match. Because both use the same L/R/U/D expansion and mark-seen-before-enqueue, the sequence of indices appended equals the old BFS order.

Call sites of `FloodPaper` (in `LargestPaperComponent`) stay unchanged.

- [ ] **Step 4: Sanity-check flood helpers still exist as named entry points**

```bash
grep -nE '\b(FloodComponent|FloodRoi|FloodPaper)\b' cli/src/QRGridNormalizer.cpp
```

Expected: one `FloodComponent` definition; `FloodRoi` / `FloodPaper` definitions plus their existing call sites.

- [ ] **Step 5: Commit** (only if the user explicitly asked for commits)

```bash
git add cli/src/QRGridNormalizer.cpp
git commit -m "$(cat <<'EOF'
refactor: share ComponentGrid flood-fill between ROI and paper

EOF
)"
```

---

### Task 4: Extract `DecodePixel` for `BuildWorkingBinary`

**Files:**
- Modify: `cli/src/QRGridNormalizer.cpp`

- [ ] **Step 1: Add helper just above `BuildWorkingBinary`**

```cpp
void DecodePixel(const unsigned char* pixel, int comps, bool hasAlpha,
                 uint8_t& gray, uint8_t& opacity) {
  if (comps == 1 || comps == 2) {
    gray = pixel[0];
    if (hasAlpha) {
      opacity = pixel[comps - 1];
    }
    return;
  }
  const uint8_t luminance = static_cast<uint8_t>(
      (306 * static_cast<int>(pixel[0]) +
       601 * static_cast<int>(pixel[1]) +
       117 * static_cast<int>(pixel[2]) + 0x200) >> 10);
  gray = comps == 4 ? CompositeOverWhite(luminance, pixel[3]) : luminance;
  if (hasAlpha) {
    opacity = pixel[3];
  }
}
```

- [ ] **Step 2: Use it inside the `BuildWorkingBinary` pixel loop**

Replace the inner `if (comps == 1 || comps == 2) { ... } else { ... }` block with:

```cpp
      DecodePixel(pixel, comps, hasAlpha, result.gray[out], result.opacity[out]);
```

Keep sizing, `scaleDown` (`longest > 1024`), nearest-neighbor `sx`/`sy`, opacity prefill of 255 when `!hasAlpha`, and `FinishBinary(result)` unchanged.

- [ ] **Step 3: Confirm `BuildBinary` was not reintroduced**

```bash
grep -nE '\b(BuildBinary|DecodePixel)\b' cli/src/QRGridNormalizer.cpp
```

Expected: `DecodePixel` definition + one call; no `BuildBinary`.

- [ ] **Step 4: Commit** (only if the user explicitly asked for commits)

```bash
git add cli/src/QRGridNormalizer.cpp
git commit -m "$(cat <<'EOF'
refactor: extract DecodePixel for working binary conversion

EOF
)"
```

---

### Task 5: Build and verify

**Files:**
- Test via build + existing scripts (no new test files required)

- [ ] **Step 1: Build the CLI**

```bash
cmake --build build --target zxing -j4
```

Expected: success. If `build/` is missing:

```bash
cmake -S . -B build
cmake --build build --target zxing -j4
```

- [ ] **Step 2: Grep acceptance for deleted symbols**

```bash
grep -nE '\b(Luminance|Opacity|BuildBinary|HasDistinctScoreLead|KeepBest|PurityScore|SparsePurityScore)\b' cli/src/QRGridNormalizer.cpp || true
```

Expected: empty output.

- [ ] **Step 3: Smoke one known sample through the CLI**

```bash
./build/zxing --qr-repair qr_fig/version1/123.png
```

Expected: successful decode of the known payload (same as before cleanup). Adjust flag/path if the local CLI invocation differs — see `docs/qr-general-repair-build.md` and `scripts/verify_qr_repair.py` for the canonical command.

- [ ] **Step 4: Run the repair verifier (preferred full check)**

```bash
python3 scripts/verify_qr_repair.py
```

or:

```bash
scripts/verify_qr_damage_samples.sh
```

Expected: all listed samples pass with latency budgets unchanged in meaning (no algorithm changes, so outcomes should match prior baseline).

- [ ] **Step 5: Final commit** (only if the user explicitly asked for commits)

If Tasks 1–4 were left uncommitted, one squashed commit is fine:

```bash
git add cli/src/QRGridNormalizer.cpp docs/superpowers/specs/2026-08-05-qr-grid-normalizer-cleanup-design.md docs/superpowers/plans/2026-08-05-qr-grid-normalizer-cleanup.md
git commit -m "$(cat <<'EOF'
refactor: clean up dead and duplicate QRGridNormalizer helpers

EOF
)"
```

---

## Spec Coverage Checklist

| Spec item | Task |
|-----------|------|
| Delete `Luminance` / `Opacity` / `BuildBinary` / `HasDistinctScoreLead` | Task 1 |
| Inline `KeepBest` / `PurityScore` / `SparsePurityScore` | Task 2 |
| Shared `FloodComponent` for ROI + paper | Task 3 |
| Extract `DecodePixel` | Task 4 |
| Build + verify; no threshold/API changes | Task 5 |
| Non-goals (no SampleBlack/ScoreGrid merge, etc.) | Not scheduled |

## Self-Review Notes

- No TBD/placeholder steps.
- `FloodPaper` rewrite preserves BFS membership via visit-on-dequeue of a separate pending queue; call sites unchanged.
- Commit steps are gated on explicit user request (repo commit policy).
