# QR Finder Pattern Repair Summary

## Background

Two QR images were used during debugging:

- `qr_fig/123.png`: decodes successfully as `123`.
- `qr_fig/123_new.png`: originally failed with `zxing::ReaderException: No code detected`.

The failure happened before payload decoding. Both `HybridBinarizer` and `GlobalHistogramBinarizer` failed to produce a detectable QR code for `123_new.png`.

## Investigation

The first attempt focused on making the existing QR detector more tolerant:

- Relaxed finder-pattern ratio checks when `tryHarder` is enabled.
- Expanded QR alignment-pattern search radius.
- Switched grid sampling from nearest-neighbor to bilinear sampling.
- Added a CLI fallback that retries with `tryHarder` after both binarizers fail.

These changes improved robustness but did not solve `123_new.png`.

Further image inspection showed the actual root cause: the left side of `123_new.png` contains gray selection/crop artifacts that overwrite the left black bar of the top-left and bottom-left finder patterns. Because finder patterns are the primary localization anchors, ZXing could not reliably identify the QR code.

## Fix

The working fix is implemented in `cli/src/ImageReaderSource.cpp`.

During image loading, the CLI now runs a lightweight pre-processing step that:

1. Finds connected black components in the decoded image.
2. Looks for components shaped like a finder pattern whose left black bar is missing.
3. Verifies that the remaining top, right, bottom, and center finder regions are present.
4. Reconstructs the full standard 7x7 finder pattern in-place.

This is intentionally narrow: it only targets the observed damaged-finder shape, so normal QR images are not rewritten.

## Verification

Build command:

```bash
cd /Users/xingjyue/Desktop/claudecode/project/zxing-cpp/build
/usr/local/Cellar/cmake/4.3.1/bin/cmake --build . -j4
```

Decode verification:

```bash
./zxing ../qr_fig/123.png
./zxing ../qr_fig/123_new.png
```

Expected output:

```text
123
123
```

This was verified after the final finder-repair change.

## Notes

- The repair is currently part of the CLI image-loading path, so it applies to files decoded through `zxing`.
- The test images in `qr_fig/` remain untracked unless explicitly added to git.
- For more general curved or heavily warped QR codes, a broader geometric dewarping pipeline would be needed. The current fix targets the concrete failure mode from `123_new.png`: missing finder-pattern bars caused by image artifacts.
