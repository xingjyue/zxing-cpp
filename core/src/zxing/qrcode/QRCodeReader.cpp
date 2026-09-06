// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
/*
 *  QRCodeReader.cpp
 *  zxing
 *
 *  Created by Christian Brunschen on 20/05/2008.
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

#include <zxing/qrcode/QRCodeReader.h>
#include <zxing/qrcode/detector/Detector.h>
#include <zxing/qrcode/QRGridNormalizer.h>
#include <zxing/common/GreyscaleLuminanceSource.h>
#include <zxing/ReaderException.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/Binarizer.h>
#include <zxing/Result.h>
#include <zxing/BarcodeFormat.h>
#include <zxing/common/DetectorResult.h>

#include <cstring>
#include <vector>

namespace zxing {
namespace qrcode {

namespace {

ArrayRef<char> RgbaToGray(const NormalizedImage& image) {
  const int w = image.width;
  const int h = image.height;
  ArrayRef<char> gray(w * h);
  if (image.components == 1) {
    std::memcpy(&gray[0], &image.pixels[0], static_cast<size_t>(w) * h);
    return gray;
  }
  const char* pixels = &image.pixels[0];
  for (int i = 0; i < w * h; ++i) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(
        pixels + i * image.components);
    if (image.components >= 3) {
      gray[i] = static_cast<char>(
          (306 * static_cast<int>(p[0]) +
           601 * static_cast<int>(p[1]) +
           117 * static_cast<int>(p[2]) + 0x200) >> 10);
    } else {
      gray[i] = static_cast<char>(p[0]);
    }
  }
  return gray;
}

}  // namespace

QRCodeReader::QRCodeReader() : decoder_() {}

QRCodeReader::~QRCodeReader() {}

Decoder& QRCodeReader::getDecoder() {
  return decoder_;
}

Ref<Result> QRCodeReader::decodeOnce(Ref<BinaryBitmap> image, DecodeHints hints) {
  Detector detector(image->getBlackMatrix());
  Ref<DetectorResult> detectorResult(detector.detect(hints));
  ArrayRef< Ref<ResultPoint> > points(detectorResult->getPoints());
  Ref<DecoderResult> decoderResult(decoder_.decode(detectorResult->getBits()));
  return Ref<Result>(new Result(
      decoderResult->getText(), decoderResult->getRawBytes(), points,
      BarcodeFormat::QR_CODE));
}

Ref<Result> QRCodeReader::decodeNormalized(Ref<BinaryBitmap> image,
                                           DecodeHints hints,
                                           ReaderException const& primary) {
  Ref<LuminanceSource> source = image->getLuminanceSource();
  if (!source || source->getWidth() <= 0 || source->getHeight() <= 0) {
    throw primary;
  }
  MutableImage mutableImage = {
      source->getMatrix(), source->getWidth(), source->getHeight(), 1};
  std::vector<NormalizedImage> candidates;
  try {
    candidates = NormalizeQR(mutableImage, 3);
  } catch (std::bad_alloc const&) {
    throw;
  }
  if (candidates.empty()) {
    throw primary;
  }
  Ref<Binarizer> binarizer = image->getBinarizer();
  for (size_t i = 0; i < candidates.size(); ++i) {
    ArrayRef<char> gray = RgbaToGray(candidates[i]);
    const int size = candidates[i].width;
    Ref<LuminanceSource> normalizedSource(new GreyscaleLuminanceSource(
        gray, size, size, 0, 0, size, size));
    Ref<BinaryBitmap> normalizedBitmap(
        new BinaryBitmap(binarizer->createBinarizer(normalizedSource)));
    try {
      return decodeOnce(normalizedBitmap, hints);
    } catch (ReaderException const&) {
      continue;
    }
  }
  throw primary;
}

Ref<Result> QRCodeReader::decode(Ref<BinaryBitmap> image, DecodeHints hints) {
  try {
    return decodeOnce(image, hints);
  } catch (ReaderException const& primary) {
    return decodeNormalized(image, hints, primary);
  }
}

}
}
