/* SPDX-License-Identifier: MIT */
#include "oa2dp/pcm_ring.h"

#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#if defined(_KERNEL_MODE)
#include <wdm.h>
#define OA2DP_FENCE() KeMemoryBarrier()
#elif defined(_M_ARM64)
#define OA2DP_FENCE() __dmb(_ARM64_BARRIER_ISH)
#else
#define OA2DP_FENCE() __faststorefence()
#endif
#else
#define OA2DP_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

static uint8_t *ring_data(const oa2dp_pcm_ring *r)
{
    return (uint8_t *)(r + 1);
}

static size_t frame_bytes(const oa2dp_pcm_ring *r)
{
    return (size_t)r->channels * 4;
}

/* Interlocked operations also work across the kernel/user mapping. On other
 * platforms use compiler atomics, including for metadata read by a seqlock. */
static uint32_t load32(const volatile uint32_t *p)
{
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
static uint16_t load16(const uint16_t *p)
{
#if defined(_MSC_VER)
    return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
static uint64_t load64(const volatile uint64_t *p)
{
#if defined(_MSC_VER)
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
#endif
}
static void store32(volatile uint32_t *p, uint32_t value)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)p, (long)value);
#else
    __atomic_store_n(p, value, __ATOMIC_SEQ_CST);
#endif
}
static void store16(uint16_t *p, uint16_t value)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange16((volatile short *)p, (short)value);
#else
    __atomic_store_n(p, value, __ATOMIC_SEQ_CST);
#endif
}
static void store64(volatile uint64_t *p, uint64_t value)
{
#if defined(_MSC_VER)
    (void)_InterlockedExchange64((volatile long long *)p, (long long)value);
#else
    __atomic_store_n(p, value, __ATOMIC_SEQ_CST);
#endif
}

static void add_overruns(oa2dp_pcm_ring *r, uint32_t frames)
{
#if defined(_MSC_VER) && !defined(_M_IX86)
    (void)_InterlockedExchangeAdd64((volatile long long *)&r->overruns, (long long)frames);
#elif defined(_MSC_VER)
    /* The producer is the sole writer; preserve atomic publication on x86. */
    store64(&r->overruns, load64(&r->overruns) + frames);
#else
    (void)__atomic_fetch_add(&r->overruns, (uint64_t)frames, __ATOMIC_SEQ_CST);
#endif
}

uint64_t oa2dp_ring_overruns(const oa2dp_pcm_ring *r)
{
    return load64(&r->overruns);
}

size_t oa2dp_ring_bytes(uint32_t capacity, uint16_t channels)
{
    return sizeof(oa2dp_pcm_ring) + (size_t)capacity * channels * 4;
}

int oa2dp_ring_init(oa2dp_pcm_ring *r, uint32_t capacity, uint32_t sample_rate,
                    uint16_t channels, uint16_t format)
{
    if (!r || !sample_rate || capacity == 0 || (capacity & (capacity - 1)) || channels == 0 ||
        channels > 8 || format > OA2DP_RING_FMT_S32_FULL)
        return OA2DP_ERR_INVAL;
    memset(r, 0, sizeof(*r));
    r->magic = OA2DP_RING_MAGIC;
    r->version = OA2DP_RING_VERSION;
    r->capacity = capacity;
    r->sample_rate = sample_rate;
    r->channels = channels;
    r->storage_channels = channels;
    r->format = format;
    return OA2DP_OK;
}

int oa2dp_ring_validate(const oa2dp_pcm_ring *r, size_t mapped)
{
    if (!r || mapped < sizeof(*r))
        return OA2DP_ERR_INVAL;
    oa2dp_ring_format format;
    int status = oa2dp_ring_snapshot(r, &format);
    if (status != OA2DP_OK)
        return status;
    if (oa2dp_ring_bytes(format.capacity, format.storage_channels) > mapped)
        return OA2DP_ERR_PROTO;
    return OA2DP_OK;
}

