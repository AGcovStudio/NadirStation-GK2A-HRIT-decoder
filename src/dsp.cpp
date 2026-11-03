#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include "dsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>

namespace nadirstation::dsp {
namespace {
constexpr float PI = 3.14159265358979323846f;

void fft(std::vector<std::complex<float>>& a) {
    // Twiddles from a table computed once per size in double precision. The previous recurrence
    // (w *= wlen) accumulated rounding over each butterfly group, which at 8192-16384 points
    // raised the leakage floor of the displayed spectrum; the table also saves the per-call cos/sin.
    const std::size_t n = a.size();
    thread_local std::size_t tableSize = 0;
    thread_local std::vector<std::complex<float>> twiddle;   // e^{-2 pi i k / n}, k < n/2
    if (tableSize != n) {
        twiddle.resize(n / 2);
        for (std::size_t k = 0; k < n / 2; ++k) {
            const double ang = -2.0 * 3.141592653589793238462643383279502884 * static_cast<double>(k) / static_cast<double>(n);
            twiddle[k] = std::complex<float>(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        }
        tableSize = n;
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len >> 1;
        const std::size_t stride = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + half] * twiddle[j * stride];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
}

float median(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

std::vector<std::complex<float>> fir(const std::vector<std::complex<float>>& in,
                                     const std::vector<float>& h) {
    // The RRC is an odd-length linear-phase filter, so h[k] == h[N-1-k].
    // Exploit that symmetry once the full tap history is available: 31 complex
    // multiplies become 16. This is a substantial CPU reduction on 4 MiS windows
    // without changing the filter response.
    const std::size_t n = in.size();
    const std::size_t taps = h.size();
    std::vector<std::complex<float>> out(n);
    if (n == 0 || taps == 0) return out;
    const std::size_t half = taps / 2;
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 1 < taps) {
            std::complex<float> acc{0, 0};
            const std::size_t maxK = std::min<std::size_t>(taps, i + 1);
            for (std::size_t k = 0; k < maxK; ++k) acc += in[i - k] * h[k];
            out[i] = acc;
            continue;
        }
        std::complex<float> acc = in[i - half] * h[half];
        for (std::size_t k = 0; k < half; ++k) {
            acc += (in[i - k] + in[i - (taps - 1 - k)]) * h[k];
        }
        out[i] = acc;
    }
    return out;
}


struct TimedSymbols {
    std::vector<std::complex<float>> value;
    float omega = 0.0f;
    float timingError = 0.0f;
};

}

std::vector<std::complex<float>> lowpass2(const std::vector<std::complex<float>>& in,
                                          float sampleRate, float cutoffHz) {
    if (in.empty()) return {};
    const float norm = std::clamp(cutoffHz / sampleRate, 0.0001f, 0.49f);
    const int taps = 65;
    std::vector<float> h(taps);
    const int m = taps / 2;
    float sum = 0.0f;
    for (int n = 0; n < taps; ++n) {
        const float x = static_cast<float>(n - m);
        const float v = (std::fabs(x) < 1e-6f) ? 2.0f * norm : std::sin(2.0f * PI * norm * x) / (PI * x);
        const float w = 0.54f - 0.46f * std::cos(2.0f * PI * n / static_cast<float>(taps - 1));
        h[n] = v * w;
        sum += h[n];
    }
    for (float& v : h) v /= sum;
    return fir(in, h);
}

SpectrumResult spectrum(const std::vector<std::complex<float>>& in, float sampleRate, std::size_t fftSize,
                        SpectrumWindow window) {
    SpectrumResult r;
    if (fftSize < 2 || in.size() < fftSize || (fftSize & (fftSize - 1))) return r;   // 0 used to pass the power-of-two test and crash
    // The window depends only on its size and shape, so it is computed once and reused (the same
    // formulas as before, evaluated per call until now).
    thread_local std::size_t windowSize = 0;
    thread_local SpectrumWindow windowShape = SpectrumWindow::Rectangular;
    thread_local std::vector<float> windowTable;
    if (windowSize != fftSize || windowShape != window || windowTable.size() != fftSize) {
        windowTable.assign(fftSize, 1.0f);
        for (std::size_t i = 0; i < fftSize; ++i) {
            const float phase = 2.0f * PI * i / static_cast<float>(fftSize - 1);
            float w = 1.0f;
            switch (window) {
            case SpectrumWindow::Hann: w = 0.5f - 0.5f * std::cos(phase); break;
            case SpectrumWindow::Hamming: w = 0.54f - 0.46f * std::cos(phase); break;
            case SpectrumWindow::Blackman: w = 0.42f - 0.5f * std::cos(phase) + 0.08f * std::cos(2.0f * phase); break;
            case SpectrumWindow::Nuttall:
                w = 0.355768f - 0.487396f * std::cos(phase) + 0.144232f * std::cos(2.0f * phase) - 0.012604f * std::cos(3.0f * phase); break;
            case SpectrumWindow::Rectangular: break;
            }
            windowTable[i] = w;
        }
        windowSize = fftSize;
        windowShape = window;
    }
    std::vector<std::complex<float>> a(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i) {
        const std::complex<float> x = in[in.size() - fftSize + i];
        // One non-finite sample would make every bin NaN and the display blank.
        a[i] = (std::isfinite(x.real()) && std::isfinite(x.imag())) ? x * windowTable[i] : std::complex<float>{};
    }
    fft(a);
    r.db.resize(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i) {
        const std::size_t k = (i + fftSize / 2) % fftSize;
        const float p = std::norm(a[k]) / static_cast<float>(fftSize * fftSize);
        r.db[i] = 10.0f * std::log10(std::max(p, 1e-14f));
    }
    r.peakDb = *std::max_element(r.db.begin(), r.db.end());
    const float hzPerBin = sampleRate / static_cast<float>(fftSize);
    // Input SNR from the spectrum: the noise floor is the median bin (most of a wide capture is
    // noise), and the signal is everything that stands more than 3 dB above it after a 5-bin
    // smoothing. The figure is the signal power over the noise power in the same occupied bins,
    // i.e. the SNR in the bandwidth the signal actually uses. A moment-based estimate over the raw
    // wideband samples, used before, measured mostly the noise outside the signal and read near
    // 0 dB for any narrow signal.
    std::vector<float> lin(fftSize);
    for (std::size_t i = 0; i < fftSize; ++i) lin[i] = std::pow(10.0f, r.db[i] / 10.0f);
    std::vector<float> sorted(lin);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(fftSize / 2), sorted.end());
    const double floorPower = std::max(1e-14f, sorted[fftSize / 2]);
    r.noiseDb = static_cast<float>(10.0 * std::log10(floorPower));
    double signalPower = 0.0;
    std::size_t occupied = 0;
    for (std::size_t i = 0; i < fftSize; ++i) {
        double smooth = 0.0;
        int count = 0;
        for (std::size_t k = (i >= 2 ? i - 2 : 0); k <= std::min(fftSize - 1, i + 2); ++k) { smooth += lin[k]; ++count; }
        smooth /= count;
        if (smooth > 2.0 * floorPower) {
            signalPower += lin[i] - floorPower;
            ++occupied;
        }
    }
    r.snrDb = (occupied > 0 && signalPower > 0.0)
        ? static_cast<float>(std::max(0.0, 10.0 * std::log10(signalPower / (floorPower * static_cast<double>(occupied)))))
        : 0.0f;
    (void)hzPerBin;
    return r;
}

