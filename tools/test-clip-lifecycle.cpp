// SPDX-License-Identifier: MIT
// Compile the actual AudioFormat handler with fake policy/OS edges.
#include "oa2dp/pcm_format.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define LOGI(...) ((void)0)
using ULONG=uint32_t;
static unsigned clock_ms=1,wakes=0;
static unsigned GetTickCount(){return clock_ms;}
static void SetEvent(void *){++wakes;}
struct Session {
    enum class Cmd { AudioFormat, Other };
    struct Command {Cmd cmd=Cmd::AudioFormat;struct {
        oa2dp_pcm_format Format{96000,2,32,32,OA2DP_PCM_FLOAT};
        unsigned ConnectionEnabled=1,SuspendRequested=0,SuspendSerial=0,ConnectionSerial=0;
        unsigned PcmSequence=2,Running=1;
    }format;};
    bool connection_enabled_=true,suspend_requested_=false,publication_pending_=false;
    bool teardown_pending_=false,transport_suspended_=false,audio_running_=true,audio_ready_=true;
    bool streaming_=true,sig_open_=true;
    unsigned suspend_serial_=0,connection_serial_=0,next_reconnect_=0,pcm_sequence_=2,idle_since_=0;
    oa2dp_pcm_format audio_format_{96000,2,32,32,OA2DP_PCM_FLOAT};
    std::atomic<ULONG> approved_pcm_sequence_{2};
    struct {bool follow_source_rate=true;}settings_;
    struct {void *data_event(){return this;}}audio_;
    unsigned suspends=0,starts=0,teardowns=0,rates=0,publications=0;
    void suspend_stream(){++suspends;streaming_=false;}
    void start_stream(){++starts;streaming_=true;}
    void teardown(const char *){++teardowns;streaming_=false;}
    void publish(const char *){++publications;}
    void update_suspend_state(){++publications;}
    void reconfigure_rate(unsigned){++rates;}
    void handle(const Command &c){switch(c.cmd){
#include "clip-lifecycle-under-test.inc"
    default:break;
    }}
};
int main(){
    Session s;Session::Command c;
    c.format.Running=0;s.handle(c);
    REQUIRE(s.suspends==0 && !s.audio_running_ && s.streaming_);
    c.format.PcmSequence=4;c.format.Running=1;s.handle(c);
    REQUIRE(s.suspends==0 && s.starts==0 && s.approved_pcm_sequence_==4 && wakes>=2);
    c.format.Format.ValidBitsPerSample=24;c.format.Format.Encoding=OA2DP_PCM_INTEGER;
    c.format.PcmSequence=6;s.handle(c);
    REQUIRE(s.suspends==1 && s.starts==1);
    c.format.Format.ValidBitsPerSample=32;c.format.PcmSequence=8;s.handle(c);
    REQUIRE(s.suspends==2 && s.starts==2); // 24-in-32 and 32-in-32 are not equivalent
    c.format.SuspendRequested=1;s.handle(c);
    REQUIRE(s.suspends==3 && !s.streaming_ && s.starts==2);
    c.format.ConnectionEnabled=0;s.handle(c);
    REQUIRE(s.teardowns==1 && !s.streaming_);
    std::puts("Actual AudioFormat handler preserves same-format clips; format/HFP/disconnect still retire transport.");
}
