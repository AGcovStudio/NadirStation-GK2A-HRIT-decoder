#pragma once
#include <complex>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <cmath>
#include <deque>

namespace nadirstation::dsp {

struct SpectrumResult {
    std::vector<float> db;
    float peakDb = -120.0f;
    float noiseDb = -120.0f;
    float snrDb = 0.0f;
};

enum class SpectrumWindow : int { Hann = 0, Hamming, Blackman, Nuttall, Rectangular };

std::vector<std::complex<float>> lowpass2(const std::vector<std::complex<float>>& in,
                                          float sampleRate, float cutoffHz);
SpectrumResult spectrum(const std::vector<std::complex<float>>& in, float sampleRate,
                        std::size_t fftSize = 1024,
                        SpectrumWindow window = SpectrumWindow::Hann);

struct DPSKResult {
    std::vector<uint8_t> bits;
    std::vector<float> soft;
    std::vector<std::complex<float>> symbols;
    float carrierOffsetHz = 0.0f;
    float timingQuality = 0.0f;
};

// Lightweight display-only raw-IQ BPSK preview. It never feeds the decoder.
std::vector<std::complex<float>> fastBpskConstellation(const std::vector<std::complex<float>>& iq,
                                                        float sampleRate,
                                                        float bitRate = 660000.0f,
                                                        std::size_t maxPoints = 1200);

// BPSK demodulator for the FY-2 S-VISSR and GK-2A LRIT downlinks. This project's own design,
// from textbook parts:
//
//   1. integer decimation to at least 4 samples per symbol (windowed-sinc low-pass);
//   2. power AGC;
//   3. carrier NCO ahead of the matched filter, steered from the symbol decisions;
//   4. root-raised-cosine matched filter;
//   5. symbol timing: Gardner detector (Gardner, IEEE Trans. Commun. 34(5), 1986) on two
//      interpolants per symbol from a cubic Lagrange (Farrow) interpolator (Erup, Gardner & Harris,
//      IEEE Trans. Commun. 41(6), 1993), with a proportional-integral loop;
//   6. carrier: decision-directed BPSK phase detector Im(y)*sign(Re(y)) at the symbol instant,
//      second-order loop, applied to the NCO;
//   0. acquisition: before the loop runs, the carrier offset is measured from the spectrum of the
//      squared signal (BPSK squared is an unmodulated line at twice the offset), so the loop can be
//      narrow and still start within a few hertz. If the lock measure stays low for a long time
//      (a slip, a retune) it measures again.
//
// The interface (configure / reset / process / setInitialCarrierOffset and the rate accessors)
// is the one the decoder and the tools already use.
class PskDemod {
public:
    PskDemod() = default;
    PskDemod(float sampleRate, float symbolRate = 660000.0f, float rrcAlpha = 0.5f, float loopBandwidth = 0.02f);

    void configure(float sampleRate, float symbolRate = 660000.0f, float rrcAlpha = 0.5f, float loopBandwidth = 0.02f);
    // Carrier loop start frequency, radians per sample at the demodulator rate (sampleRate()).
    void setInitialCarrierOffset(float radiansPerSample) { initialCarrierOffset_ = radiansPerSample; }
    void setQpsk(bool enabled) { qpsk_ = enabled; }
    // Matched filter for an unfiltered (rectangular NRZ) transmitter: integrate-and-dump over one
    // symbol instead of an RRC. Takes effect at the next configure().
    void setRectangularPulse(bool enabled) { rectPulse_ = enabled; }
    void reset();
    DPSKResult process(const std::vector<std::complex<float>>& iq);
    // process() with the two stages run one after the other on the calling thread: identical output, and faster
    // because each stage is a tight loop over a whole block.
    DPSKResult processSplit(const std::vector<std::complex<float>>& iq);

    float sampleRate() const { return rate_; }
    float inputSampleRate() const { return inRate_; }
    int inputDecimation() const { return decim_; }
    int inputUpsampling() const { return up_; }
    float symbolRate() const { return symbolRate_; }
    // Mean of cos(2*phase error) over recent symbols: about 1 when locked, about 0 when not.
    float lockMeasure() const { return static_cast<float>(lock_); }
    // Number of carrier measurements made so far (the first one, then one per lost lock).
    int acquisitions() const { return acquisitions_; }

private:
    void buildFilters();
    std::complex<float> interpolate(double t) const;
    void acquire();
    void track(std::complex<float> z, DPSKResult& out);
    std::vector<std::complex<float>> front(const std::vector<std::complex<float>>& iq);   // stage 1: filters + AGC
    DPSKResult back(const std::vector<std::complex<float>>& agc);                          // stage 2: carrier / timing loops

