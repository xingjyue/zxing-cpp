/*
 * Copyright 2026 ZXing authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zxing/qrcode/QRGridNormalizer.h>

#include <zxing/qrcode/Version.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <stdint.h>
#include <utility>
#include <vector>

namespace zxing {
namespace qrcode {

namespace {

typedef std::chrono::steady_clock SteadyClock;

struct PointF {
  float x;
  float y;
};

struct BinaryImage {
  int width;
  int height;
  uint8_t threshold;
  std::vector<uint8_t> gray;
  std::vector<uint8_t> opacity;
  std::vector<uint8_t> black;
  std::vector<int> integral;
};

struct Bounds {
  int left;
  int top;
  int right;
  int bottom;
};

struct SearchRegion {
  Bounds bounds;
  float priority;
};

struct ScaleHint {
  float moduleSize;
  float support;
};

struct DetectionLayer {
  BinaryImage image;
  float sourceScaleX;
  float sourceScaleY;
};

struct AxisSource {
  Bounds bounds;
  bool canvasAnchors;
};

struct FixedModule {
  short x;
  short y;
  uint8_t black;
  uint8_t group;
};

struct GridCandidate {
  PointF corner[4];
  float score;
  int version;
  int orientation;
};

struct AxisGridSpec {
  float left;
  float top;
  float stepX;
  float stepY;
  int version;
};

struct AffineGridSpec {
  PointF center;
  PointF axisX;
  PointF axisY;
  float stepX;
  float stepY;
  int version;
};

struct ComponentGrid {
  int width;
  int height;
  int stride;
  std::vector<uint8_t> mask;
  std::vector<uint8_t> seen;
};

struct RoiComponent {
  Bounds cells;
};

struct PaperComponent {
  std::vector<PointF> points;
  size_t area;
};

struct BorderCounts {
  size_t total;
  size_t black;
  size_t transparent;
};

struct BorderEvidence {
  float outerBlack;
  float innerBlack;
  float outerTransparent;
  float innerTransparent;
  bool darkCanvas;
  bool useOpacity;
};

struct ModuleEstimate {
  float size;
  float support;
  bool reliable;
};

struct SampleResult {
  float black;
  float coverage;
};

struct ScoreResult {
  float score;
  float coverage;
};

struct ModelSearchStats {
  size_t origins;
  size_t prescorePasses;
  size_t fullScores;
  size_t reservedSeeds;
  size_t uniqueSeeds;
  size_t duplicateSeeds;
};

struct ModelAxes {
  PointF axisX;
  PointF axisY;
};

float Distance(PointF a, PointF b);

const size_t kMaxPixels = 8u * 1024u * 1024u;
const size_t kMaxWorkingBytes = 96u * 1024u * 1024u;
const size_t kAuxiliaryWorkingBytes = 8u * 1024u * 1024u;

bool CheckedImageSize(
    const MutableImage& image, bool copyInput, size_t& pixelCount) {
  pixelCount = 0;
  if (!image.pixels || image.width <= 0 || image.height <= 0 ||
      image.components < 1 || image.components > 4) {
    return false;
  }
  const size_t width = static_cast<size_t>(image.width);
  const size_t height = static_cast<size_t>(image.height);
  const size_t maxSize = std::numeric_limits<size_t>::max();
  const size_t maxInt = static_cast<size_t>(std::numeric_limits<int>::max());
  if (height > maxSize / width) {
    return false;
  }
  pixelCount = width * height;
  if (pixelCount > kMaxPixels || pixelCount > maxInt ||
      pixelCount > maxInt / static_cast<size_t>(image.components)) {
    return false;
  }
  const size_t bytes = pixelCount * static_cast<size_t>(image.components);
  if (static_cast<size_t>(image.pixels->size()) < bytes ||
      width + 1 > maxSize / (height + 1)) {
    return false;
  }
  const size_t integralCount = (width + 1) * (height + 1);
  if (integralCount > std::vector<int>().max_size()) {
    return false;
  }
  const size_t retainedBuffers = copyInput ? 2 : 1;
  if (bytes >
      (kMaxWorkingBytes - kAuxiliaryWorkingBytes) / retainedBuffers) {
    return false;
  }
  size_t workingBytes = retainedBuffers * bytes;
  workingBytes += kAuxiliaryWorkingBytes;
  if (pixelCount > (kMaxWorkingBytes - workingBytes) / 3) {
    return false;
  }
  workingBytes += 3 * pixelCount;
  if (width > maxSize - height) {
    return false;
  }
  const size_t lineCount = width + height;
  if (lineCount > (kMaxWorkingBytes - workingBytes) / sizeof(int)) {
    return false;
  }
  workingBytes += lineCount * sizeof(int);
  return integralCount <=
      (kMaxWorkingBytes - workingBytes) / sizeof(int);
}

uint8_t CompositeOverWhite(uint8_t luminance, uint8_t alpha) {
  return static_cast<uint8_t>(
      (static_cast<int>(luminance) * alpha +
       255 * (255 - static_cast<int>(alpha)) + 127) / 255);
}

uint8_t OtsuThreshold(const std::vector<uint8_t>& gray) {
  size_t histogram[256] = {};
  double totalSum = 0;
  size_t occupiedBins = 0;
  size_t samples = 0;
  // Large images only need a subsampled histogram for thresholding.
  const size_t step = gray.size() > 400000 ? 4 : 1;
  for (size_t i = 0; i < gray.size(); i += step) {
    occupiedBins += histogram[gray[i]] == 0;
    ++histogram[gray[i]];
    totalSum += gray[i];
    ++samples;
  }
  if (samples == 0) {
    return 128;
  }
  const int fallback =
      std::max(48, std::min(208, static_cast<int>(gray[0])));
  if (occupiedBins < 2) {
    return static_cast<uint8_t>(fallback);
  }

  size_t background = 0;
  double backgroundSum = 0;
  double bestVariance = -1;
  int selected = 128;
  for (int value = 0; value < 256; ++value) {
    background += histogram[value];
    backgroundSum += static_cast<double>(value) * histogram[value];
    const size_t foreground = samples - background;
    if (background == 0) {
      continue;
    }
    if (foreground == 0) {
      break;
    }
    const double meanDifference =
        backgroundSum / background -
        (totalSum - backgroundSum) / foreground;
    const double variance = static_cast<double>(background) *
        static_cast<double>(foreground) * meanDifference * meanDifference;
    if (variance > bestVariance) {
      bestVariance = variance;
      selected = value;
    }
  }
  return static_cast<uint8_t>(
      bestVariance > 0 ? selected : fallback);
}

void FinishBinary(BinaryImage& image) {
  image.threshold = OtsuThreshold(image.gray);
  const size_t pixelCount =
      static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
  image.black.resize(pixelCount);
  const size_t stride = static_cast<size_t>(image.width) + 1;
  image.integral.assign(
      stride * (static_cast<size_t>(image.height) + 1), 0);
  for (int y = 0; y < image.height; ++y) {
    int rowSum = 0;
    for (int x = 0; x < image.width; ++x) {
      const size_t source = static_cast<size_t>(y) * image.width + x;
      const uint8_t black = image.gray[source] <= image.threshold;
      image.black[source] = black;
      rowSum += black;
      image.integral[(static_cast<size_t>(y) + 1) * stride + x + 1] =
          image.integral[static_cast<size_t>(y) * stride + x + 1] + rowSum;
    }
  }
}

inline void DecodePixel(const unsigned char* pixel, int comps, bool hasAlpha,
                        uint8_t& gray, uint8_t& opacity) {
  if (comps == 1 || comps == 2) {
    gray = pixel[0];
    if (hasAlpha) {
      opacity = pixel[comps - 1];
    }
    return;
  }
  const uint8_t luminance = static_cast<uint8_t>(
      (306 * static_cast<int>(pixel[0]) +
       601 * static_cast<int>(pixel[1]) +
       117 * static_cast<int>(pixel[2]) + 0x200) >> 10);
  gray = comps == 4 ? CompositeOverWhite(luminance, pixel[3]) : luminance;
  if (hasAlpha) {
    opacity = pixel[3];
  }
}

// Build the luminance/opacity working image, optionally already downscaled so
// multi-megapixel camera photos do not pay full-resolution Otsu/integral cost.
BinaryImage BuildWorkingBinary(const MutableImage& image) {
  size_t pixelCount = 0;
  if (!CheckedImageSize(image, false, pixelCount)) {
    return BinaryImage();
  }
  const int longest = std::max(image.width, image.height);
  BinaryImage result = {0, 0, 0, std::vector<uint8_t>(),
                        std::vector<uint8_t>(), std::vector<uint8_t>(),
                        std::vector<int>()};
  // Keep working images bounded so multi-megapixel camera photos stay fast.
  const bool scaleDown = longest > 1024;
  result.width = scaleDown ?
      std::max(1, static_cast<int>((static_cast<int64_t>(image.width) * 1024 +
                                      longest / 2) / longest)) :
      image.width;
  result.height = scaleDown ?
      std::max(1, static_cast<int>((static_cast<int64_t>(image.height) * 1024 +
                                      longest / 2) / longest)) :
      image.height;
  const size_t outCount =
      static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
  result.gray.resize(outCount);
  result.opacity.resize(outCount);
  const bool hasAlpha = image.components == 2 || image.components == 4;
  if (!hasAlpha) {
    std::fill(result.opacity.begin(), result.opacity.end(),
              static_cast<uint8_t>(255));
  }
  const char* pixels = &image.pixels[0];
  const int comps = image.components;
  for (int y = 0; y < result.height; ++y) {
    const int sy = scaleDown ?
        std::min(image.height - 1, y * image.height / result.height) : y;
    for (int x = 0; x < result.width; ++x) {
      const int sx = scaleDown ?
          std::min(image.width - 1, x * image.width / result.width) : x;
      const size_t out =
          static_cast<size_t>(y) * result.width + static_cast<size_t>(x);
      const unsigned char* pixel = reinterpret_cast<const unsigned char*>(
          pixels +
          (static_cast<size_t>(sy) * image.width + static_cast<size_t>(sx)) *
              comps);
      DecodePixel(pixel, comps, hasAlpha, result.gray[out], result.opacity[out]);
    }
  }
  FinishBinary(result);
  return result;
}

std::vector<uint8_t> AreaAverage(
    const std::vector<uint8_t>& source, int sourceWidth, int sourceHeight,
    int targetWidth, int targetHeight) {
  std::vector<uint8_t> target(
      static_cast<size_t>(targetWidth) * targetHeight, 0);
  for (int y = 0; y < targetHeight; ++y) {
    const double top = static_cast<double>(y) * sourceHeight / targetHeight;
    const double bottom =
        static_cast<double>(y + 1) * sourceHeight / targetHeight;
    const int firstY = static_cast<int>(std::floor(top));
    const int lastY = static_cast<int>(std::ceil(bottom));
    for (int x = 0; x < targetWidth; ++x) {
      const double left = static_cast<double>(x) * sourceWidth / targetWidth;
      const double right =
          static_cast<double>(x + 1) * sourceWidth / targetWidth;
      const int firstX = static_cast<int>(std::floor(left));
      const int lastX = static_cast<int>(std::ceil(right));
      double sum = 0;
      for (int sy = firstY; sy < lastY; ++sy) {
        const double yWeight =
            std::min(bottom, static_cast<double>(sy + 1)) -
            std::max(top, static_cast<double>(sy));
        for (int sx = firstX; sx < lastX; ++sx) {
          const double xWeight =
              std::min(right, static_cast<double>(sx + 1)) -
              std::max(left, static_cast<double>(sx));
          sum += source[static_cast<size_t>(sy) * sourceWidth + sx] *
              xWeight * yWeight;
        }
      }
      const double area = (right - left) * (bottom - top);
      target[static_cast<size_t>(y) * targetWidth + x] =
          static_cast<uint8_t>(std::floor(sum / area + 0.5));
    }
  }
  return target;
}

// Fast box downsample for detection; area averaging is too expensive above
// megapixel inputs.
std::vector<uint8_t> FastDownsample(
    const std::vector<uint8_t>& source, int sourceWidth, int sourceHeight,
    int targetWidth, int targetHeight) {
  std::vector<uint8_t> target(
      static_cast<size_t>(targetWidth) * targetHeight, 0);
  for (int y = 0; y < targetHeight; ++y) {
    const int sy0 = y * sourceHeight / targetHeight;
    const int sy1 = std::max(sy0 + 1, (y + 1) * sourceHeight / targetHeight);
    for (int x = 0; x < targetWidth; ++x) {
      const int sx0 = x * sourceWidth / targetWidth;
      const int sx1 = std::max(sx0 + 1, (x + 1) * sourceWidth / targetWidth);
      int sum = 0;
      int count = 0;
      for (int sy = sy0; sy < sy1; ++sy) {
        const size_t row = static_cast<size_t>(sy) * sourceWidth;
        for (int sx = sx0; sx < sx1; ++sx) {
          sum += source[row + sx];
          ++count;
        }
      }
      target[static_cast<size_t>(y) * targetWidth + x] =
          static_cast<uint8_t>(count == 0 ? 0 : (sum + count / 2) / count);
    }
  }
  return target;
}

DetectionLayer BuildDetectionLayer(const BinaryImage& source) {
  DetectionLayer result = {
      BinaryImage{0, 0, 0, std::vector<uint8_t>(), std::vector<uint8_t>(),
                  std::vector<uint8_t>(), std::vector<int>()},
      1.0f, 1.0f};
  if (source.width <= 0 || source.height <= 0 || source.gray.empty()) {
    return result;
  }
  const int longest = std::max(source.width, source.height);
  // Longest-side 300 balances mid-size rotated photos (need >=~280) against
  // print samples that regress at 288/320 with the current single-axis model search.
  if (longest <= 300) {
    result.image = source;
    return result;
  }
  result.image.width = std::max(
      1, static_cast<int>((static_cast<int64_t>(source.width) * 300 +
                           longest / 2) / longest));
  result.image.height = std::max(
      1, static_cast<int>((static_cast<int64_t>(source.height) * 300 +
                           longest / 2) / longest));
  result.image.gray = longest > 1400 ?
      FastDownsample(
          source.gray, source.width, source.height,
          result.image.width, result.image.height) :
      AreaAverage(
          source.gray, source.width, source.height,
          result.image.width, result.image.height);
  result.image.opacity = longest > 1400 ?
      FastDownsample(
          source.opacity, source.width, source.height,
          result.image.width, result.image.height) :
      AreaAverage(
          source.opacity, source.width, source.height,
          result.image.width, result.image.height);
  FinishBinary(result.image);
  result.sourceScaleX =
      static_cast<float>(source.width) / result.image.width;
  result.sourceScaleY =
      static_cast<float>(source.height) / result.image.height;
  return result;
}

PointF DetectionToSource(const DetectionLayer& layer, PointF point) {
  return PointF{
      (point.x + 0.5f) * layer.sourceScaleX - 0.5f,
      (point.y + 0.5f) * layer.sourceScaleY - 0.5f};
}

PointF SourceToDetection(const DetectionLayer& layer, PointF point) {
  return PointF{
      (point.x + 0.5f) / layer.sourceScaleX - 0.5f,
      (point.y + 0.5f) / layer.sourceScaleY - 0.5f};
}

Bounds DetectionToSource(const DetectionLayer& layer, Bounds bounds) {
  const int width = static_cast<int>(
      std::floor(layer.image.width * layer.sourceScaleX + 0.5f));
  const int height = static_cast<int>(
      std::floor(layer.image.height * layer.sourceScaleY + 0.5f));
  return Bounds{
      std::max(0, std::min(width, static_cast<int>(
          std::floor(bounds.left * layer.sourceScaleX)))),
      std::max(0, std::min(height, static_cast<int>(
          std::floor(bounds.top * layer.sourceScaleY)))),
      std::max(0, std::min(width, static_cast<int>(
          std::ceil(bounds.right * layer.sourceScaleX)))),
      std::max(0, std::min(height, static_cast<int>(
          std::ceil(bounds.bottom * layer.sourceScaleY))))};
}

Bounds SourceToDetection(const DetectionLayer& layer, Bounds bounds) {
  return Bounds{
      std::max(0, std::min(layer.image.width, static_cast<int>(
          std::floor(bounds.left / layer.sourceScaleX)))),
      std::max(0, std::min(layer.image.height, static_cast<int>(
          std::floor(bounds.top / layer.sourceScaleY)))),
      std::max(0, std::min(layer.image.width, static_cast<int>(
          std::ceil(bounds.right / layer.sourceScaleX)))),
      std::max(0, std::min(layer.image.height, static_cast<int>(
          std::ceil(bounds.bottom / layer.sourceScaleY))))};
}

GridCandidate DetectionToSource(
    const DetectionLayer& layer, GridCandidate candidate) {
  for (int corner = 0; corner < 4; ++corner) {
    candidate.corner[corner] =
        DetectionToSource(layer, candidate.corner[corner]);
  }
  return candidate;
}

bool DetectionMappingRoundTrips(const DetectionLayer& layer) {
  if (layer.sourceScaleX <= 0 || layer.sourceScaleY <= 0) {
    return false;
  }
  const PointF points[4] = {
      {0, 0},
      {static_cast<float>(std::max(0, layer.image.width - 1)),
       static_cast<float>(std::max(0, layer.image.height - 1))},
      {0.25f * layer.image.width, 0.75f * layer.image.height},
      {-0.5f, -0.5f}};
  for (int i = 0; i < 4; ++i) {
    const PointF restored =
        SourceToDetection(layer, DetectionToSource(layer, points[i]));
    if (std::fabs(restored.x - points[i].x) > 0.0001f ||
        std::fabs(restored.y - points[i].y) > 0.0001f) {
      return false;
    }
  }
  const PointF identity = DetectionToSource(
      DetectionLayer{BinaryImage(), 1.0f, 1.0f}, PointF{7.25f, 11.5f});
  const Bounds detection = {
      std::min(3, layer.image.width), std::min(5, layer.image.height),
      std::max(0, layer.image.width - 2),
      std::max(0, layer.image.height - 3)};
  const Bounds restored = SourceToDetection(
      layer, DetectionToSource(layer, detection));
  DetectionLayer identityLayer = {BinaryImage(), 1.0f, 1.0f};
  identityLayer.image.width = 32;
  identityLayer.image.height = 24;
  const Bounds identityBounds =
      DetectionToSource(identityLayer, Bounds{3, 5, 29, 21});
  return identity.x == 7.25f && identity.y == 11.5f &&
      restored.left <= detection.left && restored.top <= detection.top &&
      restored.right >= detection.right &&
      restored.bottom >= detection.bottom &&
      identityBounds.left == 3 && identityBounds.top == 5 &&
      identityBounds.right == 29 && identityBounds.bottom == 21;
}

int RegionSum(const BinaryImage& image, Bounds region) {
  if (image.width <= 0 || image.height <= 0 || image.integral.empty()) {
    return 0;
  }
  region.left = std::max(0, std::min(image.width, region.left));
  region.right = std::max(0, std::min(image.width, region.right));
  region.top = std::max(0, std::min(image.height, region.top));
  region.bottom = std::max(0, std::min(image.height, region.bottom));
  if (region.left >= region.right || region.top >= region.bottom) {
    return 0;
  }
  const size_t stride = static_cast<size_t>(image.width) + 1;
  const size_t top = static_cast<size_t>(region.top) * stride;
  const size_t bottom = static_cast<size_t>(region.bottom) * stride;
  return image.integral[bottom + region.right] -
      image.integral[top + region.right] -
      image.integral[bottom + region.left] +
      image.integral[top + region.left];
}

void SetFixed(std::vector<FixedModule>& modules, int x, int y, bool black,
              uint8_t group) {
  for (size_t i = 0; i < modules.size(); ++i) {
    if (modules[i].x == x && modules[i].y == y) {
      const uint8_t value = static_cast<uint8_t>(black);
      assert(modules[i].black == value);
      // Lower groups have precedence: finders, then timing, then other fixed
      // structures. For an invalid same-group conflict, black wins.
      if (group < modules[i].group ||
          (group == modules[i].group && value > modules[i].black)) {
        modules[i].black = value;
        modules[i].group = group;
      }
      return;
    }
  }
  FixedModule module = {
      static_cast<short>(x), static_cast<short>(y),
      static_cast<uint8_t>(black), group};
  modules.push_back(module);
}

void AddFinder(std::vector<FixedModule>& modules, int left, int top,
               uint8_t group) {
  for (int y = 0; y < 7; ++y) {
    for (int x = 0; x < 7; ++x) {
      const bool black = x == 0 || x == 6 || y == 0 || y == 6 ||
          (x >= 2 && x <= 4 && y >= 2 && y <= 4);
      SetFixed(modules, left + x, top + y, black, group);
    }
  }
}

void AddSeparator(std::vector<FixedModule>& modules, int dimension,
                  uint8_t group) {
  for (int i = 0; i < 8; ++i) {
    if (group == 0) {
      SetFixed(modules, i, 7, false, group);
      SetFixed(modules, 7, i, false, group);
    } else if (group == 1) {
      SetFixed(modules, dimension - 1 - i, 7, false, group);
      SetFixed(modules, dimension - 8, i, false, group);
    } else {
      SetFixed(modules, i, dimension - 8, false, group);
      SetFixed(modules, 7, dimension - 1 - i, false, group);
    }
  }
}

void AddFinders(std::vector<FixedModule>& modules, int dimension) {
  AddFinder(modules, 0, 0, 0);
  AddSeparator(modules, dimension, 0);
  AddFinder(modules, dimension - 7, 0, 1);
  AddSeparator(modules, dimension, 1);
  AddFinder(modules, 0, dimension - 7, 2);
  AddSeparator(modules, dimension, 2);
}

void AddTiming(std::vector<FixedModule>& modules, int dimension) {
  for (int offset = 8; offset <= dimension - 9; ++offset) {
    const bool black = (offset & 1) == 0;
    SetFixed(modules, offset, 6, black, 3);
    SetFixed(modules, 6, offset, black, 3);
  }
}

void AddAlignment(std::vector<FixedModule>& modules, int centerX, int centerY) {
  for (int y = -2; y <= 2; ++y) {
    for (int x = -2; x <= 2; ++x) {
      const bool black = std::max(std::abs(x), std::abs(y)) != 1;
      SetFixed(modules, centerX + x, centerY + y, black, 4);
    }
  }
}

void AddAlignments(std::vector<FixedModule>& modules, int version) {
  std::vector<int>& centers =
      Version::getVersionForNumber(version)
          ->getAlignmentPatternCenters();
  const size_t last = centers.empty() ? 0 : centers.size() - 1;
  for (size_t y = 0; y < centers.size(); ++y) {
    for (size_t x = 0; x < centers.size(); ++x) {
      const bool overlapsFinder =
          (x == 0 && (y == 0 || y == last)) || (x == last && y == 0);
      if (!overlapsFinder) {
        AddAlignment(modules, centers[x], centers[y]);
      }
    }
  }
}

int VersionBits(int version) {
  if (version < 7 || version > 40) {
    return 0;
  }
  int bits = version << 12;
  for (int bit = 17; bit >= 12; --bit) {
    if ((bits & (1 << bit)) != 0) {
      bits ^= 0x1f25 << (bit - 12);
    }
  }
  return (version << 12) | bits;
}

void AddVersionInformation(std::vector<FixedModule>& modules, int version,
                           int dimension) {
  const int bits = VersionBits(version);
  for (int bit = 0; bit < 18; ++bit) {
    const bool black = (bits & (1 << bit)) != 0;
    const int major = bit / 3;
    const int minor = bit % 3;
    SetFixed(modules, dimension - 11 + minor, major, black, 4);
    SetFixed(modules, major, dimension - 11 + minor, black, 4);
  }
}

std::vector<FixedModule> BuildFixedModules(int version) {
  std::vector<FixedModule> modules;
  if (version < 1 || version > 10) {
    return modules;
  }
  const int dimension = 17 + 4 * version;
  AddFinders(modules, dimension);
  AddTiming(modules, dimension);
  SetFixed(modules, 8, dimension - 8, true, 4);
  AddAlignments(modules, version);
  if (version >= 7) {
    AddVersionInformation(modules, version, dimension);
  }
  return modules;
}

bool SupportedExtent(const std::vector<int>& counts, int minimumOccupancy,
                     int minimumRun, int& first, int& last) {
  int runStart = -1;
  int bestStart = -1;
  int bestLength = 0;
  for (size_t i = 0; i <= counts.size(); ++i) {
    const bool supported =
        i < counts.size() && counts[i] >= minimumOccupancy;
    if (supported && runStart < 0) {
      runStart = static_cast<int>(i);
    } else if (!supported && runStart >= 0) {
      const int length = static_cast<int>(i) - runStart;
      if (length >= minimumRun && length > bestLength) {
        bestStart = runStart;
        bestLength = length;
      }
      runStart = -1;
    }
  }
  if (bestStart < 0) {
    return false;
  }
  first = bestStart;
  last = bestStart + bestLength;
  return true;
}

ComponentGrid BuildDarkRoiGrid(const BinaryImage& image) {
  const int maximum = std::max(image.width, image.height);
  const int stride = std::max(4, (maximum + 63) / 64);
  ComponentGrid grid = {
      (image.width + stride - 1) / stride,
      (image.height + stride - 1) / stride, stride,
      std::vector<uint8_t>(), std::vector<uint8_t>()};
  const size_t count =
      static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height);
  std::vector<uint8_t> occupied(count, 0);
  grid.mask.resize(count, 0);
  grid.seen.resize(count, 0);
  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      const Bounds cell = {
          x * stride, y * stride,
          std::min(image.width, (x + 1) * stride),
          std::min(image.height, (y + 1) * stride)};
      const int area = (cell.right - cell.left) * (cell.bottom - cell.top);
      occupied[static_cast<size_t>(y) * grid.width + x] =
          RegionSum(image, cell) >= std::max(2, area / 10);
    }
  }
  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      if (!occupied[static_cast<size_t>(y) * grid.width + x]) {
        continue;
      }
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx >= 0 && ny >= 0 && nx < grid.width && ny < grid.height) {
            grid.mask[static_cast<size_t>(ny) * grid.width + nx] = 1;
          }
        }
      }
    }
  }
  return grid;
}

template <typename Visit>
void FloodComponent(ComponentGrid& grid, int start, std::vector<int>& pending,
                    Visit visit) {
  pending.clear();
  pending.push_back(start);
  grid.seen[start] = 1;
  for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
    const int index = pending[cursor];
    const int x = index % grid.width;
    const int y = index / grid.width;
    visit(index, x, y);
    const int neighbor[4] = {
        x > 0 ? index - 1 : -1,
        x + 1 < grid.width ? index + 1 : -1,
        y > 0 ? index - grid.width : -1,
        y + 1 < grid.height ? index + grid.width : -1};
    for (int side = 0; side < 4; ++side) {
      const int next = neighbor[side];
      if (next >= 0 && grid.mask[next] && !grid.seen[next]) {
        grid.seen[next] = 1;
        pending.push_back(next);
      }
    }
  }
}

void FloodRoi(ComponentGrid& grid, int start, RoiComponent& component) {
  component = RoiComponent{{grid.width, grid.height, 0, 0}};
  std::vector<int> pending;
  FloodComponent(grid, start, pending, [&](int, int x, int y) {
    component.cells.left = std::min(component.cells.left, x);
    component.cells.top = std::min(component.cells.top, y);
    component.cells.right = std::max(component.cells.right, x + 1);
    component.cells.bottom = std::max(component.cells.bottom, y + 1);
  });
}

float RoiTextureScore(const BinaryImage& image, const Bounds& region) {
  const int width = region.right - region.left;
  const int height = region.bottom - region.top;
  const int stride = std::max(1, std::min(width, height) / 32);
  int transitions = 0;
  int lines = 0;
  for (int y = region.top + stride / 2; y < region.bottom; y += stride) {
    uint8_t previous =
        image.black[static_cast<size_t>(y) * image.width + region.left];
    for (int x = region.left + 1; x < region.right; ++x) {
      const uint8_t current =
          image.black[static_cast<size_t>(y) * image.width + x];
      transitions += current != previous;
      previous = current;
    }
    ++lines;
  }
  for (int x = region.left + stride / 2; x < region.right; x += stride) {
    uint8_t previous =
        image.black[static_cast<size_t>(region.top) * image.width + x];
    for (int y = region.top + 1; y < region.bottom; ++y) {
      const uint8_t current =
          image.black[static_cast<size_t>(y) * image.width + x];
      transitions += current != previous;
      previous = current;
    }
    ++lines;
  }
  const float average = lines == 0 ? 0.0f :
      static_cast<float>(transitions) / lines;
  return average < 4.0f ? 0.0f : std::min(1.0f, average / 10.0f);
}

float RoiScore(const BinaryImage& image, const Bounds& region) {
  const int width = region.right - region.left;
  const int height = region.bottom - region.top;
  const int shortSide = std::min(width, height);
  const int longSide = std::max(width, height);
  if (shortSide < std::min(image.width, image.height) / 4 ||
      longSide <= 0 || longSide > 2 * shortSide) {
    return 0;
  }
  const int area = width * height;
  const int black = RegionSum(image, region);
  const float density = area == 0 ? 0.0f :
      static_cast<float>(black) / area;
  const float texture = RoiTextureScore(image, region);
  if (density < 1.0f / 12.0f || density > 0.72f || texture == 0) {
    return 0;
  }
  const float square = static_cast<float>(shortSide) / longSide;
  return black * square * square * texture;
}

int64_t BoundsArea(Bounds bounds) {
  const int64_t width = std::max(0, bounds.right - bounds.left);
  const int64_t height = std::max(0, bounds.bottom - bounds.top);
  return width * height;
}

float BoundsIoU(Bounds a, Bounds b) {
  const Bounds intersection = {
      std::max(a.left, b.left), std::max(a.top, b.top),
      std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
  const int64_t overlap = BoundsArea(intersection);
  const int64_t united = BoundsArea(a) + BoundsArea(b) - overlap;
  return united <= 0 ? 0.0f :
      static_cast<float>(static_cast<double>(overlap) / united);
}

bool BetterSearchRegion(const SearchRegion& a, const SearchRegion& b) {
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  if (a.bounds.top != b.bounds.top) {
    return a.bounds.top < b.bounds.top;
  }
  if (a.bounds.left != b.bounds.left) {
    return a.bounds.left < b.bounds.left;
  }
  if (a.bounds.bottom != b.bounds.bottom) {
    return a.bounds.bottom < b.bounds.bottom;
  }
  return a.bounds.right < b.bounds.right;
}

void AddDistinctRegion(
    std::vector<SearchRegion>& regions, const SearchRegion& candidate) {
  for (size_t i = 0; i < regions.size(); ++i) {
    if (BoundsIoU(regions[i].bounds, candidate.bounds) > 0.75f) {
      return;
    }
  }
  if (regions.size() < 6) {
    regions.push_back(candidate);
  }
}

std::vector<SearchRegion> FindQRRegions(const BinaryImage& image) {
  ComponentGrid grid = BuildDarkRoiGrid(image);
  std::vector<SearchRegion> candidates;
  for (size_t start = 0; start < grid.mask.size(); ++start) {
    if (!grid.mask[start] || grid.seen[start]) {
      continue;
    }
    RoiComponent component;
    FloodRoi(grid, static_cast<int>(start), component);
    const int margin = grid.stride;
    const Bounds region = {
        std::max(0, component.cells.left * grid.stride - margin),
        std::max(0, component.cells.top * grid.stride - margin),
        std::min(image.width, component.cells.right * grid.stride + margin),
        std::min(image.height, component.cells.bottom * grid.stride + margin)};
    candidates.push_back(SearchRegion{region, RoiScore(image, region)});
  }
  std::sort(candidates.begin(), candidates.end(), BetterSearchRegion);
  std::vector<SearchRegion> result;
  for (size_t i = 0; i < candidates.size() && result.size() < 6; ++i) {
    AddDistinctRegion(result, candidates[i]);
  }
  result.push_back(SearchRegion{
      Bounds{0, 0, image.width, image.height}, -1.0f});
  return result;
}

Bounds FindDarkBounds(const BinaryImage& image, Bounds region) {
  region.left = std::max(0, std::min(image.width, region.left));
  region.right = std::max(0, std::min(image.width, region.right));
  region.top = std::max(0, std::min(image.height, region.top));
  region.bottom = std::max(0, std::min(image.height, region.bottom));
  const int regionWidth = region.right - region.left;
  const int regionHeight = region.bottom - region.top;
  if (regionWidth < 3 || regionHeight < 3) {
    return Bounds{0, 0, 0, 0};
  }
  const int inset = std::max(1, std::min(regionWidth, regionHeight) / 100);
  const int left = region.left + inset;
  const int top = region.top + inset;
  const int right = region.right - inset;
  const int bottom = region.bottom - inset;
  std::vector<int> rows(static_cast<size_t>(bottom - top), 0);
  std::vector<int> columns(static_cast<size_t>(right - left), 0);
  for (int y = top; y < bottom; ++y) {
    rows[y - top] = RegionSum(
        image, Bounds{left, y, right, y + 1});
  }
  for (int x = left; x < right; ++x) {
    columns[x - left] = RegionSum(
        image, Bounds{x, top, x + 1, bottom});
  }
  Bounds local = {0, 0, 0, 0};
  const int minimumRun =
      std::max(2, std::min(regionWidth, regionHeight) / 256);
  const int rowOccupancy = std::max(3, (right - left) / 128);
  const int columnOccupancy =
      std::max(3, (bottom - top) / 128);
  if (!SupportedExtent(rows, rowOccupancy, minimumRun,
                       local.top, local.bottom) ||
      !SupportedExtent(columns, columnOccupancy, minimumRun,
                       local.left, local.right)) {
    return Bounds{0, 0, 0, 0};
  }
  return Bounds{
      left + local.left, top + local.top,
      left + local.right, top + local.bottom};
}

BorderCounts CountBorderRing(
    const BinaryImage& image, int inset, int thickness) {
  BorderCounts result = {0, 0, 0};
  const int left = std::min(image.width, inset);
  const int top = std::min(image.height, inset);
  const int right = std::max(left, image.width - inset);
  const int bottom = std::max(top, image.height - inset);
  const int innerLeft = std::min(right, left + thickness);
  const int innerTop = std::min(bottom, top + thickness);
  const int innerRight = std::max(innerLeft, right - thickness);
  const int innerBottom = std::max(innerTop, bottom - thickness);
  const auto countPixel = [&](int x, int y) {
    const size_t index = static_cast<size_t>(y) * image.width + x;
    ++result.total;
    result.black += image.black[index] != 0;
    result.transparent += image.opacity[index] < 32;
  };
  for (int y = top; y < innerTop; ++y) {
    for (int x = left; x < right; ++x) {
      countPixel(x, y);
    }
  }
  for (int y = innerBottom; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      countPixel(x, y);
    }
  }
  for (int y = innerTop; y < innerBottom; ++y) {
    for (int x = left; x < innerLeft; ++x) {
      countPixel(x, y);
    }
    for (int x = innerRight; x < right; ++x) {
      countPixel(x, y);
    }
  }
  return result;
}

float CountFraction(size_t matching, size_t total) {
  return total == 0 ? 0.0f : static_cast<float>(matching) / total;
}

BorderEvidence MeasureBorder(const BinaryImage& image) {
  const int band = std::max(2, std::min(image.width, image.height) / 50);
  const BorderCounts outer = CountBorderRing(image, 0, band);
  const BorderCounts inner = CountBorderRing(image, band, band);
  BorderEvidence result = {
      CountFraction(outer.black, outer.total),
      CountFraction(inner.black, inner.total),
      CountFraction(outer.transparent, outer.total),
      CountFraction(inner.transparent, inner.total), false, false};
  const bool blackCanvas =
      result.outerBlack >= 0.75f && result.innerBlack >= 0.55f;
  const bool transparentCanvas =
      result.outerTransparent >= 0.75f && result.innerTransparent >= 0.55f;
  result.darkCanvas = blackCanvas || transparentCanvas;
  result.useOpacity = transparentCanvas;
  return result;
}

bool ValidPaperQuad(const BinaryImage& image, const PointF corners[4],
                    size_t brightCount) {
  float cross[4];
  float minimumEdge = std::numeric_limits<float>::max();
  float maximumEdge = 0;
  float twiceArea = 0;
  for (int i = 0; i < 4; ++i) {
    const PointF a = corners[i];
    const PointF b = corners[(i + 1) & 3];
    const PointF c = corners[(i + 2) & 3];
    cross[i] = (b.x - a.x) * (c.y - b.y) -
        (b.y - a.y) * (c.x - b.x);
    const float edge = Distance(a, b);
    minimumEdge = std::min(minimumEdge, edge);
    maximumEdge = std::max(maximumEdge, edge);
    twiceArea += a.x * b.y - a.y * b.x;
  }
  const float area = std::fabs(twiceArea) * 0.5f;
  const float canvas = static_cast<float>(image.width) * image.height;
  const float brightCoverage = area > 0 ? brightCount / area : 0;
  for (int i = 1; i < 4; ++i) {
    if (cross[i] * cross[0] <= 0) {
      return false;
    }
  }
  return area >= 0.20f * canvas && area <= 1.05f * canvas &&
      minimumEdge >= 0.25f * std::min(image.width, image.height) &&
      maximumEdge <= 1.8f * minimumEdge &&
      brightCoverage >= 0.25f && brightCoverage <= 1.10f;
}

bool IsPaperPixel(const BinaryImage& image, size_t index, bool useOpacity) {
  const int brightThreshold =
      std::max(160, static_cast<int>(image.threshold) + 32);
  return useOpacity ? image.opacity[index] >= 128 :
      image.gray[index] >= brightThreshold;
}

ComponentGrid BuildPaperGrid(const BinaryImage& image, bool useOpacity) {
  const int maximum = std::max(image.width, image.height);
  const int stride = std::max(1, (maximum + 511) / 512);
  ComponentGrid grid = {
      (image.width + stride - 1) / stride,
      (image.height + stride - 1) / stride, stride,
      std::vector<uint8_t>(), std::vector<uint8_t>()};
  const size_t count =
      static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height);
  grid.mask.resize(count, 0);
  grid.seen.resize(count, 0);
  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      const int sourceX = std::min(image.width - 1, x * stride + stride / 2);
      const int sourceY = std::min(image.height - 1, y * stride + stride / 2);
      const size_t source =
          static_cast<size_t>(sourceY) * image.width + sourceX;
      grid.mask[static_cast<size_t>(y) * grid.width + x] =
          IsPaperPixel(image, source, useOpacity);
    }
  }
  return grid;
}

void FloodPaper(ComponentGrid& grid, int start, std::vector<int>& component) {
  // Reuse component as the BFS queue (same allocation pattern as before).
  FloodComponent(grid, start, component, [&](int, int, int) {});
}

bool LargestPaperComponent(
    const BinaryImage& image, bool useOpacity, PaperComponent& result) {
  ComponentGrid grid = BuildPaperGrid(image, useOpacity);
  std::vector<int> component;
  std::vector<int> best;
  size_t secondSize = 0;
  for (size_t start = 0; start < grid.mask.size(); ++start) {
    if (!grid.mask[start] || grid.seen[start]) {
      continue;
    }
    FloodPaper(grid, static_cast<int>(start), component);
    if (component.size() > best.size()) {
      secondSize = best.size();
      best = component;
    } else {
      secondSize = std::max(secondSize, component.size());
    }
  }
  const size_t canvas = static_cast<size_t>(image.width) * image.height;
  result.area = best.size() * static_cast<size_t>(grid.stride) * grid.stride;
  if (result.area < canvas / 20 ||
      (secondSize != 0 && best.size() < 3 * secondSize)) {
    return false;
  }
  result.points.clear();
  result.points.reserve(best.size());
  for (size_t i = 0; i < best.size(); ++i) {
    const int x = best[i] % grid.width;
    const int y = best[i] / grid.width;
    result.points.push_back(PointF{
        static_cast<float>(
            std::min(image.width - 1, x * grid.stride + grid.stride / 2)),
        static_cast<float>(
            std::min(image.height - 1, y * grid.stride + grid.stride / 2))});
  }
  return true;
}

std::vector<PointF> DirectionalSupports(
    const std::vector<PointF>& points) {
  std::vector<PointF> result;
  const int directionCount = 16;
  const float turn = 6.28318530717958647692f;
  for (int direction = 0; direction < directionCount; ++direction) {
    const float angle = turn * direction / directionCount;
    const PointF axis = {std::cos(angle), std::sin(angle)};
    float best = -std::numeric_limits<float>::max();
    PointF support = {0, 0};
    for (size_t i = 0; i < points.size(); ++i) {
      const float projection =
          points[i].x * axis.x + points[i].y * axis.y;
      if (projection > best) {
        best = projection;
        support = points[i];
      }
    }
    bool duplicate = false;
    for (size_t i = 0; i < result.size(); ++i) {
      duplicate = duplicate ||
          (result[i].x == support.x && result[i].y == support.y);
    }
    if (!points.empty() && !duplicate) {
      result.push_back(support);
    }
  }
  return result;
}

void OrderQuad(const PointF input[4], PointF output[4]) {
  PointF center = {0, 0};
  std::vector<PointF> ordered(input, input + 4);
  for (int i = 0; i < 4; ++i) {
    center.x += 0.25f * input[i].x;
    center.y += 0.25f * input[i].y;
  }
  std::sort(ordered.begin(), ordered.end(),
            [center](PointF a, PointF b) {
              return std::atan2(a.y - center.y, a.x - center.x) <
                  std::atan2(b.y - center.y, b.x - center.x);
            });
  int first = 0;
  for (int i = 1; i < 4; ++i) {
    if (ordered[i].x + ordered[i].y <
        ordered[first].x + ordered[first].y) {
      first = i;
    }
  }
  for (int i = 0; i < 4; ++i) {
    output[i] = ordered[(first + i) & 3];
  }
}

float QuadArea(const PointF corners[4]) {
  float twiceArea = 0;
  for (int i = 0; i < 4; ++i) {
    const PointF a = corners[i];
    const PointF b = corners[(i + 1) & 3];
    twiceArea += a.x * b.y - a.y * b.x;
  }
  return 0.5f * std::fabs(twiceArea);
}

bool FindPaperCorners(
    const BinaryImage& image, const BorderEvidence& border,
    PointF corners[4]) {
  if (!border.darkCanvas) {
    return false;
  }
  PaperComponent component;
  if (!LargestPaperComponent(image, border.useOpacity, component)) {
    return false;
  }
  const std::vector<PointF> supports =
      DirectionalSupports(component.points);
  float bestArea = 0;
  PointF trial[4];
  PointF ordered[4];
  for (size_t a = 0; a < supports.size(); ++a) {
    for (size_t b = a + 1; b < supports.size(); ++b) {
      for (size_t c = b + 1; c < supports.size(); ++c) {
        for (size_t d = c + 1; d < supports.size(); ++d) {
          trial[0] = supports[a];
          trial[1] = supports[b];
          trial[2] = supports[c];
          trial[3] = supports[d];
          OrderQuad(trial, ordered);
          const float area = QuadArea(ordered);
          if (area > bestArea &&
              ValidPaperQuad(image, ordered, component.area)) {
            bestArea = area;
            std::copy(ordered, ordered + 4, corners);
          }
        }
      }
    }
  }
  return bestArea > 0;
}

std::vector<int> RunHistogram(const BinaryImage& image, Bounds bounds) {
  const int limit = std::max(bounds.right - bounds.left,
                             bounds.bottom - bounds.top);
  std::vector<int> histogram(static_cast<size_t>(limit) + 1, 0);
  const int stride = std::max(1, std::min(image.width, image.height) / 256);
  for (int y = bounds.top; y < bounds.bottom; y += stride) {
    int run = 0;
    for (int x = bounds.left; x <= bounds.right; ++x) {
      const bool black = x < bounds.right &&
          image.black[static_cast<size_t>(y) * image.width + x] != 0;
      if (black) {
        ++run;
      } else if (run != 0) {
        ++histogram[run];
        run = 0;
      }
    }
  }
  for (int x = bounds.left; x < bounds.right; x += stride) {
    int run = 0;
    for (int y = bounds.top; y <= bounds.bottom; ++y) {
      const bool black = y < bounds.bottom &&
          image.black[static_cast<size_t>(y) * image.width + x] != 0;
      if (black) {
        ++run;
      } else if (run != 0) {
        ++histogram[run];
        run = 0;
      }
    }
  }
  return histogram;
}

ModuleEstimate EstimateModuleSize(const BinaryImage& image, Bounds bounds) {
  const std::vector<int> histogram = RunHistogram(image, bounds);
  const int shortSide = std::min(image.width, image.height);
  const int minimum = std::max(2, shortSide / 100);
  const int maximum = std::min(static_cast<int>(histogram.size()) - 1,
                               std::max(minimum, shortSide / 15));
  double bestScore = 0;
  int bestSize = 0;
  for (int base = minimum; base <= maximum; ++base) {
    double score = 0;
    const int tolerance = std::max(1, base / 12);
    for (int multiple = 1; multiple <= 5; ++multiple) {
      const int center = base * multiple;
      for (int length = std::max(1, center - tolerance);
           length <= center + tolerance &&
           length < static_cast<int>(histogram.size()); ++length) {
        score += static_cast<double>(histogram[length]) / multiple;
      }
    }
    if (score > bestScore) {
      bestScore = score;
      bestSize = base;
    }
  }
  const int span = std::max(bounds.right - bounds.left,
                            bounds.bottom - bounds.top);
  const float support = span > 0 ?
      static_cast<float>(bestScore / span) : 0.0f;
  ModuleEstimate result = {
      static_cast<float>(bestSize), support,
      bestScore >= 12.0 && support >= 0.20f};
  return result;
}

bool BetterScaleHint(const ScaleHint& a, const ScaleHint& b) {
  if (a.support != b.support) {
    return a.support > b.support;
  }
  return a.moduleSize < b.moduleSize;
}

void MergeScaleHint(std::vector<ScaleHint>& hints, ScaleHint candidate) {
  if (candidate.moduleSize < 1.25f || candidate.support <= 0) {
    return;
  }
  for (size_t i = 0; i < hints.size(); ++i) {
    if (std::fabs(hints[i].moduleSize - candidate.moduleSize) <= 0.35f) {
      const float total = hints[i].support + candidate.support;
      hints[i].moduleSize =
          (hints[i].moduleSize * hints[i].support +
           candidate.moduleSize * candidate.support) / total;
      hints[i].support = total;
      return;
    }
  }
  hints.push_back(candidate);
}

float FoldedRunSupport(const std::vector<int>& histogram, float base) {
  float score = 0;
  for (int multiple = 1; multiple <= 7; ++multiple) {
    const float center = base * multiple;
    const int tolerance = std::max(1, static_cast<int>(base * 0.16f + 0.5f));
    const int first = std::max(1, static_cast<int>(center + 0.5f) - tolerance);
    const int last = std::min(
        static_cast<int>(histogram.size()) - 1,
        static_cast<int>(center + 0.5f) + tolerance);
    for (int length = first; length <= last; ++length) {
      score += static_cast<float>(histogram[length]) / multiple;
    }
  }
  return score;
}

void AppendRunHints(const BinaryImage& image, Bounds bounds, float weight,
                    std::vector<ScaleHint>& hints) {
  const int shortSide =
      std::min(bounds.right - bounds.left, bounds.bottom - bounds.top);
  if (shortSide < 21) {
    return;
  }
  const std::vector<int> histogram = RunHistogram(image, bounds);
  const float maximum = std::max(1.25f, shortSide / 15.0f);
  std::vector<ScaleHint> local;
  for (int quarter = 5; quarter <= static_cast<int>(maximum * 4); ++quarter) {
    const float base = quarter * 0.25f;
    const float support = FoldedRunSupport(histogram, base) * weight / shortSide;
    const float before = FoldedRunSupport(histogram, base - 0.25f);
    const float after = FoldedRunSupport(histogram, base + 0.25f);
    if (support > 0.015f * weight &&
        support >= before * weight / shortSide &&
        support >= after * weight / shortSide) {
      local.push_back(ScaleHint{base, support});
    }
  }
  std::sort(local.begin(), local.end(), BetterScaleHint);
  const size_t count = std::min<size_t>(2, local.size());
  for (size_t i = 0; i < count; ++i) {
    MergeScaleHint(hints, local[i]);
  }
}

std::vector<ScaleHint> CollectModuleHints(
    const DetectionLayer& layer, const std::vector<SearchRegion>& regions) {
  std::vector<ScaleHint> hints;
  const BinaryImage& image = layer.image;
  const Bounds full = {0, 0, image.width, image.height};
  AppendRunHints(image, full, 1.0f, hints);
  for (size_t i = 0; i < regions.size(); ++i) {
    if (regions[i].bounds.left == 0 && regions[i].bounds.top == 0 &&
        regions[i].bounds.right == image.width &&
        regions[i].bounds.bottom == image.height) {
      continue;
    }
    AppendRunHints(
        image, regions[i].bounds, 1.0f, hints);
  }
  for (int tileY = 0; tileY < 4; ++tileY) {
    for (int tileX = 0; tileX < 4; ++tileX) {
      const Bounds tile = {
          tileX * image.width / 4, tileY * image.height / 4,
          (tileX + 1) * image.width / 4,
          (tileY + 1) * image.height / 4};
      AppendRunHints(image, tile, 0.5f, hints);
    }
  }
  std::sort(hints.begin(), hints.end(), BetterScaleHint);
  // Axis-aligned search only: keep a short module-size list.
  if (hints.size() > 3) {
    hints.resize(3);
  }
  if (hints.size() > 2 && hints[0].support > 0) {
    while (hints.size() > 2 &&
           hints.back().support < 0.18f * hints[0].support) {
      hints.pop_back();
    }
  }
  return hints;
}

std::vector<int> CandidateVersions(
    const ModuleEstimate&, Bounds) {
  std::vector<int> versions;
  for (int version = 1; version <= 10; ++version) {
    versions.push_back(version);
  }
  return versions;
}

bool ModelVersionFits(int version, float module, int width, int height) {
  if (version < 1 || version > 10 || module < 1.0f) {
    return false;
  }
  const float side = (17 + 4 * version) * module;
  const float longest = static_cast<float>(std::max(width, height));
  // Keep a small quiet-zone margin so dense V10 codes still fit.
  return side <= longest * 1.08f + module;
}

int ModelScanStride(float module, int width, int height) {
  int stride = std::max(1, static_cast<int>(module / 2 + 0.5f));
  const int area = std::max(1, width) * std::max(1, height);
  // Bound origin density independently of image size / module.
  const int budget = 1100;
  const int needed = static_cast<int>(std::ceil(
      std::sqrt(static_cast<double>(area) / static_cast<double>(budget))));
  return std::max(stride, needed);
}

float Distance(PointF a, PointF b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

PointF GridPoint(const GridCandidate& grid, float x, float y) {
  const float last = static_cast<float>(16 + 4 * grid.version);
  const float u = x / last;
  const float v = y / last;
  PointF result;
  result.x = (1 - u) * (1 - v) * grid.corner[0].x +
      u * (1 - v) * grid.corner[1].x +
      u * v * grid.corner[2].x + (1 - u) * v * grid.corner[3].x;
  result.y = (1 - u) * (1 - v) * grid.corner[0].y +
      u * (1 - v) * grid.corner[1].y +
      u * v * grid.corner[2].y + (1 - u) * v * grid.corner[3].y;
  return result;
}

float GridModuleSize(const GridCandidate& grid) {
  const float modules = static_cast<float>(16 + 4 * grid.version);
  return (Distance(grid.corner[0], grid.corner[1]) +
          Distance(grid.corner[1], grid.corner[2]) +
          Distance(grid.corner[2], grid.corner[3]) +
          Distance(grid.corner[3], grid.corner[0])) / (4 * modules);
}

bool IsAxisAligned(const GridCandidate& grid) {
  for (int i = 0; i < 4; ++i) {
    const PointF a = grid.corner[i];
    const PointF b = grid.corner[(i + 1) & 3];
    const float dx = std::fabs(a.x - b.x);
    const float dy = std::fabs(a.y - b.y);
    const float major = std::max(dx, dy);
    const float minor = std::min(dx, dy);
    if (major <= 0 || minor > 0.01f * major) {
      return false;
    }
  }
  return true;
}

bool DirectBlack(const BinaryImage& image, PointF point, float& black) {
  const int x = static_cast<int>(std::floor(point.x + 0.5f));
  const int y = static_cast<int>(std::floor(point.y + 0.5f));
  if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
    return false;
  }
  black = image.black[static_cast<size_t>(y) * image.width + x];
  return true;
}

bool ContinuousBlack(const BinaryImage& image, PointF point, float& black) {
  if (point.x < 0 || point.y < 0 ||
      point.x > image.width - 1 || point.y > image.height - 1) {
    return false;
  }
  const int x0 = static_cast<int>(std::floor(point.x));
  const int y0 = static_cast<int>(std::floor(point.y));
  const int x1 = std::min(image.width - 1, x0 + 1);
  const int y1 = std::min(image.height - 1, y0 + 1);
  const float fx = point.x - x0;
  const float fy = point.y - y0;
  const float top =
      (1 - fx) * image.black[static_cast<size_t>(y0) * image.width + x0] +
      fx * image.black[static_cast<size_t>(y0) * image.width + x1];
  const float bottom =
      (1 - fx) * image.black[static_cast<size_t>(y1) * image.width + x0] +
      fx * image.black[static_cast<size_t>(y1) * image.width + x1];
  black = (1 - fy) * top + fy * bottom;
  return true;
}

float IntegralAt(const BinaryImage& image, float x, float y) {
  x = std::max(0.0f, std::min(static_cast<float>(image.width), x));
  y = std::max(0.0f, std::min(static_cast<float>(image.height), y));
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int x1 = std::min(image.width, x0 + 1);
  const int y1 = std::min(image.height, y0 + 1);
  const float fx = x - x0;
  const float fy = y - y0;
  const int stride = image.width + 1;
  const float top =
      (1 - fx) * image.integral[static_cast<size_t>(y0) * stride + x0] +
      fx * image.integral[static_cast<size_t>(y0) * stride + x1];
  const float bottom =
      (1 - fx) * image.integral[static_cast<size_t>(y1) * stride + x0] +
      fx * image.integral[static_cast<size_t>(y1) * stride + x1];
  return (1 - fy) * top + fy * bottom;
}

SampleResult SampleBlack(const BinaryImage& image, PointF point, float radius,
                         bool axisAligned) {
  if (axisAligned) {
    const float requestedLeft = point.x - radius + 0.5f;
    const float requestedTop = point.y - radius + 0.5f;
    const float requestedRight = point.x + radius + 0.5f;
    const float requestedBottom = point.y + radius + 0.5f;
    const float left = std::max(0.0f, requestedLeft);
    const float top = std::max(0.0f, requestedTop);
    const float right =
        std::min(static_cast<float>(image.width), requestedRight);
    const float bottom =
        std::min(static_cast<float>(image.height), requestedBottom);
    const float requested =
        (requestedRight - requestedLeft) * (requestedBottom - requestedTop);
    const float covered =
        std::max(0.0f, right - left) * std::max(0.0f, bottom - top);
    if (requested <= 0 || covered <= 0) {
      return SampleResult{0, 0};
    }
    const float sum = IntegralAt(image, right, bottom) -
        IntegralAt(image, left, bottom) - IntegralAt(image, right, top) +
        IntegralAt(image, left, top);
    return SampleResult{sum / covered, covered / requested};
  }
  const float offset = radius * 0.55f;
  const PointF samples[5] = {
      point, {point.x - offset, point.y}, {point.x + offset, point.y},
      {point.x, point.y - offset}, {point.x, point.y + offset}};
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < 5; ++i) {
    float value = 0;
    if (DirectBlack(image, samples[i], value)) {
      sum += value;
      ++valid;
    }
  }
  return SampleResult{
      valid == 0 ? 0.0f : sum / valid, valid / 5.0f};
}

SampleResult SampleGridBlack(
    const BinaryImage& image, const GridCandidate& grid, float x, float y,
    float radius) {
  const PointF point = GridPoint(grid, x, y);
  if (IsAxisAligned(grid)) {
    return SampleBlack(image, point, radius, true);
  }
  const PointF alongX = GridPoint(grid, x + 0.25f, y);
  const PointF alongY = GridPoint(grid, x, y + 0.25f);
  const float dx = alongX.x - point.x + alongY.x - point.x;
  const float dy = alongX.y - point.y + alongY.y - point.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0) {
    return SampleResult{0, 0};
  }
  const float scale = radius * 0.45f / length;
  const PointF samples[2] = {
      {point.x - scale * dx, point.y - scale * dy},
      {point.x + scale * dx, point.y + scale * dy}};
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < 2; ++i) {
    float value = 0;
    if (ContinuousBlack(image, samples[i], value)) {
      sum += value;
      ++valid;
    }
  }
  return SampleResult{
      valid == 0 ? 0.0f : sum / valid, valid / 2.0f};
}

bool FootprintCovered(const BinaryImage& image, const GridCandidate& grid,
                      float radius) {
  for (int i = 0; i < 4; ++i) {
    if (grid.corner[i].x - radius < 0 ||
        grid.corner[i].y - radius < 0 ||
        grid.corner[i].x + radius >= image.width ||
        grid.corner[i].y + radius >= image.height) {
      return false;
    }
  }
  return true;
}

ScoreResult FixedScore(const BinaryImage& image, const GridCandidate& grid,
                       const std::vector<FixedModule>& fixed) {
  float sums[5] = {};
  float weights[5] = {};
  float coverage = 0;
  const float radius = std::max(0.6f, GridModuleSize(grid) * 0.28f);
  for (size_t i = 0; i < fixed.size(); ++i) {
    const FixedModule& module = fixed[i];
    const SampleResult sample =
        SampleGridBlack(image, grid, module.x, module.y, radius);
    const float agreement = module.black ?
        sample.black : 1 - sample.black;
    sums[module.group] += agreement * sample.coverage;
    weights[module.group] += sample.coverage;
    coverage += sample.coverage;
  }
  float finder[3];
  for (int group = 0; group < 3; ++group) {
    finder[group] =
        weights[group] == 0 ? 0.5f : sums[group] / weights[group];
  }
  std::sort(finder, finder + 3, std::greater<float>());
  const float timing = weights[3] == 0 ? 0.5f : sums[3] / weights[3];
  const float other = weights[4] == 0 ? 0.5f : sums[4] / weights[4];
  return ScoreResult{
      0.35f * finder[0] + 0.15f * finder[1] + 0.05f * finder[2] +
      0.30f * timing + 0.15f * other,
      fixed.empty() ? 0.0f : coverage / fixed.size()};
}

ScoreResult GridPurityScore(
    const BinaryImage& image, const GridCandidate& grid, int step, int offset,
    float radiusScale) {
  const int dimension = 17 + 4 * grid.version;
  const float radius = std::max(0.6f, GridModuleSize(grid) * radiusScale);
  const int columns = (dimension - 1 - offset) / step + 1;
  std::vector<float> previous(static_cast<size_t>(columns), -1);
  float interior = 0;
  float black = 0;
  float coverage = 0;
  float transitions = 0;
  float transitionWeight = 0;
  int count = 0;
  for (int y = offset; y < dimension; y += step) {
    float left = -1;
    int column = 0;
    for (int x = offset; x < dimension; x += step) {
      const SampleResult sample =
          SampleGridBlack(image, grid, x, y, radius);
      interior += std::fabs(sample.black - 0.5f) * 2 * sample.coverage;
      black += sample.black * sample.coverage;
      coverage += sample.coverage;
      if (left >= 0) {
        transitions += std::fabs(sample.black - left) * sample.coverage;
        transitionWeight += sample.coverage;
      }
      if (previous[column] >= 0) {
        transitions +=
            std::fabs(sample.black - previous[column]) * sample.coverage;
        transitionWeight += sample.coverage;
      }
      left = sample.black;
      previous[column++] = sample.black;
      ++count;
    }
  }
  const float consistency = coverage == 0 ? 0 : interior / coverage;
  const float balance = coverage == 0 ? 0 :
      1 - std::min(1.0f, 2 * std::fabs(black / coverage - 0.5f));
  const float transition = transitionWeight == 0 ? 0 :
      std::min(1.0f, transitions / (0.35f * transitionWeight));
  return ScoreResult{
      0.75f * consistency + 0.10f * balance + 0.15f * transition,
      count == 0 ? 0.0f : coverage / count};
}

float CoarseModelScore(const BinaryImage& image, GridCandidate& grid,
                       const std::vector<FixedModule>& fixed) {
  const float radius = std::max(0.6f, GridModuleSize(grid) * 0.28f);
  if (!FootprintCovered(image, grid, radius)) {
    grid.score = -1;
    return grid.score;
  }
  const ScoreResult model = FixedScore(image, grid, fixed);
  const ScoreResult purity = GridPurityScore(image, grid, 4, 1, 0.24f);
  if (model.coverage < 0.98f || purity.coverage < 0.98f) {
    grid.score = -1;
    return grid.score;
  }
  grid.score = 0.88f * model.score + 0.12f * purity.score;
  return grid.score;
}

float ScoreGrid(const BinaryImage& image, GridCandidate& grid,
                const std::vector<FixedModule>& fixed) {
  const float radius = std::max(0.6f, GridModuleSize(grid) * 0.30f);
  if (!FootprintCovered(image, grid, radius)) {
    grid.score = -1;
    return grid.score;
  }
  const ScoreResult fixedScore = FixedScore(image, grid, fixed);
  const ScoreResult purityScore = GridPurityScore(image, grid, 2, 0, 0.28f);
  if (fixedScore.coverage < 0.98f || purityScore.coverage < 0.98f) {
    grid.score = -1;
    return grid.score;
  }
  grid.score = 0.80f * fixedScore.score + 0.20f * purityScore.score;
  return grid.score;
}

GridCandidate MakeAxisGrid(const AxisGridSpec& spec, int orientation) {
  const float last = static_cast<float>(16 + 4 * spec.version);
  const PointF physical[4] = {
      {spec.left, spec.top}, {spec.left + last * spec.stepX, spec.top},
      {spec.left + last * spec.stepX, spec.top + last * spec.stepY},
      {spec.left, spec.top + last * spec.stepY}};
  GridCandidate result = {{}, 0, spec.version, orientation};
  for (int i = 0; i < 4; ++i) {
    result.corner[i] = physical[(i + orientation) & 3];
  }
  return result;
}

GridCandidate MakeAffineGrid(const AffineGridSpec& spec, int orientation) {
  const float half = 0.5f * (16 + 4 * spec.version);
  const PointF alongX = {
      spec.axisX.x * half * spec.stepX,
      spec.axisX.y * half * spec.stepX};
  const PointF alongY = {
      spec.axisY.x * half * spec.stepY,
      spec.axisY.y * half * spec.stepY};
  const PointF physical[4] = {
      {spec.center.x - alongX.x - alongY.x,
       spec.center.y - alongX.y - alongY.y},
      {spec.center.x + alongX.x - alongY.x,
       spec.center.y + alongX.y - alongY.y},
      {spec.center.x + alongX.x + alongY.x,
       spec.center.y + alongX.y + alongY.y},
      {spec.center.x - alongX.x + alongY.x,
       spec.center.y - alongX.y + alongY.y}};
  GridCandidate result = {{}, 0, spec.version, orientation};
  for (int corner = 0; corner < 4; ++corner) {
    result.corner[corner] = physical[(corner + orientation) & 3];
  }
  return result;
}

PointF QuadPoint(const PointF quad[4], float u, float v) {
  return PointF{
      (1 - u) * (1 - v) * quad[0].x + u * (1 - v) * quad[1].x +
          u * v * quad[2].x + (1 - u) * v * quad[3].x,
      (1 - u) * (1 - v) * quad[0].y + u * (1 - v) * quad[1].y +
          u * v * quad[2].y + (1 - u) * v * quad[3].y};
}

GridCandidate MakeQuadGrid(const PointF paper[4], int version, float quiet,
                           int orientation) {
  const int dimension = 17 + 4 * version;
  const float inset = (quiet + 0.5f) / (dimension + 2 * quiet);
  const PointF physical[4] = {
      QuadPoint(paper, inset, inset),
      QuadPoint(paper, 1 - inset, inset),
      QuadPoint(paper, 1 - inset, 1 - inset),
      QuadPoint(paper, inset, 1 - inset)};
  GridCandidate result = {{}, 0, version, orientation};
  for (int corner = 0; corner < 4; ++corner) {
    result.corner[corner] = physical[(corner + orientation) & 3];
  }
  return result;
}

GridCandidate PerturbPaperCorner(GridCandidate grid, int corner) {
  PointF center = {0, 0};
  for (int i = 0; i < 4; ++i) {
    center.x += 0.25f * grid.corner[i].x;
    center.y += 0.25f * grid.corner[i].y;
  }
  const float dx = grid.corner[corner].x - center.x;
  const float dy = grid.corner[corner].y - center.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length > 0) {
    const float move = 0.25f * GridModuleSize(grid) / length;
    grid.corner[corner].x += dx * move;
    grid.corner[corner].y += dy * move;
  }
  return grid;
}

std::vector<GridCandidate> SemanticCandidates(
    const BinaryImage&, const PointF paper[4], int version) {
  const float quietZones[3] = {2, 3, 4};
  std::vector<GridCandidate> result;
  for (int inset = 0; inset < 3; ++inset) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      const GridCandidate base =
          MakeQuadGrid(paper, version, quietZones[inset], orientation);
      result.push_back(base);
      if (inset == 1) {
        result.push_back(PerturbPaperCorner(base, 0));
        result.push_back(PerturbPaperCorner(base, 2));
      }
    }
  }
  return result;
}

std::vector<GridCandidate> AxisCandidates(
    const BinaryImage& image, Bounds bounds, int version, float moduleSize,
    bool canvasAnchors) {
  const int dimension = 17 + 4 * version;
  AxisGridSpec anchors[3] = {
      {0, 0, static_cast<float>(bounds.right - bounds.left) / dimension,
       static_cast<float>(bounds.bottom - bounds.top) / dimension, version},
      {0, 0, static_cast<float>(image.width) / (dimension + 8),
       static_cast<float>(image.height) / (dimension + 8), version},
      {0, 0, static_cast<float>(image.width) / (dimension + 4),
       static_cast<float>(image.height) / (dimension + 4), version}};
  anchors[0].left = bounds.left + 0.5f * anchors[0].stepX;
  anchors[0].top = bounds.top + 0.5f * anchors[0].stepY;
  anchors[1].left = 4.5f * anchors[1].stepX;
  anchors[1].top = 4.5f * anchors[1].stepY;
  anchors[2].left = 2.5f * anchors[2].stepX;
  anchors[2].top = 2.5f * anchors[2].stepY;
  std::vector<GridCandidate> result;
  const float deltas[3] = {-0.5f, 0, 0.5f};
  const int anchorCount = canvasAnchors ? 3 : 1;
  for (int anchor = 0; anchor < anchorCount; ++anchor) {
    const float offsetStep = moduleSize > 0 ? moduleSize :
        0.5f * (anchors[anchor].stepX + anchors[anchor].stepY);
    for (int scale = 0; scale < 3; ++scale) {
      AxisGridSpec spec = anchors[anchor];
      spec.stepX = std::max(1.0f, spec.stepX + deltas[scale]);
      spec.stepY = std::max(1.0f, spec.stepY + deltas[scale]);
      for (int oy = 0; oy < 3; ++oy) {
        for (int ox = 0; ox < 3; ++ox) {
          // Only the exact scale explores a full 3x3 phase grid; scale
          // perturbations keep the center phase only.
          if ((scale != 1 && (ox != 1 || oy != 1)) ||
              (anchor == 2 && (scale != 1 || ox != 1 || oy != 1))) {
            continue;
          }
          spec.left = anchors[anchor].left + deltas[ox] * offsetStep;
          spec.top = anchors[anchor].top + deltas[oy] * offsetStep;
          for (int orientation = 0; orientation < 4; ++orientation) {
            result.push_back(MakeAxisGrid(spec, orientation));
          }
        }
      }
    }
  }
  return result;
}

bool EquivalentGrid(const GridCandidate& a, const GridCandidate& b) {
  if (a.version != b.version || a.orientation != b.orientation) {
    return false;
  }
  const float module = std::max(GridModuleSize(a), GridModuleSize(b));
  for (int i = 0; i < 4; ++i) {
    if (Distance(a.corner[i], b.corner[i]) > 0.45f * module) {
      return false;
    }
  }
  return true;
}

bool BetterCandidate(const GridCandidate& a, const GridCandidate& b) {
  if (a.score != b.score) {
    return a.score > b.score;
  }
  if (a.version != b.version) {
    return a.version < b.version;
  }
  if (a.orientation != b.orientation) {
    return a.orientation < b.orientation;
  }
  for (int i = 0; i < 4; ++i) {
    if (a.corner[i].x != b.corner[i].x) {
      return a.corner[i].x < b.corner[i].x;
    }
    if (a.corner[i].y != b.corner[i].y) {
      return a.corner[i].y < b.corner[i].y;
    }
  }
  return false;
}

void KeepLimited(std::vector<GridCandidate>& best, GridCandidate candidate,
                 size_t limit) {
  for (size_t i = 0; i < best.size(); ++i) {
    if (EquivalentGrid(best[i], candidate)) {
      if (BetterCandidate(candidate, best[i])) {
        best[i] = candidate;
        std::sort(best.begin(), best.end(), BetterCandidate);
      }
      return;
    }
  }
  best.push_back(candidate);
  std::sort(best.begin(), best.end(), BetterCandidate);
  if (best.size() > limit) {
    best.resize(limit);
  }
}

void PrintCandidateStats(
    const char* label, const std::vector<GridCandidate>& candidates) {
  if (std::getenv("ZXING_QR_SEARCH_STATS") == NULL) {
    return;
  }
  std::fprintf(stderr, "qr-model %s=%zu", label, candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    float left = candidates[i].corner[0].x;
    float top = candidates[i].corner[0].y;
    for (int corner = 1; corner < 4; ++corner) {
      left = std::min(left, candidates[i].corner[corner].x);
      top = std::min(top, candidates[i].corner[corner].y);
    }
    std::fprintf(
        stderr, " [v%d/o%d %.3f %.1f,%.1f m%.2f]",
        candidates[i].version, candidates[i].orientation,
        candidates[i].score, left, top, GridModuleSize(candidates[i]));
  }
  std::fprintf(stderr, "\n");
}

struct QuickModelSampler {
  const BinaryImage* image;
  const AffineGridSpec* spec;
  PointF stepX;
  PointF stepY;
  float half;
  float radius;
  bool axisAligned;
};

PointF QuickModelPoint(const QuickModelSampler& sampler, int x, int y) {
  const float dx = x - sampler.half;
  const float dy = y - sampler.half;
  return PointF{
      sampler.spec->center.x +
          dx * sampler.stepX.x + dy * sampler.stepY.x,
      sampler.spec->center.y +
          dx * sampler.stepX.y + dy * sampler.stepY.y};
}

SampleResult QuickBlack(
    const BinaryImage& image, PointF point, float radius) {
  const Bounds requested = {
      static_cast<int>(std::ceil(point.x - radius)),
      static_cast<int>(std::ceil(point.y - radius)),
      static_cast<int>(std::floor(point.x + radius)) + 1,
      static_cast<int>(std::floor(point.y + radius)) + 1};
  const Bounds clipped = {
      std::max(0, requested.left), std::max(0, requested.top),
      std::min(image.width, requested.right),
      std::min(image.height, requested.bottom)};
  const int requestedArea =
      std::max(0, requested.right - requested.left) *
      std::max(0, requested.bottom - requested.top);
  const int coveredArea =
      std::max(0, clipped.right - clipped.left) *
      std::max(0, clipped.bottom - clipped.top);
  if (requestedArea == 0 || coveredArea == 0) {
    return SampleResult{0, 0};
  }
  return SampleResult{
      static_cast<float>(RegionSum(image, clipped)) / coveredArea,
      static_cast<float>(coveredArea) / requestedArea};
}

float QuickAgreement(
    const QuickModelSampler& sampler, int x, int y, bool black) {
  const PointF point = QuickModelPoint(sampler, x, y);
  const SampleResult sample =
      QuickBlack(*sampler.image, point, sampler.radius);
  return (black ? sample.black : 1 - sample.black) * sample.coverage;
}

float QuickCenterAgreement(
    const QuickModelSampler& sampler, int x, int y, bool black) {
  float sample = 0;
  if (!DirectBlack(
          *sampler.image, QuickModelPoint(sampler, x, y), sample)) {
    return 0;
  }
  return black ? sample : 1 - sample;
}

float QuickFinderCoreScore(const QuickModelSampler& sampler, int group) {
  const int dimension = 17 + 4 * sampler.spec->version;
  const int left = group == 1 ? dimension - 7 : 0;
  const int top = group == 2 ? dimension - 7 : 0;
  const int samples[4][3] = {
      {0, 0, 1}, {6, 6, 1}, {3, 3, 1}, {1, 1, 0}};
  float score = 0;
  for (int i = 0; i < 4; ++i) {
    score += QuickCenterAgreement(
        sampler, left + samples[i][0], top + samples[i][1],
        samples[i][2] != 0);
  }
  return score / 4;
}

float QuickFinderScore(
    const QuickModelSampler& sampler, int group, float core) {
  const int dimension = 17 + 4 * sampler.spec->version;
  const int left = group == 1 ? dimension - 7 : 0;
  const int top = group == 2 ? dimension - 7 : 0;
  const float extra =
      QuickAgreement(sampler, left + 6, top, true) +
      QuickAgreement(sampler, left, top + 6, true);
  return (4 * core + extra) / 6;
}

float QuickTimingScore(const QuickModelSampler& sampler) {
  const int dimension = 17 + 4 * sampler.spec->version;
  const int length = std::max(1, dimension - 16);
  float score = 0;
  int count = 0;
  for (int sample = 0; sample < 4; ++sample) {
    const int offset = 8 + sample * (length - 1) / 3;
    const bool black = (offset & 1) == 0;
    score += QuickAgreement(sampler, offset, 6, black);
    score += QuickAgreement(sampler, 6, offset, black);
    count += 2;
  }
  return count == 0 ? 0.5f : score / count;
}

float QuickTimingCoreScore(const QuickModelSampler& sampler) {
  const int dimension = 17 + 4 * sampler.spec->version;
  const int offsets[2] = {8, dimension - 9};
  float score = 0;
  for (int i = 0; i < 2; ++i) {
    const bool black = (offsets[i] & 1) == 0;
    score += QuickCenterAgreement(sampler, offsets[i], 6, black);
    score += QuickCenterAgreement(sampler, 6, offsets[i], black);
  }
  return score / 4;
}

float QuickModelPrescore(const BinaryImage& image,
                         const AffineGridSpec& spec) {
  const float offAxis = std::min(
      std::max(std::fabs(spec.axisX.x), std::fabs(spec.axisX.y)),
      std::max(std::fabs(spec.axisY.x), std::fabs(spec.axisY.y)));
  const QuickModelSampler sampler = {
      &image, &spec,
      {spec.stepX * spec.axisX.x, spec.stepX * spec.axisX.y},
      {spec.stepY * spec.axisY.x, spec.stepY * spec.axisY.y},
      0.5f * (16 + 4 * spec.version),
      std::max(0.6f, spec.stepX * 0.26f),
      offAxis > 0.9999f};
  float core[3] = {
      QuickFinderCoreScore(sampler, 0),
      QuickFinderCoreScore(sampler, 1),
      QuickFinderCoreScore(sampler, 2)};
  float rankedCore[3] = {core[0], core[1], core[2]};
  std::sort(rankedCore, rankedCore + 3, std::greater<float>());
  if (rankedCore[0] < 0.58f) {
    return -1;
  }
  if (rankedCore[1] < 0.50f &&
      QuickTimingCoreScore(sampler) < 0.50f) {
    return -1;
  }
  float finder[3] = {
      QuickFinderScore(sampler, 0, core[0]),
      QuickFinderScore(sampler, 1, core[1]),
      QuickFinderScore(sampler, 2, core[2])};
  std::sort(finder, finder + 3, std::greater<float>());
  const float timing = QuickTimingScore(sampler);
  if (finder[0] < 0.50f ||
      (finder[1] < 0.35f && timing < 0.48f) || timing < 0.28f) {
    return -1;
  }
  return 0.55f * finder[0] + 0.20f * finder[1] + 0.25f * timing;
}

struct ModelScan {
  struct Seed {
    AffineGridSpec spec;
    float score;
  };
  const BinaryImage* image;
  const std::vector<FixedModule>* fixed;
  std::vector<GridCandidate>* best;
  std::vector<Seed> seeds;
  std::vector<Seed> reserved;
  std::vector<Seed> regional;
  std::vector<uint8_t> seen;
  ModelSearchStats* stats;
  bool collectRegional;
  ModelAxes axes;
  float module;
  int version;
  int stride;
  int firstX;
  int firstY;
  int countX;
  int countY;
};

bool BetterModelSeed(const ModelScan::Seed& a, const ModelScan::Seed& b) {
  if (a.score != b.score) {
    return a.score > b.score;
  }
  if (a.spec.center.y != b.spec.center.y) {
    return a.spec.center.y < b.spec.center.y;
  }
  return a.spec.center.x < b.spec.center.x;
}

void KeepModelSeed(
    std::vector<ModelScan::Seed>& seeds, const ModelScan::Seed& candidate,
    float module, size_t limit) {
  for (size_t i = 0; i < seeds.size(); ++i) {
    if (std::fabs(seeds[i].spec.center.x - candidate.spec.center.x) < module &&
        std::fabs(seeds[i].spec.center.y - candidate.spec.center.y) < module) {
      if (BetterModelSeed(candidate, seeds[i])) {
        seeds[i] = candidate;
        std::sort(seeds.begin(), seeds.end(), BetterModelSeed);
      }
      return;
    }
  }
  seeds.push_back(candidate);
  std::sort(seeds.begin(), seeds.end(), BetterModelSeed);
  if (seeds.size() > limit) {
    seeds.resize(limit);
  }
}

void ScoreModelOrigin(ModelScan& scan, int x, int y) {
  const float half = 0.5f * (16 + 4 * scan.version) * scan.module;
  const AffineGridSpec spec = {
      {x + half * (scan.axes.axisX.x + scan.axes.axisY.x),
       y + half * (scan.axes.axisX.y + scan.axes.axisY.y)},
      scan.axes.axisX, scan.axes.axisY,
      scan.module, scan.module, scan.version};
  ++scan.stats->origins;
  const float score = QuickModelPrescore(*scan.image, spec);
  if (score < 0.55f) {
    return;
  }
  ++scan.stats->prescorePasses;
  const ModelScan::Seed seed = {spec, score};
  KeepModelSeed(scan.seeds, seed, scan.module, 16);
  if (scan.collectRegional) {
    KeepModelSeed(scan.regional, seed, scan.module, 1);
  }
}

void ScanModelWindow(ModelScan& scan, Bounds origins) {
  const int startX = std::max(
      0, (origins.left - scan.firstX + scan.stride - 1) / scan.stride);
  const int startY = std::max(
      0, (origins.top - scan.firstY + scan.stride - 1) / scan.stride);
  const int endX = std::min(
      scan.countX, (origins.right - scan.firstX + scan.stride - 1) /
                       scan.stride);
  const int endY = std::min(
      scan.countY, (origins.bottom - scan.firstY + scan.stride - 1) /
                       scan.stride);
  if (startX >= endX || startY >= endY) {
    return;
  }
  for (int iy = startY; iy < endY; ++iy) {
    for (int ix = startX; ix < endX; ++ix) {
      const size_t index = static_cast<size_t>(iy) * scan.countX + ix;
      if (scan.seen[index]) {
        continue;
      }
      scan.seen[index] = 1;
      ScoreModelOrigin(
          scan, scan.firstX + ix * scan.stride,
          scan.firstY + iy * scan.stride);
    }
  }
}

/*
 * Semantic rotations share finder/timing geometry. They are scored only after
 * a physical origin survives, because the dark module and version bits make
 * orientation structurally significant.
 */
