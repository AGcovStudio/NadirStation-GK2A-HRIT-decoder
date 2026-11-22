// test_iqreader.cpp - checks the baseband reader against every format it claims to read.
// Builds small recordings in a temporary folder, opens them through IqReader and compares the samples.
// No Qt is needed, so this runs in the command-line build and in CI.
#include "iqreader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef HRIT_HAVE_ZSTD
#include <zstd.h>
#endif

namespace fs = std::filesystem;
using hritapp::IqFormat;
using hritapp::IqInfo;
using hritapp::IqReader;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

bool near(float a, float b, float tolerance = 0.002f) { return std::fabs(a - b) <= tolerance; }

void put8(std::vector<unsigned char>& v, std::uint8_t x) { v.push_back(x); }
void put16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFF));
    v.push_back(static_cast<unsigned char>(x >> 8));
}
void put32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}
void put64(std::vector<unsigned char>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF));
}
void putFloat(std::vector<unsigned char>& v, float x) {
    std::uint32_t u = 0;
    std::memcpy(&u, &x, sizeof u);
    put32(v, u);
}
void putBytes(std::vector<unsigned char>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
}

void writeFile(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

const std::vector<std::complex<float>> kSamples = {
    {0.0f, 0.0f}, {0.5f, -0.5f}, {-0.25f, 0.75f}, {1.0f, -1.0f},
};

std::int16_t encode16(float v) {
    const float scaled = std::round(v * 32768.0f);
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0f, 32767.0f));
}
std::int32_t encode32(float v) {
    const double scaled = std::round(static_cast<double>(v) * 2147483648.0);
    return static_cast<std::int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0));
}
std::int8_t encode8(float v) {
    const float scaled = std::round(v * 128.0f);
    return static_cast<std::int8_t>(std::clamp(scaled, -128.0f, 127.0f));
}
std::int16_t encodeZiq16(float v) {
    const float scaled = std::round(v * 32767.0f);
    return static_cast<std::int16_t>(std::clamp(scaled, -32767.0f, 32767.0f));
}
std::int8_t encodeZiq8(float v) {
    const float scaled = std::round(v * 127.0f);
    return static_cast<std::int8_t>(std::clamp(scaled, -127.0f, 127.0f));
}

std::vector<std::complex<float>> readAll(IqReader& reader, std::size_t blockSize = 3) {
    std::vector<std::complex<float>> all, block;
    while (reader.read(block, blockSize) > 0) all.insert(all.end(), block.begin(), block.end());
    return all;
}

void checkSamples(const std::vector<std::complex<float>>& got, float tolerance, const std::string& label) {
    check(got.size() == kSamples.size(), label + ": sample count");
    for (std::size_t i = 0; i < std::min(got.size(), kSamples.size()); ++i) {
        check(near(got[i].real(), kSamples[i].real(), tolerance) && near(got[i].imag(), kSamples[i].imag(), tolerance),
              label + ": sample " + std::to_string(i));
    }
}

// ---- file writers ----------------------------------------------------------------------------------------------

std::vector<unsigned char> makeCs8() {
    std::vector<unsigned char> v;
    for (const auto& s : kSamples) {
        v.push_back(static_cast<unsigned char>(encode8(s.real())));
        v.push_back(static_cast<unsigned char>(encode8(s.imag())));
    }
    return v;
}
std::vector<unsigned char> makeCs16() {
    std::vector<unsigned char> v;
    for (const auto& s : kSamples) {
        put16(v, static_cast<std::uint16_t>(encode16(s.real())));
        put16(v, static_cast<std::uint16_t>(encode16(s.imag())));
    }
    return v;
}
std::vector<unsigned char> makeCs32() {
    std::vector<unsigned char> v;
    for (const auto& s : kSamples) {
        put32(v, static_cast<std::uint32_t>(encode32(s.real())));
        put32(v, static_cast<std::uint32_t>(encode32(s.imag())));
    }
    return v;
}
std::vector<unsigned char> makeCf32() {
    std::vector<unsigned char> v;
    for (const auto& s : kSamples) {
        putFloat(v, s.real());
        putFloat(v, s.imag());
    }
    return v;
}
std::vector<unsigned char> makeCu8() {
    std::vector<unsigned char> v;
    for (const auto& s : kSamples) {
        v.push_back(static_cast<unsigned char>(std::clamp(std::round(127.0f + s.real() * 127.0f), 0.0f, 255.0f)));
        v.push_back(static_cast<unsigned char>(std::clamp(std::round(127.0f + s.imag() * 127.0f), 0.0f, 255.0f)));
    }
    return v;
}

