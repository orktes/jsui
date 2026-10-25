// coverRects (render/painter.h): the few rects a GPU repaint of scattered
// damage paints, one scissored pass each. It keeps its pair costs between
// joins; the result must stay what the plain greedy join gives.
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "include/core/SkRegion.h"
#include "render/painter.h"
#include "test.h"

using namespace jsui;

namespace {

int64_t area(const SkIRect& r) { return int64_t(r.width()) * r.height(); }

// The greedy join as first written: every pair's cost recomputed per join.
std::vector<SkIRect> referenceCover(const SkRegion& damage) {
  constexpr size_t kMaxRects = 4, kMaxInput = 48;
  constexpr int64_t kCheapJoin = 64 * 64;
  std::vector<SkIRect> rects;
  for (SkRegion::Iterator it(damage); !it.done(); it.next()) {
    if (rects.size() == kMaxInput) return {damage.getBounds()};
    rects.push_back(it.rect());
  }
  while (rects.size() > 1) {
    size_t bi = 0, bj = 1;
    int64_t best = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < rects.size(); i++)
      for (size_t j = i + 1; j < rects.size(); j++) {
        SkIRect u = rects[i];
        u.join(rects[j]);
        int64_t cost = area(u) - area(rects[i]) - area(rects[j]);
        if (cost < best) best = cost, bi = i, bj = j;
      }
    if (rects.size() <= kMaxRects && best > kCheapJoin) break;
    rects[bi].join(rects[bj]);
    rects.erase(rects.begin() + long(bj));
  }
  int64_t sum = 0;
  for (const SkIRect& r : rects) sum += area(r);
  if (rects.size() > 1 && sum * 3 > area(damage.getBounds()) * 2) return {damage.getBounds()};
  return rects;
}

}  // namespace

TEST(cover_rects_matches_greedy_join) {
  std::mt19937 rng(7);
  for (int round = 0; round < 400; round++) {
    // Moving things: 1-30 small rects (old and new position of nameplates,
    // markers, ...) over a 1280x720 frame, some overlapping.
    SkRegion damage;
    const int count = 1 + int(rng() % 30);
    for (int i = 0; i < count; i++) {
      int x = int(rng() % 1200), y = int(rng() % 680);
      int w = 4 + int(rng() % (round % 3 == 0 ? 400 : 120)), h = 4 + int(rng() % 60);
      damage.op(SkIRect::MakeXYWH(x, y, w, h), SkRegion::kUnion_Op);
    }
    std::vector<SkIRect> want = referenceCover(damage), got = coverRects(damage);
    CHECK_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size() && i < want.size(); i++) CHECK(got[i] == want[i]);
  }
  CHECK(coverRects(SkRegion()).empty());
  SkRegion one(SkIRect::MakeXYWH(10, 20, 30, 40));
  CHECK_EQ(coverRects(one).size(), size_t(1));
}
