// SPDX-License-Identifier: MIT
// Execute the complete production streaming loop. PCM and time are scripted;
// Bluetooth/Windows waits are fake. On Windows the real LDAC encoder is used.
#include "oa2dp/encoder.h"
#include "oa2dp/media.h"
#include "oa2dp/pcm_ring.h"
#include "../service/src/media_clock.h"
#include "../service/src/pcm_feed.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define LOGE(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define TraceLoggingWrite(...) ((void)0)
#define TraceLoggingProviderEnabled(...) false
using ULONG=uint32_t;using DWORD=uint32_t;using LONGLONG=int64_t;using HANDLE=unsigned;
constexpr bool FALSE=false;
constexpr DWORD WAIT_OBJECT_0=0,WAIT_FAILED=0xffffffffu,INFINITE=0xffffffffu;
constexpr DWORD CREATE_WAITABLE_TIMER_HIGH_RESOLUTION=2,TIMER_ALL_ACCESS=1;
constexpr uint32_t kAbrHighWater=6,kAbrLowWater=2,kAbrDownHoldMs=300,kAbrUpHoldMs=5000;
struct LARGE_INTEGER {int64_t QuadPart=0;};
struct OA2DP_SEND_IN {unsigned ConnectionGeneration=1;};
struct OA2DP_TX_STATE {unsigned QueuedPackets=0;};
using OA2DP_FORMAT=oa2dp_pcm_format;
struct AudioScheduling {};
static uint64_t ticks=10000,timer_due=0;
static unsigned timers=0,failures=0,waits=0;
static bool fail_timer=false;
static oa2dp_codec_id tested_codec=OA2DP_CODEC_LDAC;
static OA2DP_FORMAT tested_format{96000,2,32,32,OA2DP_PCM_INTEGER};
static void QueryPerformanceCounter(LARGE_INTEGER *v){v->QuadPart=ticks;}
static void QueryPerformanceFrequency(LARGE_INTEGER *v){v->QuadPart=10000000;}
static DWORD GetTickCount(){return static_cast<DWORD>(ticks/10000);}
static HANDLE CreateWaitableTimerExW(void *,void *,DWORD,DWORD){if(fail_timer)return 0;++timers;return 4;}
static HANDLE CreateWaitableTimerW(void *,bool,void *){if(fail_timer)return 0;++timers;return 4;}
static bool SetWaitableTimer(HANDLE h,LARGE_INTEGER *due,unsigned,void *,void *,bool){
    REQUIRE(h==4 && due->QuadPart<0);timer_due=ticks-static_cast<uint64_t>(due->QuadPart);return true;
}
static void CloseHandle(HANDLE h){REQUIRE(h==4 && timers==1);--timers;}
static DWORD WaitForMultipleObjects(unsigned,HANDLE *,bool,DWORD);

static std::vector<int32_t> admitted;
static int checked_encode(oa2dp_encoder *e,const int32_t *pcm,uint8_t *out,size_t size,size_t *len,uint8_t *frames){
    for(unsigned i=0;i<e->info.samples_per_call*e->info.channels;i+=2){
        REQUIRE(pcm[i]==pcm[i+1]);
        int32_t unit=0;
        if(e->info.input_format==OA2DP_ENCODER_INPUT_F32) {
            float f;std::memcpy(&f,pcm+i,sizeof(f));unit=static_cast<int32_t>(f*32768);
        } else unit=pcm[i]/(e->info.input_format==OA2DP_ENCODER_INPUT_S32?65536:256);
        if(unit)admitted.push_back(unit);
    }
    return oa2dp_encoder_encode(e,pcm,out,size,len,frames);
}