std::vector<unsigned char> makeWav(int bits, bool isFloat, std::uint32_t rate,
                                   const std::vector<unsigned char>& data, bool rf64 = false) {
    const std::size_t frameBytes = 2u * static_cast<std::size_t>(bits / 8);
    std::vector<unsigned char> v;
    putBytes(v, rf64 ? "RF64" : "RIFF");
    put32(v, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(36 + data.size() + (rf64 ? 36 : 0)));
    putBytes(v, "WAVE");
    if (rf64) {
        putBytes(v, "ds64");
        put32(v, 28);
        put64(v, 0);                              // riffSize
        put64(v, data.size());                    // dataSize
        put64(v, data.size() / frameBytes);       // sampleCount
        put32(v, 0);                              // tableLength
    }
    putBytes(v, "fmt ");
    put32(v, 16);
    put16(v, static_cast<std::uint16_t>(isFloat ? 3 : 1));
    put16(v, 2);
    put32(v, rate);
    put32(v, rate * static_cast<std::uint32_t>(frameBytes));
    put16(v, static_cast<std::uint16_t>(frameBytes));
    put16(v, static_cast<std::uint16_t>(bits));
    putBytes(v, "data");
    put32(v, rf64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(data.size()));
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

std::vector<unsigned char> makeWav16(bool rf64 = false) {
    std::vector<unsigned char> data;
    for (const auto& s : kSamples) {
        put16(data, static_cast<std::uint16_t>(encode16(s.real())));
        put16(data, static_cast<std::uint16_t>(encode16(s.imag())));
    }
    return makeWav(16, false, 3200000, data, rf64);
}
std::vector<unsigned char> makeWavFloat() {
    std::vector<unsigned char> data;
    for (const auto& s : kSamples) {
        putFloat(data, s.real());
        putFloat(data, s.imag());
    }
    return makeWav(32, true, 3200000, data);
}

std::vector<unsigned char> makeZiqHeader(bool compressed, int bits, std::uint64_t rate, const std::string& annotation) {
    std::vector<unsigned char> v;
    putBytes(v, "ZIQ_");
    put8(v, compressed ? 1 : 0);
    put8(v, static_cast<std::uint8_t>(bits));
    put64(v, rate);
    put64(v, annotation.size());
    putBytes(v, annotation);
    return v;
}
std::vector<unsigned char> makeZiq(bool compressed, int bits, const std::string& annotation = "{}") {
    std::vector<unsigned char> data;
    if (bits == 8) {
        for (const auto& s : kSamples) {
            data.push_back(static_cast<unsigned char>(encodeZiq8(s.real())));
            data.push_back(static_cast<unsigned char>(encodeZiq8(s.imag())));
        }
    } else if (bits == 16) {
        for (const auto& s : kSamples) {
            put16(data, static_cast<std::uint16_t>(encodeZiq16(s.real())));
            put16(data, static_cast<std::uint16_t>(encodeZiq16(s.imag())));
        }
    } else {
        for (const auto& s : kSamples) {
            putFloat(data, s.real());
            putFloat(data, s.imag());
        }
    }
    std::vector<unsigned char> v = makeZiqHeader(compressed, bits, 3200000, annotation);
#ifdef HRIT_HAVE_ZSTD
    if (compressed) {
        std::vector<unsigned char> compressedData(ZSTD_compressBound(data.size()));
        const std::size_t n =
            ZSTD_compress(compressedData.data(), compressedData.size(), data.data(), data.size(), 3);
        check(!ZSTD_isError(n), "zstd compress");
        compressedData.resize(n);
        v.insert(v.end(), compressedData.begin(), compressedData.end());
        return v;
    }
#else
    (void)compressed;
#endif
    v.insert(v.end(), data.begin(), data.end());
    return v;
}

std::vector<unsigned char> makeZiq2Packet(std::uint8_t type, const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> v;
    v.insert(v.end(), {0x1a, 0xcf, 0xfc, 0x1d});
    put32(v, static_cast<std::uint32_t>(payload.size()));
    put8(v, type);
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}
std::vector<unsigned char> makeZiq2() {
    std::vector<unsigned char> v;
    putBytes(v, "ZIQ2");
    std::vector<unsigned char> info;
    put64(info, 3200000);
    const auto infoPkt = makeZiq2Packet(0, info);
    v.insert(v.end(), infoPkt.begin(), infoPkt.end());

    std::vector<unsigned char> iq;
    put8(iq, 16);
    putFloat(iq, 32767.0f);
    for (const auto& s : kSamples) {
        put16(iq, static_cast<std::uint16_t>(encodeZiq16(s.real())));
        put16(iq, static_cast<std::uint16_t>(encodeZiq16(s.imag())));
    }
    const auto iqPkt = makeZiq2Packet(1, iq);
    v.insert(v.end(), iqPkt.begin(), iqPkt.end());
    return v;
}

// ---- tests ------------------------------------------------------------------------------------------------------

void testRaw(const fs::path& dir) {
    {
        const fs::path p = dir / "sample.cs8";
        writeFile(p, makeCs8());
        fs::path sidecar = p;
        sidecar += ".txt";
        std::ofstream(sidecar) << "rate 6400000\n";
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open cs8: " + err);
        check(r.info().format == IqFormat::Cs8, "cs8 format");
        check(near(static_cast<float>(r.info().sampleRate), 6400000.0f), "cs8 rate from sidecar");
        check(r.info().rateSource == "同名 .txt", "cs8 rate source");
        check(r.info().frames == kSamples.size(), "cs8 frame count");
        check(r.info().framesExact, "cs8 frame count exact");
        checkSamples(readAll(r), 0.01f, "cs8");
    }
    {
        const fs::path p = dir / "2026-01-01_12-00-00_3200000SPS_1695400000Hz.cs16";
        writeFile(p, makeCs16());
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open cs16: " + err);
        check(r.info().format == IqFormat::Cs16, "cs16 format");
        check(near(static_cast<float>(r.info().sampleRate), 3200000.0f), "cs16 rate from filename");
        check(r.info().rateSource == "文件名", "cs16 rate source");
        checkSamples(readAll(r), 0.001f, "cs16");
        std::vector<std::complex<float>> one;
        check(r.seek(2) && r.read(one, 1) == 1 && one.size() == 1, "cs16 seek read");
        check(near(one[0].real(), kSamples[2].real(), 0.001f) && near(one[0].imag(), kSamples[2].imag(), 0.001f),
              "cs16 seek sample");
    }
    {
        const fs::path p = dir / "sample.cs32";
        writeFile(p, makeCs32());
        IqReader r;
        std::string err;
        check(r.open(p, 6000000.0, err), "open cs32: " + err);
        check(r.info().format == IqFormat::Cs32, "cs32 format");
        check(near(static_cast<float>(r.info().sampleRate), 6000000.0f), "cs32 manual rate");
        checkSamples(readAll(r), 0.0001f, "cs32");
    }
    {
        const fs::path p = dir / "sample.cf32";
        writeFile(p, makeCf32());
        IqReader r;
        std::string err;
        check(r.open(p, 6000000.0, err), "open cf32: " + err);
        check(r.info().format == IqFormat::Cf32, "cf32 format");
        checkSamples(readAll(r), 0.0001f, "cf32");
    }
    {
        const fs::path p = dir / "sample.cu8";
        writeFile(p, makeCu8());
        IqReader r;
        std::string err;
        check(r.open(p, 6000000.0, err), "open cu8: " + err);
        check(r.info().format == IqFormat::Cu8, "cu8 format");
        checkSamples(readAll(r), 0.01f, "cu8");
    }
    {
        const fs::path p = dir / "sample.txt";
        std::ofstream(p) << "not IQ data\n";
        IqReader r;
        std::string err;
        check(!r.open(p, 0.0, err), "unsupported file rejected");
    }
}

void testWav(const fs::path& dir) {
    {
        const fs::path p = dir / "sample.wav";
        writeFile(p, makeWav16(false));
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open wav16: " + err);
        check(r.info().format == IqFormat::Wav, "wav format");
        check(r.info().wavBits == 16 && !r.info().wavFloat, "wav bit depth");
        check(near(static_cast<float>(r.info().sampleRate), 3200000.0f), "wav rate");
        check(r.info().rateSource == "WAV 头", "wav rate source");
        check(r.info().frames == kSamples.size(), "wav frame count");
        checkSamples(readAll(r), 0.001f, "wav16");
    }
    {
        const fs::path p = dir / "float.wav";
        writeFile(p, makeWavFloat());
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open wav float: " + err);
        check(r.info().wavFloat && r.info().wavBits == 32, "wav float bit depth");
        checkSamples(readAll(r), 0.0001f, "wav float");
    }
    {
        const fs::path p = dir / "rf64.wav";
        writeFile(p, makeWav16(true));
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open rf64 wav: " + err);
        check(r.info().frames == kSamples.size(), "rf64 frame count");
        checkSamples(readAll(r), 0.001f, "rf64");
    }
}

void testZiq(const fs::path& dir) {
    for (int bits : {8, 16, 32}) {
        const fs::path p = dir / ("sample" + std::to_string(bits) + ".ziq");
        writeFile(p, makeZiq(false, bits));
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open ziq " + std::to_string(bits) + ": " + err);
        if (!r.isOpen()) continue;
        check(r.info().format == IqFormat::Ziq, "ziq format");
        check(r.info().ziqBits == bits, "ziq bits");
        check(!r.info().ziqCompressed, "ziq uncompressed flag");
        check(near(static_cast<float>(r.info().sampleRate), 3200000.0f), "ziq rate");
        check(r.info().frames == kSamples.size(), "ziq frame count");
        check(r.info().framesExact, "ziq frame count exact");
        checkSamples(readAll(r), bits == 8 ? 0.02f : 0.001f, "ziq" + std::to_string(bits));
        std::vector<std::complex<float>> one;
        check(r.seek(2) && r.read(one, 1) == 1, "ziq seek");
        check(near(one[0].real(), kSamples[2].real(), bits == 8 ? 0.02f : 0.001f), "ziq seek sample");
    }
    {
        const fs::path p = dir / "magic.bin";
        writeFile(p, makeZiq(false, 16));
        IqFormat format = IqFormat::Wav;
        check(hritapp::formatForPath(p, format) && format == IqFormat::Ziq, "ziq detected by content, not extension");
    }
#ifdef HRIT_HAVE_ZSTD
    {
        const fs::path p = dir / "compressed.ziq";
        writeFile(p, makeZiq(true, 16));
        IqReader r;
        std::string err;
        check(r.open(p, 0.0, err), "open compressed ziq: " + err);
        if (r.isOpen()) {
            check(r.info().ziqCompressed, "ziq compressed flag");
            check(!r.info().framesExact && r.info().frames == 0, "compressed ziq frame count unknown");
            checkSamples(readAll(r), 0.001f, "ziq compressed");
            std::vector<std::complex<float>> one;
            check(r.seek(2) && r.read(one, 1) == 1, "compressed ziq seek");
            check(near(one[0].real(), kSamples[2].real(), 0.001f), "compressed ziq seek sample");
        }
    }
#endif
}

void testZiq2(const fs::path& dir) {
    const fs::path p = dir / "sample.ziq2";
    writeFile(p, makeZiq2());
    IqReader r;
    std::string err;
    check(r.open(p, 0.0, err), "open ziq2: " + err);
    if (!r.isOpen()) return;
    check(r.info().format == IqFormat::Ziq2, "ziq2 format");
    check(near(static_cast<float>(r.info().sampleRate), 3200000.0f), "ziq2 rate");
    check(!r.info().framesExact && r.info().frames == 0, "ziq2 frame count unknown");
    checkSamples(readAll(r), 0.001f, "ziq2");
    std::vector<std::complex<float>> one;
    check(r.seek(2) && r.read(one, 1) == 1, "ziq2 seek");
    check(near(one[0].real(), kSamples[2].real(), 0.001f), "ziq2 seek sample");
}

void testRateParsing() {
    check(hritapp::rateFromName("2026-01-01_12-00-00_3200000SPS_1695400000Hz.cs16") == 3200000.0,
          "rate from ...SPS filename");
    check(hritapp::rateFromName("rec_3.2MSPS.cs16") == 3200000.0, "rate from MSPS filename");
    check(hritapp::rateFromName("rec_3200ksps.cs8") == 3200000.0, "rate from ksps filename");
    check(hritapp::rateFromName("2026-01-01_12-00-00_1695400000Hz.cs16") == 0.0,
          "frequency alone is not a sample rate");
}

}  // namespace

int main() {
    fs::path dir = fs::temp_directory_path() / ("hrit_iqreader_test_" + std::to_string(::rand()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::cerr << "cannot create " << dir << ": " << ec.message() << '\n';
        return 1;
    }

    testRateParsing();
    testRaw(dir);
    testWav(dir);
    testZiq(dir);
    testZiq2(dir);

    fs::remove_all(dir, ec);
    if (g_failures == 0) std::cout << "all iqreader tests passed\n";
    else std::cerr << g_failures << " iqreader test(s) failed\n";
    return g_failures == 0 ? 0 : 1;
}