    float inRate_ = 6000000.0f, rate_ = 6000000.0f, symbolRate_ = 660000.0f;
    float rrcAlpha_ = 0.5f, loopBw_ = 0.02f;
    int decim_ = 1;
    // Integer upsampling ahead of the chain when the input has fewer than three samples a symbol
    // (a 1.5 MS/s FY-2 recording is 2.27). The cubic timing interpolator is not accurate enough
    // that sparse and slips symbols, which breaks every S-VISSR frame; see configure().
    int up_ = 1;
    std::vector<float> upTaps_;
    std::vector<std::complex<float>> upHist_;   // twice upLen_ long: history is stored at pos and pos+upLen_
    std::vector<float> upPoly_;                 // per-phase taps, oldest sample first
    std::size_t upLen_ = 0, upStride_ = 0;
    std::vector<float> upPolyDup_;              // SSE copy of upPoly_: every tap twice, rows padded to 4 floats
    std::vector<float> mfDup_;                  // SSE copy of rrc_: every tap twice, padded to 4 floats
    std::size_t upPos_ = 0;
    void consume(std::complex<float> z, DPSKResult& out);
    double sps_ = 4.0;
    bool configured_ = false;
    bool qpsk_ = false;
    bool rectPulse_ = false;
    float initialCarrierOffset_ = 0.0f;

    // decimator
    std::vector<float> aaTaps_;
    std::vector<std::complex<float>> aaHist_;
    std::size_t aaPos_ = 0;
    int aaPhase_ = 0;

    // AGC and carrier NCO
    float power_ = 1.0f;
    double phase_ = 0.0, freq_ = 0.0;      // rad / sample at the demodulator rate
    double cKp_ = 0.0, cKi_ = 0.0;         // per symbol

    // matched filter
    std::vector<float> rrc_;
    std::vector<std::complex<float>> mfHist_;
    std::size_t mfPos_ = 0;

    // matched-filter output kept for interpolation; buf_[0] is sample number base_
    std::vector<std::complex<float>> buf_;
    double base_ = 0.0;
    double next_ = 2.0;                    // next interpolation instant (absolute sample number)
    double period_ = 4.0;                  // samples per symbol, tracked
    bool midNext_ = false;                 // next interpolant is the mid-symbol one
    std::complex<float> prevSym_{0, 0}, mid_{0, 0};
    double tKp_ = 0.0, tKi_ = 0.0;
    double ampAvg_ = 1.0;

    // acquisition
    std::vector<std::complex<float>> acqBuf_;
    std::size_t acqLength_ = 32768;
    bool acquiring_ = true;
    double lock_ = 0.0;
    long long unlockedSymbols_ = 0;
    int acquisitions_ = 0;
};


// QPSK demodulator for GK-2A HRIT (3 Msym/s, RRC 0.5). Built for low SNR and low sample rates:
//
//   0. acquisition: carrier offset from the spectrum of the fourth power of the signal (QPSK to
//      the fourth is an unmodulated line at four times the offset), refined parabolically;
//   1. optional integer decimation to 2.5..5 samples per symbol;
//   2. power AGC and a carrier NCO;
//   3. polyphase matched filter: the RRC is evaluated at any fractional instant from a 256-phase
//      table, so the matched filter and the timing interpolator are one operation and work at
//      2 samples per symbol (6 MS/s for HRIT) without the error of a cubic interpolator;
//   4. Gardner timing (two evaluations per symbol), PI loop;
//   5. decision-directed QPSK carrier loop: sign(I)*Q - sign(Q)*I, second order.
// Output symbols are scaled so that mean |I| = mean |Q| = 1.
class QpskDemod {
public:
    QpskDemod() = default;
    void configure(float sampleRate, float symbolRate = 3000000.0f, float rrcAlpha = 0.5f,
                   float carrierLoopBw = 0.004f, float timingLoopBw = 0.0008f);
    void setInitialCarrierOffset(float radiansPerSample) { initialCarrierOffset_ = radiansPerSample; }
    void reset();
    DPSKResult process(const std::vector<std::complex<float>>& iq);

