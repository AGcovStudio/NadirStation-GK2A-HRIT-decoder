// bcjr.h - log-MAP (BCJR) decoder for the CCSDS rate-1/2 K=7 code, for the GK-2A HRIT rescue path.
// No Qt in here.
//
// The streaming Viterbi decoder answers "which bits", the rescue path also needs "how sure": a
// posterior log-likelihood ratio for every information bit. With those, the Reed-Solomon stage can
// declare the least certain bytes erasures (an erasure costs one parity symbol, an error two), and a
// codeword that is solved comes back as certain bits that make the neighbouring codewords easier.
// This runs over one frame (about 8200 pairs), so the full forward/backward sweep is affordable.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nadirstation::gk2a {

class BcjrK7 {
public:
    // soft: two signed values per pair, positive = symbol 0 (the rescue path's +-127 scale).
    // prior: per pair, -1 unknown, 0/1 known information bit (may be null).
    // gain: LLR per unit of soft value. post receives ln P(bit = 1) / P(bit = 0) per pair.
    // g1, g2: generator polynomials, first output symbol in bit 0 of the output pair.
    static void run(const std::int8_t* soft, std::size_t pairs, const std::int8_t* prior, float gain,
                    int g1, int g2, std::vector<float>& post) {
        constexpr int S = 64;
        std::uint8_t outs[128];
        for (int r = 0; r < 128; ++r)
            outs[r] = static_cast<std::uint8_t>((__builtin_popcount(r & g1) & 1) | ((__builtin_popcount(r & g2) & 1) << 1));
        const auto& corr = correction();
        auto mx = [&corr](float a, float b) {
            const float d = std::fabs(a - b);
            return std::max(a, b) + (d < 8.0f ? corr[static_cast<std::size_t>(d * 32.0f)] : 0.0f);
        };
        std::vector<float> alpha((pairs + 1) * S, -1e9f);
        for (int s = 0; s < S; ++s) alpha[static_cast<std::size_t>(s)] = 0.0f;
        constexpr float kKnown = 1000.0f;
        auto branch = [&](std::size_t i, int reg, int u) {
            const int o = outs[reg];
            const float l0 = gain * static_cast<float>(soft[2 * i]), l1 = gain * static_cast<float>(soft[2 * i + 1]);
            float g = 0.5f * (((o & 1) ? -l0 : l0) + ((o & 2) ? -l1 : l1));
            if (prior && prior[i] >= 0 && prior[i] != u) g -= kKnown;
            return g;
        };
        for (std::size_t i = 0; i < pairs; ++i) {
            const float* a = &alpha[i * S];
            float* an = &alpha[(i + 1) * S];
            float top = -1e30f;
            for (int ns = 0; ns < S; ++ns) {
                const int u = ns & 1, p0 = ns >> 1, p1 = p0 | 32;
                const float v = mx(a[p0] + branch(i, ((p0 << 1) | u) & 0x7F, u), a[p1] + branch(i, ((p1 << 1) | u) & 0x7F, u));
                an[ns] = v;
                top = std::max(top, v);
            }
            for (int s = 0; s < S; ++s) an[s] -= top;
        }
        post.assign(pairs, 0.0f);
        float beta[S] = {}, nb[S];
        for (std::size_t ii = pairs; ii-- > 0;) {
            const float* a = &alpha[ii * S];
            float acc0 = -1e30f, acc1 = -1e30f;
            bool f0 = false, f1 = false;
            float top = -1e30f;
            for (int ps = 0; ps < S; ++ps) {
                const int n0 = (ps << 1) & 63, n1 = ((ps << 1) | 1) & 63;
                const float b0 = branch(ii, (ps << 1) & 0x7F, 0) + beta[n0];
                const float b1 = branch(ii, ((ps << 1) | 1) & 0x7F, 1) + beta[n1];
                const float v0 = a[ps] + b0, v1 = a[ps] + b1;
                acc0 = f0 ? mx(acc0, v0) : v0; f0 = true;
                acc1 = f1 ? mx(acc1, v1) : v1; f1 = true;
                nb[ps] = mx(b0, b1);
                top = std::max(top, nb[ps]);
            }
            post[ii] = acc1 - acc0;
            for (int s = 0; s < S; ++s) beta[s] = nb[s] - top;
        }
    }

private:
    static const std::vector<float>& correction() {
        static const std::vector<float> t = [] {
            std::vector<float> v(256);
            for (int i = 0; i < 256; ++i) v[static_cast<std::size_t>(i)] = std::log1p(std::exp(-(static_cast<float>(i) + 0.5f) / 32.0f));
            return v;
        }();
        return t;
    }
};

}  // namespace nadirstation::gk2a