std::vector<std::complex<float>> fastBpskConstellation(const std::vector<std::complex<float>>& iq,
                                                        float sampleRate,
                                                        float bitRate,
                                                        std::size_t maxPoints) {
    if (iq.size() < 64 || sampleRate <= 0.0f || bitRate <= 0.0f || maxPoints == 0) return {};
    const float sps = sampleRate / bitRate;
    if (sps < 1.5f) return {};

    const std::size_t window = std::min<std::size_t>(iq.size(), 65536);
    const std::size_t start = iq.size() - window;

    // Estimate BPSK carrier phase from z^2. The average of z^2 removes the
    // 180-degree data ambiguity and gives a stable visual rotation estimate.
    std::complex<float> phase2{0.0f, 0.0f};
    float meanRe = 0.0f, meanIm = 0.0f;
    for (std::size_t i = start; i < iq.size(); ++i) {
        meanRe += iq[i].real();
        meanIm += iq[i].imag();
        const auto z = iq[i];
        phase2 += z * z;
    }
    const float invN = 1.0f / static_cast<float>(window);
    const float meanMag = std::hypot(meanRe * invN, meanIm * invN);
    if (std::abs(phase2) < 1e-8f) return {};
    const float phase = -0.5f * std::atan2(phase2.imag(), phase2.real());
    const std::complex<float> rot(std::cos(phase), std::sin(phase));

    const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(sps)));
    const std::size_t count = std::min(maxPoints, window / step);
    std::vector<std::complex<float>> out;
    out.reserve(count);
    const std::size_t stride = std::max<std::size_t>(1, (window / step) / std::max<std::size_t>(1, count));
    for (std::size_t n = 0; n < window && out.size() < count; n += stride * step) {
        const std::size_t idx = start + std::min(n, window - 1);
        auto z = iq[idx] * rot;
        if (meanMag > 1e-5f) z /= meanMag;
        // Clamp only for the GUI; the actual decoder never sees these points.
        const float mag = std::abs(z);
        if (mag > 3.0f) z *= 3.0f / mag;
        out.push_back(z);
    }
    return out;
}


// ---- PskDemod ---------------------------------------------------------------------------
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kOutputScale = 1.0;   // symbol amplitude after normalisation (mean |Re| = 1)
#ifndef PSK_MIN_SPS
#define PSK_MIN_SPS 4.0f
#endif
#ifndef PSK_TIMING_SIGN
#define PSK_TIMING_SIGN 1.0
#endif
// Loop bandwidths, normalised to the symbol rate. Chosen on a GK-2A recording with added noise
// (scratch A/B, 2026-10-18): narrower than the textbook defaults because acquisition no longer
// depends on the loop pulling in, so the loops only have to follow drift.
// Timing loop narrowed from 0.002 to 0.0005 (2026-10-26): on a synthetic BPSK stream the wider loop
// lost the symbol clock below about -4 dB Es/N0, the narrow one holds it to -8.5 dB, and the clock
// of a real receiver drifts far more slowly than 0.0005 of the symbol rate can follow.
#ifndef PSK_TIMING_BW
#define PSK_TIMING_BW 0.0005
#endif
#ifndef PSK_CBW_SCALE
#define PSK_CBW_SCALE 0.1
#endif
constexpr double kLockAlpha = 1.0 / 2000.0;     // lock measure averaging, per symbol
// The lock measure of a locked loop falls with the signal-to-noise ratio (about 0.06 at -7 dB
// Es/N0), while a spinning loop averages 0 with a spread of about 0.011. 0.1 sat inside the locked
// range at low SNR and forced a needless re-acquisition every 50000 symbols; 0.04 is 3.5 spreads
// above zero.
#ifndef PSK_UNLOCKED_LEVEL
#define PSK_UNLOCKED_LEVEL 0.04
#endif
constexpr double kUnlockedLevel = PSK_UNLOCKED_LEVEL;
constexpr long long kUnlockedSymbolsBeforeReacquire = 50000;

void fftInPlace(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void loopGains(double bw, double& kp, double& ki) {
    const double zeta = 0.70710678;
    const double d = 1.0 + 2.0 * zeta * bw + bw * bw;
    kp = 4.0 * zeta * bw / d;
    ki = 4.0 * bw * bw / d;
}

std::vector<float> windowedSinc(int taps, double cutoff) {   // cutoff in cycles per sample
    std::vector<float> h(static_cast<std::size_t>(taps));
    double sum = 0.0;
    const int mid = taps / 2;
    for (int i = 0; i < taps; ++i) {
        const double m = i - mid;
        const double s = m == 0 ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * m) / (kPi * m);
        const double w = taps > 1 ? 0.42 - 0.5 * std::cos(2.0 * kPi * i / (taps - 1)) + 0.08 * std::cos(4.0 * kPi * i / (taps - 1)) : 1.0;
        h[static_cast<std::size_t>(i)] = static_cast<float>(s * w);
        sum += s * w;
    }
    for (auto& v : h) v = static_cast<float>(v / sum);
    return h;
}

// Root-raised-cosine impulse response, unit DC gain.
std::vector<float> rootRaisedCosine(double sps, double alpha, int taps) {
    std::vector<float> h(static_cast<std::size_t>(taps));
    const int mid = taps / 2;
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double t = (i - mid) / sps;
        double v;
        if (std::fabs(t) < 1e-9) {
            v = 1.0 - alpha + 4.0 * alpha / kPi;
        } else if (std::fabs(std::fabs(4.0 * alpha * t) - 1.0) < 1e-9) {
            v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
        } else {
            v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) /
                (kPi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        }
        h[static_cast<std::size_t>(i)] = static_cast<float>(v);
        sum += v;
    }
    for (auto& v : h) v = static_cast<float>(v / sum);
    return h;
}
}  // namespace

PskDemod::PskDemod(float sampleRate, float symbolRate, float rrcAlpha, float loopBandwidth) {
    configure(sampleRate, symbolRate, rrcAlpha, loopBandwidth);
}

void PskDemod::configure(float sampleRate, float symbolRate, float rrcAlpha, float loopBandwidth) {
    if (sampleRate <= 0.0f || symbolRate <= 0.0f) return;
    inRate_ = sampleRate;
    symbolRate_ = symbolRate;
    rrcAlpha_ = std::clamp(rrcAlpha, 0.05f, 1.0f);
    loopBw_ = std::clamp(loopBandwidth, 0.0005f, 0.25f);
    const int bySymbol = std::max(1, static_cast<int>(std::floor(sampleRate / (symbolRate * PSK_MIN_SPS))));
    const int byCeiling = sampleRate > 6.0e6f ? std::max(1, static_cast<int>(std::floor(sampleRate / 6.0e6f))) : 1;
    decim_ = qpsk_ ? bySymbol : std::max(bySymbol, byCeiling);
    // Below three samples a symbol the cubic interpolator in the timing loop is too coarse: at
    // 1.5 MS/s for FY-2 (2.27 samples a symbol) it dropped about one symbol in 65 000, so every
    // 400 000-symbol S-VISSR frame was broken and nothing ever synchronised, although the symbols
    // themselves were right. Upsample by the smallest integer that gives at least four instead.
    // Rates at or above three samples a symbol are untouched.
    up_ = 1;
    if (!qpsk_ && sampleRate < 3.0f * symbolRate) {
        up_ = std::max(2, static_cast<int>(std::ceil(4.0 * symbolRate / sampleRate)));
        decim_ = 1;
    }
    rate_ = inRate_ * static_cast<float>(up_) / static_cast<float>(decim_);
    sps_ = static_cast<double>(rate_) / symbolRate_;
    buildFilters();
    reset();
    configured_ = true;
}

