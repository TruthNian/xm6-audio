# XM6 Audio

**当前可运行版本是 WinUSB 用户态桥接实验基线，不是 Windows 原生蓝牙耳机驱动。** 它会让 Windows 失去对该蓝牙适配器的管理，系统蓝牙可能显示 off，也没有独立的 XM6 标准音频端点。这些是尚未解决的架构缺口，不是已经完成的系统集成。

基于 [BlueCaster v3.0.0-beta.3](https://github.com/SeiyaFunaokaJP/BlueCaster/tree/v3.0.0-beta.3) 保存可复现的编码和传输基线，目标是在 Windows 上让 WH-1000XM6 以 **48 kHz、立体声、24 bit、固定 LDAC 990 kbps** 播放系统音频，并具有真正的 Windows 设备集成。

首个已听到音乐且稳定的基线来自 Windows 10 19045、Intel 8087:0AAA 内置蓝牙和 QQ 音乐。它证明了编码与无线发送链路可行，不等于已经验证所有播放器、重启、休眠和视频同步。

桥接分支的改进直接进入现有核心：恢复已保存配置、在耳机稍后开机时重试初次连接、保留用户断开操作、采集所有应用音频，并把耳机媒体按键优先交给 Windows 当前媒体会话。它只用于保留可播放基线和排查协议，不被视为最终原生实现。

桥接实验的一次性配置：从本仓库 Actions 获取核心构建，将它与上游同版本 `bluecaster.exe` 放在同一目录，再运行 `integration/Install-BridgeProfile.ps1`。这一步不会提供 Windows 原生蓝牙管理或 XM6 音频端点。本机地址仅写入本机配置，不能提交到仓库。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File integration/Install-BridgeProfile.ps1 -DeviceAddress 'AA:BB:CC:DD:EE:FF' -BuildDirectory 'C:\path\to\runtime'
```

完整的实现与验收状态见 [基线记录](docs/XM6-BASELINE.md)，后续工作的触发条件和验收见 [改进决策](docs/XM6-ROADMAP.md)。上游功能及构建说明见 [原始 README](README.upstream.md) 和 [Building](docs/building.md)。

工程原则：先质疑要求，删除多余环节，简化必要路径，再优化有证据的瓶颈。实际可听到音乐是验收的一部分，码率计数和电平表不能代替它。源码改变后运行针对启动恢复和输出静音的必要检查。

完整耳机行为也是验收条件：播放、暂停、上一曲、下一曲、Windows 音量、系统声音、重连和视频。当前实现与 Alternative A2DP 的架构区别、以及真正 Windows 音频端点仍缺的部分，见 [Windows 集成边界](docs/XM6-WINDOWS-INTEGRATION.md)。

后续主线首先删除整块适配器接管：保留 Windows 蓝牙栈，在 A2DP 音频设备边界接入编码与传输。候选实现的实际状态见 [原生后端调查](docs/XM6-NATIVE-CANDIDATES.md)。

边界：固定 990 kbps 需要足够的无线带宽；强干扰、DRM、独占输出、应用指定其他输出设备，以及视频唇音同步需要分别处理。这是 WinUSB 用户态蓝牙实现，当前没有为 Windows 新建原生耳机音频端点，也没有实现内核 A2DP 编码驱动。

本仓库保留上游 MIT/Apache-2.0 授权与全部第三方说明。BTstack 依赖受其个人、非商业使用条款约束；本仓库源码许可证不替代该条款。详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)。
