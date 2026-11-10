// GK-2A LRIT demodulation and framing. See gk2adecoder.h for the specification references.
//
// This file was rebuilt after a patch script replaced the whole of the first anonymous namespace,
// which silently removed three helpers other functions depend on: popcount8, buildRandomiserByte
// and the GF(256) tables. The lesson recorded here is that a text patch anchored on "namespace {"
// is not anchored at all, so the helpers are now written out in full, in one place.

#include "gk2adecoder.h"
#include "bcjr.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace nadirstation::gk2a {
namespace {

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

inline int popcount8(std::uint8_t v) {
    int n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

// Branch metric. A soft value runs from 0 for a confident symbol 0 to 255 for a confident symbol
// 1, and the cost of a branch is the summed absolute difference between the expected symbols and
// what arrived.
//
// Worked example, encoder state 0 with an input bit of 1: reg = 1 and outputFor(1) = 0b11, so the
// expected symbols are (1, 1). A transmitted symbol of 1 arrives with a negative real part, which
// maps to 255 here, and both terms are then |255 - 255| = 0 -- the cheapest possible branch, as it
// has to be.
inline int branchCost(int expectedBits, int received0, int received1) {
    const int expected[2] = {expectedBits & 1, (expectedBits >> 1) & 1};
    const int received[2] = {received0, received1};
    int cost = 0;
    for (int i = 0; i < 2; ++i) {
        const int target = expected[i] ? kSoftBit1 : kSoftBit0;
        const int d = received[i] - target;
        cost += (d < 0) ? -d : d;
    }
    return cost;
}

// The two output symbols for a shifting window, first polynomial in bit 0. That packing is the
// order libcorrect's fill_table uses.
inline int outputFor(int reg) {
    const int b0 = popcount8(static_cast<std::uint8_t>(reg & kG1)) & 1;
    const int b1 = popcount8(static_cast<std::uint8_t>(reg & kG2)) & 1;
    return b0 | (b1 << 1);
}

// Convert one raw soft value into the scale the metric works on. The demodulator's own hard
// decision is real() < 0 ? 1 : 0, so a symbol of 1 arrives as a negative real part, and on this
// scale a larger number means symbol 1 was more likely: +1 -> 0, -1 -> 255.
inline int softToScale(float v) {
    const float clamped = std::clamp(v, -1.0f, 1.0f);
    // Positive means symbol 0, which is the HIGH end of the scale, matching an in + 127 mapping on
    // a signed byte. The earlier direction mapped +1 to 0 and so inverted every branch metric.
    return static_cast<int>(std::lround((1.0f + clamped) * 0.5f * 255.0f));
}

// ---------------------------------------------------------------------------------------------
// CCSDS randomiser, CCSDS 131.0-B-3 section 10.3
//
// h(x) = x^8 + x^7 + x^5 + x^3 + 1, register preset to all ones, repeating every 255 bytes. It is
// applied to everything after the sync marker, which is where the data link layer puts it.
// ---------------------------------------------------------------------------------------------

// The CCSDS pseudo-randomiser (CCSDS 131.0-B): an 8-stage shift register, all ones at the start of
// every frame, with the generator polynomial h(x) = x^8 + x^7 + x^5 + x^3 + 1. The bit sequence has
// period 255 and 255 is odd, so the byte stream repeats after 255 bytes; one period is generated
// here from the register, as the standard defines it, and a longer frame wraps around it.
constexpr std::array<std::uint8_t, 255> makeCcsdsPn() {
    std::array<std::uint8_t, 255> table{};
    unsigned reg = 0xFFu;   // stage 1 is bit 0, stage 8 is bit 7
    for (auto& byte : table) {
        unsigned value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value << 1) | ((reg >> 7) & 1u);            // the output is stage 8
            const unsigned feedback = (reg ^ (reg >> 2) ^ (reg >> 4) ^ (reg >> 7)) & 1u;
            reg = ((reg << 1) | feedback) & 0xFFu;
        }
        byte = static_cast<std::uint8_t>(value);
    }
    return table;
}
constexpr std::array<std::uint8_t, 255> kCcsdsPn = makeCcsdsPn();
static_assert(kCcsdsPn[0] == 0xFF && kCcsdsPn[1] == 0x48 && kCcsdsPn[2] == 0x0E && kCcsdsPn[3] == 0xC0,
              "the CCSDS pseudo-random sequence starts FF 48 0E C0");

// The sequence is stored rather than generated. An earlier version produced it with
//
//     reg = 0xFF; feedback = ((reg >> 7) ^ (reg >> 6) ^ (reg >> 4) ^ (reg >> 2)) & 1;
//
// which is the shift register described in CCSDS 131.0-B-3 and yields ff 1a af 66 ... -- a
// sequence sharing only its first byte with the standard's. Every derandomisation in the chain
// therefore xored the wrong pattern, and the frames reached the packet layer scrambled. Deriving a
// sequence that contradicts the standard is a reason to use the standard's table, not to trust the
// derivation.
std::array<std::uint8_t, 255> buildRandomiserByte() {
    return kCcsdsPn;
}

// ---------------------------------------------------------------------------------------------
// GF(2^8) for the Reed-Solomon check
//
// CCSDS uses x^8 + x^7 + x^2 + x + 1, i.e. 0x187, with the (255,223) code. Only syndromes are
// computed: the check answers whether a CADU is intact, which decides whether its contents can be
// believed.
// ---------------------------------------------------------------------------------------------

struct Gf256 {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};

    Gf256() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            log[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x187;
        }
        for (int i = 255; i < 512; ++i)
            exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
        // log[0] is undefined for a logarithm; mul() tests for a zero operand and never reads it.
    }

    std::uint8_t mul(std::uint8_t a, std::uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return exp[static_cast<std::size_t>(log[a] + log[b])];
    }
};

constexpr std::array<std::uint8_t, 256> kFromDualBasis = {
    0x00, 0xCC, 0xAC, 0x60, 0x79, 0xB5, 0xD5, 0x19, 0xF0, 0x3C, 0x5C, 0x90, 0x89, 0x45, 0x25, 0xE9,
    0xFD, 0x31, 0x51, 0x9D, 0x84, 0x48, 0x28, 0xE4, 0x0D, 0xC1, 0xA1, 0x6D, 0x74, 0xB8, 0xD8, 0x14,
    0x2E, 0xE2, 0x82, 0x4E, 0x57, 0x9B, 0xFB, 0x37, 0xDE, 0x12, 0x72, 0xBE, 0xA7, 0x6B, 0x0B, 0xC7,
    0xD3, 0x1F, 0x7F, 0xB3, 0xAA, 0x66, 0x06, 0xCA, 0x23, 0xEF, 0x8F, 0x43, 0x5A, 0x96, 0xF6, 0x3A,
    0x42, 0x8E, 0xEE, 0x22, 0x3B, 0xF7, 0x97, 0x5B, 0xB2, 0x7E, 0x1E, 0xD2, 0xCB, 0x07, 0x67, 0xAB,
    0xBF, 0x73, 0x13, 0xDF, 0xC6, 0x0A, 0x6A, 0xA6, 0x4F, 0x83, 0xE3, 0x2F, 0x36, 0xFA, 0x9A, 0x56,
    0x6C, 0xA0, 0xC0, 0x0C, 0x15, 0xD9, 0xB9, 0x75, 0x9C, 0x50, 0x30, 0xFC, 0xE5, 0x29, 0x49, 0x85,
    0x91, 0x5D, 0x3D, 0xF1, 0xE8, 0x24, 0x44, 0x88, 0x61, 0xAD, 0xCD, 0x01, 0x18, 0xD4, 0xB4, 0x78,
    0xC5, 0x09, 0x69, 0xA5, 0xBC, 0x70, 0x10, 0xDC, 0x35, 0xF9, 0x99, 0x55, 0x4C, 0x80, 0xE0, 0x2C,
    0x38, 0xF4, 0x94, 0x58, 0x41, 0x8D, 0xED, 0x21, 0xC8, 0x04, 0x64, 0xA8, 0xB1, 0x7D, 0x1D, 0xD1,
    0xEB, 0x27, 0x47, 0x8B, 0x92, 0x5E, 0x3E, 0xF2, 0x1B, 0xD7, 0xB7, 0x7B, 0x62, 0xAE, 0xCE, 0x02,
    0x16, 0xDA, 0xBA, 0x76, 0x6F, 0xA3, 0xC3, 0x0F, 0xE6, 0x2A, 0x4A, 0x86, 0x9F, 0x53, 0x33, 0xFF,
    0x87, 0x4B, 0x2B, 0xE7, 0xFE, 0x32, 0x52, 0x9E, 0x77, 0xBB, 0xDB, 0x17, 0x0E, 0xC2, 0xA2, 0x6E,
    0x7A, 0xB6, 0xD6, 0x1A, 0x03, 0xCF, 0xAF, 0x63, 0x8A, 0x46, 0x26, 0xEA, 0xF3, 0x3F, 0x5F, 0x93,
    0xA9, 0x65, 0x05, 0xC9, 0xD0, 0x1C, 0x7C, 0xB0, 0x59, 0x95, 0xF5, 0x39, 0x20, 0xEC, 0x8C, 0x40,
    0x54, 0x98, 0xF8, 0x34, 0x2D, 0xE1, 0x81, 0x4D, 0xA4, 0x68, 0x08, 0xC4, 0xDD, 0x11, 0x71, 0xBD,
};