void PskDemod::buildFilters() {
    if (decim_ > 1) {
        // Only what would alias onto the signal has to go: the signal spans +-(1+alpha)*Rs/2 and an
        // alias lands on it from rate - (1+alpha)*Rs/2 upwards. So the cutoff sits at the decimated
        // Nyquist frequency, with the transition spread across that gap; the matched filter removes
        // the rest. (A cutoff at the signal edge, as first written, needed far more taps to keep its
        // passband flat and cost about 0.3 dB on FY-2.)
        const double edge = 0.5 * (1.0 + rrcAlpha_) * symbolRate_;
        const double transition = std::max(1e-3, (static_cast<double>(rate_) - 2.0 * edge) / inRate_);
        const int taps = std::clamp(static_cast<int>(std::ceil(6.0 / transition)), 4 * decim_ + 1, 255) | 1;
        aaTaps_ = windowedSinc(taps, 0.5 / decim_);
    } else {
        aaTaps_.assign(1, 1.0f);
    }
    if (up_ > 1) {
        // Interpolation filter for zero-stuffing by up_: pass the original band, cut at the
        // original Nyquist frequency; gain up_ so the stuffed zeros do not lower the level.
        const int taps = (16 * up_) | 1;
        upTaps_ = windowedSinc(taps, 0.5 / up_);
        for (auto& v : upTaps_) v *= static_cast<float>(up_);
        upLen_ = (upTaps_.size() + static_cast<std::size_t>(up_) - 1) / static_cast<std::size_t>(up_);
        // upPoly_[p][j] = tap of phase p applied to the j-th oldest of the last upLen_ input samples.
        upPoly_.assign(static_cast<std::size_t>(up_) * upLen_, 0.0f);
        for (int p = 0; p < up_; ++p)
            for (std::size_t k = 0; k < upLen_; ++k) {
                const std::size_t t = static_cast<std::size_t>(p) + k * static_cast<std::size_t>(up_);
                if (t < upTaps_.size()) upPoly_[static_cast<std::size_t>(p) * upLen_ + (upLen_ - 1 - k)] = upTaps_[t];
            }
        upStride_ = (2 * upLen_ + 3) / 4 * 4;
        upPolyDup_.assign(static_cast<std::size_t>(up_) * upStride_, 0.0f);
        for (int p = 0; p < up_; ++p)
            for (std::size_t j = 0; j < upLen_; ++j) {
                const float v = upPoly_[static_cast<std::size_t>(p) * upLen_ + j];
                upPolyDup_[static_cast<std::size_t>(p) * upStride_ + 2 * j] = v;
                upPolyDup_[static_cast<std::size_t>(p) * upStride_ + 2 * j + 1] = v;
            }
    } else {
        upTaps_.clear();
        upPoly_.clear();
        upLen_ = 0;
    }
    const int half = static_cast<int>(std::lround(4.0 * sps_));
    if (rectPulse_) {
        // Integrate-and-dump: each tap is the overlap of its sample cell with a one-symbol window,
        // so a fractional samples-per-symbol is matched exactly. Odd length and symmetric.
        const int h = static_cast<int>(std::ceil(0.5 * sps_ + 0.5));
        rrc_.assign(static_cast<std::size_t>(2 * h + 1), 0.0f);
        double sum = 0.0;
        for (int k = -h; k <= h; ++k) {
            const double lo = std::max(k - 0.5, -0.5 * sps_), hi = std::min(k + 0.5, 0.5 * sps_);
            const double w = std::max(0.0, hi - lo);
            rrc_[static_cast<std::size_t>(k + h)] = static_cast<float>(w);
            sum += w;
        }
        for (auto& v : rrc_) v = static_cast<float>(v / sum);
    } else {
        rrc_ = rootRaisedCosine(sps_, rrcAlpha_, 2 * half + 1);
    }
    mfDup_.assign((2 * rrc_.size() + 3) / 4 * 4, 0.0f);
    for (std::size_t j = 0; j < rrc_.size(); ++j) mfDup_[2 * j] = mfDup_[2 * j + 1] = rrc_[j];
    loopGains(loopBw_ * PSK_CBW_SCALE, cKp_, cKi_);
    loopGains(PSK_TIMING_BW, tKp_, tKi_);
}

void PskDemod::reset() {
    upHist_.assign(up_ > 1 ? 2 * upLen_ + 4 : 0, {0, 0});
    upPos_ = 0;
    aaHist_.assign(aaTaps_.size(), {0, 0});
    aaPos_ = 0;
    aaPhase_ = 0;
    power_ = 1.0f;
    phase_ = 0.0;
    freq_ = initialCarrierOffset_;
    mfHist_.assign(2 * rrc_.size() + 4, {0, 0});
    mfPos_ = 0;
    buf_.clear();
    base_ = 0.0;
    next_ = 2.0;
    period_ = sps_;
    midNext_ = false;
    prevSym_ = mid_ = {0, 0};
    ampAvg_ = 0.0;
    acqBuf_.clear();
    acquiring_ = true;
    lock_ = 0.0;
    unlockedSymbols_ = 0;
}

// Carrier offset from the squared signal. Squaring removes the BPSK modulation and leaves a line at
// twice the offset; the block mean is removed first so a DC spur (LO leakage) does not square into
// a line at zero. Parabolic interpolation on the log magnitude refines the peak bin.
void PskDemod::acquire() {
    const std::size_t n = acqBuf_.size();
    std::complex<double> mean{0, 0};
    for (const auto& z : acqBuf_) mean += std::complex<double>(z);
    mean /= static_cast<double>(n);
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const std::complex<double> z = std::complex<double>(acqBuf_[i]) - mean;
        a[i] = (qpsk_ ? z * z * z * z : z * z) * w;
    }
    fftInPlace(a);
    std::size_t best = 0;
    double bestPower = -1.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double p = std::norm(a[k]);
        if (p > bestPower) { bestPower = p; best = k; }
    }
    auto logPower = [&](std::size_t k) { return std::log(std::max(std::norm(a[k % n]), 1e-300)); };
    const double l = logPower(best + n - 1), c = logPower(best), r = logPower(best + 1);
    const double den = l - 2.0 * c + r;
    const double delta = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    double bin = static_cast<double>(best) + delta;
    if (bin > static_cast<double>(n) / 2.0) bin -= static_cast<double>(n);
    // Line at 2*offset, in cycles per sample -> offset in radians per sample.
    freq_ = 2.0 * kPi * (bin / static_cast<double>(n)) / (qpsk_ ? 4.0 : 2.0);
    phase_ = 0.0;
    // A carrier reacquisition often follows a signal fade. During the fade the timing loop
    // continues to see AGC-normalised noise, so its period, interpolator history and matched
    // filter state may no longer describe the returning signal. Re-prime those states from the
    // nominal symbol clock before replaying the buffered acquisition window below.
    std::fill(mfHist_.begin(), mfHist_.end(), std::complex<float>{0.0f, 0.0f});
    mfPos_ = 0;
    buf_.clear();
    base_ = 0.0;
    next_ = 2.0;
    period_ = sps_;
    midNext_ = false;
    prevSym_ = mid_ = {0.0f, 0.0f};
    ampAvg_ = 0.0;
    lock_ = 0.0;
    unlockedSymbols_ = 0;
    ++acquisitions_;
}

