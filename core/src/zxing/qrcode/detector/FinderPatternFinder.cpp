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
#include <limits>
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

class EstimatedModuleComparator {
public:
  bool operator()(Ref<FinderPattern> a, Ref<FinderPattern> b) const {
    return a->getEstimatedModuleSize() < b->getEstimatedModuleSize();
  }
};

struct RecoveredPatternCandidate {
  float x;
  float y;
  float moduleSize;
  float score;
  bool matched;
  int inBoundsCorners;
};

double squaredDistance(Ref<FinderPattern> a, Ref<FinderPattern> b) {
  double x = a->getX() - b->getX();
  double y = a->getY() - b->getY();
  return x * x + y * y;
}

bool inBounds(const BitMatrix& image, int x, int y)
{
  return x >= 0 && y >= 0 && x < (int)image.getWidth() && y < (int)image.getHeight();
}

bool foundFinderLikePattern(const int* stateCount, float varianceDivisor)
{
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
  float maxVariance = moduleSize / varianceDivisor;
  return abs(moduleSize - stateCount[0]) < maxVariance &&
         abs(moduleSize - stateCount[1]) < maxVariance &&
         abs(3.0f * moduleSize - stateCount[2]) < 3.0f * maxVariance &&
         abs(moduleSize - stateCount[3]) < maxVariance &&
         abs(moduleSize - stateCount[4]) < maxVariance;
}

bool readAxisStateCount(const BitMatrix& image, int centerX, int centerY, int dx, int dy, int* stateCount)
{
  for (int i = 0; i < 5; i++) {
    stateCount[i] = 0;
  }

  int x = centerX;
  int y = centerY;
  while (inBounds(image, x, y) && image.get(x, y)) {
    stateCount[2]++;
    x -= dx;
    y -= dy;
  }
  while (inBounds(image, x, y) && !image.get(x, y)) {
    stateCount[1]++;
    x -= dx;
    y -= dy;
  }
  while (inBounds(image, x, y) && image.get(x, y)) {
    stateCount[0]++;
    x -= dx;
    y -= dy;
  }

  x = centerX + dx;
  y = centerY + dy;
  while (inBounds(image, x, y) && image.get(x, y)) {
    stateCount[2]++;
    x += dx;
    y += dy;
  }
  while (inBounds(image, x, y) && !image.get(x, y)) {
    stateCount[3]++;
    x += dx;
    y += dy;
  }
  while (inBounds(image, x, y) && image.get(x, y)) {
    stateCount[4]++;
    x += dx;
    y += dy;
  }

  return stateCount[0] > 0 && stateCount[1] > 0 && stateCount[2] > 0 && stateCount[3] > 0 && stateCount[4] > 0;
}

float patternDeviation(const int* stateCount)
{
  int total = 0;
  for (int i = 0; i < 5; i++) {
    total += stateCount[i];
  }
  if (total < 7) {
    return std::numeric_limits<float>::max();
  }
  float module = (float)total / 7.0f;
  float expected[5] = {module, module, 3.0f * module, module, module};
  float error = 0.0f;
  for (int i = 0; i < 5; i++) {
    error += abs(stateCount[i] - expected[i]) / expected[i];
  }
  return error;
}

