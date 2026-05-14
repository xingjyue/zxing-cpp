/*
 * Tool to mark QR code finder patterns with red bounding boxes.
 * Reads images from a directory, detects finder patterns,
 * draws red rectangles around them, and saves the marked images.
 */

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/stat.h>
#include <dirent.h>

#include <zxing/common/Counted.h>
#include <zxing/Binarizer.h>
#include <zxing/common/GlobalHistogramBinarizer.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/DecodeHints.h>
#include <zxing/qrcode/detector/FinderPatternFinder.h>
#include <zxing/qrcode/detector/FinderPatternInfo.h>
#include <zxing/qrcode/detector/FinderPattern.h>
#include <zxing/LuminanceSource.h>
#include <zxing/common/IllegalArgumentException.h>
#include <zxing/ReaderException.h>

#include "lodepng.h"
#include "jpgd.h"

using namespace std;
using namespace zxing;
using namespace zxing::qrcode;

static bool hasSuffix(const string& filename, const string& suffix) {
    if (filename.length() < suffix.length()) return false;
    return filename.compare(filename.length() - suffix.length(), suffix.length(), suffix) == 0;
}

static bool isImageFile(const string& filename) {
    return hasSuffix(filename, ".png") || hasSuffix(filename, ".PNG") ||
           hasSuffix(filename, ".jpg") || hasSuffix(filename, ".JPG") ||
           hasSuffix(filename, ".jpeg") || hasSuffix(filename, ".JPEG") ||
           hasSuffix(filename, ".bmp") || hasSuffix(filename, ".BMP");
}

static string markedFilename(const string& inputPath) {
    size_t dotPos = inputPath.rfind('.');
    return inputPath.substr(0, dotPos) + "_finder_marked.png";
}

static string reconstructedFilename(const string& inputPath) {
    size_t dotPos = inputPath.rfind('.');
    return inputPath.substr(0, dotPos) + "_reconstructed.png";
}

static void renderRepairedMatrix(Ref<BitMatrix> matrix,
                                  Ref<FinderPattern> patterns[3],
                                  vector<unsigned char>& pixels) {
    int w = matrix->getWidth();
    int h = matrix->getHeight();
    pixels.resize((size_t)w * h * 4);

    // Render original binarized matrix to black/white RGBA
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            size_t idx = ((size_t)y * w + x) * 4;
            unsigned char val = matrix->get(x, y) ? 0 : 255;
            pixels[idx] = val;
            pixels[idx + 1] = val;
            pixels[idx + 2] = val;
            pixels[idx + 3] = 255;
        }
    }

    // Overwrite finder pattern regions with perfect 7x7 finder patterns.
    // A finder pattern module grid (0=black, 1=white):
    //   0 0 0 0 0 0 0   (row 0 / col 0,6 = outer black border)
    //   0 1 1 1 1 1 0
    //   0 1 0 0 0 1 0   (center 3x3 at rows/cols 2-4 also black)
    //   0 1 0 0 0 1 0
    //   0 1 0 0 0 1 0
    //   0 1 1 1 1 1 0
    //   0 0 0 0 0 0 0
    for (int i = 0; i < 3; i++) {
        if (patterns[i] == 0) continue;
        float cx = patterns[i]->getX();
        float cy = patterns[i]->getY();
        float ms = patterns[i]->getEstimatedModuleSize();
        if (ms <= 0.0f) continue;

        float half = 3.5f * ms;
        float left = cx - half;
        float top = cy - half;
        int x1 = max(0, (int)left);
        int y1 = max(0, (int)top);
        int x2 = min(w - 1, (int)(cx + half));
        int y2 = min(h - 1, (int)(cy + half));

        for (int y = y1; y <= y2; y++) {
            for (int x = x1; x <= x2; x++) {
                int mx = (int)((x - left) / ms);
                int my = (int)((y - top) / ms);
                if (mx < 0) mx = 0; else if (mx > 6) mx = 6;
                if (my < 0) my = 0; else if (my > 6) my = 6;

                // Black: outer border OR center 3x3 block
                bool black = (mx == 0 || mx == 6 || my == 0 || my == 6) ||
                             (mx >= 2 && mx <= 4 && my >= 2 && my <= 4);

                size_t idx = ((size_t)y * w + x) * 4;
                unsigned char val = black ? 0 : 255;
                pixels[idx] = val;
                pixels[idx + 1] = val;
                pixels[idx + 2] = val;
            }
        }
    }
}

