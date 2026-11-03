#include "iqreader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <system_error>

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

std::uint32_t le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

bool formatForPath(const fs::path& path, IqFormat& out) {
    const auto u = path.extension().u8string();
    const std::string ext = lower(std::string(u.begin(), u.end()));
    if (ext == ".cs8") { out = IqFormat::Cs8; return true; }
    if (ext == ".cs16") { out = IqFormat::Cs16; return true; }
    if (ext == ".wav") { out = IqFormat::Wav; return true; }
    return false;
}

const char* formatName(IqFormat format) {
    switch (format) {
    case IqFormat::Cs8: return "cs8 (I/Q int8)";
    case IqFormat::Cs16: return "cs16 (I/Q int16)";
    case IqFormat::Wav: break;
    }
    return "WAV (I/Q 双声道)";
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

bool IqReader::open(const fs::path& path, double manualRateHz, std::string& error, bool preferManual) {
    close();
    IqFormat format;
    if (!formatForPath(path, format)) {
        error = "只支持 .cs8、.cs16、.wav 文件。";
        return false;
    }
    std::error_code ec;
    const std::uint64_t size = static_cast<std::uint64_t>(fs::file_size(path, ec));
    if (ec) {
        error = "无法读取文件：" + ec.message();
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
    if (format == IqFormat::Wav) {
        if (!openWav(error)) {
            close();
            return false;
        }
    } else {
        frameBytes_ = (format == IqFormat::Cs16) ? 4 : 2;
        info_.frames = size / frameBytes_;
        dataOffset_ = 0;
        if (info_.frames == 0) {
            error = "文件是空的。";
            close();
            return false;
        }
        double hz = 0.0;
        if (preferManual && manualRateHz > 0.0) { hz = manualRateHz; info_.rateSource = "手动设置"; }
        if (hz <= 0.0) { hz = rateFromSidecar(path); info_.rateSource = "同名 .txt"; }
        if (hz <= 0.0) { hz = rateFromName(utf8FileName(path)); info_.rateSource = "文件名"; }
        if (hz <= 0.0) { hz = manualRateHz; info_.rateSource = "手动设置"; }
        if (hz <= 0.0) {
            error = "文件里没有采样率信息，请在“采样率”框里填写。";
            close();
            return false;
        }
        info_.sampleRate = hz;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_), std::ios::beg);
    open_ = true;
    return true;
}

bool IqReader::openWav(std::string& error) {
    file_.seekg(0, std::ios::end);
    const std::uint64_t fileSize = static_cast<std::uint64_t>(file_.tellg());
    file_.seekg(0, std::ios::beg);
    unsigned char head[12];
    file_.read(reinterpret_cast<char*>(head), 12);
    if (file_.gcount() != 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) {
        error = "不是有效的 WAV 文件（缺少 RIFF/WAVE 头）。";
        return false;
    }
    bool haveFmt = false, haveData = false;
    unsigned format = 0, channels = 0, bits = 0, validBits = 0;
    std::uint32_t rate = 0;
    std::uint64_t dataSize = 0;
    while (file_) {
        unsigned char chunk[8];
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
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataOffset_ = payload;
            dataSize = size;
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
    // Recorders that stream to disk leave the data size at 0 or 0xFFFFFFFF; then the rest of the file is the data.
    if (dataSize == 0 || dataSize == 0xFFFFFFFFull || dataOffset_ + dataSize > fileSize) dataSize = fileSize - dataOffset_;
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

void IqReader::close() {
    if (file_.is_open()) file_.close();
    file_.clear();
    open_ = false;
    position_ = 0;
}

std::size_t IqReader::read(std::vector<std::complex<float>>& out, std::size_t maxFrames) {
    out.clear();
    if (!open_ || position_ >= info_.frames) return 0;
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(maxFrames, info_.frames - position_));
    raw_.resize(want * frameBytes_);
    file_.read(reinterpret_cast<char*>(raw_.data()), static_cast<std::streamsize>(raw_.size()));
    const std::size_t got = static_cast<std::size_t>(file_.gcount()) / frameBytes_;
    out.resize(got);
    const unsigned char* p = raw_.data();
    switch (info_.format) {
    case IqFormat::Cs8:
        for (std::size_t i = 0; i < got; ++i)
            out[i] = {static_cast<float>(static_cast<signed char>(p[2 * i])) / 128.0f,
                      static_cast<float>(static_cast<signed char>(p[2 * i + 1])) / 128.0f};
        break;
    case IqFormat::Cs16:
        for (std::size_t i = 0; i < got; ++i) {
            const auto I = static_cast<std::int16_t>(le16(p + 4 * i));
            const auto Q = static_cast<std::int16_t>(le16(p + 4 * i + 2));
            out[i] = {static_cast<float>(I) / 32768.0f, static_cast<float>(Q) / 32768.0f};
        }
        break;
    case IqFormat::Wav:
        for (std::size_t i = 0; i < got; ++i) {
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
                        const std::uint32_t u = le32(s + 4 * ch);
                        float f;
                        std::memcpy(&f, &u, sizeof f);
                        v[ch] = std::isfinite(f) ? f : 0.0f;
                    } else {
                        v[ch] = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(le32(s + 4 * ch))) / 2147483648.0);
                    }
                }
            }
            out[i] = {v[0], v[1]};
        }
        break;
    }
    position_ += got;
    return got;
}

}  // namespace hritapp
