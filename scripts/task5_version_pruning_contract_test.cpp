#include <cassert>
#include <vector>

#include "../core/src/zxing/qrcode/QRGridNormalizer.cpp"

namespace {

void AssertAllVersions(
    const zxing::qrcode::ModuleEstimate& estimate, zxing::qrcode::Bounds bounds) {
  const std::vector<int> versions =
      zxing::qrcode::CandidateVersions(estimate, bounds);
  assert(versions.size() == 10);
  for (int version = 1; version <= 10; ++version) {
    assert(versions[version - 1] == version);
  }
}

}

int main() {
  const zxing::qrcode::Bounds regions[] = {
      {0, 0, 21, 21}, {17, 23, 137, 143}, {50, 40, 450, 440}};
  const zxing::qrcode::ModuleEstimate estimates[] = {
      {0, 0, false}, {18, 1, true}, {4, 3, true}};
  for (size_t region = 0; region < 3; ++region) {
    for (size_t estimate = 0; estimate < 3; ++estimate) {
      AssertAllVersions(estimates[estimate], regions[region]);
    }
  }
  return 0;
}
