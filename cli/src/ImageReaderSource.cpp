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
#include <zxing/qrcode/QRGridNormalizer.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <vector>

#include "jpgd.h"
#include "lodepng.h"

using std::ostringstream;
using std::string;
using zxing::ArrayRef;
using zxing::LuminanceSource;
using zxing::Ref;

namespace {

struct LoadedImage {
  ArrayRef<char> pixels;
  int width;
  int height;
  int components;
};

struct FreeBuffer {
  void operator()(unsigned char* buffer) const {
    std::free(buffer);
  }
};

void ThrowLoadError(const string& filename) {
  ostringstream message;
  message << "Loading \"" << filename << "\" failed.";
  throw zxing::IllegalArgumentException(message.str().c_str());
}

int CheckedPixelBytes(size_t width, size_t height) {
  if (width == 0 || height == 0) {
    return 0;
  }
  size_t maxInt = static_cast<size_t>(std::numeric_limits<int>::max());
  size_t maxSize = std::numeric_limits<size_t>::max();
  if (width > maxInt || height > maxInt || height > maxSize / width) {
    throw zxing::IllegalArgumentException("Image dimensions are too large.");
  }
  size_t pixelCount = width * height;
  if (pixelCount > maxInt / 4) {
    throw zxing::IllegalArgumentException("Image dimensions are too large.");
  }
  return static_cast<int>(pixelCount * 4);
}

LoadedImage LoadImage(const string& filename) {
  LoadedImage loaded = {ArrayRef<char>(), 0, 0, 4};
  string extension = filename.substr(filename.find_last_of(".") + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
      [](char value) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(value)));
      });

  if (extension == "png") {
    std::vector<unsigned char> decoded;
    unsigned width = 0;
    unsigned height = 0;
    unsigned error = lodepng::decode(decoded, width, height, filename);
    if (error) {
      ostringstream message;
      message << "Error while loading '" << lodepng_error_text(error) << "'";
      throw zxing::IllegalArgumentException(message.str().c_str());
    }
    int byteCount = CheckedPixelBytes(width, height);
    loaded.width = static_cast<int>(width);
    loaded.height = static_cast<int>(height);
    if (byteCount > 0 && decoded.size() == static_cast<size_t>(byteCount)) {
      loaded.pixels = ArrayRef<char>(byteCount);
      std::memcpy(&loaded.pixels[0], &decoded[0], byteCount);
    }
  } else if (extension == "jpg" || extension == "jpeg") {
    int actualComponents = 0;
    std::unique_ptr<unsigned char, FreeBuffer> buffer(
        jpgd::decompress_jpeg_image_from_file(
            filename.c_str(), &loaded.width, &loaded.height,
            &actualComponents, 4));
    int byteCount = loaded.width > 0 && loaded.height > 0 ?
        CheckedPixelBytes(
            static_cast<size_t>(loaded.width),
            static_cast<size_t>(loaded.height)) : 0;
    if (buffer && byteCount > 0) {
      loaded.pixels = ArrayRef<char>(
          reinterpret_cast<char*>(buffer.get()), byteCount);
    }
  }

  if (!loaded.pixels) {
    ThrowLoadError(filename);
  }
  return loaded;
}

Ref<ImageReaderSource> MakeImageSource(const LoadedImage& loaded) {
  return Ref<ImageReaderSource>(new ImageReaderSource(
      loaded.pixels, loaded.width, loaded.height, loaded.components));
}

Ref<LuminanceSource> MakeSource(const LoadedImage& loaded) {
  return MakeImageSource(loaded);
}

std::vector<Ref<LuminanceSource> > MakeNormalizedSources(
    const std::vector<zxing::qrcode::NormalizedImage>& normalized) {
  std::vector<Ref<LuminanceSource> > result;
  result.reserve(normalized.size());
  for (size_t i = 0; i < normalized.size(); ++i) {
    const LoadedImage candidate = {
        normalized[i].pixels, normalized[i].width,
        normalized[i].height, normalized[i].components};
    result.push_back(MakeSource(candidate));
  }
  return result;
}

}

inline char ImageReaderSource::convertPixel(char const* pixel_) const {
  unsigned char const* pixel = reinterpret_cast<unsigned char const*>(pixel_);
  if (comps == 1 || comps == 2) {
    // Gray or gray+alpha
    return pixel[0];
  } if (comps == 3 || comps == 4) {
    // Red, Green, Blue, (Alpha)
    // We assume 16 bit values here
    // 0x200 = 1<<9, half an lsb of the result to force rounding
    return static_cast<char>((306 * static_cast<int>(pixel[0]) +
        601 * static_cast<int>(pixel[1]) + 117 * static_cast<int>(pixel[2]) +
        0x200) >> 10);
  }
  throw zxing::IllegalArgumentException("Unexpected image depth");
}

ImageReaderSource::ImageReaderSource(
    ArrayRef<char> image_, int width, int height, int comps_)
    : Super(width, height), image(image_), comps(comps_) {}

Ref<LuminanceSource> ImageReaderSource::create(const string& filename) {
  return createLoaded(filename);
}

Ref<LuminanceSource> ImageReaderSource::create(
    const string& filename, bool repairFixedPatterns) {
  if (!repairFixedPatterns) {
    return create(filename);
  }
  bool normalized = false;
  return createNormalized(filename, normalized);
}

Ref<ImageReaderSource> ImageReaderSource::createLoaded(
    const string& filename) {
  return MakeImageSource(LoadImage(filename));
}

std::vector<Ref<LuminanceSource> >
ImageReaderSource::createNormalizedCandidates(
    const string& filename, int maximumCandidates) {
  if (maximumCandidates <= 0) {
    return std::vector<Ref<LuminanceSource> >();
  }
  return createLoaded(filename)->createNormalizedCandidates(maximumCandidates);
}

Ref<LuminanceSource> ImageReaderSource::createNormalized(
    const string& filename, bool& normalized) {
  const Ref<ImageReaderSource> loaded = createLoaded(filename);
  const std::vector<Ref<LuminanceSource> > candidates =
      loaded->createNormalizedCandidates(1);
  normalized = !candidates.empty();
  if (normalized) {
    return candidates[0];
  }
  return loaded;
}

std::vector<Ref<LuminanceSource> >
ImageReaderSource::createNormalizedCandidates(int maximumCandidates) const {
  if (maximumCandidates <= 0) {
    return std::vector<Ref<LuminanceSource> >();
  }
  const zxing::qrcode::MutableImage source = {
      image, getWidth(), getHeight(), comps};
  try {
    return MakeNormalizedSources(
        zxing::qrcode::NormalizeQR(source, std::min(3, maximumCandidates)));
  } catch (const std::bad_alloc&) {
    return std::vector<Ref<LuminanceSource> >();
  }
}

ArrayRef<char> ImageReaderSource::getRow(int y, ArrayRef<char> row) const {
  const char* pixelRow = &image[0] + y * getWidth() * comps;
  if (!row) {
    row = ArrayRef<char>(getWidth());
  }
  for (int x = 0; x < getWidth(); x++) {
    row[x] = convertPixel(pixelRow + x * comps);
  }
  return row;
}

/** This is a more efficient implementation. */
ArrayRef<char> ImageReaderSource::getMatrix() const {
  const char* pixel = &image[0];
  ArrayRef<char> matrix(getWidth() * getHeight());
  char* output = &matrix[0];
  for (int y = 0; y < getHeight(); y++) {
    for (int x = 0; x < getWidth(); x++) {
      *output++ = convertPixel(pixel);
      pixel += comps;
    }
  }
  return matrix;
}
