# QRGridNormalizer Dead-Code Cleanup Design

## Goal

Reduce size and duplication in `cli/src/QRGridNormalizer.cpp` by deleting dead
code and merging clearly redundant helpers, without changing detection latency
characteristics or algorithmic thresholds.

## Constraints

- Latency behavior must stay the same: no extra work on hot paths, no change to
  downsampling cutoffs, scan strides, candidate limits, or score thresholds.
- Do not change sampling strategies, purity weights, or absolute-evidence rules.
- Do not change the public API in `cli/src/QRGridNormalizer.h`.
- Prefer the smallest edit that removes dead/duplicate structure.
- Out of scope: merging `SampleBlack`/`QuickBlack`, `CoarseModelScore`/`ScoreGrid`,
  or `AreaAverage`/`FastDownsample`.

## Scope

Only `cli/src/QRGridNormalizer.cpp` (plus this design note). Framework docs may
be updated later if function names change; that is optional follow-up, not part
of the cleanup acceptance criteria.

## Changes

### 1. Delete dead code

Remove unused symbols that have no call sites:

| Symbol | Reason |
|--------|--------|
| `Luminance` | Unused; luminance conversion already inlined in `BuildWorkingBinary` |
| `Opacity` | Unused; opacity handling already inlined in `BuildWorkingBinary` |
| `BuildBinary` | Replaced by `BuildWorkingBinary`; never called |
| `HasDistinctScoreLead` | Defined and forward-declared but never called; callers use `HasAbsoluteEvidence` |

Keep public entry points: `CanNormalizeQR`, `NormalizeQR`, `NormalizeQRCandidates`.

### 2. Inline thin wrappers

Remove one-line / trivial wrappers and call the underlying function directly:

| Remove | Replacement at call sites |
|--------|---------------------------|
| `KeepBest(best, c)` | `KeepLimited(best, c, 8)` |
| `PurityScore(image, grid)` | `GridPurityScore(image, grid, 2, 0, 0.28f)` |
| `SparsePurityScore(image, grid)` | `GridPurityScore(image, grid, 4, 1, 0.24f)` |

Numeric arguments must match today's wrappers exactly.

### 3. Merge shared flood-fill

`FloodRoi` and `FloodPaper` both perform a 4-neighbor BFS over `ComponentGrid`
(`mask` / `seen`). Extract one shared helper that owns neighbor expansion:

```text
template<typename Visit>
void FloodComponent(ComponentGrid& grid, int start, Visit visit);
// Marks start seen, BFS 4-neighbors (L,R,U,D order unchanged),
// calls visit(index, x, y) once per newly reached cell including start.
```

- `FloodRoi`: visit updates the ROI bounding box (same as today).
- `FloodPaper`: visit appends cell indices into the component vector (same as today).
- Prefer a function template (or equivalent) so the visit callback is inlined and
  hot-path cost stays equivalent to the two specialized loops today.

Behavior of visited cells, neighbor order, and early skip of seen/mask-off cells
must remain identical. No change to ROI scoring or paper-component sizing rules.

### 4. Extract pixel decode helper

Extract the per-pixel gray/opacity conversion used inside `BuildWorkingBinary`
into a small inline helper:

```text
void DecodePixel(const unsigned char* pixel, int comps, bool hasAlpha,
                 uint8_t& gray, uint8_t& opacity);
// comps 1/2: gray=pixel[0]; comps 3/4: Rec.601-style luminance (+ alpha over white for 4).
// opacity written only when hasAlpha is true (caller fills 255 otherwise).
```

Keep the existing nested loops, nearest-neighbor downscale when
`longest > 1024`, and `FinishBinary` call unchanged. Do not resurrect
`BuildBinary`.
## Non-goals

- No threshold / constant retuning.
- No pipeline reordering (`FindQRRegions` → model/axis/paper → refine → render).
- No header or CLI API changes.
- No aggressive merging of scoring or sampling paths that differ by design.

## Testing

1. Build the CLI target that includes `QRGridNormalizer.cpp`.
2. Run an existing verification path if available (for example
   `scripts/verify_qr_damage_samples.sh` or `scripts/verify_qr_generalization.py`)
   on a small sample set to confirm decode outcomes are unchanged.
3. Manual sanity: with `ZXING_QR_SEARCH_STATS` unset, runtime path must not gain
   new stderr or getenv checks beyond what already exists.

## Acceptance

- Dead symbols listed above are gone.
- Thin wrappers listed above are gone; call sites use the underlying APIs with
  identical constants.
- Flood-fill and pixel-decode share one implementation each, with no behavioral
  change.
- Project builds; existing decode verification does not regress on the exercised
  samples.