std::complex<float> PskDemod::interpolate(double t) const {
    const double ft = static_cast<double>(static_cast<long long>(t));   // t > 0
    const std::size_t i = static_cast<std::size_t>(ft - base_);
    const double mu = t - ft;
    const double cm1 = -mu * (mu - 1.0) * (mu - 2.0) / 6.0;
    const double c0 = (mu + 1.0) * (mu - 1.0) * (mu - 2.0) / 2.0;
    const double c1 = -(mu + 1.0) * mu * (mu - 2.0) / 2.0;
    const double c2 = (mu + 1.0) * mu * (mu - 1.0) / 6.0;
    return buf_[i - 1] * static_cast<float>(cm1) + buf_[i] * static_cast<float>(c0) +
           buf_[i + 1] * static_cast<float>(c1) + buf_[i + 2] * static_cast<float>(c2);
}

DPSKResult PskDemod::process(const std::vector<std::complex<float>>& iq) {
    DPSKResult out;
    if (iq.empty()) return out;
    if (!configured_) configure(inRate_, symbolRate_, rrcAlpha_, loopBw_);
    out.symbols.reserve(static_cast<std::size_t>(static_cast<double>(iq.size()) / (decim_ * sps_)) + 8);

    const std::size_t aaN = aaTaps_.size();
    for (const auto& x : iq) {
        if (up_ > 1) {
            // 0b. polyphase interpolation: up_ output samples for each input sample. The history is
            // kept twice back to back so that the newest L samples are always contiguous, and the
            // taps are stored per phase in the same order: a plain dot product, no index wrapping.
            const std::size_t L = upLen_;
            upHist_[upPos_] = x;
            upHist_[upPos_ + L] = x;
            const std::complex<float>* h = upHist_.data() + upPos_ + 1;
            for (int p = 0; p < up_; ++p) {
#if defined(__SSE2__)
                // taps are stored doubled (t,t,t,t...) and padded with zeros to a multiple of four floats
                const float* tp = upPolyDup_.data() + static_cast<std::size_t>(p) * upStride_;
                const float* hf = reinterpret_cast<const float*>(h);
                __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
                std::size_t k = 0;
                for (; k + 8 <= upStride_; k += 8) {
                    a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(hf + k), _mm_loadu_ps(tp + k)));
                    a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_loadu_ps(hf + k + 4), _mm_loadu_ps(tp + k + 4)));
                }
                for (; k < upStride_; k += 4) a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(hf + k), _mm_loadu_ps(tp + k)));
                alignas(16) float lane[4];
                _mm_store_ps(lane, _mm_add_ps(a0, a1));
                consume(std::complex<float>(lane[0] + lane[2], lane[1] + lane[3]), out);
#else
                const float* tp = upPoly_.data() + static_cast<std::size_t>(p) * L;
                float r0 = 0, i0 = 0;
                for (std::size_t k = 0; k < L; ++k) { r0 += h[k].real() * tp[k]; i0 += h[k].imag() * tp[k]; }
                consume(std::complex<float>(r0, i0), out);
#endif
            }
            if (++upPos_ == L) upPos_ = 0;
            continue;
        }
        // 1. decimation
        std::complex<float> z;
        if (decim_ > 1) {
            aaHist_[aaPos_] = x;
            aaPos_ = (aaPos_ + 1) % aaN;
            if (++aaPhase_ < decim_) continue;
            aaPhase_ = 0;
            std::complex<float> acc{0, 0};
            std::size_t idx = aaPos_;
            for (std::size_t k = 0; k < aaN; ++k) {
                acc += aaHist_[idx] * aaTaps_[k];
                if (++idx == aaN) idx = 0;
            }
            z = acc;
        } else {
            z = x;
        }
        consume(z, out);
    }

    // HRIT consumes the full complex soft symbols. The BPSK-only soft array
    // would otherwise allocate and scan three million unused values per second.
    if (!qpsk_) {
        out.soft.reserve(out.symbols.size());
        for (const auto& s : out.symbols) out.soft.push_back(std::clamp(std::fabs(s.real()) / 2.54f, 0.0f, 1.0f));
    }
    out.carrierOffsetHz = static_cast<float>(freq_ * rate_ / (2.0 * kPi));
    out.timingQuality = static_cast<float>(std::clamp(1.0 - std::fabs(period_ - sps_) / sps_ * 20.0, 0.0, 1.0));
    return out;
}

// ---- two-stage version of process() (processSplit) -------------------------------------------------------------------------------
// front(): everything in process() up to and including the AGC; it touches only the filter histories and power_.
std::vector<std::complex<float>> PskDemod::front(const std::vector<std::complex<float>>& iq) {
    std::vector<std::complex<float>> out;
    out.reserve(iq.size() * static_cast<std::size_t>(std::max(up_, 1)) / static_cast<std::size_t>(std::max(decim_, 1)) + 8);
    const auto agc = [&](std::complex<float> z) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) z = {0.0f, 0.0f};
        power_ += 0.001f * (std::norm(z) - power_);
        z *= 1.0f / std::sqrt(std::max(power_, 1e-20f));
        out.push_back(z);
    };
    const std::size_t aaN = aaTaps_.size();
    for (const auto& x : iq) {
        if (up_ > 1) {
            const std::size_t L = upLen_;
            upHist_[upPos_] = x;
            upHist_[upPos_ + L] = x;
            const std::complex<float>* h = upHist_.data() + upPos_ + 1;
            for (int p = 0; p < up_; ++p) {
#if defined(__SSE2__)
                const float* tp = upPolyDup_.data() + static_cast<std::size_t>(p) * upStride_;
                const float* hf = reinterpret_cast<const float*>(h);
                __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
                std::size_t k = 0;
                for (; k + 8 <= upStride_; k += 8) {
                    a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(hf + k), _mm_loadu_ps(tp + k)));
                    a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_loadu_ps(hf + k + 4), _mm_loadu_ps(tp + k + 4)));
                }
                for (; k < upStride_; k += 4) a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(hf + k), _mm_loadu_ps(tp + k)));
                alignas(16) float lane[4];
                _mm_store_ps(lane, _mm_add_ps(a0, a1));
                agc(std::complex<float>(lane[0] + lane[2], lane[1] + lane[3]));