void ScoreSeedOrientations(
    ModelScan& scan, const std::vector<ModelScan::Seed>& seeds) {
  for (size_t i = 0; i < seeds.size(); ++i) {
    GridCandidate strongest = {{}, -1, scan.version, 0};
    for (int orientation = 0; orientation < 4; ++orientation) {
      GridCandidate candidate =
          MakeAffineGrid(seeds[i].spec, orientation);
      ++scan.stats->fullScores;
      if (CoarseModelScore(
              *scan.image, candidate, *scan.fixed) >= 0 &&
          BetterCandidate(candidate, strongest)) {
        strongest = candidate;
      }
    }
    if (strongest.score >= 0) {
      KeepLimited(*scan.best, strongest, 8);
    }
  }
}

void ReserveRegionSeeds(ModelScan& scan) {
  const size_t limit = 8;
  for (size_t i = 0; i < scan.regional.size() &&
       scan.reserved.size() < limit; ++i) {
    KeepModelSeed(scan.reserved, scan.regional[i], scan.module, limit);
  }
  scan.regional.clear();
}

std::vector<ModelScan::Seed> MergeModelSeeds(ModelScan& scan) {
  const size_t fullReserve = std::min<size_t>(1, scan.seeds.size());
  for (size_t i = 0; i < fullReserve; ++i) {
    KeepModelSeed(scan.reserved, scan.seeds[i], scan.module, 6);
  }
  std::vector<ModelScan::Seed> merged;
  const size_t limit = 14;
  for (size_t i = 0; i < scan.reserved.size(); ++i) {
    KeepModelSeed(merged, scan.reserved[i], scan.module, limit);
  }
  for (size_t i = 0; i < scan.seeds.size(); ++i) {
    KeepModelSeed(merged, scan.seeds[i], scan.module, limit);
  }
  scan.stats->reservedSeeds += scan.reserved.size();
  scan.stats->uniqueSeeds += merged.size();
  scan.stats->duplicateSeeds +=
      scan.reserved.size() + scan.seeds.size() - merged.size();
  return merged;
}

