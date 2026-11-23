// Command-line check of the receive chain without the window:
//   hrit_cli <recording> [sampleRateHz] [--no-repair] [--dump=cadus.bin]
//   hrit_cli --formats
#include "iqreader.h"
#include "pipeline.h"
#include "repair.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

void printFormats() {
    std::printf("supported baseband formats:\n");
    for (hritapp::IqFormat f : {hritapp::IqFormat::Cs8, hritapp::IqFormat::Cs16, hritapp::IqFormat::Cs32,
                                hritapp::IqFormat::Cf32, hritapp::IqFormat::Cu8, hritapp::IqFormat::Wav,
                                hritapp::IqFormat::Ziq, hritapp::IqFormat::Ziq2}) {
        std::printf("  %-5s %s\n", hritapp::formatShortName(f), hritapp::formatName(f));
    }
#ifdef HRIT_HAVE_ZSTD
    std::printf("ziq zstd compression: yes\n");
#else
    std::printf("ziq zstd compression: no (uncompressed ZIQ only)\n");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <recording> [sampleRateHz] [--no-repair] [--dump=cadus.bin]\n"
                     "       %s --formats\n",
                     argv[0], argv[0]);
        return 2;
    }
    if (!std::strcmp(argv[1], "--formats")) { printFormats(); return 0; }
    bool repair = true;
    const char* dumpPath = nullptr;
    double rate = 0.0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-repair")) repair = false;
        else if (!std::strncmp(argv[i], "--dump=", 7)) dumpPath = argv[i] + 7;
        else rate = std::atof(argv[i]);
    }
    hritapp::IqReader reader;
    std::string error;
    if (!reader.open(argv[1], rate, error)) { std::fprintf(stderr, "open: %s\n", error.c_str()); return 1; }
    const auto& info = reader.info();
    if (info.framesExact)
        std::printf("format %s, %.0f Hz (%s), %llu samples, %.2f s\n", hritapp::describeFormat(info).c_str(),
                    info.sampleRate, info.rateSource.c_str(), static_cast<unsigned long long>(info.frames),
                    info.frames / info.sampleRate);
    else
        std::printf("format %s, %.0f Hz (%s), sample count unknown (streaming format)\n",
                    hritapp::describeFormat(info).c_str(), info.sampleRate, info.rateSource.c_str());
    hritapp::HritPipeline pipe;
    if (!pipe.configure(info.sampleRate, error)) { std::fprintf(stderr, "configure: %s\n", error.c_str()); return 1; }
    pipe.setCollectFrames(repair);
    std::map<std::string, int> names;
    std::size_t damaged = 0;
    auto onFile = [&](nadirstation::gk2a::LritFile&& f) { ++names[f.fileName.substr(0, 24)]; if (!f.holes.empty() || f.lostPackets) ++damaged; };
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::complex<float>> block;
    while (reader.read(block, 65536) > 0) pipe.process(block, onFile);
    pipe.finish(onFile);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto s = pipe.stats();
    std::printf("RS ok %zu / sync %zu | files %zu (damaged %zu) | packets %zu | crc fail %zu | lock %.2f | cfo %.0f Hz | SNR %.1f dB | %.1f s\n",
                s.cadusRsOk, s.cadusWithSync, s.filesCompleted, damaged, s.packets, s.crcFailures, s.lockQuality, s.carrierOffsetHz, s.snrDb, sec);
    for (auto& [n, c] : names) std::printf("  %s x%d\n", n.c_str(), c);
    if (repair && damaged > 0) {
        auto frames = std::move(pipe.collectedFrames());
        const auto t1 = std::chrono::steady_clock::now();
        const auto rep = hritapp::repairFrames(reader, frames, [] { return false; },
                                               [](const std::string& m) { std::printf("%s\n", m.c_str()); });
        std::printf("repair: gaps %zu, missing %zu, recovered %zu, decodes %zu, %.1f s\n", rep.gaps, rep.framesMissing, rep.framesRecovered,
                    rep.decodes, std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
        if (dumpPath) {
            if (FILE* fo = std::fopen(dumpPath, "wb")) { for (const auto& f : frames) std::fwrite(f.data(), 1, f.size(), fo); std::fclose(fo); }
        }
        std::size_t files = 0, dmg = 0;
        hritapp::HritPipeline::assembleFrames(frames, [&](nadirstation::gk2a::LritFile&& f) { ++files; if (!f.holes.empty() || f.lostPackets) ++dmg; });
        std::printf("after repair: files %zu, damaged %zu\n", files, dmg);
    }
    return 0;
}
