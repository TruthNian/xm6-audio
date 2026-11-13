cmake_minimum_required(VERSION 3.21)
project(XM6Audio VERSION 0.4.4 LANGUAGES C CXX)

# The loaded binary's version must agree with the configured project and INF.
# Generate resources in the build tree rather than keeping a stale copy.
configure_file(native-version.rc.in native-version.rc @ONLY)
set(XM6_VERSION_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/native-version.rc")

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(OA2DP_BUILD_TESTS "Build unit tests" ON)
option(OA2DP_BUILD_DRIVERS "Build kernel drivers (needs the WDK and FindWDK)" OFF)
option(XM6_BUILD_GUI "Build the optional developer settings window" OFF)
option(XM6_REQUIRE_LDAC "Fail a Windows build without the pinned LDAC encoder" ON)

set(OA2DP_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# --- portable protocol core ------------------------------------------------
add_library(oa2dp_core STATIC
    core/src/a2dp_codecs.c
    core/src/avdtp.c
    core/src/media.c
    core/src/pcm_ring.c
    core/src/pcm_format.c
    core/src/settings.c)
target_include_directories(oa2dp_core PUBLIC core/include)
if(MSVC)
    target_compile_options(oa2dp_core PRIVATE /W4)
else()
    target_compile_options(oa2dp_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(OA2DP_BUILD_TESTS)
    enable_testing()
    add_executable(oa2dp_tests
        core/tests/test_main.c core/tests/test_avdtp.c core/tests/test_codecs.c
        core/tests/test_media.c core/tests/test_ring.c core/tests/test_settings.c
        core/tests/test_pcm_format.c)
    target_link_libraries(oa2dp_tests PRIVATE oa2dp_core)
    add_test(NAME core COMMAND oa2dp_tests)
    find_package(Threads REQUIRED)
    add_executable(test_ring_concurrency core/tests/test_ring_concurrency.cpp)
    target_link_libraries(test_ring_concurrency PRIVATE oa2dp_core Threads::Threads)
    add_test(NAME ring_concurrency COMMAND test_ring_concurrency)
    set_tests_properties(ring_concurrency PROPERTIES TIMEOUT 15)
    add_executable(test_channel_lifecycle tools/test-channel-lifecycle.cpp)
    add_test(NAME channel_lifecycle COMMAND test_channel_lifecycle)
    # Compile production bodies against fake OS APIs, rather than duplicating
    # their implementation in a model. Fail closed if named boundaries move.
    function(xm6_test_body source begin_marker end_marker name)
        set(path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" text)
        string(REPLACE "\r\n" "\n" text "${text}")
        string(FIND "${text}" "${begin_marker}" first)
        if(end_marker STREQUAL "")
            string(LENGTH "${text}" last)
        else()
            string(FIND "${text}" "${end_marker}" last)
        endif()
        if(first LESS 0 OR last LESS_EQUAL first)
            message(FATAL_ERROR "Cannot locate production body for ${name}")
        endif()
        math(EXPR count "${last} - ${first}")
        string(SUBSTRING "${text}" ${first} ${count} body)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${name}-under-test.inc" "${body}")
    endfunction()
    xm6_test_body(driver/oa2dp/l2cap.c
        "static NTSTATUS CloseChannelGeneration(OA2DP_DEVICE_CONTEXT *dev,\n                                       OA2DP_CHANNEL_KIND kind, ULONG expectedGeneration,"
        "\nNTSTATUS Oa2dpCloseChannel(" channel-close)
    add_executable(test_channel_close tools/test-channel-close.cpp)
    target_include_directories(test_channel_close PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME channel_close COMMAND test_channel_close)
    xm6_test_body(driver/oa2dp/l2cap.c "static EVT_WDF_REQUEST_COMPLETION_ROUTINE OpenDone;"
        "\nstatic NTSTATUS CloseChannelGeneration(OA2DP_DEVICE_CONTEXT *dev,\n                                       OA2DP_CHANNEL_KIND kind, ULONG expectedGeneration," channel-open)
    xm6_test_body(driver/oa2dp/l2cap.c "static void FinishResponseSubmission("
        "\nstatic void IndicationCallback(_In_ PVOID Context, _In_ INDICATION_CODE Indication,\n                               _In_ PINDICATION_PARAMETERS Params)\n{" channel-incoming)
    add_executable(test_channel_open tools/test-channel-open.cpp)
    target_include_directories(test_channel_open PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME channel_open COMMAND test_channel_open)
    xm6_test_body(driver/oa2dp/mpm.c "static BOOLEAN AcquireManager("
        "\nstatic NTSTATUS NTAPI SuspendStatus(" mpm-callbacks)
    xm6_test_body(driver/oa2dp/mpm.c "NTSTATUS Oa2dpMpmRequestConnection("
        "\nNTSTATUS Oa2dpMpmConnected(" mpm-request)
    xm6_test_body(driver/oa2dp/mpm.c "NTSTATUS Oa2dpMpmConnected("
        "\nVOID Oa2dpMpmConnectFailed(" mpm-connection)
    xm6_test_body(driver/oa2dp/mpm.c "static NTSTATUS PublishSuspended("
        "\nNTSTATUS Oa2dpMpmSuspended(" mpm-suspend)
    xm6_test_body(driver/oa2dp/mpm.c "VOID Oa2dpMpmAcknowledgeSuspend(" "" mpm-ack-suspend)
    xm6_test_body(driver/oa2dp/volume.c "static BOOLEAN ConnectionReady("
        "\n/* PublicationLock serializes" publication-validation)
    xm6_test_body(driver/oa2dp/volume.c "static NTSTATUS PublishConnectedLocked("
        "\nNTSTATUS Oa2dpInitializePublication(" publication-work)
    xm6_test_body(driver/oa2dp/volume.c "static NTSTATUS BusRegisterAudio("
        "\nNTSTATUS Oa2dpRegisterBusInterface(" policy-registration)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static VOID CsqCompleteCanceled("
        "\nstatic VOID SnapshotStateLocked(" wait-cancel)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static VOID SnapshotStateLocked("
        "\nstatic VOID FailWaits(" wait-snapshot)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static VOID FailWaits("
        "\n/* Called by the WaveRT stream" wait-fail)
    xm6_test_body(driver/oa2dpaudio/control.cpp "extern \"C\" NTSTATUS Oa2dpControlStatusLocked("
        "\nextern \"C\" NTSTATUS Oa2dpControlStatus(" wait-status)
    xm6_test_body(driver/oa2dpaudio/control.cpp "extern \"C\" NTSTATUS Oa2dpApplyPolicy("
        "\nextern \"C\" VOID Oa2dpControlInit(" policy-apply)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static NTSTATUS WaitFormat("
        "\nstatic NTSTATUS ControlIoctl(" wait-format)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static VOID ControlCleanup("
        "\nstatic NTSTATUS SetFormats(" wait-cleanup)
    add_executable(test_mpm_publication tools/test-mpm-publication.cpp)
    target_include_directories(test_mpm_publication PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME mpm_publication COMMAND test_mpm_publication)
    xm6_test_body(driver/oa2dpaudio/adapter.cpp "NTSTATUS\nPnpHandler\n(" "" audio-pnp)
    xm6_test_body(driver/oa2dpaudio/mintopo.cpp "extern \"C\" NTSTATUS Oa2dpSetConnected(" "" audio-connection)
    add_executable(test_audio_pnp tools/test-audio-pnp.cpp)
    target_include_directories(test_audio_pnp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME audio_pnp COMMAND test_audio_pnp)
    xm6_test_body(driver/oa2dpaudio/adapter.cpp "extern \"C\" NTSTATUS Oa2dpPublishNameLocked("
        "\n//=============================================================================\n#pragma code_seg(\"PAGE\")\nstatic NTSTATUS\nInstallSubdevice" endpoint-name-publication)
    xm6_test_body(driver/oa2dpaudio/control.cpp "static NTSTATUS SetName("
        "\nstatic NTSTATUS WaitFormat(" endpoint-name-update)
    xm6_test_body(driver/oa2dpaudio/adapter.cpp "    // Restore metadata before PortCls exposes the filters."
        "\n    if (dev->WavePort != NULL)" endpoint-name-startup)
    add_executable(test_endpoint_names tools/test-endpoint-names.cpp)
    target_include_directories(test_endpoint_names PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME endpoint_names COMMAND test_endpoint_names)
    xm6_test_body(driver/oa2dpaudio/adapter.cpp "NTSTATUS\nStartDevice\n("
        "\n//=============================================================================\n#pragma code_seg(\"PAGE\")\nstatic VOID\nStopDevice" endpoint-start-device)
    xm6_test_body(driver/oa2dpaudio/adapter.cpp "extern \"C\" NTSTATUS Oa2dpEnsureNamedFilters("
        "\n//=============================================================================\n#pragma code_seg()\nstatic NTSTATUS BusQueryComplete" endpoint-ensure-named)
    xm6_test_body(service/src/session.cpp "bool Session::ensure_audio_endpoint("
        "\nbool Session::setup_audio_endpoint(" endpoint-name-admission)
    add_executable(test_endpoint_startup tools/test-endpoint-startup.cpp)
    target_include_directories(test_endpoint_startup PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME endpoint_startup COMMAND test_endpoint_startup)
    xm6_test_body(tools/common/manage.cpp "bool pick_driver("
        "\n}  // namespace\n\nbool driver_available(" driver-selection)
    add_executable(test_driver_selection tools/test-driver-selection.cpp)
    target_include_directories(test_driver_selection PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME driver_selection COMMAND test_driver_selection)
    xm6_test_body(driver/oa2dp/driver.c "static VOID CompleteEvent(" "\nstatic NTSTATUS Oa2dpChildEnumerated(" transport-events)
    add_executable(test_transport_events tools/test-transport-events.cpp)
    target_include_directories(test_transport_events PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME transport_events COMMAND test_transport_events)
    xm6_test_body(codecs/src/enc_aac_mf.cpp "template <class T> void release("
        "\nstruct AacState" aac-backend)
    xm6_test_body(codecs/src/enc_aac_mf.cpp "extern \"C\" bool oa2dp_aac_mf_available("
        "\nextern \"C\" int oa2dp_aac_mf_encoder_create(" aac-availability)
    xm6_test_body(codecs/src/encoder.c "bool oa2dp_encoder_available("
        "\nint oa2dp_encoder_create(" encoder-availability)
    xm6_test_body(service/src/session.cpp "oa2dp_avdtp_caps make_config_caps("
        "\nstd::string codec_label(" codec-capabilities)
    xm6_test_body(service/src/session.cpp "    for (uint8_t i = 0; i < settings_.priority_count; i++) {"
        "\n    if (sep_codec_.empty())" codec-advertisement)
    xm6_test_body(codecs/src/enc_aac_mf.cpp "HRESULT set_types("
        "\n// Pull every available output sample" aac-media-type)
    add_executable(test_aac_backend tools/test-aac-backend.cpp)
    target_include_directories(test_aac_backend PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" codecs/include core/include)
    target_link_libraries(test_aac_backend PRIVATE oa2dp_core)
    add_test(NAME aac_backend COMMAND test_aac_backend)
    xm6_test_body(codecs/src/enc_aac_mf.cpp "template <class T> void release("
        "\nstruct AacBackend" aac-release)
    xm6_test_body(codecs/src/enc_aac_mf.cpp "HRESULT drain("
        "\nint aac_encode(" aac-output)
    add_executable(test_aac_output tools/test-aac-output.cpp)
    target_include_directories(test_aac_output PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME aac_output COMMAND test_aac_output)
    xm6_test_body(driver/oa2dpaudio/minwavertstream.cpp "static ULONGLONG HnsToFrames("
        "\n//=============================================================================\n#pragma code_seg(\"PAGE\")\nCMiniportWaveRTStream::~" wavert-time)
    xm6_test_body(driver/oa2dpaudio/minwavertstream.cpp "ULONGLONG CMiniportWaveRTStream::RunningHns("
        "\n//=============================================================================\n#pragma code_seg()\nSTDMETHODIMP_(NTSTATUS)\nCMiniportWaveRTStream::GetPosition\n(" wavert-clock)
    xm6_test_body(driver/oa2dpaudio/minwavertstream.cpp "STDMETHODIMP_(NTSTATUS)\nCMiniportWaveRTStream::GetPosition\n("
        "\n//=============================================================================\n#pragma code_seg(\"PAGE\")\nSTDMETHODIMP_(NTSTATUS)\nCMiniportWaveRTStream::SetFormat" wavert-methods)
    xm6_test_body(driver/oa2dpaudio/minwavertstream.cpp "STDMETHODIMP_(VOID)\nCMiniportWaveRTStream::FreeAudioBuffer"
        "\n//=============================================================================\n#pragma code_seg()\nSTDMETHODIMP_(NTSTATUS)\nCMiniportWaveRTStream::AllocateAudioBuffer" wavert-free)
    xm6_test_body(driver/oa2dpaudio/minwavertstream.cpp "void\nTimerNotifyRT\n(" "" wavert-timer)
    add_executable(test_wavert_clock tools/test-wavert-clock.cpp)
    target_include_directories(test_wavert_clock PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" core/include)
    add_test(NAME wavert_clock COMMAND test_wavert_clock)
    xm6_test_body(service/src/session.cpp "// Prepare codec PCM: channel layout before final precision."
        "\n        size_t len = 0;" pcm-preparation)
    xm6_test_body(codecs/src/encoder.c "void oa2dp_pcm_to_s16("
        "\nvoid oa2dp_pcm_to_s24_packed(" pcm-adapter-s16)
    add_executable(test_pcm_preparation tools/test-pcm-preparation.cpp)
    target_include_directories(test_pcm_preparation PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_pcm_preparation PRIVATE oa2dp_core)
    add_test(NAME pcm_preparation COMMAND test_pcm_preparation)
    add_executable(test_pcm_quantization tools/test-pcm-quantization.cpp)
    target_link_libraries(test_pcm_quantization PRIVATE oa2dp_core)
    add_test(NAME pcm_quantization COMMAND test_pcm_quantization)
    xm6_test_body(driver/oa2dpaudio/ring.c "NTSTATUS Oa2dpRingSetDataEvent(" "" pcm-notification)
    xm6_test_body(driver/oa2dpaudio/control.cpp "extern \"C\" VOID Oa2dpReportStream("
        "\n/* ---- lifetime" pcm-report)
    add_executable(test_pcm_notification tools/test-pcm-notification.cpp)
    target_include_directories(test_pcm_notification PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_pcm_notification PRIVATE oa2dp_core)
    add_test(NAME pcm_notification COMMAND test_pcm_notification)
    xm6_test_body(service/src/session.cpp "    const uint32_t spc = info.samples_per_call;"
        "\n    HANDLE pcm_waits[]" encoder-admission)
    add_executable(test_stream_start tools/test-stream-start.cpp)
    target_include_directories(test_stream_start PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_stream_start PRIVATE oa2dp_core)
    add_test(NAME stream_start COMMAND test_stream_start)
    add_executable(test_short_clips tools/test-short-clips.cpp)
    target_link_libraries(test_short_clips PRIVATE oa2dp_core)
    add_test(NAME short_clips COMMAND test_short_clips)
    xm6_test_body(service/src/session.cpp "    case Cmd::AudioFormat:"
        "\n    case Cmd::AudioUnavailable:" clip-lifecycle)
    add_executable(test_clip_lifecycle tools/test-clip-lifecycle.cpp)
    target_include_directories(test_clip_lifecycle PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_clip_lifecycle PRIVATE oa2dp_core)
    add_test(NAME clip_lifecycle COMMAND test_clip_lifecycle)
    xm6_test_body(service/src/driver_io.cpp "bool Transport::finish_slot(" "\nbool Transport::send_media(" media-finish)
    xm6_test_body(service/src/driver_io.cpp "bool Transport::send_media(" "\nvoid Transport::cancel_media(" media-send)
    xm6_test_body(service/src/driver_io.cpp "bool Transport::drain_media(" "\n// --------------------------------------------------------------------------\n\nbool AudioEndpoint::open(" media-drain)
    add_executable(test_media_wait tools/test-media-wait.cpp)
    target_include_directories(test_media_wait PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_wait COMMAND test_media_wait)
    xm6_test_body(service/src/session.cpp "void Session::teardown(" "\nvoid Session::refresh_transport_power(" disconnect-status)
    xm6_test_body(service/src/session.cpp "    case Cmd::MediaClosed:"
        "\n    case Cmd::StreamFailed:" media-closed)
    add_executable(test_disconnect_status tools/test-disconnect-status.cpp)
    target_include_directories(test_disconnect_status PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME disconnect_status COMMAND test_disconnect_status)
    xm6_test_body(service/src/session.cpp "void Session::refresh_transport_power("
        "\nvoid Session::handle(" power-refresh)
    add_executable(test_power_refresh tools/test-power-refresh.cpp)
    target_include_directories(test_power_refresh PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME power_refresh COMMAND test_power_refresh)
    xm6_test_body(service/src/session.cpp "bool Session::start_stream("
        "\nvoid Session::suspend_stream(" stream-start)
    xm6_test_body(service/src/session.cpp "void Session::begin_streaming("
        "\nvoid Session::stream_loop(" stream-admission)
    xm6_test_body(driver/oa2dp/mpm.c "static BOOLEAN SuspendStatusCurrent("
        "\nstatic NTSTATUS NTAPI ConnectionRequest(" suspend-epoch-admission)
    xm6_test_body(service/src/session.cpp "bool Session::update_latency("
        "\nbool Session::update_suspend_state(" latency-publication)
    xm6_test_body(service/src/session.cpp "bool Session::request("
        "\nvoid Session::on_avdtp_event(" signaling-request)
    xm6_test_body(service/src/session.cpp "    publication_pending_ = false; // an old FALSE must not follow this new link"
        "\n    return true;\n}\n\nbool Session::start_stream(" endpoint-ready)
    add_executable(test_profile_retry tools/test-profile-retry.cpp)
    target_include_directories(test_profile_retry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_profile_retry PRIVATE oa2dp_core Threads::Threads)
    add_test(NAME profile_retry COMMAND test_profile_retry)
    xm6_test_body(service/src/driver_io.cpp "bool DriverHandle::open("
        "\nDWORD DriverHandle::ioctl(" handle-lifetime)
    add_executable(test_handle_lifetime tools/test-handle-lifetime.cpp)
    target_include_directories(test_handle_lifetime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_handle_lifetime PRIVATE Threads::Threads)
    add_test(NAME handle_lifetime COMMAND test_handle_lifetime)
    set_tests_properties(handle_lifetime PROPERTIES TIMEOUT 15)
    xm6_test_body(service/src/driver_io.cpp "DWORD DriverHandle::ioctl("
        "\nbool Transport::open(" control-ioctl)
    xm6_test_body(service/src/driver_io.cpp "bool Transport::open_channel("
        "\nbool Transport::close_channel(" control-open)
    xm6_test_body(service/src/driver_io.cpp "bool Transport::send("
        "\nint Transport::recv(" control-send)
    xm6_test_body(service/src/session.cpp "bool Session::connect_signaling("
        "\nbool Session::configure_as_initiator(" control-connect)
    xm6_test_body(service/src/session.cpp "int Session::cb_send("
        "\nuint32_t Session::cb_now(" control-protocol-send)
    xm6_test_body(service/src/session.cpp "void Session::stop("
        "\n// ---------------------------------------------------------------------------\n// Helper threads" control-policy)
    xm6_test_body(service/src/session.cpp "bool Session::pump("
        "\nbool Session::request(" control-pump)
    add_executable(test_control_cancel tools/test-control-cancel.cpp)
    target_include_directories(test_control_cancel PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME control_cancel COMMAND test_control_cancel)
    xm6_test_body(service/src/main.cpp "std::wstring lower("
        "\nDeviceManager g_manager;" device-discovery)
    add_executable(test_device_discovery tools/test-device-discovery.cpp)
    target_include_directories(test_device_discovery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_device_discovery PRIVATE Threads::Threads)
    add_test(NAME device_discovery COMMAND test_device_discovery)
    set_tests_properties(device_discovery PROPERTIES TIMEOUT 15)
    xm6_test_body(service/src/session.cpp "bool Session::setup_audio_endpoint("
        "\nbool Session::start_stream(" endpoint-formats)
    xm6_test_body(service/src/session.cpp "            oa2dp_codec_caps cfg{};"
        "\n            oa2dp_avdtp_caps avc = make_config_caps(cfg, c.delay_reporting);" codec-initial-selection)
    add_executable(test_endpoint_formats tools/test-endpoint-formats.cpp)
    target_include_directories(test_endpoint_formats PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_endpoint_formats PRIVATE oa2dp_core)
    add_test(NAME endpoint_formats COMMAND test_endpoint_formats)
    xm6_test_body(service/src/session.cpp "void Session::on_avdtp_event("
        "\n// ---------------------------------------------------------------------------\n// Procedures" peer-events)
    xm6_test_body(service/src/session.cpp "void Session::reconcile_peer_configuration("
        "\nvoid Session::teardown(" peer-reconciliation)
    xm6_test_body(service/src/session.cpp "bool Session::start_stream("
        "\nvoid Session::reconcile_peer_configuration(" peer-stream-procedures)
    add_executable(test_peer_format tools/test-peer-format.cpp)
    target_include_directories(test_peer_format PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_peer_format PRIVATE oa2dp_core)
    add_test(NAME peer_format COMMAND test_peer_format)
    xm6_test_body(service/src/session.cpp "        int r = oa2dp_rtp_add_frames("
        "\n        DWORD now = GetTickCount();" service-rtp)
    xm6_test_body(service/src/session.cpp "        if (read_status == OA2DP_ERR_BUSY ||"
        "\n        if (read_status != OA2DP_OK || !block_ready || read_frames > spc)" service-pcm-wait)
    add_executable(test_service_rtp tools/test-service-rtp.cpp)
    target_include_directories(test_service_rtp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_service_rtp PRIVATE oa2dp_core)
    add_test(NAME service_rtp COMMAND test_service_rtp)
endif()

if(NOT WIN32)
    return()  # everything below is Windows-only
endif()

add_test(NAME endpoint_inf_metadata COMMAND powershell.exe -NoProfile -File
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/endpoint-metadata.ps1")

# --- codec adapters ---------------------------------------------------------
add_library(oa2dp_codecs STATIC codecs/src/encoder.c codecs/src/enc_sbc.c
    codecs/src/enc_aptx.c codecs/src/enc_ldac.c codecs/src/enc_aac_mf.cpp)
target_include_directories(oa2dp_codecs PUBLIC codecs/include)
target_link_libraries(oa2dp_codecs PUBLIC oa2dp_core)

# AAC: Media Foundation, part of Windows.
target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_MFAAC)
target_link_libraries(oa2dp_codecs PUBLIC mfplat mfuuid ole32)
if(OA2DP_BUILD_TESTS)
    add_executable(test_aac_media_type tools/test-aac-media-type.cpp)
    target_include_directories(test_aac_media_type PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_aac_media_type PRIVATE oa2dp_codecs)
    add_test(NAME aac_media_type COMMAND test_aac_media_type)
    set_tests_properties(aac_media_type PROPERTIES SKIP_RETURN_CODE 77)
endif()

# SBC + FastStream: libsbc (LGPL-2.1+), built as a DLL to keep it replaceable.
if(EXISTS ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc.c)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sbc-config/config.h "#include <stdint.h>\n")
    add_library(sbc SHARED ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc.c
        ${OA2DP_THIRD_PARTY}/sbc/sbc/sbc_primitives.c)
    target_include_directories(sbc PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sbc-config
        PUBLIC ${OA2DP_THIRD_PARTY}/sbc)
    set_target_properties(sbc PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(MSVC)  # libsbc's public header uses POSIX ssize_t
        target_compile_definitions(sbc PUBLIC ssize_t=intptr_t)
        target_compile_definitions(sbc PRIVATE __BYTE_ORDER=1234 __LITTLE_ENDIAN=1234 __BIG_ENDIAN=4321)
        target_compile_options(sbc PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/codecs/msvc-compat.h)
    endif()
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_LIBSBC)
    target_link_libraries(oa2dp_codecs PUBLIC sbc)
endif()

# aptX / aptX HD / aptX LL: libopenaptx (GPL-3.0+, see THIRD_PARTY.md).
if(EXISTS ${OA2DP_THIRD_PARTY}/libopenaptx/openaptx.c)
    add_library(openaptx SHARED ${OA2DP_THIRD_PARTY}/libopenaptx/openaptx.c)
    target_include_directories(openaptx PUBLIC ${OA2DP_THIRD_PARTY}/libopenaptx)
    set_target_properties(openaptx PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_OPENAPTX)
    target_link_libraries(oa2dp_codecs PUBLIC openaptx)
endif()

# LDAC + adaptive bitrate: AOSP libldac (Apache-2.0).
if(EXISTS ${OA2DP_THIRD_PARTY}/libldac/src/ldacBT.c)
    add_library(ldac SHARED ${OA2DP_THIRD_PARTY}/libldac/src/ldaclib.c
        ${OA2DP_THIRD_PARTY}/libldac/src/ldacBT.c
        ${OA2DP_THIRD_PARTY}/libldac/abr/src/ldacBT_abr.c)
    target_include_directories(ldac PUBLIC ${OA2DP_THIRD_PARTY}/libldac/inc
        ${OA2DP_THIRD_PARTY}/libldac/abr/inc)
    set_target_properties(ldac PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    if(MSVC)
        target_compile_options(ldac PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/codecs/msvc-compat.h)
        target_include_directories(ldac PRIVATE codecs/msvc)
    endif()
    target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_LIBLDAC OA2DP_WITH_LDAC_ABR)
    target_link_libraries(oa2dp_codecs PUBLIC ldac)
elseif(XM6_REQUIRE_LDAC)
    message(FATAL_ERROR "LDAC is required. Run git submodule update --init --recursive first.")
endif()

if(OA2DP_BUILD_TESTS)
    xm6_test_body(service/src/session.cpp "void Session::stream_loop("
        "\n}  // namespace oa2dp" stream-loop)
    add_executable(test_stream_loop tools/test-stream-loop.cpp)
    target_include_directories(test_stream_loop PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(test_stream_loop PRIVATE oa2dp_codecs)
    add_test(NAME stream_loop COMMAND test_stream_loop)
    if(TARGET ldac)
        target_compile_definitions(test_service_rtp PRIVATE XM6_TEST_WITH_CODECS)
        target_link_libraries(test_service_rtp PRIVATE oa2dp_codecs)
    endif()
    add_executable(test_encoders codecs/tests/test_encoders.c)
    target_link_libraries(test_encoders PRIVATE oa2dp_codecs)
    add_test(NAME encoders COMMAND test_encoders)
    add_executable(verify_native_metadata tools/verify-native-metadata.cpp)
    target_link_libraries(verify_native_metadata PRIVATE dbghelp)
endif()

# --- service ----------------------------------------------------------------
add_executable(oa2dpsvc
    ${XM6_VERSION_RESOURCE}
    service/src/main.cpp service/src/session.cpp service/src/driver_io.cpp
    service/src/settings_store.cpp service/src/log.cpp)
target_include_directories(oa2dpsvc PRIVATE driver/common)
target_compile_definitions(oa2dpsvc PRIVATE UNICODE _UNICODE)
target_link_libraries(oa2dpsvc PRIVATE oa2dp_codecs cfgmgr32 bthprops shell32 advapi32 avrt)

# --- tools: shared device/settings logic, command line, graphical manager ------
add_library(oa2dp_manage STATIC tools/common/manage.cpp)
target_include_directories(oa2dp_manage PUBLIC tools/common)
target_compile_definitions(oa2dp_manage PUBLIC UNICODE _UNICODE)
target_link_libraries(oa2dp_manage PUBLIC oa2dp_core setupapi bthprops advapi32 cfgmgr32)

add_executable(oa2dpctl tools/oa2dpctl/oa2dpctl.cpp ${XM6_VERSION_RESOURCE})
target_link_libraries(oa2dpctl PRIVATE oa2dp_manage)

if(XM6_BUILD_GUI)
add_executable(oa2dpgui WIN32 tools/oa2dpgui/oa2dpgui.cpp tools/oa2dpgui/oa2dpgui.rc)
target_link_libraries(oa2dpgui PRIVATE oa2dp_manage comctl32 shell32 user32 gdi32)
if(MSVC)
    # The .rc already embeds tools/oa2dpgui/oa2dpgui.manifest.
    target_link_options(oa2dpgui PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_link_options(oa2dpgui PRIVATE -municode)
endif()
endif()

# --- drivers ------------------------------------------------------------------
if(OA2DP_BUILD_TESTS)
    add_executable(test_ks_attributes tools/test-ks-attributes.cpp)
    add_test(NAME ks_attributes COMMAND test_ks_attributes)
endif()
if(MSVC)
    # User-mode payload is self-contained; do not require a separate VC runtime
    # installer. Kernel targets keep the WDK's independent runtime settings.
    foreach(binary IN ITEMS oa2dp_core oa2dp_codecs oa2dp_manage oa2dpsvc oa2dpctl
                            oa2dpgui ldac sbc oa2dp_tests test_encoders verify_native_metadata test_ks_attributes
                            test_ring_concurrency test_profile_retry test_handle_lifetime test_endpoint_formats
                            test_service_rtp test_peer_format test_aac_media_type test_aac_backend test_pcm_quantization
                            test_short_clips test_clip_lifecycle test_stream_start test_stream_loop)
        if(TARGET ${binary})
            set_property(TARGET ${binary} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endif()

if(OA2DP_BUILD_DRIVERS)
    list(APPEND CMAKE_MODULE_PATH ${OA2DP_THIRD_PARTY}/FindWDK/cmake)
    find_package(WDK REQUIRED)
    # Windows remains the implementation of this observed profile-manager ABI.
    set(XM6_MPM_MACHINE X64)
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        set(XM6_MPM_MACHINE ARM64)
    endif()
    set(XM6_MPM_IMPORT "${CMAKE_CURRENT_BINARY_DIR}/btampm.lib")
    add_custom_command(OUTPUT "${XM6_MPM_IMPORT}"
        COMMAND "${CMAKE_AR}" /NOLOGO /MACHINE:${XM6_MPM_MACHINE}
            "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/driver/oa2dp/btampm.def"
            "/OUT:${XM6_MPM_IMPORT}"
        DEPENDS driver/oa2dp/btampm.def VERBATIM)
    add_custom_target(xm6_mpm_import DEPENDS "${XM6_MPM_IMPORT}")
    wdk_add_driver(oa2dp KMDF 1.15 WINVER 0x0A00 NTDDI_VERSION 0x0A000008
        driver/oa2dp/driver.c driver/oa2dp/l2cap.c driver/oa2dp/volume.c
        driver/oa2dp/mpm.c driver/oa2dp/link.c ${XM6_VERSION_RESOURCE})
    add_dependencies(oa2dp xm6_mpm_import)
    target_link_libraries(oa2dp "${XM6_MPM_IMPORT}")

    # Virtual render endpoint: PortCls/WaveRT, derived from SYSVAD (MS-PL).
    # NTDDI 10.0.19041 for ExAllocatePool2; matches the INF decoration.
    wdk_add_driver(oa2dpaudio WINVER 0x0A00 NTDDI_VERSION 0x0A000008
        ${XM6_VERSION_RESOURCE}
        driver/oa2dpaudio/adapter.cpp
        driver/oa2dpaudio/control.cpp
        driver/oa2dpaudio/minwavert.cpp
        driver/oa2dpaudio/minwavertstream.cpp
        driver/oa2dpaudio/mintopo.cpp
        driver/oa2dpaudio/newdelete.cpp
        driver/oa2dpaudio/ring.c
        core/src/pcm_format.c
        core/src/pcm_ring.c)
    target_include_directories(oa2dpaudio PRIVATE core/include driver/common)
    # _NEW_DELETE_OPERATORS_: we supply new/delete (newdelete.cpp), so the
    # WDK's stdunk.h must not define its own (as in SYSVAD).
    target_compile_definitions(oa2dpaudio PRIVATE PC_IMPLEMENTATION UNICODE _UNICODE _KERNEL_MODE
        _NEW_DELETE_OPERATORS_)
    target_link_libraries(oa2dpaudio WDK::PORTCLS WDK::STDUNK WDK::LIBCNTPR)
    foreach(driver IN ITEMS oa2dp oa2dpaudio)
        target_compile_definitions(${driver} PRIVATE XM6_KERNEL_BINARY)
        target_compile_options(${driver} PRIVATE /Zi)
        target_link_options(${driver} PRIVATE /DEBUG)
    endforeach()
    if(OA2DP_BUILD_TESTS)
        add_test(NAME native_metadata COMMAND verify_native_metadata "$<TARGET_FILE:oa2dpaudio>")
    endif()
endif()