constexpr std::array<std::uint8_t, 256> kToDualBasis = {
    0x00, 0x7B, 0xAF, 0xD4, 0x99, 0xE2, 0x36, 0x4D, 0xFA, 0x81, 0x55, 0x2E, 0x63, 0x18, 0xCC, 0xB7,
    0x86, 0xFD, 0x29, 0x52, 0x1F, 0x64, 0xB0, 0xCB, 0x7C, 0x07, 0xD3, 0xA8, 0xE5, 0x9E, 0x4A, 0x31,
    0xEC, 0x97, 0x43, 0x38, 0x75, 0x0E, 0xDA, 0xA1, 0x16, 0x6D, 0xB9, 0xC2, 0x8F, 0xF4, 0x20, 0x5B,
    0x6A, 0x11, 0xC5, 0xBE, 0xF3, 0x88, 0x5C, 0x27, 0x90, 0xEB, 0x3F, 0x44, 0x09, 0x72, 0xA6, 0xDD,
    0xEF, 0x94, 0x40, 0x3B, 0x76, 0x0D, 0xD9, 0xA2, 0x15, 0x6E, 0xBA, 0xC1, 0x8C, 0xF7, 0x23, 0x58,
    0x69, 0x12, 0xC6, 0xBD, 0xF0, 0x8B, 0x5F, 0x24, 0x93, 0xE8, 0x3C, 0x47, 0x0A, 0x71, 0xA5, 0xDE,
    0x03, 0x78, 0xAC, 0xD7, 0x9A, 0xE1, 0x35, 0x4E, 0xF9, 0x82, 0x56, 0x2D, 0x60, 0x1B, 0xCF, 0xB4,
    0x85, 0xFE, 0x2A, 0x51, 0x1C, 0x67, 0xB3, 0xC8, 0x7F, 0x04, 0xD0, 0xAB, 0xE6, 0x9D, 0x49, 0x32,
    0x8D, 0xF6, 0x22, 0x59, 0x14, 0x6F, 0xBB, 0xC0, 0x77, 0x0C, 0xD8, 0xA3, 0xEE, 0x95, 0x41, 0x3A,
    0x0B, 0x70, 0xA4, 0xDF, 0x92, 0xE9, 0x3D, 0x46, 0xF1, 0x8A, 0x5E, 0x25, 0x68, 0x13, 0xC7, 0xBC,
    0x61, 0x1A, 0xCE, 0xB5, 0xF8, 0x83, 0x57, 0x2C, 0x9B, 0xE0, 0x34, 0x4F, 0x02, 0x79, 0xAD, 0xD6,
    0xE7, 0x9C, 0x48, 0x33, 0x7E, 0x05, 0xD1, 0xAA, 0x1D, 0x66, 0xB2, 0xC9, 0x84, 0xFF, 0x2B, 0x50,
    0x62, 0x19, 0xCD, 0xB6, 0xFB, 0x80, 0x54, 0x2F, 0x98, 0xE3, 0x37, 0x4C, 0x01, 0x7A, 0xAE, 0xD5,
    0xE4, 0x9F, 0x4B, 0x30, 0x7D, 0x06, 0xD2, 0xA9, 0x1E, 0x65, 0xB1, 0xCA, 0x87, 0xFC, 0x28, 0x53,
    0x8E, 0xF5, 0x21, 0x5A, 0x17, 0x6C, 0xB8, 0xC3, 0x74, 0x0F, 0xDB, 0xA0, 0xED, 0x96, 0x42, 0x39,
    0x08, 0x73, 0xA7, 0xDC, 0x91, 0xEA, 0x3E, 0x45, 0xF2, 0x89, 0x5D, 0x26, 0x6B, 0x10, 0xC4, 0xBF,
};

const Gf256& gf() {
    static const Gf256 instance;
    return instance;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Randomiser access
// ---------------------------------------------------------------------------------------------

const std::vector<uint8_t>& LritDecoder::randomiserBits() {
    static const std::vector<uint8_t> byteSeq = [] {
        const auto a = buildRandomiserByte();
        return std::vector<uint8_t>(a.begin(), a.end());
    }();
    return byteSeq;
}

// ---------------------------------------------------------------------------------------------
// Viterbi
//
// The state IS the K-bit shift register, which is how libcorrect defines it: numstates = 1 <<
// order. That choice removes the possibility of the encoder and decoder disagreeing about what a
// state is, which is what broke three earlier attempts. One step is
//
//     reg = ((state << 1) | bit) & kRegisterMask        and        state_next = reg
//
// so the inverse is simply the low bits of reg, and no second representation exists to fall out
// of step.
// ---------------------------------------------------------------------------------------------

ViterbiDecoder::ViterbiDecoder(int tracebackDepth)
    : traceback_(std::max(5 * kConvolutionK, tracebackDepth)) {
    resetStream();
}

void ViterbiDecoder::resetStream() {
    std::fill(std::begin(metric_), std::end(metric_), std::int16_t{0});
    decisions_.clear();
    released_ = 0;
}

std::vector<uint8_t> ViterbiDecoder::encode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() * 2 + 2 * static_cast<std::size_t>(kConvolutionK + 1));
    int state = 0;
    const auto emit = [&out](int reg) {
        const int o = outputFor(reg);
        out.push_back(static_cast<std::uint8_t>(o & 1));
        out.push_back(static_cast<std::uint8_t>((o >> 1) & 1));
    };
    for (const std::uint8_t bit : bits) {
        state = ((state << 1) | (bit & 1)) & kRegisterMask;
        emit(state);
    }
    // Flush as libcorrect does: K+1 zero bits drive the register to a known state so the final
    // message bits are constrained. Without it the tail is unrecoverable.
    for (int i = 0; i < kConvolutionK + 1; ++i) {
        state = (state << 1) & kRegisterMask;
        emit(state);
    }
    return out;
}

namespace {

// Outputs of every seven-bit register, computed once.
struct OutputTable {
    std::array<std::uint8_t, 128> out{};
    OutputTable() { for (int r = 0; r < 128; ++r) out[static_cast<std::size_t>(r)] = static_cast<std::uint8_t>(outputFor(r)); }
};
const OutputTable& outputTable() { static const OutputTable t; return t; }

// A soft value on the decoder's signed scale: +127 a confident symbol 0, -127 a confident 1.
inline int softToSigned(float v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f));
}

}  // namespace

// One trellis step on the 64-state trellis. A branch scores +s for an expected 0 and -s for an
// expected 1, so the surviving path is the one that agrees best with the received symbols.
//
// Butterfly form: new states 2i and 2i+1 both come from old states i and i+32. Metrics are 16-bit
// and re-referenced to state 0 every step; their spread is bounded by the code's memory times the
// largest branch metric (about 7 * 508), far inside the 16-bit range. With SSE2 eight states go
// through one instruction, which is what makes 3 Mbit/s (GK-2A HRIT) run in real time.
namespace {
struct ButterflySigns {
    // For butterfly i: sign of s0 and s1 in the branch metric of each of the four transitions
    // (from i or i+32, with input bit 0 or 1). +1 when the expected symbol is 0.
    alignas(16) std::int16_t a0s0[32], a0s1[32], a1s0[32], a1s1[32];
    alignas(16) std::int16_t b0s0[32], b0s1[32], b1s0[32], b1s1[32];
    ButterflySigns() {
        const auto& t = outputTable().out;
        auto sg = [](int bit) { return static_cast<std::int16_t>(bit ? -1 : 1); };
        for (int i = 0; i < 32; ++i) {
            const int ra0 = (i << 1), ra1 = (i << 1) | 1, rb0 = ((i | 32) << 1), rb1 = ((i | 32) << 1) | 1;
            a0s0[i] = sg(t[ra0] & 1); a0s1[i] = sg(t[ra0] & 2);
            a1s0[i] = sg(t[ra1] & 1); a1s1[i] = sg(t[ra1] & 2);
            b0s0[i] = sg(t[rb0] & 1); b0s1[i] = sg(t[rb0] & 2);
            b1s0[i] = sg(t[rb1] & 1); b1s1[i] = sg(t[rb1] & 2);
        }
    }
};
const ButterflySigns& butterflySigns() { static const ButterflySigns b; return b; }
}  // namespace

void ViterbiDecoder::step(int s0, int s1, int bonus0, int bonus1) {
    const ButterflySigns& g = butterflySigns();
#if defined(__SSE2__)
    const __m128i v0 = _mm_set1_epi16(static_cast<short>(s0));
    const __m128i v1 = _mm_set1_epi16(static_cast<short>(s1));
    const __m128i vb0 = _mm_set1_epi16(static_cast<short>(bonus0));
    const __m128i vb1 = _mm_set1_epi16(static_cast<short>(bonus1));
    alignas(16) std::int16_t next[64];
    std::uint64_t word = 0;
    for (int k = 0; k < 4; ++k) {
        const int o = 8 * k;
        auto ld = [o](const std::int16_t* p) { return _mm_load_si128(reinterpret_cast<const __m128i*>(p + o)); };
        const __m128i a = ld(metric_);
        const __m128i b = ld(metric_ + 32);
        const __m128i bmA0 = _mm_add_epi16(_mm_mullo_epi16(v0, ld(g.a0s0)), _mm_mullo_epi16(v1, ld(g.a0s1)));
        const __m128i bmA1 = _mm_add_epi16(_mm_mullo_epi16(v0, ld(g.a1s0)), _mm_mullo_epi16(v1, ld(g.a1s1)));
        const __m128i bmB0 = _mm_add_epi16(_mm_mullo_epi16(v0, ld(g.b0s0)), _mm_mullo_epi16(v1, ld(g.b0s1)));
        const __m128i bmB1 = _mm_add_epi16(_mm_mullo_epi16(v0, ld(g.b1s0)), _mm_mullo_epi16(v1, ld(g.b1s1)));
        const __m128i x0 = _mm_add_epi16(a, bmA0), y0 = _mm_add_epi16(b, bmB0);
        const __m128i x1 = _mm_add_epi16(a, bmA1), y1 = _mm_add_epi16(b, bmB1);
        const __m128i even = _mm_add_epi16(_mm_max_epi16(x0, y0), vb0), odd = _mm_add_epi16(_mm_max_epi16(x1, y1), vb1);
        const __m128i de = _mm_cmpgt_epi16(y0, x0), dd = _mm_cmpgt_epi16(y1, x1);
        // interleave: state 2i from `even`, 2i+1 from `odd`
        _mm_store_si128(reinterpret_cast<__m128i*>(next + 2 * o), _mm_unpacklo_epi16(even, odd));
        _mm_store_si128(reinterpret_cast<__m128i*>(next + 2 * o + 8), _mm_unpackhi_epi16(even, odd));
        const __m128i dlo = _mm_unpacklo_epi16(de, dd), dhi = _mm_unpackhi_epi16(de, dd);
        const std::uint64_t bits16 = static_cast<std::uint16_t>(_mm_movemask_epi8(_mm_packs_epi16(dlo, dhi)));
        word |= bits16 << (16 * k);
    }
    const __m128i ref = _mm_set1_epi16(next[0]);
    for (int k = 0; k < 8; ++k) {
        const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(next + 8 * k));
        _mm_store_si128(reinterpret_cast<__m128i*>(metric_ + 8 * k), _mm_sub_epi16(v, ref));
    }
#else
    std::int16_t next[64];
    std::uint64_t word = 0;
    for (int i = 0; i < 32; ++i) {
        const int a = metric_[i], b = metric_[i + 32];
        const int x0 = a + s0 * g.a0s0[i] + s1 * g.a0s1[i], y0 = b + s0 * g.b0s0[i] + s1 * g.b0s1[i];
        const int x1 = a + s0 * g.a1s0[i] + s1 * g.a1s1[i], y1 = b + s0 * g.b1s0[i] + s1 * g.b1s1[i];
        next[2 * i] = static_cast<std::int16_t>(std::max(x0, y0) + bonus0);
        next[2 * i + 1] = static_cast<std::int16_t>(std::max(x1, y1) + bonus1);
        if (y0 > x0) word |= std::uint64_t{1} << (2 * i);
        if (y1 > x1) word |= std::uint64_t{1} << (2 * i + 1);
    }
    const std::int16_t ref = next[0];
    for (int ns = 0; ns < 64; ++ns) metric_[ns] = static_cast<std::int16_t>(next[ns] - ref);
#endif
    decisions_.push_back(word);
}