#else
                const float* tp = upPoly_.data() + static_cast<std::size_t>(p) * L;
                float r0 = 0, i0 = 0;
                for (std::size_t k = 0; k < L; ++k) { r0 += h[k].real() * tp[k]; i0 += h[k].imag() * tp[k]; }
                agc(std::complex<float>(r0, i0));
#endif
            }
            if (++upPos_ == L) upPos_ = 0;
            continue;
        }
        std::complex<float> z;
        if (decim_ > 1) {
            aaHist_[aaPos_] = x;
            aaPos_ = (aaPos_ + 1) % aaN;
            if (++aaPhase_ < decim_) continue;
            aaPhase_ = 0;
            std::complex<float> acc{0, 0};
            std::size_t idx = aaPos_;
            for (std::size_t k = 0; k < aaN; ++k) {
                acc += aaHist_[idx] * aaTaps_[k];
                if (++idx == aaN) idx = 0;
            }
            z = acc;
        } else {
            z = x;
        }
        agc(z);
    }
    return out;
}

// back(): acquisition and the tracking chain, as consume() does after the AGC.
DPSKResult PskDemod::back(const std::vector<std::complex<float>>& agc) {
    DPSKResult out;
    out.symbols.reserve(static_cast<std::size_t>(static_cast<double>(agc.size()) / sps_) + 8);
    for (const auto& z : agc) {
        if (acquiring_) {
            acqBuf_.push_back(z);
            if (acqBuf_.size() < acqLength_) continue;
            acquire();
            acquiring_ = false;
            std::vector<std::complex<float>> held;
            held.swap(acqBuf_);
            for (const auto& hz : held) {
                track(hz, out);
                if (acquiring_) break;
            }
            continue;
        }
        track(z, out);
    }
    if (!qpsk_) {
        out.soft.reserve(out.symbols.size());
        for (const auto& s : out.symbols) out.soft.push_back(std::clamp(std::fabs(s.real()) / 2.54f, 0.0f, 1.0f));
    }
    out.carrierOffsetHz = static_cast<float>(freq_ * rate_ / (2.0 * kPi));
    out.timingQuality = static_cast<float>(std::clamp(1.0 - std::fabs(period_ - sps_) / sps_ * 20.0, 0.0, 1.0));
    return out;
}

DPSKResult PskDemod::processSplit(const std::vector<std::complex<float>>& iq) {
    if (iq.empty()) return {};
    if (!configured_) configure(inRate_, symbolRate_, rrcAlpha_, loopBw_);
    return back(front(iq));
}

void PskDemod::consume(std::complex<float> z, DPSKResult& out) {
        // A non-finite sample (a driver hiccup, a corrupt file) would poison the AGC and every
        // loop after it for good; treat it as silence.
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) z = {0.0f, 0.0f};
        // 2. AGC
        power_ += 0.001f * (std::norm(z) - power_);
        z *= 1.0f / std::sqrt(std::max(power_, 1e-20f));
        // 0. acquisition: hold the samples until the carrier has been measured, then run them
        // through the tracking chain so nothing is lost.
        if (acquiring_) {
            acqBuf_.push_back(z);
            if (acqBuf_.size() < acqLength_) return;
            acquire();
            acquiring_ = false;
            std::vector<std::complex<float>> held;
            held.swap(acqBuf_);
            for (const auto& h : held) {
                track(h, out);
                if (acquiring_) break;   // cannot happen this soon, but keeps the state honest
            }
            return;
        }
        track(z, out);
}

void PskDemod::track(std::complex<float> z, DPSKResult& out) {
    // 3. carrier NCO: interpolated cosine table (error about 3e-7), two libm calls a sample otherwise
    {
        constexpr int count=4096;
        static const auto cosine=[] {
            std::array<float,count+1> table{};
            for(int i=0;i<=count;++i)table[i]=float(std::cos(2.0*kPi*i/count));
            return table;
        }();
        const double angle=phase_<0.0?phase_+2.0*kPi:phase_;
        const double index=angle*(count/(2.0*kPi));
        const int ii=int(index);const float f=float(index-ii);   // index >= 0, so truncation is the floor
        const int i=ii&(count-1);
        const int j=(i+count-count/4)&(count-1);
        const float c=cosine[i]+f*(cosine[i+1]-cosine[i]);
        const float s=cosine[j]+f*(cosine[j+1]-cosine[j]);
        z*=std::complex<float>(c,-s);
    }
    phase_ += freq_;
    if (phase_ > kPi) phase_ -= 2.0 * kPi;
    else if (phase_ < -kPi) phase_ += 2.0 * kPi;
    // 4. matched filter
    const std::size_t mfN = rrc_.size();
    mfHist_[mfPos_] = z;
    mfHist_[mfPos_ + mfN] = z;
    if (++mfPos_ == mfN) mfPos_ = 0;
    // The history is stored twice, so the last mfN samples (oldest first) are w[0..mfN-1]. The RRC is
    // symmetric: pair the samples around the centre tap.
    const std::complex<float>* w = mfHist_.data() + mfPos_;
#if defined(__SSE2__)
    std::complex<float> y;
    {
        const float* wf = reinterpret_cast<const float*>(w);
        const float* cf = mfDup_.data();
        __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
        const std::size_t n = mfDup_.size();
        std::size_t k = 0;
        for (; k + 8 <= n; k += 8) {
            a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(wf + k), _mm_loadu_ps(cf + k)));
            a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_loadu_ps(wf + k + 4), _mm_loadu_ps(cf + k + 4)));
        }
        for (; k < n; k += 4) a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_loadu_ps(wf + k), _mm_loadu_ps(cf + k)));
        alignas(16) float lane[4];
        _mm_store_ps(lane, _mm_add_ps(a0, a1));
        y = std::complex<float>(lane[0] + lane[2], lane[1] + lane[3]);
    }
#else
    const float* c = rrc_.data();
    float yr0 = 0, yi0 = 0, yr1 = 0, yi1 = 0;
    const std::size_t half = mfN / 2;
    std::size_t k = 0;
    for (; k + 2 <= half; k += 2) {
        yr0 += (w[k].real() + w[mfN - 1 - k].real()) * c[k];
        yi0 += (w[k].imag() + w[mfN - 1 - k].imag()) * c[k];
        yr1 += (w[k + 1].real() + w[mfN - 2 - k].real()) * c[k + 1];
        yi1 += (w[k + 1].imag() + w[mfN - 2 - k].imag()) * c[k + 1];
    }
    for (; k < half; ++k) {
        yr0 += (w[k].real() + w[mfN - 1 - k].real()) * c[k];
        yi0 += (w[k].imag() + w[mfN - 1 - k].imag()) * c[k];
    }
    const std::complex<float> y(yr0 + yr1 + w[half].real() * c[half], yi0 + yi1 + w[half].imag() * c[half]);
