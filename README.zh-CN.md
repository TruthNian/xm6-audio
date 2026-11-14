# XM6 Audio — native Windows Bluetooth implementation

[English](README.md) · 简体中文

> **短音频缺陷修复中：** 用户报告拾词发音和 Windows 通知声不完整，0.4.4 未通过验收。本机两个驱动和服务均已部署 0.4.7：发现冷启动 START 在 Sniff 省电模式下等待后，改为先通过 Windows 请求唤醒链路，再发送 START；保留此前的音频帧保留、实时 RUN 前准备和缓冲区分配修正。自动通知声／短音频检查未记录到 Windows 调度故障、原生缓冲溢出或发送失败。冷启动改善和声音完整性尚未验收，带缓冲区的端点探测开销仍存在。见[源码审查与验证边界](docs/SHORT-CLIP-REVIEW.md)。

原生 Windows 蓝牙音频项目，面向 WH-1000XM6。**0.4.0 定为当前已验证配置（本机 Windows 10 x64 + XM6）的稳定版。** 按项目所有者于 2026-11-12 的决定，暂停功能扩展和其他平台／耳机适配。本机保留现有测试签名部署；公开程序包仍未签名，普通保护配置下的安装仍需微软生产签名。其他耳机及 Windows 11 不在本次验证范围内，保留系统原生 A2DP 回退。

0.3.50 修复了已独立复现的离线误显示 Connected 路径。用户此前对 QQ 音乐、PotPlayer、系统提示声及蓝牙控制的确认，以及 YouTube 音频卡顿报告，适用于 0.3.44。0.3.61 已获用户实测确认：**未连接时正确显示 Paired，连接和断开后的 Windows 显示均正常**，随后测试期间暂未再次出现 Chrome 声音卡顿。这些记录保留各自的版本与验收范围。

**0.4.0** 将两个原生驱动和编码服务统一为一个程序包，并修复旧端点名称的安装迁移：即使安装同一个包，也会对指定音频设备执行完整安装流程。本机 Core Audio 和 PnP 已返回“耳机 (WH-1000XM6)”，实际卸载／重新加载驱动后名称仍正确；默认输出已迁移，混音格式、音量和静音状态保留。见[名称调查](docs/ENDPOINT-IDENTITY-REVIEW.md)、[0.4.0 范围](docs/RELEASE-0.4.0.md)和[部署记录](docs/HARDWARE-BRINGUP.md)。稳定版状态与签名状态分别记录；此次定版未修改、重建或重新部署二进制文件。

用户已验收 0.3.61 下未连接时显示 Paired、连接和断开后状态正确，并反馈暂未再次出现 Chrome 声音卡顿。Chrome／QQ 音乐与 PotPlayer 各约五分钟的独立记录均无媒体发送失败，此前两次中断已按用户要求结束调查，不推定原因，也不标记为已修复。0.3.62 激活后的短时记录也无发送失败，随后采样确认原有 PotPlayer 会话包含非零音频信号。这些是部分验收结果，尚不能证明所有播放、格式切换和生命周期场景通过。总线 ABI 6、自动 PCM／编码策略及此前时钟和 RTP 修复保留。详见[生命周期审查](docs/LIFECYCLE-REVIEW.md)、[事件链审查](docs/EVENT-CHAIN-REVIEW.md)、[性能审查](docs/PERFORMANCE-REVIEW.md)和[编码审查](docs/CODEC-REVIEW.md)。**不要加载 0.3.5 候选驱动**：该版属性表布局错误曾导致开发机蓝屏，0.3.6 已修正并增加离线 PE/PDB 检查。

已配对但未连接时，英文界面显示 **Paired**。`Disconnected(3)` 是 Windows 内部状态枚举，本机对应资源文字为 `Paired`。当前界面状态已由用户亲自确认；Computer Use 的窗口截图仍超时，因此视觉验收依据是用户实测，而非工具截图。