RecoveredPatternCandidate evaluateRecoveredCandidate(const BitMatrix& image, Ref<FinderPattern> a, Ref<FinderPattern> b,
                                                     float estX, float estY, float moduleSize)
{
  RecoveredPatternCandidate result;
  result.x = estX;
  result.y = estY;
  result.moduleSize = moduleSize;
  result.score = std::numeric_limits<float>::max();
  result.matched = false;
  result.inBoundsCorners = 0;

  int cX = (int)(estX + 0.5f);
  int cY = (int)(estY + 0.5f);
  if (inBounds(image, cX, cY)) {
    result.inBoundsCorners++;
  }

  float dX = b->getX() + estX - a->getX();
  float dY = b->getY() + estY - a->getY();
  if (inBounds(image, (int)(dX + 0.5f), (int)(dY + 0.5f))) {
    result.inBoundsCorners++;
  }

  int searchRadius = max(2, (int)(moduleSize * 4.0f));
  int estXi = (int)(estX + 0.5f);
  int estYi = (int)(estY + 0.5f);
  for (int dy = -searchRadius; dy <= searchRadius; dy++) {
    for (int dx = -searchRadius; dx <= searchRadius; dx++) {
      int x = estXi + dx;
      int y = estYi + dy;
      if (!inBounds(image, x, y) || !image.get(x, y)) {
        continue;
      }

      int horizontal[5];
      int vertical[5];
      if (!readAxisStateCount(image, x, y, 1, 0, horizontal) || !readAxisStateCount(image, x, y, 0, 1, vertical)) {
        continue;
      }

      if (!foundFinderLikePattern(horizontal, 1.4f) || !foundFinderLikePattern(vertical, 1.4f)) {
        continue;
      }

      float score = patternDeviation(horizontal) + patternDeviation(vertical);
      float offsetPenalty = (float)(dx * dx + dy * dy) / (moduleSize * moduleSize + 1.0f);
      score += 0.05f * offsetPenalty;
      if (score < result.score) {
        result.score = score;
        result.x = (float)x;
        result.y = (float)y;
        result.moduleSize = (float)(horizontal[0] + horizontal[1] + horizontal[2] + horizontal[3] + horizontal[4]) / 7.0f;
        result.matched = true;
      }
    }
  }

  if (!result.matched) {
    // Keep deterministic ordering for synthetic fallback.
    float imageCx = (image.getWidth() - 1) / 2.0f;
    float imageCy = (image.getHeight() - 1) / 2.0f;
    float dx = estX - imageCx;
    float dy = estY - imageCy;
    result.score = dx * dx + dy * dy;
  }

  return result;
}

bool isBetterRecoveredCandidate(const RecoveredPatternCandidate& a, const RecoveredPatternCandidate& b)
{
  if (a.matched != b.matched) {
    return a.matched;
  }
  if (a.inBoundsCorners != b.inBoundsCorners) {
    return a.inBoundsCorners > b.inBoundsCorners;
  }
  return a.score < b.score;
}

}

int FinderPatternFinder::CENTER_QUORUM = 2;
int FinderPatternFinder::MIN_SKIP = 3;
int FinderPatternFinder::MAX_MODULES = 97;

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
  float maxVariance = moduleSize / 2.0f;
  // Allow less than 50% variance from 1-1-3-1-1 proportions
  return abs(moduleSize - stateCount[0]) < maxVariance && abs(moduleSize - stateCount[1]) < maxVariance && abs(3.0f
         * moduleSize - stateCount[2]) < 3.0f * maxVariance && abs(moduleSize - stateCount[3]) < maxVariance && abs(
           moduleSize - stateCount[4]) < maxVariance;
}

