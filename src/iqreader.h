// iqreader.h - reads baseband recordings for the GK-2A HRIT decoder: .cs8, .cs16 and .wav.
// No Qt in here, so it can be tested on its own.
//
//   .cs8   interleaved I,Q, signed 8-bit
//   .cs16  interleaved I,Q, signed 16-bit little-endian
//   .wav   2-channel (I = left, Q = right) PCM 8/16/24/32-bit or 32-bit float; the sample rate is in the header
//
// A raw .cs8/.cs16 file does not carry its sample rate. It is taken, in this order, from
//   1. a text file next to it ("<name>.txt" or "<name>.cs16.txt") with a "rate <Hz>" field,
//   2. a number with a unit in the file name ("..._3200000SPS_...", "3.2MSPS", "3200ksps"),
//   3. the value the caller passes in (the box in the window).
#pragma once
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hritapp {

enum class IqFormat { Cs8, Cs16, Wav };

struct IqInfo {
    IqFormat format = IqFormat::Cs16;
    double sampleRate = 0.0;            // Hz
    std::string rateSource;             // "WAV header", "sidecar .txt", "file name" or "manual"
    std::uint64_t frames = 0;           // complex samples in the file
    int wavBits = 0;                    // WAV only
    bool wavFloat = false;              // WAV only
};

// Format from the extension; false for anything that is not .cs8, .cs16 or .wav.
bool formatForPath(const std::filesystem::path& path, IqFormat& out);
const char* formatName(IqFormat format);
// 0 when the file name / the text file next to it does not say.
double rateFromName(const std::string& fileName);
double rateFromSidecar(const std::filesystem::path& path);

class IqReader {
public:
    // preferManual: for .cs8/.cs16 a manual rate beats the text file and the file name (the box in the window).
    bool open(const std::filesystem::path& path, double manualRateHz, std::string& error, bool preferManual = false);
    void close();
    bool isOpen() const { return open_; }
    const IqInfo& info() const { return info_; }
    // Reads up to maxFrames complex samples (scaled to about +-1). Returns how many were read; 0 at the end.
    std::size_t read(std::vector<std::complex<float>>& out, std::size_t maxFrames);
    std::uint64_t position() const { return position_; }
    // Moves to a complex-sample index (clamped to the end). False when no file is open.
    bool seek(std::uint64_t frame);

private:
    bool openWav(std::string& error);
    std::ifstream file_;
    IqInfo info_;
    bool open_ = false;
    std::uint64_t position_ = 0;
    std::uint64_t dataOffset_ = 0;
    std::size_t frameBytes_ = 4;
    std::vector<unsigned char> raw_;
};

}  // namespace hritapp