bool ModelOriginRange(
    const BinaryImage& image, const ModelAxes& axes, float span, int radius,
    Bounds& range) {
  const float dx[4] = {
      0, axes.axisX.x * span, axes.axisY.x * span,
      (axes.axisX.x + axes.axisY.x) * span};
  const float dy[4] = {
      0, axes.axisX.y * span, axes.axisY.y * span,
      (axes.axisX.y + axes.axisY.y) * span};
  const float minX = *std::min_element(dx, dx + 4);
  const float maxX = *std::max_element(dx, dx + 4);
  const float minY = *std::min_element(dy, dy + 4);
  const float maxY = *std::max_element(dy, dy + 4);
  range = Bounds{
      static_cast<int>(std::ceil(radius - minX)),
      static_cast<int>(std::ceil(radius - minY)),
      static_cast<int>(std::floor(image.width - 1 - radius - maxX)) + 1,
      static_cast<int>(std::floor(image.height - 1 - radius - maxY)) + 1};
  return range.left < range.right && range.top < range.bottom;
}

Bounds ModelOriginBounds(
    const SearchRegion& region, const ModelAxes& axes, float span,
    float module) {
  const int margin = static_cast<int>(2 * module + 0.5f);
  const float dx[4] = {
      0, axes.axisX.x * span, axes.axisY.x * span,
      (axes.axisX.x + axes.axisY.x) * span};
  const float dy[4] = {
      0, axes.axisX.y * span, axes.axisY.y * span,
      (axes.axisX.y + axes.axisY.y) * span};
  return Bounds{
      static_cast<int>(region.bounds.left - *std::min_element(dx, dx + 4)) -
          margin,
      static_cast<int>(region.bounds.top - *std::min_element(dy, dy + 4)) -
          margin,
      static_cast<int>(region.bounds.right - *std::max_element(dx, dx + 4)) +
          margin + 1,
      static_cast<int>(region.bounds.bottom - *std::max_element(dy, dy + 4)) +
          margin + 1};
}

