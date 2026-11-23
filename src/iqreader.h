// iqreader.h - reads baseband recordings for the GK-2A HRIT decoder.
// No Qt in here, so it can be tested on its own.
//
// Additional recording formats:
//   cs8 / s8    interleaved I,Q, signed 8-bit
//   cs16 / s16  interleaved I,Q, signed 16-bit little-endian
//   cs32 / s32  interleaved I,Q, signed 32-bit little-endian
//   cf32 / f32  interleaved I,Q, 32-bit float little-endian
//   cu8 / u8    interleaved I,Q, unsigned 8-bit (127 = zero)
//   wav         2-channel PCM 8/16/24/32-bit or 32-bit float, RIFF or RF64
//   ziq         ZIQ, optional zstd compression, 8/16-bit integer or 32-bit float samples
//   ziq2        ZIQ2 packet stream, 8/16-bit integer samples
//
// The format is taken from the file header first (ZIQ_, ZIQ2 or RIFF/RF64) and from the extension
// otherwise, so a renamed recording keeps its format. A raw cs*/cu8 file does
// not carry its sample rate. It is taken, in this order, from
//   1. a text file next to it ("<name>.txt" or "<name>.cs16.txt") with a "rate <Hz>" field,
//   2. a number with a unit in the file name ("..._3200000SPS_...", "3.2MSPS", "3200ksps"),
//   3. the value the caller passes in (the box in the window).
// WAV, ZIQ and ZIQ2 carry their sample rate in the header.
#pragma once
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hritapp {

enum class IqFormat { Cs8, Cs16, Cs32, Cf32, Cu8, Wav, Ziq, Ziq2 };

struct IqInfo {
    IqFormat format = IqFormat::Cs16;
    double sampleRate = 0.0;            // Hz
    std::string rateSource;             // "WAV 头", "ZIQ 头", "ZIQ2 头", "同名 .txt", "文件名" or "手动设置"
    std::uint64_t frames = 0;           // complex samples in the file
    // False for compressed ZIQ and for ZIQ2, whose sample count is not known before reading the data.
    // In that case `frames` is 0 and progress comes from IqReader::progress() (file position).
    bool framesExact = true;
    int wavBits = 0;                    // WAV only
    bool wavFloat = false;              // WAV only
    bool ziqCompressed = false;         // ZIQ only
    int ziqBits = 0;                    // ZIQ only: 8, 16 or 32 (32 = complex float)
};

// Format from the file header or, when it is not one of the known headers, from the extension.
bool formatForPath(const std::filesystem::path& path, IqFormat& out);
// Only the extension, without opening the file.
bool formatFromExtension(const std::filesystem::path& path, IqFormat& out);
// Only the header ("ZIQ_", "ZIQ2", "RIFF"/"RF64" + "WAVE").
bool sniffFormat(const std::filesystem::path& path, IqFormat& out);

const char* formatName(IqFormat format);
const char* formatShortName(IqFormat format);
// True for WAV / ZIQ / ZIQ2: the sample rate is stored in the file.
bool formatCarriesRate(IqFormat format);
// Human-readable format plus the details known from the header ("ZIQ（16 位，zstd 压缩）").
std::string describeFormat(const IqInfo& info);
// "cs8、cs16、cs32、cf32、cu8、wav、ziq、ziq2" for error messages.
std::string supportedFormats();

// 0 when the file name / the text file next to it does not say.
double rateFromName(const std::string& fileName);
double rateFromSidecar(const std::filesystem::path& path);

class IqReader {
public:
    IqReader() = default;
    ~IqReader();
    IqReader(const IqReader&) = delete;
    IqReader& operator=(const IqReader&) = delete;

    // preferManual: for raw cs*/cu8 a manual rate beats the text file and the file name (the box in the window).
    bool open(const std::filesystem::path& path, double manualRateHz, std::string& error, bool preferManual = false);
    void close();
    bool isOpen() const { return open_; }
    const IqInfo& info() const { return info_; }
    // Reads up to maxFrames complex samples (scaled to about +-1). Returns how many were read; 0 at the end.
    std::size_t read(std::vector<std::complex<float>>& out, std::size_t maxFrames);
    std::uint64_t position() const { return position_; }
    // Moves to a complex-sample index (clamped to the end). False when no file is open.
    bool seek(std::uint64_t frame);
    // 0..1, from the byte position in the file. Works for formats whose sample count is unknown.
    double progress();

private:
    bool openWav(std::string& error);
    bool openZiq(std::string& error);
    bool openZiq2(std::string& error);
    std::size_t readRaw(std::size_t want, std::vector<std::complex<float>>& out);
    std::size_t readZiqCompressed(std::size_t want, std::vector<std::complex<float>>& out);
    std::size_t readZiq2(std::size_t want, std::vector<std::complex<float>>& out);
    bool readZiq2Packet(std::string* error);
    bool seekByReading(std::uint64_t target);
    void decodeBlock(const unsigned char* p, std::size_t frames, std::vector<std::complex<float>>& out) const;
    void resetZstd();

    std::ifstream file_;
    IqInfo info_;
    bool open_ = false;
    std::uint64_t position_ = 0;
    std::uint64_t dataOffset_ = 0;
    std::uint64_t fileSize_ = 0;
    std::size_t frameBytes_ = 4;
    std::vector<unsigned char> raw_;

    // zstd decompressor for compressed ZIQ; void* keeps zstd.h out of this header.
    void* zstdCtx_ = nullptr;
    std::vector<unsigned char> zstdIn_;
    std::size_t zstdInPos_ = 0;
    std::size_t zstdInSize_ = 0;

    // ZIQ2 packet reader.
    std::vector<std::complex<float>> ziq2Samples_;
    std::size_t ziq2SamplePos_ = 0;
    std::vector<unsigned char> ziq2Packet_;
    int ziq2LastType_ = -1;
};

}  // namespace hritapp
