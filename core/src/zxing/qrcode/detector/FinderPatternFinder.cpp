// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
/*
 *  FinderPatternFinder.cpp
 *  zxing
 *
 *  Created by Christian Brunschen on 13/05/2008.
 *  Copyright 2008 ZXing authors All rights reserved.
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

#include <algorithm>
#include <zxing/qrcode/detector/FinderPatternFinder.h>
#include <zxing/ReaderException.h>
#include <zxing/DecodeHints.h>

using std::sort;
using std::max;
using std::min;
using std::abs;
using std::vector;
using zxing::Ref;
using zxing::qrcode::FinderPatternFinder;
using zxing::qrcode::FinderPattern;
using zxing::qrcode::FinderPatternInfo;

// VC++

using zxing::BitMatrix;
using zxing::ResultPointCallback;
using zxing::ResultPoint;
using zxing::DecodeHints;

namespace {

class FurthestFromAverageComparator {
private:
  const float averageModuleSize_;
public:
  FurthestFromAverageComparator(float averageModuleSize) :
    averageModuleSize_(averageModuleSize) {
  }
  bool operator()(Ref<FinderPattern> a, Ref<FinderPattern> b) {
    float dA = abs(a->getEstimatedModuleSize() - averageModuleSize_);
    float dB = abs(b->getEstimatedModuleSize() - averageModuleSize_);
    return dA > dB;
  }
};

class CenterComparator {
  const float averageModuleSize_;
public:
  CenterComparator(float averageModuleSize) :
    averageModuleSize_(averageModuleSize) {
  }
  bool operator()(Ref<FinderPattern> a, Ref<FinderPattern> b) {
    // N.B.: we want the result in descending order ...
    if (a->getCount() != b->getCount()) {
      return a->getCount() > b->getCount();
    } else {
      float dA = abs(a->getEstimatedModuleSize() - averageModuleSize_);
      float dB = abs(b->getEstimatedModuleSize() - averageModuleSize_);
      return dA < dB;
    }
  }
};

}

int FinderPatternFinder::CENTER_QUORUM = 2;
int FinderPatternFinder::MIN_SKIP = 3;
int FinderPatternFinder::MAX_MODULES = 57;

float FinderPatternFinder::centerFromEnd(int* stateCount, int end) {
  return (float)(end - stateCount[4] - stateCount[3]) - stateCount[2] / 2.0f;
}

bool FinderPatternFinder::foundPatternCross(int* stateCount) {
  int totalModuleSize = 0;
  for (int i = 0; i < 5; i++) {
    if (stateCount[i] == 0) {
      return false;
    }
    totalModuleSize += stateCount[i];
  }
  if (totalModuleSize < 7) {
    return false;
  }
  float moduleSize = (float)totalModuleSize / 7.0f;
  // Default: 50% tolerance. tryHarder: tolerate local stretch from perspective / mild warping.
  float maxVariance = tryHarder_ ? moduleSize * 0.82f : moduleSize / 2.0f;
  return abs(moduleSize - stateCount[0]) < maxVariance && abs(moduleSize - stateCount[1]) < maxVariance && abs(3.0f
         * moduleSize - stateCount[2]) < 3.0f * maxVariance && abs(moduleSize - stateCount[3]) < maxVariance && abs(
           moduleSize - stateCount[4]) < maxVariance;
}

bool FinderPatternFinder::foundPatternCrossPartial(int* stateCount) {
  // Accept patterns where only 3 of 5 segments match the expected 1:1:3:1:1 ratio.
  // This handles partially occluded finder patterns where 1-2 outer segments
  // are broken. Checks three overlapping windows of 3 consecutive segments.
  int totalModuleSize = 0;
  for (int i = 0; i < 5; i++) {
    if (stateCount[i] == 0) return false;
    totalModuleSize += stateCount[i];
  }
  if (totalModuleSize < 7) return false;

  float moduleSize = (float)totalModuleSize / 7.0f;
  // Stricter tolerance for partial patterns: 45% for normal, 70% for tryHarder
  float maxVariance = tryHarder_ ? moduleSize * 0.7f : moduleSize * 0.45f;

  // Center 3: segments 1,2,3 should match 1:3:1 ratio
  float c3Sum = (float)(stateCount[1] + stateCount[2] + stateCount[3]);
  float c3Ms = c3Sum / 5.0f;  // expected: 1+3+1=5 modules
  bool centerOk = abs(c3Ms - stateCount[1]) < maxVariance &&
                  abs(3.0f * c3Ms - stateCount[2]) < 3.0f * maxVariance &&
                  abs(c3Ms - stateCount[3]) < maxVariance;

  // Left 3: segments 0,1,2 should match 1:1:3 ratio
  float l3Sum = (float)(stateCount[0] + stateCount[1] + stateCount[2]);
  float l3Ms = l3Sum / 5.0f;  // 1+1+3=5 modules
  bool leftOk = abs(l3Ms - stateCount[0]) < maxVariance &&
                abs(l3Ms - stateCount[1]) < maxVariance &&
                abs(3.0f * l3Ms - stateCount[2]) < 3.0f * maxVariance;

  // Right 3: segments 2,3,4 should match 3:1:1 ratio
  float r3Sum = (float)(stateCount[2] + stateCount[3] + stateCount[4]);
  float r3Ms = r3Sum / 5.0f;
  bool rightOk = abs(3.0f * r3Ms - stateCount[2]) < 3.0f * maxVariance &&
                 abs(r3Ms - stateCount[3]) < maxVariance &&
                 abs(r3Ms - stateCount[4]) < maxVariance;

  return centerOk || leftOk || rightOk;
}

float FinderPatternFinder::crossCheckVertical(size_t startI, size_t centerJ, int maxCount, int originalStateCountTotal) {

  int maxI = image_->getHeight();
  int stateCount[5];
  for (int i = 0; i < 5; i++)
    stateCount[i] = 0;


  // Start counting up from center
  int i = startI;
  while (i >= 0 && image_->get(centerJ, i)) {
    stateCount[2]++;
    i--;
  }
  if (i < 0) {
    return nan();
  }
  while (i >= 0 && !image_->get(centerJ, i) && stateCount[1] <= maxCount) {
    stateCount[1]++;
    i--;
  }
  // If already too many modules in this state or ran off the edge:
  if (i < 0 || stateCount[1] > maxCount) {
    return nan();
  }
  while (i >= 0 && image_->get(centerJ, i) && stateCount[0] <= maxCount) {
    stateCount[0]++;
    i--;
  }
  if (stateCount[0] > maxCount) {
    return nan();
  }

  // Now also count down from center
  i = startI + 1;
  while (i < maxI && image_->get(centerJ, i)) {
    stateCount[2]++;
    i++;
  }
  if (i == maxI) {
    return nan();
  }
  while (i < maxI && !image_->get(centerJ, i) && stateCount[3] < maxCount) {
    stateCount[3]++;
    i++;
  }
  if (i == maxI || stateCount[3] >= maxCount) {
    return nan();
  }
  while (i < maxI && image_->get(centerJ, i) && stateCount[4] < maxCount) {
    stateCount[4]++;
    i++;
  }
  if (stateCount[4] >= maxCount) {
    return nan();
  }

  // If we found a finder-pattern-like section, but its size is more than 40% different than
  // the original, assume it's a false positive
  int stateCountTotal = stateCount[0] + stateCount[1] + stateCount[2] + stateCount[3] + stateCount[4];
  int verticalSlack = tryHarder_ ? 3 : 2;
  if (5 * abs(stateCountTotal - originalStateCountTotal) >= verticalSlack * originalStateCountTotal) {
    return nan();
  }

  return foundPatternCross(stateCount) ? centerFromEnd(stateCount, i) : nan();
}

float FinderPatternFinder::crossCheckHorizontal(size_t startJ, size_t centerI, int maxCount,
    int originalStateCountTotal) {

  int maxJ = image_->getWidth();
  int stateCount[5];
  for (int i = 0; i < 5; i++)
    stateCount[i] = 0;

  int j = startJ;
  while (j >= 0 && image_->get(j, centerI)) {
    stateCount[2]++;
    j--;
  }
  if (j < 0) {
    return nan();
  }
  while (j >= 0 && !image_->get(j, centerI) && stateCount[1] <= maxCount) {
    stateCount[1]++;
    j--;
  }
  if (j < 0 || stateCount[1] > maxCount) {
    return nan();
  }
  while (j >= 0 && image_->get(j, centerI) && stateCount[0] <= maxCount) {
    stateCount[0]++;
    j--;
  }
  if (stateCount[0] > maxCount) {
    return nan();
  }

  j = startJ + 1;
  while (j < maxJ && image_->get(j, centerI)) {
    stateCount[2]++;
    j++;
  }
  if (j == maxJ) {
    return nan();
  }
  while (j < maxJ && !image_->get(j, centerI) && stateCount[3] < maxCount) {
    stateCount[3]++;
    j++;
  }
  if (j == maxJ || stateCount[3] >= maxCount) {
    return nan();
  }
  while (j < maxJ && image_->get(j, centerI) && stateCount[4] < maxCount) {
    stateCount[4]++;
    j++;
  }
  if (stateCount[4] >= maxCount) {
    return nan();
  }

  // If we found a finder-pattern-like section, but its size is significantly different than
  // the original, assume it's a false positive
  int stateCountTotal = stateCount[0] + stateCount[1] + stateCount[2] + stateCount[3] + stateCount[4];
  int horizontalSlack = tryHarder_ ? 2 : 1;
  if (5 * abs(stateCountTotal - originalStateCountTotal) >= horizontalSlack * originalStateCountTotal) {
    return nan();
  }

  return foundPatternCross(stateCount) ? centerFromEnd(stateCount, j) : nan();
}

bool FinderPatternFinder::handlePossibleCenter(int* stateCount, size_t i, size_t j) {
  int stateCountTotal = stateCount[0] + stateCount[1] + stateCount[2] + stateCount[3] + stateCount[4];
  float centerJ = centerFromEnd(stateCount, j);
  float centerI = crossCheckVertical(i, (size_t)centerJ, stateCount[2], stateCountTotal);
  if (!isnan(centerI)) {
    // Re-cross check
    centerJ = crossCheckHorizontal((size_t)centerJ, (size_t)centerI, stateCount[2], stateCountTotal);
    if (!isnan(centerJ)) {
      float estimatedModuleSize = (float)stateCountTotal / 7.0f;
      bool found = false;
      size_t max = possibleCenters_.size();
      for (size_t index = 0; index < max; index++) {
        Ref<FinderPattern> center = possibleCenters_[index];
        // Look for about the same center and module size:
        if (center->aboutEquals(estimatedModuleSize, centerI, centerJ)) {
          possibleCenters_[index] = center->combineEstimate(centerI, centerJ, estimatedModuleSize);
          found = true;
          break;
        }
      }
      if (!found) {
        Ref<FinderPattern> newPattern(new FinderPattern(centerJ, centerI, estimatedModuleSize));
        possibleCenters_.push_back(newPattern);
        if (callback_ != 0) {
          callback_->foundPossibleResultPoint(*newPattern);
        }
      }
      return true;
    }
  }
  return false;
}

bool FinderPatternFinder::handlePossibleCenterPartial(int* stateCount, size_t i, size_t j) {
  // Wider cross-check tolerances for partially occluded finder patterns.
  // The pattern may lack 1-2 outer segments, so we allow 1.5x the center
  // width for vertical/horizontal cross-check ranges.
  int stateCountTotal = stateCount[0] + stateCount[1] + stateCount[2] +
                        stateCount[3] + stateCount[4];
  float centerJ = centerFromEnd(stateCount, j);
  // Allow wider search range for cross checks
  int wideMaxCount = stateCount[2] + (stateCount[2] >> 1);  // 1.5x
  float centerI = crossCheckVertical(i, (size_t)centerJ, wideMaxCount, stateCountTotal);
  if (!isnan(centerI)) {
    centerJ = crossCheckHorizontal((size_t)centerJ, (size_t)centerI, wideMaxCount, stateCountTotal);
    if (!isnan(centerJ)) {
      float estimatedModuleSize = (float)stateCountTotal / 7.0f;
      bool found = false;
      size_t max = possibleCenters_.size();
      for (size_t index = 0; index < max; index++) {
        Ref<FinderPattern> center = possibleCenters_[index];
        if (center->aboutEquals(estimatedModuleSize, centerI, centerJ)) {
          possibleCenters_[index] = center->combineEstimate(centerI, centerJ, estimatedModuleSize);
          found = true;
          break;
        }
      }
      if (!found) {
        Ref<FinderPattern> newPattern(new FinderPattern(centerJ, centerI, estimatedModuleSize));
        possibleCenters_.push_back(newPattern);
        if (callback_ != 0) {
          callback_->foundPossibleResultPoint(*newPattern);
        }
      }
      return true;
    }
  }
  return false;
}

int FinderPatternFinder::findRowSkip() {
  size_t max = possibleCenters_.size();
  if (max <= 1) {
    return 0;
  }
  Ref<FinderPattern> firstConfirmedCenter;
  for (size_t i = 0; i < max; i++) {
    Ref<FinderPattern> center = possibleCenters_[i];
    if (center->getCount() >= CENTER_QUORUM) {
      if (firstConfirmedCenter == 0) {
        firstConfirmedCenter = center;
      } else {
        // We have two confirmed centers
        // How far down can we skip before resuming looking for the next
        // pattern? In the worst case, only the difference between the
        // difference in the x / y coordinates of the two centers.
        // This is the case where you find top left first. Draw it out.
        hasSkipped_ = true;
        return (int)(abs(firstConfirmedCenter->getX() - center->getX()) - abs(firstConfirmedCenter->getY()
                     - center->getY()))/2;
      }
    }
  }
  return 0;
}

bool FinderPatternFinder::haveMultiplyConfirmedCenters() {
  int confirmedCount = 0;
  float totalModuleSize = 0.0f;
  size_t max = possibleCenters_.size();
  for (size_t i = 0; i < max; i++) {
    Ref<FinderPattern> pattern = possibleCenters_[i];
    if (pattern->getCount() >= CENTER_QUORUM) {
      confirmedCount++;
      totalModuleSize += pattern->getEstimatedModuleSize();
    }
  }
  if (confirmedCount < 3) {
    return false;
  }
  // OK, we have at least 3 confirmed centers, but, it's possible that one is a "false positive"
  // and that we need to keep looking. We detect this by asking if the estimated module sizes
  // vary too much. We arbitrarily say that when the total deviation from average exceeds
  // 5% of the total module size estimates, it's too much.
  float average = totalModuleSize / max;
  float totalDeviation = 0.0f;
  for (size_t i = 0; i < max; i++) {
    Ref<FinderPattern> pattern = possibleCenters_[i];
    totalDeviation += abs(pattern->getEstimatedModuleSize() - average);
  }
  return totalDeviation <= 0.05f * totalModuleSize;
}

vector< Ref<FinderPattern> > FinderPatternFinder::inferThirdFromTwo(
    Ref<FinderPattern> a, Ref<FinderPattern> b) {
  // Given two finder patterns A and B, infer the third using right-isosceles
  // triangle geometry. In a QR code, the three finder patterns form a
  // right isosceles triangle. The third point C satisfies:
  //   C = A + (B - A)_perp   or   C = A - (B - A)_perp
  // where perpendicular rotation is (-dy, dx) or (dy, -dx).

  float ax = a->getX(), ay = a->getY();
  float bx = b->getX(), by = b->getY();
  float dx = bx - ax, dy = by - ay;
  float avgMs = (a->getEstimatedModuleSize() + b->getEstimatedModuleSize()) / 2.0f;

  // Two candidate positions for the 3rd finder
  float candidates[4] = {
    ax - dy, ay + dx,   // perpendicular counter-clockwise
    ax + dy, ay - dx    // perpendicular clockwise
  };

  BitMatrix& matrix = *image_;
  int maxI = (int)image_->getHeight();
  int maxJ = (int)image_->getWidth();

  for (int c = 0; c < 2; c++) {
    float cx = candidates[c * 2];
    float cy = candidates[c * 2 + 1];

    // Check bounds with some margin
    int margin = (int)(avgMs * 7);
    if ((int)cx < margin || (int)cx >= maxJ - margin ||
        (int)cy < margin || (int)cy >= maxI - margin) {
      continue;
    }

    // Scan horizontally through the candidate center for 1:1:3:1:1 ratio
    int hStateCount[5] = {0, 0, 0, 0, 0};
    int hState = 0;
    int ci = (int)cy;
    int startJ = std::max(0, (int)(cx - avgMs * 7));
    int endJ = std::min(maxJ - 1, (int)(cx + avgMs * 7));
    bool lastBlack = matrix.get(startJ, ci);
    for (int j = startJ; j <= endJ && hState < 5; j++) {
      bool black = matrix.get(j, ci);
      if (black != lastBlack) {
        if (hState < 4) hState++;
        else break;
        lastBlack = black;
      }
      hStateCount[hState]++;
    }
    if (hState < 4) continue;
    // Adjust: first state should be black
    if (!matrix.get(startJ, ci)) {
      for (int k = 4; k > 0; k--) hStateCount[k] = hStateCount[k-1];
      hStateCount[0] = 0;
    }
    if (!foundPatternCross(hStateCount)) continue;

    // Scan vertically through the candidate center
    int vStateCount[5] = {0, 0, 0, 0, 0};
    int vState = 0;
    int cj = (int)cx;
    int startI = std::max(0, (int)(cy - avgMs * 7));
    int endI = std::min(maxI - 1, (int)(cy + avgMs * 7));
    lastBlack = matrix.get(cj, startI);
    for (int i = startI; i <= endI && vState < 5; i++) {
      bool black = matrix.get(cj, i);
      if (black != lastBlack) {
        if (vState < 4) vState++;
        else break;
        lastBlack = black;
      }
      vStateCount[vState]++;
    }
    if (vState < 4) continue;
    if (!matrix.get(cj, startI)) {
      for (int k = 4; k > 0; k--) vStateCount[k] = vStateCount[k-1];
      vStateCount[0] = 0;
    }
    if (!foundPatternCross(vStateCount)) continue;

    // Both scans passed — create a synthesized FinderPattern
    float hTotal = (float)(hStateCount[0] + hStateCount[1] + hStateCount[2] +
                           hStateCount[3] + hStateCount[4]);
    float vTotal = (float)(vStateCount[0] + vStateCount[1] + vStateCount[2] +
                           vStateCount[3] + vStateCount[4]);
    float inferredMs = (hTotal + vTotal) / 14.0f;  // 7 modules each

    Ref<FinderPattern> third(new FinderPattern(cx, cy, inferredMs));
    vector<Ref<FinderPattern> > result(3);
    result[0] = a;
    result[1] = b;
    result[2] = third;
    return result;
  }

  throw zxing::ReaderException("Could not find three finder patterns");
}

vector< Ref<FinderPattern> > FinderPatternFinder::selectBestPatterns() {
  size_t startSize = possibleCenters_.size();

  if (startSize < 3) {
    if (startSize == 2) {
      return inferThirdFromTwo(possibleCenters_[0], possibleCenters_[1]);
    }
    // Couldn't find enough finder patterns
    throw zxing::ReaderException("Could not find three finder patterns");
  }

  // Filter outlier possibilities whose module size is too different
  if (startSize > 3) {
    // But we can only afford to do so if we have at least 4 possibilities to choose from
    float totalModuleSize = 0.0f;
    float square = 0.0f;
    for (size_t i = 0; i < startSize; i++) {
      float size = possibleCenters_[i]->getEstimatedModuleSize();
      totalModuleSize += size;
      square += size * size;
    }
    float average = totalModuleSize / (float) startSize;
    float stdDev = (float)sqrt(square / startSize - average * average);

    sort(possibleCenters_.begin(), possibleCenters_.end(), FurthestFromAverageComparator(average));
    
    float limit = max(0.2f * average, stdDev);

    for (size_t i = 0; i < possibleCenters_.size() && possibleCenters_.size() > 3; i++) {
      if (abs(possibleCenters_[i]->getEstimatedModuleSize() - average) > limit) {
        possibleCenters_.erase(possibleCenters_.begin()+i);
        i--;
      }
    }
  }

  if (possibleCenters_.size() > 3) {
    // Throw away all but those first size candidate points we found.
    float totalModuleSize = 0.0f;
    for (size_t i = 0; i < possibleCenters_.size(); i++) {
      float size = possibleCenters_[i]->getEstimatedModuleSize();
      totalModuleSize += size;
    }
    float average = totalModuleSize / (float) possibleCenters_.size();
    sort(possibleCenters_.begin(), possibleCenters_.end(), CenterComparator(average));
  }

  if (possibleCenters_.size() > 3) {
    possibleCenters_.erase(possibleCenters_.begin()+3,possibleCenters_.end());
  }

  vector<Ref<FinderPattern> > result(3);
  result[0] = possibleCenters_[0];
  result[1] = possibleCenters_[1];
  result[2] = possibleCenters_[2];
  return result;
}

vector<Ref<FinderPattern> > FinderPatternFinder::orderBestPatterns(vector<Ref<FinderPattern> > patterns) {
  // Find distances between pattern centers
  float abDistance = distance(patterns[0], patterns[1]);
  float bcDistance = distance(patterns[1], patterns[2]);
  float acDistance = distance(patterns[0], patterns[2]);

  Ref<FinderPattern> topLeft;
  Ref<FinderPattern> topRight;
  Ref<FinderPattern> bottomLeft;
  // Assume one closest to other two is top left;
  // topRight and bottomLeft will just be guesses below at first
  if (bcDistance >= abDistance && bcDistance >= acDistance) {
    topLeft = patterns[0];
    topRight = patterns[1];
    bottomLeft = patterns[2];
  } else if (acDistance >= bcDistance && acDistance >= abDistance) {
    topLeft = patterns[1];
    topRight = patterns[0];
    bottomLeft = patterns[2];
  } else {
    topLeft = patterns[2];
    topRight = patterns[0];
    bottomLeft = patterns[1];
  }

  // Use cross product to figure out which of other1/2 is the bottom left
  // pattern. The vector "top-left -> bottom-left" x "top-left -> top-right"
  // should yield a vector with positive z component
  if ((bottomLeft->getY() - topLeft->getY()) * (topRight->getX() - topLeft->getX()) < (bottomLeft->getX()
      - topLeft->getX()) * (topRight->getY() - topLeft->getY())) {
    Ref<FinderPattern> temp = topRight;
    topRight = bottomLeft;
    bottomLeft = temp;
  }

  vector<Ref<FinderPattern> > results(3);
  results[0] = bottomLeft;
  results[1] = topLeft;
  results[2] = topRight;
  return results;
}

float FinderPatternFinder::distance(Ref<ResultPoint> p1, Ref<ResultPoint> p2) {
  float dx = p1->getX() - p2->getX();
  float dy = p1->getY() - p2->getY();
  return (float)sqrt(dx * dx + dy * dy);
}

FinderPatternFinder::FinderPatternFinder(Ref<BitMatrix> image,
                                           Ref<ResultPointCallback>const& callback) :
    image_(image), possibleCenters_(), hasSkipped_(false), callback_(callback), tryHarder_(false) {
}

Ref<FinderPatternInfo> FinderPatternFinder::find(DecodeHints const& hints) {
  tryHarder_ = hints.getTryHarder();
  bool tryHarder = tryHarder_;

  size_t maxI = image_->getHeight();
  size_t maxJ = image_->getWidth();


  // We are looking for black/white/black/white/black modules in
  // 1:1:3:1:1 ratio; this tracks the number of such modules seen so far

  // As this is used often, we use an integer array instead of vector
  int stateCount[5];
  bool done = false;


  // Let's assume that the maximum version QR Code we support takes up 1/4
  // the height of the image, and then account for the center being 3
  // modules in size. This gives the smallest number of pixels the center
  // could be, so skip this often. When trying harder, look for all
  // QR versions regardless of how dense they are.
  int iSkip = (3 * maxI) / (4 * MAX_MODULES);
  if (iSkip < MIN_SKIP || tryHarder) {
      iSkip = MIN_SKIP;
  }

  // This is slightly faster than using the Ref. Efficiency is important here
  BitMatrix& matrix = *image_;

  for (size_t i = iSkip - 1; i < maxI && !done; i += iSkip) {
    // Get a row of black/white values

    stateCount[0] = 0;
    stateCount[1] = 0;
    stateCount[2] = 0;
    stateCount[3] = 0;
    stateCount[4] = 0;
    int currentState = 0;
    for (size_t j = 0; j < maxJ; j++) {
      if (matrix.get(j, i)) {
        // Black pixel
        if ((currentState & 1) == 1) { // Counting white pixels
          currentState++;
        }
        stateCount[currentState]++;
      } else { // White pixel
        if ((currentState & 1) == 0) { // Counting black pixels
          if (currentState == 4) { // A winner?
            if (foundPatternCross(stateCount)) { // Yes
              bool confirmed = handlePossibleCenter(stateCount, i, j);
              if (confirmed) {
                iSkip = 2;
                if (hasSkipped_) {
                  done = haveMultiplyConfirmedCenters();
                } else {
                  int rowSkip = findRowSkip();
                  if (rowSkip > stateCount[2]) {
                    i += rowSkip - stateCount[2] - iSkip;
                    j = maxJ - 1;
                  }
                }
              } else {
                stateCount[0] = stateCount[2];
                stateCount[1] = stateCount[3];
                stateCount[2] = stateCount[4];
                stateCount[3] = 1;
                stateCount[4] = 0;
                currentState = 3;
                continue;
              }
              // Clear state to start looking again
              currentState = 0;
              stateCount[0] = 0;
              stateCount[1] = 0;
              stateCount[2] = 0;
              stateCount[3] = 0;
              stateCount[4] = 0;
            } else { // No, shift counts back by two
              stateCount[0] = stateCount[2];
              stateCount[1] = stateCount[3];
              stateCount[2] = stateCount[4];
              stateCount[3] = 1;
              stateCount[4] = 0;
              currentState = 3;
            }
          } else {
            stateCount[++currentState]++;
          }
        } else { // Counting white pixels
          stateCount[currentState]++;
        }
      }
    }
    if (foundPatternCross(stateCount)) {
      bool confirmed = handlePossibleCenter(stateCount, i, maxJ);
      if (confirmed) {
        iSkip = stateCount[0];
        if (hasSkipped_) {
          done = haveMultiplyConfirmedCenters();
        }
      }
    } else if (foundPatternCrossPartial(stateCount)) {
      handlePossibleCenterPartial(stateCount, i, maxJ);
    }
  }

  vector<Ref<FinderPattern> > patternInfo = selectBestPatterns();
  patternInfo = orderBestPatterns(patternInfo);

  Ref<FinderPatternInfo> result(new FinderPatternInfo(patternInfo));
  return result;
}

Ref<BitMatrix> FinderPatternFinder::getImage() {
  return image_;
}

vector<Ref<FinderPattern> >& FinderPatternFinder::getPossibleCenters() {
  return possibleCenters_;
}