struct ModelSearch {
  const BinaryImage* image;
  const std::vector<SearchRegion>* regions;
  const std::vector<ScaleHint>* hints;
  std::vector<GridCandidate>* best;
  ModelSearchStats* stats;
};

float DistinctScoreMargin(const std::vector<GridCandidate>& candidates);

bool ModelSearchFinished(const std::vector<GridCandidate>& best) {
  if (best.empty()) {
    return false;
  }
  if (best[0].score >= 0.92f) {
    return true;
  }
  return best[0].score >= 0.88f && DistinctScoreMargin(best) >= 0.02f;
}

std::vector<int> PreferredVersions(
    const BinaryImage& image, float module,
    const std::vector<SearchRegion>& regions) {
  std::vector<int> ordered;
  ordered.reserve(10);
  int estimate = -1;
  if (module >= 1.0f && !regions.empty()) {
    float side = 0;
    for (size_t i = 0; i + 1 < regions.size(); ++i) {
      const Bounds& b = regions[i].bounds;
      side = std::max(
          side, static_cast<float>(std::max(b.right - b.left, b.bottom - b.top)));
    }
    if (side < 21) {
      side = static_cast<float>(std::min(image.width, image.height));
    }
    estimate = static_cast<int>(
        std::floor(((side / module) - 17.0f) / 4.0f + 0.5f));
    estimate = std::max(1, std::min(10, estimate));
  }
  if (estimate > 0) {
    ordered.push_back(estimate);
    if (estimate > 1) {
      ordered.push_back(estimate - 1);
    }
    if (estimate < 10) {
      ordered.push_back(estimate + 1);
    }
  }
  for (int version = 1; version <= 10; ++version) {
    bool present = false;
    for (size_t i = 0; i < ordered.size(); ++i) {
      present = present || ordered[i] == version;
    }
    if (!present) {
      ordered.push_back(version);
    }
  }
  return ordered;
}