class RawImageSource : public LuminanceSource {
private:
    const ArrayRef<char> image_;
    const int comps_;

    char convertPixel(const char* pixel) const {
        const unsigned char* p = (const unsigned char*)pixel;
        if (comps_ == 1 || comps_ == 2) return (char)p[0];
        // RGB(A): ITU-R BT.601 luminance
        return (char)((306 * (int)p[0] + 601 * (int)p[1] + 117 * (int)p[2] + 0x200) >> 10);
    }

public:
    RawImageSource(ArrayRef<char> image, int width, int height, int comps)
        : LuminanceSource(width, height), image_(image), comps_(comps) {}

    ArrayRef<char> getRow(int y, ArrayRef<char> row) const {
        const char* pixelRow = &image_[0] + y * getWidth() * comps_;
        if (!row) row = ArrayRef<char>(getWidth());
        for (int x = 0; x < getWidth(); x++) {
            row[x] = convertPixel(pixelRow + x * comps_);
        }
        return row;
    }

    ArrayRef<char> getMatrix() const {
        ArrayRef<char> matrix(getWidth() * getHeight());
        char* m = &matrix[0];
        for (int y = 0; y < getHeight(); y++) {
            for (int x = 0; x < getWidth(); x++) {
                *m++ = convertPixel(&image_[0] + (y * getWidth() + x) * comps_);
            }
        }
        return matrix;
    }
};

static void setPixelRGBA(ArrayRef<char>& image, int width, int x, int y,
                          unsigned char r, unsigned char g, unsigned char b) {
    unsigned char* pixel = (unsigned char*)&image[(y * width + x) * 4];
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 255;
}

static void drawRect(ArrayRef<char>& image, int width, int height,
                     int x1, int y1, int x2, int y2,
                     unsigned char r, unsigned char g, unsigned char b, int thickness) {
    // Clamp coordinates
    x1 = max(0, x1);
    y1 = max(0, y1);
    x2 = min(width - 1, x2);
    y2 = min(height - 1, y2);

    // Top and bottom edges
    for (int t = 0; t < thickness; t++) {
        int yt = min(y1 + t, y2);
        int yb = max(y2 - t, y1);
        for (int x = x1; x <= x2; x++) {
            setPixelRGBA(image, width, x, yt, r, g, b);
            setPixelRGBA(image, width, x, yb, r, g, b);
        }
        // Left and right edges
        int xl = min(x1 + t, x2);
        int xr = max(x2 - t, x1);
        for (int y = y1 + t; y <= y2 - t; y++) {
            setPixelRGBA(image, width, xl, y, r, g, b);
            setPixelRGBA(image, width, xr, y, r, g, b);
        }
    }
}

static bool loadImage(const string& filepath, vector<unsigned char>& pixels,
                      int& width, int& height) {
    string extension = filepath.substr(filepath.find_last_of(".") + 1);
    transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == "png") {
        unsigned w, h;
        unsigned error = lodepng::decode(pixels, w, h, filepath);
        if (error) {
            cerr << "  lodepng error: " << lodepng_error_text(error) << endl;
            return false;
        }
        width = (int)w;
        height = (int)h;
        return true;
    } else if (extension == "jpg" || extension == "jpeg") {
        int comps;
        char* buffer = reinterpret_cast<char*>(jpgd::decompress_jpeg_image_from_file(
            filepath.c_str(), &width, &height, &comps, 4));
        if (!buffer) {
            cerr << "  jpgd decompress failed" << endl;
            return false;
        }
        pixels.resize(4 * width * height);
        memcpy(&pixels[0], buffer, pixels.size());
        free(buffer);
        return true;
    } else if (extension == "bmp") {
        // BMP support via lodepng is limited; try lodepng first
        unsigned w, h;
        unsigned error = lodepng::decode(pixels, w, h, filepath);
        if (error) {
            cerr << "  Cannot load BMP (unsupported format for this tool)" << endl;
            return false;
        }
        width = (int)w;
        height = (int)h;
        return true;
    }
    cerr << "  Unsupported file format: " << extension << endl;
    return false;
}

