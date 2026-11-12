// SPDX-License-Identifier: MIT
#include "../service/src/media_clock.h"
#include "../service/src/pcm_feed.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)

static void clips() {
    for (uint32_t rate : {44100u,48000u,88200u,96000u})
    for (uint16_t type : {uint16_t(OA2DP_RING_FMT_S32),uint16_t(OA2DP_RING_FMT_S32_FULL),uint16_t(OA2DP_RING_FMT_FLOAT32)})
    for (uint16_t channels : {uint16_t(1),uint16_t(2)}) {
        std::vector<uint64_t> memory((oa2dp_ring_bytes(8192,2)+7)/8);
        auto *r=reinterpret_cast<oa2dp_pcm_ring *>(memory.data());
        REQUIRE(oa2dp_ring_init(r,8192,rate,2,type)==OA2DP_OK);
        REQUIRE(!oa2dp_ring_consumer_ready(r));
        REQUIRE(oa2dp_ring_set_format(r,rate,channels,type)>=0);
        oa2dp_ring_format format{};
        REQUIRE(oa2dp_ring_snapshot(r,&format)==OA2DP_OK);
        REQUIRE(oa2dp_ring_admit(r,&format)==OA2DP_OK && oa2dp_ring_consumer_ready(r));
        for (uint32_t length : {1u,127u,128u,129u,255u,511u,2401u}) {
            const auto old=format;
            REQUIRE(oa2dp_ring_begin_format(r,rate,channels,type)==1);
            REQUIRE(!oa2dp_ring_consumer_ready(r));
            REQUIRE(oa2dp_ring_admit(r,&old)==OA2DP_ERR_STATE);
            REQUIRE(!oa2dp::adopt_pcm_epoch(r,format,old.sequence));
            REQUIRE(oa2dp::adopt_pcm_epoch(r,format,r->format_sequence));
            REQUIRE(oa2dp_ring_consumer_ready(r));
            r->flags|=OA2DP_RING_FLAG_RUNNING;
            std::vector<int32_t> input(length*channels),output(128*channels,-1);
            for(size_t i=0;i<input.size();++i)input[i]=static_cast<int32_t>(i+1);
            REQUIRE(oa2dp_ring_write(r,input.data(),length)==length);
            uint32_t taken=0;
            while(taken+128<=length) {
                uint32_t n=0; bool ready=false;
                REQUIRE(oa2dp_ring_read_block(r,&format,output.data(),output.size()*4,128,&n,&ready)==OA2DP_OK);
                REQUIRE(ready && n==128);
                REQUIRE(std::equal(output.begin(),output.end(),input.begin()+taken*channels));
                taken+=n;
            }
            uint32_t n=99; bool ready=true;
            REQUIRE(oa2dp_ring_read_block(r,&format,output.data(),output.size()*4,128,&n,&ready)==OA2DP_OK);
            REQUIRE(!ready && n==0); // do not insert silence into an active partial block
            r->flags|=OA2DP_RING_FLAG_EOS; // EOS can precede the engine's STOP
            REQUIRE(oa2dp_ring_read_block(r,&format,output.data(),output.size()*4,128,&n,&ready)==OA2DP_OK);
            REQUIRE(ready && n==length-taken);
            REQUIRE(std::equal(output.begin(),output.begin()+n*channels,input.begin()+taken*channels));
            REQUIRE(std::all_of(output.begin()+n*channels,output.end(),[](int32_t v){return v==0;}));
            taken+=n;
            REQUIRE(taken==length && oa2dp_ring_readable(r)==0);
            for(int i=0;i<8;++i) {
                REQUIRE(oa2dp_ring_read_block(r,&format,output.data(),output.size()*4,128,&n,&ready)==OA2DP_OK);
                REQUIRE(ready && n==0 && std::all_of(output.begin(),output.end(),[](int32_t v){return v==0;}));
            }
            r->flags&=~OA2DP_RING_FLAG_RUNNING;
        }
        oa2dp_ring_retire(r);
        REQUIRE(!oa2dp_ring_consumer_ready(r));
        REQUIRE(oa2dp_ring_admit(r,&format)==OA2DP_OK);
        // A rate or storage change cannot be adopted by the old encoder even
        // if a caller accidentally authorizes its sequence.
        REQUIRE(oa2dp_ring_begin_format(r,rate==48000?44100:48000,channels,type)==1);
        REQUIRE(!oa2dp::adopt_pcm_epoch(r,format,r->format_sequence));
        REQUIRE(!oa2dp_ring_consumer_ready(r));
        REQUIRE(oa2dp_ring_overruns(r)==0);
    }
}

static void pacing() {
    for(uint32_t rate : {44100u,48000u,88200u,96000u})
    for(uint64_t hz : {3000000ull,10000000ull,24000000ull}) {
        oa2dp::MediaClock clock(hz,rate);
        uint64_t frames=0,now=1234567,origin=now;
        for(unsigned i=0;i<20000;++i) {
            const auto due=clock.deadline(now);
            REQUIRE(due==origin+frames*hz/rate);
            // Always-available input / instantaneous sends still take media
            // time. The old sender had no deadline and could drain at CPU speed.
            const uint32_t packet=i%3?512:128;
            frames+=packet;
            clock.advance(packet);
            now=due;
        }
        now+=hz; // one-second scheduling/link stall: no giant catch-up burst
        REQUIRE(clock.deadline(now)==now);
        clock.advance(512);
        REQUIRE(clock.deadline(now)>now);
    }
}
int main(){clips();pacing();std::puts("Short clips: exact prefix/tail, epoch admission, EOS padding and bounded media pacing passed.");}
