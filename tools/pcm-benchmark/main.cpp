/* SPDX-License-Identifier: MIT */
// Computational timing only: no audio device, Bluetooth or player access.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "oa2dp/pcm_ring.h"

using Prepare=int (*)(const oa2dp_ring_format *,const void *,size_t,int32_t *,size_t,uint32_t,uint16_t,uint8_t);
#ifdef BENCH_PAIRED
extern "C" int before_oa2dp_pcm_prepare_encoder(const oa2dp_ring_format *,const void *,size_t,int32_t *,size_t,uint32_t,uint16_t,uint8_t);
extern "C" int after_oa2dp_pcm_prepare_encoder(const oa2dp_ring_format *,const void *,size_t,int32_t *,size_t,uint32_t,uint16_t,uint8_t);
#endif

static uint64_t ticks() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
static uint64_t cycles() {
    ULONG64 value=0;
    if(!QueryThreadCycleTime(GetCurrentThread(),&value)) std::exit(2);
    return value;
}
int main() {
    DWORD_PTR process_mask=0,system_mask=0,previous_mask=0;
    if(GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask) && process_mask)
        previous_mask=SetThreadAffinityMask(GetCurrentThread(),process_mask & (~process_mask+1));
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    constexpr unsigned frames=128,iterations=25000;
#ifdef BENCH_PAIRED
    const Prepare functions[]={before_oa2dp_pcm_prepare_encoder,after_oa2dp_pcm_prepare_encoder};
    const char *labels[]={"before","after"};
#else
    const Prepare functions[]={oa2dp_pcm_prepare_encoder};
    const char *labels[]={BENCH_VARIANT};
#endif
    constexpr size_t variants=sizeof(functions)/sizeof(functions[0]);
    uint32_t seed=0x3197ab21;
    std::printf("variant,format,input_channels,output_channels,bits,pinned,ns_per_block,cycles_per_block,checksum\n");
    for(unsigned format:{unsigned(OA2DP_RING_FMT_S32_FULL),unsigned(OA2DP_RING_FMT_S32),unsigned(OA2DP_RING_FMT_FLOAT32)})
        for(unsigned channels:{1u,2u})
            for(unsigned output_channels:{1u,2u})
                for(unsigned bits:{16u,24u}) {
                    oa2dp_ring_format descriptor{};
                    descriptor.channels=channels;
                    descriptor.format=format;
                    std::vector<int32_t> input(frames*channels),buffer(frames*std::max(channels,2u));
                    for(auto &sample:input) {
                        seed^=seed<<13; seed^=seed>>17; seed^=seed<<5;
                        if(format==OA2DP_RING_FMT_FLOAT32) {
                            const float value=float(int32_t(seed&0xffffff)-0x800000)/8388608.f;
                            std::memcpy(&sample,&value,4);
                        } else if(format==OA2DP_RING_FMT_S32) sample=int32_t(seed&0xffffff)-0x800000;
                        else std::memcpy(&sample,&seed,4);
                    }
                    auto prepare=[&](Prepare function) {
                        // The service reads fresh PCM before converting in the same buffer.
                        std::memcpy(buffer.data(),input.data(),input.size()*4);
                        if(function(&descriptor,buffer.data(),input.size()*4,
                                buffer.data(),buffer.size(),frames,output_channels,bits)!=OA2DP_OK)
                            std::exit(3);
                    };
                    for(unsigned i=0;i<2000;++i) for(auto function:functions) prepare(function);
                    std::vector<double> times[variants],counts[variants];
                    for(unsigned batch=0;batch<6;++batch) for(size_t slot=0;slot<variants;++slot) {
                        const size_t variant=(batch+slot)%variants; // balance which runs first
                        const auto begin=ticks(),begin_cycles=cycles();
                        for(unsigned i=0;i<iterations;++i) prepare(functions[variant]);
                        const auto end_cycles=cycles(),end=ticks();
                        times[variant].push_back(double(end-begin)*1e9/double(frequency.QuadPart)/iterations);
                        counts[variant].push_back(double(end_cycles-begin_cycles)/iterations);
                    }
                    const char *name=format==OA2DP_RING_FMT_S32_FULL?"integer32":
                        format==OA2DP_RING_FMT_S32?"integer-in-s24":"float32";
                    uint64_t first_hash=0;
                    for(size_t variant=0;variant<variants;++variant) {
                        std::sort(times[variant].begin(),times[variant].end());
                        std::sort(counts[variant].begin(),counts[variant].end());
                        prepare(functions[variant]);
                        uint64_t hash=1469598103934665603ull;
                        for(size_t i=0;i<frames*output_channels;++i) {
                            hash^=uint32_t(buffer[i]); hash*=1099511628211ull;
                        }
                        if(variant && hash!=first_hash) return 4;
                        first_hash=hash;
                        std::printf("%s,%s,%u,%u,%u,%u,%.3f,%.3f,%llu\n",labels[variant],name,
                            channels,output_channels,bits,previous_mask?1u:0u,times[variant][3],counts[variant][3],
                            static_cast<unsigned long long>(hash));
                    }
                }
    if(previous_mask) SetThreadAffinityMask(GetCurrentThread(),previous_mask);
}