    float sampleRate() const { return rate_; }
    float inputSampleRate() const { return inRate_; }
    float symbolRate() const { return symbolRate_; }
    float carrierOffsetHz() const { return static_cast<float>(freq_ * rate_ / 6.283185307179586); }
    // Mean of cos(4 * phase error): about 1 when locked, about 0 when not.
    float lockMeasure() const { return static_cast<float>(lock_); }
    int acquisitions() const { return acquisitions_; }
    // Es/N0 (dB) the sample-rate-limited receive filter is designed for; call before configure().
    void setMmseDesignSnrDb(double db) { mmseDesignSnrDb_ = db; }

private:
    void acquire();
    void track(std::complex<float> z, DPSKResult& out);
    std::complex<float> mf(double t) const;

    // Below 1.5 samples a symbol (RTL-SDR at 3.2 MS/s for the 3 Msym/s HRIT link) the tuner's IF
    // filter and the sampling cut away the roll-off, so Gardner has almost nothing to work from and
    // its integrator runs away (seen on a real recording: lock for about 5 s, then period at the clamp).
    // Mueller & Muller works from the decisions and needs no excess bandwidth. The RTL-SDR's DC spike
    // also sits in the middle of the signal and its 4th power line fools the carrier search, so a
    // slow DC blocker runs ahead of everything at these rates.
    bool lowRate_ = false;
    double mmseDesignSnrDb_ = 0.0;                // design Es/N0 of the low-rate receive filter
    double clampFrac_ = 0.005;                    // period limit, fraction of the nominal period
    std::complex<float> dc_{0.0f, 0.0f};
    float inRate_ = 6e6f, rate_ = 6e6f, symbolRate_ = 3e6f, alpha_ = 0.5f;
    float cBw_ = 0.004f, tBw_ = 0.0008f;
    int decim_ = 1;
    double sps_ = 2.0;
    bool configured_ = false;
    float initialCarrierOffset_ = 0.0f;

    std::vector<float> aaTaps_;
    std::vector<std::complex<float>> aaHist_;
    std::size_t aaPos_ = 0;
    int aaPhase_ = 0;

    float power_ = 1.0f;
    double phase_ = 0.0, freq_ = 0.0, cKp_ = 0.0, cKi_ = 0.0, tKp_ = 0.0, tKi_ = 0.0;
    // The carrier NCO as a rotating phasor instead of cos/sin per sample; re-derived from phase_
    // and freq_ every kNcoResync samples so rounding and the small-angle updates cannot drift.
    std::complex<double> rot_{1.0, 0.0}, step_{1.0, 0.0};
    int ncoCount_ = 0;
    static constexpr int kNcoResync = 256;

    int half_ = 8;                      // matched filter half span, samples
    static constexpr int kPhases = 256;
    std::vector<float> table_;          // RRC at tau = -(half+1) .. (half+1), step 1/kPhases
    // The same filter laid out for evaluation: row r (0..kPhases) holds the 2*half+2 taps for
    // fractional position r/kPhases, every tap repeated for the re and im lanes, contiguous.
    std::vector<float> poly_;
    float agcScale_ = 1.0f;
    int agcCount_ = 0;

    std::vector<std::complex<float>> buf_;   // NCO-corrected samples; buf_[0] is sample base_
    double base_ = 0.0;
    double next_ = 0.0;                 // next on-time instant (absolute sample number)
    double period_ = 2.0;
    std::complex<float> prevSym_{0, 0};
    double ampAvg_ = 0.0;

    std::vector<std::complex<float>> acqBuf_;
    std::size_t acqLength_ = 65536;
    bool acquiring_ = true;
    double lock_ = 0.0;
    long long unlockedSymbols_ = 0;
    int acquisitions_ = 0;
};

// The previous demodulator's name, kept so the command-line tools build unchanged. It now refers to
// PskDemod; the old implementation it used to name has been removed.

// Compatibility wrapper used by small offline tests. It creates a fresh demodulator for the
// supplied contiguous vector.
DPSKResult differentialDpsk660k(const std::vector<std::complex<float>>& iq,
                                 float sampleRate,
                                 float bitRate = 660000.0f,
                                 int polarity = 0,
                                 uint8_t* nrzmLastBit = nullptr);

}
