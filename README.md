# XM6 Audio — native Windows Bluetooth implementation

原生 Windows 蓝牙 LDAC 的开发原型，面向 WH-1000XM6。**尚未完成驱动签名和本机实机验收，当前不是可安装成品。** 已恢复可用的 Windows 自带 A2DP 通道应继续保留。

默认分支保留 Windows 的 BTHUSB/BTHPORT、配对、蓝牙开关和 AVRCP 媒体控制。新的 A2DP 配置驱动只绑定所选耳机的 Audio Sink 服务，创建标准 WaveRT 耳机音频端点，复用系统输出选择和端点音量。编码通过原生驱动的 PCM 环形缓冲区交给 SCM 管理的 LDAC 服务，再经 Windows 蓝牙 L2CAP 发送。

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
- 16-bit 端点流使用 LDAC S16 输入；24-bit/浮点流保留最多 24 个有用位。零扩展不会恢复原音源中不存在的信息，浮点混音也不应因原文件是 16-bit 而再次截短。
- 默认使用固定 LDAC **HQ**：44.1/88.2 kHz 为标称 909 kbps，48/96 kHz 为 990 kbps。SQ 为 606/660，MQ 为 303/330。名称取自 Sony 的 libldac，不按名称猜质量。
- 默认关闭 ABR；如需按链路稳定性降档，必须明确配置，不将降档报告成最高品质。
- 独占流可以让驱动接收播放器选择的原格式。共享模式收到的是 Windows 混音格式，驱动无法从中还原每个文件的原始采样率或位深，也不会逐首改系统默认格式干扰其他应用。
- 可显式设置固定 48 kHz/24-bit/HQ 作为诊断配置，但它不再是覆盖所有音源的成功标准。

来源：[Sony libldac API](https://android.googlesource.com/platform/external/libldac/+/eeee1a3f5f8df1282e3a6d297085885fd886737b/inc/ldacBT.h)、[Windows 共享/独占设备格式](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats)。

## 开发状态

| 项目 | 状态 |
|---|---|
| 协议、配置及 LDAC 编码器检查 | 可在本机快速构建运行；包含 44.1/16、48/24、88.2/24、96/24 |
| MSVC 原生服务、命令行工具 | 本机已构建；内核驱动由 WDK CI 构建 |
| 可追溯依赖、哈希清单、INF/CAT 包装 | 固定 git submodule 提交；CI 只生成未签名候选包 |
| 微软内核签名 | 未取得；[免费渠道核实记录](docs/SIGNING.md) |
| 本机原生 LDAC 实际声音、系统音量、耳机控制 | 未验收，不以离线编码或发送计数代替 |
| 关机、重连、睡眠唤醒、断连端点状态、视频同步 | 已补标准 jack 状态通知和重连旧 PCM 清理；仍须实机验证 |

签名验证脚本拒绝未签名包，不安装测试证书，不关闭 Secure Boot，不改变签名强制。不要把已有厂商驱动的签名移植到新文件；签名绑定文件内容和目录成员。

参见 [BUILDING.md](BUILDING.md)、[签名与部署](docs/SIGNING.md)。原有可听的整卡接管桥接实现保存在 [`v0.1.0-bridge`](https://github.com/TruthNian/xm6-audio/tree/v0.1.0-bridge)；它不是本分支的原生实现。

## 来源与许可

原生实现基于 [RafiMota/open-a2dp](https://github.com/RafiMota/open-a2dp)，固定源码提交 `167ebc0e2c89fec570eec535fefdade72bd9e416`。本项目保留其 MIT 许可和 clean-room 规则；WaveRT 衍生代码另保留 Microsoft SYSVAD 的 MS-PL 许可和原始版权。Sony libldac 使用 Apache-2.0，FindWDK 使用 BSD-3-Clause。参见 [THIRD_PARTY.md](THIRD_PARTY.md)。本项目与 Sony、Microsoft、Alternative A2DP 厂商无隶属关系。
