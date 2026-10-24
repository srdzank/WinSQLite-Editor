#pragma once
#include "VecDatabase.h"
#include <span>
#include <vector>

namespace vec {

struct Vec3 { float x = 0.f, y = 0.f, z = 0.f; };

// PCA to 3 dimensions (power iteration with deflation; no external dependencies).
// Output is centered and scaled uniformly into [-1, 1], index-aligned with `items`.
// Cost is O(iterations * n * d * 3) -- run it off the GUI thread for large tables.
std::vector<Vec3> pcaTo3D(std::span<const Embedding> items, int iterations = 30);

} // namespace vec
