# QRCodeReader In-Library Normalize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After primary QR decode fails, `QRCodeReader` always runs `NormalizeQR` on the grayscale luminance matrix and retries up to three normalized candidates; CLI stops calling `createNormalizedCandidates` and routes timed QR decode through `QRCodeReader`.

**Architecture:** Factor `decodeOnce` for Detector→Decoder. On `ReaderException`, build `MutableImage` (`components=1`) from `getMatrix()`, call `NormalizeQR(..., 3)`, convert each RGBA candidate to gray, rebuild `BinaryBitmap` via `binarizer->createBinarizer`, and `decodeOnce` again. CLI removes explicit repair orchestration; timing option 1 leaves `repair_ms` at 0.

**Tech Stack:** C++11, existing `libzxing` / CLI `zxing`, `scripts/verify_qr_repair.py`.

**Spec:** `docs/superpowers/specs/2026-08-05-qrcode-reader-normalize-design.md`

**Commits:** Only if the user explicitly asks; otherwise leave changes uncommitted.

---

## File Structure

- Modify: `core/src/zxing/BinaryBitmap.h` / `.cpp` — expose `getBinarizer()` so retries reuse the same binarizer family.
- Modify: `core/src/zxing/qrcode/QRCodeReader.h` / `.cpp` — `decodeOnce` + normalize fallback.
- Modify: `cli/src/main.cpp` — remove CLI-side repair; `timed_qr_decode` uses `QRCodeReader`.
- Unchanged API: `NormalizeQR` in `QRGridNormalizer.h`.
- Keep: `ImageReaderSource::createNormalizedCandidates` for contract tests.

---

### Task 1: Expose `BinaryBitmap::getBinarizer`

**Files:**
- Modify: `core/src/zxing/BinaryBitmap.h`
- Modify: `core/src/zxing/BinaryBitmap.cpp`

- [ ] **Step 1: Add accessor to the header**

In `BinaryBitmap` public section, after `getLuminanceSource()`, add:

```cpp
Ref<Binarizer> getBinarizer() const;
```

Ensure `Binarizer.h` is already included (it is via the existing include).

- [ ] **Step 2: Implement**

```cpp
Ref<Binarizer> BinaryBitmap::getBinarizer() const {
  return binarizer_;
}
```

- [ ] **Step 3: Build libzxing object**

```bash
cmake --build build --target libzxing -j4
```

Expected: success.

- [ ] **Step 4: Commit** — SKIP unless user asked.

---

### Task 2: Integrate normalize fallback into `QRCodeReader`

**Files:**
- Modify: `core/src/zxing/qrcode/QRCodeReader.h`
- Modify: `core/src/zxing/qrcode/QRCodeReader.cpp`

- [ ] **Step 1: Extend the header**

```cpp
#include <zxing/Reader.h>
#include <zxing/qrcode/decoder/Decoder.h>
#include <zxing/DecodeHints.h>

namespace zxing {
namespace qrcode {

class QRCodeReader : public Reader {
 private:
  Decoder decoder_;

  Ref<Result> decodeOnce(Ref<BinaryBitmap> image, DecodeHints hints);

 protected:
  Decoder& getDecoder();

 public:
  QRCodeReader();
  virtual ~QRCodeReader();

  Ref<Result> decode(Ref<BinaryBitmap> image, DecodeHints hints);
};

}
}
```

- [ ] **Step 2: Rewrite `QRCodeReader.cpp`**

Replace the body with (adapt includes to match repo style):

