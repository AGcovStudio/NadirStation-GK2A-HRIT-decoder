// GEO-KOMPSAT-2A LRIT demodulation and framing.
//
// GK-2A is a different spacecraft with a different downlink service from FY-2, so nothing in the
// FY-2 decoder applies. Every parameter here comes from the operator's specification, the NMSC
// "GK2A LRIT Mission Specification Document", Issue 1.0, 31 May 2019, which is held in
// docs_official/GK2A_LRIT_Mission_Specification_v1.0.pdf. The passages that fix the chain:
//
//   Table 9.1, physical layer
//     Downloading frequency 1692.14 MHz, bandwidth 1 MHz
//     Information data rate 64 kbps, which is the CADU rate before convolution encoding
//     Coding: Reed-Solomon (255/223, 4) and convolution coding (1/2, K = 7)
//     Pulse shaping: root-raised cosine, roll-off 0.5
//     Modulation: NRZ-L / BPSK
//     Length of one CADU: 1024 bytes
//     The system follows the convolution coding of CCSDS 131.0-B-3 "except symbol inversion on
//     output path of G2"
//
//   Table 8.5, CADU structure
//     Sync 0x1ACFFC1D (4 bytes) followed by the AOS transfer frame (1020 bytes)
//     within which the VCDU is 892 octets and the Reed-Solomon check symbols are 128 octets
//
//   Section 5.2, session layer
//     DES encryption, FIPS PUB 46-2, ECB mode
//
// The symbol inversion the specification mentions is why this decoder can be asked to try both
// polarities: the document says the inversion exists without saying on which of its two output
// paths, so which polarity the downlink presents is settled by which one yields frame sync.
//
// Scope: this file covers the physical and data link layers -- symbol polarity, Viterbi
// decoding, frame synchronisation, derandomisation and Reed-Solomon integrity checking. The
// layers above (CCSDS packets, DES decryption, JPEG/JPEG2000 decompression, LRIT file assembly)
// are not implemented here.

#pragma once

#include <cstddef>
#include <complex>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace nadirstation::gk2a {

// Specification values, at namespace scope because both the frame layer and the Viterbi decoder
// need them and neither owns the other.
// The symbol rate on the link is 128 000, not the 64 000 the specification quotes for the
// information rate, and confusing the two is the single most likely reason a recording fails to
// lock.
//
// Table 9.1 gives an information data rate of 64 kbps and a rate-1/2 convolutional code, so the
// channel carries two symbols per information bit: 128 kbaud, and BPSK puts one bit per symbol,
// so 128 000 symbols per second.
//
// An earlier version of this file set the demodulator to 64 kbaud, which samples only every other
// symbol. Its self test passed anyway, because that test encodes with the same encoder it
// decodes, so both sides shared the mistake and the round trip was self-consistent. Nothing in a
// self test can catch this; only the specification and a real recording can.
constexpr int kInformationRateBps = 64000;   // Table 9.1, before convolutional encoding
constexpr int kSymbolRateBps = 128000;       // on the air: 64 kbps x rate 1/2, one bit per symbol
constexpr int kCaduBytes = 1024;           // Table 9.1, length of one CADU
constexpr std::uint32_t kSyncWord = 0x1ACFFC1DU;   // Table 8.5
constexpr int kConvolutionK = 7;           // Table 9.1
constexpr int kConvolutionRateDen = 2;     // rate 1/2
constexpr int kRsInterleaveDepth = 4;      // RS (255/223, 4)
constexpr int kRsDataBytes = 223;
constexpr int kRsCheckBytes = 32;

// The two generator polynomials of the CCSDS rate-1/2 K=7 convolutional code.
//
// The values in use are 79 and 109 (0x4F and 0x6D). An earlier version used 0x71 and 0x57, the
// octal pair {0161, 0127}, on the reasoning that the two pairs describe the same code up to a
// common factor. They do not; with the wrong polynomials the decoder searches a trellis the
// transmitter never used.
constexpr std::uint8_t kG1 = 0x4F;         // 79, first output symbol
constexpr std::uint8_t kG2 = 0x6D;         // 109, second output symbol