int oa2dp_ring_snapshot(const oa2dp_pcm_ring *r, oa2dp_ring_format *format)
{
    if (!r || !format)
        return OA2DP_ERR_INVAL;
    if (r->magic != OA2DP_RING_MAGIC || r->version != OA2DP_RING_VERSION)
        return OA2DP_ERR_PROTO;
    oa2dp_ring_format value;
    memset(&value, 0, sizeof(value));
    value.sequence = load32(&r->format_sequence);
    if (value.sequence & 1)
        return OA2DP_ERR_BUSY;
    value.sample_rate = load32(&r->sample_rate);
    value.channels = load16(&r->channels);
    value.format = load16(&r->format);
    value.start_pos = load64(&r->format_start_pos);
    value.capacity = r->capacity;
    value.storage_channels = r->storage_channels;
    if (load32(&r->format_sequence) != value.sequence)
        return OA2DP_ERR_BUSY;
    if (!value.sample_rate || !value.capacity || (value.capacity & (value.capacity - 1)) || !value.channels ||
        value.storage_channels < value.channels || value.storage_channels > 8 ||
        value.format > OA2DP_RING_FMT_S32_FULL)
        return OA2DP_ERR_PROTO;
    *format = value;
    return OA2DP_OK;
}

int oa2dp_ring_set_format(oa2dp_pcm_ring *r, uint32_t rate, uint16_t channels, uint16_t format)
{
    if (!r || !rate || !channels || channels > r->storage_channels ||
        format > OA2DP_RING_FMT_S32_FULL)
        return OA2DP_ERR_INVAL;
    if (r->sample_rate == rate && r->channels == channels && r->format == format)
        return 0;
    uint32_t sequence = load32(&r->format_sequence);
    if (sequence & 1)
        return OA2DP_ERR_BUSY;
    store32(&r->format_sequence, sequence + 1);
    store32(&r->sample_rate, rate);
    store16(&r->channels, channels);
    store16(&r->format, format);
    store64(&r->format_start_pos, load64(&r->write_pos));
#if defined(_MSC_VER)
    (void)_InterlockedOr((volatile long *)&r->flags, OA2DP_RING_FLAG_FORMAT_CHANGED);
#else
    (void)__atomic_fetch_or(&r->flags, OA2DP_RING_FLAG_FORMAT_CHANGED, __ATOMIC_SEQ_CST);
#endif
    store32(&r->format_sequence, sequence + 2);
    return 1;
}

uint32_t oa2dp_ring_readable(const oa2dp_pcm_ring *r)
{
    oa2dp_ring_format format;
    if (oa2dp_ring_snapshot(r, &format) != OA2DP_OK)
        return 0;
    uint64_t rd = load64(&r->read_pos);
    if (rd < format.start_pos)
        rd = format.start_pos;
    uint64_t d = load64(&r->write_pos) - rd;
    return d > r->capacity ? r->capacity : (uint32_t)d;
}

uint32_t oa2dp_ring_writable(const oa2dp_pcm_ring *r)
{
    uint64_t rd = load64(&r->read_pos);
    uint64_t d = load64(&r->write_pos) - rd;
    return d >= r->capacity ? 0 : (uint32_t)(r->capacity - d);
}

uint32_t oa2dp_ring_discard(oa2dp_pcm_ring *r)
{
    uint64_t w = load64(&r->write_pos);
    uint64_t discarded = w - load64(&r->read_pos);
    store64(&r->read_pos, w);
    return discarded > r->capacity ? r->capacity : (uint32_t)discarded;
}

static void copy_in(oa2dp_pcm_ring *r, uint64_t pos, const uint8_t *src, uint32_t frames)
{
    size_t fb = frame_bytes(r);
    size_t stride = (size_t)r->storage_channels * 4;
    if (stride != fb) {
        for (uint32_t i = 0; i < frames; ++i)
            memcpy(ring_data(r) + ((pos + i) & (r->capacity - 1)) * stride, src + i * fb, fb);
        return;
    }
    uint32_t slot = (uint32_t)(pos & (r->capacity - 1));
    uint32_t first = r->capacity - slot;
    if (first > frames)
        first = frames;
    memcpy(ring_data(r) + slot * fb, src, first * fb);
    if (frames > first)
        memcpy(ring_data(r), src + first * fb, (frames - first) * fb);
}