void SearchModelAxis(ModelSearch& search) {
  const BinaryImage& image = *search.image;
  const ModelAxes axes = {PointF{1, 0}, PointF{0, 1}};
  for (size_t hint = 0; hint < search.hints->size(); ++hint) {
    const float module = (*search.hints)[hint].moduleSize;
    const std::vector<int> versions =
        PreferredVersions(image, module, *search.regions);
    for (size_t vi = 0; vi < versions.size(); ++vi) {
      const int version = versions[vi];
      if (!ModelVersionFits(version, module, image.width, image.height)) {
        continue;
      }
      const std::vector<FixedModule> fixed = BuildFixedModules(version);
      const int dimension = 17 + 4 * version;
      const float span = (dimension - 1) * module;
      const int radius = std::max(1, static_cast<int>(module * 0.28f + 0.5f));
      Bounds range;
      if (!ModelOriginRange(image, axes, span, radius, range)) {
        continue;
      }
      const int stride = ModelScanStride(
          module, range.right - range.left, range.bottom - range.top);
      ModelScan scan = {
          &image, &fixed, search.best, std::vector<ModelScan::Seed>(),
          std::vector<ModelScan::Seed>(), std::vector<ModelScan::Seed>(),
          std::vector<uint8_t>(), search.stats, false, axes, module, version,
          stride, range.left, range.top,
          (range.right - range.left + stride - 1) / stride,
          (range.bottom - range.top + stride - 1) / stride};
      scan.seen.assign(
          static_cast<size_t>(scan.countX) * scan.countY, 0);
      for (size_t region = 0; region + 1 < search.regions->size(); ++region) {
        scan.regional.clear();
        scan.collectRegional = true;
        ScanModelWindow(
            scan, ModelOriginBounds(
                      (*search.regions)[region], axes, span, module));
        ReserveRegionSeeds(scan);
      }
      scan.regional.clear();
      scan.collectRegional = false;
      ScanModelWindow(
          scan, Bounds{scan.firstX, scan.firstY,
                       scan.firstX + scan.countX * stride,
                       scan.firstY + scan.countY * stride});
      const std::vector<ModelScan::Seed> merged = MergeModelSeeds(scan);
      ScoreSeedOrientations(scan, merged);
      if (ModelSearchFinished(*search.best)) {
        return;
      }
    }
  }
}

