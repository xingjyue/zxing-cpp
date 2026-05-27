# QR Finder Pattern Repair Summary

## Goal

Two requirements drive the design:

1. **Always decode normal, undamaged QR codes** of any version. The decoder must not regress because of repair logic.
2. **Best-effort decode** of QR codes whose finder patterns or other fixed structures are partially or fully destroyed.

## Sample Set

All samples live under `qr_fig/`:

- `123.png`: clean baseline, Version 1, decodes as `123`.
- `123_new.png`: Version 1 with gray crop artifacts overlapping the left edges of two finder patterns.
- `123_destroy1.png` to `123_destroy8.png`: Version 1 with progressively heavier finder damage.
- `333.png`: clean Version 15 (`77x77` modules), decodes as `333`.
- `333_destroy1.png`, `333_destroy2.png`: Version 15 with two finder patterns mostly missing.
- `888.png`: clean Version 10 (`57x57` modules), decodes as `888`.
- `888_destroy1.png` to `888_destroy3.png`: Version 10 with one or more damaged finders.
- `333v15.png`, `333v20.png`, `333v30.png`, `333v35.png`, `333v40.png`: clean high-version stress samples (the `vNN` suffix encodes the QR version, **not** the payload), all rendered at `1080x1080`. Versions 15/20/30/35/40 decode to `333`.
- `333v38.png`: clean Version 38 sample whose payload is actually `84244086` (the filename only marks the version).

## Algorithmic Background

The literature on damaged QR recovery (e.g., the Dynamsoft auto-restore writeup, the MDPI paper on partial finder damage, MMA 2015 QR code recovery, the QRazyBox toolkit) consistently uses three building blocks:

1. **Recover deterministic structures first.** Finder patterns, separators, timing patterns, dark module, and alignment patterns have known positions and colors per version.
2. **Use surviving geometry as anchors.** When some finders are missing, the remaining geometry plus the QR module grid lets the decoder estimate the missing pieces.
3. **Trust Reed-Solomon for residual data damage.** Once the grid is recovered, ZXing's existing decoder handles a fair amount of payload corruption.

Curved surfaces or strong perspective distortion still require a dewarping pre-stage; this repository targets axis-aligned generated QR images.

## Design

The CLI flow now has two strict layers:

### Layer 1: Original Image Decode

`ImageReaderSource::create(filename)` loads the image **without modification**. The CLI runs ZXing's normal pipeline on it. Any clean QR code, regardless of version, is expected to take this path.

### Layer 2: Repair-Then-Retry

Only if Layer 1 fails does the CLI reload the image with `ImageReaderSource::create(filename, /*repairFixedPatterns=*/true)`. This pipeline tries two repair strategies in order; each one falls back to the next if it cannot satisfy its safety threshold:

1. **Version-aware grid normalization (`normalizeQRToModuleImage`)**
   - Estimate the dominant module size from a black run-length histogram. The estimator is tuned so it returns the **fundamental module width** even on dense codes (see "Module-size estimator" below).
   - Find the bounding box of the QR content.
   - For each QR version `v` in `1..40` whose implied module size is within `±max(2, estimatedModuleSize/2)` of the estimator output when the estimate is reliable. If the estimate is below 4 pixels, keep the version in the search because photographed print texture can create many tiny black runs.
     - Pre-compute the deterministic fixed modules of version `v`: three finder patterns, the horizontal and vertical timing patterns, the dark module, and all internal alignment patterns.
     - Run a multi-anchor prescore: probe a `5x5` grid of offsets around the bounding box corner with the per-version `baseModuleSize`. If the best probe is below `0.45` and clearly worse than the running best score, skip the spatial search for that version.
     - Otherwise sweep a small window around the bounding box top-left for a `(gridLeft, gridTop)` whose fixed-module score is highest.
   - Pick the best `(version, gridLeft, gridTop, moduleSize)` overall. If the best score is `>= 0.55`, render a normalized standard QR image:
     - Each module is `8x8` pixels with a quiet zone of 4 modules.
     - Fixed modules are forced to their canonical color.
     - Data modules are sampled from the original image at the inferred grid using an inner-module majority vote.
   - Hand this clean module image to ZXing.
2. **Version-independent finder restoration (`repairDamagedFinderPatterns`)**
   - Connected-component scan for finder-shaped survivors.
   - Recognizes left-edge or right-edge damage and redraws the affected finder.
   - Tries multiple alignment hypotheses around each candidate component, but only writes a finder when the candidate score passes the structural check, so it does not corrupt valid data modules.

The grid normalization is version-generic and safe enough to run unconditionally inside Layer 2 because:

- The version filter based on `estimatedModuleSize` keeps the search bounded when the estimate is reliable; low estimates from photographed texture are treated as unreliable and are not used as hard gates.
- Fixed modules are pre-computed per version, so the inner loop is small even for Version 40.
- Layer 2 only runs after Layer 1 has already failed; it never touches healthy decoding.

### Detector Robustness Tweak

`Detector::processFinderPatternInfo` now retries grid sampling with a 3-finder transform if alignment-pattern based sampling lands a coordinate outside the image. This used to cause `Transformed point out of bounds` failures on legitimate high-version codes such as `333.png`.

### Module-size estimator (added for high-version codes)

`estimateModuleSize` originally counted black runs of length `>= max(3, min(width, height) / 80)`. On a 1080-pixel high-version code the floor became `13 px`, which is **larger than the real module width** (≈ 6 px for v35–v40). The histogram peak then landed on 2- or 3-module clusters, and the version filter `|baseModuleSize - estimatedModuleSize| <= max(2, est/2)` rejected the true high versions before they were even scored. v30 happened to sit inside the tolerance band; v35+ never did.

