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

bool isLeftDamagedFinder(zxing::ArrayRef<char> const& image, int width, int height, int comps,
                         Component const& component, int& moduleSize) {
  int w = component.right - component.left + 1;
  int h = component.bottom - component.top + 1;
  if (w < 30 || h < std::min(width, height) / 5 || h > height / 2) {
    return false;
  }

  moduleSize = (h + 3) / 7;
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

  // A finder with the left black bar missing still has top, right, bottom and center black areas.
  return blackFraction(image, width, height, comps,
             component.left, component.top, 6 * moduleSize, moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left + 5 * moduleSize, component.top, moduleSize, 7 * moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left, component.top + 6 * moduleSize, 6 * moduleSize, moduleSize) > 0.65f &&
         blackFraction(image, width, height, comps,
             component.left + moduleSize, component.top + 2 * moduleSize, 3 * moduleSize, 3 * moduleSize) > 0.65f;
}

void repairLeftDamagedFinderPatterns(zxing::ArrayRef<char>& image, int width, int height, int comps) {
  std::vector<unsigned char> visited(width * height, 0);
  std::vector<int> stack;

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

      int moduleSize = 0;
      if (isLeftDamagedFinder(image, width, height, comps, component, moduleSize)) {
        drawFinderPattern(image, width, height, comps,
            component.left - moduleSize, component.top, moduleSize);
      }
    }
  }
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

  repairLeftDamagedFinderPatterns(image, width, height, comps);

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
