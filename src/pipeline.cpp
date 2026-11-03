#include "pipeline.h"

#include <algorithm>
#include <cmath>

namespace hritapp {

namespace {
// Same loop bandwidths NadirStation uses for HRIT (normalised to the symbol rate).
constexpr float kRrcAlpha = 0.5f;
constexpr float kCarrierLoopBw = 0.0002f;
constexpr float kTimingLoopBw = 0.0002f;
}  // namespace

bool HritPipeline::configure(double sampleRate, std::string& error) {
    configured_ = false;
    if (!(sampleRate >= kMinSampleRate)) {
        error = "GK-2A HRIT 的符号率是 3 Msym/s，采样率至少要 3.15 MS/s（推荐 4.5 MS/s 以上，6 MS/s 最佳）。";
        return false;
    }
    sampleRate_ = sampleRate;
    demod_.configure(static_cast<float>(sampleRate), static_cast<float>(kSymbolRate), kRrcAlpha, kCarrierLoopBw,
                     kTimingLoopBw);
    link_.setAsync(true);   // the link decoder gets its own thread, like in the live receiver
    configured_ = true;
    reset();
    return true;
}

void HritPipeline::reset() {
    demod_.reset();
    link_.reset();
    assembler_.reset();
    assembler_.setStrict(true);
    assembler_.setLossTolerant(true);   // HRIT segments are 25+ MB; one lost packet must not discard a segment
    symbols_.clear();
    filesCompleted_ = 0;
    snrY1_ = snrY2_ = 0.0;
    carrierOffsetHz_ = 0.0f;
}

void HritPipeline::handleFrame(nadirstation::gk2a::Cadu&& frame,
                               const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile) {
    if (!frame.rsOk) return;
    for (auto& file : assembler_.feed(frame.bytes)) {
        ++filesCompleted_;
        onFile(std::move(file));
    }
}

const std::vector<std::complex<float>>& HritPipeline::process(
    const std::vector<std::complex<float>>& iq, const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile) {
    if (!configured_) {
        symbols_.clear();
        return symbols_;
    }
    auto result = demod_.process(iq);
    carrierOffsetHz_ = result.carrierOffsetHz;
    symbols_ = std::move(result.symbols);
    link_.feed(symbols_, [this, &onFile](nadirstation::gk2a::Cadu&& frame) { handleFrame(std::move(frame), onFile); });
    // SNR the way SatDump reports it: M2M4 moment estimator over every demodulated symbol, exponential
    // averages with alpha = 0.001 (about 1000 symbols), kept across blocks.
    constexpr double alpha = 0.001, beta = 1.0 - alpha;
    double y1 = snrY1_, y2 = snrY2_;
    for (const auto& z : symbols_) {
        const double m = double(z.real()) * z.real() + double(z.imag()) * z.imag();
        y1 = alpha * m + beta * y1;
        y2 = alpha * m * m + beta * y2;
    }
    snrY1_ = std::isfinite(y1) ? y1 : 0.0;
    snrY2_ = std::isfinite(y2) ? y2 : 0.0;
    return symbols_;
}

void HritPipeline::finish(const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile) {
    if (!configured_) return;
    link_.drain([this, &onFile](nadirstation::gk2a::Cadu&& frame) { handleFrame(std::move(frame), onFile); });
    // Files still open at the end of the recording (closing packet lost, or the pass simply ended) are
    // closed now; the missing tail is zero-filled and the segment is decoded as far as it goes.
    for (auto& file : assembler_.flush()) {
        ++filesCompleted_;
        onFile(std::move(file));
    }
}

PipelineStats HritPipeline::stats() const {
    PipelineStats s;
    const auto& d = link_.stats();
    s.cadusWithSync = d.cadusWithSync;
    s.cadusRsOk = d.cadusWithGoodRs;
    s.rsBytesCorrected = d.rsBytesCorrected;
    s.packets = assembler_.packetsSeen;
    s.filesCompleted = filesCompleted_;
    s.crcFailures = assembler_.crcFailures;
    s.fillerFrames = assembler_.fillerCadus;
    s.frameGaps = assembler_.frameDiscontinuities;
    s.locked = link_.locked();
    s.lockQuality = std::clamp(demod_.lockMeasure(), 0.0f, 1.0f);
    s.carrierOffsetHz = carrierOffsetHz_;
    const double signal = std::sqrt(std::max(0.0, 2.0 * snrY1_ * snrY1_ - snrY2_));
    const double noise = snrY1_ - signal;
    s.snrDb = (signal > 0.0 && noise > 1e-12) ? std::max(0.0, 10.0 * std::log10(signal / noise)) : 0.0;
    s.mapping = link_.mapping();
    return s;
}

}  // namespace hritapp
