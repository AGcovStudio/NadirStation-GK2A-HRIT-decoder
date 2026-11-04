// pipeline.h - the GK-2A HRIT receive chain without any Qt:
//   IQ -> QpskDemod (polyphase RRC matched filter, 4th-power carrier search, Gardner / Mueller-Muller timing,
//   decision-directed carrier loop) -> HritDecoder (Viterbi r=1/2 K=7, marker search, Reed-Solomon 255/223 x4)
//   -> LritFileAssembler (VCDU / M_PDU / CCSDS packets -> LRIT files).
// It is the chain NadirStation runs for a live reception; this class only wires it up for a recording.
#pragma once
#include "dsp.h"
#include "gk2adatalink.h"
#include "gk2ahrit.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hritapp {

struct PipelineStats {
    std::size_t cadusWithSync = 0;
    std::size_t cadusRsOk = 0;
    std::size_t rsBytesCorrected = 0;
    std::size_t packets = 0;
    std::size_t filesCompleted = 0;
    std::size_t crcFailures = 0;
    std::size_t fillerFrames = 0;
    std::size_t frameGaps = 0;
    bool locked = false;        // a QPSK mapping validated by Reed-Solomon is held
    float lockQuality = 0.0f;   // 0..1, carrier loop
    float carrierOffsetHz = 0.0f;
    double snrDb = 0.0;         // M2M4 estimate over the demodulated symbols
    int mapping = 0;
};

class HritPipeline {
public:
    static constexpr double kSymbolRate = 3.0e6;
    static constexpr double kMinSampleRate = 1.05 * kSymbolRate;   // below this the signal does not fit
    HritPipeline() = default;

    // false (with a message) when the sample rate cannot carry the signal.
    bool configure(double sampleRate, std::string& error);
    void reset();
    // One block of IQ. Completed LRIT files are handed to onFile. Returns the symbols of this block (for the display).
    const std::vector<std::complex<float>>& process(const std::vector<std::complex<float>>& iq,
                                                    const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile);
    // End of the recording: wait for the link-decoder thread and hand over what is left.
    void finish(const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile);
    PipelineStats stats() const;
    // Keep every Reed-Solomon-checked CADU, in order, for a later repair pass (see repair.h).
    void setCollectFrames(bool on) { collect_ = on; }
    std::vector<std::vector<std::uint8_t>>& collectedFrames() { return collected_; }
    // Assembles LRIT files from a list of CADUs the way the receive chain does (strict, loss-tolerant).
    static void assembleFrames(const std::vector<std::vector<std::uint8_t>>& frames,
                               const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile);
    double sampleRate() const { return sampleRate_; }

private:
    void handleFrame(nadirstation::gk2a::Cadu&& frame, const std::function<void(nadirstation::gk2a::LritFile&&)>& onFile);
    nadirstation::dsp::QpskDemod demod_;
    nadirstation::gk2a::HritDecoder link_;
    nadirstation::gk2a::LritFileAssembler assembler_;
    std::vector<std::complex<float>> symbols_;
    std::size_t filesCompleted_ = 0;
    double snrY1_ = 0.0, snrY2_ = 0.0;
    float carrierOffsetHz_ = 0.0f;
    double sampleRate_ = 0.0;
    bool configured_ = false;
    bool collect_ = false;
    std::vector<std::vector<std::uint8_t>> collected_;
};

}  // namespace hritapp
