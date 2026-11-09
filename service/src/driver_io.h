// SPDX-License-Identifier: MIT
// User-mode side of the IOCTL contract in driver/common/oa2dp_ioctl.h.
#pragma once

#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "oa2dp_ioctl.h"
#include "oa2dp/pcm_ring.h"

namespace oa2dp {

std::vector<std::wstring> enumerate_interfaces(const GUID &guid);

// Overlapped handle with a synchronous-with-timeout IOCTL helper.
class DriverHandle {
public:
    DriverHandle() = default;
    ~DriverHandle() { close(); }
    DriverHandle(const DriverHandle &) = delete;
    DriverHandle &operator=(const DriverHandle &) = delete;

    bool open(const std::wstring &path);
    void close();
    bool is_open() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h_; }
    void cancel_all();

    // Returns ERROR_SUCCESS, WAIT_TIMEOUT, ERROR_CANCELLED or a Win32 error.
    DWORD ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len,
                DWORD *bytes, DWORD timeout_ms = INFINITE, HANDLE cancel = nullptr);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

class Transport {
public:
    ~Transport() { close(); }
    bool open(const std::wstring &path);
    void close();
    void cancel_all() { dev_.cancel_all(); }

    bool get_info(OA2DP_DEVICE_INFO &out);
    bool open_channel(OA2DP_CHANNEL_KIND kind, OA2DP_CHANNEL_INFO &out, ULONG connection = 0);
    bool close_channel(OA2DP_CHANNEL_KIND kind, ULONG connection = 0,
                        ULONG *closed_connection = nullptr);
    bool send(const OA2DP_SEND_IN &binding, const uint8_t *data, size_t len);
    // > 0 bytes, 0 timeout, < 0 channel closed / error
    int recv(const OA2DP_RECV_IN &binding, uint8_t *buf, size_t cap, DWORD timeout_ms,
             HANDLE cancel = nullptr);
    bool wait_event(OA2DP_EVENT &ev, HANDLE cancel);
    HANDLE signaling_event() const { return receive_.event; }
    bool tx_state(OA2DP_TX_STATE &out);

    // Media: up to kSlots sends in flight; blocks when all are busy.
    bool send_media(const OA2DP_SEND_IN &binding, const uint8_t *data, size_t len);
    void drain_media();

private:
    static constexpr size_t kSlots = 8;
    struct Slot {
        OVERLAPPED ov{};
        HANDLE event = nullptr;
        std::vector<uint8_t> data;
        bool busy = false;
        OA2DP_SEND_IN binding{}; // stable for the entire overlapped request
    };
    bool finish_slot(Slot &s, DWORD timeout_ms);
    void cancel_receive();

    DriverHandle dev_;
    std::array<Slot, kSlots> slots_{};
    Slot receive_{};  // one persistent overlapped signalling receive
    size_t next_slot_ = 0;
};

class AudioEndpoint {
public:
    ~AudioEndpoint() { close(); }
    bool open(uint64_t bt_addr);
    void close();
    bool set_formats(const OA2DP_FORMAT_LIST &list);
    bool set_latency(uint32_t transport_us, uint32_t device_us);
    bool set_name(const std::wstring &name);
    bool set_connected(bool connected, ULONG connection = 0);
    bool set_suspended(bool suspended, ULONG request_serial, ULONG connection);
    bool get_state(OA2DP_FORMAT_CHANGE &out);
    bool map_ring();
    void unmap_ring();
    oa2dp_pcm_ring *ring() const { return ring_; }
    // ERROR_SUCCESS with a new format; preserve the native cancel/device error.
    DWORD wait_format(OA2DP_FORMAT_CHANGE &out, HANDLE cancel);
    void cancel_all() { dev_.cancel_all(); }

private:
    DriverHandle dev_;
    oa2dp_pcm_ring *ring_ = nullptr;
};

}  // namespace oa2dp