此前保留的离线蓝牙开关事件有约 1 秒的临时 Connected，之后模型稳定为对应 Paired 的状态。Windows 自带驱动对照也出现过相似过渡，不能仅凭这一段判定自定义驱动有错。当前稳定后的显示已获用户确认，详见[运行记录](docs/HARDWARE-BRINGUP.md)。

`follow_source_rate` 这个兼容配置名实际表示“跟随端点 PCM 采样率”，不读取播放器文件或 Hi-Res／SQ 标签。共享模式下，多个应用先按一个 Windows 端点格式混音；96 kHz 文件进入 48 kHz 混音时，驱动仍只收到 48 kHz。逐文件自动选择共享混音格式尚未实现。播放器可通过独占模式直接提交支持的格式，但会排除其他应用同时使用该端点。

连接标签异常可使用[只读命令行检查](docs/DIAGNOSTICS.md)：同时读取 Windows 蓝牙开关、AEP、设备容器和原生音频会话，无需打开设置窗口或操作耳机，不修改任何配置。它用于定位状态矛盾，不代表重连问题已修复或通过实机验收。

默认分支保留 Windows 的 BTHUSB/BTHPORT、配对、蓝牙开关和 AVRCP 媒体控制。新的 A2DP 配置驱动只绑定所选耳机的 Audio Sink 服务，创建标准 WaveRT 耳机音频端点，复用系统输出选择和端点音量。编码通过原生驱动的 PCM 环形缓冲区交给 SCM 管理的 LDAC 服务，再经 Windows 蓝牙 L2CAP 发送。

编码服务、管理工具和 LDAC/SBC 库纳入驱动 INF 的 Driver Store 包，由 Windows 根据原生设备接口触发服务启动。部署脚本负责签名验证、指定耳机绑定和回退，不再单独复制 Program Files 文件或手工注册服务。

这里没有 WinUSB 整卡接管、系统声音回录、QQ 音乐进程捕获、模拟媒体键、托盘启动器或登录后看门狗。编码服务是原生驱动的一部分；它不接管蓝牙无线电。

```text
任意播放器 / 系统音频
        ↓ Windows Audio / WASAPI
XM6 WaveRT 标准端点 → PCM ring → LDAC 编码服务
        ↓ A2DP 配置驱动 / L2CAP BRB
Windows BTHPORT / BTHUSB → XM6

Windows AVRCP → 系统媒体控制
```

## 音频策略

- 默认跟随 Windows 实际送达端点的 PCM 采样率；不把 44.1 kHz 无理由升至 48/96 kHz。
- 接收整数 16/24/32-bit 和 float32 PCM；分别记录容器位宽、有效位数、整数/浮点，包含 24-in-32。当前源码的自动模式可将 int32/float32 直接交给 libldac，删除中间一次固定 24-bit 量化；显式 16/24-bit 设置仍按指定精度处理。这是编码器输入支持，不代表蓝牙传输变成 32-bit 或无损。本机 0.3.63 已使用新的输入路径，但尚未证明有可听提升。16-bit 实际精度使用 S16 输入，浮点混音不会因原文件是 16-bit 而被猜测为 16-bit。
- 默认按耳机能力优先协商 LDAC，其次 AAC、SBC；状态显示实际编码和端点格式。固定 LDAC 配置仍可显式选择，协商降级不算达到固定 LDAC 的目标。
- 默认使用固定 LDAC **HQ**：44.1/88.2 kHz 为标称 909 kbps，48/96 kHz 为 990 kbps。SQ 为 606/660，MQ 为 303/330。名称取自 Sony 的 libldac，不按名称猜质量。
- 默认关闭 ABR；如需按链路稳定性降档，必须明确配置，不将降档报告成最高品质。
- 独占流可以让驱动接收播放器选择的原格式。共享模式收到的是 Windows 混音格式，驱动无法从中还原每个文件的原始采样率或位深，也不会逐首改系统默认格式干扰其他应用。
- 可显式设置固定 48 kHz/24-bit/HQ 作为诊断配置，但它不再是覆盖所有音源的成功标准。

