#include <cassert>
#include <vector>

#include "../core/src/zxing/qrcode/QRGridNormalizer.cpp"

namespace {

zxing::qrcode::GridCandidate Candidate(float score, float left) {
  zxing::qrcode::GridCandidate candidate = {
      {{left, 10}, {left + 20, 10}, {left + 20, 30}, {left, 30}},
      score, 1, 0};
  return candidate;
}

bool Contains(
    const std::vector<zxing::qrcode::GridCandidate>& candidates,
    const zxing::qrcode::GridCandidate& expected) {
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (zxing::qrcode::EquivalentGrid(candidates[i], expected)) {
      return true;
    }
  }
  return false;
}

}

int main() {
  std::vector<zxing::qrcode::GridCandidate> local;
  for (int i = 0; i < 12; ++i) {
    local.push_back(Candidate(1.0f - 0.01f * i, 2.0f * i));
  }
  std::vector<zxing::qrcode::GridCandidate> global;
  zxing::qrcode::MergeCoarseCandidates(global, local);
  assert(global.size() == 8);

  std::vector<zxing::qrcode::GridCandidate> paper(
      1, Candidate(0.60f, 100.0f));
  std::vector<zxing::qrcode::GridCandidate> model(
      1, Candidate(0.70f, 120.0f));
  const std::vector<zxing::qrcode::GridCandidate> fair =
      zxing::qrcode::FairCoarseCandidates(global, paper, model);
  assert(fair.size() == 8);
  assert(Contains(fair, paper[0]));
  assert(Contains(fair, model[0]));
  return 0;
}
