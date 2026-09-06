// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
#ifndef __QR_GRID_NORMALIZER_H__
#define __QR_GRID_NORMALIZER_H__

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

#include <zxing/common/Array.h>

#include <vector>

namespace zxing {
namespace qrcode {

struct MutableImage {
  ArrayRef<char> pixels;
  int width;
  int height;
  int components;
};

struct NormalizedImage {
  ArrayRef<char> pixels;
  int width;
  int height;
  int components;
};

// Returns up to min(3, maximumCandidates) normalized images.
// Empty when maximumCandidates <= 0, the image fails the working-set budget
// check (caller keeps the original), or normalization finds no confident grid.
std::vector<NormalizedImage> NormalizeQR(
    const MutableImage& image, int maximumCandidates);

}
}

#endif // __QR_GRID_NORMALIZER_H__
