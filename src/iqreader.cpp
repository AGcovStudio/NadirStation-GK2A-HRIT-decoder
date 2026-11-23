#include "iqreader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <system_error>

#ifdef HRIT_HAVE_ZSTD
#include <zstd.h>
#endif

namespace fs = std::filesystem;

namespace hritapp {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// u8string() is std::string in C++17 and std::u8string in C++20; this works with both.
std::string utf8FileName(const fs::path& p) {
    const auto u = p.filename().u8string();
    return std::string(u.begin(), u.end());
}

std::string utf8Extension(const fs::path& p) {
    const auto u = p.extension().u8string();
    return lower(std::string(u.begin(), u.end()));
}

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint64_t le64(const unsigned char* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}
float leFloat(const unsigned char* p) {
    const std::uint32_t u = le32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

bool extensionToFormat(const std::string& ext, IqFormat& out) {
    if (ext == ".cs8" || ext == ".s8") { out = IqFormat::Cs8; return true; }
    if (ext == ".cs16" || ext == ".s16") { out = IqFormat::Cs16; return true; }
    if (ext == ".cs32" || ext == ".s32") { out = IqFormat::Cs32; return true; }
    if (ext == ".cf32" || ext == ".f32") { out = IqFormat::Cf32; return true; }
    if (ext == ".cu8" || ext == ".u8") { out = IqFormat::Cu8; return true; }
    if (ext == ".wav" || ext == ".w16") { out = IqFormat::Wav; return true; }
    if (ext == ".ziq") { out = IqFormat::Ziq; return true; }
    if (ext == ".ziq2") { out = IqFormat::Ziq2; return true; }
    return false;
}

constexpr unsigned char kZiq2Sync[4] = {0x1a, 0xcf, 0xfc, 0x1d};

}  // namespace

bool formatFromExtension(const fs::path& path, IqFormat& out) {
    return extensionToFormat(utf8Extension(path), out);
}

bool sniffFormat(const fs::path& path, IqFormat& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    unsigned char head[12] = {};
    f.read(reinterpret_cast<char*>(head), 12);
    const std::streamsize got = f.gcount();
    if (got >= 4 && std::memcmp(head, "ZIQ_", 4) == 0) { out = IqFormat::Ziq; return true; }
    if (got >= 4 && std::memcmp(head, "ZIQ2", 4) == 0) { out = IqFormat::Ziq2; return true; }
    if (got >= 12 && (std::memcmp(head, "RIFF", 4) == 0 || std::memcmp(head, "RF64", 4) == 0) &&
        std::memcmp(head + 8, "WAVE", 4) == 0) {
        out = IqFormat::Wav;
        return true;
    }
    return false;
}

bool formatForPath(const fs::path& path, IqFormat& out) {
    if (sniffFormat(path, out)) return true;
    return formatFromExtension(path, out);
}

const char* formatName(IqFormat format) {
    switch (format) {
    case IqFormat::Cs8: return "cs8（I/Q int8）";
    case IqFormat::Cs16: return "cs16（I/Q int16）";
    case IqFormat::Cs32: return "cs32（I/Q int32）";
    case IqFormat::Cf32: return "cf32（I/Q float32）";
    case IqFormat::Cu8: return "cu8（I/Q uint8）";
    case IqFormat::Wav: return "WAV（I/Q 双声道）";
    case IqFormat::Ziq: return "ZIQ（压缩 I/Q 基带）";
    case IqFormat::Ziq2: return "ZIQ2（包流 I/Q 基带）";
    }
    return "未知";
}

const char* formatShortName(IqFormat format) {
    switch (format) {
    case IqFormat::Cs8: return "cs8";
    case IqFormat::Cs16: return "cs16";
    case IqFormat::Cs32: return "cs32";
    case IqFormat::Cf32: return "cf32";
    case IqFormat::Cu8: return "cu8";
    case IqFormat::Wav: return "wav";
    case IqFormat::Ziq: return "ziq";
    case IqFormat::Ziq2: return "ziq2";
    }
    return "?";
}

bool formatCarriesRate(IqFormat format) {
    return format == IqFormat::Wav || format == IqFormat::Ziq || format == IqFormat::Ziq2;
}

std::string supportedFormats() { return "cs8、cs16、cs32、cf32、cu8、wav、ziq、ziq2"; }

std::string describeFormat(const IqInfo& info) {
    switch (info.format) {
    case IqFormat::Wav: {
        std::string s = "WAV（";
        s += std::to_string(info.wavBits);
        s += info.wavFloat ? " 位浮点" : " 位整数";
        s += "，双声道）";
        return s;
    }
    case IqFormat::Ziq: {
        std::string s = "ZIQ（";
        s += std::to_string(info.ziqBits);
        s += " 位";
        if (info.ziqCompressed) s += "，zstd 压缩";
        s += "）";
        return s;
    }
    case IqFormat::Ziq2: return "ZIQ2（数据包流）";
    default: return formatName(info.format);
    }
}

double rateFromName(const std::string& fileName) {
    // Only a number directly followed by a rate unit counts; a bare number in a name may as well be a
    // frequency or a date.
    static const std::regex rx(R"(([0-9]+(?:\.[0-9]+)?)\s*([km]?)(?:sps|s/s|sa/s))", std::regex::icase);
    for (auto it = std::sregex_iterator(fileName.begin(), fileName.end(), rx); it != std::sregex_iterator(); ++it) {
        double hz = std::atof((*it)[1].str().c_str());
        const std::string unit = lower((*it)[2].str());
        if (unit == "k") hz *= 1e3;
        else if (unit == "m") hz *= 1e6;
        if (hz >= 1000.0 && hz <= 100e6) return hz;
    }
    return 0.0;
}

double rateFromSidecar(const fs::path& path) {
    fs::path withTxt = path;
    withTxt.replace_extension(".txt");
    fs::path appended = path;
    appended += ".txt";
    static const std::regex rx(R"(\brate\s*[=:]?\s*([0-9]+(?:\.[0-9]+)?))", std::regex::icase);
    for (const fs::path& candidate : {withTxt, appended}) {
        std::ifstream f(candidate, std::ios::binary);
        if (!f) continue;
        std::string head(4096, '\0');
        f.read(head.data(), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(f.gcount()));
        std::smatch m;
        if (std::regex_search(head, m, rx)) {
            const double hz = std::atof(m[1].str().c_str());
            if (hz >= 1000.0 && hz <= 100e6) return hz;
        }
    }
    return 0.0;
}

IqReader::~IqReader() { close(); }

bool IqReader::open(const fs::path& path, double manualRateHz, std::string& error, bool preferManual) {
    close();
    IqFormat format;
    if (!formatForPath(path, format)) {
        error = "不支持的文件格式。支持 " + supportedFormats() + "。";
        return false;
    }
    std::error_code ec;
    fileSize_ = static_cast<std::uint64_t>(fs::file_size(path, ec));
    if (ec) {
        error = "无法读取文件：" + ec.message();
        return false;
    }
    if (fileSize_ == 0) {
        error = "文件是空的。";
        return false;
    }
    file_.open(path, std::ios::binary);
    if (!file_) {
        error = "无法打开文件（被其他程序占用，或没有读取权限）。";
        return false;
    }
    info_ = IqInfo{};
    info_.format = format;
    position_ = 0;
    dataOffset_ = 0;
    frameBytes_ = 4;
    raw_.clear();

    bool ok = false;
    switch (format) {
    case IqFormat::Wav:
        ok = openWav(error);
        break;
    case IqFormat::Ziq:
        ok = openZiq(error);
        break;
    case IqFormat::Ziq2:
        ok = openZiq2(error);
        break;
    default: {
        switch (format) {
        case IqFormat::Cs8: frameBytes_ = 2; break;
        case IqFormat::Cs16: frameBytes_ = 4; break;
        case IqFormat::Cs32: frameBytes_ = 8; break;
        case IqFormat::Cf32: frameBytes_ = 8; break;
        case IqFormat::Cu8: frameBytes_ = 2; break;
        default: break;
        }
        info_.frames = fileSize_ / frameBytes_;
        if (info_.frames == 0) {
            error = "文件里没有完整的采样。";
            break;
        }
        double hz = 0.0;
        if (preferManual && manualRateHz > 0.0) {
            hz = manualRateHz;
            info_.rateSource = "手动设置";
        }
        if (hz <= 0.0) {
            hz = rateFromSidecar(path);
            if (hz > 0.0) info_.rateSource = "同名 .txt";
        }
        if (hz <= 0.0) {
            hz = rateFromName(utf8FileName(path));
            if (hz > 0.0) info_.rateSource = "文件名";
        }
        if (hz <= 0.0) {
            hz = manualRateHz;
            if (hz > 0.0) info_.rateSource = "手动设置";
        }
        if (hz <= 0.0) {
            error = "文件里没有采样率信息，请在“采样率”框里填写。";
            break;
        }
        info_.sampleRate = hz;
        ok = true;
        break;
    }
    }
    if (!ok) {
        close();
        return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_), std::ios::beg);
    if (!file_) {
        error = "无法定位到文件的数据区。";
        close();
        return false;
    }
    open_ = true;
    return true;
}

