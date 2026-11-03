// GK-2A LRIT demodulation and framing. See gk2adecoder.h for the specification references.
//
// This file was rebuilt after a patch script replaced the whole of the first anonymous namespace,
// which silently removed three helpers other functions depend on: popcount8, buildRandomiserByte
// and the GF(256) tables. The lesson recorded here is that a text patch anchored on "namespace {"
// is not anchored at all, so the helpers are now written out in full, in one place.

#include "gk2adecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
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
int rsCorrectCodeword(std::array<std::uint8_t, kRsN>& c) {
    const Gf256& g = gf();
    const auto& products = rsRootProducts();
    std::array<std::uint8_t, kRsParity> s{};
    bool clean = true;
    for (int k = 0; k < kRsParity; ++k) {
        // S_k = c(beta^(112+k)), by Horner from the highest power.
        std::uint8_t acc = 0;
        for (int i = 0; i < kRsN; ++i) acc = static_cast<std::uint8_t>(products[k][acc] ^ c[static_cast<std::size_t>(i)]);
        s[static_cast<std::size_t>(k)] = acc;
        if (acc != 0) clean = false;
    }
    if (clean) return 0;

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

void LritDecoder::assembleAt(std::size_t bitOffset, bool inverted, int asmErrors, Cadu* out) {
    Cadu cadu;
    cadu.syncFound = true;
    cadu.asmBitErrors = asmErrors;
    cadu.syncOffsetBits = static_cast<int>(bitOffset);
    cadu.bytes.resize(kCaduBytes);
    for (std::size_t b = 0; b < static_cast<std::size_t>(kCaduBytes); ++b) {
        std::uint8_t byte = 0;
        for (int i = 0; i < 8; ++i)
            byte = static_cast<std::uint8_t>(
                (byte << 1) | bitBuffer_[bitOffset + b * 8 + static_cast<std::size_t>(i)]);
        if (inverted) byte = static_cast<std::uint8_t>(~byte);
        cadu.bytes[b] = byte;
    }
    // The marker bytes are rewritten to the marker itself: a frame accepted with marker bit
    // errors would otherwise carry them on to anything that re-checks bytes 0..3.
    cadu.bytes[0] = 0x1A; cadu.bytes[1] = 0xCF; cadu.bytes[2] = 0xFC; cadu.bytes[3] = 0x1D;
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
    std::vector<std::uint8_t> plain = cadu.bytes;
    const auto& seq = LritDecoder::randomiserBits();
    for (std::size_t i = 4; i < plain.size(); ++i)
        plain[i] = static_cast<std::uint8_t>(plain[i] ^ seq[(i - 4) % seq.size()]);

    // Reed-Solomon correction before anything is read out of the frame, so the header fields come
    // from the corrected bytes. rsCorrect leaves a frame it cannot correct untouched, and that frame
    // then fails the check below exactly as before.
    int rsFixed = rsCorrect(plain, kRsInterleaveDepth);
    if (rsFixed < 0 && rescueEnabled_) {
        ++stats_.rescueTried;
        std::vector<uint8_t> better = plain;
        if (rescueFrame(bitOffset, inverted, asmErrors, better)) {
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
bool LritDecoder::rescueFrame(std::size_t bitOffset, bool inverted, int asmErrors, std::vector<uint8_t>& plain) {
    constexpr std::size_t kFrameBits = static_cast<std::size_t>(kCaduBytes) * 8;
    constexpr int kBonus = 400;
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
    for (int round = 0; round < kMaxRounds; ++round) {
        const std::vector<uint8_t> bits = vit.decodeBlockWithPrior(soft.data(), pairs, prior.data(), kBonus);
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
            if (rsCorrectCodeword(code) < 0) continue;
            solved[static_cast<std::size_t>(pos)] = code;
            have[static_cast<std::size_t>(pos)] = true;
            ++newly;
            for (int ii = 0; ii < kRsN; ++ii) {
                const std::size_t byteIdx = 4 + static_cast<std::size_t>(ii) * kRsInterleaveDepth + static_cast<std::size_t>(pos);
                const std::uint8_t v = static_cast<std::uint8_t>(kToDualBasis[code[static_cast<std::size_t>(ii)]] ^ pn[(byteIdx - 4) % pn.size()]);
                for (int i = 0; i < 8; ++i) prior[byteIdx * 8 + static_cast<std::size_t>(i)] = static_cast<std::int8_t>((v >> (7 - i)) & 1);
            }
        }
        if (have[0] && have[1] && have[2] && have[3]) break;
        if (newly == 0) break;
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
    const std::size_t good = dec_.stats().cadusWithGoodRs;
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
