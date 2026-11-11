/* SPDX-License-Identifier: MIT */
/* Actual session power refresh/retry bodies; driver queries and time controlled.
 * No Bluetooth, sleep/resume or Windows scheduler behavior is simulated. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while (0)
#define LOGW(...) ((void)0)
using ULONG=uint32_t;
using DWORD=uint32_t;
constexpr ULONG OA2DP_IOCTL_VERSION=6, OA2DP_DEVICE_SUSPENDED=2;
struct OA2DP_DEVICE_INFO { ULONG Version=0, Flags=0, PowerSerial=0; };
static DWORD now;
static DWORD GetTickCount() { return now; }
static bool is_after(DWORD a,DWORD b) { return static_cast<int32_t>(a-b)>=0; }
struct Session {
    struct Transport {
        OA2DP_DEVICE_INFO current{OA2DP_IOCTL_VERSION,0,2};
        unsigned failures=0,calls=0;
        bool get_info(OA2DP_DEVICE_INFO &out) {
            ++calls;
            if(failures) { --failures; return false; }
            out=current;
            return true;
        }
    } tr_;
    bool stopping_=false,transport_suspended_=true,power_refresh_pending_=false;
    // These must never gate the power retry or be changed by its result.
    bool audio_ready_=false,connection_enabled_=false;
    ULONG power_serial_=1;
    DWORD next_power_refresh_=0,next_reconnect_=0;
    unsigned teardowns=0;
    std::string state;
    void teardown(const char *) { ++teardowns; }
    void publish(const char *value) { state=value; }
    void refresh_transport_power();
    void retry_transport_power();
};
#include "power-refresh-under-test.inc"
int main() {
    now=100;
    {
        Session s;
        s.tr_.failures=1;
        s.refresh_transport_power(); // sole resume event; query temporarily fails
        REQUIRE(s.power_refresh_pending_ && s.transport_suspended_ && s.power_serial_==1);
        REQUIRE(s.tr_.calls==1 && !s.teardowns && s.state.empty());
        now=1099;s.retry_transport_power();
        REQUIRE(s.tr_.calls==1); // no busy retry and no additional event required
        now=1100;s.retry_transport_power();
        REQUIRE(s.tr_.calls==2 && !s.power_refresh_pending_ && !s.transport_suspended_);
        REQUIRE(s.power_serial_==2 && s.teardowns==1 && s.next_reconnect_==now);
        REQUIRE(s.state=="power-resuming" && !s.audio_ready_ && !s.connection_enabled_);
        now=9000;s.retry_transport_power();
        REQUIRE(s.tr_.calls==2 && s.teardowns==1); // no healthy-state polling
    }
    now=100;
    {
        Session s;
        s.transport_suspended_=false;
        s.tr_.failures=2;
        s.refresh_transport_power();
        now=1100;s.retry_transport_power();
        REQUIRE(s.power_refresh_pending_ && !s.transport_suspended_ && !s.teardowns);
        now=2099;s.retry_transport_power();
        REQUIRE(s.tr_.calls==2);
        // Power changed again while queries failed: consume the latest snapshot.
        s.tr_.current.Flags=OA2DP_DEVICE_SUSPENDED;s.tr_.current.PowerSerial=3;
        now=2100;s.retry_transport_power();
        REQUIRE(s.tr_.calls==3 && s.transport_suspended_ && s.power_serial_==3);
        REQUIRE(!s.power_refresh_pending_ && s.teardowns==1 && s.state=="power-suspended");
    }
    now=100;
    {
        Session s;
        s.tr_.failures=1;s.refresh_transport_power();
        // A fresh notification may resolve the state before the retry deadline.
        now=150;s.refresh_transport_power();
        REQUIRE(!s.power_refresh_pending_ && s.tr_.calls==2 && s.teardowns==1);
        now=1100;s.retry_transport_power();
        s.refresh_transport_power(); // same serial and state: no duplicate teardown
        REQUIRE(s.tr_.calls==3 && s.teardowns==1);
    }
    now=100;
    {
        Session s;
        s.tr_.current={OA2DP_IOCTL_VERSION,OA2DP_DEVICE_SUSPENDED,1};
        s.tr_.failures=1;s.refresh_transport_power();
        now=1100;s.retry_transport_power();
        REQUIRE(!s.power_refresh_pending_ && s.tr_.calls==2 && !s.teardowns);
        REQUIRE(s.transport_suspended_ && s.power_serial_==1);
        // A complete sleep/resume can leave the same flag but a newer serial.
        s.tr_.current.PowerSerial=3;s.refresh_transport_power();
        REQUIRE(s.teardowns==1 && s.power_serial_==3);
    }
    now=0xfffffe00u;
    {
        Session s;
        s.tr_.current.Version=OA2DP_IOCTL_VERSION+1;s.refresh_transport_power();
        REQUIRE(s.power_refresh_pending_ && !s.teardowns && s.power_serial_==1);
        now+=999;s.retry_transport_power();REQUIRE(s.tr_.calls==1);
        ++now;s.tr_.current.Version=OA2DP_IOCTL_VERSION;s.retry_transport_power();
        REQUIRE(s.tr_.calls==2 && !s.transport_suspended_ && !s.power_refresh_pending_);
    }
    now=100;
    {
        Session s;
        s.tr_.failures=1;s.refresh_transport_power();
        s.stopping_=true;now=1100;s.retry_transport_power();s.refresh_transport_power();
        REQUIRE(s.tr_.calls==1 && !s.teardowns && s.power_serial_==1);
    }
    std::puts("production power refresh failure/latest-state/retry/stop orderings passed (driver and clock controlled)");
}