bool IqReader::openWav(std::string& error) {
    file_.clear();
    file_.seekg(0, std::ios::beg);
    unsigned char head[12] = {};
    file_.read(reinterpret_cast<char*>(head), 12);
    const bool rf64 = std::memcmp(head, "RF64", 4) == 0;
    if (file_.gcount() != 12 || (!rf64 && std::memcmp(head, "RIFF", 4) != 0) ||
        std::memcmp(head + 8, "WAVE", 4) != 0) {
        error = "不是有效的 WAV 文件（缺少 RIFF/RF64/WAVE 头）。";
        return false;
    }
    bool haveFmt = false, haveData = false;
    unsigned format = 0, channels = 0, bits = 0, validBits = 0;
    std::uint32_t rate = 0;
    std::uint64_t dataSize = 0;
    std::uint64_t ds64DataSize = 0;
    while (file_) {
        unsigned char chunk[8] = {};
        file_.read(reinterpret_cast<char*>(chunk), 8);
        if (file_.gcount() != 8) break;
        const std::uint32_t size = le32(chunk + 4);
        const std::uint64_t payload = static_cast<std::uint64_t>(file_.tellg());
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size > 4096) {
                error = "WAV 的 fmt 块长度异常。";
                return false;
            }
            std::vector<unsigned char> fmt(size);
            file_.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(size));
            if (size < 16 || static_cast<std::size_t>(file_.gcount()) != size) {
                error = "WAV 的 fmt 块不完整。";
                return false;
            }
            format = le16(fmt.data());
            channels = le16(fmt.data() + 2);
            rate = le32(fmt.data() + 4);
            bits = le16(fmt.data() + 14);
            validBits = bits;
            if (format == 0xFFFE && size >= 40) {   // WAVE_FORMAT_EXTENSIBLE: the real code is in the sub-format GUID
                validBits = le16(fmt.data() + 18);
                format = le16(fmt.data() + 24);
            }
            haveFmt = true;
        } else if (std::memcmp(chunk, "ds64", 4) == 0) {
            if (size > 4096 || size < 16) {
                error = "WAV 的 ds64 块长度异常。";
                return false;
            }
            std::vector<unsigned char> ds64(size);
            file_.read(reinterpret_cast<char*>(ds64.data()), static_cast<std::streamsize>(size));
            if (static_cast<std::size_t>(file_.gcount()) != size) {
                error = "WAV 的 ds64 块不完整。";
                return false;
            }
            // ds64 payload: riffSize (8), dataSize (8), sampleCount (8), ...
            ds64DataSize = le64(ds64.data() + 8);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataOffset_ = payload;
            dataSize = size;
            if (rf64 && (size == 0xFFFFFFFFu || size == 0xFFFFFFFEu) && ds64DataSize > 0) dataSize = ds64DataSize;
            haveData = true;
            break;
        } else {
            file_.seekg(static_cast<std::streamoff>(payload + size), std::ios::beg);
        }
        if (size & 1u) file_.seekg(1, std::ios::cur);
        if (haveFmt && haveData) break;
    }
    if (!haveFmt || !haveData) {
        error = "WAV 缺少 fmt 或 data 块。";
        return false;
    }
    (void)validBits;
    if (channels != 2) {
        error = "WAV 必须是双声道（左 = I，右 = Q）；这个文件有 " + std::to_string(channels) + " 个声道。";
        return false;
    }
    const bool isFloat = (format == 3);
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (isFloat && bits == 32))) {
        error = "不支持的 WAV 编码（格式 " + std::to_string(format) + "，" + std::to_string(bits) +
                " 位）；支持 8/16/24/32 位整数 PCM 和 32 位浮点。";
        return false;
    }
    if (rate == 0) {
        error = "WAV 头里的采样率无效。";
        return false;
    }
    if (dataOffset_ >= fileSize_) {
        error = "WAV 的 data 块位置无效。";
        return false;
    }
    // Recorders that stream to disk leave the data size at 0 or 0xFFFFFFFF; then the rest of the file is the data.
    if (dataSize == 0 || dataSize == 0xFFFFFFFFull || dataSize == 0xFFFFFFFEull ||
        dataSize > fileSize_ - dataOffset_)
        dataSize = fileSize_ - dataOffset_;
    frameBytes_ = 2 * (bits / 8);
    info_.frames = dataSize / frameBytes_;
    info_.wavBits = static_cast<int>(bits);
    info_.wavFloat = isFloat;
    info_.sampleRate = static_cast<double>(rate);
    info_.rateSource = "WAV 头";
    if (info_.frames == 0) {
        error = "WAV 里没有数据。";
        return false;
    }
    return true;
}

