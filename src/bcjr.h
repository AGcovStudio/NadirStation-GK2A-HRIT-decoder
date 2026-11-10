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
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NADIR_BCJR_SSE2 1
#else
#define NADIR_BCJR_SSE2 0
#endif

namespace nadirstation::gk2a {

class BcjrK7 {
    // Parity of a 7-bit value (no compiler builtin, so this builds the same everywhere).
    static int parity(int v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }
public:
    // soft: two signed values per pair, positive = symbol 0 (the rescue path's +-127 scale).
    // prior: per pair, -1 unknown, 0/1 known information bit (may be null).
    // gain: LLR per unit of soft value. post receives ln P(bit = 1) / P(bit = 0) per pair.
    // g1, g2: generator polynomials, first output symbol in bit 0 of the output pair.
    static void run(const std::int8_t* soft, std::size_t pairs, const std::int8_t* prior, float gain,
                    int g1, int g2, std::vector<float>& post) {
#if NADIR_BCJR_SSE2
        // The fast path relies on both generators tapping the newest and the oldest register bit (true of
        // the CCSDS pair): the two branches into a state, and the two out of one, then carry opposite
        // symbols, so one metric per butterfly serves all four branches.
        if ((g1 & 1) && (g2 & 1) && (g1 & 64) && (g2 & 64)) { runSse2(soft, pairs, prior, gain, g1, g2, post); return; }
#endif
        runReference(soft, pairs, prior, gain, g1, g2, post);
    }

