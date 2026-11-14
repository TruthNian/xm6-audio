// SPDX-License-Identifier: MIT
// Production wake/completion bodies with adversarial fake KMDF completion.
// This verifies lifetime and admission, not controller wake latency.
#include "../driver/common/channel_lifecycle.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define NT_SUCCESS(x) ((x) >= 0)
#define TraceLoggingWrite(...) ((void)0)
using ULONG=uint32_t; using NTSTATUS=int32_t; using BOOLEAN=bool; using VOID=void;
using WDFIOTARGET=void*; using WDFCONTEXT=void*;
constexpr bool FALSE=false;
constexpr NTSTATUS STATUS_SUCCESS=0, STATUS_PENDING=259, STATUS_CONNECTION_DISCONNECTED=-1,
    STATUS_INVALID_PARAMETER=-2, STATUS_INSUFFICIENT_RESOURCES=-3, STATUS_CANCELLED=-4, STATUS_IO_TIMEOUT=-5;
constexpr int Oa2dpChannelSignaling=0, Oa2dpChannelMedia=1, IO_NO_INCREMENT=0,
    BRB_ACL_ENTER_ACTIVE_MODE=0x301, OA2DP_POOL_TAG=0;
struct _BRB { uint64_t address; };
using PBRB=_BRB*;
struct _BRB_ACL_ENTER_ACTIVE_MODE { uint64_t BtAddress; };
struct Lock { bool held=false; };
struct Event { bool signaled=true; };
static void WdfSpinLockAcquire(Lock* l) { REQUIRE(!l->held);l->held=true; }
static void WdfSpinLockRelease(Lock* l) { REQUIRE(l->held);l->held=false; }
static void KeClearEvent(Event* e) { e->signaled=false; }
static void KeSetEvent(Event* e,int,bool) { e->signaled=true; }
struct OA2DP_CHANNEL { OA2DP_CHANNEL_LIFECYCLE Life{}; Event SubmissionsIdle; };
struct OA2DP_REQUEST_CONTEXT { _BRB* Brb=nullptr; ULONG Generation=0,ConnectionGeneration=0; };
struct Request { OA2DP_REQUEST_CONTEXT context; bool completed=false; NTSTATUS status=STATUS_PENDING; };
using WDFREQUEST=Request*;
struct Params { struct { NTSTATUS Status; } IoStatus; };
using PWDF_REQUEST_COMPLETION_PARAMS=Params*;
using EVT_WDF_REQUEST_COMPLETION_ROUTINE=void(WDFREQUEST,WDFIOTARGET,PWDF_REQUEST_COMPLETION_PARAMS,WDFCONTEXT);
static OA2DP_REQUEST_CONTEXT* Oa2dpGetRequest(WDFREQUEST r) { REQUIRE(!r->completed);return &r->context; }
static void WdfRequestComplete(WDFREQUEST r,NTSTATUS status) {
    REQUIRE(!r->completed && !r->context.Brb);r->status=status;r->completed=true;
}
static bool allocation_fail;
static unsigned allocated,freed,submissions;
struct OA2DP_DEVICE_CONTEXT {
    struct ProfileApi {
        PBRB BthAllocateBrb(int kind,int) {
            REQUIRE(kind==BRB_ACL_ENTER_ACTIVE_MODE);
            if(allocation_fail)return nullptr;
            ++allocated;return new _BRB{};
        }
        void BthFreeBrb(PBRB b) { REQUIRE(b);++freed;delete b; }
    } Profile;
    struct Lock storage;
    struct Lock* Lock=&storage;
    OA2DP_CHANNEL Channels[2];
    bool TransportStopping=false,PowerSuspended=false,PolicyConnected=true,PolicySuspended=false;
    uint64_t RemoteAddress=17;
    OA2DP_DEVICE_CONTEXT() {
        Channels[0].Life={ChannelOpen,4,4,0,0};
        Channels[1].Life={ChannelOpen,7,4,0,0};
    }
};
struct OA2DP_SEND_IN { ULONG Kind,Generation,ConnectionGeneration; };
enum class Delivery { Deferred, Inline, Reject };
static Delivery delivery;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE* callback;
static WDFREQUEST pending;
static OA2DP_DEVICE_CONTEXT* context;
static std::function<void()> during_submit;
static void complete(NTSTATUS status) {
    REQUIRE(pending && !pending->completed);
    Params params{{status}};
    callback(pending,nullptr,&params,context);
    pending=nullptr;
}
static NTSTATUS SubmitBrbAsync(OA2DP_DEVICE_CONTEXT* dev,WDFREQUEST request,_BRB* brb,ULONG size,
    EVT_WDF_REQUEST_COMPLETION_ROUTINE* done,void* ctx,ULONG timeout) {
    REQUIRE(!dev->Lock->held && dev->Channels[1].Life.Submissions==1);
    REQUIRE(size==sizeof(_BRB_ACL_ENTER_ACTIVE_MODE) && brb->address==dev->RemoteAddress);
    REQUIRE(timeout==1500 && ctx==dev);
    ++submissions;
    if(during_submit)during_submit();
    if(delivery==Delivery::Reject)return STATUS_CANCELLED;
    callback=done;pending=request;context=dev;
    if(delivery==Delivery::Inline)complete(STATUS_SUCCESS);
    return STATUS_PENDING;
}
#include "link-wake-under-test.inc"