#endif
    buf_.push_back(y);

    // 5./6. interpolants while the cubic has its four samples
    const double periodMin = sps_ * 0.995, periodMax = sps_ * 1.005;
    while (next_ + 2.0 < base_ + static_cast<double>(buf_.size())) {
        const std::complex<float> s = interpolate(next_);
        if (midNext_) {
            mid_ = s;
            midNext_ = false;
            next_ += 0.5 * period_;
            continue;
        }
        const double amp = qpsk_ ? (std::fabs(s.real()) + std::fabs(s.imag())) * 0.5 : std::fabs(s.real());
        // Floored: on dead input (all zeros) the average decays towards zero, and dividing by it
        // turned the symbols and both loops into NaN, which never recovered when signal returned.
        ampAvg_ = std::max(ampAvg_ <= 0.0 ? amp : ampAvg_ + 0.002 * (amp - ampAvg_), 1e-9);
        const double a2 = std::max(ampAvg_ * ampAvg_, 1e-12);
        // Gardner, complex form: carrier-phase independent.
        const double et = PSK_TIMING_SIGN *
            std::clamp(static_cast<double>(std::real(std::conj(mid_) * (prevSym_ - s))) / a2, -1.0, 1.0);
        // Decision-directed BPSK phase error.
        const double phaseError = static_cast<double>(s.imag()) * (s.real() >= 0.f ? 1. : -1.)
            - (qpsk_ ? static_cast<double>(s.real()) * (s.imag() >= 0.f ? 1. : -1.) : 0.);
        const double ec = std::clamp(phaseError / ampAvg_, -1., 1.);
        freq_ += cKi_ * ec / period_;
        phase_ += cKp_ * ec;
        period_ = std::clamp(period_ + tKi_ * et, periodMin, periodMax);
        next_ += 0.5 * period_ + tKp_ * et * period_ * 0.5;
        midNext_ = true;
        prevSym_ = s;
        out.symbols.push_back(s * static_cast<float>(kOutputScale / ampAvg_));
        if (!std::isfinite(freq_) || !std::isfinite(phase_) || !std::isfinite(period_)) {
            // Belt and braces: never carry a broken loop state into the next sample.
            freq_ = 0.0; phase_ = 0.0; period_ = sps_; ampAvg_ = 0.0; lock_ = 0.0;
            acquiring_ = true; acqBuf_.clear();
        }

        // Lock measure: cos(2*phase error), which is 1 on the axis and averages 0 when spinning.
        const double e2 = static_cast<double>(std::norm(s));
        if (e2 > 0.0) {
            const double c2 = (static_cast<double>(s.real()) * s.real() - static_cast<double>(s.imag()) * s.imag()) / e2;
            const double measure = qpsk_ ? 1.0 - 2.0 * c2 * c2 : c2;
            lock_ += kLockAlpha * (measure - lock_);
        }
        // At 3 Mbaud the legacy 50,000-symbol timeout is only 17 ms. A short fade
        // then reacquired the carrier even while valid HRIT frames were still arriving.
        const double unlockLevel=qpsk_?0.015:kUnlockedLevel;
        const long long reacquireAfter=qpsk_?1500000:kUnlockedSymbolsBeforeReacquire;
        unlockedSymbols_ = lock_ < unlockLevel ? unlockedSymbols_ + 1 : 0;
        if (unlockedSymbols_ > reacquireAfter) {
            acquiring_ = true;
            acqBuf_.clear();
            unlockedSymbols_ = 0;
        }
    }
    // Keep only the samples the interpolator can still reach.
    // (next_ is positive, so truncation is the floor; the check is only worth making once the buffer is long)
    const double keepFrom = buf_.size() > 4096 ? static_cast<double>(static_cast<long long>(next_)) - 2.0 : base_;
    if (keepFrom - base_ > 8192.0) {
        const std::size_t drop = static_cast<std::size_t>(keepFrom - base_);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += static_cast<double>(drop);
    }
}


// ---- QpskDemod ---------------------------------------------------------------------------

void QpskDemod::configure(float sampleRate, float symbolRate, float rrcAlpha, float carrierLoopBw,
                          float timingLoopBw) {
    if (sampleRate <= 0.0f || symbolRate <= 0.0f) return;
    inRate_ = sampleRate;
    symbolRate_ = symbolRate;
    alpha_ = std::clamp(rrcAlpha, 0.05f, 1.0f);
    cBw_ = std::clamp(carrierLoopBw, 0.0002f, 0.05f);
    tBw_ = std::clamp(timingLoopBw, 0.00005f, 0.02f);
    decim_ = std::max(1, static_cast<int>(std::floor(sampleRate / (symbolRate * 2.5f))));
    rate_ = inRate_ / static_cast<float>(decim_);
    sps_ = static_cast<double>(rate_) / symbolRate_;
    if (decim_ > 1) {
        const double edge = 0.5 * (1.0 + alpha_) * symbolRate_;
        const double transition = std::max(1e-3, (static_cast<double>(rate_) - 2.0 * edge) / inRate_);
        const int taps = std::clamp(static_cast<int>(std::ceil(6.0 / transition)), 4 * decim_ + 1, 255) | 1;
        aaTaps_ = windowedSinc(taps, 0.5 / decim_);
    } else {
        aaTaps_.assign(1, 1.0f);
    }
    lowRate_ = sps_ < 1.5;
    clampFrac_ = lowRate_ ? 0.001 : 0.005;
    half_ = static_cast<int>(std::ceil((lowRate_ ? 7.0 : 4.0) * sps_));
    const int span = half_ + 1;
    table_.assign(static_cast<std::size_t>(2 * span * kPhases + 1), 0.0f);
    double sum = 0.0;
    for (std::size_t i = 0; i < table_.size(); ++i) {
        const double tau = static_cast<double>(i) / kPhases - span;   // samples
        const double t = tau / sps_;                                  // symbols
        double v;
        const double a = alpha_;
        if (std::fabs(t) > 4.0) v = 0.0;
        else if (std::fabs(t) < 1e-9) v = 1.0 - a + 4.0 * a / kPi;
        else if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-9)
            v = a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
        else
            v = (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
        // Hann taper over the 4-symbol half span keeps the truncated filter's sidelobes down.
        v *= 0.5 + 0.5 * std::cos(kPi * std::clamp(t / 4.0, -1.0, 1.0));
        table_[i] = static_cast<float>(v);
        if (i % kPhases == 0) sum += v;
    }
    if (lowRate_) {
        // Below 1.5 samples a symbol the receive filter is designed rather than taken from the
        // transmit pulse. The SDR's band limit (half the sample rate, 1.6 MHz for 3.2 MS/s against a
        // 3 Msym/s signal whose roll-off reaches 2.25 MHz) cuts the tails the raised-cosine pulse
        // needs, so a plain RRC matched filter leaves inter-symbol interference of its own and, on
        // top of that, matches a pulse that is no longer there (about a decibel at the weakest
        // signals in simulation with a band-limited front end). The received pulse is P(f) = RRC(f)
        // inside +-fs/2 and zero outside; the minimum mean-square-error symbol-spaced receive
        // filter for white noise is R(f) = P*(f) / (sum_k |P(f - k Rs)|^2 + 1/SNR). Its impulse
        // response is evaluated on the same fine time grid the polyphase matched filter already
        // reads, so nothing downstream changes.
        const double fs = static_cast<double>(rate_), rs = static_cast<double>(symbolRate_);
        const double a = alpha_;
        auto rcMag = [&](double f) {   // |RRC(f)|, unity in the pass band
            const double af = std::fabs(f), f1 = 0.5 * (1.0 - a) * rs, f2 = 0.5 * (1.0 + a) * rs;
            if (af <= f1) return 1.0;
            if (af >= f2) return 0.0;
            return std::sqrt(0.5 * (1.0 + std::cos(kPi * (af - f1) / (a * rs))));
        };
        auto P = [&](double f) { return std::fabs(f) < 0.5 * fs ? rcMag(f) : 0.0; };
        const double snr = std::pow(10.0, mmseDesignSnrDb_ / 10.0);
        constexpr int kNf = 1024;
        std::vector<double> rf(kNf), fgrid(kNf);
        for (int j = 0; j < kNf; ++j) {
            const double f = (-0.5 + (j + 0.5) / kNf) * fs;
            double fold = 0.0;
            for (int k = -2; k <= 2; ++k) { const double p = P(f - k * rs); fold += p * p; }
            fgrid[static_cast<std::size_t>(j)] = f;
            rf[static_cast<std::size_t>(j)] = P(f) / (fold + 1.0 / snr);
        }
        sum = 0.0;
        for (std::size_t i = 0; i < table_.size(); ++i) {
            const double tau = static_cast<double>(i) / kPhases - span;   // samples
            const double t = tau / fs;                                     // seconds
            double v = 0.0;
            for (int j = 0; j < kNf; ++j) v += rf[static_cast<std::size_t>(j)] * std::cos(2.0 * kPi * fgrid[static_cast<std::size_t>(j)] * t);
            v /= kNf;
            v *= 0.5 + 0.5 * std::cos(kPi * std::clamp(tau / span, -1.0, 1.0));
            table_[i] = static_cast<float>(v);
            if (i % kPhases == 0) sum += v;
        }
    }
    for (auto& v : table_) v = static_cast<float>(v / sum);
    {
        const int taps = 2 * half_ + 2;
        poly_.assign(static_cast<std::size_t>((kPhases + 1) * taps * 2), 0.0f);
        for (int r = 0; r <= kPhases; ++r)
            for (int j = 0; j < taps; ++j) {
                const float v = table_[static_cast<std::size_t>(r + (2 * half_ + 1 - j) * kPhases)];
                poly_[static_cast<std::size_t>((r * taps + j) * 2)] = v;
                poly_[static_cast<std::size_t>((r * taps + j) * 2 + 1)] = v;
            }
    }
    loopGains(cBw_, cKp_, cKi_);
    loopGains(tBw_, tKp_, tKi_);
    reset();
    configured_ = true;
}

