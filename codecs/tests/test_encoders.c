/* SPDX-License-Identifier: MIT */
/* Smoke test for every encoder adapter compiled into this build: feed one
 * second of a 1 kHz tone and check that well-formed frames come out. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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
        printf("%-18s skipped (not compiled in)\n", oa2dp_codec_name(id));
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
    run(OA2DP_CODEC_FASTSTREAM, fs, sizeof(fs));
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