namespace oa2dp {
struct Session {
    enum class Cmd {StreamFailed};
    struct Command {Cmd cmd;struct {unsigned Generation=0;}channel;};
    struct StreamContext {
        unsigned generation=1;
        OA2DP_SEND_IN media;
        oa2dp_codec_caps config;
        oa2dp_settings settings;
        OA2DP_FORMAT pcm{96000,2,32,32,OA2DP_PCM_INTEGER};
        oa2dp_ring_format ring_format;
        oa2dp_pcm_ring *ring;
        unsigned mtu=895;
    };
    bool streaming_=true;
    std::atomic<ULONG> approved_pcm_sequence_{0};
    HANDLE stream_stop_event_=1,stop_event_=2;
    std::mutex status_mu_;
    struct {std::string encoder_input;unsigned bitrate=0,abr_level=0;}status_;
    struct Audio {bool set_data_event(unsigned n){return n>0 && n<=8192;} HANDLE data_event(){return 3;}}audio_;
    struct Transport {
        struct Packet{uint64_t ticks;uint32_t timestamp;unsigned frames;};
        std::vector<Packet> sent;
        bool send_media(const OA2DP_SEND_IN &,const uint8_t *p,size_t n,HANDLE){
            REQUIRE(n>=13 && n<=895);
            uint32_t stamp=uint32_t(p[4])<<24|uint32_t(p[5])<<16|uint32_t(p[6])<<8|p[7];
            const unsigned frames=tested_codec==OA2DP_CODEC_AAC?((p[1]&0x80)?1u:0u):(p[12]&15u);
            if(tested_codec!=OA2DP_CODEC_AAC)REQUIRE(frames);
            sent.push_back({::ticks,stamp,frames});return true;
        }
        void cancel_media(){}
        bool drain_media(){return true;}
        bool tx_state(OA2DP_TX_STATE &){return true;}
    }tr_;
    void post(const Command &){++failures;streaming_=false;}
    void stream_loop(const StreamContext &);
};
#define oa2dp_encoder_encode checked_encode
#include "stream-loop-under-test.inc"
#undef oa2dp_encoder_encode
} // namespace oa2dp

static oa2dp::Session *active;
static oa2dp_pcm_ring *source;
static uint64_t clip_start,second_at,stop_at;
static unsigned clip=0,produced=0;
static constexpr unsigned lengths[]={129,4097};
static std::vector<int32_t> expected;
static uint32_t tested_rate;
static bool cancel_from_wait=false;

