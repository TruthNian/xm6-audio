/* SPDX-License-Identifier: MIT */
/* Smoke test for usable encoder backends in this build: feed one
 * second of a 1 kHz tone and check that well-formed frames come out. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef OA2DP_WITH_LIBLDAC
#include <ldacBT.h>
#endif

#include "oa2dp/encoder.h"
#include "oa2dp/media.h"

static int failures;

#define EXPECT(c)                                                            \
    do {                                                                     \
        if (!(c)) {                                                          \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                    \
    } while (0)

static void run_profile(oa2dp_codec_id id, const uint8_t *remote_ie, size_t ie_len,
                        uint32_t rate, uint8_t bits)
{
    oa2dp_codec_caps remote, cfg;
    oa2dp_settings s;
    oa2dp_encoder *enc = NULL;

    if (!oa2dp_encoder_available(id)) {
        printf("%-18s skipped (backend unavailable)\n", oa2dp_codec_name(id));
        return;
    }
    oa2dp_settings_default(&s);
    s.sample_rate = rate;
    s.bits = bits;
    EXPECT(oa2dp_codec_parse(remote_ie, ie_len, &remote) == OA2DP_OK);
    EXPECT(oa2dp_codec_select(NULL, &remote, &s, &cfg) == OA2DP_OK);
    int r = oa2dp_encoder_create(&cfg, &s, 895, &enc);
    if (r != OA2DP_OK) {
        printf("%-18s create failed: %s\n", oa2dp_codec_name(id), oa2dp_strerror(r));
        failures++;
        return;
    }

    const oa2dp_encoder_info *in = &enc->info;
    if (id == OA2DP_CODEC_LDAC) {
        EXPECT(oa2dp_codec_check_settings(&cfg, &s) == OA2DP_OK);
        EXPECT(in->sample_rate == (rate ? rate : 48000) && in->channels == 2);
        EXPECT(oa2dp_codec_pcm_bits(&cfg, &s) == (bits ? bits : 24));
        EXPECT(in->levels == 1 && !in->native_abr);
    }
    size_t n = (size_t)in->samples_per_call * in->channels;
    int32_t *pcm = malloc(n * sizeof(*pcm));
    uint8_t *out = malloc(in->max_output);
    uint32_t calls = in->sample_rate / in->samples_per_call;
    size_t total = 0, frames_total = 0;
    double phase = 0, step = 2 * 3.14159265358979 * 1000 / in->sample_rate;

    for (uint32_t c = 0; c < calls; c++) {
        for (uint32_t i = 0; i < in->samples_per_call; i++) {
            int32_t v = (int32_t)(sin(phase) * 4000000.0);
            phase += step;
            for (uint8_t ch = 0; ch < in->channels; ch++)
                pcm[i * in->channels + ch] = v;
        }
        size_t len = 0;
        uint8_t frames = 0;
        r = oa2dp_encoder_encode(enc, pcm, out, in->max_output, &len, &frames);
        EXPECT(r == OA2DP_OK);
        if (r != OA2DP_OK)
            break;
        EXPECT(len <= in->max_output);
        if (id == OA2DP_CODEC_SBC && len)
            EXPECT(out[0] == 0x9C); /* SBC syncword */
        if (id == OA2DP_CODEC_AAC && len)
            EXPECT((out[0] & 0x80) == 0); /* LATM useSameStreamMux = 0 */
        total += len;
        frames_total += frames;
    }
    uint32_t spf = in->samples_per_frame ? in->samples_per_frame : in->samples_per_call;
    double kbps = frames_total ? total * 8.0 * in->sample_rate / (frames_total * spf * 1000.0) : 0;
    printf("%-18s %u Hz %u ch %u bit: %zu frames, %.3f kbit/s\n", oa2dp_codec_name(id),
           (unsigned)in->sample_rate, in->channels, oa2dp_codec_pcm_bits(&cfg, &s), frames_total, kbps);
    EXPECT(frames_total > 0);
    EXPECT(total > 0);
    if (id == OA2DP_CODEC_LDAC && frames_total) {
        /* Account for encoder priming by measuring represented frames. */
        double expected = in->sample_rate % 48000 == 0 ? 990000.0 : 909562.5;
        EXPECT(fabs(kbps * 1000.0 - expected) < 1.0);
    }
    free(pcm);
    free(out);
    oa2dp_encoder_destroy(enc);
}

