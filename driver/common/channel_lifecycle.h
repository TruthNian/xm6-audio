/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_CHANNEL_LIFECYCLE_H
#define OA2DP_CHANNEL_LIFECYCLE_H
#include <stdint.h>

/* Caller serializes these transitions. No OS handles or allocation: the same
 * transitions are exercised off-host with deliberately reordered completions. */
typedef enum {
    ChannelClosed = 0, ChannelOpening, ChannelOpen, ChannelClosing
} OA2DP_CHANNEL_STATE;
typedef struct {
    OA2DP_CHANNEL_STATE State;
    uint32_t Generation;
    uint32_t ConnectionGeneration;
    int CloseRequested;
    uint32_t Submissions;
} OA2DP_CHANNEL_LIFECYCLE;

static inline uint32_t Oa2dpChannelBeginOpen(OA2DP_CHANNEL_LIFECYCLE *c,
                                           int enabled, int stopping)
{
    if (!enabled || stopping || c->CloseRequested || c->Submissions || c->State != ChannelClosed)
        return 0;
    if (++c->Generation == 0) ++c->Generation;
    c->State = ChannelOpening;
    return c->Generation;
}

/* Returns 1 for a current completion, 0 for an obsolete completion. A close
 * requested during opening remains latched until the actual handle is closed. */
static inline int Oa2dpChannelFinishOpen(OA2DP_CHANNEL_LIFECYCLE *c,
                                        uint32_t generation, int success)
{
    if (c->Generation != generation || c->State != ChannelOpening) return 0;
    c->State = success ? ChannelOpen : ChannelClosed;
    if (!success) c->CloseRequested = 0;
    return 1;
}

static inline void Oa2dpChannelRequestClose(OA2DP_CHANNEL_LIFECYCLE *c)
{
    c->CloseRequested = c->State != ChannelClosed;
    if (c->State == ChannelOpen) c->State = ChannelClosing;
}

static inline int Oa2dpChannelFinishClose(OA2DP_CHANNEL_LIFECYCLE *c,
                                         uint32_t generation, int success)
{
    if (c->Generation != generation || c->State != ChannelClosing || c->Submissions) return 0;
    if (success) {
        c->State = ChannelClosed;
        c->CloseRequested = 0;
    }
    return success;
}

static inline int Oa2dpChannelEventCurrent(uint32_t current, uint32_t event)
{
    return event != 0 && event == current;
}
/* Mandatory data-plane binding: zero is never a wildcard for audio bytes. */
static inline int Oa2dpChannelTransferCurrent(const OA2DP_CHANNEL_LIFECYCLE *c,
                                             uint32_t channel, uint32_t connection)
{
    return c->State == ChannelOpen && !c->CloseRequested && channel && connection &&
           c->Generation == channel && c->ConnectionGeneration == connection;
}
static inline int Oa2dpChannelBeginSubmit(OA2DP_CHANNEL_LIFECYCLE *c,
                                         uint32_t channel, uint32_t connection)
{
    if (!Oa2dpChannelTransferCurrent(c, channel, connection) || c->Submissions == UINT32_MAX)
        return 0;
    ++c->Submissions;
    return 1;
}
static inline int Oa2dpChannelEndSubmit(OA2DP_CHANNEL_LIFECYCLE *c)
{
    if (c->Submissions) --c->Submissions;
    return c->Submissions == 0;
}
static inline int Oa2dpConnectionReady(const OA2DP_CHANNEL_LIFECYCLE *signal,
                                      const OA2DP_CHANNEL_LIFECYCLE *media,
                                      uint32_t connection, int enabled, int stopping)
{
    return enabled && !stopping &&
           Oa2dpChannelTransferCurrent(signal, connection, connection) &&
           Oa2dpChannelTransferCurrent(media, media->Generation, connection);
}
static inline int Oa2dpConnectionClosed(const OA2DP_CHANNEL_LIFECYCLE *signal,
                                       const OA2DP_CHANNEL_LIFECYCLE *media)
{
    return signal->State == ChannelClosed && media->State == ChannelClosed;
}
static inline int Oa2dpChannelCloseTargets(const OA2DP_CHANNEL_LIFECYCLE *c,
                                          uint32_t channel, uint32_t connection)
{
    return (!channel || channel == c->Generation) &&
           (!connection || connection == c->ConnectionGeneration);
}
static inline int Oa2dpChannelEventReady(const OA2DP_CHANNEL_LIFECYCLE *c,
                                        uint32_t generation, int opened)
{
    return Oa2dpChannelEventCurrent(c->Generation, generation) &&
        (opened ? c->State == ChannelOpen && !c->CloseRequested : c->State == ChannelClosed);
}
static inline int Oa2dpChannelGenerationAfter(uint32_t next, uint32_t previous)
{
    return next != 0 && (previous == 0 || (int32_t)(next - previous) > 0);
}
#endif