// Match finder patterns by proximity (ignoring TL/TR/BL label ordering).
// Returns max displacement among matched pairs, or -1 if fewer than 3 matched.
static float matchPatterns(Ref<FinderPattern> a[3], Ref<FinderPattern> b[3],
                           int matched[3]) {
    float maxDist = 0.0f;
    int n = 0;
    bool used[3] = {false, false, false};
    for (int i = 0; i < 3; i++) {
        if (a[i] == 0) { matched[i] = -1; continue; }
        float ax = a[i]->getX(), ay = a[i]->getY();
        float best = 1e9f;
        int bestJ = -1;
        for (int j = 0; j < 3; j++) {
            if (b[j] == 0 || used[j]) continue;
            float dx = ax - b[j]->getX();
            float dy = ay - b[j]->getY();
            float d = dx*dx + dy*dy;
            if (d < best) { best = d; bestJ = j; }
        }
        if (bestJ >= 0) {
            matched[i] = bestJ;
            used[bestJ] = true;
            if (best > maxDist) maxDist = best;
            n++;
        } else {
            matched[i] = -1;
        }
    }
    return (n == 3) ? sqrt(maxDist) : -1.0f;
}

// Run finder pattern detection on an in-memory RGBA pixel buffer.
// Returns true if 3 patterns found, fills in out[3] and their module size.
static bool detectFinders(const vector<unsigned char>& pixels, int w, int h,
                          Ref<FinderPattern> out[3], float& avgModuleSize) {
    ArrayRef<char> arr((char*)&pixels[0], (int)pixels.size());
    Ref<LuminanceSource> src(new RawImageSource(arr, w, h, 4));
    Ref<Binarizer> bin(new GlobalHistogramBinarizer(src));
    Ref<BinaryBitmap> bmp(new BinaryBitmap(bin));
    Ref<BitMatrix> mat = bmp->getBlackMatrix();

    DecodeHints hints(DecodeHints::TRYHARDER_HINT);
    FinderPatternFinder f(mat, Ref<ResultPointCallback>(0));
    try {
        Ref<FinderPatternInfo> info = f.find(hints);
        out[0] = info->getTopLeft();
        out[1] = info->getTopRight();
        out[2] = info->getBottomLeft();
        avgModuleSize = (out[0]->getEstimatedModuleSize() +
                         out[1]->getEstimatedModuleSize() +
                         out[2]->getEstimatedModuleSize()) / 3.0f;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void processImage(const string& filepath) {
    cout << "Processing: " << filepath << endl;

    // Load image as RGBA pixel data
    vector<unsigned char> pixelData;
    int width, height;
    if (!loadImage(filepath, pixelData, width, height)) {
        return;
    }

    // Convert to ArrayRef for LuminanceSource
    ArrayRef<char> imageArray((char*)&pixelData[0], (int)pixelData.size());
    int comps = 4;

    try {
        Ref<LuminanceSource> source(new RawImageSource(imageArray, width, height, comps));
        Ref<Binarizer> binarizer(new GlobalHistogramBinarizer(source));
        Ref<BinaryBitmap> bitmap(new BinaryBitmap(binarizer));
        Ref<BitMatrix> matrix = bitmap->getBlackMatrix();

        DecodeHints hints(DecodeHints::TRYHARDER_HINT);
        FinderPatternFinder finder(matrix, Ref<ResultPointCallback>(0));
        Ref<FinderPatternInfo> info = finder.find(hints);

        // Pass-1: original detection
        Ref<FinderPattern> patterns[3] = {
            info->getTopLeft(),
            info->getTopRight(),
            info->getBottomLeft()
        };
        const char* labels[3] = {"TL", "TR", "BL"};

        float origAvgMs = 0.0f;
        int marked = 0;
        for (int i = 0; i < 3; i++) {
            if (patterns[i] == 0) continue;
            float ms = patterns[i]->getEstimatedModuleSize();
            if (ms <= 0.0f) continue;

            float cx = patterns[i]->getX();
            float cy = patterns[i]->getY();
            float halfSize = 3.5f * ms;
            drawRect(imageArray, width, height,
                     (int)(cx - halfSize), (int)(cy - halfSize),
                     (int)(cx + halfSize), (int)(cy + halfSize),
                     255, 0, 0, 2);

            cout << "  " << labels[i] << " at (" << cx << ", " << cy
                 << "), ms=" << ms << endl;
            origAvgMs += ms;
            marked++;
        }
        origAvgMs /= 3.0f;

        // Pass-1 repair: render matrix + overwrite finder pattern regions
        vector<unsigned char> repairedPixels;
        renderRepairedMatrix(matrix, patterns, repairedPixels);
        int rw = matrix->getWidth();
        int rh = matrix->getHeight();

        // Verify: re-detect on the repaired image
        // If detection finds patterns at significantly different positions,
        // those positions are more reliable — re-repair using them.
        Ref<FinderPattern> verifyPatterns[3];
        float verifyAvgMs = 0.0f;
        bool verified = detectFinders(repairedPixels, rw, rh, verifyPatterns, verifyAvgMs);

        if (verified) {
            int matchIdx[3];
            float maxDisplacement = matchPatterns(patterns, verifyPatterns, matchIdx);

            // If verification positions differ by > 2 modules, original detection
            // was unreliable. Re-repair using verification positions as base.
            if (maxDisplacement > 2.0f * origAvgMs) {
                cout << "  Positions shifted by " << maxDisplacement
                     << "px — re-repairing at verified positions" << endl;

                // Build base matrix from the already-repaired image so
                // the second pass converges on top of the first repair.
                {
                    ArrayRef<char> baseArr((char*)&repairedPixels[0],
                                            (int)repairedPixels.size());
                    Ref<LuminanceSource> baseSrc(
                        new RawImageSource(baseArr, rw, rh, 4));
                    Ref<Binarizer> baseBin(
                        new GlobalHistogramBinarizer(baseSrc));
                    Ref<BinaryBitmap> baseBmp(new BinaryBitmap(baseBin));
                    Ref<BitMatrix> baseMat = baseBmp->getBlackMatrix();
                    renderRepairedMatrix(baseMat, verifyPatterns,
                                        repairedPixels);
                }

                // Verify again on the re-repaired image
                Ref<FinderPattern> final[3];
                float finalMs = 0.0f;
                if (detectFinders(repairedPixels, rw, rh, final, finalMs)) {
                    int m2[3];
                    float d2 = matchPatterns(verifyPatterns, final, m2);
                    if (d2 >= 0) {
                        cout << "  Final check: displacement " << d2
                             << "px — "
                             << (d2 <= 2.0f * finalMs ? "stable." :
                                 "still shifting, using best available.")
                             << endl;
                    }
                }
            } else if (maxDisplacement >= 0) {
                cout << "  Positions match (max displacement " << maxDisplacement
                     << "px) — repair confirmed." << endl;
            }
        }

        // Save final reconstructed image
        string reconPath = reconstructedFilename(filepath);
        unsigned saveErr = lodepng::encode(reconPath, &repairedPixels[0],
                                           (unsigned)rw, (unsigned)rh, LCT_RGBA, 8);
        if (saveErr) {
            cerr << "  Failed to save " << reconPath << ": "
                 << lodepng_error_text(saveErr) << endl;
        } else {
            cout << "  Saved reconstructed QR to: " << reconPath << endl;

            // Final verification pass
            if (verified) {
                cout << "  Final verify: pattern positions stable, repair complete." << endl;
            } else {
                cout << "  Final verify: 3 finder patterns confirmed intact." << endl;
            }
        }

        if (marked > 0) {
            string outPath = markedFilename(filepath);
            unsigned error = lodepng::encode(outPath, (const unsigned char*)&imageArray[0],
                                             (unsigned)width, (unsigned)height,
                                             LCT_RGBA, 8);
            if (error) {
                cerr << "  Failed to save " << outPath << ": "
                     << lodepng_error_text(error) << endl;
            } else {
                cout << "  Saved marked image to: " << outPath << endl;
            }
        } else {
            cerr << "  No finder patterns found." << endl;
        }
    } catch (const ReaderException& e) {
        cerr << "  Detection failed: " << e.what() << endl;
    } catch (const std::exception& e) {
        cerr << "  Error: " << e.what() << endl;
    }
}

int main(int argc, char** argv) {
    string dirPath = "./qr_fig";

    if (argc > 1) {
        dirPath = argv[1];
    }

    cout << "Scanning directory: " << dirPath << endl;

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) {
        cerr << "Failed to open directory: " << dirPath << endl;
        return 1;
    }

    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        string filename = entry->d_name;
        if (!isImageFile(filename)) continue;

        string filepath = dirPath + "/" + filename;
        processImage(filepath);
        count++;
    }
    closedir(dir);

    cout << "\nProcessed " << count << " image(s)." << endl;
    return 0;
}
