# AGENTS.md

## Cursor Cloud specific instructions

This is a **C++ CMake project** (zxing-cpp — a barcode/QR code decoding library). There are no web services, databases, or package managers beyond apt.

### Build

The default compiler detected by CMake on this VM is Clang, which fails to link due to a missing `-lstdc++` issue. **You must force GCC** when configuring:

```bash
mkdir -p build && cd build
cmake -G "Unix Makefiles" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON ..
make -j$(nproc)
```

This produces three targets:
- `libzxing.a` — static library
- `zxing` — CLI executable (reads PNG/JPEG barcodes)
- `testrunner` — unit tests (requires `libcppunit-dev`)

### Test

```bash
cd build && ./testrunner
```

Runs CppUnit-based unit tests (39 tests covering BitArray, BitMatrix, Reed-Solomon, QR code components, etc.).

### Run (CLI)

```bash
./build/zxing [--more] [--verbose] [--try-harder] <image.png|image.jpg>
```

The CLI bundles its own PNG (lodepng) and JPEG (jpgd) decoders — no external image libraries needed.

### Key caveats

- The project enforces out-of-source builds; never run `cmake` from the repo root.
- `BUILD_TESTING=ON` is required to build the `testrunner` target (off by default).
- OpenCV support is optional; if `libopencv-dev` is installed, the `zxing-cv` target is built automatically.
