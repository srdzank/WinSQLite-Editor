#include "Projection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <random>

namespace vec {
namespace {

double dot(const std::vector<double>& a, const std::vector<double>& b)
{
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
}

bool normalize(std::vector<double>& v)
{
    const double n = std::sqrt(dot(v, v));
    if (n < 1e-12) return false;
    for (double& x : v) x /= n;
    return true;
}

} // namespace

std::vector<Vec3> pcaTo3D(std::span<const Embedding> items, int iterations)
{
    const std::size_t n = items.size();
    if (n == 0) return {};

    std::vector<Vec3> out(n);
    const std::size_t d = items.front().values.size();
    if (d == 0 || n == 1) return out;
    if (!std::ranges::all_of(items, [d](const Embedding& e) { return e.values.size() == d; }))
        return out; // mixed dimensionality: caller should filter first

    // 1) mean vector
    std::vector<double> mean(d, 0.0);
    for (const Embedding& e : items)
        for (std::size_t j = 0; j < d; ++j) mean[j] += e.values[j];
    for (double& m : mean) m /= double(n);

    // 2) top components of X^T X without ever building the d x d covariance matrix
    constexpr std::size_t kMaxComp = 3;
    const std::size_t nComp = std::min(kMaxComp, d);
    std::array<std::vector<double>, kMaxComp> comp;

    std::mt19937 rng(42); // fixed seed -> stable layout between runs
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<double> scores(n), w(d);

    for (std::size_t c = 0; c < nComp; ++c) {
        std::vector<double> v(d);
        for (double& x : v) x = gauss(rng);
        if (!normalize(v)) break;

        for (int it = 0; it < iterations; ++it) {
            for (std::size_t i = 0; i < n; ++i) {            // scores = X v
                double s = 0.0;
                const auto& row = items[i].values;
                for (std::size_t j = 0; j < d; ++j) s += (row[j] - mean[j]) * v[j];
                scores[i] = s;
            }
            std::ranges::fill(w, 0.0);                       // w = X^T scores
            for (std::size_t i = 0; i < n; ++i) {
                const auto& row = items[i].values;
                for (std::size_t j = 0; j < d; ++j) w[j] += (row[j] - mean[j]) * scores[i];
            }
            for (std::size_t p = 0; p < c; ++p) {            // keep orthogonal to earlier PCs
                const double proj = dot(w, comp[p]);
                for (std::size_t j = 0; j < d; ++j) w[j] -= proj * comp[p][j];
            }
            if (!normalize(w)) break;
            v = w;
        }
        comp[c] = std::move(v);
    }

    // 3) project + uniform scale (uniform so cluster shapes are not distorted)
    float maxAbs = 0.f;
    for (std::size_t i = 0; i < n; ++i) {
        std::array<double, 3> r{};
        for (std::size_t c = 0; c < nComp; ++c) {
            if (comp[c].empty()) continue;
            double s = 0.0;
            for (std::size_t j = 0; j < d; ++j) s += (items[i].values[j] - mean[j]) * comp[c][j];
            r[c] = s;
        }
        out[i] = {float(r[0]), float(r[1]), float(r[2])};
        maxAbs = std::max({maxAbs, std::abs(out[i].x), std::abs(out[i].y), std::abs(out[i].z)});
    }
    if (maxAbs > 0.f)
        for (Vec3& p : out) { p.x /= maxAbs; p.y /= maxAbs; p.z /= maxAbs; }
    return out;
}

} // namespace vec
