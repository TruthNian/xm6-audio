// SPDX-License-Identifier: MIT
#include <windows.h>
#include <winioctl.h>
// After the system headers, so only the GUIDs from oa2dp_ioctl.h are
// instantiated in this translation unit.
#include <initguid.h>

#include "driver_io.h"

#include <cfgmgr32.h>

#include <algorithm>
#include <cwctype>

#include "log.h"
#include "settings_store.h"

namespace oa2dp {

std::vector<std::wstring> enumerate_interfaces(const GUID &guid)
{
    std::vector<std::wstring> result;
    for (int attempt = 0; attempt < 3; attempt++) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID *>(&guid), nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS)
            return result;
        std::vector<wchar_t> buf(len + 1, L'\0');
        CONFIGRET cr = CM_Get_Device_Interface_ListW(const_cast<GUID *>(&guid), nullptr,
                                                     buf.data(), len,
                                                     CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL)
            continue;  // device arrived between the two calls
        if (cr != CR_SUCCESS)
            return result;
        for (const wchar_t *p = buf.data(); *p; p += wcslen(p) + 1)
            result.emplace_back(p);
        return result;
    }
    return result;
}

// --------------------------------------------------------------------------

bool DriverHandle::open(const std::wstring &path)
{
    std::unique_lock lock(handle_mu_);
    close_locked();
    h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                     FILE_FLAG_OVERLAPPED, nullptr);
    const bool opened = h_ != INVALID_HANDLE_VALUE;
    const DWORD error = opened ? ERROR_SUCCESS : GetLastError();
    lock.unlock();
    // The caller classifies startup/reopen errors. A C++ mutex operation has
    // no contract to preserve the thread's Win32 last-error value.
    if (!opened) SetLastError(error);
    return opened;
}

void DriverHandle::close()
{
    std::lock_guard lock(handle_mu_);
    close_locked();
}

void DriverHandle::close_locked()
{
    if (h_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(h_, nullptr);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
}

void DriverHandle::cancel_all()
{
    // Hold through CancelIoEx, not merely the h_ load: CloseHandle may retire
    // and Windows may reuse the numeric value before that cancel is issued.
    std::lock_guard lock(handle_mu_);
    if (h_ != INVALID_HANDLE_VALUE)
        CancelIoEx(h_, nullptr);
}

DWORD DriverHandle::ioctl(DWORD code, const void *in, DWORD in_len, void *out, DWORD out_len,
                          DWORD *bytes, DWORD timeout_ms, HANDLE cancel)
{
    // CancelIoEx only covers requests already submitted. The cancellation
    // event also closes the gap before this submission and participates in
    // the overlapped wait below. Cleanup calls deliberately omit cancellation.
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)
        return ERROR_CANCELLED;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return GetLastError();
    DWORD n = 0, err = ERROR_SUCCESS;
    if (!DeviceIoControl(h_, code, const_cast<void *>(in), in_len, out, out_len, nullptr, &ov)) {
        err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {ov.hEvent, cancel};
            DWORD w = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, timeout_ms);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &n, TRUE);  // wait for the cancel to land
                CloseHandle(ov.hEvent);
                return w == WAIT_TIMEOUT ? WAIT_TIMEOUT : ERROR_CANCELLED;
            }
            err = ERROR_SUCCESS;
        }
    }
    if (err == ERROR_SUCCESS && !GetOverlappedResult(h_, &ov, &n, FALSE))
        err = GetLastError();
    CloseHandle(ov.hEvent);
    if (bytes)
        *bytes = n;
    return err;
}

// --------------------------------------------------------------------------

bool Transport::open(const std::wstring &path)
{
    close();
    if (!dev_.open(path))
        return false;
    for (auto &s : slots_) {
        s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!s.event) {
            close();
            return false;
        }
    }
    receive_.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!receive_.event) {
        close();
        return false;
    }
    next_slot_ = 0;
    return true;
}

void Transport::close()
{
    dev_.cancel_all();
    cancel_receive();
    // Final object destruction cannot release buffers still owned by the
    // kernel. This safety wait is deliberately outside the realtime loop.
    drain_media(INFINITE);
    dev_.close();
    for (auto &s : slots_) {
        if (s.event)
            CloseHandle(s.event);
        s.event = nullptr;
        s.busy = false;
    }
    if (receive_.event)
        CloseHandle(receive_.event);
    receive_.event = nullptr;
}

bool Transport::get_info(OA2DP_DEVICE_INFO &out)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_GET_INFO, nullptr, 0, &out, sizeof(out), &n, 5000) ==
               ERROR_SUCCESS &&
           n == sizeof(out);
}

