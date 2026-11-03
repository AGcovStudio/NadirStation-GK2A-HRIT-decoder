// Command-line check of the receive chain without the window: test_pipeline <file.cs8|cs16|wav> [sampleRateHz]
#include "iqreader.h"
#include "pipeline.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s file.cs8|cs16|wav [sampleRateHz]\n", argv[0]); return 2; }
    hritapp::IqReader reader;
    std::string error;
    if (!reader.open(argv[1], argc > 2 ? std::atof(argv[2]) : 0.0, error)) { std::fprintf(stderr, "open: %s\n", error.c_str()); return 1; }
    const auto& info = reader.info();
    std::printf("format %s, %.0f Hz (%s), %llu samples, %.2f s\n", hritapp::formatName(info.format), info.sampleRate,
                info.rateSource.c_str(), static_cast<unsigned long long>(info.frames), info.frames / info.sampleRate);
    hritapp::HritPipeline pipe;
    if (!pipe.configure(info.sampleRate, error)) { std::fprintf(stderr, "configure: %s\n", error.c_str()); return 1; }
    std::map<std::string, int> names;
    auto onFile = [&](nadirstation::gk2a::LritFile&& f) { ++names[f.fileName.substr(0, 24)]; };
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::complex<float>> block;
    while (reader.read(block, 65536) > 0) pipe.process(block, onFile);
    pipe.finish(onFile);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto s = pipe.stats();
    std::printf("RS ok %zu / sync %zu | files %zu | packets %zu | crc fail %zu | lock %.2f | cfo %.0f Hz | SNR %.1f dB | %.1f s\n",
                s.cadusRsOk, s.cadusWithSync, s.filesCompleted, s.packets, s.crcFailures, s.lockQuality, s.carrierOffsetHz, s.snrDb, sec);
    for (auto& [n, c] : names) std::printf("  %s x%d\n", n.c_str(), c);
    return 0;
}