std::vector<GridCandidate> SearchQRModel(
    const DetectionLayer& layer, const std::vector<SearchRegion>& regions,
    const std::vector<ScaleHint>& hints) {
  std::vector<GridCandidate> best;
  ModelSearchStats stats = {0, 0, 0, 0, 0, 0};
  const BinaryImage& image = layer.image;
  if (std::getenv("ZXING_QR_SEARCH_STATS") != NULL) {
    std::fprintf(stderr, "qr-model hints=%zu", hints.size());
    for (size_t i = 0; i < hints.size(); ++i) {
      std::fprintf(
          stderr, " %.2f/%.3f", hints[i].moduleSize, hints[i].support);
    }
    std::fprintf(stderr, " axes 0.0\n");
  }
  ModelSearch search = {
      &image, &regions, &hints, &best, &stats};
  SearchModelAxis(search);
  if (std::getenv("ZXING_QR_SEARCH_STATS") != NULL) {
    std::fprintf(
        stderr,
        "qr-model axis=%.3f buckets=0 origins=%zu "
        "prescore=%zu "
        "reserved=%zu unique=%zu deduped=%zu scored=%zu kept=%zu\n",
        best.empty() ? -1.0f : best[0].score,
        stats.origins, stats.prescorePasses,
        stats.reservedSeeds, stats.uniqueSeeds, stats.duplicateSeeds,
        stats.fullScores, best.size());
  }
  PrintCandidateStats("detection", best);
  return best;
}

