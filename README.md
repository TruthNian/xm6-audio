# XM6 Audio

基于 [BlueCaster v3.0.0-beta.3](https://github.com/SeiyaFunaokaJP/BlueCaster/tree/v3.0.0-beta.3) 的源码集成版本，目标是在 Windows 上让 WH-1000XM6 以 **48 kHz、立体声、24 bit、固定 LDAC 990 kbps** 播放系统音频。

首个已听到音乐且稳定的基线来自 Windows 10 19045、Intel 8087:0AAA 内置蓝牙和 QQ 音乐。它证明了编码与无线发送链路可行，不等于已经验证所有播放器、重启、休眠和视频同步。

改进直接进入现有原生核心：恢复已保存配置、在耳机稍后开机时重试初次连接、保留用户断开操作，并在普通模式中使用现有系统输出静音机制。安装完成后由同一个 `bluecaster-core.exe` 承担音频与设备生命周期；没有新增运行时脚本或守护进程。

一次性安装：从本仓库 Actions 获取原生核心构建，将它与上游同版本 `bluecaster.exe` 放在同一目录，再运行 `integration/Install-NativeProfile.ps1`。本机地址仅作为本机参数写入 `%APPDATA%/BlueCaster`，不要提交到仓库。已有配对密钥保留在本地。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File integration/Install-NativeProfile.ps1 -DeviceAddress 'AA:BB:CC:DD:EE:FF' -BuildDirectory 'C:\path\to\runtime'
```

完整的实现、验收状态和后续顺序见 [基线记录](docs/XM6-BASELINE.md)。上游功能及构建说明见 [原始 README](README.upstream.md) 和 [Building](docs/building.md)。

工程原则：先质疑要求，删除多余环节，简化必要路径，再优化有证据的瓶颈。实际可听到音乐是验收的一部分，码率计数和电平表不能代替它。源码改变后运行针对启动恢复和输出静音的必要检查。

边界：固定 990 kbps 需要足够的无线带宽；强干扰、DRM、独占输出、应用指定其他输出设备，以及视频唇音同步需要分别处理。这是 WinUSB 用户态蓝牙实现，当前没有为 Windows 新建原生耳机音频端点，也没有实现内核 A2DP 编码驱动。

本仓库保留上游 MIT/Apache-2.0 授权与全部第三方说明。BTstack 依赖受其个人、非商业使用条款约束；本仓库源码许可证不替代该条款。详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
