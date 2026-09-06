# QRCodeReader In-Library Grid Normalization Design

## Goal

After the ordinary QR detect/decode path fails, `QRCodeReader` automatically
runs grid normalization and retries decode on up to three normalized
candidates—without a DecodeHints switch (always on).

CLI keeps hybrid/global/`try-harder` orchestration but stops calling
`createNormalizedCandidates`; repair happens inside the reader. Timing stays
minimal: no separate `repair_ms` attribution for in-reader normalize (cost is
folded into the existing decode/attempt timing).

## Decisions (locked)

| Topic | Choice |
|-------|--------|
| When to run | Always after primary decode failure (default on) |
| CLI | Remove explicit normalize calls; keep outer decode loop; thin timing |
| Pixel feed | Grayscale passthrough: `getMatrix()` → `MutableImage` with `components=1` |
| Structure | Integrate in `QRCodeReader`; switch CLI `timed_qr_decode` to use `QRCodeReader` |
| Timing | Option 1: no dedicated `repair_ms` for library normalize |

## Architecture

```text
QRCodeReader::decode(BinaryBitmap, hints)
  try:
    Detector(blackMatrix) → Decoder → Result
  catch failure:
    source = image->getLuminanceSource()
    MutableImage { source->getMatrix(), w, h, components=1 }
    candidates = NormalizeQR(mutable, 3)
    for each candidate:
      GreyscaleLuminanceSource(normalized gray)
      BinaryBitmap via binarizer->createBinarizer(newSource)
      try Detector → Decoder → return Result on success
    rethrow original failure (or last failure if original was consumed)
```

`NormalizeQR` remains the sole public normalizer API. No new DecodeHints.

## Components

### 1. `QRCodeReader.cpp` / `.h`

- Keep public `decode(Ref<BinaryBitmap>, DecodeHints)` signature unchanged.
- Factor a private `decodeOnce(Ref<BinaryBitmap>, DecodeHints)` for the
  existing Detector→Decoder body to avoid duplicating success-path code.
- On failure of `decodeOnce` on the input image:
  - Build `MutableImage` from luminance matrix (`components = 1`).
  - Call `NormalizeQR(image, 3)`.
  - For each `NormalizedImage`, build `GreyscaleLuminanceSource` with
    `dataWidth = dataHeight = width` (square module image, typically RGBA
    rendered as 4-component today—**see pixel format note below**).
  - Recreate `BinaryBitmap` with the **same binarizer family** as the input
    (`image`’s binarizer `createBinarizer(newSource)`), then `decodeOnce`.
  - Return first success; if all fail, rethrow the exception from the
    original attempt.

**Pixel format note:** Current `NormalizeQR` renders candidates as RGBA
(`components = 4`). For the retry path, either:

- Prefer converting each candidate to grayscale once when wrapping
  `GreyscaleLuminanceSource` (R channel or existing Rec.601), or
- Change render output to grayscale later (out of this phase’s minimum scope).

Minimum for this phase: wrap normalized RGBA by extracting luminance into a
gray `ArrayRef` for `GreyscaleLuminanceSource` (same formula as CLI
`convertPixel` for RGB). Do not feed 4-byte RGBA into `GreyscaleLuminanceSource`
as if it were gray.

### 2. CLI `main.cpp`

- Remove `run_repair` / `createNormalizedCandidates` usage and the large-image
  `repairFirst` branch that only existed to front-load CLI-side normalize.
- Change `timed_qr_decode` to call `QRCodeReader::decode` (so verify scripts
  actually hit in-library repair). Detect/decode split timing may collapse to
  a single timed `decode` call attributed to decode (or split best-effort
  around one call—acceptable under timing option 1).
- Leave hybrid vs global and final `try-harder` outer attempts as they are.
- `repair_ms` may remain in the timing struct printed as `0` (or omit meaningful
  repair phase); document that normalize cost is inside reader decode.

### 3. CLI `ImageReaderSource`

- `createNormalizedCandidates` may remain for tests/compat but is no longer
  required on the main decode path. Prefer leaving the method for
  `task4_api_contract_test` unless removing it is trivial; not required for
  Reader integration.

## Error handling

- Primary path exceptions that mean “no QR found” (typically `ReaderException`
  and subclasses used by detector/decoder) trigger normalize retry.
- Do not catch unrelated errors (e.g. `bad_alloc` from normalize) and pretend
  success; propagate allocation failures.
- If normalize returns empty, rethrow the original primary failure.

## Non-goals

- DecodeHints flag for normalize on/off.
- Restoring multi-angle search or changing detection-layer size.
- Accurate `repair_ms` accounting.
- Feeding RGBA buffers directly into grayscale sources without conversion.
- Changing `MultiFormatReader` beyond whatever it already does by delegating
  to `QRCodeReader` (QR path benefits automatically).

## Testing

1. Build `zxing`.
2. Clean sample (`version1/123.png`): succeeds without needing normalize
   (fast path).
3. Damaged / rotated set via `scripts/verify_qr_repair.py` (available
   samples): still decode expected payloads.
4. Confirm CLI no longer depends on `createNormalizedCandidates` for the
   default single-QR timed path.

## Acceptance

- Primary failure in `QRCodeReader` triggers at most one `NormalizeQR(..., 3)`
  and up to three decode retries.
- Grayscale matrix from the original `LuminanceSource` is the normalize input.
- CLI default path uses `QRCodeReader` (not a raw Detector-only timed path).
- No separate repair timing requirement beyond existing prints.
