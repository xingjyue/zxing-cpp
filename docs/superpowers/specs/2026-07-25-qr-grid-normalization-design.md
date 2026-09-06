# QR Grid Normalization Design

## Goal

Repair and decode every QR image under `qr_fig/` while keeping repair latency
low. On the current 34-image corpus, decoding must return the exact expected
payload, average at most 0.5 seconds per image, and have a P95 of at most
0.8 seconds.

The repair path supports QR Versions 1 through 10. Higher versions and other
barcode formats retain the unmodified ZXing decode path.

## Constraints

- Try the original ZXing decode before any repair.
- Do not add an OpenCV dependency to the C++ CLI.
- A function accepts at most five parameters.
- A function contains at most 100 lines.
- Prefer the smallest implementation that meets correctness and latency goals.
- Do not repeatedly reload an image or retry the full decoder for each
  orientation.

## Architecture

Add `cli/src/QRGridNormalizer.h` and `cli/src/QRGridNormalizer.cpp`. This module
owns thresholding, geometry estimation, grid search, scoring, and normalized
image generation. Small context types such as `ImageView`, `BinaryImage`,
`ContentGeometry`, and `GridCandidate` keep signatures within the parameter
limit.

`ImageReaderSource` remains responsible for PNG/JPEG loading and construction
of a ZXing luminance source. When requested, it calls the normalizer once and
uses the returned module image. The CLI uses this bounded decode sequence:

1. Decode the original image.
2. If that fails and normalization is confident, decode the normalized image
   once.
3. If normalization cannot produce a confident candidate, decode the original
   image once with ZXing's `try-harder` hint.

The current repeated orientation reloads and multi-round `try-harder` loop are
removed from the repair path.

## Processing Pipeline

### 1. Binary image and integral image

Convert the source to grayscale once. Compute an Otsu threshold and produce a
binary image plus a summed-area table. The integral image provides constant-time
black-fraction queries for rectangular module samples.

### 2. Content geometry

For a normal white-background image, find the bounds of black content after
excluding border-connected background. For a rotated image on a black canvas,
detect the white paper/QR quadrilateral and exclude the surrounding black
region.

Compute the foreground covariance and PCA eigenvalue ratio. PCA is a search
gate, not an authoritative angle estimate: a square QR can have unstable PCA
axes. A low-confidence axis-aligned image skips rotation search. A high-
confidence result enables a small angle search seeded by PCA and quadrilateral
edges. Mild perspective is represented by four grid corners instead of
rendering a large intermediate deskewed image.

### 3. Module and version candidates

Estimate module size from black run-length histograms along the candidate grid
axes. Consider only Versions 1 through 10. Use content span and module-size
consistency to reject implausible versions before spatial scoring.

### 4. Coarse-to-fine grid search

Coarsely search the plausible origin, scale, orientation, and four QR
orientations. Use sparse, deterministic structures for scoring:

- finder patterns and white separators;
- horizontal and vertical timing patterns;
- alignment patterns;
- the dark module;
- Version 7 through 10 version-information modules;
- whole-grid module purity.

Score each finder separately and cap its contribution. This permits a correct
grid to survive when one or two finders are completely white while preventing
one intact finder from dominating all other evidence.

Retain only a small fixed number of top candidates. Refine those candidates
locally over origin, module size, angle, and corner offsets. Accept a result
only when the best score reaches a minimum confidence and exceeds the runner-up
by a required margin.

### 5. Canonical output

Sample data modules with an inner-region majority vote. Write deterministic
fixed modules with their canonical values. Render each module as 8 by 8 pixels
and add a four-module white quiet zone. Pass only this best normalized image to
ZXing.

## Failure Handling

The original image is never modified. Unsupported files and loading errors keep
the existing exception behavior. If geometry detection or candidate confidence
is insufficient, normalization returns failure immediately and the CLI makes
one final decode attempt on the original image with `try-harder`. If that also
fails, it reports the ordinary decode failure. There is no unbounded search and
no orientation-by-orientation decoder retry.

The confidence threshold and best-versus-runner-up margin protect against false
positive reconstruction. Exact decoded text is required in regression tests;
merely producing any QR result is not success.

## Verification

The image-level regression corpus and expected payloads are:

- `qr_fig/version1/*.png`: `123`
- `qr_fig/version5/*.png`: `version5`
- `qr_fig/version10/*.png`: `version10`
- `qr_fig/print/1.png`: `333`
- `qr_fig/print/2.png` through `8.png`: `123`
- `qr_fig/Rotated/123_rotated1.png` through `4.png`: `123`
- `qr_fig/Rotated/rotated5.png` and `rotated6.png`: `version5`
- `qr_fig/Rotated/rotated7.png` and `rotated8.png`: `version10`

Testing follows red-green-refactor:

1. Update the regression runner for the reorganized 34-image corpus.
2. Record current correctness and timing failures, including
   `version1/123_destroy5.png` and `123_destroy6.png`.
3. Add normalization behavior in small steps, rerunning focused failing images
   before the complete corpus.
4. Verify that a low-confidence normalization result triggers exactly one
   `try-harder` attempt and never enters an orientation retry loop.
5. Build the CLI and run available existing C++ tests.
6. Run the complete corpus and fail unless 34 of 34 payloads match, average
   latency is at most 0.5 seconds, and P95 is at most 0.8 seconds.
7. Check every new or changed function for the five-parameter and 100-line
   limits.

Latency is measured as process-level wall-clock time on the current development
machine. The regression report includes every sample duration, average, and
nearest-rank P95.

## Out of Scope

- Repair of Versions 11 through 40.
- Curved-surface dewarping.
- Recovery when data-module corruption exceeds Reed-Solomon capacity.
- New OpenCV or Python runtime dependencies for the decoder.