// The number of states is 2^K, not 2^(K-1).
//
// An order-7 code carries a seven-bit shift register and therefore 128 states. This file
// first assumed six bits of history and 64 states while the encoder kept seven bits, leaving the
// encoder with 128 states and the decoder searching 64. The two agreed only on inputs that visit
// a handful of states, which is why periodic patterns round-tripped perfectly and random ones
// came back half wrong, and why three separate attempts to correct the traceback changed nothing.
constexpr int kStates = 1 << kConvolutionK;   // 128, the whole shift register
constexpr int kRegisterMask = kStates - 1;   // 0x7F, the shift register

// Soft-symbol convention: a value near 0 says
// the transmitted symbol was 0 and a value near 255 says it was 1, so the metric is a distance to
// be minimised. This header's public interface takes signed values instead, positive for 0.
// Window length for the traceback, in symbol pairs, and the depth of the
// predecessor ring buffer. Sixteen constraint lengths is well past the point where
// survivor paths merge for a K=7 code, and it is what bounds the decoder's memory
// independently of how long the recording runs.
constexpr int kTracebackDepth = 256;

// Soft-value scale: the demodulator writes clamp(real * 50) as a signed
// byte, so a POSITIVE soft value means symbol 0 was transmitted, and the FEC stage converts with
// in + 127. On that scale a confident 1 is a LOW number and a confident 0 is a HIGH one. The
// previous values had this the other way round and were self-consistent with the rest of the file,
// which is why the encoder round trip passed while the satellite decoded as noise.
constexpr int kSoftBit0 = 255;   // symbol 0, confident
constexpr int kSoftBit1 = 0;     // symbol 1, confident

// Viterbi decoder for the CCSDS rate-1/2 K=7 convolutional code.
//
// Exposed separately so it can be tested on its own against a known input, which is the only way
// to tell a broken decoder from a broken demodulator when no frames appear.
//
// Low-SNR rework (2026-11-01). The previous decoder ran each 256-pair window to its end and
// emitted every bit of it, so the last few dozen bits of every window were decided with almost
// no look-ahead -- exactly the bits a Viterbi decoder gets wrong first when the signal is weak.
// This one keeps a running decision history and only releases a bit once kDecisionDelay further
// pairs have been seen, so every decision has the same, long traceback behind it. The price is a
// fixed output latency of kDecisionDelay bits, which the frame layer does not notice.
//
// The trellis is the 64-state one (six bits of memory); the transition taken from state s with
// input bit b is the seven-bit register (s << 1) | b, whose outputs are outputFor(register) --
// the same register and the same polynomials the encoder uses, so the two cannot disagree.
// The metric is a correlation (maximised) on soft values quantised to [-127, 127].
constexpr int kDecisionDelay = 160;   // pairs of look-ahead behind every released bit (~23 K)

class ViterbiDecoder {
public:
    explicit ViterbiDecoder(int tracebackDepth = kDecisionDelay);

    // Soft input: pairs of values, roughly [-1, +1], positive = symbol 0. Returns hard bits.
    //
    // decode()          one whole transmission: starts from the all-zero register, returns
    //                   exactly one bit per pair (the self tests rely on that).
    // decodeStreaming() one block of a continuous stream: returns the bits that have become final,
    //                   which lags the input by tracebackDepth() pairs; resetAfter = true flushes
    //                   the rest and clears the state.
    // decodePartial()   alias of decodeStreaming(soft, false).
    // decodeFinal()     decodeStreaming(soft, true).
    std::vector<uint8_t> decode(const std::vector<float>& softBits);
    std::vector<uint8_t> decodePartial(const std::vector<float>& softBits);
    std::vector<uint8_t> decodeFinal(const std::vector<float>& softBits);
    std::vector<uint8_t> decodeStreaming(const std::vector<float>& softBits,
                                         bool resetAfter = false);
    // Emit every bit still held back (the last tracebackDepth() of them) from the best state.
    std::vector<uint8_t> flush();
    // Forget the stream. The next block starts with every state equally likely, which is right
    // for joining a transmission in the middle; decode() starts from state 0 instead.
    void resetStream();

