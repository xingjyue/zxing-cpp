# QR Finder Pattern Repair Summary

## Background

The local sample set contains one clean QR code and several deliberately damaged QR codes under `qr_fig/`:

- `123.png`: clean baseline, decodes as `123`.
- `123_new.png`: gray selection/crop artifacts damage the left edge of two finder patterns.
- `123_destroy1.png` through `123_destroy4.png`: first class, one finder edge is removed or badly weakened.
- `123_destroy5.png` and `123_destroy6.png`: second class, one or more finder patterns are almost completely removed.
- `123_destroy7.png` and `123_destroy8.png`: third class, multiple finder patterns are partially damaged at the same time.
- `888.png`: clean high-version QR code, decodes as `888`.
- `888_destroy1.png`: high-version QR code with damaged finder patterns, decodes as `888` after repair.

Before the final fix, `123.png` and `123_new.png` decoded successfully, but all eight `123_destroy*.png` images failed with `decoding failed` / `zxing::ReaderException: No code detected`. Later, `888.png` exposed another issue: the old ZXing detector could decode it in normal mode, but `--try-harder` could over-relax finder detection and choose the wrong finder triplet. `888_destroy1.png` also showed that the earlier damaged-finder repair was too narrow because it assumed low-version QR codes and left-edge damage only.

## Algorithm Research

The relevant recovery strategies from current QR recovery tools and papers are:

1. **Recover fixed QR structures first.** Finder patterns, separators, timing patterns, format information, and version information are deterministic. If these structures are damaged, decoders often fail before Reed-Solomon error correction can help.
2. **Use remaining geometry to estimate the grid.** Damaged-finder algorithms often localize partial finder components, then infer the missing finder or missing corner from the expected QR geometry.
3. **Leave data damage to QR error correction.** Once the grid and fixed patterns are reliable, ordinary QR decoding can handle moderate data-module loss through Reed-Solomon correction.
4. **For severe curvature or perspective distortion, dewarp first.** That is a separate problem. The new samples are axis-aligned generated QR images, so fixed-pattern/grid repair is the right level of intervention.

This project now follows that structure: a small image preprocessor repairs fixed QR structures before ZXing's existing binarizer, detector, grid sampler, and decoder run.

## Root Cause

The new failures were not caused by payload corruption. They happened during QR localization.

ZXing's QR detector searches for the three large finder patterns using the `1:1:3:1:1` black/white run-length shape. When a finder pattern has a missing side, missing corner, or is entirely removed, the detector cannot confidently identify the three anchors. The pipeline then stops with `No code detected`, so Reed-Solomon error correction never gets a chance to recover the message.

For these generated images, the module grid is still regular:

- The QR version is Version 1 (`21x21` modules).
- The module size is consistent within each image.
- Most non-finder data modules remain aligned.
- The intended message is still recoverable once the fixed patterns are restored.

For the `888` samples, the root cause differs slightly:

- `888.png` is a valid high-version QR code. It can decode without `tryHarder`, but the relaxed `tryHarder` finder-pattern search can select an invalid high-version geometry.
- `888_destroy1.png` has top-left and bottom-left finder damage in a high-version QR grid. The remaining geometry is still aligned, but the damaged finders prevent reliable localization.

## Implemented Fix

The fix is implemented in `cli/src/ImageReaderSource.cpp` during image loading.

There are two repair stages:

1. **Version 1 fixed-pattern restoration**
   - Estimate module size from the most common black run length in rows and columns.
   - Search for the best `21x21` Version 1 grid position.
   - Score each candidate grid against deterministic Version 1 fixed modules:
     - top-left, top-right, and bottom-left finder patterns;
     - horizontal and vertical timing patterns.
   - If the best score is high enough, redraw:
     - all three `7x7` finder patterns;
     - the two timing-pattern spans between finders.

2. **Targeted left-damaged finder repair**
   - This is the earlier narrow repair for `123_new.png`.
   - It finds connected black components that look like a finder pattern with the left black bar missing.
   - It redraws that finder pattern only when the remaining top/right/bottom/center geometry matches a QR finder.
   - The module-size estimate now uses the minimum of the observed `height / 7` and `width / 6`, which handles high-resolution damaged finders like `888_destroy1.png` where the observed component is not a perfect 7-module square.

There is also a CLI-level decode fallback:

- If `--try-harder` is requested and a read fails, the CLI retries the same image once with normal finder detection.
- This preserves `tryHarder` behavior for hard images, but prevents clean high-version QR codes such as `888.png` from failing only because the relaxed search picked the wrong pattern triplet.

The Version 1 grid repair handles the `123_destroy*.png` samples. The targeted connected-component repair remains useful for the non-uniform `123_new.png` artifact case.

## Detailed Flow

For each PNG/JPEG loaded by the CLI:

1. Decode the image into RGBA/gray pixels.
2. Convert pixels to luminance using the existing ZXing weighting.
3. Estimate module size:
   - scan horizontal black runs;
   - scan vertical black runs;
   - build a run-length histogram;
   - choose the most common plausible run length.
4. Search candidate Version 1 grids:
   - candidate size is `21 * moduleSize`;
   - candidate origin ranges over all positions where that square fits in the image;
   - each candidate samples only fixed modules, not payload modules.
5. Score candidates:
   - finder modules must match the standard outer-black / inner-white / center-black pattern;
   - timing modules must alternate black/white;
   - data modules are ignored because they are content-dependent.
6. Repair the best candidate if its score passes the threshold:
   - redraw three finder patterns;
   - redraw horizontal and vertical timing patterns.
7. Skip Version 1 grid repair for larger QR images whose module-size estimate implies a high-version code. This avoids corrupting valid high-version QR codes such as `888.png`.
8. Run the targeted damaged-finder repair.
9. Pass the repaired luminance source to ZXing's normal decoding pipeline.

The repair does not hard-code the payload `123`. It restores structural QR markers and lets the decoder recover the data normally.

## Verification Script

A regression script was added at:

```text
scripts/verify_qr_damage_samples.sh
```

It runs the built CLI against:

- `123.png`
- `123_new.png`
- `123_destroy1.png` through `123_destroy8.png`
- `888.png`
- `888_destroy1.png`

and requires every image to decode exactly as `123`.

Before the grid repair, the script failed at the first damaged sample:

```text
123.png: 123
123_new.png: 123
123_destroy1.png: expected 123, got decoding failed
```

After the final fix, the script output was:

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
888.png: 888
888_destroy1.png: 888
```

## Build And Verify

Build:

```bash
cd /Users/xingjyue/Desktop/claudecode/project/zxing-cpp/build
/usr/local/Cellar/cmake/4.3.1/bin/cmake --build . -j4
```

Run regression verification:

```bash
cd /Users/xingjyue/Desktop/claudecode/project/zxing-cpp
scripts/verify_qr_damage_samples.sh
```

## Limitations

- The grid-level fixed-pattern restoration currently targets Version 1 QR codes (`21x21` modules), which matches the `123` damaged samples.
- Higher-version codes are protected from the Version 1 repair by a module-size/image-size guard. Their localized finder damage is handled by the connected-component finder repair instead.
- The approach is intended for axis-aligned generated QR images. Strong perspective distortion, curved surfaces, or warped modules require a dewarping stage before this repair.
- If payload damage exceeds QR error-correction capacity, fixed-pattern repair alone will not be enough. At that point the decoder would need bit-level erasure handling or manual/brute-force recovery.