float DistinctScoreMargin(const std::vector<GridCandidate>& candidates) {
  if (candidates.empty()) {
    return 0;
  }
  for (size_t i = 1; i < candidates.size(); ++i) {
    if (!EquivalentGrid(candidates[0], candidates[i])) {
      return candidates[0].score - candidates[i].score;
    }
  }
  return 0;
}

bool HasAbsoluteEvidence(const std::vector<GridCandidate>& candidates) {
  return !candidates.empty() && candidates[0].score >= 0.72f;
}

void ScoreCandidates(
    const BinaryImage& image, std::vector<GridCandidate> candidates,
    int version, std::vector<GridCandidate>& best) {
  const std::vector<FixedModule> fixed = BuildFixedModules(version);
  std::vector<GridCandidate> shortlist;
  for (size_t i = 0; i < candidates.size(); ++i) {
    CoarseModelScore(image, candidates[i], fixed);
    if (candidates[i].score >= 0) {
      KeepLimited(shortlist, candidates[i], 4);
    }
  }
  for (size_t i = 0; i < shortlist.size(); ++i) {
    ScoreGrid(image, shortlist[i], fixed);
    KeepLimited(best, shortlist[i], 8);
  }
}

bool ValidQRBounds(Bounds bounds) {
  return bounds.right - bounds.left >= 21 &&
      bounds.bottom - bounds.top >= 21;
}

std::vector<AxisSource> FindAxisSources(
    const BinaryImage& source, const DetectionLayer& layer,
    const std::vector<SearchRegion>& regions) {
  std::vector<AxisSource> result;
  result.reserve(regions.size() + 1);
  for (size_t i = 0; i < regions.size(); ++i) {
    const Bounds sourceRegion =
        DetectionToSource(layer, regions[i].bounds);
    const Bounds bounds = FindDarkBounds(source, sourceRegion);
    if (ValidQRBounds(bounds)) {
      result.push_back(AxisSource{bounds, false});
    }
  }
  const Bounds full = {0, 0, source.width, source.height};
  if (ValidQRBounds(full)) {
    result.push_back(AxisSource{full, true});
  }
  return result;
}

// Score axis grids on the detection thumbnail first; remap winners later.
std::vector<AxisSource> DetectionAxisSources(
    const BinaryImage& image, const std::vector<SearchRegion>& regions) {
  std::vector<AxisSource> result;
  result.reserve(regions.size());
  for (size_t i = 0; i < regions.size(); ++i) {
    const bool fullFrame =
        regions[i].bounds.left == 0 && regions[i].bounds.top == 0 &&
        regions[i].bounds.right == image.width &&
        regions[i].bounds.bottom == image.height;
    if (ValidQRBounds(regions[i].bounds)) {
      result.push_back(AxisSource{regions[i].bounds, fullFrame});
    }
  }
  return result;
}

std::vector<GridCandidate> SourceModelCandidates(
    const BinaryImage& source, const DetectionLayer& layer,
    const std::vector<GridCandidate>& detection) {
  std::vector<GridCandidate> result;
  for (int version = 1; version <= 10; ++version) {
    const std::vector<FixedModule> fixed = BuildFixedModules(version);
    for (size_t i = 0; i < detection.size(); ++i) {
      if (detection[i].version != version) {
        continue;
      }
      GridCandidate candidate = DetectionToSource(layer, detection[i]);
      ScoreGrid(source, candidate, fixed);
      if (candidate.score >= 0) {
        KeepLimited(result, candidate, 8);
      }
    }
  }
  PrintCandidateStats("source", result);
  return result;
}

void ScoreAxisBounds(
    const BinaryImage& image, const AxisSource& source,
    std::vector<GridCandidate>& best) {
  const ModuleEstimate estimate = EstimateModuleSize(image, source.bounds);
  // Prefer module-size-aware pruning; full canvas still keeps the full set.
  std::vector<int> versions = CandidateVersions(estimate, source.bounds);
  if (!source.canvasAnchors && estimate.size > 0) {
    const float side = static_cast<float>(std::min(
        source.bounds.right - source.bounds.left,
        source.bounds.bottom - source.bounds.top));
    std::vector<int> pruned;
    for (size_t i = 0; i < versions.size(); ++i) {
      const float expected =
          (17 + 4 * versions[i]) * estimate.size;
      if (expected >= side * 0.55f && expected <= side * 1.35f) {
        pruned.push_back(versions[i]);
      }
    }
    if (!pruned.empty()) {
      versions.swap(pruned);
    }
  }
  for (size_t i = 0; i < versions.size(); ++i) {
    ScoreCandidates(
        image, AxisCandidates(
            image, source.bounds, versions[i], estimate.size,
            source.canvasAnchors),
        versions[i], best);
    if (!best.empty() && best[0].score >= 0.92f &&
        DistinctScoreMargin(best) >= 0.02f) {
      break;
    }
  }
}

GridCandidate RefineCandidate(
    const BinaryImage& image, GridCandidate candidate,
    const std::vector<FixedModule>& fixed, bool forceCorners);
bool NeedsAxisCornerRefinement(const GridCandidate& candidate);
bool RenderableCandidate(
    const BinaryImage& image, const GridCandidate& candidate);

std::vector<GridCandidate> RefineTop(
    const BinaryImage& image, const std::vector<GridCandidate>& coarse,
    bool allowAxisCorners) {
  std::vector<GridCandidate> refined;
  const size_t count = std::min<size_t>(4, coarse.size());
  for (size_t i = 0; i < count; ++i) {
    const std::vector<FixedModule> fixed =
        BuildFixedModules(coarse[i].version);
    KeepLimited(refined, RefineCandidate(image, coarse[i], fixed, false), 8);
  }
  if (allowAxisCorners && !refined.empty() &&
      IsAxisAligned(refined[0]) &&
      NeedsAxisCornerRefinement(refined[0])) {
    std::vector<GridCandidate> perspective;
    for (size_t i = 0; i < count; ++i) {
      if (IsAxisAligned(coarse[i])) {
        const std::vector<FixedModule> fixed =
            BuildFixedModules(coarse[i].version);
        KeepLimited(
            perspective, RefineCandidate(image, coarse[i], fixed, true), 8);
      }
    }
    for (size_t i = 0; i < perspective.size(); ++i) {
      KeepLimited(refined, perspective[i], 8);
    }
  }
  return refined;
}

void MergeCoarseCandidates(
    std::vector<GridCandidate>& global,
    const std::vector<GridCandidate>& local) {
  for (size_t i = 0; i < local.size(); ++i) {
    KeepLimited(global, local[i], 8);
  }
}

bool AppendDistinctCandidate(
    std::vector<GridCandidate>& result,
    const GridCandidate& candidate) {
  if (result.size() >= 8) {
    return false;
  }
  for (size_t i = 0; i < result.size(); ++i) {
    if (EquivalentGrid(result[i], candidate)) {
      return false;
    }
  }
  result.push_back(candidate);
  return true;
}

void ReserveCandidateSource(
    std::vector<GridCandidate>& result,
    const std::vector<GridCandidate>& source) {
  for (size_t i = 0; i < source.size(); ++i) {
    if (AppendDistinctCandidate(result, source[i])) {
      return;
    }
  }
}

std::vector<GridCandidate> FairCoarseCandidates(
    const std::vector<GridCandidate>& axis,
    const std::vector<GridCandidate>& paper,
    const std::vector<GridCandidate>& model) {
  std::vector<GridCandidate> ranked;
  MergeCoarseCandidates(ranked, axis);
  MergeCoarseCandidates(ranked, paper);
  MergeCoarseCandidates(ranked, model);
  std::vector<GridCandidate> result;
  ReserveCandidateSource(result, axis);
  ReserveCandidateSource(result, paper);
  ReserveCandidateSource(result, model);
  for (size_t i = 0; i < ranked.size() && result.size() < 8; ++i) {
    AppendDistinctCandidate(result, ranked[i]);
  }
  std::sort(result.begin(), result.end(), BetterCandidate);
  return result;
}

std::vector<GridCandidate> CollectAxisCandidates(
    const BinaryImage& image, const std::vector<AxisSource>& sources) {
  std::vector<GridCandidate> result;
  std::vector<const AxisSource*> deferred;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (sources[i].canvasAnchors) {
      deferred.push_back(&sources[i]);
      continue;
    }
    std::vector<GridCandidate> local;
    ScoreAxisBounds(image, sources[i], local);
    MergeCoarseCandidates(result, local);
  }
  const bool strongLocal =
      !result.empty() && result[0].score >= 0.88f &&
      DistinctScoreMargin(result) >= 0.015f;
  if (!strongLocal) {
    for (size_t i = 0; i < deferred.size(); ++i) {
      std::vector<GridCandidate> local;
      ScoreAxisBounds(image, *deferred[i], local);
      MergeCoarseCandidates(result, local);
    }
  }
  return result;
}