static void reset() {
    REQUIRE(!pending && allocated==freed);
    allocation_fail=false;allocated=freed=submissions=0;
    delivery=Delivery::Deferred;during_submit={};
}
int main() {
    const OA2DP_SEND_IN binding{Oa2dpChannelMedia,7,4};
    for(unsigned gate=0;gate<9;++gate) {
        reset();OA2DP_DEVICE_CONTEXT d;Request r;auto in=binding;
        switch(gate) {
        case 0:d.TransportStopping=true;break;
        case 1:d.PowerSuspended=true;break;
        case 2:d.PolicyConnected=false;break;
        case 3:d.PolicySuspended=true;break;
        case 4:++in.Generation;break;
        case 5:++in.ConnectionGeneration;break;
        case 6:d.Channels[0].Life.State=ChannelClosing;break;
        case 7:d.Channels[1].Life.CloseRequested=1;break;
        case 8:in.Kind=99;break;
        }
        Oa2dpWakeLink(&d,&r,&in);
        REQUIRE(r.completed && !NT_SUCCESS(r.status) && !allocated && !submissions);
        REQUIRE(!d.Channels[1].Life.Submissions && d.Channels[1].SubmissionsIdle.signaled);
    }
    for(auto mode:{Delivery::Deferred,Delivery::Inline,Delivery::Reject}) {
        reset();OA2DP_DEVICE_CONTEXT d;Request r;delivery=mode;
        Oa2dpWakeLink(&d,&r,&binding);
        REQUIRE(!d.Channels[1].Life.Submissions && d.Channels[1].SubmissionsIdle.signaled);
        if(mode==Delivery::Deferred) { REQUIRE(!r.completed);complete(STATUS_SUCCESS); }
        REQUIRE(r.completed && r.status==(mode==Delivery::Reject?STATUS_CANCELLED:STATUS_SUCCESS));
        REQUIRE(allocated==1 && freed==1);
    }
    {
        reset();OA2DP_DEVICE_CONTEXT d;Request r;allocation_fail=true;
        Oa2dpWakeLink(&d,&r,&binding);
        REQUIRE(r.completed && r.status==STATUS_INSUFFICIENT_RESOURCES && !submissions);
        REQUIRE(!d.Channels[1].Life.Submissions && d.Channels[1].SubmissionsIdle.signaled);
    }
    for(unsigned race=0;race<6;++race) {
        reset();OA2DP_DEVICE_CONTEXT d;Request r;
        Oa2dpWakeLink(&d,&r,&binding);
        NTSTATUS result=STATUS_SUCCESS;
        switch(race) {
        case 0:d.PolicySuspended=true;break;
        case 1:d.PolicyConnected=false;break;
        case 2:++d.Channels[1].Life.Generation;break;
        case 3:++d.Channels[0].Life.Generation;break;
        case 4:result=STATUS_CANCELLED;break;
        case 5:result=STATUS_IO_TIMEOUT;break;
        }
        complete(result);
        REQUIRE(r.completed && r.status==(race<4?STATUS_CONNECTION_DISCONNECTED:result));
        REQUIRE(freed==allocated && !d.Channels[1].Life.Submissions);
    }
    reset();
    std::puts("link wake admission, cancellation and immediate/deferred ownership: OK");
}
