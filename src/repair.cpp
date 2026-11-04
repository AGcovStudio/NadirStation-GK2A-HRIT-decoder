#include "repair.h"

#include "dsp.h"
#include "gk2adecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <map>

namespace hritapp {

namespace {

constexpr std::uint32_t kCounterMask = 0xFFFFFFu;
constexpr std::uint32_t kFillVcid = 63;
constexpr std::uint32_t kMaxGapFrames = 8;     // longer runs are a lost signal, not a lost frame

struct FrameKey {
    std::uint32_t vcid = 0;
    std::uint32_t counter = 0;
};

// AOS transfer frame header after the 4-byte marker: byte 5 = SCID low bits + 6-bit VCID,
// bytes 6..8 = 24-bit virtual channel frame count.
bool keyOf(const std::vector<std::uint8_t>& f, FrameKey& k) {
    if (f.size() < 10) return false;
    k.vcid = f[5] & 0x3Fu;
    k.counter = (static_cast<std::uint32_t>(f[6]) << 16) | (static_cast<std::uint32_t>(f[7]) << 8) | f[8];
    return true;
}

struct Gap {
    std::size_t afterIndex = 0;                 // index in `frames` of the last good frame before the gap
    std::uint32_t vcid = 0;
    std::uint32_t after = 0;                    // its counter
    std::vector<std::uint32_t> missing;         // counters that should follow it
};

struct Variant {
    float alpha, carrierBw, timingBw, softScale;
};

// Ordered by how often each one rescued a frame the default setting lost on a real 1.4 dB recording.
constexpr Variant kVariants[] = {
    {0.50f, 2e-4f, 2e-4f, 0.30f}, {0.50f, 1e-4f, 1e-4f, 0.30f}, {0.50f, 3e-4f, 4e-4f, 0.30f},
    {0.50f, 1e-4f, 2e-4f, 0.30f}, {0.50f, 5e-4f, 4e-4f, 0.30f}, {0.60f, 5e-4f, 2e-4f, 0.30f},
    {0.60f, 1e-4f, 4e-4f, 0.30f}, {0.60f, 2e-4f, 4e-4f, 0.30f}, {0.35f, 3e-4f, 4e-4f, 0.30f},
    {0.35f, 3e-4f, 1e-4f, 0.30f}, {0.40f, 3e-4f, 1e-4f, 0.30f}, {0.50f, 2e-4f, 2e-4f, 0.50f},
    {0.50f, 3e-4f, 1e-4f, 0.30f}, {0.40f, 2e-4f, 2e-4f, 0.30f}, {0.50f, 2e-4f, 2e-4f, 0.20f},
    {0.50f, 2e-4f, 2e-4f, 0.40f}, {0.45f, 2e-4f, 2e-4f, 0.30f}, {0.30f, 3e-4f, 1e-4f, 0.30f},
    {0.25f, 2e-4f, 2e-4f, 0.30f}, {0.50f, 5e-4f, 5e-4f, 0.30f}, {0.35f, 1e-4f, 2e-4f, 0.30f},
    {0.60f, 3e-4f, 3e-4f, 0.30f}, {0.40f, 5e-4f, 2e-4f, 0.30f}, {0.50f, 1e-3f, 1e-3f, 0.30f},
};

// Decodes samples [start, end) of the recording with one setting and returns the checked frames.
std::vector<std::vector<std::uint8_t>> decodeWindow(IqReader& reader, std::uint64_t start, std::uint64_t end,
                                                    const Variant& v) {
    std::vector<std::vector<std::uint8_t>> out;
    if (!reader.seek(start)) return out;
    const double fs = reader.info().sampleRate;
    nadirstation::dsp::QpskDemod demod;
    demod.configure(static_cast<float>(fs), 3.0e6f, v.alpha, v.carrierBw, v.timingBw);
    nadirstation::gk2a::QpskLinkDecoder link;
    link.setSoftScale(v.softScale);
    std::vector<std::complex<float>> block;
    while (reader.position() < end) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(65536, end - reader.position()));
        if (reader.read(block, want) == 0) break;
        const auto r = demod.process(block);
        link.feedSymbols(r.symbols);
        for (auto& c : link.takeCadus())
            if (c.syncFound && c.rsOk) out.push_back(std::move(c.bytes));
    }
    return out;
}

}  // namespace