static void copy_out(const oa2dp_pcm_ring *r, const oa2dp_ring_format *format,
                      uint64_t pos, uint8_t *dst, uint32_t frames)
{
    size_t fb = (size_t)format->channels * 4;
    size_t stride = (size_t)format->storage_channels * 4;
    if (stride != fb) {
        for (uint32_t i = 0; i < frames; ++i)
            memcpy(dst + i * fb, ring_data(r) + ((pos + i) & (format->capacity - 1)) * stride, fb);
        return;
    }
    uint32_t slot = (uint32_t)(pos & (format->capacity - 1));
    uint32_t first = format->capacity - slot;
    if (first > frames)
        first = frames;
    memcpy(dst, ring_data(r) + slot * fb, first * fb);
    if (frames > first)
        memcpy(dst + first * fb, ring_data(r), (frames - first) * fb);
}

static void convert_integer(int32_t *dst, const uint8_t *src, size_t samples, uint16_t bits)
{
    if (bits == 16) {
        for (size_t i = 0; i < samples; ++i, src += 2) {
            uint32_t value = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
            int32_t signed_value = value < 0x8000u ? (int32_t)value : (int32_t)value - 0x10000;
            dst[i] = signed_value * 256;
        }
    } else {
        for (size_t i = 0; i < samples; ++i, src += 3) {
            uint32_t value = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
            dst[i] = value < 0x800000u ? (int32_t)value : (int32_t)value - 0x1000000;
        }
    }
}

static void copy_in_integer(oa2dp_pcm_ring *r, uint64_t pos, const uint8_t *src,
                             uint32_t frames, uint16_t bits)
{
    const size_t fb = (size_t)r->channels * 4;
    const size_t stride = (size_t)r->storage_channels * 4;
    const size_t input_fb = (size_t)r->channels * (bits / 8);
    if (stride != fb) {
        for (uint32_t i = 0; i < frames; ++i)
            convert_integer((int32_t *)(ring_data(r) + ((pos + i) & (r->capacity - 1)) * stride),
                src + i * input_fb, r->channels, bits);
        return;
    }
    const uint32_t slot = (uint32_t)(pos & (r->capacity - 1));
    const uint32_t first = frames < r->capacity - slot ? frames : r->capacity - slot;
    convert_integer((int32_t *)(ring_data(r) + slot * stride), src, (size_t)first * r->channels, bits);
    if (frames > first)
        convert_integer((int32_t *)ring_data(r), src + first * input_fb,
            (size_t)(frames - first) * r->channels, bits);
}

static uint32_t write_samples(oa2dp_pcm_ring *r, const void *data, uint32_t frames, uint16_t bits)
{
    uint32_t room = oa2dp_ring_writable(r);
    uint32_t n = frames < room ? frames : room;
    if (n < frames)
        add_overruns(r, frames - n);
    if (n) {
        uint64_t pos = load64(&r->write_pos);
        if (bits == 32)
            copy_in(r, pos, (const uint8_t *)data, n);
        else
            copy_in_integer(r, pos, (const uint8_t *)data, n, bits);
        OA2DP_FENCE(); /* data visible before the new position */
        store64(&r->write_pos, pos + n);
    }
    return n;
}

uint32_t oa2dp_ring_write(oa2dp_pcm_ring *r, const void *data, uint32_t frames)
{
    return write_samples(r, data, frames, 32);
}

