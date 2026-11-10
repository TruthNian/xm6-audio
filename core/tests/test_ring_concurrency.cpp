/* SPDX-License-Identifier: MIT */
/* Real producer/consumer threads; memory-only, no device or audio API. */
#include "oa2dp/pcm_ring.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static uint32_t sample(const oa2dp_ring_format &format, unsigned channel)
{
    return format.format == OA2DP_RING_FMT_FLOAT32 ?
        (0x3E800000u | ((format.sequence & 0xFFFF) << 2) | channel) :
        format.sequence * 4 + channel + 1;
}
static bool overrun_observer()
{
    auto *r = static_cast<oa2dp_pcm_ring *>(std::calloc(1, oa2dp_ring_bytes(8, 2)));
    if (!r || oa2dp_ring_init(r, 8, 48000, 2, OA2DP_RING_FMT_S32) != OA2DP_OK) return false;
    int32_t input[16]{};
    if (oa2dp_ring_write(r, input, 8) != 8) { std::free(r); return false; }
    const uint64_t seed = UINT64_C(0xFFFFFFFF) - 16, expected = seed + 20000 * 7;
    r->overruns = seed; /* before any concurrent access */
    std::atomic<bool> ready{false}, done{false}, failed{false};
    std::thread observer([&] {
        uint64_t previous = seed;
        ready = true;
        do {
            const auto current = oa2dp_ring_overruns(r);
            if (current < previous || current > expected) failed = true;
            previous = current;
        } while (!done.load());
    });
    while (!ready.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 20000; ++i)
        if (oa2dp_ring_write(r, input, 7) != 0) failed = true;
    done = true; observer.join();
    const bool ok = !failed.load() && oa2dp_ring_overruns(r) == expected;
    std::free(r);
    return ok;
}
int main()
{
    if (!overrun_observer()) { std::fputs("Concurrent overrun observation failed.\n", stderr); return 1; }
    static_assert(sizeof(oa2dp_pcm_ring) == 80, "Mapped header size changed");
    auto *ring = static_cast<oa2dp_pcm_ring *>(std::calloc(1, oa2dp_ring_bytes(64, 2)));
    if (!ring || oa2dp_ring_init(ring, 64, 48000, 2, OA2DP_RING_FMT_S32_FULL) != OA2DP_OK)
        return 1;
    std::atomic<bool> start{false}, done{false}, failed{false};
    unsigned blocks = 0;
    std::thread consumer([&] {
        while (!start.load()) std::this_thread::yield();
        while (!failed.load()) {
            oa2dp_ring_format format{};
            int status = oa2dp_ring_snapshot(ring, &format);
            if (status == OA2DP_ERR_BUSY) continue;
            if (status != OA2DP_OK) { failed = true; break; }
            uint32_t output[10];
            for (auto &value : output) value = 0xA5A5A5A5u;
            const size_t samples = 4 * format.channels;
            uint32_t got = 0;
            status = oa2dp_ring_read(ring, &format, output, samples * sizeof(uint32_t), 4, &got);
            // Check the boundary even for a rejected, concurrently changed block.
            if (output[samples] != 0xA5A5A5A5u) { failed = true; break; }
            if (status == OA2DP_ERR_STATE || status == OA2DP_ERR_BUSY) continue;
            if (status != OA2DP_OK) { failed = true; break; }
            if (got) {
                if (got != 4) { failed = true; break; }
                ++blocks;
                for (size_t n = 0; n < samples; ++n)
                    if (output[n] != sample(format, static_cast<unsigned>(n % format.channels)))
                        failed = true;
            } else {
                if (done.load() && oa2dp_ring_readable(ring) == 0) break;
                std::this_thread::yield();
            }
        }
    });
    std::thread producer([&] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned n = 0; n < 10000 && !failed.load(); ++n) {
            const bool mono = (n & 1) == 0;
            if (oa2dp_ring_set_format(ring, mono ? 44100 : 48000, mono ? 1 : 2,
                    mono ? OA2DP_RING_FMT_FLOAT32 : OA2DP_RING_FMT_S32_FULL) != 1) {
                failed = true; break;
            }
            oa2dp_ring_format format{};
            if (oa2dp_ring_snapshot(ring, &format) != OA2DP_OK) { failed = true; break; }
            uint32_t input[16];
            for (unsigned i = 0; i < 8 * format.channels; ++i)
                input[i] = sample(format, i % format.channels);
            unsigned written = 0;
            while (written < 8 && !failed.load()) {
                written += oa2dp_ring_write(ring, input + written * format.channels, 8 - written);
                if (written < 8) std::this_thread::yield();
            }
        }
        done = true;
    });
    start = true;
    producer.join(); consumer.join();
    const bool ok = !failed.load() && blocks > 0;
    std::free(ring);
    if (!ok) { std::fputs("Concurrent PCM format/read contract failed.\n", stderr); return 1; }
    std::printf("Concurrent PCM PASS: 10000 format transitions, %u complete blocks; no audio device opened.\n", blocks);
    return 0;
}