来源：[Sony libldac API](https://android.googlesource.com/platform/external/libldac/+/eeee1a3f5f8df1282e3a6d297085885fd886737b/inc/ldacBT.h)、[Windows 共享/独占设备格式](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats)。

MP3、FLAC、ALAC、AAC、Opus、WAV 等文件由播放器/Windows 解码为 PCM 后进入相同端点，不要求在蓝牙驱动里重新实现文件解码器。XM6 的经典蓝牙实际支持 SBC/AAC/LDAC；LC3 属于 LE Audio，不能列成这个 Windows 10 A2DP 实现的可用模式。参见 [Sony 编码支持列表](https://helpguide.sony.net/mdr/2984/v1/en/contents/TP1001856790.html)。

## 开发状态

| 项目 | 状态 |
|---|---|
| 协议、PCM 格式及编码器检查 | 整数/浮点及有效位数检查；SBC、AAC、LDAC 编码；LDAC 包含 44.1/16、48/24、88.2/24、96/24 |
| MSVC 原生服务、命令行工具 | 本机已构建；内核驱动由 WDK CI 构建 |
| 可追溯依赖、哈希清单、INF/CAT 包装 | 固定 git submodule 提交；CI 只生成未签名候选包 |
| 微软内核签名 | 未取得；[免费渠道核实记录](docs/SIGNING.md) |
| 本机原生 LDAC 实际声音、系统音量、耳机控制 | 已确认 QQ 音乐原生 LDAC HQ、PotPlayer 视频声音、播放／暂停／切歌、摘戴暂停／恢复及耳机与 Windows 主音量双向同步；不代表已通过已有播放器自动重连验收 |
| 关机、重连、睡眠唤醒、断连端点状态、视频同步 | 0.3.61 的 Paired／连接／断开显示已由用户确认；已有播放器恢复、硬件格式切换、睡眠唤醒和视频同步仍需分别验证 |
| 0.4.0 统一部署与旧名称修复 | 两个实际内核映像与运行中服务均匹配同一 0.4.0.0 程序包；升级并重新加载驱动后，Core Audio、PnP 和底层接口名称均正确，默认输出及音频设置保留，短时测试信号进入 LDAC 传输。见[名称与标识根因审查](docs/ENDPOINT-IDENTITY-REVIEW.md)。两次历史中断按用户要求结束调查，不推定原因 |

签名验证脚本拒绝未签名包，不安装测试证书，不关闭 Secure Boot，不改变签名强制。不要把已有厂商驱动的签名移植到新文件；签名绑定文件内容和目录成员。

参见 [BUILDING.md](BUILDING.md)、[签名与部署](docs/SIGNING.md)。原有可听的整卡接管桥接实现保存在 [`v0.1.0-bridge`](https://github.com/TruthNian/xm6-audio/tree/v0.1.0-bridge)；它不是本分支的原生实现。

## 来源与许可

原生实现基于 [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp)，固定源码提交 `167ebc0e2c89fec570eec535fefdade72bd9e416`。本项目保留其 MIT 许可和 clean-room 规则；WaveRT 衍生代码另保留 Microsoft SYSVAD 的 MS-PL 许可和原始版权。Sony libldac 使用 Apache-2.0，BlueZ libsbc 使用 LGPL-2.1-or-later，FindWDK 使用 BSD-3-Clause。参见 [THIRD_PARTY.md](THIRD_PARTY.md)。本项目与 Sony、Microsoft、Alternative A2DP 厂商无隶属关系。

另见 [源码来源](docs/PROVENANCE.md)、[隐私政策](docs/PRIVACY.md)。默认文档为英文，便于开源贡献者和签名机构审阅。