// Trace back from the best state now and emit the decided bits of every step that has at least
// `keep` later steps behind it (keep = 0 emits everything). Emitted decisions are dropped.
void ViterbiDecoder::traceback(std::size_t keep, std::vector<uint8_t>& out) {
    const std::size_t total = decisions_.size();
    if (total <= released_ + keep) return;
    const std::size_t emitEnd = total - keep;          // emit steps [released_, emitEnd)
    int state = 0;
    for (int s = 1; s < 64; ++s)
        if (metric_[s] > metric_[state]) state = s;
    const std::size_t first = out.size();
    out.resize(first + (emitEnd - released_));
    for (std::size_t t = total; t-- > released_;) {
        if (t < emitEnd) out[first + (t - released_)] = static_cast<std::uint8_t>(state & 1);
        const int oldest = static_cast<int>((decisions_[t] >> state) & 1u);
        state = (state >> 1) | (oldest << 5);
    }
    released_ = emitEnd;
    // Drop what has been emitted once it is a sizeable prefix, so memory stays bounded.
    if (released_ > 65536) {
        decisions_.erase(decisions_.begin(), decisions_.begin() + static_cast<std::ptrdiff_t>(released_));
        released_ = 0;
    }
}

std::vector<uint8_t> ViterbiDecoder::decodeStreaming(const std::vector<float>& softBits,
                                                       bool resetAfter) {
    std::vector<uint8_t> out;
    const std::size_t pairs = softBits.size() / 2;
    out.reserve(pairs + static_cast<std::size_t>(traceback_));
    for (std::size_t i = 0; i < pairs; ++i) {
        const int s0 = softToSigned(softBits[2 * i]);
        const int s1 = softToSigned(softBits[2 * i + 1]);
        step(s0, s1);
        // Bound the decision store inside very long blocks too.
        if (decisions_.size() - released_ > static_cast<std::size_t>(traceback_) + 32768)
            traceback(static_cast<std::size_t>(traceback_), out);
    }
    if (resetAfter) {
        traceback(0, out);
        resetStream();
    } else {
        traceback(static_cast<std::size_t>(traceback_), out);
    }
    return out;
}

std::vector<uint8_t> ViterbiDecoder::decodeBlockWithPrior(const std::int8_t* soft, std::size_t pairs,
                                                          const std::int8_t* prior, int bonus) {
    resetStream();
    std::vector<uint8_t> out;
    out.reserve(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        int b0 = 0, b1 = 0;
        if (prior && prior[i] >= 0) { b0 = prior[i] == 0 ? bonus : -bonus; b1 = -b0; }
        step(soft[2 * i], soft[2 * i + 1], b0, b1);
    }
    traceback(0, out);
    resetStream();
    return out;
}

std::vector<uint8_t> ViterbiDecoder::flush() {
    std::vector<uint8_t> out;
    traceback(0, out);
    decisions_.clear();
    released_ = 0;
    return out;
}

std::vector<uint8_t> ViterbiDecoder::decodePartial(const std::vector<float>& softBits) {
    return decodeStreaming(softBits, false);
}

std::vector<uint8_t> ViterbiDecoder::decodeFinal(const std::vector<float>& softBits) {
    return decodeStreaming(softBits, true);
}

