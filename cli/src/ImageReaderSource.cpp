/*
 *  Copyright 2010-2011 ZXing authors
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

#include "ImageReaderSource.h"
#include <zxing/common/IllegalArgumentException.h>
#include <zxing/qrcode/Version.h>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include "lodepng.h"
#include "jpgd.h"

using std::string;
using std::ostringstream;
using zxing::Ref;
using zxing::ArrayRef;
using zxing::LuminanceSource;

namespace {

struct Component {
  int left;
  int top;
  int right;
  int bottom;
  int area;
};

struct QRGrid {
  int version;
  int dimension;
  int moduleSize;
  int left;
  int top;
  float score;
};

inline unsigned char luminanceAt(zxing::ArrayRef<char> const& image, int width, int comps, int x, int y) {
  unsigned char const* pixel = reinterpret_cast<unsigned char const*>(&image[(y * width + x) * comps]);
  if (comps == 1 || comps == 2) {
    return pixel[0];
  }
  return (unsigned char)((306 * (int)pixel[0] + 601 * (int)pixel[1] +
      117 * (int)pixel[2] + 0x200) >> 10);
}

inline void setPixel(zxing::ArrayRef<char>& image, int width, int comps, int x, int y, unsigned char value) {
  unsigned char* pixel = reinterpret_cast<unsigned char*>(&image[(y * width + x) * comps]);
  for (int c = 0; c < comps && c < 3; c++) {
    pixel[c] = value;
  }
  if (comps == 2) {
    pixel[1] = 255;
  } else if (comps == 4) {
    pixel[3] = 255;
  }
}

float blackFraction(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                    int left, int top, int regionWidth, int regionHeight) {
  if (left < 0 || top < 0 || left + regionWidth > width || top + regionHeight > height ||
      regionWidth <= 0 || regionHeight <= 0) {
    return 0.0f;
  }

  int black = 0;
  int total = regionWidth * regionHeight;
  for (int y = top; y < top + regionHeight; y++) {
    for (int x = left; x < left + regionWidth; x++) {
      if (luminanceAt(image, width, comps, x, y) < 80) {
        black++;
      }
    }
  }
  return (float)black / (float)total;
}

void drawFinderPattern(zxing::ArrayRef<char>& image, int width, int height, int comps,
                       int left, int top, int moduleSize) {
  int size = moduleSize * 7;
  if (left < 0 || top < 0 || left + size > width || top + size > height) {
    return;
  }

  for (int y = top; y < top + size; y++) {
    for (int x = left; x < left + size; x++) {
      setPixel(image, width, comps, x, y, 0);
    }
  }
  for (int y = top + moduleSize; y < top + moduleSize * 6; y++) {
    for (int x = left + moduleSize; x < left + moduleSize * 6; x++) {
      setPixel(image, width, comps, x, y, 255);
    }
  }
  for (int y = top + moduleSize * 2; y < top + moduleSize * 5; y++) {
    for (int x = left + moduleSize * 2; x < left + moduleSize * 5; x++) {
      setPixel(image, width, comps, x, y, 0);
    }
  }
}

void drawModule(zxing::ArrayRef<char>& image, int width, int height, int comps,
                int gridLeft, int gridTop, int moduleSize, int moduleX, int moduleY,
                unsigned char value) {
  int left = gridLeft + moduleX * moduleSize;
  int top = gridTop + moduleY * moduleSize;
  if (left < 0 || top < 0 || left + moduleSize > width || top + moduleSize > height) {
    return;
  }

  for (int y = top; y < top + moduleSize; y++) {
    for (int x = left; x < left + moduleSize; x++) {
      setPixel(image, width, comps, x, y, value);
    }
  }
}

bool finderModule(int localX, int localY, bool& isBlack) {
  if (localX >= 0 && localX < 7 && localY >= 0 && localY < 7) {
    isBlack = localX == 0 || localX == 6 || localY == 0 || localY == 6 ||
        (localX >= 2 && localX <= 4 && localY >= 2 && localY <= 4);
    return true;
  }
  return false;
}

bool alignmentModule(int localX, int localY, bool& isBlack) {
  if (localX >= -2 && localX <= 2 && localY >= -2 && localY <= 2) {
    int x = localX + 2;
    int y = localY + 2;
    isBlack = x == 0 || x == 4 || y == 0 || y == 4 || (x == 2 && y == 2);
    return true;
  }
  return false;
}

bool fixedModuleForVersion(int version, int moduleX, int moduleY, bool& isBlack) {
  int dimension = 17 + 4 * version;
  const int finderLeft[3] = {0, dimension - 7, 0};
  const int finderTop[3] = {0, 0, dimension - 7};
  for (int i = 0; i < 3; i++) {
    if (finderModule(moduleX - finderLeft[i], moduleY - finderTop[i], isBlack)) {
      return true;
    }
  }

  if (moduleY == 6 && moduleX >= 8 && moduleX <= dimension - 9) {
    isBlack = (moduleX & 1) == 0;
    return true;
  }
  if (moduleX == 6 && moduleY >= 8 && moduleY <= dimension - 9) {
    isBlack = (moduleY & 1) == 0;
    return true;
  }

  if (moduleX == 8 && moduleY == dimension - 8) {
    isBlack = true;
    return true;
  }

  if (version > 1) {
    zxing::qrcode::Version *qrVersion = zxing::qrcode::Version::getVersionForNumber(version);
    std::vector<int> &centers = qrVersion->getAlignmentPatternCenters();
    for (size_t y = 0; y < centers.size(); y++) {
      for (size_t x = 0; x < centers.size(); x++) {
        bool nearTopLeft = x == 0 && y == 0;
        bool nearTopRight = x == centers.size() - 1 && y == 0;
        bool nearBottomLeft = x == 0 && y == centers.size() - 1;
        if (nearTopLeft || nearTopRight || nearBottomLeft) {
          continue;
        }
        if (alignmentModule(moduleX - centers[x], moduleY - centers[y], isBlack)) {
          return true;
        }
      }
    }
  }

  return false;
}

int estimateModuleSize(zxing::ArrayRef<char> const& image, int width, int height, int comps) {
  int maxLength = std::max(width, height);
  std::vector<int> histogram(maxLength + 1, 0);
  // Use a small floor so that single-module runs of dense (high-version) QR
  // codes are not filtered out. The previous floor of std::min(w,h)/80 ruled
  // out runs shorter than ~13 px on 1080-pixel images, while the actual
  // module width for QR v35-v40 is only ~6 px. As a result the histogram
  // peak landed on 2- or 3-module clusters and the version filter rejected
  // the true high versions.
  int minRun = std::max(2, std::min(width, height) / 400);
  int maxRun = std::max(minRun, std::min(width, height) / 3);

  for (int y = 5; y < height - 5; y++) {
    for (int x = 0; x < width; ) {
      while (x < width && luminanceAt(image, width, comps, x, y) >= 80) {
        x++;
      }
      int start = x;
      while (x < width && luminanceAt(image, width, comps, x, y) < 80) {
        x++;
      }
      int run = x - start;
      if (run >= minRun && run <= maxRun) {
        histogram[run]++;
      }
    }
  }

  for (int x = 5; x < width - 5; x++) {
    for (int y = 0; y < height; ) {
      while (y < height && luminanceAt(image, width, comps, x, y) >= 80) {
        y++;
      }
      int start = y;
      while (y < height && luminanceAt(image, width, comps, x, y) < 80) {
        y++;
      }
      int run = y - start;
      if (run >= minRun && run <= maxRun) {
        histogram[run]++;
      }
    }
  }

  int bestRun = 0;
  int bestCount = 0;
  for (size_t i = 0; i < histogram.size(); i++) {
    if ((int)histogram[i] > bestCount) {
      bestRun = (int)i;
      bestCount = (int)histogram[i];
    }
  }
  if (bestCount == 0) {
    return 0;
  }

  // Alignment patterns and adjacent same-color modules bias the peak toward
  // small integer multiples of the true module size. Walk up from the
  // smallest captured run and return the first bin that is itself a strong
  // peak (>= 1/3 of the absolute peak and not just the leading edge of a
  // smooth ramp). This recovers the fundamental module size on dense codes
  // without affecting low-version codes whose modules already sit at the
  // dominant peak.
  int significantThreshold = std::max(2, bestCount / 3);
  for (int i = minRun; i <= bestRun; i++) {
    if (histogram[i] >= significantThreshold &&
        (i == minRun || histogram[i] >= histogram[i - 1])) {
      return i;
    }
  }
  return bestRun;
}

int estimateContentDimension(zxing::ArrayRef<char> const& image, int width, int height, int comps, int moduleSize) {
  int minX = width;
  int minY = height;
  int maxX = -1;
  int maxY = -1;
  int border = 5;
  for (int y = border; y < height - border; y++) {
    for (int x = border; x < width - border; x++) {
      if (luminanceAt(image, width, comps, x, y) < 80) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
  }
  if (maxX < minX || maxY < minY || moduleSize <= 0) {
    return 0;
  }

  int contentSpan = std::max(maxX - minX + 1, maxY - minY + 1);
  return (contentSpan + moduleSize / 2) / moduleSize;
}

bool findContentBounds(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                       int& left, int& top, int& size) {
  int minX = width;
  int minY = height;
  int maxX = -1;
  int maxY = -1;
  int border = 5;
  for (int y = border; y < height - border; y++) {
    for (int x = border; x < width - border; x++) {
      if (luminanceAt(image, width, comps, x, y) < 80) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
  }
  if (maxX < minX || maxY < minY) {
    return false;
  }

  left = minX;
  top = minY;
  size = std::max(maxX - minX + 1, maxY - minY + 1);
  return true;
}

float scoreQRGrid(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                  int version, int gridLeft, int gridTop, int moduleSize) {
  int matches = 0;
  int total = 0;
  int dimension = 17 + 4 * version;
  for (int moduleY = 0; moduleY < dimension; moduleY++) {
    for (int moduleX = 0; moduleX < dimension; moduleX++) {
      bool expectedBlack = false;
      if (!fixedModuleForVersion(version, moduleX, moduleY, expectedBlack)) {
        continue;
      }

      int sampleX = gridLeft + moduleX * moduleSize + moduleSize / 2;
      int sampleY = gridTop + moduleY * moduleSize + moduleSize / 2;
      if (sampleX < 0 || sampleY < 0 || sampleX >= width || sampleY >= height) {
        continue;
      }

      bool actualBlack = luminanceAt(image, width, comps, sampleX, sampleY) < 80;
      if (actualBlack == expectedBlack) {
        matches++;
      }
      total++;
    }
  }
  return total == 0 ? 0.0f : (float)matches / (float)total;
}

float scoreFloatQRGrid(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                       int version, int gridLeft, int gridTop, int gridSize) {
  int dimension = 17 + 4 * version;
  float moduleSize = (float)gridSize / (float)dimension;
  if (moduleSize < 2.0f) {
    return 0.0f;
  }

  int matches = 0;
  int total = 0;
  for (int moduleY = 0; moduleY < dimension; moduleY++) {
    for (int moduleX = 0; moduleX < dimension; moduleX++) {
      bool expectedBlack = false;
      if (!fixedModuleForVersion(version, moduleX, moduleY, expectedBlack)) {
        continue;
      }

      int sampleX = gridLeft + (int)((moduleX + 0.5f) * moduleSize + 0.5f);
      int sampleY = gridTop + (int)((moduleY + 0.5f) * moduleSize + 0.5f);
      if (sampleX < 0 || sampleY < 0 || sampleX >= width || sampleY >= height) {
        continue;
      }

      bool actualBlack = luminanceAt(image, width, comps, sampleX, sampleY) < 80;
      if (actualBlack == expectedBlack) {
        matches++;
      }
      total++;
    }
  }
  return total == 0 ? 0.0f : (float)matches / (float)total;
}

struct FixedModuleEntry {
  int x;
  int y;
  bool isBlack;
};

void buildFixedModules(int version, std::vector<FixedModuleEntry>& out) {
  int dimension = 17 + 4 * version;
  out.clear();
  out.reserve(256);
  for (int moduleY = 0; moduleY < dimension; moduleY++) {
    for (int moduleX = 0; moduleX < dimension; moduleX++) {
      bool isBlack = false;
      if (fixedModuleForVersion(version, moduleX, moduleY, isBlack)) {
        FixedModuleEntry entry = {moduleX, moduleY, isBlack};
        out.push_back(entry);
      }
    }
  }
}

float scoreCachedQRGrid(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                        std::vector<FixedModuleEntry> const& fixedModules,
                        int gridLeft, int gridTop, float moduleSize) {
  int matches = 0;
  int total = 0;
  for (size_t i = 0; i < fixedModules.size(); i++) {
    FixedModuleEntry const& entry = fixedModules[i];
    int sampleX = gridLeft + (int)((entry.x + 0.5f) * moduleSize + 0.5f);
    int sampleY = gridTop + (int)((entry.y + 0.5f) * moduleSize + 0.5f);
    if (sampleX < 0 || sampleY < 0 || sampleX >= width || sampleY >= height) {
      continue;
    }
    bool actualBlack = luminanceAt(image, width, comps, sampleX, sampleY) < 80;
    if (actualBlack == entry.isBlack) {
      matches++;
    }
    total++;
  }
  return total == 0 ? 0.0f : (float)matches / (float)total;
}

bool normalizeQRToModuleImage(zxing::ArrayRef<char>& image, int& width, int& height, int& comps) {
  int bboxLeft = 0;
  int bboxTop = 0;
  int bboxSize = 0;
  if (!findContentBounds(image, width, height, comps, bboxLeft, bboxTop, bboxSize)) {
    return false;
  }

  int estimatedModuleSize = estimateModuleSize(image, width, height, comps);
  if (estimatedModuleSize < 2) {
    return false;
  }

  int bestVersion = 0;
  int bestLeft = bboxLeft;
  int bestTop = bboxTop;
  float bestModuleSize = 0.0f;
  float bestScore = 0.0f;

  std::vector<FixedModuleEntry> fixedModules;
  for (int version = 1; version <= 40; version++) {
    int dimension = 17 + 4 * version;
    float baseModuleSize = (float)bboxSize / (float)dimension;
    if (baseModuleSize < 2.0f) {
      continue;
    }

    // Histogram-based version filter. With the improved estimateModuleSize
    // returning the smallest significant peak, the value is close to the
    // true module width even on dense high-version codes. Accept the
    // version when its bbox-implied module size is within ~50% of the
    // estimate, plus a small additive slack to cover sub-pixel rendering.
    float moduleTolerance = std::max(2.0f, (float)estimatedModuleSize * 0.5f);
    if (std::abs(baseModuleSize - (float)estimatedModuleSize) > moduleTolerance) {
      continue;
    }

    buildFixedModules(version, fixedModules);
    // Multi-anchor prescore. Sampling a single point at the bbox origin
    // is unreliable when finder patterns are damaged (bbox can shift by a
    // few modules). Probe a small grid of offsets around the bbox corner
    // and use the best score as the gate; this preserves performance for
    // the typical case while not killing recovery on partially damaged
    // codes whose true grid origin is offset from the bbox.
    float anchorPrescore = 0.0f;
    int anchorStep = std::max(1, (int)(baseModuleSize + 0.5f));
    for (int dy = -2; dy <= 2 && anchorPrescore < 0.85f; dy++) {
      for (int dx = -2; dx <= 2 && anchorPrescore < 0.85f; dx++) {
        int ax = bboxLeft + dx * anchorStep;
        int ay = bboxTop + dy * anchorStep;
        float s = scoreCachedQRGrid(image, width, height, comps, fixedModules,
            ax, ay, baseModuleSize);
        if (s > anchorPrescore) {
          anchorPrescore = s;
        }
      }
    }
    if (anchorPrescore < 0.45f && anchorPrescore < bestScore - 0.05f) {
      continue;
    }

    float candidateModuleSizes[3] = {baseModuleSize, baseModuleSize + 0.5f, baseModuleSize - 0.5f};
    for (int i = 0; i < 3; i++) {
      float moduleSize = candidateModuleSizes[i];
      if (moduleSize < 2.0f) {
        continue;
      }

      int qrSize = (int)(moduleSize * dimension + 0.5f);
      if (qrSize > std::min(width, height)) {
        continue;
      }

      int searchRadius = std::max(2, (int)(3.0f * moduleSize));
      int step = std::max(1, (int)(moduleSize / 3.0f));
      int leftStart = std::max(0, bboxLeft - searchRadius);
      int leftEnd = std::min(width - qrSize, bboxLeft + searchRadius);
      int topStart = std::max(0, bboxTop - searchRadius);
      int topEnd = std::min(height - qrSize, bboxTop + searchRadius);

      for (int top = topStart; top <= topEnd; top += step) {
        for (int left = leftStart; left <= leftEnd; left += step) {
          float score = scoreCachedQRGrid(image, width, height, comps, fixedModules, left, top, moduleSize);
          if (score > bestScore) {
            bestScore = score;
            bestVersion = version;
            bestLeft = left;
            bestTop = top;
            bestModuleSize = moduleSize;
          }
        }
      }
    }
  }

  if (bestVersion == 0 || bestScore < 0.72f) {
    return false;
  }

  int dimension = 17 + 4 * bestVersion;
  float moduleSize = bestModuleSize;
  int gridLeft = bestLeft;
  int gridTop = bestTop;
  const int scale = 8;
  const int quietZone = 4;
  int normalizedWidth = (dimension + quietZone * 2) * scale;
  int normalizedHeight = normalizedWidth;
  zxing::ArrayRef<char> normalized(4 * normalizedWidth * normalizedHeight);
  for (int y = 0; y < normalizedHeight; y++) {
    for (int x = 0; x < normalizedWidth; x++) {
      setPixel(normalized, normalizedWidth, 4, x, y, 255);
    }
  }

  for (int moduleY = 0; moduleY < dimension; moduleY++) {
    for (int moduleX = 0; moduleX < dimension; moduleX++) {
      bool fixedBlack = false;
      bool hasFixedValue = fixedModuleForVersion(bestVersion, moduleX, moduleY, fixedBlack);
      int sampleX = gridLeft + (int)((moduleX + 0.5f) * moduleSize + 0.5f);
      int sampleY = gridTop + (int)((moduleY + 0.5f) * moduleSize + 0.5f);
      bool isBlack = hasFixedValue ? fixedBlack :
          (sampleX >= 0 && sampleY >= 0 && sampleX < width && sampleY < height &&
           luminanceAt(image, width, comps, sampleX, sampleY) < 80);
      if (!isBlack) {
        continue;
      }

      int outLeft = (moduleX + quietZone) * scale;
      int outTop = (moduleY + quietZone) * scale;
      for (int y = outTop; y < outTop + scale; y++) {
        for (int x = outLeft; x < outLeft + scale; x++) {
          setPixel(normalized, normalizedWidth, 4, x, y, 0);
        }
      }
    }
  }

  image = normalized;
  width = normalizedWidth;
  height = normalizedHeight;
  comps = 4;
  return true;
}

void refineQRGrid(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                  int version, int moduleSize, int centerLeft, int centerTop, int radius,
                  QRGrid& best) {
  int dimension = 17 + 4 * version;
  int qrSize = dimension * moduleSize;
  if (qrSize > width || qrSize > height) {
    return;
  }

  int minLeft = std::max(0, centerLeft - radius);
  int maxLeft = std::min(width - qrSize, centerLeft + radius);
  int minTop = std::max(0, centerTop - radius);
  int maxTop = std::min(height - qrSize, centerTop + radius);
  for (int top = minTop; top <= maxTop; top++) {
    for (int left = minLeft; left <= maxLeft; left++) {
      float score = scoreQRGrid(image, width, height, comps, version, left, top, moduleSize);
      if (score > best.score ||
          (std::abs(score - best.score) < 0.0001f &&
           (best.dimension == 0 || dimension < best.dimension ||
            (dimension == best.dimension && moduleSize > best.moduleSize)))) {
        best.version = version;
        best.dimension = dimension;
        best.moduleSize = moduleSize;
        best.left = left;
        best.top = top;
        best.score = score;
      }
    }
  }
}

QRGrid findBestQRGrid(zxing::ArrayRef<char> const& image, int width, int height, int comps) {
  QRGrid best = {0, 0, 0, 0, 0, 0.0f};
  int estimatedModuleSize = estimateModuleSize(image, width, height, comps);
  if (estimatedModuleSize < 3) {
    return best;
  }

  for (int moduleSize = std::max(3, estimatedModuleSize - 2);
       moduleSize <= estimatedModuleSize + 2; moduleSize++) {
    int contentDimension = estimateContentDimension(image, width, height, comps, moduleSize);
    for (int version = 1; version <= 40; version++) {
      int dimension = 17 + 4 * version;
      if (contentDimension > 0 && std::abs(dimension - contentDimension) > 3) {
        continue;
      }
      int qrSize = dimension * moduleSize;
      if (qrSize > width || qrSize > height) {
        continue;
      }
      if (width - qrSize > moduleSize * 24 || height - qrSize > moduleSize * 24) {
        continue;
      }

      int step = std::max(1, moduleSize / 3);
      QRGrid coarse = {0, 0, 0, 0, 0, 0.0f};
      for (int top = 0; top <= height - qrSize; top += step) {
        for (int left = 0; left <= width - qrSize; left += step) {
          float score = scoreQRGrid(image, width, height, comps, version, left, top, moduleSize);
          if (score > coarse.score) {
            coarse.version = version;
            coarse.dimension = dimension;
            coarse.moduleSize = moduleSize;
            coarse.left = left;
            coarse.top = top;
            coarse.score = score;
          }
        }
      }
      refineQRGrid(image, width, height, comps, version, moduleSize, coarse.left, coarse.top, step, best);
    }
  }
  return best;
}

bool repairQRFixedPatterns(zxing::ArrayRef<char>& image, int width, int height, int comps) {
  if (std::min(width, height) < 600 || std::min(width, height) > 1200) {
    return false;
  }

  QRGrid grid = findBestQRGrid(image, width, height, comps);
  if (grid.score < 0.55f) {
    return false;
  }
  if (grid.version != 1) {
    return false;
  }

  drawFinderPattern(image, width, height, comps,
      grid.left, grid.top, grid.moduleSize);
  drawFinderPattern(image, width, height, comps,
      grid.left + (grid.dimension - 7) * grid.moduleSize, grid.top, grid.moduleSize);
  drawFinderPattern(image, width, height, comps,
      grid.left, grid.top + (grid.dimension - 7) * grid.moduleSize, grid.moduleSize);

  for (int i = 8; i <= grid.dimension - 9; i++) {
    unsigned char value = (i & 1) == 0 ? 0 : 255;
    drawModule(image, width, height, comps, grid.left, grid.top, grid.moduleSize, i, 6, value);
    drawModule(image, width, height, comps, grid.left, grid.top, grid.moduleSize, 6, i, value);
  }

  drawModule(image, width, height, comps, grid.left, grid.top, grid.moduleSize, 8, grid.dimension - 8, 0);

  if (grid.version > 1) {
    zxing::qrcode::Version *qrVersion = zxing::qrcode::Version::getVersionForNumber(grid.version);
    std::vector<int> &centers = qrVersion->getAlignmentPatternCenters();
    for (size_t y = 0; y < centers.size(); y++) {
      for (size_t x = 0; x < centers.size(); x++) {
        bool nearTopLeft = x == 0 && y == 0;
        bool nearTopRight = x == centers.size() - 1 && y == 0;
        bool nearBottomLeft = x == 0 && y == centers.size() - 1;
        if (nearTopLeft || nearTopRight || nearBottomLeft) {
          continue;
        }
        int centerX = centers[x];
        int centerY = centers[y];
        for (int dy = -2; dy <= 2; dy++) {
          for (int dx = -2; dx <= 2; dx++) {
            bool isBlack = false;
            alignmentModule(dx, dy, isBlack);
            drawModule(image, width, height, comps, grid.left, grid.top, grid.moduleSize,
                centerX + dx, centerY + dy, isBlack ? 0 : 255);
          }
        }
      }
    }
  }

  return true;
}

float scoreFinderCandidate(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                           int left, int top, int moduleSize) {
  if (left < 0 || top < 0 || left + 7 * moduleSize > width || top + 7 * moduleSize > height) {
    return 0.0f;
  }

  int matches = 0;
  int total = 0;
  for (int moduleY = 0; moduleY < 7; moduleY++) {
    for (int moduleX = 0; moduleX < 7; moduleX++) {
      bool expectedBlack = false;
      finderModule(moduleX, moduleY, expectedBlack);
      int sampleX = left + moduleX * moduleSize + moduleSize / 2;
      int sampleY = top + moduleY * moduleSize + moduleSize / 2;
      bool actualBlack = luminanceAt(image, width, comps, sampleX, sampleY) < 80;
      if (actualBlack == expectedBlack) {
        matches++;
      }
      total++;
    }
  }
  return (float)matches / (float)total;
}

bool tryRepairFinderCandidate(zxing::ArrayRef<char>& image, int width, int height, int comps,
                              int left, int top, int moduleSize) {
  if (scoreFinderCandidate(image, width, height, comps, left, top, moduleSize) > 0.62f) {
    drawFinderPattern(image, width, height, comps, left, top, moduleSize);
    return true;
  }
  return false;
}

bool isLeftDamagedFinder(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                         Component const& component, int& moduleSize) {
  int w = component.right - component.left + 1;
  int h = component.bottom - component.top + 1;
  if (w < 30 || h < 40 || h > height / 2) {
    return false;
  }

  moduleSize = h / 7;
  if (moduleSize < 3) {
    return false;
  }

  int tolerance = std::max(3, moduleSize / 2);
  if (std::abs(h - 7 * moduleSize) > tolerance || std::abs(w - 6 * moduleSize) > tolerance) {
    return false;
  }
  if (component.left - moduleSize < 0 || component.top + 7 * moduleSize > height ||
      component.left + 6 * moduleSize > width) {
    return false;
  }

  return blackFraction(image, width, height, comps,
             component.left, component.top, 6 * moduleSize, moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left + 5 * moduleSize, component.top, moduleSize, 7 * moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left, component.top + 6 * moduleSize, 6 * moduleSize, moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left + moduleSize, component.top + 2 * moduleSize, 3 * moduleSize, 3 * moduleSize) > 0.65f;
}

bool isRightDamagedFinder(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                          Component const& component, int& moduleSize) {
  int w = component.right - component.left + 1;
  int h = component.bottom - component.top + 1;
  if (w < 30 || h < 40 || h > height / 2) {
    return false;
  }

  moduleSize = h / 7;
  if (moduleSize < 3) {
    return false;
  }

  int tolerance = std::max(3, moduleSize / 2);
  if (std::abs(h - 7 * moduleSize) > tolerance || w < 4 * moduleSize || w > 7 * moduleSize) {
    return false;
  }
  if (component.left + 7 * moduleSize > width || component.top + 7 * moduleSize > height) {
    return false;
  }

  return blackFraction(image, width, height, comps,
             component.left, component.top, std::min(w, 6 * moduleSize), moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left, component.top, moduleSize, 7 * moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left, component.top + 6 * moduleSize, std::min(w, 6 * moduleSize), moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left + 2 * moduleSize, component.top + 2 * moduleSize, 3 * moduleSize, 3 * moduleSize) > 0.65f;
}

bool repairDamagedFinderPatterns(zxing::ArrayRef<char>& image, int width, int height, int comps) {
  std::vector<unsigned char> visited(width * height, 0);
  std::vector<int> stack;
  bool repairedSpecificFinder = false;
  bool repairedAnyFinder = false;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int start = y * width + x;
      if (visited[start] || luminanceAt(image, width, comps, x, y) >= 80) {
        continue;
      }

      Component component = {x, y, x, y, 0};
      visited[start] = 1;
      stack.clear();
      stack.push_back(start);

      while (!stack.empty()) {
        int p = stack.back();
        stack.pop_back();
        int px = p % width;
        int py = p / width;
        component.area++;
        component.left = std::min(component.left, px);
        component.top = std::min(component.top, py);
        component.right = std::max(component.right, px);
        component.bottom = std::max(component.bottom, py);

        const int dx[4] = {-1, 1, 0, 0};
        const int dy[4] = {0, 0, -1, 1};
        for (int i = 0; i < 4; i++) {
          int nx = px + dx[i];
          int ny = py + dy[i];
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
            continue;
          }
          int n = ny * width + nx;
          if (!visited[n] && luminanceAt(image, width, comps, nx, ny) < 80) {
            visited[n] = 1;
            stack.push_back(n);
          }
        }
      }

      int componentWidth = component.right - component.left + 1;
      int componentHeight = component.bottom - component.top + 1;
      int minFinderComponentSize = std::max(30, std::min(width, height) / 14);
      if (componentWidth < minFinderComponentSize || componentHeight < minFinderComponentSize ||
          componentWidth > width / 2 || componentHeight > height / 2) {
        continue;
      }
      float aspect = (float)componentWidth / (float)componentHeight;
      if (aspect < 0.70f || aspect > 1.45f) {
        continue;
      }

      int leftDamagedModuleSize = 0;
      if (isLeftDamagedFinder(image, width, height, comps, component, leftDamagedModuleSize)) {
        drawFinderPattern(image, width, height, comps,
            component.left - leftDamagedModuleSize, component.top, leftDamagedModuleSize);
        repairedSpecificFinder = true;
        repairedAnyFinder = true;
        continue;
      }
      int rightDamagedModuleSize = 0;
      if (isRightDamagedFinder(image, width, height, comps, component, rightDamagedModuleSize)) {
        float rightScore = scoreFinderCandidate(image, width, height, comps,
            component.left, component.top, rightDamagedModuleSize);
        float leftScore = scoreFinderCandidate(image, width, height, comps,
            component.left - rightDamagedModuleSize, component.top, rightDamagedModuleSize);
        if (rightScore >= leftScore) {
          drawFinderPattern(image, width, height, comps,
              component.left, component.top, rightDamagedModuleSize);
          repairedSpecificFinder = true;
          repairedAnyFinder = true;
          continue;
        }
      }
      if (repairedSpecificFinder) {
        continue;
      }
      if (std::min(width, height) < 600) {
        continue;
      }

      int estimatedModuleSize = (std::max(componentWidth, componentHeight) + 3) / 7;
      for (int moduleSize = std::max(3, estimatedModuleSize - 2);
           moduleSize <= estimatedModuleSize + 2; moduleSize++) {
        int finderSize = 7 * moduleSize;
        if (finderSize < componentWidth || finderSize < componentHeight) {
          continue;
        }
        int missingWidth = finderSize - componentWidth;
        int missingHeight = finderSize - componentHeight;
        if (missingWidth > 2 * moduleSize || missingHeight > 2 * moduleSize) {
          continue;
        }

        int candidateLefts[3] = {
          component.left,
          component.right - finderSize + 1,
          component.left - missingWidth / 2
        };
        int candidateTops[3] = {
          component.top,
          component.bottom - finderSize + 1,
          component.top - missingHeight / 2
        };

        for (int y = 0; y < 3; y++) {
          for (int x = 0; x < 3; x++) {
            if (tryRepairFinderCandidate(image, width, height, comps,
                candidateLefts[x], candidateTops[y], moduleSize)) {
              repairedAnyFinder = true;
            }
          }
        }
      }
    }
  }
  return repairedAnyFinder;
}

}

inline char ImageReaderSource::convertPixel(char const* pixel_) const {
  unsigned char const* pixel = (unsigned char const*)pixel_;
  if (comps == 1 || comps == 2) {
    // Gray or gray+alpha
    return pixel[0];
  } if (comps == 3 || comps == 4) {
    // Red, Green, Blue, (Alpha)
    // We assume 16 bit values here
    // 0x200 = 1<<9, half an lsb of the result to force rounding
    return (char)((306 * (int)pixel[0] + 601 * (int)pixel[1] +
        117 * (int)pixel[2] + 0x200) >> 10);
  } else {
    throw zxing::IllegalArgumentException("Unexpected image depth");
  }
}

ImageReaderSource::ImageReaderSource(ArrayRef<char> image_, int width, int height, int comps_)
    : Super(width, height), image(image_), comps(comps_) {}

Ref<LuminanceSource> ImageReaderSource::create(string const& filename) {
  return create(filename, false);
}

Ref<LuminanceSource> ImageReaderSource::create(string const& filename, bool repairFixedPatterns) {
  string extension = filename.substr(filename.find_last_of(".") + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
  int width, height;
  int comps = 0;
  zxing::ArrayRef<char> image;
  if (extension == "png") {
    std::vector<unsigned char> out;

    { unsigned w, h;
      unsigned error = lodepng::decode(out, w, h, filename);
      if (error) {
        ostringstream msg;
        msg << "Error while loading '" << lodepng_error_text(error) << "'";
        throw zxing::IllegalArgumentException(msg.str().c_str());
      }
      width = w;
      height = h;
    }

    comps = 4;
    image = zxing::ArrayRef<char>(4 * width * height);
    memcpy(&image[0], &out[0], image->size());
  } else if (extension == "jpg" || extension == "jpeg") {
    char *buffer = reinterpret_cast<char*>(jpgd::decompress_jpeg_image_from_file(
        filename.c_str(), &width, &height, &comps, 4));
    image = zxing::ArrayRef<char>(buffer, 4 * width * height);
    free(buffer);
  }
  if (!image) {
    ostringstream msg;
    msg << "Loading \"" << filename << "\" failed.";
    throw zxing::IllegalArgumentException(msg.str().c_str());
  }

  if (repairFixedPatterns) {
    // Try the version-aware grid normalization first; it is now safe enough to
    // run unconditionally because it requires a high fixed-pattern score and a
    // module size consistent with the dominant black run length.
    if (!normalizeQRToModuleImage(image, width, height, comps)) {
      // Fall back: rebuild specific finder patterns when the normalization is not
      // confident enough.
      if (!repairQRFixedPatterns(image, width, height, comps)) {
        repairDamagedFinderPatterns(image, width, height, comps);
      } else {
        repairDamagedFinderPatterns(image, width, height, comps);
      }
    }
  }

  return Ref<LuminanceSource>(new ImageReaderSource(image, width, height, comps));
}

zxing::ArrayRef<char> ImageReaderSource::getRow(int y, zxing::ArrayRef<char> row) const {
  const char* pixelRow = &image[0] + y * getWidth() * 4;
  if (!row) {
    row = zxing::ArrayRef<char>(getWidth());
  }
  for (int x = 0; x < getWidth(); x++) {
    row[x] = convertPixel(pixelRow + (x * 4));
  }
  return row;
}

/** This is a more efficient implementation. */
zxing::ArrayRef<char> ImageReaderSource::getMatrix() const {
  const char* p = &image[0];
  zxing::ArrayRef<char> matrix(getWidth() * getHeight());
  char* m = &matrix[0];
  for (int y = 0; y < getHeight(); y++) {
    for (int x = 0; x < getWidth(); x++) {
      *m = convertPixel(p);
      m++;
      p += 4;
    }
  }
  return matrix;
}
