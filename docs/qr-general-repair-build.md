# General QR Repair Pipeline and Build Notes

## Runtime flow

The `zxing` CLI uses a bounded normal-to-model sequence:

1. normal decode of the loaded original image;
2. after failure, model-driven normalization and ordered decode of at most
   three distinct candidates;
3. after all normalized candidates fail, one `try-harder` decode of the
   original image, unless `--try-harder` was already requested.

Normalization searches every Version 1 through 10 on a full-image detection
layer whose longest side is at most 320 pixels. ROI, paper, PCA, and module-run
regions are candidate sources only; none can suppress the full scan. Bounded
physical-angle hypotheses and four semantic QR orientations are scored with
Version 1–10 fixed evidence. Axis/ROI, paper, and model candidates enter one
fair Top-8 coarse pool, then continuous translation, scale, rotation, and
evidence-gated perspective refinement samples the original source image.

The image is loaded once, and no orientation reload loop is used.

## Build

Configure when necessary, then build only the required CLI:

```bash
cmake -S . -B build
cmake --build build --target zxing -j4
```

The production `zxing` C++ target has no OpenCV dependency. Python OpenCV and
NumPy below are test-only dependencies for deterministic image generation.

### Optional `zxing-cv` target

This is separate from the required `zxing` CLI. In the current configured tree,
`zxing-cv` is present, but its objects are arm64 while the configured Homebrew
OpenCV 4.13.0 dynamic libraries are x86_64. That optional architecture mismatch
does not affect `cmake --build build --target zxing`.

## Real-image verification

The standard-library-only verifier contains the exact 38-sample manifest:

- 9 files in `qr_fig/version1`
- 5 files in `qr_fig/version5`
- 4 files in `qr_fig/version10`
- 12 files in `qr_fig/print`, including `print/12.png`
- 8 files in `qr_fig/Rotated`

Run from the repository root:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B scripts/verify_qr_repair.py
```

The verifier requires exact payloads, average process latency at most `0.5s`,
and nearest-rank P95 at most `0.8s`. Fresh Task6 evidence from 2026-07-30:

```text
summary: 38/38, average=0.4268s, p95=0.7277s
```

The compatibility entry point runs the same manifest:

```bash
PYTHONDONTWRITEBYTECODE=1 scripts/verify_qr_damage_samples.sh
```

## Generalization verification

Install the pinned test-only generator dependencies:

```bash
python3 -m pip install -r scripts/requirements-qr-generalization.txt
```

The requirements pin `numpy==2.2.6` and
`opencv-python==4.13.0.92`. Generated positive and negative PNGs are kept in a
`TemporaryDirectory` and are removed after each run.

The reproducible tuning command is:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  scripts/verify_qr_generalization.py \
  --corpus tuning --positive-count 100 --negative-count 50
```

Seed `20260729` produced `97/100` exact positives, `0/50` negative false
positives, `0.337s` average, and `0.672s` P95.

The protected holdout command used after the final tuning freeze was:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  scripts/verify_qr_generalization.py \
  --corpus holdout --confirm-holdout \
  --positive-count 100 --negative-count 50
```

That seed-`20260730` holdout was run exactly once. It produced `98/100` exact
positives, `0/50` negative false positives, `0.364s` average, and `0.779s` P95,
with configuration SHA-256
`c712617004508cd0332774c5124694ad4637cc533fa9a577454287d69711b365`.
It was not rerun after the tuning freeze and must remain preserved as one-shot
evidence.

## Persistent regression tests

Run the Python unit and integration tests without creating bytecode:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest \
  scripts/test_verify_qr_generalization.py
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  scripts/test_task4_topk_integration.py
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  scripts/test_task5_version_pruning_integration.py
```

Compile standalone C++ contracts only to temporary output:

```bash
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/zxing-contracts.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

c++ -std=c++11 -Icore/src -Icli/src \
  scripts/task4_api_contract_test.cpp \
  cli/src/ImageReaderSource.cpp cli/src/QRGridNormalizer.cpp \
  cli/src/lodepng.cpp cli/src/jpgd.cpp build/libzxing.a \
  -o "$tmpdir/task4_api_contract_test"
"$tmpdir/task4_api_contract_test" qr_fig/print/12.png

c++ -std=c++11 -Icore/src -Icli/src \
  scripts/task4_coarse_pool_test.cpp build/libzxing.a \
  -o "$tmpdir/task4_coarse_pool_test"
"$tmpdir/task4_coarse_pool_test"

c++ -std=c++11 -Icore/src \
  scripts/task5_version_pruning_contract_test.cpp build/libzxing.a \
  -o "$tmpdir/task5_version_pruning_contract_test"
"$tmpdir/task5_version_pruning_contract_test"
```