std::vector<uint8_t> ViterbiDecoder::decode(const std::vector<float>& softBits) {
    // One whole transmission from an empty encoder register: state 0 is the only possible start.
    resetStream();
    constexpr std::int16_t kUnreachable = -8000;
    std::fill(std::begin(metric_), std::end(metric_), kUnreachable);
    metric_[0] = 0;
    std::vector<uint8_t> out = decodeStreaming(softBits, true);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Reed-Solomon syndrome check
// ---------------------------------------------------------------------------------------------

// The 32 CCSDS roots are fixed. A table for multiplication by each root
// removes millions of GF logarithm lookups from the real-time HRIT path.
static const std::array<std::array<std::uint8_t,256>,32>& rsRootProducts() {
    static const auto table=[] {
        std::array<std::array<std::uint8_t,256>,32> result{};
        const auto& field=gf();
        for(int k=0;k<32;++k){
            const auto root=field.exp[static_cast<std::size_t>((11*(k+112))%255)];
            for(int value=0;value<256;++value)
                result[k][value]=field.mul(static_cast<std::uint8_t>(value),root);
        }
        return result;
    }();
    return table;
}

bool rsSyndromesZero(const std::vector<uint8_t>& cadu, int interleaveDepth) {
    if (cadu.size() < 4) return false;
    const std::size_t bodyBytes = cadu.size() - 4;      // everything after the sync marker
    if (interleaveDepth <= 0) interleaveDepth = 1;

    // The codewords are interleaved by stride, not laid out in blocks. the deinterleaver
    // reads output[ii] = data[ii * depth + pos] for exactly 255 bytes, so codeword `pos` is every
    // depth-th byte across the whole body. An earlier version walked the body with the same stride
    // but did not stop at 255, which made each codeword longer than the code and its syndromes
    // meaningless.
    static constexpr std::size_t kCodewordBytes = 255;
    if (interleaveDepth <= 0 || bodyBytes < kCodewordBytes) return false;

    // The caller has already undone the randomiser over the M_PDU, so these bytes are the ones
    // the transmitter encoded.
    const auto& products = rsRootProducts();
    // libcorrect: roots[k] = exp[(gap * (k + first_root)) % 255].
    //
    // The first root is multiplied by the gap as well as the stride, which is easy to miss: with
    // first_root 112 and gap 11 the sequence starts at alpha^212, not alpha^112, and the two root
    // sets have no element in common. Every syndrome computed under the wrong formula was the
    // syndrome of a different code.
    constexpr int kRoots = 32;

    for (int pos = 0; pos < interleaveDepth; ++pos) {
        std::vector<std::uint8_t> code;
        code.reserve(kCodewordBytes);
        for (std::size_t ii = 0; ii < kCodewordBytes; ++ii) {
            const std::size_t idx = ii * static_cast<std::size_t>(interleaveDepth)
                                    + static_cast<std::size_t>(pos);
            if (idx >= bodyBytes) return false;
            // CCSDS represents GF(256) elements in the dual basis. A syndrome computed on the
            // standard-basis representation is the syndrome of some other codeword, so the
            // mapping is not optional.
            code.push_back(kFromDualBasis[cadu[4 + idx]]);
        }
        const std::size_t n = code.size();
        for (int k = 0; k < kRoots; ++k) {
            std::uint8_t acc = 0;
            for (std::size_t i = 0; i < n; ++i)
                acc = static_cast<std::uint8_t>(products[k][acc] ^ code[i]);
            if (acc != 0) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Reed-Solomon correction
//
// The same code as the check above: RS(255,223), interleave 4, field 0x187, dual-basis symbols,
// roots beta^(112 + k) for k = 0..31 with beta = alpha^11. Each codeword is corrected on its own
// (Berlekamp-Massey, Chien search, Forney), up to 16 byte errors. The CADU is changed only when
// every codeword was corrected and the syndrome check then passes on the result, so a frame that
// is beyond the code's reach is left exactly as it arrived and still fails the check. This is done
// before the same check gates the packet layer.
// ---------------------------------------------------------------------------------------------

namespace {

constexpr int kRsFirstRoot = 112;
constexpr int kRsGap = 11;
constexpr int kRsParity = 32;
constexpr int kRsN = 255;

inline std::uint8_t betaPow(const Gf256& g, int e) {
    e %= 255;
    if (e < 0) e += 255;
    return g.exp[static_cast<std::size_t>((e * kRsGap) % 255)];
}
inline std::uint8_t gfInv(const Gf256& g, std::uint8_t x) { return g.exp[static_cast<std::size_t>(255 - g.log[x])]; }

// Corrects one codeword in the conventional basis, c[0] being the coefficient of x^254.
// Returns the number of symbols changed, or -1 when the errors are beyond the code.
bool rsSyndromes(const std::array<std::uint8_t, kRsN>& c, std::array<std::uint8_t, kRsParity>& s);

int rsCorrectCodeword(std::array<std::uint8_t, kRsN>& c) {
    const Gf256& g = gf();
    // S_k = c(beta^(112+k)), by Horner from the highest power.
    std::array<std::uint8_t, kRsParity> s{};
    if (rsSyndromes(c, s)) return 0;

    // Berlekamp-Massey: error locator lambda(x) = prod (1 - X_j x).
    std::array<std::uint8_t, kRsParity + 1> lambda{}, prev{}, tmp{};
    lambda[0] = 1;
    prev[0] = 1;
    int l = 0, m = 1;
    std::uint8_t b = 1;
    for (int n = 0; n < kRsParity; ++n) {
        std::uint8_t d = s[static_cast<std::size_t>(n)];
        for (int i = 1; i <= l; ++i) d ^= g.mul(lambda[static_cast<std::size_t>(i)], s[static_cast<std::size_t>(n - i)]);
        if (d == 0) { ++m; continue; }
        const std::uint8_t coef = g.mul(d, gfInv(g, b));
        tmp = lambda;
        for (int i = 0; i + m <= kRsParity; ++i)
            lambda[static_cast<std::size_t>(i + m)] ^= g.mul(coef, prev[static_cast<std::size_t>(i)]);
        if (2 * l <= n) {
            l = n + 1 - l;
            prev = tmp;
            b = d;
            m = 1;
        } else {
            ++m;
        }
    }
    if (l == 0 || l > kRsParity / 2) return -1;

    // Chien search over the 255 positions: position power j (x^j) is in error when lambda(beta^-j) = 0.
    std::array<int, kRsParity / 2> powers{};
    int found = 0;
    for (int j = 0; j < kRsN; ++j) {
        const std::uint8_t xinv = betaPow(g, -j);
        std::uint8_t v = 0, xp = 1;
        for (int i = 0; i <= l; ++i) {
            v ^= g.mul(lambda[static_cast<std::size_t>(i)], xp);
            xp = g.mul(xp, xinv);
        }
        if (v == 0) {
            if (found == kRsParity / 2) return -1;
            powers[static_cast<std::size_t>(found++)] = j;
        }
    }
    if (found != l) return -1;

    // Forney: omega(x) = S(x) * lambda(x) mod x^32; e_j = X_j^(1 - 112) * omega(X_j^-1) / lambda'(X_j^-1).
    std::array<std::uint8_t, kRsParity> omega{};
    for (int i = 0; i < kRsParity; ++i) {
        std::uint8_t acc = 0;
        for (int k = 0; k <= std::min(i, l); ++k)
            acc ^= g.mul(lambda[static_cast<std::size_t>(k)], s[static_cast<std::size_t>(i - k)]);
        omega[static_cast<std::size_t>(i)] = acc;
    }
    for (int e = 0; e < found; ++e) {
        const int j = powers[static_cast<std::size_t>(e)];
        const std::uint8_t xinv = betaPow(g, -j);
        std::uint8_t num = 0, xp = 1;
        for (int i = 0; i < kRsParity; ++i) {
            num ^= g.mul(omega[static_cast<std::size_t>(i)], xp);
            xp = g.mul(xp, xinv);
        }
        // Formal derivative in characteristic 2: only odd powers survive.
        std::uint8_t den = 0;
        xp = 1;   // xinv^(i-1) for odd i
        const std::uint8_t xinv2 = g.mul(xinv, xinv);
        for (int i = 1; i <= l; i += 2) {
            den ^= g.mul(lambda[static_cast<std::size_t>(i)], xp);
            xp = g.mul(xp, xinv2);
        }
        if (den == 0) return -1;
        const std::uint8_t scale = betaPow(g, j * (1 - kRsFirstRoot));
        const std::uint8_t magnitude = g.mul(g.mul(num, gfInv(g, den)), scale);
        c[static_cast<std::size_t>(kRsN - 1 - j)] ^= magnitude;
    }
    return found;
}


// Errors-and-erasures correction of one codeword. `eras` lists array indices (0 = x^254) whose
// bytes are doubtful. Works while 2 * (unknown errors) + erasures <= 32. Returns the number of
// symbols changed, or -1. The result is only accepted by the caller after a full syndrome check.
// Syndromes S_k = c(beta^(112+k)) of a received word. True when they are all zero (a codeword).
bool rsSyndromes(const std::array<std::uint8_t, kRsN>& c, std::array<std::uint8_t, kRsParity>& s) {
    const auto& products = rsRootProducts();
    // Horner for all 32 roots side by side. Each root's chain is 255 table reads that each wait for
    // the one before; run one root at a time that wait is the whole cost, run together the 32
    // chains overlap in the processor. Same arithmetic, several times the speed.
    s.fill(0);
    for (int i = 0; i < kRsN; ++i) {
        const std::uint8_t ci = c[static_cast<std::size_t>(i)];
        for (int k = 0; k < kRsParity; ++k)
            s[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(products[static_cast<std::size_t>(k)][s[static_cast<std::size_t>(k)]] ^ ci);
    }
    bool clean = true;
    for (int k = 0; k < kRsParity; ++k) if (s[static_cast<std::size_t>(k)] != 0) clean = false;
    return clean;
}

// Errors-and-erasures correction of a word whose (non-zero) syndromes are already known. The rescue
// tries many erasure sets on one received word; its syndromes do not depend on the set, so they are
// computed once by the caller instead of once per set.
int rsCorrectWithSyndromes(std::array<std::uint8_t, kRsN>& c, const std::array<std::uint8_t, kRsParity>& s,
                           const std::vector<int>& eras, int spare) {
    const Gf256& g = gf();
    const int f = static_cast<int>(eras.size());
    if (f > kRsParity) return -1;
    // Erasure locator: prod (1 - X_e x), X_e = beta^j, j = 254 - index.
    std::array<std::uint8_t, kRsParity + 1> lambda{}, prev{}, tmp{};
    lambda[0] = 1;
    for (int e = 0; e < f; ++e) {
        const std::uint8_t X = betaPow(g, kRsN - 1 - eras[static_cast<std::size_t>(e)]);
        for (int i = e + 1; i >= 1; --i)
            lambda[static_cast<std::size_t>(i)] ^= g.mul(X, lambda[static_cast<std::size_t>(i - 1)]);
    }
    prev = lambda;
    int l = f, m = 1;
    std::uint8_t b = 1;
    for (int n = f; n < kRsParity; ++n) {
        std::uint8_t d = 0;
        for (int i = 0; i <= l && i <= n; ++i) d ^= g.mul(lambda[static_cast<std::size_t>(i)], s[static_cast<std::size_t>(n - i)]);
        if (d == 0) { ++m; continue; }
        const std::uint8_t coef = g.mul(d, gfInv(g, b));
        tmp = lambda;
        for (int i = 0; i + m <= kRsParity; ++i)
            lambda[static_cast<std::size_t>(i + m)] ^= g.mul(coef, prev[static_cast<std::size_t>(i)]);
        if (2 * l <= n + f) {
            l = n + 1 - l + f;
            prev = tmp;
            b = d;
            m = 1;
        } else {
            ++m;
        }
    }
    // Spare redundancy: each unknown error costs two parity symbols and each erasure one. Leave
    // at least 8 unused, otherwise a wrong word is as likely to come out "corrected" as a right
    // one (at 32 erasures every word is a codeword). 8 spare symbols put a false accept near
    // 2^-64.
    if (l == 0 || l > kRsParity || (l - f) * 2 + f + spare > kRsParity) return -1;
    std::array<int, kRsParity> powers{};
    int found = 0;
    // Chien search, lambda(beta^-j) for every position j, in the log domain: the term lambda_i * x^i
    // is exp[log lambda_i + i * log x], and i * log x is kept as a running sum modulo 255. One
    // table read per term instead of two multiplications; the values are the same.
    std::array<int, kRsParity + 1> logLambda{};
    for (int i = 0; i <= l; ++i)
        logLambda[static_cast<std::size_t>(i)] = lambda[static_cast<std::size_t>(i)] ? g.log[lambda[static_cast<std::size_t>(i)]] : -1;
    for (int j = 0; j < kRsN; ++j) {
        const int logX = (((255 - j) % 255) * kRsGap) % 255;   // log of beta^-j
        std::uint8_t v = 0;
        int power = 0;
        for (int i = 0; i <= l; ++i) {
            const int ll = logLambda[static_cast<std::size_t>(i)];
            if (ll >= 0) v ^= g.exp[static_cast<std::size_t>(ll + power)];
            power += logX;
            if (power >= 255) power -= 255;
        }
        if (v == 0) {
            if (found == kRsParity) return -1;
            powers[static_cast<std::size_t>(found++)] = j;
        }
    }
    if (found != l) return -1;
    std::array<std::uint8_t, kRsParity> omega{};
    for (int i = 0; i < kRsParity; ++i) {
        std::uint8_t acc = 0;
        for (int k = 0; k <= std::min(i, l); ++k)
            acc ^= g.mul(lambda[static_cast<std::size_t>(k)], s[static_cast<std::size_t>(i - k)]);
        omega[static_cast<std::size_t>(i)] = acc;
    }
    for (int e = 0; e < found; ++e) {
        const int j = powers[static_cast<std::size_t>(e)];
        const std::uint8_t xinv = betaPow(g, -j);
        std::uint8_t num = 0, xp = 1;
        for (int i = 0; i < kRsParity; ++i) {
            num ^= g.mul(omega[static_cast<std::size_t>(i)], xp);
            xp = g.mul(xp, xinv);
        }
        std::uint8_t den = 0;
        xp = 1;
        const std::uint8_t xinv2 = g.mul(xinv, xinv);
        for (int i = 1; i <= l; i += 2) {
            den ^= g.mul(lambda[static_cast<std::size_t>(i)], xp);
            xp = g.mul(xp, xinv2);
        }
        if (den == 0) return -1;
        const std::uint8_t scale = betaPow(g, j * (1 - kRsFirstRoot));
        c[static_cast<std::size_t>(kRsN - 1 - j)] ^= g.mul(g.mul(num, gfInv(g, den)), scale);
    }
    return found;
}

int rsCorrectCodewordErasures(std::array<std::uint8_t, kRsN>& c, const std::vector<int>& eras, int spare = 8) {
    if (static_cast<int>(eras.size()) > kRsParity) return -1;
    std::array<std::uint8_t, kRsParity> s{};
    if (rsSyndromes(c, s)) return 0;
    return rsCorrectWithSyndromes(c, s, eras, spare);
}

}  // namespace

int rsCorrect(std::vector<uint8_t>& cadu, int interleaveDepth) {
    if (interleaveDepth <= 0 || cadu.size() < 4 + static_cast<std::size_t>(kRsN) * static_cast<std::size_t>(interleaveDepth))
        return -1;
    std::vector<uint8_t> work = cadu;
    int corrected = 0;
    for (int pos = 0; pos < interleaveDepth; ++pos) {
        std::array<std::uint8_t, kRsN> code{};
        for (int ii = 0; ii < kRsN; ++ii)
            code[static_cast<std::size_t>(ii)] = kFromDualBasis[work[4 + static_cast<std::size_t>(ii * interleaveDepth + pos)]];
        const int n = rsCorrectCodeword(code);
        if (n < 0) return -1;
        if (n == 0) continue;
        corrected += n;
        for (int ii = 0; ii < kRsN; ++ii)
            work[4 + static_cast<std::size_t>(ii * interleaveDepth + pos)] = kToDualBasis[code[static_cast<std::size_t>(ii)]];
    }
    if (corrected > 0 && !rsSyndromesZero(work, interleaveDepth)) return -1;
    cadu = std::move(work);
    return corrected;
}

// ---------------------------------------------------------------------------------------------
// Derandomisation
// ---------------------------------------------------------------------------------------------

void derandomise(std::vector<uint8_t>& frame) {
    if (frame.size() <= 4) return;
    const auto& seq = LritDecoder::randomiserBits();
    for (std::size_t i = 4; i < frame.size(); ++i)
        frame[i] = static_cast<std::uint8_t>(frame[i] ^ seq[(i - 4) % seq.size()]);
}

// ---------------------------------------------------------------------------------------------
// LritDecoder
// ---------------------------------------------------------------------------------------------

LritDecoder::LritDecoder() {
    stats_.vcidHistogram.assign(64, 0);
    reset();
}

void LritDecoder::reset() {
    viterbi_.resetStream();
    stats_ = DecodeStats{};
    bitBuffer_.clear();
    softBuf_.clear();
    consumed_ = 0;
    ready_.clear();
    // The half symbol held over from a feed that ended on an odd symbol count belongs to the
    // transmission that was being decoded. Keeping it across a reset pairs it with the first symbol
    // of the next transmission and shifts that stream's pairing by one symbol for its whole length,
    // which is the same silent failure the hold-over exists to prevent.
    pendingSoft_ = 0.0f;
    hasPending_ = false;
    symbolsSinceSync_ = 0;
    dropNext_ = false;
    locked_ = false;
    lockInverted_ = false;
    misses_ = 0;
    lastGoodEnd_ = 0;
    stats_.vcidHistogram.assign(64, 0);
}

void LritDecoder::feedSoftBits(const std::vector<float>& soft, bool final) {
    // The code has rate 1/2, so bit k is recovered from symbols 2k and 2k+1. A block that ends on
    // an odd symbol count therefore leaves one symbol without a partner, and it must be held over
    // rather than dropped: dropping it shifts every later pairing by one symbol relative to the
    // transmitter's, and the decoder then reads noise. That is what made the frame count swing
    // over a factor of six with nothing changed but the block size -- it was never the timing
    // loop, only whether each block happened to end even.
    //
    // Measured before the fix, on one span split into two calls:
    //     first call   1250137 symbols (odd)  ->  75 frames
    //     second call  1250136 symbols        ->   1 frame
    //     one call     2500273 symbols        -> 151 frames
    // A pairing shift requested below takes effect here, on the first symbol of this feed.
    std::vector<float> shifted;
    const std::vector<float>* fed = &soft;
    if (dropNext_ && !soft.empty()) {
        shifted.assign(soft.begin() + 1, soft.end());
        fed = &shifted;
        dropNext_ = false;
    }
    std::vector<float> aligned;
    const std::vector<float>* input = fed;
    if (hasPending_) {
        aligned.reserve(fed->size() + 1);
        aligned.push_back(pendingSoft_);
        aligned.insert(aligned.end(), fed->begin(), fed->end());
        pendingSoft_ = 0.0f;
        hasPending_ = false;
        input = &aligned;
    }
    if (input->size() % 2 != 0) {
        pendingSoft_ = input->back();
        hasPending_ = true;
        if (input == fed) {
            aligned.assign(fed->begin(), fed->end() - 1);
            input = &aligned;
        } else {
            aligned.pop_back();
        }
    }
    const std::vector<float>& symbols = *input;
    const std::size_t syncedBefore = stats_.cadusWithSync + stats_.flywheelRsOk;

    stats_.bitsIn += symbols.size() / 2;
    // One decoder for the whole stream, so the survivor metric and the decision history carry
    // from block to block. Bits come out kDecisionDelay pairs behind the symbols that produced
    // them; the final block flushes the rest. decode() must not be used here: it restarts from
    // state 0 on every call, so the bit stream would lose continuity at every seam.
    if (rescueEnabled_) {
        const std::size_t base = softBuf_.size();
        softBuf_.resize(base + symbols.size());
        for (std::size_t i = 0; i < symbols.size(); ++i) softBuf_[base + i] = static_cast<std::int8_t>(softToSigned(symbols[i]));
    }
    const std::vector<uint8_t> bits = viterbi_.decodeStreaming(symbols, final);
    stats_.softBitsUsed += symbols.size();
    if (final) { pendingSoft_ = 0.0f; hasPending_ = false; }   // a new transmission starts its own pairing
    if (final) viterbi_.resetStream();
    bitBuffer_.insert(bitBuffer_.end(), bits.begin(), bits.end());
    if (final && rescueEnabled_) softBuf_.resize(2 * bitBuffer_.size());   // the flush released every pending bit
    processPending();
    if (final) viterbi_.resetStream();

    // No marker and no Reed-Solomon-confirmed frame for three CADUs' worth of symbols: try the
    // other pairing.
    if (stats_.cadusWithSync + stats_.flywheelRsOk != syncedBefore) {
        symbolsSinceSync_ = 0;
    } else {
        symbolsSinceSync_ += fed->size();
        if (pairSlipEnabled_ && !final && symbolsSinceSync_ > kPairSlipSymbols) {
            symbolsSinceSync_ = 0;
            if (hasPending_) { pendingSoft_ = 0.0f; hasPending_ = false; }   // dropping the held symbol is the shift
            else dropNext_ = true;
            viterbi_.resetStream();
            if (rescueEnabled_) softBuf_.resize(2 * bitBuffer_.size());   // bits still held in the Viterbi delay were dropped
            // The bit stream restarts with the new pairing, so the frame clock means nothing now.
            locked_ = false;
            misses_ = 0;
            ++stats_.pairSlips;
        }
    }
}

void LritDecoder::feedHardBits(const std::vector<uint8_t>& bits) {
    stats_.bitsIn += bits.size();
    bitBuffer_.insert(bitBuffer_.end(), bits.begin(), bits.end());
    processPending();
}

void LritDecoder::flushDecoder() {
    const std::vector<uint8_t> bits = viterbi_.flush();
    viterbi_.resetStream();
    bitBuffer_.insert(bitBuffer_.end(), bits.begin(), bits.end());
    if (rescueEnabled_) softBuf_.resize(2 * bitBuffer_.size());   // the flush released every pending bit
    processPending();
}

int LritDecoder::asmErrorsAt(std::size_t bitOffset, bool* inverted) const {
    int normal = 0;
    for (int i = 0; i < kSyncBits; ++i) {
        const std::uint8_t bit = bitBuffer_[bitOffset + static_cast<std::size_t>(i)];
        const std::uint8_t want = static_cast<std::uint8_t>((kSyncWord >> (31 - i)) & 1u);
        normal += bit != want;
    }
    // A BPSK CCSDS stream can arrive with either polarity (the code maps a complemented symbol
    // stream onto the complemented bit stream), so the complemented marker counts as well.
    const int complemented = kSyncBits - normal;
    if (inverted) *inverted = complemented < normal;
    return std::min(normal, complemented);
}

// Worker threads for the frame rescue, shared by every decoder in the process and started on first
// use. One batch runs at a time; the calling thread takes jobs alongside the workers.
class RescuePool {
public:
    static RescuePool& instance() {
        static RescuePool pool;
        return pool;
    }
    // Calls fn(0) .. fn(n-1), on at most `threads` threads including the caller's; returns when all
    // have finished. fn must not throw.
    void run(int n, int threads, const std::function<void(int)>& fn) {
        if (n <= 0) return;
        threads = std::min(threads, n);
        if (threads <= 1) {
            for (int i = 0; i < n; ++i) fn(i);
            return;
        }
        std::lock_guard<std::mutex> batch(batchMutex_);
        {
            std::lock_guard<std::mutex> lock(m_);
            while (static_cast<int>(workers_.size()) < threads - 1) workers_.emplace_back([this] { work(); });
            fn_ = &fn;
            n_ = n;
            next_ = 0;
            pending_ = n;
            helpers_ = threads - 1;
            ++generation_;
        }
        wake_.notify_all();
        takeJobs();
        std::unique_lock<std::mutex> lock(m_);
        done_.wait(lock, [this] { return pending_ == 0 && active_ == 0; });
        fn_ = nullptr;
        helpers_ = 0;
    }
    ~RescuePool() {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : workers_) if (t.joinable()) t.join();
    }

private:
    void takeJobs() {
        for (;;) {
            int i;
            {
                std::lock_guard<std::mutex> lock(m_);
                if (next_ >= n_) return;
                i = next_++;
            }
            (*fn_)(i);
            {
                std::lock_guard<std::mutex> lock(m_);
                --pending_;
            }
            done_.notify_all();
        }
    }
    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_);
                wake_.wait(lock, [&] { return quit_ || (generation_ != seen && fn_ != nullptr && helpers_ > 0); });
                if (quit_) return;
                seen = generation_;
                --helpers_;
                ++active_;
            }
            takeJobs();
            {
                std::lock_guard<std::mutex> lock(m_);
                --active_;
            }
            done_.notify_all();
        }
    }
    std::mutex batchMutex_, m_;
    std::condition_variable wake_, done_;
    std::vector<std::thread> workers_;
    const std::function<void(int)>* fn_ = nullptr;
    int n_ = 0, next_ = 0, pending_ = 0, helpers_ = 0, active_ = 0;
    std::uint64_t generation_ = 0;
    bool quit_ = false;
};

