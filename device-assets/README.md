# 机型侧线节模板（接管轮音频必需，换机型要重抓）

这三个文件由 `bin/halsink.sh` 在启动时从 `/data/local/tmp/` 读取，缺任何一个音频桥都起不来
（`halsink.sh` 里有显式检查并 exit 3）：

| 文件 | 内容 | 来源 |
|---|---|---|
| `mix2.bin` | `deep_buffer_out`（portId 2）的 AudioPortConfig 线节 | 在 anland 会话里从 HAL 抓取 |
| `dev23.bin` | 扬声器（portId 23）设备端口的 AudioPortConfig 线节 | 同上 |
| `patch0.bin` | 一条 AudioPatch 线节（源/汇 id 运行时由 PAUTO 改写） | 同上 |

⚠ **这些是 Xiaomi Pad 8 Pro（piano）+ 当前 HyperOS 版本的产物**，换机型或大版本升级后必须重新抓取，
不能照搬（线节里的端口 id、采样率、mask 位都是设备相关的）。

原来它们放在被 `.gitignore` 挡住的 `artifact/` 里，只存在于开发机上 ⇒ 新设备装出来没有声音。
现在随 release 资产一起分发，由 `droid-drm-takeover` 的安装器部署到 `/data/local/tmp/`。
