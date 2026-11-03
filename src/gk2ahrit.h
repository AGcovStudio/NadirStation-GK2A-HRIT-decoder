#pragma once
#include "gk2adecoder.h"
#include <array>
#include <complex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nadirstation::gk2a {
// GK-2A HRIT data link (NMSC HRIT Mission Specification v1.0, tables 9.1 / 8.5): QPSK 3 Mbaud,
// K=7 r=1/2 without G2 inversion, RS(255,223) interleave 4, 1024-byte CADU.
//
// Low-SNR rework (2026-11-01). This used to carry its own Viterbi (6-bit soft values) and an
// exact-match ASM search, so one wrong bit in the 32-bit marker lost the whole frame and the
// complement of a CADU (which is also an RS codeword) could be delivered after a 180-degree slip.
// It is now a thin wrapper around QpskLinkDecoder, which shares the LRIT path's decoder:
// 8-bit soft SIMD Viterbi with a 160-pair decision delay, tolerant marker search with a
// frame-clock flywheel, Reed-Solomon as the arbiter, and polarity settled on the VCDU version.
// Simulated threshold (6 MS/s, RRC 0.5, CFO, clock error): 95 % of frames at Es/N0 2.0 dB and
// 85 % at 1.5 dB, against 84 % / 67 % for the previous implementation; no wrong frame delivered.
// Previous HRIT Viterbi (6-bit soft values, 128-pair traceback). Kept as a reference for the
// HRIT self test; the receive path uses QpskLinkDecoder through HritDecoder.
class HritViterbi {
public:
    HritViterbi();
    void reset();
    // Signed likelihoods: positive means bit 1. Quantized magnitude retains confidence.
    void feed(float a, float b, const std::function<void(uint8_t)>& bit);
private:
    alignas(16) std::array<int16_t,64> metrics_{}, signA_{}, signB_{};
    alignas(16) std::array<std::array<int16_t,64>,256> survivors_{};
    uint64_t steps_=0;
};

class HritDecoder {
public:
    HritDecoder() { reset(); }
    ~HritDecoder() { setAsync(false); }
    HritDecoder(const HritDecoder&) = delete;
    HritDecoder& operator=(const HritDecoder&) = delete;

    // Run the link decoder (Viterbi, frame sync, Reed-Solomon) on its own thread. The demodulator
    // and the link decoder together need more than half a core at 6 MS/s; on one thread a busy
    // machine makes the IQ queue overflow, blocks of samples are dropped and frames are lost in
    // bursts. Results are still delivered by feed()/drain() on the caller's thread, one block late.
    void setAsync(bool on) {
        if (on == async_) return;
        if (on) {
            stop_ = false;
            async_ = true;
            worker_ = std::thread([this] { run(); });
        } else {
            {
                std::lock_guard<std::mutex> lock(m_);
                stop_ = true;
            }
            cvIn_.notify_all();
            cvDone_.notify_all();
            if (worker_.joinable()) worker_.join();
            async_ = false;
            in_.clear();
            out_.clear();
        }
    }
    bool async() const { return async_; }

    void reset() {
        if (!async_) { link_.reset(); statsView_ = link_.stats(); return; }
        std::unique_lock<std::mutex> lock(m_);
        in_.clear();
        out_.clear();
        cvDone_.wait(lock, [this] { return !busy_; });
        link_.reset();
        statsSnap_ = link_.stats();
        statsView_ = statsSnap_;
        lockedSnap_ = false;
        mappingSnap_ = 0;
    }
    // Symbols scaled to mean |I| = mean |Q| = 1. Every CADU that carried a marker or passed RS is
    // handed to `frame`; callers use only those with rsOk.
    void feed(std::vector<std::complex<float>> symbols,
              const std::function<void(Cadu&&)>& frame) {
        if (!async_) {
            link_.feedSymbols(symbols);
            for (auto& c : link_.takeCadus())
                if (c.syncFound) frame(std::move(c));
            return;
        }
        {
            std::unique_lock<std::mutex> lock(m_);
            // back-pressure: a decoder that falls behind slows the producer instead of growing a queue
            cvDone_.wait(lock, [this] { return in_.size() < kMaxQueuedBlocks || stop_; });
            if (!stop_) in_.push_back(std::move(symbols));
        }
        cvIn_.notify_one();
        deliver(frame);
    }
    // Wait for every queued block to be decoded and hand over the frames (end of a reception).
    void drain(const std::function<void(Cadu&&)>& frame) {
        if (!async_) return;
        {
            std::unique_lock<std::mutex> lock(m_);
            cvDone_.wait(lock, [this] { return (in_.empty() && !busy_) || stop_; });
        }
        deliver(frame);
    }
    const DecodeStats& stats() const { return async_ ? statsView_ : link_.stats(); }
    // A QPSK mapping validated by Reed-Solomon is in force (held through short fades).
    bool locked() const { return async_ ? lockedView_ : link_.mappingHeld(); }
    int mapping() const { return async_ ? mappingView_ : link_.hypothesis(); }
private:
    static constexpr std::size_t kMaxQueuedBlocks = 64;
    void deliver(const std::function<void(Cadu&&)>& frame) {
        std::vector<Cadu> ready;
        {
            std::lock_guard<std::mutex> lock(m_);
            ready.swap(out_);
            statsView_ = statsSnap_;
            lockedView_ = lockedSnap_;
            mappingView_ = mappingSnap_;
        }
        for (auto& c : ready) frame(std::move(c));
    }
    void run() {
        std::vector<std::complex<float>> block;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_);
                cvIn_.wait(lock, [this] { return !in_.empty() || stop_; });
                if (stop_) return;
                block = std::move(in_.front());
                in_.pop_front();
                busy_ = true;
            }
            link_.feedSymbols(block);
            auto cadus = link_.takeCadus();
            {
                std::lock_guard<std::mutex> lock(m_);
                for (auto& c : cadus)
                    if (c.syncFound) out_.push_back(std::move(c));
                statsSnap_ = link_.stats();
                lockedSnap_ = link_.mappingHeld();
                mappingSnap_ = link_.hypothesis();
                busy_ = false;
            }
            cvDone_.notify_all();
        }
    }

    QpskLinkDecoder link_;
    bool async_ = false;
    bool stop_ = false;
    bool busy_ = false;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cvIn_, cvDone_;
    std::deque<std::vector<std::complex<float>>> in_;
    std::vector<Cadu> out_;
    DecodeStats statsSnap_, statsView_;
    bool lockedSnap_ = false, lockedView_ = false;
    int mappingSnap_ = 0, mappingView_ = 0;
};
}
