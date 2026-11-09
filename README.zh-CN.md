# XM6 Audio — native Windows Bluetooth implementation

[English](README.md) · 简体中文

原生 Windows 蓝牙 LDAC 的开发原型，面向 WH-1000XM6。**尚未完成驱动签名和本机实机验收，当前不是可安装成品。** 已恢复可用的 Windows 自带 A2DP 通道应继续保留。

一台明确授权测试签名的开发机已验证 QQ 音乐原生 LDAC、PotPlayer 视频声音、媒体控制、摘戴自动暂停／恢复及耳机与 Windows 主音量双向同步，0.3.18 重启后也确认声音正常；Windows 设置中的单设备断开／连接状态仍然错误。后续 [0.3.31 生命周期审查](docs/LIFECYCLE-REVIEW.md)记录源码修正和离线验证范围，尚未部署，不能算作实机修复完成。完整验收见[开发记录](docs/HARDWARE-BRINGUP.md)。**不要加载 0.3.5 候选驱动**：该版属性表布局错误已导致开发机蓝屏，0.3.6 源码修正了结构并增加离线 PE/PDB 检查。

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
- 接收整数 16/24/32-bit 和 float32 PCM；分别记录容器位宽、有效位数、整数/浮点，包含 24-in-32。完整整数 32-bit 在环形缓冲区保留，送入编码器时按其能力一次量化；LDAC 最多保留 24 个有用位。16-bit 实际精度使用 S16 输入，浮点混音不会因原文件是 16-bit 而被猜测为 16-bit。
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
| 关机、重连、睡眠唤醒、断连端点状态、视频同步 | 已核对两个 0.3.18 驱动实际加载、服务自动启动及重启后声音正常；Windows 单设备断开／连接界面仍有错误，格式切换、睡眠唤醒和视频同步仍待验收 |
| 后续生命周期源码修正 | 0.3.31 尚未部署；修正范围、离线检查和剩余限制见[审查记录](docs/LIFECYCLE-REVIEW.md) |

签名验证脚本拒绝未签名包，不安装测试证书，不关闭 Secure Boot，不改变签名强制。不要把已有厂商驱动的签名移植到新文件；签名绑定文件内容和目录成员。

参见 [BUILDING.md](BUILDING.md)、[签名与部署](docs/SIGNING.md)。原有可听的整卡接管桥接实现保存在 [`v0.1.0-bridge`](https://github.com/TruthNian/xm6-audio/tree/v0.1.0-bridge)；它不是本分支的原生实现。

## 来源与许可

原生实现基于 [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp)，固定源码提交 `167ebc0e2c89fec570eec535fefdade72bd9e416`。本项目保留其 MIT 许可和 clean-room 规则；WaveRT 衍生代码另保留 Microsoft SYSVAD 的 MS-PL 许可和原始版权。Sony libldac 使用 Apache-2.0，BlueZ libsbc 使用 LGPL-2.1-or-later，FindWDK 使用 BSD-3-Clause。参见 [THIRD_PARTY.md](THIRD_PARTY.md)。本项目与 Sony、Microsoft、Alternative A2DP 厂商无隶属关系。

另见 [源码来源](docs/PROVENANCE.md)、[隐私政策](docs/PRIVACY.md)。默认文档为英文，便于开源贡献者和签名机构审阅。
