# droid-audio-bridge

在小米 Pad 8 Pro（SM8750/piano）的 **DRM 接管态** Linux 桌面里，让声音从**内置扬声器**出来。

原理一句话：接管轮的 `stop` 会杀掉 audioserver（class core），但 vendor 音频 HAL
`audiohalservice.qti`（class hal）活着 —— 本项目做它的 binder 客户端，
经 `android.hardware.audio.core.IModule/default` 直接开输出流，
数据走 HAL 的共享内存环（AudioRingBuffer），完全绕开安卓 framework。

- 事实基线 / 事务码表 / 里程碑 / 红线：[`直连音频HAL方案.md`](直连音频HAL方案.md)
- 构建：GitHub Actions（NDK 交叉编译），产物 push 到 `/data/local/tmp/` 运行
- 兄弟项目：droid-drm-takeover（显示/输入/网络接管）、droid-bluetooth-bridge（蓝牙）、droid-pc-keyboard（键盘）

> 仅在小米 Pad 8 Pro（HyperOS / android15-6.6 vendor）上验证过。