    // Plain scalar log-MAP with the exact correction term (tabulated). The reference for the fast path.
    static void runReference(const std::int8_t* soft, std::size_t pairs, const std::int8_t* prior, float gain,
                             int g1, int g2, std::vector<float>& post) {
        constexpr int S = 64;
        std::uint8_t outs[128];
        for (int r = 0; r < 128; ++r)
            outs[r] = static_cast<std::uint8_t>(parity(r & g1) | (parity(r & g2) << 1));
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

#if NADIR_BCJR_SSE2
private:
    // max*(a, b) = max(a, b) + ln(1 + e^-|a-b|), with the correction as the usual straight line
    // (linear-log-MAP: 0.24904 * (2.5068 - d), zero beyond). Within about 0.01 dB of the exact term,
    // and unlike a table it works on four states at a time.
    static inline __m128 mxv(__m128 a, __m128 b) {
        const __m128 d = _mm_andnot_ps(_mm_set1_ps(-0.0f), _mm_sub_ps(a, b));
        const __m128 c = _mm_max_ps(_mm_setzero_ps(), _mm_mul_ps(_mm_set1_ps(0.24904f), _mm_sub_ps(_mm_set1_ps(2.5068f), d)));
        return _mm_add_ps(_mm_max_ps(a, b), c);
    }
    static inline float mxs(float a, float b) {
        const float d = std::fabs(a - b);
        return std::max(a, b) + std::max(0.0f, 0.24904f * (2.5068f - d));
    }
    static inline float hmax(__m128 v) {
        v = _mm_max_ps(v, _mm_movehl_ps(v, v));
        v = _mm_max_ss(v, _mm_shuffle_ps(v, v, 1));
        return _mm_cvtss_f32(v);
    }
    static inline float hmxs(__m128 v) {
        alignas(16) float t[4];
        _mm_store_ps(t, v);
        return mxs(mxs(t[0], t[1]), mxs(t[2], t[3]));
    }

    // Same recursion as runReference, organised by butterfly. With p = 0..31, predecessors p and p+32
    // lead to states 2p and 2p+1, and with G[p] the metric of the branch p -> 2p:
    //   p -> 2p: +G    p -> 2p+1: -G    p+32 -> 2p: -G    p+32 -> 2p+1: +G
    static void runSse2(const std::int8_t* soft, std::size_t pairs, const std::int8_t* prior, float gain,
                        int g1, int g2, std::vector<float>& post) {
        constexpr int S = 64;
        constexpr float kKnown = 1000.0f;
        alignas(16) float s0[32], s1[32];   // sign of the first / second soft value in G[p]
        for (int p = 0; p < 32; ++p) {
            const int reg = p << 1;
            s0[p] = parity(reg & g1) ? -1.0f : 1.0f;
            s1[p] = parity(reg & g2) ? -1.0f : 1.0f;
        }
        static thread_local std::vector<float> alphaStore;   // one frame is 2 MB; kept between calls
        alphaStore.resize((pairs + 1) * S + 4);
        float* const alpha = alphaStore.data();
        for (int s = 0; s < S; ++s) alpha[s] = 0.0f;
        const float half = 0.5f * gain;
        const __m128 known = _mm_set1_ps(kKnown);
        for (std::size_t i = 0; i < pairs; ++i) {
            const float* a = alpha + i * S;
            float* an = alpha + (i + 1) * S;
            const __m128 h0 = _mm_set1_ps(half * static_cast<float>(soft[2 * i]));
            const __m128 h1 = _mm_set1_ps(half * static_cast<float>(soft[2 * i + 1]));
            const int pr = prior ? prior[i] : -1;
            __m128 top = _mm_set1_ps(-1e30f);
            for (int p = 0; p < 32; p += 4) {
                const __m128 g = _mm_add_ps(_mm_mul_ps(_mm_load_ps(s0 + p), h0), _mm_mul_ps(_mm_load_ps(s1 + p), h1));
                const __m128 lo = _mm_loadu_ps(a + p), hi = _mm_loadu_ps(a + p + 32);
                __m128 even = mxv(_mm_add_ps(lo, g), _mm_sub_ps(hi, g));   // into state 2p   (bit 0)
                __m128 odd = mxv(_mm_sub_ps(lo, g), _mm_add_ps(hi, g));    // into state 2p+1 (bit 1)
                if (pr == 0) odd = _mm_sub_ps(odd, known);
                else if (pr == 1) even = _mm_sub_ps(even, known);
                top = _mm_max_ps(top, _mm_max_ps(even, odd));
                _mm_storeu_ps(an + 2 * p, _mm_unpacklo_ps(even, odd));
                _mm_storeu_ps(an + 2 * p + 4, _mm_unpackhi_ps(even, odd));
            }
            const __m128 t = _mm_set1_ps(hmax(top));
            for (int s = 0; s < S; s += 4) _mm_storeu_ps(an + s, _mm_sub_ps(_mm_loadu_ps(an + s), t));
        }
        post.assign(pairs, 0.0f);
        alignas(16) float beta[S] = {}, nb[S];
        for (std::size_t ii = pairs; ii-- > 0;) {
            const float* a = alpha + ii * S;
            const __m128 h0 = _mm_set1_ps(half * static_cast<float>(soft[2 * ii]));
            const __m128 h1 = _mm_set1_ps(half * static_cast<float>(soft[2 * ii + 1]));
            const int pr = prior ? prior[ii] : -1;
            __m128 acc0 = _mm_set1_ps(-1e30f), acc1 = acc0, top = acc0;
            bool first = true;
            for (int p = 0; p < 32; p += 4) {
                const __m128 g = _mm_add_ps(_mm_mul_ps(_mm_load_ps(s0 + p), h0), _mm_mul_ps(_mm_load_ps(s1 + p), h1));
                const __m128 x = _mm_load_ps(beta + 2 * p), y = _mm_load_ps(beta + 2 * p + 4);
                const __m128 bev = _mm_shuffle_ps(x, y, _MM_SHUFFLE(2, 0, 2, 0));   // beta[2p]
                const __m128 bod = _mm_shuffle_ps(x, y, _MM_SHUFFLE(3, 1, 3, 1));   // beta[2p+1]
                __m128 b0lo = _mm_add_ps(bev, g), b1lo = _mm_sub_ps(bod, g);        // out of state p
                __m128 b0hi = _mm_sub_ps(bev, g), b1hi = _mm_add_ps(bod, g);        // out of state p+32
                if (pr == 0) { b1lo = _mm_sub_ps(b1lo, known); b1hi = _mm_sub_ps(b1hi, known); }
                else if (pr == 1) { b0lo = _mm_sub_ps(b0lo, known); b0hi = _mm_sub_ps(b0hi, known); }
                const __m128 alo = _mm_loadu_ps(a + p), ahi = _mm_loadu_ps(a + p + 32);
                const __m128 v0 = mxv(_mm_add_ps(alo, b0lo), _mm_add_ps(ahi, b0hi));
                const __m128 v1 = mxv(_mm_add_ps(alo, b1lo), _mm_add_ps(ahi, b1hi));
                if (first) { acc0 = v0; acc1 = v1; first = false; }
                else { acc0 = mxv(acc0, v0); acc1 = mxv(acc1, v1); }
                const __m128 nlo = mxv(b0lo, b1lo), nhi = mxv(b0hi, b1hi);
                top = _mm_max_ps(top, _mm_max_ps(nlo, nhi));
                _mm_store_ps(nb + p, nlo);
                _mm_store_ps(nb + p + 32, nhi);
            }
            post[ii] = hmxs(acc1) - hmxs(acc0);
            const __m128 t = _mm_set1_ps(hmax(top));
            for (int s = 0; s < S; s += 4) _mm_store_ps(beta + s, _mm_sub_ps(_mm_load_ps(nb + s), t));
        }
    }
#endif

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
