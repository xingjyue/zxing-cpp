# Generalized QR Detection Design

## Goal

Replace the single-ROI hard gate with QR-model-driven detection that remains
stable when unrelated dark objects are added outside the QR code and quiet
zone.

Acceptance requires:

- exact decoding of all 38 current real samples;
- at least 95% exact decoding on a deterministic 100-image synthetic holdout;
- no decoded result on generated non-QR negatives;
- average latency at most 0.5 seconds and nearest-rank P95 at most 0.8 seconds;
- no regression to the existing Version 1–10 repair scope or coding limits.

## Evidence and Root Cause

`print/11.png` decodes, while the larger source-like `print/12.png` fails.
The current ROI is not the only cause:

- `12.png` decodes after several resampling ratios, including ROI scales
  `0.65`, `0.75`, `0.85`, and `1.15`, or a full-image scale of `1.3`.
- This sensitivity shows that discrete module size, sample phase, and grid
  anchors can reject a valid QR even when the spatial region is reasonable.
- The current single-ROI implementation passed only `14/30` and `11/18` on
  two generated background-pollution sets.
- A multi-scale Top-K connected-component prototype improved one set to
  `20/30`, but still failed on unseen background shapes.
- A QR fixed-structure model searched over the full image decoded `12.png`
  with its highest-scoring candidate and improved a separate holdout from
  `11/18` to `17/18`.

The root architectural problem is early commitment to a region based on generic
shape, density, and texture. A background object can merge with the QR or rank
above it before QR-specific evidence is evaluated.

## Architecture

### Candidate sources

No region proposal is authoritative. The detector combines:

- the full image;
- multiple two-dimensional occupancy components at bounded scales;
- dark-background paper quadrilaterals;
- global and local module-run peaks.

ROI candidates are acceleration hints only. Failure to propose or rank an ROI
must not exclude the full-image QR search.

### Fixed-size detection layer

Create a detection layer whose longest side has a fixed upper bound. All coarse
search work happens on this layer, so candidate count is bounded independently
of source resolution. Candidate coordinates map back to the original image for
refinement and rendering.

### QR-model search

For Versions 1 through 10, construct the deterministic QR structures already
used by the normalizer:

- three finder patterns and separators;
- horizontal and vertical timing patterns;
- alignment patterns;
- the dark module;
- Version 7–10 version information.

Generate a small set of module-size hypotheses from global and tiled run
histograms. For each plausible version and module size, scan grid origins at a
coarse stride of approximately half a module.

Score the three finder groups independently, then combine the strongest
surviving finder evidence with timing, alignment, dark-module, and module-purity
scores. This permits one or two damaged finders without replacing QR evidence
with generic foreground shape.

Keep at most eight geometrically and semantically distinct candidates globally.

## Refinement and Decode Validation

Map retained candidates to the original image. Refine:

- grid origin;
- horizontal and vertical module scale;
- subpixel sampling phase;
- rotation;
- four perspective corners when evidence warrants it.

Use region/area sampling at continuous coordinates. Do not depend on a specific
whole-image resize ratio to repair aliasing.

After confidence and distinct-runner checks, render and decode at most the top
three normalized candidates. A failed decode is evidence that the selected grid
was not sufficient; continue to the next distinct candidate. If all normalized
candidates fail, perform one bounded `try-harder` attempt on the original image.

No stage may create an unbounded orientation, version, or pixel-resolution
search.

## Generalization Test Corpus

### Positive generation

Generate polluted images from clean synthetic, printed, rotated, and damaged
finder samples with known payloads. Randomize:

- canvas width and height;
- QR position and scale;
- background luminance and mild gradients;
- elongated bars, squares, circles, and text-like line segments;
- sensor-like noise and mild blur.

Pollution must not touch the QR content or its four-module quiet zone. Such
samples test detection invariance, not additional payload destruction.

Use fixed and separate seeds:

- a tuning set used while implementing;
- a holdout set of at least 100 images that is not inspected or individually
  tuned.

The metamorphic property is: adding legal background pollution to a decodable
source must preserve the exact payload.

### Negative generation

Generate backgrounds containing nested squares, bars, checker patterns, and
high-contrast texture but no QR. None may produce a decoded result.

### Regression and performance

Run:

1. all 38 real samples;
2. the tuning set;
3. the untouched positive holdout;
4. the negative set.

Report exact payload accuracy, false positives, average latency, and
nearest-rank P95. Required gates are 38/38 real, at least 95% positive holdout,
zero negative decodes, average at most 0.5 seconds, and P95 at most 0.8 seconds.

## Scope and Constraints

- Keep normal ZXing decoding as the first path.
- Keep repair support limited to Versions 1–10.
- Do not add an OpenCV dependency to the C++ `zxing` target.
- Keep every function at no more than five parameters and 100 lines.
- Do not tune constants against `12.png` or any individual holdout failure.
- Do not remove the existing ROI detector; demote it to a non-authoritative
  candidate source.