int LritDecoder::defaultRescueThreads() {
    // Two cores are left for the demodulator and the interface.
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp<unsigned>(cores > 2 ? cores - 2 : 1, 1, 8));
}

// The part of a frame's decoding that depends only on the buffers: the bytes at this position,
// derandomised, and Reed-Solomon. No decoder state is read or written, so it can run on any thread
// while the buffers are not being changed.
LritDecoder::FrameBody LritDecoder::hardDecode(std::size_t bitOffset, bool inverted, int asmErrors) const {
    FrameBody f;
    f.valid = true;
    f.bitOffset = bitOffset;
    f.inverted = inverted;
    f.asmErrors = asmErrors;
    std::vector<std::uint8_t>& plain = f.plain;
    plain.resize(kCaduBytes);
    for (std::size_t b = 0; b < static_cast<std::size_t>(kCaduBytes); ++b) {
        std::uint8_t byte = 0;
        for (int i = 0; i < 8; ++i)
            byte = static_cast<std::uint8_t>(
                (byte << 1) | bitBuffer_[bitOffset + b * 8 + static_cast<std::size_t>(i)]);
        if (inverted) byte = static_cast<std::uint8_t>(~byte);
        plain[b] = byte;
    }
    // The marker bytes are rewritten to the marker itself: a frame accepted with marker bit
    // errors would otherwise carry them on to anything that re-checks bytes 0..3.
    plain[0] = 0x1A; plain[1] = 0xCF; plain[2] = 0xFC; plain[3] = 0x1D;
    // One derandomisation, over the range the link randomises, and every later reader uses the
    // result. Doing it separately for the check and for the packet layer is how the two ended up
    // describing different byte sequences.
    // Offset 4, covering the whole body after the sync marker. The FEC
    // stage derandomises everything after the marker (cadu[4..]) BEFORE the Reed-Solomon step, then writes the
    // derandomised frame out. So the bytes the packet layer reads are the same derandomised bytes
    // the parity was checked over, and there is exactly one derandomisation in the chain.
    //
    // An earlier version read the VCDU header out of the bytes as they arrived, on the theory that
    // the randomiser only covers the M_PDU from byte 10. It does not. Section 8 of the mission
    // specification puts the VCA sub-layer's steps in order -- Reed-Solomon, then data
    // randomization, then attachment of the sync marker -- so the randomiser covers the whole
    // 1020-byte AOS transfer frame and byte 4 of the CADU is its first randomised byte. The
    // VCDU is parsed only after derandomising, for the same reason.
    //
    // Reading the header before the randomiser is undone does not fail loudly; it reads six
    // randomised bytes as if they were fields. The sequence is FF 48 0E C0 9A 0D 70 BC, so a data
    // frame's vcid byte (VCID | 0x48) comes out with six low bits of 0x48 = 63 and a filler
    // frame's (VCID | 0x3F, VCID 63) comes out with six low bits of 0xC0 = 0: filler and data
    // swapped places exactly, which silently discarded every data frame and sent every filler
    // frame on to the packet reassembler.
    const auto& seq = LritDecoder::randomiserBits();
    for (std::size_t i = 4; i < plain.size(); ++i)
        plain[i] = static_cast<std::uint8_t>(plain[i] ^ seq[(i - 4) % seq.size()]);

    // Reed-Solomon correction before anything is read out of the frame, so the header fields come
    // from the corrected bytes. rsCorrect leaves a frame it cannot correct untouched, and that frame
    // then fails the check in assembleAt exactly as before.
    f.rsFixed = rsCorrect(plain, kRsInterleaveDepth);
    return f;
}

void LritDecoder::rescueInto(FrameBody& f, std::chrono::steady_clock::time_point deadline) const {
    f.rescueAttempted = true;
    std::vector<uint8_t> better = f.plain;
    if (rescueFrame(f.bitOffset, f.inverted, f.asmErrors, better, deadline)) {
        f.rescued = true;
        f.better = std::move(better);
    }
}