static void advance(uint64_t now) {
    REQUIRE(now>=ticks);ticks=now;
    if(ticks>=stop_at){active->streaming_=false;return;}
    if(clip==0 && produced==lengths[0] && ticks>=second_at){
        ++clip;produced=0;clip_start=ticks;
        REQUIRE(oa2dp_ring_begin_format(source,tested_rate,tested_format.Channels,
            oa2dp_pcm_format_ring_type(&tested_format))==1);
        active->approved_pcm_sequence_=source->format_sequence;
        source->flags|=OA2DP_RING_FLAG_RUNNING;
    }
    if(!oa2dp_ring_consumer_ready(source))return;
    unsigned available=static_cast<unsigned>(std::min<uint64_t>(lengths[clip],(ticks-clip_start)*tested_rate/10000000));
    if(available>produced){
        const unsigned channels=tested_format.Channels;
        std::vector<int32_t> pcm((available-produced)*channels);
        for(unsigned i=produced;i<available;++i){
            int32_t unit=static_cast<int32_t>((clip+1)*8000+i+1);
            if(i&1)unit=-unit;
            int32_t v=0;
            if(tested_format.Encoding==OA2DP_PCM_FLOAT){
                float f=static_cast<float>(unit)/32768;std::memcpy(&v,&f,sizeof(v));
            }else v=unit*(tested_format.BitsPerSample==32?65536:256);
            for(unsigned channel=0;channel<channels;++channel)pcm[(i-produced)*channels+channel]=v;
            expected.push_back(unit);
        }
        REQUIRE(oa2dp_ring_write(source,pcm.data(),available-produced)==available-produced);
        produced=available;
    }
    if(produced==lengths[clip])source->flags|=OA2DP_RING_FLAG_EOS;
}
static DWORD WaitForMultipleObjects(unsigned n,HANDLE *h,bool all,DWORD timeout){
    REQUIRE(++waits<10000 && n==3 && h[0]==1 && h[1]==2 && !all && timeout==INFINITE);
    if(cancel_from_wait){active->streaming_=false;return WAIT_OBJECT_0;}
    if(h[2]==4)advance(timer_due);
    else {REQUIRE(h[2]==3);advance(ticks+10000);}
    return active->streaming_?WAIT_OBJECT_0+2:WAIT_OBJECT_0;
}
static void run(uint32_t rate,oa2dp_codec_id codec=OA2DP_CODEC_LDAC,
    OA2DP_FORMAT format={0,2,32,32,OA2DP_PCM_INTEGER},bool cancel=false,bool timer_failure=false) {
    ticks=10000;timers=failures=waits=clip=produced=0;admitted.clear();expected.clear();
    cancel_from_wait=cancel;fail_timer=timer_failure;tested_rate=rate;
    tested_codec=codec;tested_format=format;tested_format.SampleRate=rate;
    clip_start=ticks;second_at=ticks+1000000;stop_at=ticks+3000000;
    std::vector<uint64_t> memory((oa2dp_ring_bytes(32768,2)+7)/8);
    source=reinterpret_cast<oa2dp_pcm_ring *>(memory.data());
    REQUIRE(oa2dp_ring_init(source,32768,rate,2,oa2dp_pcm_format_ring_type(&tested_format))==0);
    REQUIRE(oa2dp_ring_begin_format(source,rate,tested_format.Channels,oa2dp_pcm_format_ring_type(&tested_format))==1);
    source->flags|=OA2DP_RING_FLAG_RUNNING;
    oa2dp::Session s;active=&s;
    oa2dp::Session::StreamContext stream{};stream.ring=source;stream.pcm=tested_format;
    oa2dp_settings_default(&stream.settings);
    stream.settings.sample_rate=rate;stream.settings.bits=0;stream.settings.ldac_quality=OA2DP_LDAC_HQ;
    oa2dp_codec_caps remote{};oa2dp_codec_local_caps(codec,&remote);
    REQUIRE(oa2dp_codec_select(nullptr,&remote,&stream.settings,&stream.config)==0);
    REQUIRE(oa2dp_ring_snapshot(source,&stream.ring_format)==0);
    s.approved_pcm_sequence_=stream.ring_format.sequence;
    s.stream_loop(stream);
    REQUIRE(timers==0 && !oa2dp_ring_consumer_ready(source));
    if(timer_failure){REQUIRE(failures==1 && s.tr_.sent.empty());return;}
    REQUIRE(failures==0 && oa2dp_ring_overruns(source)==0);
    if(cancel){REQUIRE(admitted.empty() && s.tr_.sent.empty());return;}
    if(admitted!=expected) {
        size_t first=0;while(first<admitted.size() && first<expected.size() && admitted[first]==expected[first])++first;
        std::fprintf(stderr,"rate=%u clip=%u produced=%u admitted=%zu expected=%zu first_diff=%zu actual=%d wanted=%d\n",
            rate,clip,produced,admitted.size(),expected.size(),first,
            first<admitted.size()?admitted[first]:0,first<expected.size()?expected[first]:0);
    }
    REQUIRE(clip==1 && produced==lengths[1] && admitted==expected);
    REQUIRE(admitted.size()==lengths[0]+lengths[1] && !s.tr_.sent.empty());
    const unsigned spf=codec==OA2DP_CODEC_AAC?1024:codec==OA2DP_CODEC_LDAC && rate>48000?256:128;
    uint32_t timestamp=0;
    for(size_t i=0;i<s.tr_.sent.size();++i){
        const auto &p=s.tr_.sent[i];
        REQUIRE(p.timestamp==timestamp);timestamp+=p.frames*spf;
        // A late source wake can shorten the next interval by that lateness;
        // absolute media time must still bound the whole queued burst.
        REQUIRE(p.ticks-s.tr_.sent[0].ticks+1>=static_cast<uint64_t>(p.timestamp)*10000000/rate);
    }
    REQUIRE(timestamp>admitted.size()); // encoder lookahead/tail also emitted through paced silence
    std::printf("Production loop %s %u Hz %uch %u/%u-bit %s: %zu source frames retained, %zu paced packets, 2 PCM epochs / 1 encoder.\n",
        oa2dp_codec_name(codec),rate,format.Channels,format.ValidBitsPerSample,format.BitsPerSample,
        format.Encoding==OA2DP_PCM_FLOAT?"float":"integer",admitted.size(),s.tr_.sent.size());
}
int main(){
    for(oa2dp_codec_id codec:{OA2DP_CODEC_LDAC,OA2DP_CODEC_SBC,OA2DP_CODEC_AAC})
    for(uint32_t rate:{44100u,48000u,88200u,96000u}) {
        if(codec!=OA2DP_CODEC_LDAC && rate>48000)continue;
        for(uint16_t channels:{uint16_t(1),uint16_t(2)})
        for(OA2DP_FORMAT f: {OA2DP_FORMAT{0,channels,16,16,OA2DP_PCM_INTEGER},
            OA2DP_FORMAT{0,channels,24,24,OA2DP_PCM_INTEGER},OA2DP_FORMAT{0,channels,32,24,OA2DP_PCM_INTEGER},
            OA2DP_FORMAT{0,channels,32,32,OA2DP_PCM_INTEGER},OA2DP_FORMAT{0,channels,32,32,OA2DP_PCM_FLOAT}})
            run(rate,codec,f);
    }
    const OA2DP_FORMAT f{0,2,32,32,OA2DP_PCM_INTEGER};
    run(96000,OA2DP_CODEC_LDAC,f,true);run(96000,OA2DP_CODEC_LDAC,f,false,true);
}