bool Transport::open_channel(OA2DP_CHANNEL_KIND kind, OA2DP_CHANNEL_INFO &out, ULONG connection,
                              HANDLE cancel)
{
    OA2DP_OPEN_CHANNEL_IN in{};
    in.Kind = kind;
    in.ConnectionGeneration = connection;
    DWORD n = 0;
    // L2CAP connection setup can take a few seconds (paging, auth).
    DWORD err = dev_.ioctl(IOCTL_OA2DP_OPEN_CHANNEL, &in, sizeof(in), &out, sizeof(out), &n, 15000, cancel);
    if (err != ERROR_SUCCESS)
        LOGW("open %s channel failed: %lu", kind == Oa2dpChannelMedia ? "media" : "signalling",
             err);
    return err == ERROR_SUCCESS && n == sizeof(out);
}

bool Transport::close_channel(OA2DP_CHANNEL_KIND kind, ULONG connection, ULONG *closed_connection)
{
    if (kind == Oa2dpChannelSignaling)
        cancel_receive();
    OA2DP_CLOSE_CHANNEL_IN in{};
    in.Kind = kind;
    in.ConnectionGeneration = connection;
    OA2DP_CLOSE_CHANNEL_OUT out{};
    DWORD bytes = 0;
    const bool ok = dev_.ioctl(IOCTL_OA2DP_CLOSE_CHANNEL, &in, sizeof(in),
                               &out, sizeof(out), &bytes, 15000) == ERROR_SUCCESS &&
                    bytes == sizeof(out);
    if (ok && closed_connection) *closed_connection = out.ConnectionGeneration;
    return ok;
}

DWORD Transport::wake_link(const OA2DP_SEND_IN &binding, HANDLE cancel)
{
    OA2DP_SEND_IN in = binding;
    return dev_.ioctl(IOCTL_OA2DP_WAKE_LINK, &in, sizeof(in), nullptr, 0, nullptr,
                      2000, cancel);
}

bool Transport::send(const OA2DP_SEND_IN &binding, const uint8_t *data, size_t len, HANDLE cancel)
{
    OA2DP_SEND_IN in = binding;
    return dev_.ioctl(IOCTL_OA2DP_SEND, &in, sizeof(in), const_cast<uint8_t *>(data),
                      static_cast<DWORD>(len), nullptr, 5000, cancel) == ERROR_SUCCESS;
}

int Transport::recv(const OA2DP_RECV_IN &binding, uint8_t *buf, size_t cap, DWORD timeout_ms,
                    HANDLE cancel)
{
    if (binding.Kind == Oa2dpChannelSignaling) {
        Slot &s = receive_;
        if (s.busy && (s.binding.Generation != binding.Generation ||
                       s.binding.ConnectionGeneration != binding.ConnectionGeneration))
            cancel_receive(); // never deliver an old pending read to a new AVDTP session
        if (!s.busy) {
            s.data.resize(cap);
            s.ov = OVERLAPPED{};
            s.ov.hEvent = s.event;
            ResetEvent(s.event);
            s.binding = binding;
            if (!DeviceIoControl(dev_.get(), IOCTL_OA2DP_RECV, &s.binding, sizeof(s.binding), s.data.data(),
                                 static_cast<DWORD>(cap), nullptr, &s.ov) &&
                GetLastError() != ERROR_IO_PENDING)
                return -1;
            s.busy = true;
        }
        HANDLE waits[2] = {s.event, cancel};
        DWORD wait = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, timeout_ms);
        if (wait == WAIT_TIMEOUT)
            return 0;  // keep the request and its buffer alive between pumps
        if (wait != WAIT_OBJECT_0) {
            cancel_receive();
            return -1;
        }
        DWORD bytes = 0;
        BOOL ok = GetOverlappedResult(dev_.get(), &s.ov, &bytes, FALSE);
        s.busy = false;
        if (!ok || bytes > cap)
            return -1;
        std::copy_n(s.data.data(), bytes, buf);
        return static_cast<int>(bytes);
    }
    OA2DP_RECV_IN in = binding;
    DWORD n = 0;
    DWORD err = dev_.ioctl(IOCTL_OA2DP_RECV, &in, sizeof(in), buf, static_cast<DWORD>(cap), &n,
                           timeout_ms, cancel);
    if (err == WAIT_TIMEOUT)
        return 0;
    if (err != ERROR_SUCCESS)
        return -1;
    return static_cast<int>(n);
}

void Transport::cancel_receive()
{
    if (receive_.busy && dev_.is_open()) {
        CancelIoEx(dev_.get(), &receive_.ov);
        DWORD bytes = 0;
        GetOverlappedResult(dev_.get(), &receive_.ov, &bytes, TRUE);
    }
    receive_.busy = false;
    if (receive_.event)
        ResetEvent(receive_.event);
}

