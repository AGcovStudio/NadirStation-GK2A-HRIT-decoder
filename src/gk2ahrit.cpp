// GK-2A HRIT data link: see gk2ahrit.h. HritDecoder is implemented in gk2adecoder.cpp
// (QpskLinkDecoder), shared with the LRIT path. HritViterbi below is the previous HRIT Viterbi,
// kept as a reference implementation for NadirStation_HritTest.
#include "gk2ahrit.h"
#include <algorithm>
#include <cmath>
#include <bit>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define HRIT_SSE2 1
#endif
namespace nadirstation::gk2a {
HritViterbi::HritViterbi() {
    for(int s=0;s<64;++s) {
        signA_[s]=(std::popcount(unsigned(s & kG1))&1)?0:-1;
        signB_[s]=(std::popcount(unsigned(s & kG2))&1)?0:-1;
    }
    reset();
}
void HritViterbi::reset() { metrics_.fill(0); steps_=0; }
void HritViterbi::feed(float a,float b,const std::function<void(uint8_t)>& output) {
    const auto quant=[](float v)->int16_t {
        if(!std::isfinite(v))return 0;
        const float q=std::clamp(v*24.f,-31.f,31.f);
        // round away from zero, like lround, without a per-soft-bit libm call.
        return int16_t(q+(q>=0.f?0.5f:-0.5f));
    };
    const int16_t qa=quant(a), qb=quant(b);
    alignas(16) std::array<int16_t,64> next;
    auto& decisions=survivors_[steps_%256];
#ifdef HRIT_SSE2
    const auto av=_mm_set1_epi16(qa), bv=_mm_set1_epi16(qb);
    for(int s=0;s<64;s+=8) {
        auto m0=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(metrics_.data()+s/2));
        auto m1=_mm_loadl_epi64(reinterpret_cast<const __m128i*>(metrics_.data()+s/2+32));
        m0=_mm_unpacklo_epi16(m0,m0); m1=_mm_unpacklo_epi16(m1,m1);
        const auto sa=_mm_load_si128(reinterpret_cast<const __m128i*>(signA_.data()+s));
        const auto sb=_mm_load_si128(reinterpret_cast<const __m128i*>(signB_.data()+s));
        const auto cost=_mm_add_epi16(_mm_sub_epi16(_mm_xor_si128(av,sa),sa),
                                     _mm_sub_epi16(_mm_xor_si128(bv,sb),sb));
        m0=_mm_add_epi16(m0,cost); m1=_mm_sub_epi16(m1,cost);
        _mm_store_si128(reinterpret_cast<__m128i*>(next.data()+s),_mm_max_epi16(m0,m1));
        _mm_store_si128(reinterpret_cast<__m128i*>(decisions.data()+s),_mm_cmpgt_epi16(m1,m0));
    }
#else
    for(int s=0;s<64;++s) {
        const int c=(signA_[s]?-qa:qa)+(signB_[s]?-qb:qb);
        const int x=metrics_[s/2]+c,y=metrics_[s/2+32]-c;
        next[s]=int16_t(std::max(x,y)); decisions[s]=y>x?-1:0;
    }
#endif
    metrics_=next;
    ++steps_;
    // Normalize long-running metrics before signed-16 overflow.
    if(steps_%32==0) {
        const int16_t best=*std::max_element(metrics_.begin(),metrics_.end());
        for(auto& m:metrics_) m-=best;
    }
    // 128-bit output blocks with another 128 steps of convergence, invariant to IQ block sizes.
    if(steps_>=256 && steps_%128==0) {
        int state=int(std::max_element(metrics_.begin(),metrics_.end())-metrics_.begin());
        std::array<uint8_t,128> bits{};
        for(int j=255;j>=0;--j) {
            const auto pos=(steps_-256+uint64_t(j))%256;
            if(j<128) bits[j]=uint8_t(state&1);
            state=(state>>1)|(survivors_[pos][state]?32:0);
        }
        for(auto v:bits) output(v);
    }
}
}