uint32_t oa2dp_ring_write_pcm(oa2dp_pcm_ring *r, const void *data, uint32_t frames,
                             uint16_t container_bits)
{
    if (container_bits != 32 && ((container_bits != 16 && container_bits != 24) ||
        r->format != OA2DP_RING_FMT_S32))
        return 0;
    return write_samples(r, data, frames, container_bits);
}

int oa2dp_ring_read(oa2dp_pcm_ring *r, const oa2dp_ring_format *format,
                    void *data, size_t data_bytes, uint32_t frames, uint32_t *read_frames)
{
    if (!read_frames)
        return OA2DP_ERR_INVAL;
    *read_frames = 0;
    if (!format || (!data && frames))
        return OA2DP_ERR_INVAL;
    oa2dp_ring_format current;
    int status = oa2dp_ring_snapshot(r, &current);
    if (status != OA2DP_OK)
        return status;
    if (current.sequence != format->sequence || current.sample_rate != format->sample_rate ||
        current.channels != format->channels || current.format != format->format ||
        current.capacity != format->capacity || current.storage_channels != format->storage_channels ||
        current.start_pos != format->start_pos)
        return OA2DP_ERR_STATE;
    size_t fb = (size_t)format->channels * 4;
    if (frames > format->capacity || frames > data_bytes / fb)
        return OA2DP_ERR_INVAL;
    uint64_t rd = load64(&r->read_pos);
    if (rd < format->start_pos)
        rd = format->start_pos;
    uint64_t w = load64(&r->write_pos);
    if (w < rd || w - rd > format->capacity)
        return OA2DP_ERR_PROTO;
    uint32_t n = w - rd >= frames ? frames : 0;
    if (n)
        copy_out(r, format, rd, (uint8_t *)data, n);
    OA2DP_FENCE();
    if (load32(&r->format_sequence) != format->sequence)
        return OA2DP_ERR_STATE;
    // Even an empty new-format block releases the obsolete backlog, so a full
    // ring from the previous format cannot starve its new producer.
    store64(&r->read_pos, rd + n);
    *read_frames = n;
    return OA2DP_OK;
}

