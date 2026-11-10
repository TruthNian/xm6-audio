/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_MPM_LIFECYCLE_H
#define OA2DP_MPM_LIFECYCLE_H
#include <stdint.h>

/* Serialized by the transport lock. An observed link state is not itself a
 * completion of a Windows request. Keep request ownership across external calls. */
typedef struct {
    uint32_t Serial;
    uint32_t FailedConnect;
    uint32_t AttemptGeneration;
    uint32_t PolicyRevision;
    int Target;
    int Pending;
} OA2DP_MPM_REQUEST;

static inline uint32_t Oa2dpMpmAdvancePolicy(OA2DP_MPM_REQUEST *r)
{
    if (++r->PolicyRevision == 0) ++r->PolicyRevision;
    return r->PolicyRevision;
}

static inline uint32_t Oa2dpMpmBeginRequest(OA2DP_MPM_REQUEST *r, int connected)
{
    Oa2dpMpmAdvancePolicy(r);
    if (++r->Serial == 0) ++r->Serial;
    r->Target = connected != 0;
    r->Pending = 1;
    r->FailedConnect = 0;
    r->AttemptGeneration = 0;
    return r->Serial;
}

/* An explicit KS request is not a manager callback. If the manager elides an
 * already-at-target callback, apply local intent without inventing a pending
 * manager operation. A callback or newer caller supersedes this fallback. */
static inline int Oa2dpMpmFallbackPolicy(OA2DP_MPM_REQUEST *r, uint32_t revision, int target)
{
    if (!revision || revision != r->PolicyRevision) return 0;
    if (r->Pending && r->Target != (target != 0)) {
        r->Pending = 0;
        r->FailedConnect = 0;
        r->AttemptGeneration = 0;
    }
    return 1;
}

static inline void Oa2dpMpmBindAttempt(OA2DP_MPM_REQUEST *r, uint32_t generation)
{
    if (generation && r->Pending && r->Target) {
        r->AttemptGeneration = generation;
        r->FailedConnect = 0;
    }
}

static inline int Oa2dpMpmFailConnect(OA2DP_MPM_REQUEST *r, uint32_t generation)
{
    if (!generation || generation != r->AttemptGeneration || !r->Pending || !r->Target) return 0;
    r->FailedConnect = r->Serial;
    return 1;
}

/* Zero means unsolicited status, not a requested-operation completion. */
static inline uint32_t Oa2dpMpmCompletion(const OA2DP_MPM_REQUEST *r, int connected)
{
    return r->Pending && (r->Target == (connected != 0) ||
        (!connected && r->FailedConnect == r->Serial)) ? r->Serial : 0;
}

/* Withdrawing stale readiness while channels drain is only an indication.
 * It must not complete Windows' outstanding Disconnect operation. */
static inline uint32_t Oa2dpMpmConnectionCompletion(const OA2DP_MPM_REQUEST *r,
                                                  int connected, int settled)
{
    return settled ? Oa2dpMpmCompletion(r, connected) : 0;
}

static inline int Oa2dpMpmFinishRequest(OA2DP_MPM_REQUEST *r, uint32_t serial)
{
    if (!serial || serial != r->Serial || !r->Pending) return 0;
    r->Pending = 0;
    r->FailedConnect = 0;
    r->AttemptGeneration = 0;
    return 1;
}
/* A service reply explicitly identifies the request it handled. A mismatched
 * result may report failure of that request, but cannot complete a newer one. */
static inline uint32_t Oa2dpMpmOwnedReply(const OA2DP_MPM_REQUEST *r, uint32_t serial)
{
    return serial && r->Pending && serial == r->Serial ? serial : 0;
}
#endif
