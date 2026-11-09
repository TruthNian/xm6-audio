/* SPDX-License-Identifier: MIT */
#include "../driver/common/channel_lifecycle.h"
#include <cstdio>
#include <cstdlib>
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

int main()
{
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