bool IqReader::openZiq(std::string& error) {
    file_.clear();
    file_.seekg(0, std::ios::beg);
    unsigned char header[22] = {};
    file_.read(reinterpret_cast<char*>(header), 22);
    if (file_.gcount() != 22 || std::memcmp(header, "ZIQ_", 4) != 0) {
        error = "不是有效的 ZIQ 文件（缺少 ZIQ_ 签名）。";
        return false;
    }
    const bool compressed = header[4] != 0;
    const int bits = header[5];
    const std::uint64_t rate = le64(header + 6);
    const std::uint64_t annotation = le64(header + 14);
    if (bits != 8 && bits != 16 && bits != 32) {
        error = "ZIQ 的位深不受支持（" + std::to_string(bits) + "）。";
        return false;
    }
    if (annotation > fileSize_ || 22 + annotation > fileSize_) {
        error = "ZIQ 头里的注解长度无效。";
        return false;
    }
    if (rate == 0) {
        error = "ZIQ 头里的采样率无效。";
        return false;
    }
    dataOffset_ = 22 + annotation;
    if (dataOffset_ >= fileSize_) {
        error = "ZIQ 文件里没有采样数据。";
        return false;
    }
    frameBytes_ = static_cast<std::size_t>(bits / 8) * 2;
    info_.ziqBits = bits;
    info_.ziqCompressed = compressed;
    info_.sampleRate = static_cast<double>(rate);
    info_.rateSource = "ZIQ 头";
    if (compressed) {
#ifdef HRIT_HAVE_ZSTD
        zstdCtx_ = ZSTD_createDCtx();
        if (!zstdCtx_) {
            error = "无法初始化 zstd 解压器。";
            return false;
        }
        // The decompressed length of a ZIQ stream is not stored anywhere, so the sample count stays
        // unknown until the data has been read.
        info_.frames = 0;
        info_.framesExact = false;
#else
        error = "这个 ZIQ 文件是 zstd 压缩的，但本程序构建时没有启用 zstd。请使用未压缩的 ZIQ，或用带 zstd 的构建。";
        return false;
#endif
    } else {
        info_.frames = (fileSize_ - dataOffset_) / frameBytes_;
        info_.framesExact = true;
        if (info_.frames == 0) {
            error = "ZIQ 文件里没有完整的采样。";
            return false;
        }
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_), std::ios::beg);
    return static_cast<bool>(file_);
}