```cpp
#include <zxing/qrcode/QRCodeReader.h>
#include <zxing/qrcode/detector/Detector.h>
#include <zxing/qrcode/QRGridNormalizer.h>
#include <zxing/common/GreyscaleLuminanceSource.h>
#include <zxing/ReaderException.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/Binarizer.h>

#include <vector>

namespace zxing {
namespace qrcode {

namespace {

ArrayRef<char> RgbaToGray(const NormalizedImage& image) {
  const int w = image.width;
  const int h = image.height;
  ArrayRef<char> gray(w * h);
  if (image.components == 1) {
    std::memcpy(&gray[0], &image.pixels[0], static_cast<size_t>(w) * h);
    return gray;
  }
  const char* pixels = &image.pixels[0];
  for (int i = 0; i < w * h; ++i) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(
        pixels + i * image.components);
    if (image.components >= 3) {
      gray[i] = static_cast<char>(
          (306 * static_cast<int>(p[0]) +
           601 * static_cast<int>(p[1]) +
           117 * static_cast<int>(p[2]) + 0x200) >> 10);
    } else {
      gray[i] = static_cast<char>(p[0]);
    }
  }
  return gray;
}

}  // namespace

QRCodeReader::QRCodeReader() : decoder_() {}

QRCodeReader::~QRCodeReader() {}

Decoder& QRCodeReader::getDecoder() {
  return decoder_;
}

Ref<Result> QRCodeReader::decodeOnce(Ref<BinaryBitmap> image, DecodeHints hints) {
  Detector detector(image->getBlackMatrix());
  Ref<DetectorResult> detectorResult(detector.detect(hints));
  ArrayRef< Ref<ResultPoint> > points(detectorResult->getPoints());
  Ref<DecoderResult> decoderResult(decoder_.decode(detectorResult->getBits()));
  return Ref<Result>(new Result(
      decoderResult->getText(), decoderResult->getRawBytes(), points,
      BarcodeFormat::QR_CODE));
}

Ref<Result> QRCodeReader::decode(Ref<BinaryBitmap> image, DecodeHints hints) {
  try {
    return decodeOnce(image, hints);
  } catch (ReaderException const& primary) {
    Ref<LuminanceSource> source = image->getLuminanceSource();
    if (!source || source->getWidth() <= 0 || source->getHeight() <= 0) {
      throw primary;
    }
    MutableImage mutableImage = {
        source->getMatrix(), source->getWidth(), source->getHeight(), 1};
    std::vector<NormalizedImage> candidates;
    try {
      candidates = NormalizeQR(mutableImage, 3);
    } catch (std::bad_alloc const&) {
      throw;
    }
    if (candidates.empty()) {
      throw primary;
    }
    Ref<Binarizer> binarizer = image->getBinarizer();
    for (size_t i = 0; i < candidates.size(); ++i) {
      ArrayRef<char> gray = RgbaToGray(candidates[i]);
      const int size = candidates[i].width;
      Ref<LuminanceSource> normalizedSource(new GreyscaleLuminanceSource(
          gray, size, size, 0, 0, size, size));
      Ref<BinaryBitmap> normalizedBitmap(
          new BinaryBitmap(binarizer->createBinarizer(normalizedSource)));
      try {
        return decodeOnce(normalizedBitmap, hints);
      } catch (ReaderException const&) {
        continue;
      }
    }
    throw primary;
  }
}

}
}
```

Add `#include <cstring>` if using `memcpy`. Include `BarcodeFormat.h` / `Result.h` / `DetectorResult.h` as required by existing compile errors (match whatever the old file pulled transitively; add explicit includes if the build complains).

- [ ] **Step 3: Build**

```bash
cmake --build build --target libzxing -j4
```

Expected: success. Fix missing includes if reported.

- [ ] **Step 4: Commit** — SKIP unless user asked.

---

### Task 3: Point CLI timed QR path at `QRCodeReader` and remove CLI repair

**Files:**
- Modify: `cli/src/main.cpp`

- [ ] **Step 1: Update includes**

Ensure:

```cpp
#include <zxing/qrcode/QRCodeReader.h>
```

Remove unused detector/decoder includes from the timed path if they become unused after Step 2 (keep if still referenced elsewhere in the file).

- [ ] **Step 2: Replace `timed_qr_decode`**

Replace the Detector/Decoder split implementation with:

```cpp
vector<Ref<Result> > timed_qr_decode(
    Ref<BinaryBitmap> image, DecodeHints hints, TimingPair& timing) {
  SteadyTime t0 = SteadyClock::now();
  try {
    qrcode::QRCodeReader reader;
    Ref<Result> result = reader.decode(image, hints);
    // Timing option 1: attribute the whole reader call (including any
    // in-library normalize) to decodeMs; detectMs stays 0 for this path.
    timing.decodeMs += elapsed_ms(t0, SteadyClock::now());
    return vector<Ref<Result> >(1, result);
  } catch (...) {
    timing.decodeMs += elapsed_ms(t0, SteadyClock::now());
    throw;
  }
}
```