bool Transport::wait_event(OA2DP_EVENT &ev, HANDLE cancel)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_WAIT_EVENT, nullptr, 0, &ev, sizeof(ev), &n, INFINITE,
                      cancel) == ERROR_SUCCESS &&
           n == sizeof(ev);
}

bool Transport::tx_state(OA2DP_TX_STATE &out)
{
    DWORD n = 0;
    return dev_.ioctl(IOCTL_OA2DP_GET_TX_STATE, nullptr, 0, &out, sizeof(out), &n, 1000) ==
               ERROR_SUCCESS &&
           n == sizeof(out);
}

bool Transport::finish_slot(Slot &s, DWORD timeout_ms, HANDLE cancel, MediaFailure *failure)
{
    if (failure) *failure = {};
    const auto fail = [failure](const char *stage, DWORD error) {
        if (failure) *failure = {stage, error};
        return false;
    };
    if (!s.busy)
        return true;
    DWORD n = 0;
    BOOL ok = GetOverlappedResult(dev_.get(), &s.ov, &n, FALSE);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_INCOMPLETE) {
        HANDLE waits[] = {cancel, s.event};
        const DWORD wake = WaitForMultipleObjects(cancel ? 2 : 1,
            cancel ? waits : waits + 1, FALSE, timeout_ms);
        if (wake != WAIT_OBJECT_0 + (cancel ? 1 : 0)) {
            // Cancellation requests completion; it does not finish ownership.
            // Keep busy/OVERLAPPED/data intact for a later bounded drain.
            if (wake == WAIT_TIMEOUT) {
                CancelIoEx(dev_.get(), &s.ov);
                return fail("wait-timeout", WAIT_TIMEOUT);
            }
            if (wake == WAIT_FAILED) return fail("wait", GetLastError());
            if (cancel && wake == WAIT_OBJECT_0) return fail("cancel", ERROR_CANCELLED);
            return fail("wait-result", wake);
        }
        ok = GetOverlappedResult(dev_.get(), &s.ov, &n, FALSE);
        error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_INCOMPLETE) return fail("completion-pending", error);
    }
    s.busy = false;
    return ok ? true : fail("completion", error);
}

bool Transport::send_media(const OA2DP_SEND_IN &binding, const uint8_t *data, size_t len, HANDLE cancel)
{
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return false;
    Slot &s = slots_[next_slot_];
    MediaFailure failure;
    if (!finish_slot(s, 1000, cancel, &failure)) {
        if (!cancel || WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0)
            LOGE("media send retirement failed: stage=%s error=%lu slot=%u pending=%u budget_ms=1000",
                 failure.stage, static_cast<unsigned long>(failure.error),
                 static_cast<unsigned>(next_slot_), s.busy ? 1u : 0u);
        return false;
    }
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return false;
    next_slot_ = (next_slot_ + 1) % kSlots;

    s.binding = binding;
    s.data.assign(data, data + len);
    s.ov = OVERLAPPED{};
    s.ov.hEvent = s.event;
    ResetEvent(s.event);
    if (!DeviceIoControl(dev_.get(), IOCTL_OA2DP_SEND, &s.binding, sizeof(s.binding), s.data.data(),
                         static_cast<DWORD>(len), nullptr, &s.ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            if (!cancel || WaitForSingleObject(cancel, 0) != WAIT_OBJECT_0)
                LOGE("media send submission failed: error=%lu bytes=%u",
                     static_cast<unsigned long>(err), static_cast<unsigned>(len));
            return false;
        }
    }
    s.busy = true;
    return true;
}

void Transport::cancel_media()
{
    for (auto &s : slots_)
        if (s.busy && dev_.is_open()) CancelIoEx(dev_.get(), &s.ov);
}

bool Transport::drain_media(DWORD timeout_ms)
{
    if (!dev_.is_open()) return true;
    const ULONGLONG start = GetTickCount64();
    bool retired = true;
    for (auto &s : slots_) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        const DWORD left = timeout_ms == INFINITE ? INFINITE :
            elapsed >= timeout_ms ? 0 : static_cast<DWORD>(timeout_ms - elapsed);
        (void)finish_slot(s, left);
        retired = retired && !s.busy;
    }
    return retired;
}

// --------------------------------------------------------------------------

bool AudioEndpoint::open(uint64_t bt_addr)
{
    close();
    std::wstring want = address_key(bt_addr);
    for (const auto &path : enumerate_interfaces(GUID_DEVINTERFACE_OA2DP_AUDIO)) {
        std::wstring upper = path;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
        if (upper.find(L"#" + want + L"#") != std::wstring::npos)
        {
            if (!dev_.open(path)) return false;
            data_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!data_event_) { dev_.close(); return false; }
            return true;
        }
    }
    return false;
}

void AudioEndpoint::close()
{
    if (dev_.is_open())
        set_connected(false);
    unmap_ring();
    dev_.close();
    if (data_event_) CloseHandle(data_event_);
    data_event_ = nullptr;
}