bool IqReader::openZiq2(std::string& error) {
    file_.clear();
    file_.seekg(0, std::ios::beg);
    unsigned char signature[4] = {};
    file_.read(reinterpret_cast<char*>(signature), 4);
    if (file_.gcount() != 4 || std::memcmp(signature, "ZIQ2", 4) != 0) {
        error = "不是有效的 ZIQ2 文件（缺少 ZIQ2 签名）。";
        return false;
    }
    ziq2Samples_.clear();
    ziq2SamplePos_ = 0;
    std::string packetError;
    if (!readZiq2Packet(&packetError)) {
        error = packetError.empty() ? "ZIQ2 文件头不完整。" : packetError;
        return false;
    }
    if (ziq2LastType_ != 0 || info_.sampleRate <= 0.0) {
        error = "ZIQ2 文件头里没有采样率信息包。";
        return false;
    }
    dataOffset_ = static_cast<std::uint64_t>(file_.tellg());
    info_.rateSource = "ZIQ2 头";
    info_.frames = 0;
    info_.framesExact = false;
    ziq2Samples_.clear();
    ziq2SamplePos_ = 0;
    return true;
}

bool IqReader::readZiq2Packet(std::string* error) {
    ziq2LastType_ = -1;
    unsigned char header[9] = {};
    file_.read(reinterpret_cast<char*>(header), 9);
    if (file_.gcount() != 9) {
        if (error) *error = "ZIQ2 数据包不完整（文件被截断）。";
        return false;
    }
    if (std::memcmp(header, kZiq2Sync, 4) != 0) {
        if (error) *error = "ZIQ2 数据包失去同步（找不到 1A CF FC 1D 标记）。";
        return false;
    }
    const std::uint32_t size = le32(header + 4);
    ziq2LastType_ = header[8];
    if (size < 5 || size > 64u * 1024u * 1024u) {
        if (error) *error = "ZIQ2 数据包长度异常（" + std::to_string(size) + " 字节）。";
        return false;
    }
    ziq2Packet_.resize(size);
    file_.read(reinterpret_cast<char*>(ziq2Packet_.data()), static_cast<std::streamsize>(size));
    if (static_cast<std::uint32_t>(file_.gcount()) != size) {
        if (error) *error = "ZIQ2 数据包被截断。";
        return false;
    }
    if (ziq2LastType_ == 0) {   // ZIQ2_PKT_INFO
        if (size < 8) {
            if (error) *error = "ZIQ2 信息包长度异常。";
            return false;
        }
        info_.sampleRate = static_cast<double>(le64(ziq2Packet_.data()));
        return true;
    }
    if (ziq2LastType_ == 1) {   // ZIQ2_PKT_IQ
        if (size < 5) {
            if (error) *error = "ZIQ2 IQ 包长度异常。";
            return false;
        }
        const int bits = ziq2Packet_[0];
        const float scale = leFloat(ziq2Packet_.data() + 1);
        if ((bits != 8 && bits != 16) || !(scale > 0.0f) || !std::isfinite(scale)) {
            if (error) *error = "ZIQ2 IQ 包的位深或缩放系数无效。";
            return false;
        }
        const std::size_t bytes = size - 5;
        const std::size_t samples = (bytes * 8u) / (static_cast<std::size_t>(bits) * 2u);
        ziq2Samples_.resize(samples);
        ziq2SamplePos_ = 0;
        const unsigned char* p = ziq2Packet_.data() + 5;
        if (bits == 8) {
            for (std::size_t i = 0; i < samples; ++i) {
                const float re = static_cast<float>(static_cast<signed char>(p[2 * i])) / scale;
                const float im = static_cast<float>(static_cast<signed char>(p[2 * i + 1])) / scale;
                ziq2Samples_[i] = {std::isfinite(re) ? re : 0.0f, std::isfinite(im) ? im : 0.0f};
            }
        } else {
            for (std::size_t i = 0; i < samples; ++i) {
                const auto re = static_cast<std::int16_t>(le16(p + 4 * i));
                const auto im = static_cast<std::int16_t>(le16(p + 4 * i + 2));
                const float fre = static_cast<float>(re) / scale;
                const float fim = static_cast<float>(im) / scale;
                ziq2Samples_[i] = {std::isfinite(fre) ? fre : 0.0f, std::isfinite(fim) ? fim : 0.0f};
            }
        }
        return true;
    }
    // Unknown packet type: skip it. The next call starts at the next packet.
    return true;
}