/* Consumer-only; kept out of oa2dpaudio.sys, which never touches floats. */
#if !defined(_KERNEL_MODE)
static int32_t quantize_float(double value, int32_t scale, int32_t expand)
{
    if (!(value == value)) value = 0.0; /* NaN, including opposite infinities after a mix */
    if (value > 1.0) value = 1.0;
    if (value < -1.0) value = -1.0;
    const double scaled = value * scale;
    int32_t rounded = (int32_t)(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
    if (rounded >= scale) rounded = scale - 1;
    return rounded * expand;
}

static int32_t quantize_integer(int64_t value, int64_t step, int32_t limit, int32_t expand)
{
    int64_t rounded = value < 0 ? -((-value + step / 2) / step) : (value + step / 2) / step;
    if (rounded >= limit) rounded = limit - 1;
    if (rounded < -limit) rounded = -limit;
    return (int32_t)rounded * expand;
}

void oa2dp_pcm_float_to_s24(const float *in, int32_t *out, size_t n)
{
    oa2dp_pcm_float_to_quantized24(in, out, n, 24);
}

void oa2dp_pcm_float_to_quantized24(const float *in, int32_t *out, size_t n, uint8_t bits)
{
    const int32_t scale = bits == 16 ? 32768 : 8388608;
    const int32_t expand = bits == 16 ? 256 : 1;
    for (size_t i = 0; i < n; i++)
        out[i] = quantize_float((double)in[i], scale, expand);
}

void oa2dp_pcm_s32_to_quantized24(const int32_t *in, int32_t *out, size_t n, uint8_t bits)
{
    const int64_t step = bits == 16 ? 65536 : 256;
    const int32_t limit = bits == 16 ? 32768 : 8388608;
    const int32_t expand = bits == 16 ? 256 : 1;
    for (size_t i = 0; i < n; i++)
        out[i] = quantize_integer(in[i], step, limit, expand);
}

static double load_float_sample(const uint8_t *input, size_t sample)
{
    float value;
    /* A ring read can place float bits in the same int32_t storage used for
     * codec output. memcpy avoids aliasing/alignment assumptions on that input. */
    memcpy(&value, input + sample * sizeof(value), sizeof(value));
    return value == value ? (double)value : 0.0;
}

static int64_t load_integer_sample(const uint8_t *input, size_t sample)
{
    int32_t value;
    memcpy(&value, input + sample * sizeof(value), sizeof(value));
    return value;
}

int oa2dp_pcm_prepare_encoder(const oa2dp_ring_format *format,
                              const void *input, size_t input_bytes,
                              int32_t *output, size_t output_samples,
                              uint32_t frames, uint16_t output_channels, uint8_t bits)
{
    if (!format || !format->channels || format->channels > 2 ||
        !output_channels || output_channels > 2 || format->format > OA2DP_RING_FMT_S32_FULL ||
        (bits != 16 && bits != 24) || (frames && (!input || !output)))
        return OA2DP_ERR_INVAL;
    if (frames > input_bytes / sizeof(int32_t) / format->channels)
        return OA2DP_ERR_INVAL;
    if (frames > output_samples / output_channels)
        return OA2DP_ERR_NOSPC;
    const uint8_t *source = (const uint8_t *)input;
    const int32_t limit = bits == 16 ? 32768 : 8388608;
    const int32_t expand = bits == 16 ? 256 : 1;
    const uint16_t channels = format->channels;
    if (channels == output_channels) {
        const size_t samples = (size_t)frames * channels;
        if (format->format == OA2DP_RING_FMT_FLOAT32) {
            for (size_t i = 0; i < samples; ++i)
                output[i] = quantize_float(load_float_sample(source, i), limit, expand);
        } else if (format->format == OA2DP_RING_FMT_S32 && bits == 24) {
            /* Already in the encoder's declared S24 representation. */
            if (samples && input != output) memcpy(output, input, samples * sizeof(int32_t));
        } else {
            const int64_t step = format->format == OA2DP_RING_FMT_S32_FULL ?
                (bits == 16 ? 65536 : 256) : 256;
            for (size_t i = 0; i < samples; ++i)
                output[i] = quantize_integer(load_integer_sample(source, i), step, limit, expand);
        }
        return OA2DP_OK;
    }
    /* Only stereo-to-mono or mono-to-stereo remain after the equal-channel path. */
    const int downmix = channels == 2 && output_channels == 1;
    const int reverse = output_channels > channels; /* protect identical buffers on expansion */
    if (format->format == OA2DP_RING_FMT_FLOAT32) {
        for (size_t i = 0; i < frames; ++i) {
            const size_t frame = reverse ? frames - 1 - i : i;
            const size_t src = frame * channels, dst = frame * output_channels;
            double value = load_float_sample(source, src);
            if (downmix) value = (value + load_float_sample(source, src + 1)) * 0.5;
            output[dst] = quantize_float(value, limit, expand);
            if (output_channels == 2) output[dst + 1] = output[dst];
        }
    } else {
        const int64_t base_step = format->format == OA2DP_RING_FMT_S32_FULL ?
            (bits == 16 ? 65536 : 256) : (bits == 16 ? 256 : 1);
        const int64_t step = downmix ? base_step * 2 : base_step;
        for (size_t i = 0; i < frames; ++i) {
            const size_t frame = reverse ? frames - 1 - i : i;
            const size_t src = frame * channels, dst = frame * output_channels;
            int64_t value = load_integer_sample(source, src);
            if (downmix) value += load_integer_sample(source, src + 1);
            output[dst] = quantize_integer(value, step, limit, expand);
            if (output_channels == 2) output[dst + 1] = output[dst];
        }
    }
    return OA2DP_OK;
}
#endif

void oa2dp_pcm_downmix_to_mono(const int32_t *in, int32_t *out, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
        out[i] = (int32_t)(((int64_t)in[2 * i] + in[2 * i + 1]) / 2);
}