// The frames now in the buffer, decoded ahead of the frame synchroniser and in parallel.
//
// The rescue of one frame costs many times a plain decode, and at the SNR it exists for most
// frames need it, so run in line it is slower than the stream arrives. Frames are independent of
// one another, and while the synchroniser is locked it is known where the next ones start. So each
// batch is decoded here first, one frame per worker, and the synchroniser then walks the same
// frames in order exactly as before and finds their results ready (assembleAt). A frame it asks
// for that is not here -- lock just acquired, a polarity change -- is decoded in line as before.
// Nothing about which frames are accepted changes; only where the work is done.
void LritDecoder::precomputeFrames() {
    frameCache_.clear();
    if (!rescueEnabled_ || !locked_ || rescueThreads_ < 2) return;
    using Clock = std::chrono::steady_clock;
    const std::size_t needBits = static_cast<std::size_t>(kCaduBytes) * 8;
    if (bitBuffer_.size() < consumed_ + 2 * needBits) return;   // one frame: nothing to share out
    const std::size_t n = std::min<std::size_t>((bitBuffer_.size() - consumed_) / needBits, 64);
    // The position and polarity the locked synchroniser will ask for, frame by frame.
    frameCache_.resize(n);
    bool lockInverted = lockInverted_;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t at = consumed_ + k * needBits;
        bool inverted = lockInverted;
        const int errors = asmErrorsAt(at, &inverted);
        const bool flip = inverted != lockInverted;
        FrameBody& f = frameCache_[k];
        f.bitOffset = at;
        if (errors <= (flip ? kFlipMaxErrors : kLockMaxErrors)) {
            lockInverted = inverted;
            f.inverted = inverted;
            f.asmErrors = errors;
        } else {
            f.inverted = lockInverted;
            f.asmErrors = inverted == lockInverted ? errors : kSyncBits - errors;
        }
    }
    const bool limited = rescueRatio_ > 0.0;
    const auto tStart = Clock::now();
    const bool budget = !limited ||
        rescueSpent_ <= rescueRatio_ * std::chrono::duration<double>(tStart - rescueStart_).count() + 0.3;
    // Frames the back-off after a run of failures would pass over are not worth starting.
    const std::size_t skip = static_cast<std::size_t>(std::max(0, rescueSkip_));
    const auto perFrame = std::chrono::milliseconds(limited ? 200 : 1000);
    RescuePool::instance().run(static_cast<int>(n), rescueThreads_, [&](int k) {
        const std::size_t at = frameCache_[static_cast<std::size_t>(k)].bitOffset;
        const bool inverted = frameCache_[static_cast<std::size_t>(k)].inverted;
        const int asmErrors = frameCache_[static_cast<std::size_t>(k)].asmErrors;
        try {
            FrameBody f = hardDecode(at, inverted, asmErrors);
            if (f.rsFixed < 0 && budget && static_cast<std::size_t>(k) >= skip) rescueInto(f, Clock::now() + perFrame);
            frameCache_[static_cast<std::size_t>(k)] = std::move(f);
        } catch (...) {
            frameCache_[static_cast<std::size_t>(k)].valid = false;   // out of memory: the synchroniser decodes it in line
        }
    });
    if (limited) {
        bool any = false;
        for (const FrameBody& f : frameCache_) any = any || f.rescueAttempted;
        if (any) rescueSpent_ += std::chrono::duration<double>(Clock::now() - tStart).count();
    }
}

void LritDecoder::assembleAt(std::size_t bitOffset, bool inverted, int asmErrors, Cadu* out) {
    Cadu cadu;
    cadu.syncFound = true;
    cadu.asmBitErrors = asmErrors;
    cadu.syncOffsetBits = static_cast<int>(bitOffset);
    FrameBody body;
    bool ahead = false;   // decoded by precomputeFrames
    for (FrameBody& f : frameCache_) {
        if (f.valid && f.bitOffset == bitOffset && f.inverted == inverted && f.asmErrors == asmErrors) {
            body = std::move(f);
            f.valid = false;
            ahead = true;
            break;
        }
    }
    if (!ahead) body = hardDecode(bitOffset, inverted, asmErrors);
    std::vector<std::uint8_t> plain = std::move(body.plain);
    int rsFixed = body.rsFixed;
    if (rsFixed < 0 && rescueEnabled_) {
        ++stats_.rescueTried;
        using Clock = std::chrono::steady_clock;
        const auto tStart = Clock::now();
        const bool limited = rescueRatio_ > 0.0;
        // Frames inside a signal dropout cannot be rescued and each attempt costs the full search, so after a run of
        // failures the next frames are not tried (the pause doubles up to 64 frames and ends at the first success).
        bool allowed = rescueSkip_ == 0;
        if (rescueSkip_ > 0) --rescueSkip_;
        bool rescued = false;
        std::vector<uint8_t> better;
        if (allowed && ahead && body.rescueAttempted) {
            rescued = body.rescued;   // done ahead, its time already counted for the batch
            better = std::move(body.better);
        } else if (allowed && ahead && limited) {
            allowed = false;          // the batch had no time budget left for it
        } else if (allowed) {
            if (limited)
                allowed = rescueSpent_ <= rescueRatio_ * std::chrono::duration<double>(tStart - rescueStart_).count() + 0.3;
            if (allowed) {
                // Longest a single frame may take: 200 ms live, 1 s offline.
                better = plain;
                rescued = rescueFrame(bitOffset, inverted, asmErrors, better, tStart + std::chrono::milliseconds(limited ? 200 : 1000));
                if (limited) rescueSpent_ += std::chrono::duration<double>(Clock::now() - tStart).count();
            }
        }
        if (allowed) {
            if (rescued) { rescueFailStreak_ = 0; rescueBackoff_ = 0; }
            else if (++rescueFailStreak_ >= 6) {
                rescueBackoff_ = rescueBackoff_ == 0 ? 4 : std::min(64, rescueBackoff_ * 2);
                rescueSkip_ = rescueBackoff_;
            }
        }
        if (rescued) {
            int diff = 0;
            for (std::size_t i = 4; i < better.size(); ++i) diff += better[i] != plain[i];
            plain = std::move(better);
            rsFixed = diff;
            ++stats_.rescueOk;
        }
    }
    cadu.rsCorrectedBytes = rsFixed > 0 ? rsFixed : 0;

    // The VCDU header, now read from the plaintext. Table 8.3, as offsets from the start of the
    // frame: bytes 4 and 5 are two bits of version, the eight-bit spacecraft id, and the top two
    // bits of the six-bit VCID; byte 5's low six bits are the rest of the VCID; bytes 5, 6 and 7
    // carry the 24-bit virtual channel frame count; byte 8 holds the replay flag.
    if (plain.size() >= 10) {
        cadu.vcid = static_cast<uint8_t>(((plain[4] & 0x3Fu) << 2) | ((plain[5] >> 6) & 0x03u));
        cadu.virtualFrameCount = (static_cast<uint64_t>(plain[5] & 0x3Fu) << 16)
                                 | (static_cast<uint64_t>(plain[6]) << 8)
                                 | static_cast<uint64_t>(plain[7]);
    }

    // The check is defined on the plaintext, and the caller -- and the packet layer above it --
    // receives the plaintext, so the derandomised bytes are what this CADU is from here on.
    cadu.bytes = std::move(plain);
    cadu.rsOk = rsSyndromesZero(cadu.bytes, kRsInterleaveDepth);
    // Polarity arbitration. The complement of a CCSDS Reed-Solomon codeword is also a codeword,
    // so a frame read with the wrong polarity passes the check above as the bitwise complement of
    // the transmitted frame. The VCDU version field settles it: it is binary 01 (AOS), and the
    // complement reads 10. A checked frame whose version reads 10 is therefore the complement and
    // is turned back, and the caller learns that the stream polarity has flipped.
    if (cadu.rsOk && (cadu.bytes[4] >> 6) == 2u) {
        for (std::size_t i = 4; i < cadu.bytes.size(); ++i)
            cadu.bytes[i] = static_cast<std::uint8_t>(~cadu.bytes[i]);
        cadu.vcid = static_cast<uint8_t>(((cadu.bytes[4] & 0x3Fu) << 2) | ((cadu.bytes[5] >> 6) & 0x03u));
        cadu.virtualFrameCount = (static_cast<uint64_t>(cadu.bytes[5] & 0x3Fu) << 16)
                                 | (static_cast<uint64_t>(cadu.bytes[6]) << 8)
                                 | static_cast<uint64_t>(cadu.bytes[7]);
        cadu.polarityCorrected = true;
    }
    *out = std::move(cadu);
}