void QpskDemod::reset() {
    aaHist_.assign(aaTaps_.size(), {0, 0});
    aaPos_ = 0;
    aaPhase_ = 0;
    power_ = 1.0f;
    phase_ = 0.0;
    freq_ = initialCarrierOffset_;
    ncoCount_ = 0;
    buf_.clear();
    base_ = 0.0;
    next_ = static_cast<double>(half_ + 2);
    period_ = sps_;
    prevSym_ = {0, 0};
    ampAvg_ = 0.0;
    acqBuf_.clear();
    dc_ = {0.0f, 0.0f};
    acqLength_ = rate_ > 1.5e6f ? 65536 : 16384;
    acquiring_ = true;
    lock_ = 0.0;
    unlockedSymbols_ = 0;
}

void QpskDemod::acquire() {
    const std::size_t n = acqBuf_.size();
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const std::complex<double> z(acqBuf_[i]);
        const std::complex<double> z2 = z * z;
        a[i] = z2 * z2 * w;
    }
    // The 4th power of a DC spur is a line at 0; ignore the bins next to DC unless nothing else.
    fftInPlace(a);
    std::size_t best = 0;
    double bestPower = -1.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double p = std::norm(a[k]);
        if (p > bestPower) { bestPower = p; best = k; }
    }
    auto logPower = [&](std::size_t k) { return std::log(std::max(std::norm(a[k % n]), 1e-300)); };
    const double l = logPower(best + n - 1), c = logPower(best), r = logPower(best + 1);
    const double den = l - 2.0 * c + r;
    const double delta = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    double bin = static_cast<double>(best) + delta;
    if (bin > static_cast<double>(n) / 2.0) bin -= static_cast<double>(n);
    freq_ = 2.0 * kPi * (bin / static_cast<double>(n)) / 4.0;
    phase_ = 0.0;
    buf_.clear();
    base_ = 0.0;
    next_ = static_cast<double>(half_ + 2);
    period_ = sps_;
    prevSym_ = {0, 0};
    ampAvg_ = 0.0;
    lock_ = 0.0;
    unlockedSymbols_ = 0;
    ncoCount_ = 0;
    ++acquisitions_;
}

std::complex<float> QpskDemod::mf(double t) const {
    // y(t) = sum_k x[k] h(t - k), k from floor(t)-half .. floor(t)+half+1, evaluated from the
    // contiguous polyphase rows built in configure().
    const double ft = static_cast<double>(static_cast<long long>(t));   // t is positive
    const double mu = t - ft;
    const long first = static_cast<long>(ft - base_) - half_;
    const int taps = 2 * half_ + 2;
    const int r = static_cast<int>(mu * kPhases + 0.5);
    const float* h = poly_.data() + static_cast<std::size_t>(r) * taps * 2;
    const float* x = reinterpret_cast<const float*>(buf_.data() + first);
#if defined(__SSE2__)
    __m128 acc = _mm_setzero_ps();
    for (int j = 0; j < taps; j += 2)
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(x + 2 * j), _mm_loadu_ps(h + 2 * j)));
    alignas(16) float l[4];
    _mm_store_ps(l, acc);
    return {l[0] + l[2], l[1] + l[3]};
#else
    float re = 0.0f, im = 0.0f;
    for (int j = 0; j < taps; ++j) { re += x[2 * j] * h[2 * j]; im += x[2 * j + 1] * h[2 * j + 1]; }
    return {re, im};
#endif
}

DPSKResult QpskDemod::process(const std::vector<std::complex<float>>& iq) {
    DPSKResult out;
    if (iq.empty()) return out;
    if (!configured_) configure(inRate_, symbolRate_, alpha_, cBw_, tBw_);
    out.symbols.reserve(static_cast<std::size_t>(static_cast<double>(iq.size()) / (decim_ * sps_)) + 8);
    const std::size_t aaN = aaTaps_.size();
    for (const auto& x : iq) {
        std::complex<float> z = x;
        if (decim_ > 1) {
            aaHist_[aaPos_] = x;
            aaPos_ = (aaPos_ + 1) % aaN;
            if (++aaPhase_ < decim_) continue;
            aaPhase_ = 0;
            std::complex<float> acc{0, 0};
            std::size_t idx = aaPos_;
            for (std::size_t k = 0; k < aaN; ++k) {
                acc += aaHist_[idx] * aaTaps_[k];
                if (++idx == aaN) idx = 0;
            }
            z = acc;
        }
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) z = {0.0f, 0.0f};
        if (lowRate_) { dc_ += (z - dc_) * (1.0f / 4096.0f); z -= dc_; }
        power_ += 0.0005f * (std::norm(z) - power_);
        if (--agcCount_ <= 0) { agcScale_ = 1.0f / std::sqrt(std::max(power_, 1e-20f)); agcCount_ = 8; }
        z *= agcScale_;
        if (acquiring_) {
            acqBuf_.push_back(z);
            if (acqBuf_.size() < acqLength_) continue;
            acquire();
            acquiring_ = false;
            std::vector<std::complex<float>> held;
            held.swap(acqBuf_);
            for (const auto& h : held) track(h, out);
            continue;
        }
        track(z, out);
    }
    out.carrierOffsetHz = carrierOffsetHz();
    out.timingQuality = static_cast<float>(std::clamp(1.0 - std::fabs(period_ - sps_) / sps_ * 20.0, 0.0, 1.0));
    return out;
}