Use the correct namespace qualification (`zxing::qrcode::QRCodeReader`) consistent with existing `using` directives in `main.cpp`.

- [ ] **Step 3: Simplify `process_image` — remove CLI repair**

Delete:

- `DecodeNormalizedCandidates` helper if it becomes unused
- `repairFirst` / `triedRepair` / `run_repair` lambda and both call sites
- `kRepaired` phase usage for normalize (keep `PhaseTiming` fields; leave `repairMs` / repaired_* at 0)

Keep the outer flow as:

```cpp
phaseKind = kFirst;
attempt_decode(source, try_harder, use_hybrid || !use_global,
               !dualBinarizer || test_mode);
if (hybridResult != 0 && globalResult != 0 && dualBinarizer && !test_mode) {
  attempt_decode(source, try_harder, false, true);
}
if (hybridResult != 0 && globalResult != 0 && !try_harder) {
  attempt_decode(source, true, true, true);
}
```

(Adjust to match current variable names; remove only repair-specific branches.)

- [ ] **Step 4: Build CLI**

```bash
cmake --build build --target zxing -j4
```

Expected: success.

- [ ] **Step 5: Commit** — SKIP unless user asked.

---

### Task 4: Verify

**Files:** none (run only)

- [ ] **Step 1: Fast-path smoke**

```bash
./build/zxing qr_fig/version1/123.png
```

Expected: prints `123`. Timing may show `repair_ms=0` and non-zero first decode.

- [ ] **Step 2: Repair-path smoke**

```bash
./build/zxing qr_fig/Rotated/123_rotated1.png
./build/zxing qr_fig/version1/123_destroy1.png
./build/zxing qr_fig/print/10.png
```

Expected: each prints the known payload (`123` / `123` / `123`).

- [ ] **Step 3: Available-sample verifier**

```bash
python3 scripts/verify_qr_repair.py \
  version1/123.png version1/123_destroy1.png version1/123_destroy2.png \
  version1/123_destroy3.png version1/123_destroy4.png version1/123_destroy5.png \
  version1/123_destroy6.png version1/123_destroy7.png version1/123_destroy8.png \
  version5/1.png version5/2.png version5/3.png version5/4.png version5/5.png \
  version10/1.png version10/2.png version10/3.png version10/4.png \
  print/1.png print/2.png print/3.png print/4.png print/5.png print/6.png \
  print/7.png print/8.png print/9.png print/10.png print/11.png print/12.png \
  Rotated/123_rotated1.png Rotated/123_rotated2.png \
  Rotated/rotated5.png Rotated/rotated6.png Rotated/rotated7.png Rotated/rotated8.png
```

Expected: `36/36` (or all listed files present) pass latency budgets.

- [ ] **Step 4: Confirm no CLI normalize call on default path**

```bash
grep -n createNormalizedCandidates cli/src/main.cpp || true
```

Expected: no matches in `main.cpp` (method may still exist on `ImageReaderSource`).

---

## Spec Coverage

| Spec item | Task |
|-----------|------|
| `decodeOnce` + normalize on `ReaderException` | Task 2 |
| Grayscale `MutableImage` input | Task 2 |
| RGBA→gray for candidates | Task 2 |
| Same binarizer family via `createBinarizer` | Task 1 + 2 |
| CLI remove `createNormalizedCandidates` usage | Task 3 |
| `timed_qr_decode` uses `QRCodeReader` | Task 3 |
| Timing option 1 (`repair_ms` unused / 0) | Task 3 |
| Build + sample verification | Task 4 |

## Self-Review Notes

- `BinaryBitmap::getBinarizer` is required because the binarizer was previously private.
- `throw primary` requires `ReaderException` to be copyable (it is in this tree); if copy fails to compile, rethrow with `throw ReaderException(primary.what());`.
- Do not catch `bad_alloc` from `NormalizeQR` and convert to “not found”.