// Iterative re-decode. See the declaration. Reed-Solomon is run per codeword; every codeword that
// corrects is a certainty (up to a negligible miscorrection rate), and its 255 bytes are handed to
// the Viterbi decoder as known bits. With three of four interleaved codewords known the trellis
// has almost nothing left to guess in the fourth, and with one known it still sheds most of its
// errors, so a frame a single pass loses by a few bytes per codeword comes back after one or two
// rounds. The known marker bits at both ends of the frame are used the same way.
bool LritDecoder::rescueFrame(std::size_t bitOffset, bool inverted, int asmErrors, std::vector<uint8_t>& plain,
                              std::chrono::steady_clock::time_point deadline) const {
    constexpr std::size_t kFrameBits = static_cast<std::size_t>(kCaduBytes) * 8;
    constexpr int kMaxRounds = 6;
    if (softBuf_.size() < 2 * (bitOffset + kFrameBits)) return false;
    const std::size_t avail = softBuf_.size() / 2 - bitOffset;
    const std::size_t pairs = std::min(avail, kFrameBits + static_cast<std::size_t>(kSyncBits));
    std::vector<std::int8_t> soft(softBuf_.begin() + static_cast<std::ptrdiff_t>(2 * bitOffset),
                                  softBuf_.begin() + static_cast<std::ptrdiff_t>(2 * (bitOffset + pairs)));
    if (inverted) for (auto& v : soft) v = static_cast<std::int8_t>(-v);
    std::vector<std::int8_t> prior(pairs, -1);
    auto setMarker = [&](std::size_t at) {
        if (at + kSyncBits > pairs) return;
        for (int i = 0; i < kSyncBits; ++i) prior[at + static_cast<std::size_t>(i)] = static_cast<std::int8_t>((kSyncWord >> (31 - i)) & 1u);
    };
    if (asmErrors <= 4) setMarker(0);
    setMarker(kFrameBits);

    std::array<std::array<std::uint8_t, kRsN>, kRsInterleaveDepth> solved{};
    std::array<bool, kRsInterleaveDepth> have{};
    const auto& pn = kCcsdsPn;
    ViterbiDecoder vit;
    // Variant 0 is the soft input as it came. When that leaves a codeword unsolved, the same
    // samples are decoded again with a different metric (clipped or compressed soft values, a
    // weaker or stronger prior). Each variant makes different mistakes, a codeword any of them
    // gets through Reed-Solomon is a certainty and is kept, and it goes into the next variant as
    // known bits, so the variants help one another instead of being a plain vote.
    struct Variant { int clip; int bonus; bool sqrtMap; int noise; };
    static constexpr Variant kVariants[] = {
        {127, 400, false, 0}, {24, 400, false, 0}, {64, 150, false, 0}, {127, 1500, false, 0},
        {127, 400, true, 0},  {12, 400, false, 0}, {40, 800, false, 0}, {127, 60, false, 0},
        {18, 250, true, 0},   {90, 2500, false, 0},
        {127, 400, false, 6}, {127, 400, false, 10}, {60, 400, false, 8}, {30, 400, false, 6},
        {127, 800, false, 12}, {45, 300, false, 10}, {127, 400, true, 6}, {20, 400, false, 5}};
    const std::vector<std::int8_t> soft0 = soft;
    // Every unsolved codeword's decoded bytes, one candidate per variant: where the candidates
    // disagree the bytes are unreliable, which is what the erasure stage below feeds on.
    std::array<std::vector<std::array<std::uint8_t, kRsN>>, kRsInterleaveDepth> cands;
    auto allHave = [&] { return have[0] && have[1] && have[2] && have[3]; };
    auto adopt = [&](int pos, const std::array<std::uint8_t, kRsN>& code) {
        solved[static_cast<std::size_t>(pos)] = code;
        have[static_cast<std::size_t>(pos)] = true;
        for (int ii = 0; ii < kRsN; ++ii) {
            const std::size_t byteIdx = 4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos);
            const std::uint8_t v = static_cast<std::uint8_t>(kToDualBasis[code[static_cast<std::size_t>(ii)]] ^ pn[(byteIdx - 4) % pn.size()]);
            for (int i = 0; i < 8; ++i) prior[byteIdx * 8 + static_cast<std::size_t>(i)] = static_cast<std::int8_t>((v >> (7 - i)) & 1);
        }
    };

    // Stage A: posterior decoding. A log-MAP pass over the frame gives every information bit a
    // reliability. Per codeword the least reliable bytes are declared erasures in growing numbers;
    // every candidate Reed-Solomon accepts is scored against the posteriors (the soft distance to
    // the candidate, relative to the total reliability), and the best one that is either well inside
    // the code's reach or very close to what the channel said is adopted. Adopted codewords become
    // known bits for the next pass, which is where most of the gain comes from: the four codewords
    // are byte-interleaved, so one solved codeword pins a quarter of the bits of every error burst.
    {
        float gain = 0.05f;   // LLR per soft unit: the demodulator normalises the symbols, so this is nearly constant over the useful range (flat optimum, 0.035..0.07)
        // Soft distance (relative to the codeword's total reliability) below which a candidate with many erasures is believed.
        // Calibrated on simulated streams (about 2,700 frames, no wrong frame accepted at these values); looser values let wrong codewords in.
        constexpr float dth = 0.0100f;       // candidates with redundancy left (2*errors + erasures <= 28)
        constexpr float dthTight = 0.0075f;  // candidates using nearly all 32 parity symbols: nothing but the soft distance checks them
        constexpr int fstep = 2, maxIter = 10, autoAccept = 18;
        constexpr int maxReach = 32;   // 2*errors + erasures the code can still resolve
        const int kRandomTrials = randomTrials_; constexpr int kRankWindow = 24;   // candidates with 2*errors + erasures <= 18 need no soft check
        std::vector<float> post;
        float curGain = gain; int stall = 0;
        for (int iter = 0; iter < maxIter && !allHave() && std::chrono::steady_clock::now() < deadline; ++iter) {
            BcjrK7::run(soft.data(), pairs, prior.data(), curGain, kG1, kG2, post);
            int newly = 0;
            // Two passes over the unsolved codewords. The first is the plain sweep; a codeword it solves
            // goes back into the next BCJR run as known bits, which does more for the others than any
            // search here. Only when it solves nothing is the wider (and far costlier) search run.
            for (int phase = 0; phase < 2 && newly == 0; ++phase) {
            const bool deep = phase == 1;
            if (deep && stall != 0) break;   // once per frame, on the nominal gain: repeating it on the other gains costs 25% more time for 0.4 points
            for (int pos = 0; pos < kRsInterleaveDepth; ++pos) {
                if (have[static_cast<std::size_t>(pos)]) continue;
                std::array<std::uint8_t, kRsN> raw{};
                std::array<float, kRsN> rel{};
                float tot = 0.0f;
                for (int ii = 0; ii < kRsN; ++ii) {
                    const std::size_t byteIdx = 4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos);
                    std::uint8_t v = 0; float r = 1e30f;
                    for (int i = 0; i < 8; ++i) {
                        const float l = post[byteIdx * 8 + static_cast<std::size_t>(i)];
                        v = static_cast<std::uint8_t>((v << 1) | (l > 0.0f ? 1 : 0));
                        r = std::min(r, std::fabs(l)); tot += std::fabs(l);
                    }
                    v = static_cast<std::uint8_t>(v ^ pn[(byteIdx - 4) % pn.size()]);
                    raw[static_cast<std::size_t>(ii)] = kFromDualBasis[v];
                    rel[static_cast<std::size_t>(ii)] = r;
                }
                std::array<int, kRsN> order{};
                for (int ii = 0; ii < kRsN; ++ii) order[static_cast<std::size_t>(ii)] = ii;
                std::sort(order.begin(), order.end(), [&](int a, int b) { return rel[static_cast<std::size_t>(a)] < rel[static_cast<std::size_t>(b)]; });
                float bestScore = -1e30f; bool got = false; std::array<std::uint8_t, kRsN> bestCode{};
                // Every erasure set below is tried on this one word, so its syndromes are taken once.
                std::array<std::uint8_t, kRsParity> rawSyndromes{};
                const bool rawClean = rsSyndromes(raw, rawSyndromes);
                // One erasure set: decode, check, and score the result against the posteriors.
                float dScale = 1.0f;
                auto tryErasures = [&](const std::vector<int>& er) {
                    const int F = static_cast<int>(er.size());
                    auto code = raw;
                    const int rcx = F == 0 ? rsCorrectCodeword(code)
                                           : (rawClean ? 0 : rsCorrectWithSyndromes(code, rawSyndromes, er, 0));
                    if (rcx < 0) return;
                    int ne = 0;
                    { std::array<bool, kRsN> isE{}; for (int e : er) isE[static_cast<std::size_t>(e)] = true;
                      for (int ii = 0; ii < kRsN; ++ii) if (!isE[static_cast<std::size_t>(ii)] && code[static_cast<std::size_t>(ii)] != raw[static_cast<std::size_t>(ii)]) ++ne; }
                    if (2 * ne + F > maxReach) return;
                    { std::array<std::uint8_t, kRsParity> check{}; if (!rsSyndromes(code, check)) return; }   // must be a codeword
                    float sc = 0.0f;
                    for (int ii = 0; ii < kRsN; ++ii) {
                        const std::size_t byteIdx = 4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos);
                        const std::uint8_t v = static_cast<std::uint8_t>(kToDualBasis[code[static_cast<std::size_t>(ii)]] ^ pn[(byteIdx - 4) % pn.size()]);
                        for (int i = 0; i < 8; ++i) { const float l = post[byteIdx * 8 + static_cast<std::size_t>(i)]; sc += ((v >> (7 - i)) & 1) ? l : -l; }
                    }
                    const float d = (tot - sc) / (2.0f * tot);
                    if (!(2 * ne + F <= autoAccept || d < dScale * (2 * ne + F > 28 ? dthTight : dth))) return;
                    if (sc > bestScore) { bestScore = sc; bestCode = code; got = true; }
                };
                if (!deep) for (int F = 0; F <= 32; F += fstep) tryErasures(std::vector<int>(order.begin(), order.begin() + F));
                // Deterministic neighbourhood of the plain sweep: erase the F-2 least reliable bytes plus two more
                // from the next few ranks. The true errors are mostly, not always, the least reliable bytes, and
                // this covers the commonest misses (one or two errors ranked just outside the erased set) with a
                // fixed number of decodes, only for codewords the sweep failed. More candidates mean more
                // chances of a false one, so these are believed only when closer to the channel (dScale).
                if (!got && deep) {
                    constexpr int pairWindow = 8;
                    dScale = 0.8f;
                    for (int F = 22; F <= 32 && !got; F += 2)
                        for (int a = F - 2; a < std::min(kRsN, F - 2 + pairWindow) && !got; ++a)
                            for (int b = a + 1; b < std::min(kRsN, F - 2 + pairWindow) && !got; ++b) {
                                std::vector<int> er(order.begin(), order.begin() + (F - 2));
                                er.push_back(order[static_cast<std::size_t>(a)]);
                                er.push_back(order[static_cast<std::size_t>(b)]);
                                tryErasures(er);
                            }
                }
                // Randomised erasure sets: the true errors are mostly, not always, among the least reliable bytes.
                // Erase the least reliable F-s bytes and s more drawn from the next ranks; with a few hundred
                // draws one of them covers enough of the real errors to land inside the code's reach.
                if (!got && deep) {
                    std::uint32_t rng = 0x9E3779B9u ^ static_cast<std::uint32_t>(pos * 2654435761u) ^ static_cast<std::uint32_t>(iter * 40503u);
                    auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
                    std::vector<int> pool;
                    for (int t = 0; t < kRandomTrials && !got; ++t) {
                        if ((t & 15) == 15 && std::chrono::steady_clock::now() >= deadline) break;
                        const int F = 20 + 2 * static_cast<int>(next() % 7);
                        const int sw = std::min(F, 2 + static_cast<int>(next() % 8));
                        const int W = std::min(kRsN, F + kRankWindow);
                        std::vector<int> er(order.begin(), order.begin() + (F - sw));
                        pool.assign(order.begin() + (F - sw), order.begin() + W);
                        for (int k = 0; k < sw; ++k) {
                            const std::size_t j = static_cast<std::size_t>(k) + next() % (pool.size() - static_cast<std::size_t>(k));
                            std::swap(pool[static_cast<std::size_t>(k)], pool[j]);
                            er.push_back(pool[static_cast<std::size_t>(k)]);
                        }
                        tryErasures(er);
                    }
                }
                if (got) { adopt(pos, bestCode); ++newly; }
            }
            }
            if (newly == 0) { if (stall < 2) { curGain = gain * (stall == 0 ? 0.6f : 1.6f); ++stall; continue; } break; }
        }
    }
    for (int sweep = 0; sweep < 2 && !allHave(); ++sweep) {
        for (const Variant& var : kVariants) {
            if (allHave()) break;
            std::uint32_t lcg = 12345u + static_cast<std::uint32_t>(var.noise) * 7919u + static_cast<std::uint32_t>(sweep) * 104729u;
            for (std::size_t i = 0; i < soft0.size(); ++i) {
                int v = soft0[i];
                if (var.sqrtMap) {
                    const int a2 = std::abs(v);
                    const int r = static_cast<int>(std::sqrt(static_cast<double>(a2)) * 11.2);
                    v = v < 0 ? -r : r;
                }
                if (var.noise) {
                    lcg = lcg * 1664525u + 1013904223u;
                    v += static_cast<int>((lcg >> 16) % static_cast<std::uint32_t>(2 * var.noise + 1)) - var.noise;
                }
                soft[i] = static_cast<std::int8_t>(std::max(-var.clip, std::min(var.clip, v)));
            }
            for (int round = 0; round < kMaxRounds; ++round) {
                const std::vector<uint8_t> bits = vit.decodeBlockWithPrior(soft.data(), pairs, prior.data(), var.bonus);
                int newly = 0;
                for (int pos = 0; pos < kRsInterleaveDepth; ++pos) {
                    if (have[static_cast<std::size_t>(pos)]) continue;
                    std::array<std::uint8_t, kRsN> code{};
                    for (int ii = 0; ii < kRsN; ++ii) {
                        const std::size_t byteIdx = 4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos);
                        std::uint8_t v = 0;
                        for (int i = 0; i < 8; ++i) v = static_cast<std::uint8_t>((v << 1) | bits[byteIdx * 8 + static_cast<std::size_t>(i)]);
                        v = static_cast<std::uint8_t>(v ^ pn[(byteIdx - 4) % pn.size()]);
                        code[static_cast<std::size_t>(ii)] = kFromDualBasis[v];
                    }
                    auto raw = code;
                    if (rsCorrectCodeword(code) < 0) {
                        if (round == 0 || cands[static_cast<std::size_t>(pos)].empty()) cands[static_cast<std::size_t>(pos)].push_back(raw);
                        continue;
                    }
                    adopt(pos, code);
                    ++newly;
                }
                if (allHave()) break;
                if (newly == 0) break;
            }
        }
        // Erasure stage. The bytes the variants disagree on are declared erasures; a Reed-Solomon
        // codeword tolerates 32 of them (or fewer together with real errors), which turns many
        // more wrong bytes than the 16 an errors-only decoder takes into a correctable pattern.
        for (int pos = 0; pos < kRsInterleaveDepth; ++pos) {
            if (have[static_cast<std::size_t>(pos)]) continue;
            const auto& cl = cands[static_cast<std::size_t>(pos)];
            if (cl.size() < 3) continue;
            std::array<std::uint8_t, kRsN> base{};
            std::array<int, kRsN> minority{};
            for (int ii = 0; ii < kRsN; ++ii) {
                int cnt[256] = {0};
                int bestv = 0;
                for (const auto& cc : cl) { const int v = cc[static_cast<std::size_t>(ii)]; if (++cnt[v] > cnt[bestv]) bestv = v; }
                base[static_cast<std::size_t>(ii)] = static_cast<std::uint8_t>(bestv);
                minority[static_cast<std::size_t>(ii)] = static_cast<int>(cl.size()) - cnt[bestv];
            }
            for (int thr = 1; thr <= static_cast<int>(cl.size()) && !have[static_cast<std::size_t>(pos)]; ++thr) {
                std::vector<int> er;
                for (int ii = 0; ii < kRsN; ++ii) if (minority[static_cast<std::size_t>(ii)] >= thr) er.push_back(ii);
                if (er.empty() || er.size() > 32) continue;
                auto code = base;
                if (rsCorrectCodewordErasures(code, er) < 0) continue;
                auto chk = code;
                if (rsCorrectCodeword(chk) != 0) continue;
                adopt(pos, code);
            }
            // The first candidate as the base as well: the unmodified metric is the best single guess.
            if (!have[static_cast<std::size_t>(pos)]) {
                for (int thr = 1; thr <= static_cast<int>(cl.size()) && !have[static_cast<std::size_t>(pos)]; ++thr) {
                    std::vector<int> er;
                    for (int ii = 0; ii < kRsN; ++ii) if (minority[static_cast<std::size_t>(ii)] >= thr) er.push_back(ii);
                    if (er.empty() || er.size() > 32) continue;
                    auto code = cl[0];
                    if (rsCorrectCodewordErasures(code, er) < 0) continue;
                    auto chk = code;
                    if (rsCorrectCodeword(chk) != 0) continue;
                    adopt(pos, code);
                }
            }
        }
    }
    if (!(have[0] && have[1] && have[2] && have[3])) return false;
    std::vector<uint8_t> out = plain;
    for (int pos = 0; pos < kRsInterleaveDepth; ++pos)
        for (int ii = 0; ii < kRsN; ++ii)
            out[4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos)] =
                kToDualBasis[solved[static_cast<std::size_t>(pos)][static_cast<std::size_t>(ii)]];
    if (!rsSyndromesZero(out, kRsInterleaveDepth)) return false;
    plain = std::move(out);
    return true;
}

