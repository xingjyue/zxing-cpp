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

Only if Layer 1 fails does the CLI reload the image with `ImageReaderSource::create(filename, /*repairFixedPatterns=*/true)`. This pipeline tries three repair strategies in order; each one falls back to the next if it cannot satisfy its safety threshold:

1. **Version-aware grid normalization (`normalizeQRToModuleImage`)**
   - Estimate the dominant module size from a black run-length histogram.
   - Find the bounding box of the QR content.
   - For each QR version `v` in `1..40` whose implied module size is within `±max(2, estimatedModuleSize/2)` of the dominant run length:
     - Pre-compute the deterministic fixed modules of version `v`: three finder patterns, the horizontal and vertical timing patterns, the dark module, and all internal alignment patterns.
     - Search a small window around the bounding box top-left for a `(gridLeft, gridTop)` whose fixed-module score is highest.
   - Pick the best `(version, gridLeft, gridTop, moduleSize)` overall. If the best score is `>= 0.72`, render a normalized standard QR image:
     - Each module is `8x8` pixels with a quiet zone of 4 modules.
     - Fixed modules are forced to their canonical color.
     - Data modules are sampled from the original image at the inferred grid.
   - Hand this clean module image to ZXing.
2. **Version-1 fixed-pattern restoration (`repairQRFixedPatterns`)**
   - Specialized fallback for `21x21` codes that the normalization could not lock in.
   - Redraws the three finder patterns and the timing patterns at the best Version 1 grid match.
3. **Version-independent finder restoration (`repairDamagedFinderPatterns`)**
   - Connected-component scan for finder-shaped survivors.
   - Recognizes left-edge or right-edge damage and redraws the affected finder.
   - Tries multiple alignment hypotheses around each candidate component, but only writes a finder when the candidate score passes the structural check, so it does not corrupt valid data modules.

The grid normalization is now safe enough to run unconditionally inside Layer 2 because:

- The version filter based on `estimatedModuleSize` keeps the search bounded.
- Fixed modules are pre-computed per version, so the inner loop is small even for Version 40.
- Layer 2 only runs after Layer 1 has already failed; it never touches healthy decoding.

### Detector Robustness Tweak

`Detector::processFinderPatternInfo` now retries grid sampling with a 3-finder transform if alignment-pattern based sampling lands a coordinate outside the image. This used to cause `Transformed point out of bounds` failures on legitimate high-version codes such as `333.png`.

## Detailed Pipeline

For each PNG/JPEG handed to the CLI:

1. Decode the image into RGBA/gray pixels.
2. Convert pixels to luminance using ZXing's standard weighting.
3. Try the normal ZXing pipeline (binarizer + detector + decoder). If it succeeds, return.
4. If decoding fails, reload the image with `repairFixedPatterns=true`.
5. Run version-aware grid normalization:
   - Estimate dominant module size from row/column black run-length histogram.
   - Compute bounding box of all black content.
   - For each QR version whose implied module size matches the histogram, sweep a small window around the bounding box top-left and score against pre-computed fixed modules.
   - If the best score >= 0.72, rebuild a normalized canonical QR image (8 px modules, 4-module quiet zone) and hand it to ZXing.
6. If grid normalization is not confident enough, try Version 1 fixed-pattern restoration.
7. If neither applied, run version-independent connected-component finder restoration.
8. Pass the (possibly repaired) luminance source through ZXing's normal decode path.
9. If `--try-harder` is requested but the relaxed finder search picks an inconsistent triplet, the CLI falls back to a normal-mode retry.

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

Latest passing output:

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
```

Normal-decode smoke (no repair invoked, both with and without `--try-harder`):

```text
123 -> 123
333 -> 333
888 -> 888
```

## Limitations

- All repair logic targets axis-aligned generated QR images. Strong perspective distortion or curved surfaces need a dewarping stage before these steps.
- The version-aware normalization gives up if the best candidate score is below `0.72`. This is intentional; we prefer to fail loudly rather than emit garbage data.
- Connected-component finder repair refuses to touch components whose aspect ratio is not finder-like, to avoid mistaking large black blobs for finders.
- If payload damage exceeds the QR Reed-Solomon capacity, fixed-pattern repair alone is not enough. Bit-level erasure handling and exhaustive mask/version brute-force would be needed in that case.
