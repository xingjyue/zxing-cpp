#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

#include "../cli/src/ImageReaderSource.h"
#include <zxing/qrcode/QRGridNormalizer.h>
#include "../cli/src/lodepng.h"

namespace {

bool rejectAllocation = false;

std::string WriteBlankPng() {
  char path[] = "/tmp/zxing-task4-XXXXXX.png";
  const int descriptor = mkstemps(path, 4);
  assert(descriptor >= 0);
  close(descriptor);
  const std::vector<unsigned char> rgba(4, 255);
  assert(lodepng::encode(std::string(path), rgba, 1, 1) == 0);
  return std::string(path);
}

bool EqualMatrices(
    const zxing::Ref<zxing::LuminanceSource>& left,
    const zxing::Ref<zxing::LuminanceSource>& right) {
  const zxing::ArrayRef<char> leftMatrix = left->getMatrix();
  const zxing::ArrayRef<char> rightMatrix = right->getMatrix();
  if (leftMatrix->size() != rightMatrix->size()) {
    return false;
  }
  for (int i = 0; i < leftMatrix->size(); ++i) {
    if (leftMatrix[i] != rightMatrix[i]) {
      return false;
    }
  }
  return true;
}

}

void* operator new(std::size_t size) {
  if (rejectAllocation) {
    throw std::bad_alloc();
  }
  void* allocation = std::malloc(size);
  if (allocation == NULL) {
    throw std::bad_alloc();
  }
  return allocation;
}

void* operator new[](std::size_t size) {
  return operator new(size);
}

void operator delete(void* allocation) throw() {
  std::free(allocation);
}

void operator delete[](void* allocation) throw() {
  std::free(allocation);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string filename(argv[1]);
  zxing::Ref<zxing::LuminanceSource> compatible =
      ImageReaderSource::create(filename);
  zxing::Ref<zxing::LuminanceSource> original =
      ImageReaderSource::create(filename, false);
  zxing::Ref<zxing::LuminanceSource> repaired =
      ImageReaderSource::create(filename, true);
  zxing::Ref<ImageReaderSource> loaded =
      ImageReaderSource::createLoaded(filename);
  assert(compatible->getWidth() == loaded->getWidth());
  assert(compatible->getHeight() == loaded->getHeight());
  assert(original->getWidth() == compatible->getWidth());
  assert(original->getHeight() == compatible->getHeight());

  const zxing::ArrayRef<char> before = loaded->getMatrix();
  const std::vector<zxing::Ref<zxing::LuminanceSource> > candidates =
      loaded->createNormalizedCandidates(99);
  const zxing::qrcode::MutableImage raw = {
      before, loaded->getWidth(), loaded->getHeight(), 1};
  const std::vector<zxing::qrcode::NormalizedImage> normalized =
      zxing::qrcode::NormalizeQR(raw, 99);
  const zxing::ArrayRef<char> after = loaded->getMatrix();
  assert(!candidates.empty() && candidates.size() <= 3);
  assert(!normalized.empty() && normalized.size() <= 3);
  assert(repaired->getWidth() == candidates[0]->getWidth());
  assert(repaired->getHeight() == candidates[0]->getHeight());
  assert(loaded->createNormalizedCandidates(0).empty());
  assert(loaded->createNormalizedCandidates(-1).empty());
  assert(before->size() == after->size());
  for (int i = 0; i < before->size(); ++i) {
    assert(before[i] == after[i]);
  }

  zxing::ArrayRef<char> onePixel(4);
  ImageReaderSource oversized(onePixel, 4097, 2049, 4);
  rejectAllocation = true;
  assert(oversized.createNormalizedCandidates(3).empty());
  rejectAllocation = false;

  zxing::ArrayRef<char> allocationPixels(64 * 64 * 4);
  ImageReaderSource allocationFailure(allocationPixels, 64, 64, 4);
  rejectAllocation = true;
  assert(allocationFailure.createNormalizedCandidates(3).empty());
  rejectAllocation = false;

  zxing::ArrayRef<char> budgetPixels(3200 * 2000 * 4);
  const zxing::qrcode::MutableImage budgetImage = {
      budgetPixels, 3200, 2000, 4};
  // Former CanNormalizeQR(..., true) reject path is now inside NormalizeQR.
  assert(zxing::qrcode::NormalizeQR(budgetImage, 3).empty());
  ImageReaderSource overBudget(budgetPixels, 3200, 2000, 4);
  rejectAllocation = true;
  assert(overBudget.createNormalizedCandidates(3).empty());
  rejectAllocation = false;

  const std::string blank = WriteBlankPng();
  const zxing::Ref<zxing::LuminanceSource> blankOriginal =
      ImageReaderSource::create(blank, false);
  const zxing::Ref<zxing::LuminanceSource> blankRepaired =
      ImageReaderSource::create(blank, true);
  assert(EqualMatrices(blankOriginal, blankRepaired));
  assert(std::remove(blank.c_str()) == 0);
  return 0;
}