bool AudioEndpoint::set_data_event(ULONG minimum_frames)
{
    OA2DP_DATA_EVENT_IN value{static_cast<ULONGLONG>(reinterpret_cast<uintptr_t>(data_event_)),
                              minimum_frames, 0};
    return data_event_ && dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_DATA_EVENT, &value, sizeof(value),
        nullptr, 0, nullptr, 5000) == ERROR_SUCCESS;
}

bool AudioEndpoint::signal_consumer(ULONG pcm_sequence)
{
    OA2DP_CONSUMER_READY_IN value{pcm_sequence, 0};
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_CONSUMER_READY, &value, sizeof(value),
        nullptr, 0, nullptr, 5000) == ERROR_SUCCESS;
}

bool AudioEndpoint::set_connected(bool connected, ULONG connection)
{
    OA2DP_CONNECTED_IN value{connected ? 1UL : 0UL, connection};
    const DWORD error = dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_CONNECTED, &value, sizeof(value), nullptr, 0,
                                   nullptr, 5000);
    if (error != ERROR_SUCCESS)
        LOGW("audio connection publication (%u, epoch %lu) failed: %lu", connected, connection, error);
    return error == ERROR_SUCCESS;
}

bool AudioEndpoint::set_suspended(bool suspended, ULONG request_serial, ULONG connection)
{
    OA2DP_SUSPENDED_IN value{suspended ? 1UL : 0UL, request_serial, connection};
    const DWORD error = dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_SUSPENDED, &value, sizeof(value),
                                   nullptr, 0, nullptr, 5000);
    if (error != ERROR_SUCCESS)
        LOGW("profile suspend publication (%u, request %lu, epoch %lu) failed: %lu",
             suspended, request_serial, connection, error);
    return error == ERROR_SUCCESS;
}

bool AudioEndpoint::get_state(OA2DP_FORMAT_CHANGE &out)
{
    DWORD bytes = 0;
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_GET_STATE, nullptr, 0, &out, sizeof(out),
                      &bytes, 5000) == ERROR_SUCCESS && bytes == sizeof(out);
}

bool AudioEndpoint::set_formats(const OA2DP_FORMAT_LIST &list)
{
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_FORMATS, &list, sizeof(list), nullptr, 0, nullptr,
                      5000) == ERROR_SUCCESS;
}

bool AudioEndpoint::set_latency(uint32_t transport_us, uint32_t device_us)
{
    OA2DP_LATENCY l{transport_us, device_us};
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_LATENCY, &l, sizeof(l), nullptr, 0, nullptr, 5000) ==
           ERROR_SUCCESS;
}

bool AudioEndpoint::set_name(const std::wstring &name)
{
    OA2DP_ENDPOINT_NAME n{};
    wcsncpy(n.Name, name.c_str(), ARRAYSIZE(n.Name) - 1);
    return dev_.ioctl(IOCTL_OA2DP_AUDIO_SET_NAME, &n, sizeof(n), nullptr, 0, nullptr, 5000) ==
           ERROR_SUCCESS;
}

bool AudioEndpoint::map_ring()
{
    OA2DP_RING_MAPPING m{};
    DWORD n = 0;
    if (dev_.ioctl(IOCTL_OA2DP_AUDIO_MAP_RING, nullptr, 0, &m, sizeof(m), &n, 5000) !=
            ERROR_SUCCESS ||
        n != sizeof(m))
        return false;
    auto *r = reinterpret_cast<oa2dp_pcm_ring *>(static_cast<uintptr_t>(m.Address));
    if (oa2dp_ring_validate(r, static_cast<size_t>(m.Bytes)) != OA2DP_OK) {
        LOGE("audio driver returned an invalid PCM ring");
        dev_.ioctl(IOCTL_OA2DP_AUDIO_UNMAP_RING, nullptr, 0, nullptr, 0, nullptr, 5000);
        return false;
    }
    ring_ = r;
    return true;
}

void AudioEndpoint::unmap_ring()
{
    if (ring_ && dev_.is_open())
        dev_.ioctl(IOCTL_OA2DP_AUDIO_UNMAP_RING, nullptr, 0, nullptr, 0, nullptr, 5000);
    ring_ = nullptr;
}

DWORD AudioEndpoint::wait_format(OA2DP_FORMAT_CHANGE &out, HANDLE cancel)
{
    DWORD n = 0;
    DWORD error = dev_.ioctl(IOCTL_OA2DP_AUDIO_WAIT_FORMAT, nullptr, 0, &out, sizeof(out), &n,
                              INFINITE, cancel);
    if (error != ERROR_SUCCESS)
        return error;
    return n == sizeof(out) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

}  // namespace oa2dp