bool FinderPatternFinder::foundPatternDiagonal(int* stateCount) {
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
  float maxVariance = moduleSize / 1.333f;
  // Allow less than 75% variance from 1-1-3-1-1 proportions.
  return abs(moduleSize - stateCount[0]) < maxVariance && abs(moduleSize - stateCount[1]) < maxVariance && abs(3.0f
         * moduleSize - stateCount[2]) < 3.0f * maxVariance && abs(moduleSize - stateCount[3]) < maxVariance && abs(
           moduleSize - stateCount[4]) < maxVariance;
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
  if (5 * abs(stateCountTotal - originalStateCountTotal) >= 2 * originalStateCountTotal) {
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
  if (5 * abs(stateCountTotal - originalStateCountTotal) >= originalStateCountTotal) {
    return nan();
  }

  return foundPatternCross(stateCount) ? centerFromEnd(stateCount, j) : nan();
}

bool FinderPatternFinder::crossCheckDiagonal(size_t centerI, size_t centerJ) {
  int stateCount[5] = {0, 0, 0, 0, 0};

  // Start counting up-left from center finding black center mass.
  size_t i = 0;
  while (centerI >= i && centerJ >= i && image_->get(centerJ - i, centerI - i)) {
    stateCount[2]++;
    i++;
  }
  if (stateCount[2] == 0) {
    return false;
  }

  // Continue up-left finding white ring.
  while (centerI >= i && centerJ >= i && !image_->get(centerJ - i, centerI - i)) {
    stateCount[1]++;
    i++;
  }
  if (stateCount[1] == 0) {
    return false;
  }

  // Continue up-left finding black ring.
  while (centerI >= i && centerJ >= i && image_->get(centerJ - i, centerI - i)) {
    stateCount[0]++;
    i++;
  }
  if (stateCount[0] == 0) {
    return false;
  }

  size_t maxI = image_->getHeight();
  size_t maxJ = image_->getWidth();

  // Now count down-right from center.
  i = 1;
  while (centerI + i < maxI && centerJ + i < maxJ && image_->get(centerJ + i, centerI + i)) {
    stateCount[2]++;
    i++;
  }

  while (centerI + i < maxI && centerJ + i < maxJ && !image_->get(centerJ + i, centerI + i)) {
    stateCount[3]++;
    i++;
  }
  if (stateCount[3] == 0) {
    return false;
  }

  while (centerI + i < maxI && centerJ + i < maxJ && image_->get(centerJ + i, centerI + i)) {
    stateCount[4]++;
    i++;
  }
  if (stateCount[4] == 0) {
    return false;
  }

  return foundPatternDiagonal(stateCount);
}

bool FinderPatternFinder::handlePossibleCenter(int* stateCount, size_t i, size_t j) {
  int stateCountTotal = stateCount[0] + stateCount[1] + stateCount[2] + stateCount[3] + stateCount[4];
  float centerJ = centerFromEnd(stateCount, j);
  float centerI = crossCheckVertical(i, (size_t)centerJ, stateCount[2], stateCountTotal);
  if (!isnan(centerI)) {
    // Re-cross check
    centerJ = crossCheckHorizontal((size_t)centerJ, (size_t)centerI, stateCount[2], stateCountTotal);
    if (!isnan(centerJ) && crossCheckDiagonal((size_t)centerI, (size_t)centerJ)) {
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

vector< Ref<FinderPattern> > FinderPatternFinder::selectBestPatterns() {
  if (possibleCenters_.size() < 2) {
    throw zxing::ReaderException("Could not find enough finder patterns");
  }

  vector<Ref<FinderPattern> > confirmedCenters;
  confirmedCenters.reserve(possibleCenters_.size());
  for (size_t i = 0; i < possibleCenters_.size(); i++) {
    if (possibleCenters_[i]->getCount() >= CENTER_QUORUM) {
      confirmedCenters.push_back(possibleCenters_[i]);
    }
  }

  vector<Ref<FinderPattern> > candidates = confirmedCenters.size() >= 3 ? confirmedCenters : possibleCenters_;

  if (candidates.size() >= 3) {
    // Sort by module size so we can prune combinations whose sizes are too dissimilar.
    sort(candidates.begin(), candidates.end(), EstimatedModuleComparator());

    double bestDistortion = std::numeric_limits<double>::max();
    vector<Ref<FinderPattern> > bestPatterns(3);

    for (size_t i = 0; i + 2 < candidates.size(); i++) {
      Ref<FinderPattern> fpi = candidates[i];
      float minModuleSize = fpi->getEstimatedModuleSize();

      for (size_t j = i + 1; j + 1 < candidates.size(); j++) {
        Ref<FinderPattern> fpj = candidates[j];
        double squares0 = squaredDistance(fpi, fpj);

        for (size_t k = j + 1; k < candidates.size(); k++) {
          Ref<FinderPattern> fpk = candidates[k];
          float maxModuleSize = fpk->getEstimatedModuleSize();
          if (maxModuleSize > minModuleSize * 1.8f) {
            break;
          }

          double a = squares0;
          double b = squaredDistance(fpj, fpk);
          double c = squaredDistance(fpi, fpk);

          // Sort a,b,c ascending (inlined, branch-light).
          if (a < b) {
            if (b > c) {
              if (a < c) {
                double t = b;
                b = c;
                c = t;
              } else {
                double t = a;
                a = c;
                c = b;
                b = t;
              }
            }
          } else {
            if (b < c) {
              if (a < c) {
                double t = a;
                a = b;
                b = t;
              } else {
                double t = a;
                a = b;
                b = c;
                c = t;
              }
            } else {
              double t = a;
              a = c;
              c = t;
            }
          }

          // Finder patterns should form an isosceles-right triangle.
          double distortion = abs(c - 2 * b) + abs(c - 2 * a);
          if (distortion < bestDistortion) {
            bestDistortion = distortion;
            bestPatterns[0] = fpi;
            bestPatterns[1] = fpj;
            bestPatterns[2] = fpk;
          }
        }
      }
    }

    if (bestDistortion != std::numeric_limits<double>::max()) {
      return bestPatterns;
    }
  }

  // Recover missing finder pattern by copying/extrapolating from the best two detected "hui" patterns.
  double bestPairScore = -1.0;
  Ref<FinderPattern> bestA;
  Ref<FinderPattern> bestB;
  for (size_t i = 0; i + 1 < possibleCenters_.size(); i++) {
    for (size_t j = i + 1; j < possibleCenters_.size(); j++) {
      Ref<FinderPattern> a = possibleCenters_[i];
      Ref<FinderPattern> b = possibleCenters_[j];
      float minSize = std::min(a->getEstimatedModuleSize(), b->getEstimatedModuleSize());
      float maxSize = max(a->getEstimatedModuleSize(), b->getEstimatedModuleSize());
      if (minSize <= 0.0f || maxSize > minSize * 2.2f) {
        continue;
      }
      double dist = squaredDistance(a, b);
      if (dist < (double)(minSize * minSize * 9.0f)) {
        continue;
      }
      double pairScore = a->getCount() + b->getCount() - abs(a->getEstimatedModuleSize() - b->getEstimatedModuleSize()) / maxSize;
      if (pairScore > bestPairScore) {
        bestPairScore = pairScore;
        bestA = a;
        bestB = b;
      }
    }
  }
  if (bestA == 0 || bestB == 0) {
    throw zxing::ReaderException("Could not recover missing finder pattern");
  }

  float avgModuleSize = (bestA->getEstimatedModuleSize() + bestB->getEstimatedModuleSize()) / 2.0f;
  float vx = bestB->getX() - bestA->getX();
  float vy = bestB->getY() - bestA->getY();

  RecoveredPatternCandidate candidate1 = evaluateRecoveredCandidate(*image_, bestA, bestB, bestA->getX() - vy,
                                                                    bestA->getY() + vx, avgModuleSize);
  RecoveredPatternCandidate candidate2 = evaluateRecoveredCandidate(*image_, bestA, bestB, bestA->getX() + vy,
                                                                    bestA->getY() - vx, avgModuleSize);
  RecoveredPatternCandidate bestRecovered = isBetterRecoveredCandidate(candidate1, candidate2) ? candidate1 : candidate2;

  Ref<FinderPattern> recovered(new FinderPattern(bestRecovered.x, bestRecovered.y, bestRecovered.moduleSize));
  vector<Ref<FinderPattern> > result(3);
  result[0] = bestA;
  result[1] = bestB;
  result[2] = recovered;
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
    image_(image), possibleCenters_(), hasSkipped_(false), callback_(callback) {
}

Ref<FinderPatternInfo> FinderPatternFinder::find(DecodeHints const& hints) {
  bool tryHarder = hints.getTryHarder();

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
            bool foundCross = foundPatternCross(stateCount);
            // Fallback for degraded images: allow a looser ratio early, then rely on
            // vertical/horizontal/diagonal cross-checks plus geometry ranking to reject noise.
            if (!foundCross && possibleCenters_.size() < 3) {
              foundCross = foundPatternDiagonal(stateCount);
            }
            if (foundCross) { // Yes
              bool confirmed = handlePossibleCenter(stateCount, i, j);
              if (confirmed) {
                // Start examining every other line. Checking each line turned out to be too
                // expensive and didn't improve performance.
                iSkip = 2;
                if (hasSkipped_) {
                  done = haveMultiplyConfirmedCenters();
                } else {
                  int rowSkip = findRowSkip();
                  if (rowSkip > stateCount[2]) {
                    // Skip rows between row of lower confirmed center
                    // and top of presumed third confirmed center
                    // but back up a bit to get a full chance of detecting
                    // it, entire width of center of finder pattern

                    // Skip by rowSkip, but back off by stateCount[2] (size
                    // of last center of pattern we saw) to be conservative,
                    // and also back off by iSkip which is about to be
                    // re-added
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
    bool foundCross = foundPatternCross(stateCount);
    if (!foundCross && possibleCenters_.size() < 3) {
      foundCross = foundPatternDiagonal(stateCount);
    }
    if (foundCross) {
      bool confirmed = handlePossibleCenter(stateCount, i, maxJ);
      if (confirmed) {
        iSkip = stateCount[0];
        if (hasSkipped_) {
          // Found a third one
          done = haveMultiplyConfirmedCenters();
        }
      }
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
