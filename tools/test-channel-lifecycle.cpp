/* SPDX-License-Identifier: MIT */
#include "../driver/common/channel_lifecycle.h"
#include "../driver/common/mpm_lifecycle.h"
#include "../driver/common/server_lifecycle.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static OA2DP_CHANNEL_LIFECYCLE opened(uint32_t previous, uint32_t connection)
{
    OA2DP_CHANNEL_LIFECYCLE c{};
    c.Generation = previous;
    auto generation = Oa2dpChannelBeginOpen(&c, 1, 0);
    c.ConnectionGeneration = connection ? connection : generation;
    REQUIRE(Oa2dpChannelFinishOpen(&c, generation, 1));
    return c;
}

static void data_and_publication_boundaries()
{
    auto signal = opened(7, 0);
    auto media = opened(2, signal.Generation);
    const auto epoch = signal.Generation;
    const auto channel = media.Generation;
    REQUIRE(Oa2dpConnectionReady(&signal, &media, epoch, 1, 0));
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, epoch - 1, 1, 0)); // old TRUE publisher
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, 0, 1, 0));
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, epoch, 0, 0));
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, epoch, 1, 1));
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, 0, epoch));
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, channel, 0));
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, channel, epoch - 1));
    REQUIRE(Oa2dpChannelBeginSubmit(&media, channel, epoch));
    REQUIRE(Oa2dpChannelBeginSubmit(&media, channel, epoch));
    // CLOSE arrives after admission but before either WdfRequestSend returns.
    Oa2dpChannelRequestClose(&media);
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, channel, epoch));
    REQUIRE(!Oa2dpChannelFinishClose(&media, channel, 1));
    REQUIRE(!Oa2dpChannelEndSubmit(&media));
    REQUIRE(!Oa2dpChannelFinishClose(&media, channel, 1));
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, epoch, 1, 0)); // TRUE invalidated during publish
    REQUIRE(!Oa2dpConnectionClosed(&signal, &media)); // FALSE cannot acknowledge yet
    REQUIRE(Oa2dpChannelEndSubmit(&media)); // submission, not idle RECV completion
    REQUIRE(Oa2dpChannelFinishClose(&media, channel, 1));
    Oa2dpChannelRequestClose(&signal);
    REQUIRE(Oa2dpChannelFinishClose(&signal, epoch, 1));
    REQUIRE(Oa2dpConnectionClosed(&signal, &media));
    auto replacement = Oa2dpChannelBeginOpen(&signal, 1, 0);
    signal.ConnectionGeneration = replacement;
    REQUIRE(!Oa2dpConnectionClosed(&signal, &media)); // delayed FALSE must not set the event
    REQUIRE(Oa2dpChannelFinishOpen(&signal, replacement, 1));
    auto next_media = Oa2dpChannelBeginOpen(&media, 1, 0);
    media.ConnectionGeneration = replacement;
    REQUIRE(Oa2dpChannelFinishOpen(&media, next_media, 1));
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, channel, epoch)); // retired encoder/read
    REQUIRE(!Oa2dpChannelBeginSubmit(&media, next_media, epoch)); // matching channel, wrong link
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, epoch, 1, 0));
    REQUIRE(Oa2dpConnectionReady(&signal, &media, replacement, 1, 0));
    REQUIRE(Oa2dpChannelBeginSubmit(&media, next_media, replacement));
    REQUIRE(Oa2dpChannelEndSubmit(&media));
    media.ConnectionGeneration = epoch;
    REQUIRE(!Oa2dpConnectionReady(&signal, &media, replacement, 1, 0)); // mismatched pair
}

struct FakeServer {
    int32_t register_server = 0, register_psm = 0, unregister_server = 0, unregister_psm = 0;
    std::vector<int> calls;
};
static const OA2DP_SERVER_OPERATIONS fake_server_operations = {
    [](void *context, uintptr_t *handle) -> int32_t {
        auto &f = *static_cast<FakeServer *>(context);
        f.calls.push_back(1);
        if (f.register_server >= 0) *handle = 0x42;
        return f.register_server;
    },
    [](void *context) -> int32_t {
        auto &f = *static_cast<FakeServer *>(context);
        f.calls.push_back(2);
        return f.register_psm;
    },
    [](void *context, uintptr_t handle) -> int32_t {
        auto &f = *static_cast<FakeServer *>(context);
        REQUIRE(handle == 0x42); // exactly the returned owner, never a guessed handle
        f.calls.push_back(3);
        return f.unregister_server;
    },
    [](void *context) -> int32_t {
        auto &f = *static_cast<FakeServer *>(context);
        f.calls.push_back(4);
        return f.unregister_psm;
    }
};

