// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
#ifndef __FINDER_PATTERN_TEMPLATE_MATCHER_H__
#define __FINDER_PATTERN_TEMPLATE_MATCHER_H__

/*
 *  FinderPatternTemplateMatcher.h
 *  zxing
 *
 *  Copyright 2024 ZXing authors All rights reserved.
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

#include <zxing/common/Counted.h>
#include <zxing/common/BitMatrix.h>
#include <zxing/qrcode/detector/FinderPattern.h>
#include <zxing/ResultPoint.h>
#include <vector>

namespace zxing {
namespace qrcode {

class FinderPatternTemplateMatcher {
public:
  // Find candidate finder patterns by normalized cross-correlation with a
  // 7x7 finder pattern template at multiple scales.
  // Returns candidates sorted by decreasing correlation score.
  static std::vector<Ref<FinderPattern> > findCandidates(
      Ref<BitMatrix> image, int minModuleSize, int maxModuleSize);

  // Remove overlapping candidates, keeping the one with higher score.
  static std::vector<Ref<FinderPattern> > nonMaxSuppression(
      std::vector<Ref<FinderPattern> >& candidates, float overlapFraction);

private:
  // 7x7 finder pattern template: 1=black, 0=white
  static const int TEMPLATE[7][7];

  // Match score at position (cx,cy) with given module size.
  // Returns correlation score in [0, 1].
  static float matchAt(Ref<BitMatrix> image, int cx, int cy, int moduleSize);

  // Score threshold for accepting a candidate (empirically tuned).
  static const float SCORE_THRESHOLD;  // 0.35
};

}
}

#endif // __FINDER_PATTERN_TEMPLATE_MATCHER_H__
