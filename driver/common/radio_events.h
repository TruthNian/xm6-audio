/* SPDX-License-Identifier: MIT */
#ifndef OA2DP_RADIO_EVENTS_H
#define OA2DP_RADIO_EVENTS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Public BTH_HCI_EVENT_INFO wire-in-memory fields: BTH_ADDR, type, connected.
 * Do not depend on trailing C structure padding or alignment of custom data. */
#define OA2DP_HCI_ACL 1
static inline int Oa2dpAclConnectHint(const void *data, size_t bytes, uint64_t peer)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t address;
    if (!p || bytes < sizeof(address) + 2) return 0;
    memcpy(&address, p, sizeof(address));
    return address == peer && p[sizeof(address)] == OA2DP_HCI_ACL &&
           p[sizeof(address) + 1] != 0;
}
#endif
