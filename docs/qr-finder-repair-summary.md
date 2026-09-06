# QR Finder Pattern Repair Summary

## Scope and decode order

The `zxing` CLI keeps ordinary ZXing decoding as the first path. If normal
decoding fails, it runs model-driven QR grid normalization for damaged QR
Versions 1 through 10. Other QR versions and barcode formats retain the normal
decoder behavior.

The bounded normal-to-model sequence is:

1. Decode the already loaded original image normally.
2. If that fails, generate and decode at most three distinct normalized
   candidates in ranked order.
3. If all normalized candidates fail and the caller did not already request
   `--try-harder`, decode the original image once with `try-harder`.

The image is not reloaded per orientation or candidate. The production C++ path
has no OpenCV dependency.

## Model-driven normalization

`QRGridNormalizer` performs these bounded steps:

- Convert the source to grayscale once, apply Otsu thresholding, and build
  binary and integral images.
- Downsample only the detection layer so its longest side is at most 320
  pixels. Coarse QR-model scanning is therefore bounded independently of source
  resolution.
- Treat connected-component ROI, foreground/PCA, paper-quadrilateral, and
  full-image information only as candidate sources. No ROI is authoritative,
  and the complete detection image is always scanned.
- Collect global, tiled, and region module-size hints. For every retained hint,
  search every QR Version 1 through 10 over all proposed regions and then the
  full detection layer.
- Search bounded physical rotation angles separately from the four semantic QR
  orientations. Score Version 1–10 fixed evidence: finder patterns and
  separators, timing patterns, alignment patterns, the dark module, Version
  7–10 version information, and module purity.
- Merge axis/ROI, paper, and model hypotheses into one fair common pool of at
  most eight geometrically distinct coarse candidates.
- Map candidates back to the source image and continuously refine translation,
  independent X/Y scale, rotation, and evidence-gated perspective corners at
  submodule steps. Rendering samples the original source, not the <=320
  detection layer.
- Render at most three distinct candidates. Fixed modules receive canonical
  values, data modules use continuous inner-region sampling, each output module
  is `8x8` pixels, and the image has a four-module quiet zone.

## Real regression manifest and fresh result

`scripts/verify_qr_repair.py` checks these exact 38 files and payloads:

- `qr_fig/version1/123.png` plus `123_destroy1.png` through
  `123_destroy8.png` (9): `123`
- `qr_fig/version5/1.png` through `5.png` (5): `version5`
- `qr_fig/version10/1.png` through `4.png` (4): `version10`
- `qr_fig/print/1.png` (1): `333`
- `qr_fig/print/2.png` through `12.png` (11): `123`
- `qr_fig/Rotated/123_rotated1.png` through `123_rotated4.png` (4): `123`
- `qr_fig/Rotated/rotated5.png` and `rotated6.png` (2): `version5`
- `qr_fig/Rotated/rotated7.png` and `rotated8.png` (2): `version10`

Fresh Task6 verification on 2026-07-30:

```text
summary: 38/38, average=0.4268s, p95=0.7277s
```

This passes the required `0.5000s` average and `0.8000s` nearest-rank P95
limits. The command was:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/verify_qr_repair.py
```

## Frozen generalization evidence

The tuning corpus used seed `20260729` and reported `97/100` exact positives,
`0/50` negative false positives, `0.337s` average, and `0.672s` P95.

After constants were frozen, the untouched holdout was run exactly once with
seed `20260730`. It reported `98/100` exact positives, `0/50` negative false
positives, `0.364s` average, and `0.779s` P95. Its configuration SHA-256 was
`c712617004508cd0332774c5124694ad4637cc533fa9a577454287d69711b365`.
The protected holdout was not rerun after the tuning freeze; Task6 preserves
that one-shot result.

## Limits

Recovery is best effort when payload corruption exceeds Reed-Solomon capacity.
Strong perspective, curved surfaces, and damaged versions above Version 10 may
still require preprocessing outside this bounded repair path.