    // One block decoded on its own from an unknown start state, with optional a-priori knowledge
    // of individual bits (prior[i] = -1 unknown, 0 or 1 known; each known bit adds +-bonus to the
    // branches that agree/disagree). Used to re-decode a frame once Reed-Solomon has fixed some
    // of its codewords. `soft` holds `pairs` pairs on the +-127 scale (+127 = symbol 0).
    std::vector<uint8_t> decodeBlockWithPrior(const std::int8_t* soft, std::size_t pairs,
                                              const std::int8_t* prior, int bonus);

    // For testing: encode hard bits with the same polynomials.
    static std::vector<uint8_t> encode(const std::vector<uint8_t>& bits);

    void reset() { resetStream(); }
    int tracebackDepth() const { return traceback_; }

private:
    void step(int s0, int s1, int bonus0 = 0, int bonus1 = 0);
    void traceback(std::size_t keep, std::vector<uint8_t>& out);

    int traceback_;
    alignas(16) std::int16_t metric_[64] = {};   // path metrics, 64 states, kept near 0
    std::vector<std::uint64_t> decisions_;  // one 64-bit survivor decision word per pair
    std::size_t released_ = 0;              // decisions_[0 .. released_) already emitted
};

// One CADU as the specification defines it.
struct Cadu {
    std::vector<uint8_t> bytes;     // exactly kCaduBytes once assembled
    bool syncFound = false;         // the 0x1ACFFC1D marker was present at the expected offset
    int syncOffsetBits = -1;        // where the marker was found, for diagnosis
    bool rsOk = false;              // Reed-Solomon syndrome check passed (after correction)
    bool flywheel = false;
    bool polarityCorrected = false; // read complemented; turned back on the VCDU version field          // taken at the expected position although the marker was damaged
    int asmBitErrors = 0;           // bits of the 32-bit marker that differed (after polarity)
    int rsCorrectedBytes = 0;       // bytes the Reed-Solomon decoder changed in this frame
    uint8_t vcid = 0;               // virtual channel id, from the VCDU header
    uint64_t virtualFrameCount = 0; // VCDU counter, for spotting gaps
};

struct DecodeStats {
    std::size_t bitsIn = 0;
    std::size_t softBitsUsed = 0;
    std::size_t cadusAssembled = 0;
    std::size_t cadusWithSync = 0;
    std::size_t cadusWithGoodRs = 0;   // passed the check, corrected or not
    std::size_t cadusCorrected = 0;    // of those, how many needed correction
    std::size_t rsBytesCorrected = 0;  // bytes changed by correction in total
    std::size_t pairSlips = 0;         // times the symbol pairing was shifted by one to look for sync
    std::size_t asmTolerated = 0;      // frames accepted with 1..N marker bit errors
    std::size_t flywheelFrames = 0;    // frames taken on the frame clock with the marker unreadable
    std::size_t flywheelRsOk = 0;      // of those, how many Reed-Solomon then confirmed
    std::size_t polarityCorrections = 0;   // complemented frames turned back (version field 10)
    std::size_t rescueTried = 0;       // frames Reed-Solomon failed on that the iterative re-decoder was run on
    std::size_t rescueOk = 0;          // of those, how many it recovered
    std::size_t syncLosses = 0;        // times the frame lock was given up and search restarted
    bool polarityInverted = false;  // which polarity produced the sync
    double frameSyncRate = 0.0;     // cadusWithSync / cadusAssembled
    std::vector<uint8_t> vcidHistogram;   // 64 entries
};

// Physical and data link layers of the LRIT downlink.
class LritDecoder {
public:
    LritDecoder();

    // Feed soft symbols in the range roughly [-1, +1]; positive means a bit of 1 under the
    // non-inverted convention. Hard bits are accepted too, as 0/1, through feedHardBits().
    // The last call of a transmission passes final = true, which supplies the encoder's flush so
    // the final bits are settled and a CADU straddling the end is still completed. Interior calls
    // leave it false: they must consume exactly their own symbols and no more, or the windows stop
    // lining up.
    void feedSoftBits(const std::vector<float>& soft, bool final = false);
    void feedHardBits(const std::vector<uint8_t>& bits);
    // Supplies the encoder's flush at the end of a transmission, then clears the metric.
    void flushDecoder();
    void reset();