void QpskDemod::track(std::complex<float> z, DPSKResult& out) {
    if (ncoCount_ <= 0) {
        rot_ = std::complex<double>(std::cos(phase_), -std::sin(phase_));
        step_ = std::complex<double>(std::cos(freq_), -std::sin(freq_));
        ncoCount_ = kNcoResync;
    }
    --ncoCount_;
    {
        // Written out: std::complex multiplication carries an inf/NaN recovery path that is a
        // function call per sample.
        const float rr = static_cast<float>(rot_.real()), ri = static_cast<float>(rot_.imag());
        z = {z.real() * rr - z.imag() * ri, z.real() * ri + z.imag() * rr};
        rot_ = {rot_.real() * step_.real() - rot_.imag() * step_.imag(),
                rot_.real() * step_.imag() + rot_.imag() * step_.real()};
    }
    phase_ += freq_;
    if (phase_ > kPi) phase_ -= 2.0 * kPi;
    else if (phase_ < -kPi) phase_ += 2.0 * kPi;
    buf_.push_back(z);

    const double periodMin = sps_ * (1.0 - clampFrac_), periodMax = sps_ * (1.0 + clampFrac_);
    // Need samples up to floor(next_) + half_ + 1.
    while (next_ + static_cast<double>(half_) + 2.0 < base_ + static_cast<double>(buf_.size())) {
        const std::complex<float> s = mf(next_);
        const std::complex<float> mid = lowRate_ ? std::complex<float>(0.0f, 0.0f) : mf(next_ - 0.5 * period_);
        const double sr = s.real(), si = s.imag();
        const double amp = 0.5 * (std::fabs(sr) + std::fabs(si));
        ampAvg_ = std::max(ampAvg_ <= 0.0 ? amp : ampAvg_ + 0.001 * (amp - ampAvg_), 1e-9);
        const double invAmp = 1.0 / ampAvg_;
        const double a2inv = 1.0 / std::max(ampAvg_ * ampAvg_, 1e-12);
        double et;
        if (lowRate_) {
            // Mueller & Muller on the decisions: (previous decision x this sample - this decision x previous sample).
            const double dI = sr >= 0.0 ? 1.0 : -1.0, dQ = si >= 0.0 ? 1.0 : -1.0;
            const double pI = prevSym_.real() >= 0.0f ? 1.0 : -1.0, pQ = prevSym_.imag() >= 0.0f ? 1.0 : -1.0;
            et = std::clamp(((pI * sr - dI * static_cast<double>(prevSym_.real())) +
                             (pQ * si - dQ * static_cast<double>(prevSym_.imag()))) * invAmp * 0.5, -1.0, 1.0);
        } else {
            et = std::clamp(static_cast<double>(mid.real() * (prevSym_.real() - s.real())
                                                + mid.imag() * (prevSym_.imag() - s.imag())) * a2inv, -1.0, 1.0);
        }
        const double ec = std::clamp((si * (sr >= 0.0 ? 1.0 : -1.0) - sr * (si >= 0.0 ? 1.0 : -1.0)) * invAmp, -1.0, 1.0);
        {
            // Small-angle phasor updates (|x| stays well under 0.1 rad); the exact values come back
            // at the next resync.
            const double df = cKi_ * ec / period_, dp = cKp_ * ec;
            freq_ += df;
            phase_ += dp;
            {
                const double cs = 1.0 - 0.5 * df * df, sn = -df;
                step_ = {step_.real() * cs - step_.imag() * sn, step_.real() * sn + step_.imag() * cs};
            }
            {
                const double cs = 1.0 - 0.5 * dp * dp, sn = -dp;
                rot_ = {rot_.real() * cs - rot_.imag() * sn, rot_.real() * sn + rot_.imag() * cs};
            }
        }
        period_ = std::clamp(period_ + tKi_ * et, periodMin, periodMax);
        next_ += period_ + tKp_ * et * period_;
        prevSym_ = s;
        out.symbols.push_back(s * static_cast<float>(invAmp));
        if (!std::isfinite(freq_ + phase_ + period_)) {
            ncoCount_ = 0;
            freq_ = 0.0; phase_ = 0.0; period_ = sps_; ampAvg_ = 0.0; lock_ = 0.0;
            acquiring_ = true; acqBuf_.clear();
            return;
        }
        const double aa = sr * sr, bb = si * si, e2 = aa + bb;
        if (e2 > 0.0) {
            // cos(4 theta) = Re(z^4) / |z|^4, written out: Re(z^4) = (a^2 - b^2)^2 - (2ab)^2
            const double d = aa - bb, x2 = 2.0 * sr * si;
            lock_ += (1.0 / 4000.0) * (-(d * d - x2 * x2) / (e2 * e2) - lock_);
        }
        unlockedSymbols_ = lock_ < 0.03 ? unlockedSymbols_ + 1 : 0;
        if (unlockedSymbols_ > 3000000) {   // one second of symbols without lock
            acquiring_ = true;
            acqBuf_.clear();
            unlockedSymbols_ = 0;
            return;
        }
    }
    // (buf_ can only hold more than 16384 samples past base_ after enough input has arrived, so the
    // floor() below is skipped for most samples.)
    const double keepFrom = buf_.size() > 16384 ? std::floor(next_) - static_cast<double>(half_) - 4.0 : base_;
    if (keepFrom - base_ > 16384.0) {
        const std::size_t drop = static_cast<std::size_t>(keepFrom - base_);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += static_cast<double>(drop);
    }
}

DPSKResult differentialDpsk660k(const std::vector<std::complex<float>>& iq,
                                 float sampleRate, float bitRate, int polarity,
                                 uint8_t* nrzmLastBit) {
    DPSKResult out;
    if (iq.empty()) return out;

    PskDemod demod(sampleRate, bitRate);
    out = demod.process(iq);
    if (out.symbols.empty()) return out;

    const uint8_t prevStart = nrzmLastBit ? *nrzmLastBit : 0u;
    uint8_t prev = prevStart;
    out.bits.reserve(out.symbols.size());
    for (std::size_t i = 0; i < out.symbols.size(); ++i) {
        uint8_t hard = out.symbols[i].real() < 0.0f ? 1u : 0u;
        if (polarity) hard ^= 1u;
        out.bits.push_back(static_cast<uint8_t>(hard ^ prev));
        prev = hard;
    }
    if (nrzmLastBit) *nrzmLastBit = prev;
    return out;
}

}
