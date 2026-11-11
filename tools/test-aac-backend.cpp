/* SPDX-License-Identifier: MIT */
/* Actual backend startup/cleanup/probe and encoder availability dispatch.
 * COM/MF activation is a controlled boundary; no OS components are changed. */
#include "oa2dp/encoder.h"
#include "oa2dp/avdtp.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <initializer_list>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define SUCCEEDED(x) ((x) >= 0)
#define FAILED(x) ((x) < 0)
#define IID_PPV_ARGS(p) 0, p
#define OA2DP_WITH_MFAAC
#define OA2DP_WITH_LIBSBC
#define LOGW(...) ((void)0)
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0, S_FALSE = 1, RPC_E_CHANGED_MODE = -1, E_FAIL = -2;
constexpr unsigned COINIT_MULTITHREADED = 0, MF_VERSION = 1, MFSTARTUP_LITE = 1;
constexpr unsigned CLSCTX_INPROC_SERVER = 1, kClsidAacEncoder = 1;
static HRESULT init_result, startup_result, create_result;
static unsigned inits, starts, creates, uninitializes, shutdowns, releases, references;
static unsigned mf_owned, com_owned;
struct IMFTransform {
    void Release() { REQUIRE(references == 1); --references; ++releases; }
};
static IMFTransform transform;
static HRESULT CoInitializeEx(void *, unsigned) {
    ++inits;
    if (SUCCEEDED(init_result)) ++com_owned;
    return init_result;
}
static void CoUninitialize() { REQUIRE(com_owned == 1); --com_owned; ++uninitializes; }
static HRESULT MFStartup(unsigned, unsigned) {
    ++starts;
    if (SUCCEEDED(startup_result)) ++mf_owned;
    return startup_result;
}
static void MFShutdown() { REQUIRE(mf_owned == 1 && references == 0); --mf_owned; ++shutdowns; }
static HRESULT CoCreateInstance(unsigned, void *, unsigned, unsigned, IMFTransform **out) {
    ++creates; REQUIRE(mf_owned == 1);
    if (SUCCEEDED(create_result)) { *out = &transform; ++references; }
    return create_result;
}
#include "aac-backend-under-test.inc"
#include "aac-availability-under-test.inc"
#include "encoder-availability-under-test.inc"
#include "codec-capabilities-under-test.inc"
struct Advertisement {
    oa2dp_settings settings_{};
    oa2dp_avdtp av_{};
    std::map<uint8_t,oa2dp_codec_id> sep_codec_;
    Advertisement() {
        oa2dp_settings_default(&settings_);
        oa2dp_avdtp_callbacks cb{};
        oa2dp_avdtp_init(&av_,&cb,nullptr,672);
    }
    void publish() {
#include "codec-advertisement-under-test.inc"
    }
};
static void Reset(HRESULT init, HRESULT mf, HRESULT create) {
    REQUIRE(!references && !mf_owned && !com_owned);
    init_result = init; startup_result = mf; create_result = create;
    inits = starts = creates = uninitializes = shutdowns = releases = 0;
}
int main() {
    struct Case { HRESULT init, mf, create; bool available; unsigned start, activate, uninit, shutdown, release; };
    const Case cases[] = {
        {S_OK,S_OK,E_FAIL,false,1,1,1,1,0},
        {S_OK,S_OK,S_OK,true,1,1,1,1,1},
        {S_FALSE,S_OK,S_OK,true,1,1,1,1,1},
        {RPC_E_CHANGED_MODE,S_OK,S_OK,true,1,1,0,1,1},
        {E_FAIL,S_OK,S_OK,false,0,0,0,0,0},
        {S_OK,E_FAIL,S_OK,false,1,0,1,0,0},
        {RPC_E_CHANGED_MODE,S_OK,E_FAIL,false,1,1,0,1,0}
    };
    for (const auto &c : cases) {
        Reset(c.init,c.mf,c.create);
        REQUIRE(oa2dp_encoder_available(OA2DP_CODEC_AAC) == c.available);
        REQUIRE(inits == 1 && starts == c.start && creates == c.activate);
        REQUIRE(uninitializes == c.uninit && shutdowns == c.shutdown && releases == c.release);
        REQUIRE(!references && !mf_owned && !com_owned);
    }
    Reset(S_OK,S_OK,E_FAIL);
    REQUIRE(!oa2dp_encoder_available(OA2DP_CODEC_AAC));
    create_result = S_OK; // a later session can use a newly available backend
    REQUIRE(oa2dp_encoder_available(OA2DP_CODEC_AAC));
    REQUIRE(!references && !mf_owned && !com_owned);
    Reset(S_OK,S_OK,S_OK);
    REQUIRE(!oa2dp_encoder_available(static_cast<oa2dp_codec_id>(255)) && !inits);
    AacBackend backend;
    REQUIRE(SUCCEEDED(start_backend(&backend)));
    close_backend(&backend);
    close_backend(&backend);
    REQUIRE(releases == 1 && shutdowns == 1 && uninitializes == 1);
    Reset(S_OK,S_OK,E_FAIL);
    Advertisement missing;
    missing.publish();
    REQUIRE(missing.sep_codec_.size() == 1 && missing.sep_codec_.begin()->second == OA2DP_CODEC_SBC);
    Reset(S_OK,S_OK,S_OK);
    Advertisement present;
    present.publish();
    REQUIRE(present.sep_codec_.size() == 2 && present.sep_codec_.begin()->second == OA2DP_CODEC_AAC);
    Reset(S_OK,S_OK,E_FAIL);
    Advertisement explicit_aac;
    explicit_aac.settings_.priority[0] = OA2DP_CODEC_AAC;
    explicit_aac.settings_.priority_count = 1;
    explicit_aac.publish();
    REQUIRE(explicit_aac.sep_codec_.empty()); // explicit AAC never silently becomes SBC
    for (bool precision : {false,true}) {
        Reset(S_OK,S_OK,S_OK);
        Advertisement fixed;
        if (precision) fixed.settings_.bits = 24;
        else fixed.settings_.sample_rate = 96000;
        fixed.publish(); // only AAC/SBC compiled in this controlled fixture
        REQUIRE(fixed.sep_codec_.empty());
        REQUIRE(!inits); // do not initialize a backend excluded by user policy
    }
    std::puts("production AAC availability and balanced COM/MF lifetime checks passed");
}
