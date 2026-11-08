# XM6 Audio development

Implement a real Windows Bluetooth A2DP profile and standard XM6 render endpoint. Windows owns BTHUSB/BTHPORT, pairing, AVRCP/HFP and device discovery. Do not reintroduce WinUSB radio ownership, system loopback capture, simulated keyboard control or a runtime launcher/watchdog.

Match the actual Windows PCM stream rather than forcing 48 kHz/24 bit. The user's latest correction supersedes the earlier fixed-format target. Preserve supported sample rates (44.1 kHz stays 44.1 kHz); use 16-bit LDAC input for a 16-bit endpoint stream and preserve up to 24 useful bits for 24-bit/float streams. Keep LDAC HQ (909 kbps for 44.1/88.2 kHz, 990 kbps for 48/96 kHz) without ABR by default. Keep an explicit fixed 48 kHz/24-bit profile available for diagnostics. Never mistake a shared-mode mix format for original file metadata or call a negotiated downgrade success.

Keep the current Windows inbox audio recovery usable until a signed native LDAC package passes hardware acceptance. Do not install unsigned/test-signed kernel drivers, trust test certificates, disable Secure Boot or change signature enforcement on the user's host. Building and inspecting a package is not hardware acceptance. No new paid services without the user's decision.

Question requirements, remove unnecessary layers, simplify, then optimize demonstrated bottlenecks. Use focused fast protocol/encoder checks and required builds; avoid broad repeated tests. Preserve upstream attribution and the bridge baseline tag. Keep machine addresses, pairing keys, account identifiers, signing keys and local state out of git. External contact or signing submissions require the user's explicit instruction and a reviewable request/package first.