    // Drain the CADUs assembled so far, in order.
    std::vector<Cadu> takeCadus();

    const DecodeStats& stats() const { return stats_; }

    // The rate-1/2 pairing search (one symbol dropped after kPairSlipSymbols without sync) is for
    // BPSK, where two consecutive symbols form a pair. On QPSK the pair is the I and Q of one
    // symbol and is never ambiguous in that way, so the HRIT path turns it off.
    void setPairSlipEnabled(bool enabled) { pairSlipEnabled_ = enabled; }
    // The frame synchroniser holds lock (marker or Reed-Solomon confirmed recently).
    bool frameLocked() const { return locked_; }
    void setRescueEnabled(bool on) { rescueEnabled_ = on; }
    // Share of wall-clock time the soft-decision rescue may use (default 0.5, so a live receiver on
    // its own thread keeps up with the stream); 0 or less removes the limit, for offline decoding.
    void setRescueBudget(double ratio) { rescueRatio_ = ratio; }
    // Extra randomised erasure sets tried per codeword after the deterministic sweep (0 = off, the default).
    // Each costs a full erasure decode; 600 to 2400 buys about one percentage point at 1.2 dB, offline only.
    void setRandomTrials(int n) { randomTrials_ = n; }
    // Threads the rescue may use at once, the decoding thread included (1 = no parallel rescue).
    // Default: the machine's cores less two, at most 8.
    void setRescueThreads(int n) { rescueThreads_ = n < 1 ? 1 : n; }
    static int defaultRescueThreads();
    // Frames in a row without marker or Reed-Solomon pass before the lock is given up (default 1).
    void setMissLimit(int frames) { missLimit_ = frames < 1 ? 1 : frames; }
    // Start a new symbol stream (new I/Q hypothesis) without clearing the statistics.
    void restartStream();

    // Configuration, all documented defaults.
    void setPolarityInverted(bool inverted) { inverted_ = inverted; }
    bool polarityInverted() const { return inverted_; }

    // The constants above are at namespace scope so the Viterbi decoder can share them; these
    // mirror them for callers that have a LritDecoder in hand.
    static constexpr int kSyncBits = 32;      // 0x1ACFFC1D

    // The randomiser sequence of CCSDS 131.0-B-3, applied to the frame after the sync marker.
    static const std::vector<uint8_t>& randomiserBits();

private:
    void processPending();
    // Marker bit errors at a bit offset, for the better of the two polarities.
    int asmErrorsAt(std::size_t bitOffset, bool* inverted) const;
    void assembleAt(std::size_t bitOffset, bool inverted, int asmErrors, Cadu* out);
    void acceptCadu(Cadu&& cadu);
    // Iterative re-decode of a frame Reed-Solomon could not correct: codewords that do correct are
    // fed back into the Viterbi decoder as known bits (the four codewords are byte-interleaved, so
    // knowing one in four bytes pins the trellis for the rest), and the others are tried again.
    // Reads the buffers only (no decoder state), so several frames can be rescued at once.
    bool rescueFrame(std::size_t bitOffset, bool inverted, int asmErrors, std::vector<uint8_t>& plain,
                     std::chrono::steady_clock::time_point deadline) const;
    // One frame's bytes after derandomising and Reed-Solomon, and the rescue's result if it was run.
    struct FrameBody {
        bool valid = false;
        std::size_t bitOffset = 0;
        bool inverted = false;
        int asmErrors = 0;
        std::vector<uint8_t> plain;    // derandomised, Reed-Solomon applied when it could correct
        int rsFixed = -1;              // bytes Reed-Solomon corrected, negative when it could not
        bool rescueAttempted = false, rescued = false;
        std::vector<uint8_t> better;   // the rescued frame
    };
    FrameBody hardDecode(std::size_t bitOffset, bool inverted, int asmErrors) const;
    void rescueInto(FrameBody& frame, std::chrono::steady_clock::time_point deadline) const;
    void precomputeFrames();
    std::vector<FrameBody> frameCache_;   // filled and emptied inside one processPending()
    int rescueThreads_ = defaultRescueThreads();

