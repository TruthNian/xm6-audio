/* SPDX-License-Identifier: MIT */
/* Integer PCM contract oracle uses quotient/remainder, independent of the
 * production shift implementation. No audio/device APIs. */
#include "oa2dp/pcm_ring.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define REQUIRE(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
static uint64_t checked_samples;
static int32_t reference(int64_t value,unsigned precision,unsigned bits,bool downmix) {
    const int64_t divisor=INT64_C(1)<<(precision-bits+(downmix?1:0));
    const bool negative=value<0;
    const int64_t magnitude=negative?-value:value; // at most two int32 samples
    int64_t quantized=magnitude/divisor;
    if(divisor>1 && magnitude%divisor>=divisor/2) ++quantized;
    if(negative) quantized=-quantized;
    const int64_t limit=INT64_C(1)<<(bits-1);
    quantized=std::max(-limit,std::min(limit-1,quantized));
    return int32_t(quantized)*(bits==16?256:1);
}
static void check(const std::vector<int32_t>& input,unsigned input_channels,unsigned output_channels,unsigned bits,unsigned precision) {
    const auto frames=static_cast<uint32_t>(input.size()/input_channels);
    std::vector<int32_t> expected(size_t(frames)*output_channels);
    for(size_t frame=0;frame<frames;++frame) {
        if(input_channels==2 && output_channels==1)
            expected[frame]=reference(int64_t(input[2*frame])+input[2*frame+1],precision,bits,true);
        else for(unsigned ch=0;ch<output_channels;++ch)
            expected[frame*output_channels+ch]=reference(input[frame*input_channels+(input_channels==1?0:ch)],precision,bits,false);
    }
    oa2dp_ring_format f{}; f.channels=input_channels;
    f.format=precision==32?OA2DP_RING_FMT_S32_FULL:OA2DP_RING_FMT_S32;
    std::vector<uint8_t> unaligned(input.size()*4+1);
    std::memcpy(unaligned.data()+1,input.data(),input.size()*4);
    std::vector<int32_t> guarded(expected.size()+2,0x12345678);
    REQUIRE(oa2dp_pcm_prepare_encoder(&f,unaligned.data()+1,input.size()*4,guarded.data()+1,expected.size(),frames,output_channels,bits)==OA2DP_OK);
    REQUIRE(guarded.front()==0x12345678 && guarded.back()==0x12345678);
    REQUIRE(std::equal(expected.begin(),expected.end(),guarded.begin()+1));
    std::vector<int32_t> in_place(size_t(frames)*std::max(input_channels,output_channels));
    std::copy(input.begin(),input.end(),in_place.begin());
    REQUIRE(oa2dp_pcm_prepare_encoder(&f,in_place.data(),input.size()*4,in_place.data(),in_place.size(),frames,output_channels,bits)==OA2DP_OK);
    REQUIRE(std::equal(expected.begin(),expected.end(),in_place.begin()));
    if(precision==32 && input_channels==output_channels) {
        std::vector<int32_t> direct(input.size());
        oa2dp_pcm_s32_to_quantized24(input.data(),direct.data(),direct.size(),static_cast<uint8_t>(bits));
        REQUIRE(direct==expected);
    }
    checked_samples+=expected.size();
}
int main() {
    uint32_t seed=0x729ab431;
    for(unsigned precision:{24u,32u}) {
        std::vector<int32_t> exact16(65536);
        for(size_t i=0;i<exact16.size();++i) exact16[i]=int32_t((int64_t(i)-32768)*(precision==32?65536:256));
        for(unsigned bits:{16u,24u})for(unsigned channels:{1u,2u})check(exact16,1,channels,bits,precision);
        for(unsigned batch=0;batch<128;++batch) {
            std::vector<int32_t> input(256);
            for(auto &v:input) {
                seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
                if(precision==24) v=int32_t(seed&0xffffff)-0x800000;
                else std::memcpy(&v,&seed,4);
            }
            input[0]=precision==32?INT32_MIN:-8388608;
            input[1]=input[0];
            input[2]=precision==32?INT32_MAX:8388607;
            input[3]=input[2];
            for(unsigned bits:{16u,24u})for(unsigned in:{1u,2u})for(unsigned out:{1u,2u})check(input,in,out,bits,precision);
        }
        std::vector<int32_t> edges;
        for(int64_t value=-131073;value<=131073;++value)edges.push_back(int32_t(value));
        for(unsigned bits:{16u,24u})check(edges,1,1,bits,precision);
    }
    std::printf("integer PCM quotient/remainder oracle matched %llu output samples, including exhaustive int16 and rounding boundaries\n",static_cast<unsigned long long>(checked_samples));
}