std::size_t IqReader::read(std::vector<std::complex<float>>& out, std::size_t maxFrames) {
    out.clear();
    if (!open_ || maxFrames == 0) return 0;
    std::size_t want = maxFrames;
    if (info_.framesExact) {
        if (position_ >= info_.frames) return 0;
        want = static_cast<std::size_t>(std::min<std::uint64_t>(maxFrames, info_.frames - position_));
    }
    if (want == 0) return 0;
    if (info_.format == IqFormat::Ziq2) return readZiq2(want, out);
    if (info_.format == IqFormat::Ziq && info_.ziqCompressed) return readZiqCompressed(want, out);
    return readRaw(want, out);
}

std::size_t IqReader::readRaw(std::size_t want, std::vector<std::complex<float>>& out) {
    const std::size_t need = want * frameBytes_;
    raw_.resize(need);
    file_.read(reinterpret_cast<char*>(raw_.data()), static_cast<std::streamsize>(need));
    const std::size_t got = static_cast<std::size_t>(file_.gcount()) / frameBytes_;
    decodeBlock(raw_.data(), got, out);
    position_ += got;
    return got;
}

std::size_t IqReader::readZiqCompressed(std::size_t want, std::vector<std::complex<float>>& out) {
#ifdef HRIT_HAVE_ZSTD
    const std::size_t need = want * frameBytes_;
    raw_.resize(need);
    std::size_t have = 0;
    while (have < need) {
        if (zstdInPos_ >= zstdInSize_) {
            zstdIn_.resize(64 * 1024);
            file_.read(reinterpret_cast<char*>(zstdIn_.data()), static_cast<std::streamsize>(zstdIn_.size()));
            zstdInSize_ = static_cast<std::size_t>(file_.gcount());
            zstdInPos_ = 0;
            if (zstdInSize_ == 0) break;
        }
        ZSTD_inBuffer in{zstdIn_.data() + zstdInPos_, zstdInSize_ - zstdInPos_, 0};
        ZSTD_outBuffer ob{raw_.data() + have, need - have, 0};
        const std::size_t ret = ZSTD_decompressStream(static_cast<ZSTD_DCtx*>(zstdCtx_), &ob, &in);
        zstdInPos_ += in.pos;
        have += ob.pos;
        if (ZSTD_isError(ret)) break;
        if (ret != 0 && in.pos == 0 && ob.pos == 0) break;   // no progress: avoid a spin
    }
    const std::size_t got = have / frameBytes_;
    decodeBlock(raw_.data(), got, out);
    position_ += got;
    return got;
#else
    (void)want;
    out.clear();
    return 0;
#endif
}