The estimator now:

- Lowers the run-length floor to `max(2, min(width, height) / 400)` so single-module runs of dense codes contribute to the histogram.
- After locating the absolute peak `bestRun`, walks **upward from the smallest captured run** and returns the first bin whose count is at least `bestCount / 3` and is itself a non-trailing peak. On dense codes this returns the **fundamental** module width (e.g., `7` for v30 instead of `14`); on low-version codes whose modules already sit at the dominant peak, it returns the same value as before.

Empirically (`333v15.png` … `333v40.png` at 1080×1080), the new estimator returns module widths of `12 / 8 / 7 / 7 / 5 / 5` for v15/v20/v30/v35/v38/v40, which match the real module widths within one pixel for every version. All these versions now satisfy the histogram-based version filter and reach the per-version spatial search and scoring stage.

## Detailed Pipeline

For each PNG/JPEG handed to the CLI:

1. Decode the image into RGBA/gray pixels.
2. Convert pixels to luminance using ZXing's standard weighting.
3. Try the normal ZXing pipeline (binarizer + detector + decoder). If it succeeds, return.
4. If decoding fails, reload the image with `repairFixedPatterns=true`.
5. Run version-aware grid normalization:
   - Estimate dominant module size from row/column black run-length histogram.
   - Compute bounding box of all black content.
   - For each QR version whose implied module size matches a reliable histogram estimate, sweep a small window around the bounding box top-left and score against pre-computed fixed modules.
   - If the best score >= 0.55, rebuild a normalized canonical QR image (8 px modules, 4-module quiet zone) and hand it to ZXing.
6. If grid normalization is not confident enough, run version-independent connected-component finder restoration.
7. Pass the (possibly repaired) luminance source through ZXing's normal decode path.
8. If `--try-harder` is requested but the relaxed finder search picks an inconsistent triplet, the CLI falls back to a normal-mode retry.

## Verification

Regression script: `scripts/verify_qr_damage_samples.sh`. Each entry is `name:expected_payload` and the script requires every sample to decode exactly to its expected text.

Build:

```bash
cd /Users/xingjyue/Desktop/claudecode/project/zxing-cpp/build
/usr/local/Cellar/cmake/4.3.1/bin/cmake --build . -j4
```

Run:

```bash
cd /Users/xingjyue/Desktop/claudecode/project/zxing-cpp
scripts/verify_qr_damage_samples.sh
```

Latest passing output (23 samples, ~45 s on the reference machine):

```text
123.png: 123
123_new.png: 123
123_destroy1.png: 123
123_destroy2.png: 123
123_destroy3.png: 123
123_destroy4.png: 123
123_destroy5.png: 123
123_destroy6.png: 123
123_destroy7.png: 123
123_destroy8.png: 123
333.png: 333
888.png: 888
888_destroy1.png: 888
888_destroy2.png: 888
888_destroy3.png: 888
333_destroy1.png: 333
333_destroy2.png: 333
333v15.png: 333
333v20.png: 333
333v30.png: 333
333v35.png: 333
333v38.png: 84244086
333v40.png: 333
```

Normal-decode smoke (no repair invoked, both with and without `--try-harder`):

```text
123 -> 123
333 -> 333
888 -> 888
```

### High-version regression analysis

The new `333vNN.png` samples sit in two regimes:

- **Layer 1 succeeds** for low/medium versions (e.g., `333v15.png`).
- **Layer 1 fails, Layer 2 normalization succeeds** for high versions (`333v30.png` and above) because the dense rendering pushes the standard ZXing detector past its tolerances. Before the estimator fix, only `v30` reached normalization; `v35`/`v38`/`v40` were rejected by the histogram-based version filter and decoded as `decoding failed`. After the estimator fix, all of them reach the spatial search and lock onto the correct grid with score `1.0`.

`333v38.png`'s real payload is `84244086`, not `333`. This was confirmed by upscaling the original 2x and 3x and decoding it via Layer 1 directly. The filename's `v38` only marks the QR version — the `333` prefix is shared with other samples but does not imply the payload. The regression script encodes this expectation explicitly so a future change cannot "fix" the decode by silently producing `333`.

## Limitations

- All repair logic targets axis-aligned generated QR images. Strong perspective distortion or curved surfaces need a dewarping stage before these steps.
- The version-aware normalization gives up if the best candidate score is below `0.55`. This still avoids arbitrary grids while allowing damaged fixed patterns in photographed samples.
- The module-size estimator assumes that **single-module black runs are the most numerous run-length** in the image. This holds for axis-aligned generated QRs where the renderer produces near-integer module widths. For images with strong anti-aliasing, blur, sub-sampled rendering, or photographed texture, the histogram peak may still drift; the multi-anchor prescore + the `>= 0.55` score gate are the second line of defense.
- Connected-component finder repair refuses to touch components whose aspect ratio is not finder-like, to avoid mistaking large black blobs for finders.
- If payload damage exceeds the QR Reed-Solomon capacity, fixed-pattern repair alone is not enough. Bit-level erasure handling and exhaustive mask/version brute-force would be needed in that case.
- The repair pipeline **does not infer** the version from format/version-info bits; it relies on the bbox-derived `baseModuleSize`. As a result, adjacent versions with very similar `bboxSize/dimension` ratios are disambiguated only by the fixed-pattern score. Adding a version-info BCH check would make this more robust on synthetic edge cases.
