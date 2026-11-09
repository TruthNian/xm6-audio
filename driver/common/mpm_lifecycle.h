/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_MPM_LIFECYCLE_H
#define OA2DP_MPM_LIFECYCLE_H
#include <stdint.h>

/* Serialized by the transport lock. An observed link state is not itself a
 * completion of a Windows request. Keep request ownership across external calls. */
typedef struct {
    uint32_t Serial;
    uint32_t FailedConnect;
    int Target;
    int Pending;
} OA2DP_MPM_REQUEST;

static inline uint32_t Oa2dpMpmBeginRequest(OA2DP_MPM_REQUEST *r, int connected)
{
    if (++r->Serial == 0) ++r->Serial;
    r->Target = connected != 0;
    r->Pending = 1;
    r->FailedConnect = 0;
    return r->Serial;
}

static inline int Oa2dpMpmFailConnect(OA2DP_MPM_REQUEST *r, uint32_t serial)
{
    if (!serial || serial != r->Serial || !r->Pending || !r->Target) return 0;
    r->FailedConnect = serial;
    return 1;
}

/* Zero means unsolicited status, not a requested-operation completion. */
static inline uint32_t Oa2dpMpmCompletion(const OA2DP_MPM_REQUEST *r, int connected)
{
    return r->Pending && (r->Target == (connected != 0) ||
        (!connected && r->FailedConnect == r->Serial)) ? r->Serial : 0;
}

static inline int Oa2dpMpmFinishRequest(OA2DP_MPM_REQUEST *r, uint32_t serial)
{
    if (!serial || serial != r->Serial || !r->Pending) return 0;
    r->Pending = 0;
    r->FailedConnect = 0;
    return 1;
}
#endif