static void server_ownership_failures()
{
    OA2DP_SERVER_REGISTRATION r{};
    FakeServer f;
    f.register_psm = -1; // failure after server acquired (allocation or lower BRB failure)
    REQUIRE(Oa2dpServerRegister(&r, &fake_server_operations, &f) == -1);
    REQUIRE(r.Handle == 0x42 && !r.PsmRegistered);
    REQUIRE(f.calls == std::vector<int>({1, 2}));
    f.register_psm = 0;
    REQUIRE(Oa2dpServerRegister(&r, &fake_server_operations, &f) == 0);
    REQUIRE(f.calls == std::vector<int>({1, 2, 2})); // retry does not duplicate server
    REQUIRE(r.PsmRegistered);
    REQUIRE(Oa2dpServerRegister(&r, &fake_server_operations, &f) == 0);
    REQUIRE(f.calls.size() == 3);
    f.unregister_server = -2;
    REQUIRE(Oa2dpServerUnregister(&r, &fake_server_operations, &f) == -2);
    REQUIRE(r.Handle == 0x42 && r.PsmRegistered);
    REQUIRE(f.calls.back() == 3 && f.calls.size() == 4); // no PSM drop after failed server close
    f.unregister_server = 0;
    f.unregister_psm = -3;
    REQUIRE(Oa2dpServerUnregister(&r, &fake_server_operations, &f) == -3);
    REQUIRE(!r.Handle && r.PsmRegistered);
    REQUIRE(f.calls == std::vector<int>({1, 2, 2, 3, 3, 4}));
    f.unregister_psm = 0;
    REQUIRE(Oa2dpServerUnregister(&r, &fake_server_operations, &f) == 0);
    REQUIRE(!r.Handle && !r.PsmRegistered);
    REQUIRE(f.calls == std::vector<int>({1, 2, 2, 3, 3, 4, 4})); // retry only the remaining resource
    REQUIRE(Oa2dpServerUnregister(&r, &fake_server_operations, &f) == 0);
    REQUIRE(f.calls.size() == 7);

    f = FakeServer{};
    f.register_server = -4;
    REQUIRE(Oa2dpServerRegister(&r, &fake_server_operations, &f) == -4);
    REQUIRE(!r.Handle && !r.PsmRegistered && f.calls == std::vector<int>({1}));
    f = FakeServer{};
    f.register_psm = -5;
    REQUIRE(Oa2dpServerRegister(&r, &fake_server_operations, &f) == -5);
    REQUIRE(Oa2dpServerUnregister(&r, &fake_server_operations, &f) == 0);
    REQUIRE(!r.Handle && !r.PsmRegistered);
    REQUIRE(f.calls == std::vector<int>({1, 2, 3})); // never unregister an unacquired PSM
}