void LritDecoder::acceptCadu(Cadu&& cadu) {
    ++stats_.cadusAssembled;
    if (cadu.syncFound) ++stats_.cadusWithSync;
    if (cadu.rsOk) ++stats_.cadusWithGoodRs;
    if (cadu.rsOk && cadu.rsCorrectedBytes > 0) {
        ++stats_.cadusCorrected;
        stats_.rsBytesCorrected += static_cast<std::size_t>(cadu.rsCorrectedBytes);
    }
    if (cadu.syncFound && !cadu.flywheel && cadu.asmBitErrors > 0) ++stats_.asmTolerated;
    if (cadu.syncFound && cadu.vcid < stats_.vcidHistogram.size())
        ++stats_.vcidHistogram[cadu.vcid];
    ready_.push_back(std::move(cadu));
}

void LritDecoder::processPending() {
    const std::size_t needBits = static_cast<std::size_t>(kCaduBytes) * 8;
    precomputeFrames();
    while (true) {
        if (locked_) {
            if (bitBuffer_.size() < consumed_ + needBits) break;
            bool inverted = lockInverted_;
            const int errors = asmErrorsAt(consumed_, &inverted);
            Cadu cadu;
            // A 180-degree slip shows as the complemented marker. Reed-Solomon cannot arbitrate
            // polarity -- the complement of a CCSDS RS codeword is itself a codeword, so a frame
            // read with the wrong polarity passes the check as the complement of the real frame
            // (seen on a real capture with noise added: a fill frame came out as VCID 0, every
            // byte inverted). So the polarity only changes on a near-perfect complemented marker;
            // anything weaker is read in the locked polarity.
            const bool flip = inverted != lockInverted_;
            if (errors <= (flip ? kFlipMaxErrors : kLockMaxErrors)) {
                lockInverted_ = inverted;
                assembleAt(consumed_, inverted, errors, &cadu);
                misses_ = 0;
            } else {
                // Flywheel: the marker is unreadable, but the frame is where it must be. Hand it
                // to Reed-Solomon, which is a far stronger test than 32 marker bits; it is only
                // delivered if the check passes.
                const int lockedErrors = inverted == lockInverted_ ? errors : kSyncBits - errors;
                assembleAt(consumed_, lockInverted_, lockedErrors, &cadu);
                cadu.flywheel = true;
                cadu.syncFound = cadu.rsOk;
                ++stats_.flywheelFrames;
                if (cadu.rsOk) { ++stats_.flywheelRsOk; misses_ = 0; }
                else ++misses_;
            }
            if (cadu.polarityCorrected) { lockInverted_ = !lockInverted_; ++stats_.polarityCorrections; }
            consumed_ += needBits;
            if (cadu.syncFound) lastGoodEnd_ = consumed_;
            acceptCadu(std::move(cadu));
            if (misses_ >= missLimit_) {
                // Lost: search again from just after the last frame that was believed, so a
                // slipped stream is picked up at its new alignment without skipping frames.
                locked_ = false;
                misses_ = 0;
                ++stats_.syncLosses;
                const std::size_t back = static_cast<std::size_t>(kMaxMisses) * needBits;   // rewind at most this far
                consumed_ = std::max(lastGoodEnd_, consumed_ > back ? consumed_ - back + 1 : 0);
            }
            continue;
        }

        // Search, one bit at a time. The extra 32 bits are the next frame's marker, used to
        // confirm a candidate whose own marker had errors.
        if (bitBuffer_.size() < consumed_ + needBits) break;
        bool inverted = false;
        const int errors = asmErrorsAt(consumed_, &inverted);
        if (errors <= kSearchMaxErrors) {
            Cadu cadu;
            assembleAt(consumed_, inverted, errors, &cadu);
            bool confirmed = errors == 0 || cadu.rsOk;
            if (!confirmed) {
                // Wait for the next marker's bits before judging this candidate.
                if (bitBuffer_.size() < consumed_ + needBits + static_cast<std::size_t>(kSyncBits)) break;
                bool nextInverted = false;
                const int next = asmErrorsAt(consumed_ + needBits, &nextInverted);
                confirmed = next <= kVerifyMaxErrors && nextInverted == inverted;
            }
            if (confirmed) {
                locked_ = true;
                lockInverted_ = cadu.polarityCorrected ? !inverted : inverted;
                if (cadu.polarityCorrected) ++stats_.polarityCorrections;
                misses_ = 0;
                consumed_ += needBits;
                lastGoodEnd_ = consumed_;
                acceptCadu(std::move(cadu));
                continue;
            }
        }
        // No sync here; advance one bit and look again.
        ++consumed_;
        ++stats_.cadusAssembled;
    }
    frameCache_.clear();   // its bit offsets are about to move
    if (stats_.cadusAssembled > 0)
        stats_.frameSyncRate = static_cast<double>(stats_.cadusWithSync)
                               / static_cast<double>(stats_.cadusAssembled);
    if (consumed_ > (1u << 20)) {
        // Keep one frame's worth before the read position so a lost lock can rewind into it.
        const std::size_t keepFrom = locked_ ? std::min(lastGoodEnd_, consumed_) : consumed_;
        const std::size_t drop = keepFrom > needBits * static_cast<std::size_t>(kMaxMisses)
                                     ? keepFrom - needBits * static_cast<std::size_t>(kMaxMisses) : 0;
        if (drop > 0) {
            bitBuffer_.erase(bitBuffer_.begin(), bitBuffer_.begin() + static_cast<std::ptrdiff_t>(drop));
            if (rescueEnabled_ && softBuf_.size() >= 2 * drop) softBuf_.erase(softBuf_.begin(), softBuf_.begin() + static_cast<std::ptrdiff_t>(2 * drop));
            consumed_ -= drop;
            lastGoodEnd_ = lastGoodEnd_ > drop ? lastGoodEnd_ - drop : 0;
        }
    }
}

void LritDecoder::restartStream() {
    viterbi_.resetStream();
    bitBuffer_.clear();
    softBuf_.clear();
    consumed_ = 0;
    pendingSoft_ = 0.0f;
    hasPending_ = false;
    symbolsSinceSync_ = 0;
    dropNext_ = false;
    locked_ = false;
    misses_ = 0;
    lastGoodEnd_ = 0;
}

QpskLinkDecoder::QpskLinkDecoder() { reset(); }

void QpskLinkDecoder::reset() {
    dec_.reset();
    dec_.setPairSlipEnabled(false);
    hypothesis_ = 0;
    everLocked_ = false;
    symbolsSinceGood_ = 0;
    goodBefore_ = 0;
}

void QpskLinkDecoder::feedSymbols(const std::vector<std::complex<float>>& symbols) {
    if (symbols.empty()) return;
    std::vector<float> soft;
    soft.reserve(symbols.size() * 2);
    const float k = -scale_;   // same sign convention as the BPSK path: soft = -scale * value
    for (const auto& z : symbols) {
        const float i = z.real(), q = z.imag();
        float a = i, b = q;
        switch (hypothesis_) {
        case 1: a = q; b = -i; break;
        case 2: a = i; b = -q; break;
        case 3: a = q; b = i; break;
        default: break;
        }
        soft.push_back(k * a);
        soft.push_back(k * b);
    }
    dec_.feedSoftBits(soft, false);
    // Evidence that the mapping is right is a frame marker found where the frame clock says it must be
    // (verified against the next marker), or a frame Reed-Solomon passes. At low SNR Reed-Solomon
    // alone is rare for long stretches while the markers keep coming, and judging by it threw away a
    // working mapping every few frames.
    const std::size_t good = dec_.stats().cadusWithGoodRs + dec_.stats().cadusWithSync;
    if (good != goodBefore_) {
        goodBefore_ = good;
        symbolsSinceGood_ = 0;
        everLocked_ = true;
        return;
    }
    symbolsSinceGood_ += symbols.size();
    // Before the first frame, cycle the mapping quickly. After frames have been seen, a gap is
    // far more likely a fade than a wrong mapping, so the validated one is held for a second
    // before the search resumes.
    const std::size_t limit = everLocked_ ? kHoldSymbols : kHypothesisSymbols;
    if (symbolsSinceGood_ > limit) {
        symbolsSinceGood_ = 0;
        everLocked_ = false;
        hypothesis_ = (hypothesis_ + 1) % 4;
        dec_.restartStream();
    }
}

std::vector<Cadu> LritDecoder::takeCadus() {
    std::vector<Cadu> out;
    out.swap(ready_);
    return out;
}

}  // namespace nadirstation::gk2a