    // Frame synchroniser. The 32-bit marker is searched for with up to kSearchMaxErrors bit errors
    // (a marker with errors is still a marker: at the SNR where Reed-Solomon just copes, one or
    // two of its 32 bits are often wrong, and the old exact match threw those frames away). A
    // candidate with errors is only believed when Reed-Solomon passes on it or the next marker is
    // where the frame length says it must be. Once locked, each frame is taken at the expected
    // position; its marker may carry up to kLockMaxErrors errors, and a frame whose marker is
    // unreadable is still handed to Reed-Solomon (flywheel). kMaxMisses frames in a row with
    // neither a marker nor a good Reed-Solomon check drop the lock back to search.
    static constexpr int kSearchMaxErrors = 4;
    static constexpr int kVerifyMaxErrors = 8;
    static constexpr int kLockMaxErrors = 8;
    static constexpr int kFlipMaxErrors = 2;    // complemented marker needed to change polarity in lock
    static constexpr int kMaxMisses = 4;   // how far back a lost lock rewinds (frames)
    // One frame that neither shows a marker nor passes Reed-Solomon gives the lock up and starts a
    // search from just after the last good frame. On real recordings the frame clock does move
    // (a frame boundary appeared 176..440 bits early in a Pluto capture, and data frames carry no
    // readable marker, so a stale clock decodes nothing). Holding the lock for four such frames,
    // as before, cost a fifth of the frames at low SNR; giving it up at once and re-finding the
    // next marker recovers them (3.2 MS/s recording at 0.2 dB: 3075 -> 3589 frames). Simulated
    // streams, whose clock never moves, are unaffected because their markers stay readable.
    int missLimit_ = 1;
    bool pairSlipEnabled_ = true;
    bool locked_ = false;
    bool lockInverted_ = false;
    int misses_ = 0;
    std::size_t lastGoodEnd_ = 0;   // bit offset just past the last frame believed to be real

    // Kept across calls. A Viterbi decoder carries the survivor metric between windows, and
    // creating a new one per feed resets it, so the windows no longer join and the bit stream
    // loses its alignment at every block boundary. That is a silent failure -- the symbols look
    // fine and the frames simply never sync -- which is why it is worth stating here.
    ViterbiDecoder viterbi_;
    std::vector<uint8_t> bitBuffer_;
    std::vector<std::int8_t> softBuf_;   // two quantised soft symbols per pair, pair k aligned with bitBuffer_[k] (may run ahead by the Viterbi delay)
    bool rescueEnabled_ = true;
    double rescueRatio_ = 0.5;
    int randomTrials_ = 0;
    int rescueFailStreak_ = 0, rescueBackoff_ = 0, rescueSkip_ = 0;
    double rescueSpent_ = 0.0;                                   // seconds spent in rescueFrame
    std::chrono::steady_clock::time_point rescueStart_ = std::chrono::steady_clock::now();
    // One soft value held over from a feed that ended on an odd symbol
    // count, because the rate-1/2 code pairs symbols two at a time.
    float pendingSoft_ = 0.0f;   // a symbol held over from an odd-length feed
    bool hasPending_ = false;    // whether pendingSoft_ holds one (a soft value of exactly 0 is legal)
    std::size_t consumed_ = 0;
    std::vector<Cadu> ready_;
    DecodeStats stats_;
    bool inverted_ = false;
    // Which two symbols form a code pair is not known from the symbols themselves: a demodulator
    // that starts one symbol earlier or later hands over the other pairing, and the Viterbi
    // decoder then outputs noise for as long as it runs. When no sync marker has been found for
    // kPairSlipSymbols symbols, one symbol is dropped so the other pairing is tried.
    static constexpr std::size_t kPairSlipSymbols = 3 * 1024 * 8 * 2;   // three CADUs
    std::size_t symbolsSinceSync_ = 0;
    bool dropNext_ = false;
};