int main()
{
    server_ownership_failures();
    OA2DP_MPM_REQUEST request{};
    REQUIRE(!Oa2dpMpmCompletion(&request, 1)); // unsolicited peer connection
    auto connect = Oa2dpMpmBeginRequest(&request, 1);
    REQUIRE(!Oa2dpMpmCompletion(&request, 0)); // baseline FALSE is not connect failure
    REQUIRE(Oa2dpMpmCompletion(&request, 1) == connect);
    REQUIRE(!Oa2dpMpmFailConnect(&request, connect + 1));
    REQUIRE(Oa2dpMpmFailConnect(&request, connect));
    REQUIRE(Oa2dpMpmCompletion(&request, 0) == connect); // actual failed requested OPEN
    auto disconnect = Oa2dpMpmBeginRequest(&request, 0);
    REQUIRE(!Oa2dpMpmFailConnect(&request, connect)); // late OPEN failure
    REQUIRE(Oa2dpMpmCompletion(&request, 0) == disconnect);
    connect = Oa2dpMpmBeginRequest(&request, 1); // reentrant policy during external call
    REQUIRE(!Oa2dpMpmFinishRequest(&request, disconnect));
    REQUIRE(request.Pending && Oa2dpMpmCompletion(&request, 1) == connect);
    REQUIRE(Oa2dpMpmFinishRequest(&request, connect));
    REQUIRE(!Oa2dpMpmCompletion(&request, 0)); // later remote disconnect is unsolicited
    REQUIRE(!Oa2dpMpmCompletion(&request, 1)); // duplicate status is not another completion
    auto suspend = Oa2dpMpmBeginRequest(&request, 1);
    REQUIRE(!Oa2dpMpmOwnedReply(&request, 0)); // ordinary music pause has no request completion
    REQUIRE(Oa2dpMpmOwnedReply(&request, suspend) == suspend);
    auto resume = Oa2dpMpmBeginRequest(&request, 0);
    REQUIRE(!Oa2dpMpmOwnedReply(&request, suspend)); // old stop cannot acknowledge the new resume
    REQUIRE(!Oa2dpMpmFinishRequest(&request, suspend));
    REQUIRE(Oa2dpMpmOwnedReply(&request, resume) == resume);
    // Caller retires only after the external update succeeds. On failure the
    // same token remains pending; a normal idle/resume reply can retry it.
    REQUIRE(request.Pending && Oa2dpMpmOwnedReply(&request, resume) == resume);
    REQUIRE(Oa2dpMpmFinishRequest(&request, resume));
    REQUIRE(!Oa2dpMpmOwnedReply(&request, resume));
    request.Serial = UINT32_MAX;
    REQUIRE(Oa2dpMpmBeginRequest(&request, 0) == 1);
    data_and_publication_boundaries();
    OA2DP_CHANNEL_LIFECYCLE c{};
    REQUIRE(!Oa2dpChannelBeginOpen(&c, 0, 0)); // explicit user disconnect
    REQUIRE(!Oa2dpChannelBeginOpen(&c, 1, 1)); // PnP stop
    auto first = Oa2dpChannelBeginOpen(&c, 1, 0);
    REQUIRE(first != 0);
    c.ConnectionGeneration = 1;
    Oa2dpChannelRequestClose(&c);             // disconnect while BRB is pending
    REQUIRE(c.State == ChannelOpening && c.CloseRequested);
    REQUIRE(Oa2dpChannelFinishOpen(&c, first, 1)); // late success still needs close
    REQUIRE(c.CloseRequested);
    REQUIRE(!Oa2dpChannelBeginOpen(&c, 1, 0));
    Oa2dpChannelRequestClose(&c);
    REQUIRE(!Oa2dpChannelFinishClose(&c, first, 0)); // failed close keeps ownership
    REQUIRE(c.State == ChannelClosing && c.CloseRequested);
    REQUIRE(!Oa2dpChannelBeginOpen(&c, 1, 0));
    REQUIRE(Oa2dpChannelFinishClose(&c, first, 1));
    REQUIRE(!Oa2dpChannelEventReady(&c, first, 1)); // late OPEN notification after close
    REQUIRE(Oa2dpChannelEventReady(&c, first, 0));
    auto second = Oa2dpChannelBeginOpen(&c, 1, 0);
    c.ConnectionGeneration = 2;
    REQUIRE(second != first);
    REQUIRE(!Oa2dpChannelFinishOpen(&c, first, 1)); // obsolete completion cannot mutate new open
    REQUIRE(c.State == ChannelOpening);
    REQUIRE(Oa2dpChannelFinishOpen(&c, second, 1));
    REQUIRE(!Oa2dpChannelFinishClose(&c, first, 1)); // delayed worker cannot close replacement
    REQUIRE(c.State == ChannelOpen);
    REQUIRE(!Oa2dpChannelCloseTargets(&c, 0, 1)); // teardown of old link owns neither replacement channel
    REQUIRE(Oa2dpChannelCloseTargets(&c, second, 2));
    auto old_media = c;
    old_media.ConnectionGeneration = 1;
    REQUIRE(Oa2dpChannelCloseTargets(&old_media, 0, 1)); // old media still needs cleanup beside new signalling
    REQUIRE(!Oa2dpChannelGenerationAfter(1, 1)); // queued OPEN from an unknown-but-closed link is barred
    REQUIRE(Oa2dpChannelGenerationAfter(2, 1)); // new link remains admissible
    REQUIRE(!Oa2dpChannelEventCurrent(second, first));
    REQUIRE(!Oa2dpChannelEventReady(&c, first, 0)); // stale close notification
    REQUIRE(Oa2dpChannelEventReady(&c, second, 1));
    REQUIRE(Oa2dpChannelEventCurrent(second, second));
    REQUIRE(!Oa2dpChannelGenerationAfter(first, second));
    Oa2dpChannelRequestClose(&c);
    REQUIRE(Oa2dpChannelFinishClose(&c, second, 1));
    auto third = Oa2dpChannelBeginOpen(&c, 1, 0);
    Oa2dpChannelRequestClose(&c);
    REQUIRE(Oa2dpChannelFinishOpen(&c, third, 0)); // canceled/timed-out open releases latch
    REQUIRE(c.State == ChannelClosed && !c.CloseRequested);
    c.Generation = UINT32_MAX;
    REQUIRE(Oa2dpChannelBeginOpen(&c, 1, 0) == 1); // zero reserved; wrap remains ordered
    REQUIRE(Oa2dpChannelGenerationAfter(1, UINT32_MAX));
    std::puts("channel lifecycle ordering checks passed");
}
