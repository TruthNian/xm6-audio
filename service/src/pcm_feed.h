// SPDX-License-Identifier: MIT
#pragma once
#include "oa2dp/pcm_ring.h"

namespace oa2dp {
// Authorization comes from a full OA2DP_FORMAT comparison on the policy
// thread, including valid precision; a ring representation match is not enough.
inline bool adopt_pcm_epoch(oa2dp_pcm_ring *ring, oa2dp_ring_format &current,
                            uint32_t authorized_sequence)
{
    oa2dp_ring_format next{};
    if (oa2dp_ring_snapshot(ring, &next) != OA2DP_OK ||
        next.sequence != authorized_sequence || next.sample_rate != current.sample_rate ||
        next.channels != current.channels || next.format != current.format ||
        next.capacity != current.capacity || next.storage_channels != current.storage_channels ||
        oa2dp_ring_admit(ring, &next) != OA2DP_OK)
        return false;
    current = next;
    return true;
}
} // namespace oa2dp
