// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
/*
 *  FinderPatternTemplateMatcher.cpp
 *  zxing
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <zxing/qrcode/detector/FinderPatternTemplateMatcher.h>
#include <algorithm>
#include <cmath>

namespace zxing {
namespace qrcode {

using std::vector;
using std::max;
using std::min;
using std::abs;
using std::sqrt;

// 7x7 finder pattern: outer black border, white ring, inner 3x3 black
const int FinderPatternTemplateMatcher::TEMPLATE[7][7] = {
  {1, 1, 1, 1, 1, 1, 1},
  {1, 0, 0, 0, 0, 0, 1},
  {1, 0, 1, 1, 1, 0, 1},
  {1, 0, 1, 1, 1, 0, 1},
  {1, 0, 1, 1, 1, 0, 1},
  {1, 0, 0, 0, 0, 0, 1},
  {1, 1, 1, 1, 1, 1, 1},
};

const float FinderPatternTemplateMatcher::SCORE_THRESHOLD = 0.35f;

float FinderPatternTemplateMatcher::matchAt(
    Ref<BitMatrix> image, int cx, int cy, int moduleSize) {
  if (moduleSize < 1) return 0.0f;

  float half = 3.5f * (float)moduleSize;
  float left = (float)cx - half;
  float top = (float)cy - half;

  int w = image->getWidth();
  int h = image->getHeight();

  // Check that the template region fits within the image
  if (left < 0 || top < 0 || left + 7.0f * moduleSize >= w ||
      top + 7.0f * moduleSize >= h) {
    return 0.0f;
  }

  float num = 0.0f, denomImg = 0.0f, denomTpl = 0.0f;
  float tplMean = 0.0f;
  int tplCount = 0;

  // Compute template mean (for normalized correlation)
  for (int ty = 0; ty < 7; ty++) {
    for (int tx = 0; tx < 7; tx++) {
      tplMean += (float)TEMPLATE[ty][tx];
      tplCount++;
    }
  }
  tplMean /= (float)tplCount;

  // Sample the image at each module center in the 7x7 grid
  for (int ty = 0; ty < 7; ty++) {
    for (int tx = 0; tx < 7; tx++) {
      float sx = left + ((float)tx + 0.5f) * (float)moduleSize;
      float sy = top + ((float)ty + 0.5f) * (float)moduleSize;
      int ix = (int)sx;
      int iy = (int)sy;
      if (ix < 0 || ix >= w || iy < 0 || iy >= h) return 0.0f;

      float imgVal = image->get(ix, iy) ? 1.0f : 0.0f;
      float tplVal = (float)TEMPLATE[ty][tx];

      num += imgVal * tplVal;
      denomImg += imgVal * imgVal;
      denomTpl += (tplVal - tplMean) * (tplVal - tplMean);
    }
  }

  if (denomImg == 0.0f || denomTpl == 0.0f) return 0.0f;

  float score = num / sqrt(denomImg * denomTpl);
  return score;
}

vector<Ref<FinderPattern> >
FinderPatternTemplateMatcher::findCandidates(
    Ref<BitMatrix> image, int minModuleSize, int maxModuleSize) {
  vector<Ref<FinderPattern> > candidates;

  int w = image->getWidth();
  int h = image->getHeight();

  if (minModuleSize < 2) minModuleSize = 2;
  if (maxModuleSize > min(w, h) / 7) maxModuleSize = min(w, h) / 7;
  if (maxModuleSize < minModuleSize) return candidates;

  // Search at geometrically-spaced scales for efficiency
  for (int ms = minModuleSize; ms <= maxModuleSize;
       ms = max(ms + 1, (int)(ms * 1.25f))) {
    int step = max(1, ms);      // one module step for x
    int skip = max(1, ms * 2);  // two module skip for y

    for (int y = ms * 3; y < h - ms * 3; y += skip) {
      for (int x = ms * 3; x < w - ms * 3; x += step) {
        float score = matchAt(image, x, y, ms);
        if (score > SCORE_THRESHOLD) {
          Ref<FinderPattern> fp(new FinderPattern((float)x, (float)y, (float)ms));
          candidates.push_back(fp);
        }
      }
    }
  }

  // Sort by score descending (we'll use count_ from the FinderPattern
  // to infer quality; but FinderPattern constructor sets count_ to 1).
  // We need a different mechanism — sort by module size consistency instead.
  // For now, just return candidates sorted by proximity for NMS.
  return candidates;
}

vector<Ref<FinderPattern> >
FinderPatternTemplateMatcher::nonMaxSuppression(
    vector<Ref<FinderPattern> >& candidates, float overlapFraction) {
  if (candidates.size() <= 1) return candidates;

  vector<Ref<FinderPattern> > result;

  for (size_t i = 0; i < candidates.size(); i++) {
    if (candidates[i] == 0) continue;
    bool keep = true;
    float msA = candidates[i]->getEstimatedModuleSize();
    float cxA = candidates[i]->getX();
    float cyA = candidates[i]->getY();

    for (size_t j = i + 1; j < candidates.size(); j++) {
      if (candidates[j] == 0) continue;
      float msB = candidates[j]->getEstimatedModuleSize();
      float cxB = candidates[j]->getX();
      float cyB = candidates[j]->getY();

      // Check spatial overlap: centers within overlapFraction * moduleSize
      float dist = sqrt((cxA - cxB) * (cxA - cxB) +
                        (cyA - cyB) * (cyA - cyB));
      if (dist < overlapFraction * max(msA, msB) * 7.0f &&
          abs(msA - msB) < 0.5f * max(msA, msB)) {
        // Keep the one with smaller module size (more likely correct)
        if (msA > msB) {
          keep = false;
          break;
        } else {
          candidates[j].reset(0);
        }
      }
    }
    if (keep) {
      result.push_back(candidates[i]);
    }
  }

  return result;
}

}
}
