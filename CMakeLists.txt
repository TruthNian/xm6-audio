cmake_minimum_required(VERSION 3.21)
project(XM6Audio VERSION 0.3.0 LANGUAGES C CXX)

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
endif()

if(NOT WIN32)
    return()  # everything below is Windows-only
endif()

# --- codec adapters ---------------------------------------------------------
add_library(oa2dp_codecs STATIC codecs/src/encoder.c codecs/src/enc_sbc.c
    codecs/src/enc_aptx.c codecs/src/enc_ldac.c codecs/src/enc_aac_mf.cpp)
target_include_directories(oa2dp_codecs PUBLIC codecs/include)
target_link_libraries(oa2dp_codecs PUBLIC oa2dp_core)

# AAC: Media Foundation, part of Windows.
target_compile_definitions(oa2dp_codecs PUBLIC OA2DP_WITH_MFAAC)
target_link_libraries(oa2dp_codecs PUBLIC mfplat mfuuid ole32)

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
    add_executable(test_encoders codecs/tests/test_encoders.c)
    target_link_libraries(test_encoders PRIVATE oa2dp_codecs)
    add_test(NAME encoders COMMAND test_encoders)
endif()

# --- service ----------------------------------------------------------------
add_executable(oa2dpsvc
    native-version.rc
    service/src/main.cpp service/src/session.cpp service/src/driver_io.cpp
    service/src/settings_store.cpp service/src/log.cpp)
target_include_directories(oa2dpsvc PRIVATE driver/common)
target_compile_definitions(oa2dpsvc PRIVATE UNICODE _UNICODE)
target_link_libraries(oa2dpsvc PRIVATE oa2dp_codecs cfgmgr32 bthprops shell32 advapi32 avrt)

# --- tools: shared device/settings logic, command line, graphical manager ------
add_library(oa2dp_manage STATIC tools/common/manage.cpp)
target_include_directories(oa2dp_manage PUBLIC tools/common)
target_compile_definitions(oa2dp_manage PUBLIC UNICODE _UNICODE)
target_link_libraries(oa2dp_manage PUBLIC oa2dp_core setupapi bthprops advapi32)

add_executable(oa2dpctl tools/oa2dpctl/oa2dpctl.cpp native-version.rc)
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
if(MSVC)
    # User-mode payload is self-contained; do not require a separate VC runtime
    # installer. Kernel targets keep the WDK's independent runtime settings.
    foreach(binary IN ITEMS oa2dp_core oa2dp_codecs oa2dp_manage oa2dpsvc oa2dpctl
                            oa2dpgui ldac sbc oa2dp_tests test_encoders)
        if(TARGET ${binary})
            set_property(TARGET ${binary} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endforeach()
endif()

if(OA2DP_BUILD_DRIVERS)
    list(APPEND CMAKE_MODULE_PATH ${OA2DP_THIRD_PARTY}/FindWDK/cmake)
    find_package(WDK REQUIRED)
    wdk_add_driver(oa2dp KMDF 1.15 driver/oa2dp/driver.c driver/oa2dp/l2cap.c native-version.rc)

    # Virtual render endpoint: PortCls/WaveRT, derived from SYSVAD (MS-PL).
    # NTDDI 10.0.19041 for ExAllocatePool2; matches the INF decoration.
    wdk_add_driver(oa2dpaudio WINVER 0x0A00 NTDDI_VERSION 0x0A000008
        native-version.rc
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
    target_compile_definitions(oa2dpaudio PRIVATE PC_IMPLEMENTATION UNICODE _UNICODE
        _NEW_DELETE_OPERATORS_)
    target_link_libraries(oa2dpaudio WDK::PORTCLS WDK::STDUNK WDK::LIBCNTPR)
    foreach(driver IN ITEMS oa2dp oa2dpaudio)
        target_compile_definitions(${driver} PRIVATE XM6_KERNEL_BINARY)
        target_compile_options(${driver} PRIVATE /Zi)
        target_link_options(${driver} PRIVATE /DEBUG)
    endforeach()
endif()
