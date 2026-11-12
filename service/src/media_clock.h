// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace oa2dp {
// Packet deadlines use the media sample count, not Bluetooth send completion.
// Carry fractional ticks at 44.1 kHz. A long scheduler/link stall must not cause
// an unbounded catch-up burst into a receiver whose buffer size is unknown.
class MediaClock {
public:
    MediaClock(uint64_t frequency, uint32_t rate) : hz_(frequency), rate_(rate) {}
    uint64_t deadline(uint64_t now) {
        if (!started_ || (now > next_ && now - next_ > last_duration_)) {
            next_ = now;
            started_ = true;
        }
        return next_;
    }
    void advance(uint32_t frames) {
        const uint64_t numerator = static_cast<uint64_t>(frames) * hz_ + remainder_;
        last_duration_ = numerator / rate_;
        remainder_ = numerator % rate_;
        next_ += last_duration_;
    }
private:
    uint64_t hz_, next_ = 0, remainder_ = 0, last_duration_ = 0;
    uint32_t rate_;
    bool started_ = false;
};
} // namespace oa2dp