// GK-2A HRIT data link on QPSK symbols (Mission Specification, HRIT, Table 9.1: NRZ-L/QPSK,
// 3 Mbps before the rate-1/2 K=7 code, RS(255,223) depth 4, 1024-byte CADU, no G2 inversion).
//
// Each QPSK symbol carries one code pair, G1 on I and G2 on Q. The carrier loop locks at any of
// four phases, and a receiver with swapped I/Q sees the spectrum inverted. 180 degrees only
// complements every bit, which the frame layer already handles, so four mappings remain:
//   0: ( I,  Q)   1: ( Q, -I)  [90 degrees]   2: ( I, -Q)   3: ( Q,  I)  [spectrum inverted]
// Without sync for kHypothesisSymbols the next one is tried; once frames arrive it stays.
class QpskLinkDecoder {
public:
    QpskLinkDecoder();
    // Symbols scaled so mean |I| = mean |Q| = 1 (QpskDemod's output).
    void feedSymbols(const std::vector<std::complex<float>>& symbols);
    std::vector<Cadu> takeCadus() { return dec_.takeCadus(); }
    const DecodeStats& stats() const { return dec_.stats(); }
    int hypothesis() const { return hypothesis_; }
    bool frameLocked() const { return dec_.frameLocked(); }
    // A mapping confirmed by Reed-Solomon is held through gaps of up to kHoldSymbols (about 80 ms
    // at 3 Mbaud), so a fade does not throw away a validated mapping.
    bool mappingHeld() const { return everLocked_ && symbolsSinceGood_ <= kHoldSymbols; }
    void reset();
    // Soft scale applied before the Viterbi clip at |1|: 0.5 keeps noisy symbols out of clipping
    // (same tuning as the LRIT path).
    void setSoftScale(float s) { scale_ = s; }
    void setRescueEnabled(bool on) { dec_.setRescueEnabled(on); }
    void setRescueBudget(double ratio) { dec_.setRescueBudget(ratio); }
    void setRandomTrials(int n) { dec_.setRandomTrials(n); }
    void setRescueThreads(int n) { dec_.setRescueThreads(n); }
    void setMissLimit(int frames) { dec_.setMissLimit(frames); }

private:
    LritDecoder dec_;
    int hypothesis_ = 0;
    bool everLocked_ = false;
    std::size_t symbolsSinceGood_ = 0;
    std::size_t goodBefore_ = 0;
    float scale_ = 0.5f;
    static constexpr std::size_t kHypothesisSymbols = 6 * 8192;   // six CADUs
    static constexpr std::size_t kHoldSymbols = 250000;   // ~80 ms at 3 Msym/s; then the mapping search resumes
};

// Derandomise in place, as the data link layer requires before the frame can be read. The
// sequence starts at the beginning of the transfer frame, i.e. after the four sync bytes.
void derandomise(std::vector<uint8_t>& frame);

// Reed-Solomon syndrome check for RS(255/223) with interleaving depth 4, over GF(2^8) with the
// CCSDS primitive polynomial x^8+x^7+x^2+x+1 (0x187) and first consecutive root 112.
//
// It answers "is this CADU intact", which is what decides whether a frame can be trusted. Frames
// are passed through rsCorrect first; a frame the code cannot correct is left as
// it arrived and fails this check, so it is still reported rather than silently repaired.
bool rsSyndromesZero(const std::vector<uint8_t>& cadu, int interleaveDepth);

// Reed-Solomon correction for the same code. Corrects each of the interleaved codewords (up to 16
// byte errors each) and returns the number of bytes changed, 0 for an intact CADU, or -1 when any
// codeword is beyond the code -- in which case the CADU is left exactly as it was. The result is
// re-checked with rsSyndromesZero before it is accepted.
int rsCorrect(std::vector<uint8_t>& cadu, int interleaveDepth);

// Convenience: the specification's frame length and offsets.
constexpr std::size_t kCaduPayloadBytes = kCaduBytes - 4;   // 1020

}  // namespace nadirstation::gk2a
