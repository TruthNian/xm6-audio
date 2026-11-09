/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_SERVER_LIFECYCLE_H
#define OA2DP_SERVER_LIFECYCLE_H
#include <stdint.h>

/* PnP caller serializes this resource transaction. The same orchestration is
 * fault-injected off-host; callbacks implement the actual Bluetooth BRBs. */
typedef struct {
    uintptr_t Handle;
    int PsmRegistered;
} OA2DP_SERVER_REGISTRATION;
typedef struct {
    int32_t (*RegisterServer)(void *context, uintptr_t *handle);
    int32_t (*RegisterPsm)(void *context);
    int32_t (*UnregisterServer)(void *context, uintptr_t handle);
    int32_t (*UnregisterPsm)(void *context);
} OA2DP_SERVER_OPERATIONS;

static inline int32_t Oa2dpServerRegister(OA2DP_SERVER_REGISTRATION *r,
    const OA2DP_SERVER_OPERATIONS *ops, void *context)
{
    int32_t status;
    if (!r->Handle) {
        uintptr_t handle = 0;
        status = ops->RegisterServer(context, &handle);
        if (status < 0) return status;
        r->Handle = handle; /* callback validates a nonzero successful handle */
    }
    if (!r->PsmRegistered) {
        status = ops->RegisterPsm(context);
        if (status < 0) return status; // retain the server for cleanup/retry
        r->PsmRegistered = 1;
    }
    return 0;
}

static inline int32_t Oa2dpServerUnregister(OA2DP_SERVER_REGISTRATION *r,
    const OA2DP_SERVER_OPERATIONS *ops, void *context)
{
    int32_t status;
    if (r->Handle) {
        status = ops->UnregisterServer(context, r->Handle);
        if (status < 0) return status; // do not drop PSM while server still owns callbacks
        r->Handle = 0;
    }
    if (r->PsmRegistered) {
        status = ops->UnregisterPsm(context);
        if (status < 0) return status;
        r->PsmRegistered = 0;
    }
    return 0;
}
#endif