static void run(oa2dp_codec_id id, const uint8_t *remote_ie, size_t ie_len)
{
    run_profile(id, remote_ie, ie_len, 0, 0);
}

#ifdef OA2DP_WITH_LIBLDAC
static void native_ldac(const uint8_t *remote_ie, size_t ie_len)
{
    const uint32_t rates[] = {44100, 48000, 88200, 96000};
    unsigned comparisons = 0;
    for (size_t ri = 0; ri < 4; ++ri) for (unsigned kind = 0; kind < 3; ++kind)
    for (unsigned channels = 1; channels <= 2; ++channels) {
        oa2dp_settings s; oa2dp_settings_default(&s); s.sample_rate = rates[ri];
        oa2dp_codec_caps remote, cfg;
        EXPECT(oa2dp_codec_parse(remote_ie, ie_len, &remote) == OA2DP_OK);
        EXPECT(oa2dp_codec_select(NULL, &remote, &s, &cfg) == OA2DP_OK);
        cfg.u.ldac.chmode = channels == 1 ? LDACBT_CHANNEL_MODE_MONO : LDACBT_CHANNEL_MODE_STEREO;
        oa2dp_pcm_format format = {rates[ri], (uint16_t)channels, 32, kind == 2 ? 24 : 32,
            kind == 1 ? OA2DP_PCM_FLOAT : OA2DP_PCM_INTEGER};
        oa2dp_encoder *enc = NULL;
        EXPECT(oa2dp_encoder_create_for_pcm(&cfg, &s, 895, &format, &enc) == OA2DP_OK);
        if (!enc) continue;
        EXPECT(enc->info.input_format == (kind == 1 ? OA2DP_ENCODER_INPUT_F32 : OA2DP_ENCODER_INPUT_S32));
        HANDLE_LDAC_BT reference = ldacBT_get_handle();
        EXPECT(reference != NULL);
        if (!reference) { oa2dp_encoder_destroy(enc); continue; }
        int initialized = ldacBT_init_handle_encode(reference, 895, LDACBT_EQMID_HQ,
            cfg.u.ldac.chmode, kind == 1 ? LDACBT_SMPL_FMT_F32 : LDACBT_SMPL_FMT_S32, rates[ri]);
        EXPECT(initialized == 0);
        if (initialized != 0) { ldacBT_free_handle(reference); oa2dp_encoder_destroy(enc); continue; }
        uint32_t rng = 0x62a52d51;
        unsigned total_frames = 0;
        for (unsigned block = 0; block < 64; ++block) {
            int32_t words[256], before[256]; uint8_t actual[1024], expected[1024];
            size_t count = 128 * channels;
            for (size_t i = 0; i < count; ++i) {
                rng = rng * 1664525u + 1013904223u;
                if (kind == 1) {
                    float value = (float)((double)(rng & 0xffffff) / 8388608.0 - 1.0);
                    if (i % 7 == 0) value = (i % 2 ? -1.0f : 1.0f) / 67108864.0f;
                    memcpy(words+i, &value, sizeof(value));
                } else {
                    uint32_t value = kind == 2 ? rng & 0xffffff00u : rng;
                    if (kind == 0 && i % 7 == 0) value = i % 2 ? (uint32_t)-127 : 127;
                    memcpy(words+i, &value, sizeof(value));
                }
            }
            memcpy(before, words, count * sizeof(*words));
            int used = 0, size = 0, frames = 0; size_t actual_size = 0; uint8_t actual_frames = 0;
            EXPECT(ldacBT_encode(reference, words, &used, expected, &size, &frames) == 0);
            EXPECT(used == (int)(count * sizeof(*words)));
            EXPECT(oa2dp_encoder_encode(enc, words, actual, sizeof(actual), &actual_size, &actual_frames) == OA2DP_OK);
            EXPECT(actual_size == (size_t)size && actual_frames == frames);
            EXPECT(size >= 0 && size <= (int)sizeof(expected));
            if (actual_size == (size_t)size && size >= 0 && size <= (int)sizeof(expected))
                EXPECT(memcmp(actual, expected, size) == 0);
            EXPECT(memcmp(words, before, count * sizeof(*words)) == 0);
            if (channels == 2 && frames) EXPECT(size == frames * 330);
            total_frames += actual_frames; ++comparisons;
        }
        EXPECT(total_frames > 0);
        ldacBT_close_handle(reference); ldacBT_free_handle(reference); oa2dp_encoder_destroy(enc);
        for (unsigned bits = 16; bits <= 32; bits += 8) {
            s.bits = (uint8_t)bits; enc = NULL;
            EXPECT(oa2dp_encoder_create_for_pcm(&cfg, &s, 895, &format, &enc) == OA2DP_OK);
            if (enc) {
                EXPECT(enc->info.input_format == (bits == 32 ?
                    (kind == 1 ? OA2DP_ENCODER_INPUT_F32 : OA2DP_ENCODER_INPUT_S32) : OA2DP_ENCODER_INPUT_S24));
                oa2dp_encoder_destroy(enc);
            }
        }
        s.bits = 0; format.Encoding = OA2DP_PCM_INTEGER; format.ValidBitsPerSample = 16; enc = NULL;
        EXPECT(oa2dp_encoder_create_for_pcm(&cfg, &s, 895, &format, &enc) == OA2DP_OK);
        if (enc) { EXPECT(enc->info.input_format == OA2DP_ENCODER_INPUT_S24); oa2dp_encoder_destroy(enc); }
        format.SampleRate = rates[ri] == 48000 ? 44100 : 48000; enc = NULL;
        EXPECT(oa2dp_encoder_create_for_pcm(&cfg, &s, 895, &format, &enc) == OA2DP_ERR_INVAL && !enc);
    }
    printf("native LDAC matched direct libldac output for %u calls (S32/F32/24-in-32, four rates, mono/stereo)\n", comparisons);
}
#endif