std::size_t IqReader::readZiq2(std::size_t want, std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(want);
    while (out.size() < want) {
        if (ziq2SamplePos_ < ziq2Samples_.size()) {
            const std::size_t n =
                std::min<std::size_t>(want - out.size(), ziq2Samples_.size() - ziq2SamplePos_);
            out.insert(out.end(), ziq2Samples_.begin() + static_cast<std::ptrdiff_t>(ziq2SamplePos_),
                       ziq2Samples_.begin() + static_cast<std::ptrdiff_t>(ziq2SamplePos_ + n));
            ziq2SamplePos_ += n;
            continue;
        }
        if (!readZiq2Packet(nullptr)) break;
    }
    position_ += out.size();
    return out.size();
}

bool IqReader::seek(std::uint64_t frame) {
    if (!open_) return false;
    std::uint64_t target = frame;
    if (info_.framesExact) target = std::min(frame, info_.frames);
    if (info_.format == IqFormat::Ziq2 || (info_.format == IqFormat::Ziq && info_.ziqCompressed))
        return seekByReading(target);
    position_ = target;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_ + target * frameBytes_), std::ios::beg);
    return static_cast<bool>(file_);
}

bool IqReader::seekByReading(std::uint64_t target) {
    if (info_.format == IqFormat::Ziq && info_.ziqCompressed) resetZstd();
    if (info_.format == IqFormat::Ziq2) {
        ziq2Samples_.clear();
        ziq2SamplePos_ = 0;
    }
    position_ = 0;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_), std::ios::beg);
    if (!file_) return false;
    std::vector<std::complex<float>> discard;
    while (position_ < target) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(target - position_, 65536));
        if (read(discard, want) == 0) break;
    }
    return position_ == target;
}

