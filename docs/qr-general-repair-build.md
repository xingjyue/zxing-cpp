# General QR Repair Pipeline and Build Notes

## Scope

The CLI keeps normal ZXing decoding as the first path. Repair only runs after
both normal decode attempts fail. The repair path is version-generic: it scans
QR versions 1 through 40 and does not include a Version 1-only fixed-pattern
fallback.

## Changes

### Removed Version 1-only fallback

The previous `repairQRFixedPatterns` path redrew a `21x21` QR using a
Version 1-specific grid match. That code was removed so recovery does not rely
on a special case for the sample set.

The remaining fallback after grid normalization is
`repairDamagedFinderPatterns`, which is based on finder-shaped connected
components and does not assume a QR version.

### More tolerant version-generic grid normalization

`normalizeQRToModuleImage` still searches all QR versions from 1 to 40 and
scores each candidate against deterministic QR structures:

- three finder patterns;
- horizontal and vertical timing patterns;
- the dark module;
- alignment patterns for versions greater than 1.

The module-size histogram is no longer used as a hard version filter when the
estimated module size is below 4 pixels. Photographed prints can contain paper
texture, glare, and broken ink regions that create many tiny black runs; using
that value as a hard gate rejected otherwise valid grid candidates. In that
case, the code keeps all plausible versions in the search and lets the
fixed-module score choose the best grid.

The normalization score threshold was relaxed from `0.72` to `0.55` because
damaged finder patterns reduce the fixed-structure score even when the inferred
module grid is correct. Fixed modules are still forced to their canonical color
when the normalized QR image is rendered.

### Region-vote module sampling

Grid scoring and normalized image generation now sample the inner area of each
module and use a black-pixel majority vote instead of a single center pixel.
This makes the repair path more robust on photographed codes where print
texture or blur can make a module center unrepresentative.

### OpenCV status

The top-level CMake project already contains optional OpenCV support. If CMake
finds OpenCV, it builds the `zxing-cv` target and links OpenCV sources. In this
workspace, `OpenCV_DIR` is currently `OpenCV_DIR-NOTFOUND`, so the verified
changes above build without OpenCV.

OpenCV remains the recommended next step for stronger camera-image support:

- adaptive thresholding before connected-component analysis;
- morphology close/open to reconnect textured black regions;
- contour-based perspective correction before grid normalization.

## Build

From the repository root:

```bash
cmake --build build
```

If creating a fresh build directory:

```bash
cmake -S . -B build
cmake --build build
```

If OpenCV is installed but CMake cannot find it automatically, configure with
the OpenCV package directory:

```bash
cmake -S . -B build -DOpenCV_DIR=/path/to/opencv/lib/cmake/opencv4
cmake --build build
```

## Verification

The photographed samples in `qr_fig/QRcode` were checked with:

```bash
./build/zxing --try-harder \
  qr_fig/QRcode/1.png \
  qr_fig/QRcode/2.png \
  qr_fig/QRcode/3.png \
  qr_fig/QRcode/4.png \
  qr_fig/QRcode/5.png \
  qr_fig/QRcode/6.png \
  qr_fig/QRcode/7.png \
  qr_fig/QRcode/8.png
```

Current output:

```text
333
123
123
123
123
123
123
123
```