int main(void)
{
    static const uint8_t sbc[] = {0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35};
    static const uint8_t aac[] = {0x00, 0x02, 0x80, 0x01, 0x8C, 0x84, 0xE2, 0x00};
    static const uint8_t aptx[] = {0x00, 0xFF, 0x4F, 0, 0, 0, 0x01, 0x00, 0x32};
    static const uint8_t aptx_hd[] = {0x00, 0xFF, 0xD7, 0, 0, 0, 0x24, 0x00, 0x32, 0, 0, 0, 0};
    static const uint8_t ldac[] = {0x00, 0xFF, 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07};
    static const uint8_t fs[] = {0x00, 0xFF, 0x0A, 0, 0, 0, 0x01, 0x00, 0x03, 0x23};

    run(OA2DP_CODEC_SBC, sbc, sizeof(sbc));
    run(OA2DP_CODEC_AAC, aac, sizeof(aac));
    run(OA2DP_CODEC_APTX, aptx, sizeof(aptx));
    run(OA2DP_CODEC_APTX_HD, aptx_hd, sizeof(aptx_hd));
    run(OA2DP_CODEC_LDAC, ldac, sizeof(ldac));
    run_profile(OA2DP_CODEC_LDAC, ldac, sizeof(ldac), 44100, 16);
    run_profile(OA2DP_CODEC_LDAC, ldac, sizeof(ldac), 88200, 24);
    run_profile(OA2DP_CODEC_LDAC, ldac, sizeof(ldac), 96000, 24);
#ifdef OA2DP_WITH_LIBLDAC
    native_ldac(ldac, sizeof(ldac));
#endif
    run(OA2DP_CODEC_FASTSTREAM, fs, sizeof(fs));
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