double IqReader::progress() {
    if (!open_ || fileSize_ == 0) return 0.0;
    const auto pos = file_.tellg();
    if (pos < 0) return 1.0;   // at EOF, or the stream cannot tell
    const double p = static_cast<double>(pos) / static_cast<double>(fileSize_);
    return std::clamp(p, 0.0, 1.0);
}

void IqReader::decodeBlock(const unsigned char* p, std::size_t frames, std::vector<std::complex<float>>& out) const {
    out.resize(frames);
    switch (info_.format) {
    case IqFormat::Cs8:
        for (std::size_t i = 0; i < frames; ++i) {
            out[i] = {static_cast<float>(static_cast<signed char>(p[2 * i])) / 128.0f,
                      static_cast<float>(static_cast<signed char>(p[2 * i + 1])) / 128.0f};
        }
        break;
    case IqFormat::Cs16:
        for (std::size_t i = 0; i < frames; ++i) {
            const auto re = static_cast<std::int16_t>(le16(p + 4 * i));
            const auto im = static_cast<std::int16_t>(le16(p + 4 * i + 2));
            out[i] = {static_cast<float>(re) / 32768.0f, static_cast<float>(im) / 32768.0f};
        }
        break;
    case IqFormat::Cs32:
        for (std::size_t i = 0; i < frames; ++i) {
            const auto re = static_cast<std::int32_t>(le32(p + 8 * i));
            const auto im = static_cast<std::int32_t>(le32(p + 8 * i + 4));
            out[i] = {static_cast<float>(static_cast<double>(re) / 2147483648.0),
                      static_cast<float>(static_cast<double>(im) / 2147483648.0)};
        }
        break;
    case IqFormat::Cf32:
        for (std::size_t i = 0; i < frames; ++i) {
            const float re = leFloat(p + 8 * i);
            const float im = leFloat(p + 8 * i + 4);
            out[i] = {std::isfinite(re) ? re : 0.0f, std::isfinite(im) ? im : 0.0f};
        }
        break;
    case IqFormat::Cu8:
        for (std::size_t i = 0; i < frames; ++i) {
            const float re = (static_cast<float>(p[2 * i]) - 127.0f) / 127.0f;
            const float im = (static_cast<float>(p[2 * i + 1]) - 127.0f) / 127.0f;
            out[i] = {re, im};
        }
        break;
    case IqFormat::Wav:
        for (std::size_t i = 0; i < frames; ++i) {
            const unsigned char* s = p + i * frameBytes_;
            float v[2];
            for (int ch = 0; ch < 2; ++ch) {
                switch (info_.wavBits) {
                case 8: v[ch] = (static_cast<float>(s[ch]) - 128.0f) / 128.0f; break;
                case 16: v[ch] = static_cast<float>(static_cast<std::int16_t>(le16(s + 2 * ch))) / 32768.0f; break;
                case 24: {
                    const unsigned char* b = s + 3 * ch;
                    std::int32_t x = static_cast<std::int32_t>(b[0] | (b[1] << 8) | (b[2] << 16));
                    if (x & 0x800000) x |= ~0xFFFFFF;
                    v[ch] = static_cast<float>(x) / 8388608.0f;
                    break;
                }
                default:
                    if (info_.wavFloat) {
                        const float f = leFloat(s + 4 * ch);
                        v[ch] = std::isfinite(f) ? f : 0.0f;
                    } else {
                        const auto x = static_cast<std::int32_t>(le32(s + 4 * ch));
                        v[ch] = static_cast<float>(static_cast<double>(x) / 2147483648.0);
                    }
                }
            }
            out[i] = {v[0], v[1]};
        }
        break;
    case IqFormat::Ziq:
        if (info_.ziqBits == 8) {
            for (std::size_t i = 0; i < frames; ++i)
                out[i] = {static_cast<float>(static_cast<signed char>(p[2 * i])) / 127.0f,
                          static_cast<float>(static_cast<signed char>(p[2 * i + 1])) / 127.0f};
        } else if (info_.ziqBits == 16) {
            for (std::size_t i = 0; i < frames; ++i) {
                const auto re = static_cast<std::int16_t>(le16(p + 4 * i));
                const auto im = static_cast<std::int16_t>(le16(p + 4 * i + 2));
                out[i] = {static_cast<float>(re) / 32767.0f, static_cast<float>(im) / 32767.0f};
            }
        } else {
            for (std::size_t i = 0; i < frames; ++i) {
                const float re = leFloat(p + 8 * i);
                const float im = leFloat(p + 8 * i + 4);
                out[i] = {std::isfinite(re) ? re : 0.0f, std::isfinite(im) ? im : 0.0f};
            }
        }
        break;
    case IqFormat::Ziq2:
        break;   // readZiq2 fills the output directly
    }
}

void IqReader::resetZstd() {
#ifdef HRIT_HAVE_ZSTD
    if (zstdCtx_) ZSTD_DCtx_reset(static_cast<ZSTD_DCtx*>(zstdCtx_), ZSTD_reset_session_only);
#endif
    zstdInPos_ = 0;
    zstdInSize_ = 0;
    zstdIn_.clear();
}

void IqReader::close() {
#ifdef HRIT_HAVE_ZSTD
    if (zstdCtx_) {
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(zstdCtx_));
        zstdCtx_ = nullptr;
    }
#endif
    if (file_.is_open()) file_.close();
    file_.clear();
    open_ = false;
    position_ = 0;
    dataOffset_ = 0;
    fileSize_ = 0;
    frameBytes_ = 4;
    raw_.clear();
    zstdIn_.clear();
    zstdInPos_ = 0;
    zstdInSize_ = 0;
    ziq2Samples_.clear();
    ziq2SamplePos_ = 0;
    ziq2Packet_.clear();
    ziq2LastType_ = -1;
    info_ = IqInfo{};
}

}  // namespace hritapp
