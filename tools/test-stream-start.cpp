/* SPDX-License-Identifier: MIT */
/* Real service admission body and PCM ring: audio can arrive while AVDTP
 * START/encoder creation is pending. It must reach the first encoder call. */
#include "oa2dp/pcm_ring.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define LOGE(...) ((void)0)
static void oa2dp_encoder_destroy(void *) {}
struct Admission {
    struct Audio {
        unsigned minimum=0;
        bool set_data_event(unsigned frames) { minimum=frames; return true; }
    } audio_;
    void fail_stream() { REQUIRE(false); }
    void begin(oa2dp_pcm_ring *ring, const oa2dp_ring_format &format) {
        struct { uint32_t samples_per_call=128,samples_per_frame=128; uint8_t channels=2; } info;
        struct { oa2dp_ring_format ring_format; } stream{format};
        void *enc=nullptr;
#include "encoder-admission-under-test.inc"
    }
};
int main() {
    for (uint16_t type : {uint16_t(OA2DP_RING_FMT_S32), uint16_t(OA2DP_RING_FMT_S32_FULL),
                          uint16_t(OA2DP_RING_FMT_FLOAT32)}) {
        std::vector<uint64_t> storage((oa2dp_ring_bytes(8192,2)+7)/8);
        auto *ring=reinterpret_cast<oa2dp_pcm_ring *>(storage.data());
        REQUIRE(oa2dp_ring_init(ring,8192,48000,2,type)==OA2DP_OK);
        std::vector<int32_t> obsolete(256*2,-1),prefix(2400*2);
        for(size_t i=0;i<prefix.size();++i)prefix[i]=static_cast<int32_t>(i+1);
        REQUIRE(oa2dp_ring_write(ring,obsolete.data(),256)==256);
        REQUIRE(oa2dp_ring_begin_format(ring,48000,2,type)==1);
        oa2dp_ring_format current{};
        REQUIRE(oa2dp_ring_snapshot(ring,&current)==OA2DP_OK);
        // Fifty milliseconds of the *new* clip queues before the encoder starts.
        REQUIRE(oa2dp_ring_write(ring,prefix.data(),2400)==2400);
        Admission admission;
        admission.begin(ring,current);
        REQUIRE(admission.audio_.minimum==128);
        uint32_t frames=0;int32_t block[256]{};
        REQUIRE(oa2dp_ring_read(ring,&current,block,sizeof(block),128,&frames)==OA2DP_OK);
        if(frames!=128)std::fprintf(stderr,"New clip prefix lost at encoder startup: read %u/128 frames\n",frames);
        REQUIRE(frames==128 && block[0]==1 && block[255]==256);
        REQUIRE(oa2dp_ring_readable(ring)==2400-128);
        // Restarting only the sender must preserve the unread suffix as well.
        admission.begin(ring,current);
        REQUIRE(oa2dp_ring_read(ring,&current,block,sizeof(block),128,&frames)==OA2DP_OK);
        REQUIRE(frames==128 && block[0]==257);
    }
    std::puts("Queued clip prefix/suffix survive encoder admission; obsolete epochs excluded.");
}
