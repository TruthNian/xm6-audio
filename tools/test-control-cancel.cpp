/* SPDX-License-Identifier: MIT */
/* Production Session -> Transport -> IOCTL chain with controlled Windows I/O.
 * Stop-before-submit and stop-during-submit are explicit orderings. No radio
 * or kernel cancellation timing is inferred from this user-mode fixture. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define LOGI(...) ((void)0)
#define LOGW(...) ((void)0)
using DWORD=uint32_t;
using ULONG=uint32_t;
using BOOL=bool;
constexpr bool TRUE=true, FALSE=false;
constexpr DWORD ERROR_SUCCESS=0, ERROR_IO_PENDING=997, ERROR_IO_INCOMPLETE=996,
    ERROR_OPERATION_ABORTED=995, ERROR_CANCELLED=1223, WAIT_TIMEOUT=258,
    WAIT_OBJECT_0=0, INFINITE=0xffffffff;
constexpr DWORD IOCTL_OA2DP_OPEN_CHANNEL=1, IOCTL_OA2DP_SEND=2;
enum OA2DP_CHANNEL_KIND { Oa2dpChannelSignaling, Oa2dpChannelMedia };
struct OA2DP_OPEN_CHANNEL_IN { ULONG Kind=0; ULONG ConnectionGeneration=0; };
struct OA2DP_CHANNEL_INFO { ULONG Kind=0, Generation=0, ConnectionGeneration=0; uint16_t OutMtu=0; };
struct OA2DP_SEND_IN { ULONG Kind, Generation, ConnectionGeneration; };
struct Event { bool signaled=false, allocated=false; };
using HANDLE=Event*;
struct OVERLAPPED { HANDLE hEvent=nullptr; };
static Event stop_event, device;
static DWORD last_error, elapsed, completed_bytes;
static unsigned submits, cancels, allocations, closes, blocking_drains;
static bool stop_during_submit, complete_on_wait, immediate_success;
static bool completion_done, cancellation_requested;
static OVERLAPPED *pending;
static DWORD last_open_kind, last_connection;
static DWORD GetLastError() { return last_error; }
static HANDLE CreateEventW(void*, bool manual, bool signaled, void*) {
    REQUIRE(manual);
    ++allocations;
    return new Event{signaled,true};
}
static void CloseHandle(HANDLE event) {
    REQUIRE(event && event->allocated && (!pending || completion_done));
    ++closes;
    delete event;
}
static DWORD WaitForSingleObject(HANDLE event, DWORD timeout) {
    REQUIRE(event && timeout==0);
    return event->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static BOOL DeviceIoControl(HANDLE h,DWORD code,void *in,DWORD,void *out,DWORD outlen,void*,OVERLAPPED *ov) {
    REQUIRE(h==&device && (!pending || completion_done));
    ++submits;
    pending=ov;
    completion_done=immediate_success;
    cancellation_requested=false;
    completed_bytes=0;
    if (code==IOCTL_OA2DP_OPEN_CHANNEL) {
        REQUIRE(outlen==sizeof(OA2DP_CHANNEL_INFO));
        const auto &input=*static_cast<OA2DP_OPEN_CHANNEL_IN*>(in);
        last_open_kind=input.Kind; last_connection=input.ConnectionGeneration;
        *static_cast<OA2DP_CHANNEL_INFO*>(out)={input.Kind,9,input.ConnectionGeneration ? input.ConnectionGeneration : 9,679};
        completed_bytes=sizeof(OA2DP_CHANNEL_INFO);
    } else REQUIRE(code==IOCTL_OA2DP_SEND);
    if (stop_during_submit) stop_event.signaled=true;
    ov->hEvent->signaled=completion_done;
    last_error=completion_done ? ERROR_SUCCESS : ERROR_IO_PENDING;
    return completion_done;
}
static BOOL CancelIoEx(HANDLE h,OVERLAPPED *ov) {
    REQUIRE(h==&device);
    ++cancels;
    if (pending && (!ov || ov==pending) && !completion_done)
        cancellation_requested=true; // cancellation is not completion
    return true;
}
static DWORD WaitForMultipleObjects(DWORD count,HANDLE *events,BOOL all,DWORD timeout) {
    REQUIRE(!all);
    for (DWORD i=0;i<count;++i) if(events[i]->signaled) return WAIT_OBJECT_0+i;
    if (complete_on_wait) {
        REQUIRE(pending);
        completion_done=true;pending->hEvent->signaled=true;
        for (DWORD i=0;i<count;++i) if(events[i]->signaled) return WAIT_OBJECT_0+i;
    }
    REQUIRE(timeout!=INFINITE);
    elapsed+=timeout;
    return WAIT_TIMEOUT;
}
static BOOL GetOverlappedResult(HANDLE h,OVERLAPPED *ov,DWORD *bytes,BOOL wait) {
    REQUIRE(h==&device && ov==pending);
    if (wait) {
        ++blocking_drains;
        REQUIRE(cancellation_requested || completion_done);
        completion_done=true;ov->hEvent->signaled=true;
    }
    if (!completion_done) { last_error=ERROR_IO_INCOMPLETE;return false; }
    *bytes=cancellation_requested ? 0 : completed_bytes;
    last_error=cancellation_requested ? ERROR_OPERATION_ABORTED : ERROR_SUCCESS;
    return !cancellation_requested;
}
struct DriverHandle {
    HANDLE h_=&device;
    DWORD ioctl(DWORD,const void*,DWORD,void*,DWORD,DWORD*,DWORD=INFINITE,HANDLE=nullptr);
};
struct Transport {
    DriverHandle dev_;
    // Both declarations let the same fixture compile the old and new exact
    // production bodies. Only Session's actual call determines propagation.
    bool open_channel(OA2DP_CHANNEL_KIND,OA2DP_CHANNEL_INFO&,ULONG=0);
    bool open_channel(OA2DP_CHANNEL_KIND,OA2DP_CHANNEL_INFO&,ULONG,HANDLE);
    bool send(const OA2DP_SEND_IN&,const uint8_t*,size_t);
    bool send(const OA2DP_SEND_IN&,const uint8_t*,size_t,HANDLE);
};
struct Session {
    Transport tr_;
    HANDLE stop_event_=&stop_event;
    ULONG sig_generation_=7,media_generation_=0;
    uint16_t sig_mtu_=0,media_mtu_=0;
    struct { uint16_t out_mtu=0; } av_;
    bool sig_open_=false,media_open_=false,rx_failed_=false;
    unsigned teardowns=0;
    void teardown(const char*) { ++teardowns; }
    bool connect_signaling();
    bool open_media_channel();
    static int cb_send(void*,const uint8_t*,size_t);
};
#include "control-ioctl-under-test.inc"
#include "control-open-under-test.inc"
#include "control-send-under-test.inc"
#include "control-connect-under-test.inc"
#include "control-protocol-send-under-test.inc"

static void reset() {
    REQUIRE(!pending || completion_done);
    REQUIRE(allocations==closes);
    pending=nullptr;stop_event={};
    last_error=elapsed=completed_bytes=0;
    submits=cancels=allocations=closes=blocking_drains=0;
    stop_during_submit=complete_on_wait=immediate_success=false;
    completion_done=cancellation_requested=false;
}
static void invoke_stopped(unsigned operation,bool during) {
    reset();Session session;
    stop_event.signaled=!during;
    // Session::stop's one-shot cancellation can precede the next submission.
    CancelIoEx(&device,nullptr);
    stop_during_submit=during;
    const uint8_t pdu[]={0x00,0x01};
    if(operation==0) REQUIRE(!session.connect_signaling());
    if(operation==1) REQUIRE(!session.open_media_channel());
    if(operation==2) REQUIRE(Session::cb_send(&session,pdu,sizeof(pdu))==-1);
    if (elapsed) std::fprintf(stderr,"operation %u, stop %s submission: waited %u simulated ms\n",
        operation,during ? "during" : "before",elapsed);
    REQUIRE(elapsed==0);
    if(!during) REQUIRE(submits==0 && allocations==0 && cancels==1);
    else REQUIRE(submits==1 && cancels==2 && blocking_drains==1 && completion_done);
    REQUIRE(allocations==closes && !session.sig_open_ && !session.media_open_);
}
int main() {
    for(unsigned operation=0;operation<3;++operation) {
        invoke_stopped(operation,false);
        invoke_stopped(operation,true);
    }
    reset();Session session;
    complete_on_wait=true;
    REQUIRE(session.connect_signaling() && session.sig_generation_==9 && last_open_kind==0);
    REQUIRE(session.open_media_channel() && session.media_generation_==9 && last_connection==9);
    const uint8_t pdu[]={0x00,0x01};
    REQUIRE(Session::cb_send(&session,pdu,sizeof(pdu))==0);
    REQUIRE(submits==3 && !cancels && !elapsed && allocations==closes);

    // A completed request can win a simultaneous stop; ownership is still
    // terminal, and Session's normal shutdown can close the acquired channel.
    reset();session={};immediate_success=stop_during_submit=true;
    REQUIRE(session.connect_signaling() && completion_done && !blocking_drains);
    REQUIRE(submits==1 && allocations==closes);

    // Preserve timeout/cancel cleanup for ordinary uncancelled operations.
    reset();DriverHandle h;DWORD bytes=0;
    REQUIRE(h.ioctl(IOCTL_OA2DP_SEND,nullptr,0,nullptr,0,&bytes,50)==WAIT_TIMEOUT);
    REQUIRE(elapsed==50 && cancels==1 && blocking_drains==1 && completion_done);
    reset();
    std::puts("production control I/O stop-before/during-submit and completion ownership checks passed (Windows APIs controlled)");
}