std::vector<GridCandidate> CollectPaperCandidates(
    const BinaryImage& image, const PointF paper[4]) {
  std::vector<GridCandidate> coarse;
  // Paper insets already pin geometry; score a reduced quiet-zone set.
  for (int version = 1; version <= 10; ++version) {
    ScoreCandidates(
        image, SemanticCandidates(image, paper, version), version, coarse);
    // Early stop once a clearly winning paper grid exists.
    if (!coarse.empty() && coarse[0].score >= 0.92f &&
        DistinctScoreMargin(coarse) >= 0.02f) {
      break;
    }
  }
  return coarse;
}

GridCandidate TranslateGrid(GridCandidate grid, float dx, float dy) {
  for (int i = 0; i < 4; ++i) {
    grid.corner[i].x += dx;
    grid.corner[i].y += dy;
  }
  return grid;
}

GridCandidate ScaleGrid(GridCandidate grid, int axis, float delta) {
  const int end = axis == 0 ? 1 : 3;
  const float length = Distance(grid.corner[0], grid.corner[end]);
  if (length <= 0) {
    return grid;
  }
  const float factor = delta * (16 + 4 * grid.version) / length;
  const float dx = (grid.corner[end].x - grid.corner[0].x) * factor;
  const float dy = (grid.corner[end].y - grid.corner[0].y) * factor;
  const int first = axis == 0 ? 1 : 2;
  grid.corner[first].x += dx;
  grid.corner[first].y += dy;
  grid.corner[first + 1].x += dx;
  grid.corner[first + 1].y += dy;
  return grid;
}

GridCandidate MoveCorner(GridCandidate grid, int corner, int axis,
                         float delta) {
  if (axis == 0) {
    grid.corner[corner].x += delta;
  } else {
    grid.corner[corner].y += delta;
  }
  return grid;
}

void PreferScored(const BinaryImage& image, GridCandidate trial,
                  const std::vector<FixedModule>& fixed, GridCandidate& best) {
  ScoreGrid(image, trial, fixed);
  if (trial.score > best.score) {
    best = trial;
  }
}

bool NeedsAxisCornerRefinement(const GridCandidate& candidate) {
  const float horizontal =
      Distance(candidate.corner[0], candidate.corner[1]) +
      Distance(candidate.corner[2], candidate.corner[3]);
  const float vertical =
      Distance(candidate.corner[0], candidate.corner[3]) +
      Distance(candidate.corner[1], candidate.corner[2]);
  if (horizontal <= 0 || vertical <= 0) {
    return false;
  }
  const float ratio = horizontal / vertical;
  return candidate.score < 0.90f ||
      (candidate.score < 0.97f && (ratio < 0.98f || ratio > 1.02f));
}

bool HasPerspectiveEvidence(const GridCandidate& candidate) {
  const float top = Distance(candidate.corner[0], candidate.corner[1]);
  const float right = Distance(candidate.corner[1], candidate.corner[2]);
  const float bottom = Distance(candidate.corner[2], candidate.corner[3]);
  const float left = Distance(candidate.corner[3], candidate.corner[0]);
  if (std::min(std::min(top, bottom), std::min(left, right)) <= 0) {
    return false;
  }
  const float horizontal =
      std::fabs(top - bottom) / std::max(top, bottom);
  const float vertical =
      std::fabs(left - right) / std::max(left, right);
  return horizontal > 0.015f || vertical > 0.015f;
}

GridCandidate RefineSteps(
    const BinaryImage& image, GridCandidate candidate,
    const std::vector<FixedModule>& fixed, bool refineCorners) {
  const float stages[3] = {0.5f, 0.2f, 0.1f};
  const int stageCount = refineCorners || candidate.score < 0.93f ? 3 : 2;
  for (int stage = 0; stage < stageCount; ++stage) {
    const GridCandidate baseline = candidate;
    const float move = stages[stage] * GridModuleSize(baseline);
    PreferScored(image, TranslateGrid(baseline, -move, 0), fixed, candidate);
    PreferScored(image, TranslateGrid(baseline, move, 0), fixed, candidate);
    PreferScored(image, TranslateGrid(baseline, 0, -move), fixed, candidate);
    PreferScored(image, TranslateGrid(baseline, 0, move), fixed, candidate);
    PreferScored(image, ScaleGrid(baseline, 0, -stages[stage]), fixed,
                 candidate);
    PreferScored(image, ScaleGrid(baseline, 0, stages[stage]), fixed,
                 candidate);
    PreferScored(image, ScaleGrid(baseline, 1, -stages[stage]), fixed,
                 candidate);
    PreferScored(image, ScaleGrid(baseline, 1, stages[stage]), fixed,
                 candidate);
    if (refineCorners) {
      for (int corner = 0; corner < 4; ++corner) {
        for (int axis = 0; axis < 2; ++axis) {
        PreferScored(
              image, MoveCorner(baseline, corner, axis, -move),
              fixed, candidate);
          PreferScored(
              image, MoveCorner(baseline, corner, axis, move),
              fixed, candidate);
        }
      }
    }
  }
  return candidate;
}

GridCandidate RefineCandidate(
    const BinaryImage& image, GridCandidate candidate,
    const std::vector<FixedModule>& fixed, bool forceCorners) {
  return RefineSteps(
      image, candidate, fixed,
      forceCorners || HasPerspectiveEvidence(candidate));
}

zxing::ArrayRef<char> RenderNormalized(
    const BinaryImage& image, const GridCandidate& grid,
    const std::vector<FixedModule>& fixed, int& outputSize) {
  const int dimension = 17 + 4 * grid.version;
  const int scale = 8;
  std::vector<int8_t> fixedValues(
      static_cast<size_t>(dimension) * dimension, -1);
  for (size_t i = 0; i < fixed.size(); ++i) {
    fixedValues[static_cast<size_t>(fixed[i].y) * dimension + fixed[i].x] =
        static_cast<int8_t>(fixed[i].black);
  }
  outputSize = (dimension + 8) * scale;
  zxing::ArrayRef<char> output(outputSize * outputSize * 4);
  std::fill(&output[0], &output[0] + outputSize * outputSize * 4,
            static_cast<char>(255));
  const float radius = std::max(0.6f, GridModuleSize(grid) * 0.30f);
  for (int moduleY = 0; moduleY < dimension; ++moduleY) {
    for (int moduleX = 0; moduleX < dimension; ++moduleX) {
      const int fixedValue =
          fixedValues[static_cast<size_t>(moduleY) * dimension + moduleX];
      const bool black = fixedValue >= 0 ? fixedValue != 0 :
          SampleGridBlack(
              image, grid, moduleX, moduleY, radius).black >= 0.5f;
      const unsigned char value = black ? 0 : 255;
      const int startX = (moduleX + 4) * scale;
      const int startY = (moduleY + 4) * scale;
      for (int y = 0; y < scale; ++y) {
        for (int x = 0; x < scale; ++x) {
          const int pixel = ((startY + y) * outputSize + startX + x) * 4;
          output[pixel] = static_cast<char>(value);
          output[pixel + 1] = static_cast<char>(value);
          output[pixel + 2] = static_cast<char>(value);
          output[pixel + 3] = static_cast<char>(255);
        }
      }
    }
  }
  return output;
}

std::vector<GridCandidate> FindRefinedCandidates(
    const BinaryImage& binary) {
  const auto stageMs = [](const char* label, SteadyClock::time_point start) {
    if (std::getenv("ZXING_QR_STAGE_MS") == NULL) {
      return SteadyClock::now();
    }
    const SteadyClock::time_point end = SteadyClock::now();
    std::fprintf(
        stderr, "qr-stage %s=%.1fms\n", label,
        std::chrono::duration<double, std::milli>(end - start).count());
    return end;
  };
  SteadyClock::time_point t = SteadyClock::now();
  const BorderEvidence border = MeasureBorder(binary);
  const DetectionLayer layer = BuildDetectionLayer(binary);
  t = stageMs("layer", t);
  if (!DetectionMappingRoundTrips(layer)) {
    return std::vector<GridCandidate>();
  }
  const std::vector<SearchRegion> regions = FindQRRegions(layer.image);
  t = stageMs("regions", t);
  const size_t pixels =
      static_cast<size_t>(binary.width) * static_cast<size_t>(binary.height);
  const bool largeWorkingImage = pixels > 980u * 980u;

  // Cheap axis candidates first on large images: often already win after
  // refine and avoid the heavy model origin grid entirely.
  std::vector<GridCandidate> axis;
  if (largeWorkingImage) {
    axis = SourceModelCandidates(
        binary, layer,
        CollectAxisCandidates(
            layer.image, DetectionAxisSources(layer.image, regions)));
    t = stageMs("axis", t);
    if (!axis.empty() && axis[0].score >= 0.78f) {
      std::vector<GridCandidate> axisRefined =
          RefineTop(binary, axis, true);
      if (!axisRefined.empty() && axisRefined[0].score >= 0.92f) {
        stageMs("refine", t);
        PrintCandidateStats("refined", axisRefined);
        return axisRefined;
      }
      MergeCoarseCandidates(axis, axisRefined);
    }
  }

  const std::vector<ScaleHint> hints = CollectModuleHints(layer, regions);
  t = stageMs("regions_hints", t);

  std::vector<GridCandidate> model = SourceModelCandidates(
      binary, layer, SearchQRModel(layer, regions, hints));
  t = stageMs("model", t);
  std::vector<GridCandidate> modelRefined;
  if (!model.empty() && model[0].score >= 0.80f) {
    modelRefined = RefineTop(binary, model, true);
    const float refinedScore =
        modelRefined.empty() ? -1.0f : modelRefined[0].score;
    if (largeWorkingImage &&
        (refinedScore >= 0.94f ||
         (model[0].score >= 0.92f && DistinctScoreMargin(model) >= 0.04f &&
          refinedScore >= 0.90f))) {
      stageMs("refine", t);
      PrintCandidateStats("refined", modelRefined);
      return modelRefined;
    }
  }

  // Modest images (and large images whose early axis/model misses) keep the
  // full axis path and optional paper path as a safety net.
  if (!largeWorkingImage) {
    axis = CollectAxisCandidates(
        binary, FindAxisSources(binary, layer, regions));
  } else if (axis.empty() || axis[0].score < 0.88f) {
    const bool weakModel =
        modelRefined.empty() || modelRefined[0].score < 0.90f;
    if (weakModel) {
      MergeCoarseCandidates(
          axis, CollectAxisCandidates(
                    binary, FindAxisSources(binary, layer, regions)));
    }
  }
  t = stageMs("axis2", t);
  PointF paper[4];
  const bool usePaper =
      border.darkCanvas && FindPaperCorners(binary, border, paper);
  std::vector<GridCandidate> paperCandidates;
  if (usePaper) {
    paperCandidates = CollectPaperCandidates(binary, paper);
  }
  t = stageMs("paper", t);
  const std::vector<GridCandidate> coarse =
      FairCoarseCandidates(axis, paperCandidates, model);
  std::vector<GridCandidate> refined = RefineTop(binary, coarse, true);
  for (size_t i = 0; i < modelRefined.size(); ++i) {
    KeepLimited(refined, modelRefined[i], 8);
  }
  stageMs("refine", t);
  PrintCandidateStats("refined", refined);
  return refined;
}

bool RenderableCandidate(
    const BinaryImage& image, const GridCandidate& candidate) {
  const float radius = std::max(0.6f, GridModuleSize(candidate) * 0.30f);
  return candidate.version >= 1 && candidate.version <= 10 &&
      std::isfinite(candidate.score) && candidate.score >= 0.68f &&
      FootprintCovered(image, candidate, radius);
}

std::vector<NormalizedImage> RenderCandidates(
    const BinaryImage& binary,
    const std::vector<GridCandidate>& refined, int maximumCandidates) {
  std::vector<NormalizedImage> result;
  if (maximumCandidates <= 0 || !HasAbsoluteEvidence(refined)) {
    return result;
  }
  const int limit = std::min(3, maximumCandidates);
  std::vector<GridCandidate> rendered;
  for (size_t i = 0;
       i < refined.size() && static_cast<int>(result.size()) < limit; ++i) {
    if (!RenderableCandidate(binary, refined[i])) {
      continue;
    }
    bool equivalent = false;
    for (size_t j = 0; j < rendered.size(); ++j) {
      equivalent = equivalent || EquivalentGrid(rendered[j], refined[i]);
    }
    if (equivalent) {
      continue;
    }
    const std::vector<FixedModule> fixed =
        BuildFixedModules(refined[i].version);
    int outputSize = 0;
    zxing::ArrayRef<char> pixels =
        RenderNormalized(binary, refined[i], fixed, outputSize);
    if (pixels && outputSize > 0) {
      result.push_back(NormalizedImage{
          pixels, outputSize, outputSize, 4});
      rendered.push_back(refined[i]);
    }
  }
  return result;
}

}

std::vector<NormalizedImage> NormalizeQR(
    const MutableImage& image, int maximumCandidates) {
  if (maximumCandidates <= 0) {
    return std::vector<NormalizedImage>();
  }
  // Match the former CanNormalizeQR(..., true) gate: caller retains the input.
  size_t pixelCount = 0;
  if (!CheckedImageSize(image, true, pixelCount)) {
    return std::vector<NormalizedImage>();
  }
  const SteadyClock::time_point t0 = SteadyClock::now();
  BinaryImage binary = BuildWorkingBinary(image);
  if (std::getenv("ZXING_QR_STAGE_MS") != NULL) {
    std::fprintf(
        stderr, "qr-stage binary=%.1fms\n",
        std::chrono::duration<double, std::milli>(
            SteadyClock::now() - t0).count());
  }
  if (binary.gray.empty()) {
    return std::vector<NormalizedImage>();
  }
  return RenderCandidates(
      binary, FindRefinedCandidates(binary),
      std::min(3, maximumCandidates));
}

}
}