RepairReport repairFrames(IqReader& reader, std::vector<std::vector<std::uint8_t>>& frames,
                          const std::function<bool()>& stopRequested,
                          const std::function<void(const std::string&)>& log, double budgetSeconds) {
    RepairReport rep;
    // ---- find the gaps -------------------------------------------------------------------------------------
    std::vector<Gap> gaps;
    std::map<std::uint32_t, std::pair<std::size_t, std::uint32_t>> last;   // vcid -> (index, counter)
    for (std::size_t i = 0; i < frames.size(); ++i) {
        FrameKey k;
        if (!keyOf(frames[i], k) || k.vcid == kFillVcid) continue;
        const auto it = last.find(k.vcid);
        if (it != last.end()) {
            const std::uint32_t step = (k.counter - it->second.second) & kCounterMask;
            if (step >= 2 && step <= kMaxGapFrames + 1) {
                Gap g;
                g.afterIndex = it->second.first;
                g.vcid = k.vcid;
                g.after = it->second.second;
                for (std::uint32_t s = 1; s < step; ++s) g.missing.push_back((g.after + s) & kCounterMask);
                rep.framesMissing += g.missing.size();
                gaps.push_back(std::move(g));
                ++rep.gaps;
            }
        }
        last[k.vcid] = {i, k.counter};
    }
    if (gaps.empty() || reader.info().frames == 0) return rep;
    if (rep.framesMissing == 0) return rep;
    if (log) log("第一遍有 " + std::to_string(rep.framesMissing) + " 个帧没有解出来，正在换参数回头重解这些位置…");

    // Frames are evenly spaced in time, so the place of a gap in the recording follows from how far into
    // the frame sequence it is. The estimate drifts a little (the sample clock is not exactly the symbol
    // clock), so each window is measured against the counters it actually contains and moved if needed.
    const double totalFrames = static_cast<double>(frames.size() + rep.framesMissing);
    const double perFrame = static_cast<double>(reader.info().frames) / totalFrames;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    std::map<std::size_t, std::map<std::uint32_t, std::vector<std::uint8_t>>> insertAfter;   // frames to add
    for (const Gap& gap : gaps) {
        if (stopRequested && stopRequested()) break;
        std::map<std::uint32_t, std::vector<std::uint8_t>> found;   // counter -> frame
        const std::uint32_t before = (gap.after + static_cast<std::uint32_t>(gap.missing.size()) + 1) & kCounterMask;
        const double centre = (static_cast<double>(gap.afterIndex) + 1.0 + 0.5 * static_cast<double>(gap.missing.size())) * perFrame;
        double shiftFrames = 0.0;   // correction learnt from the windows seen so far
        int shiftTries = 0;
        for (std::size_t vi = 0; vi < std::size(kVariants) && found.size() < gap.missing.size(); ++vi) {
            if (elapsed() > budgetSeconds) { rep.budgetExceeded = true; break; }
            if (stopRequested && stopRequested()) break;
            const double margin = 48.0 + 16.0 * static_cast<double>(vi % 3);
            const double half = (0.5 * static_cast<double>(gap.missing.size()) + margin) * perFrame;
            const double c = centre + shiftFrames * perFrame;
            const std::uint64_t s0 = static_cast<std::uint64_t>(std::max(0.0, c - half));
            const std::uint64_t s1 = static_cast<std::uint64_t>(std::min(static_cast<double>(reader.info().frames), c + half));
            if (s1 <= s0) continue;
            ++rep.decodes;
            const auto got = decodeWindow(reader, s0, s1, kVariants[vi]);
            // Where in the counter sequence did this window land?
            std::int64_t lo = INT64_MAX, hi = INT64_MIN;
            for (const auto& f : got) {
                FrameKey k;
                if (!keyOf(f, k) || k.vcid != gap.vcid) continue;
                const std::int64_t rel = static_cast<std::int32_t>(((k.counter - gap.after) & kCounterMask) << 8) >> 8;   // signed 24-bit distance from `after`
                lo = std::min(lo, rel);
                hi = std::max(hi, rel);
                if (rel >= 1 && rel <= static_cast<std::int64_t>(gap.missing.size()) && !found.count(k.counter)) found[k.counter] = f;
            }
            if (hi == INT64_MIN) continue;   // nothing locked in this window; the next setting may do better
            const double want = 0.5 * static_cast<double>(gap.missing.size() + 1);
            const double got_mid = 0.5 * static_cast<double>(lo + hi);
            if (std::fabs(got_mid - want) > 12.0 && shiftTries < 4) {
                // the window is off: move it by the counter difference and give the same setting another go
                shiftFrames += want - got_mid;
                ++shiftTries;
                --vi;
            }
        }
        if (!found.empty()) {
            rep.framesRecovered += found.size();
            insertAfter[gap.afterIndex] = std::move(found);
        }
        (void)before;
        if (rep.budgetExceeded) break;
    }

    // ---- put the recovered frames in their place ---------------------------------------------------------------
    if (!insertAfter.empty()) {
        std::vector<std::vector<std::uint8_t>> merged;
        merged.reserve(frames.size() + rep.framesRecovered);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            merged.push_back(std::move(frames[i]));
            const auto it = insertAfter.find(i);
            if (it == insertAfter.end()) continue;
            // `found` is ordered by counter; counters of a gap are consecutive, so plain order is stream order
            for (auto& kv : it->second) merged.push_back(std::move(kv.second));
        }
        frames = std::move(merged);
    }
    if (log)
        log("丢失帧找回：" + std::to_string(rep.framesRecovered) + " / " + std::to_string(rep.framesMissing) +
            (rep.budgetExceeded ? "（时间预算用完，剩下的放弃）" : ""));
    return rep;
}

}  // namespace hritapp
