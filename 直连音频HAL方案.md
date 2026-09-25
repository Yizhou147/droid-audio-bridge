# droid-audio-bridge：直连 vendor 音频 HAL 的板载出声桥（事实基线与方案）

> 目标：在 **DRM 接管态**（安卓 framework 全员 `stop`、system_server 冻结）下，让容器 Linux 桌面的声音
> 从平板**内置扬声器**出声。独立于 droid-drm-takeover，产物是一个安卓侧守护 + 容器侧喂流胶水。
> 本文 = 该项目的唯一事实源（蓝牙桥同款纪律：实测过才写，猜的标"假设"）。

## 1. 为什么必须直连 HAL（2026-09-25 全链路破案，原始记录在主仓库《工作总结》§38）

- `stop`（不带类名）= 停掉除 class hal 以外的**所有**服务；audioserver 是 `class core`（/system/etc/init/audioserver.rc 实测）⇒ **接管轮里 audioserver 必死**，"轮前起音乐带进轮子"不成立（用户判断正确）。
- 轮内新起 audioserver 也不行：它初始化时**阻塞等 `activity`（system_server）**，而 system_server 冻结是我们自己的 watchdog/panic 防线，不能解冻。
- **`audiohalservice.qti` 是 class hal，`stop` 不动它**（composer HAL 同族事实，09-21 已实锤）⇒ 接管轮里唯一活着的、且自带完整 AGM 建图/ACDB 校准/智能功放保护链的进程就是它。
- 反证也已拿到：anland 态用真播放器（MIUI MediaViewer）放 6 秒音，strace HAL 看到 `AHAL_StreamOut_MI` 活跃 = 流一旦经它打开，**图自动建、声音自动出**，一切厂商逻辑都在它内部。

## 2. binder 客户端硬事实（事务码全部从设备自己的桩库反汇编，未猜）

来源：`/vendor/lib64/android.hardware.audio.core-V2-ndk.so`（= HAL 加载的那份；服务在 vintf manifest 声明 **version=2**）。
方法名以设备二进制为准：**是 `getAudioPorts`，不是旧文档里的 `getPorts`**。

| 接口 | 方法 | 事务码 | 取证方式 |
|---|---|---|---|
| IModule | getAudioPorts | **11** | objdump BpModule@0x3af28 → transact 前 mov w1 |
| IModule | openOutputStream | **15** | BpModule@0x3bdf8 |
| IStreamOut(父 IStream) | getStreamCommon | 1 | 全库扫描 |
| IStreamOut(父) | updateMetadata | 2 | 同上 |
| IStreamOut | updateOffloadMetadata | 3 | 同上 |
| IStreamOut | getHwVolume / setHwVolume | 4 / 5 | 同上 |
| IStreamOut | getAudioDescriptionMixLevel / set… | 6 / 7 | 同上 |
| IStreamOut | getDualMonoMode / setDualMonoMode | 8 / 9 | 同上 |
| IStreamOut | getRecommendedLatencyModes / setLatencyMode | 10 / 11 | 同上 |
| IStreamOut | get/setPlaybackRateParameters | 12 / 13 | 同上 |
| IStreamOut | selectPresentation | 14 | 同上 |
| （通用） | getInterfaceHash | 0xFEFFFFFE | 同上 |
| （通用） | getInterfaceVersion | 0xFEFFFFFF | 同上 |

**IStreamOut 没有 `write(byte[])`！**（导出+本地符号表全扫，无此方法）
⇒ **数据面 = 共享内存**：`createMmapBuffer` 拿到 ring（AOSP `AudioRingBuffer`，头带读写指针），
client 写环、HAL 自取；binder 上只有 `writeAvBrokenHwModule...HackAidl...`（float 推进量）与 drain/standby 类控制。
`createMmapBuffer` 的码在本地符号里没抓到（未导出）——M1 第一件事 = 反汇编定位它（同表法），不许猜。

## 3. 硬件侧备案（今日实测，防重复踩）

- 声卡 sun-mtp-snd-card：容器要 `mknod /dev/snd/*`（主从号现查 `/sys/class/sound/*/dev`），
  但 **裸 ALSA 全线封死**：FE open/prepare 过、write EINVAL（AGM 不建图不出声，设计如此）。
- `system_suspend` 是 AGM/audioserver 初始化依赖（轮内被停 ⇒ agmplay/audioserver 卡 `Waited ...ISystemSuspend`，
  agmplay 的 rc=0 是假成功）。
- 4 路 fs16xx 智能功放：FSM_* kcontrol（tl/tr/bl/br Scene=7/9/11/13, Vol=235，见 mixer_paths_sun_mtp.xml）；
  vendor 参考工具（agmplay/PalTest/mm-audio-ftm）对本机全是死码，别再碰。
- 设备号会变（kgsl 主号、snd 次号都实测漂移过），一切现查。

## 4. 架构

```
容器（uid 1000）                                安卓侧（root, class-hal 生态之外零依赖）
┌─ PipeWire 桌面声 ─┐   raw s16le/48k/2ch     ┌─ audio-sink 守护（NDK 二进制，CI 构建）─┐
│ pw-loopback/record ├──── unix socket ───────►│ binder→IModule/default: getAudioPorts →  │
└───────────────────┘  (/data/local/tmp/      │ openOutputStream(speaker port, config)   │
                       audio.sock，容器可直接  │ createMmapBuffer → AudioRingBuffer 写环   │
                       读写该路径=共享 fs)      └──────────────────► audiohalservice.qti ──┘
```

- 守护只在接管轮内跑（同蓝牙桥的窗口纪律：进轮 `desk-takeover` 拉起、`desk-stop`/rollback 一律 pkill 交还；
  它不占任何独占硬件，HAL 本来就活着，冲突面=同一 module 的流位——交还安卓时 framework 复活会重建自己的流）。
- **红线**：只 read 端口 + open 一条音乐流；不碰 IConfig.setAudioPorts、不调 setDevicesConnectionState、
  不发任何 `setParameters` 全局改动。
- **禁外放（09-25 用户要求）**：开发期一切通路验证用**全零静音帧**灌 ring（链路真跑、喇叭不响）；
  需要出声的验收必须先问用户要时间窗口。判据改用回包内容/流状态/HAL 日志，不用耳朵。
- 与 anland 的关系：anland 态 audioserver 活着，安卓自己的 AudioService 也在——**桥在 anland 与轮内都物理可跑**，
  但 anland 态要不要它出声属产品决策（安卓 UI 没有音量控制它），默认只在轮内拉起。

## 5. 里程碑

- **M0（本仓库当前代码）= audio-probe 探针**：getService + getAudioPorts(11)，把回包 hexdump/AString 扫出来，
  证明 binder 链路与端口可见性；顺带拿扬声器 port 的 id/角色/profile。
- **M1 = audio-sink v0**：openOutputStream(15) 建流 + 定位 createMmapBuffer 码 + 实现 ring 写，喂本地 wav 从喇叭出声（anland 态验收；先不碰轮）。
- **M2 = 容器胶水**：PipeWire 出口 → socket 喂守护（纯容器侧 python/socat，不需要编译），桌面音乐/系统音全通。
- **M3 = 接管轮接线**：desk-takeover 拉起+判据（流状态、无声时点名卡点）、desk-stop/rollback 交还 pkill；双模式回归。
- **M4 = 收尾**：音量策略（HAL 侧默认满量程？守护自缩）、与 A2DP 共存（切设备时的 sink 迁移）。

## 6. 构建/部署（照蓝牙仓）

- 本机 arm64 无 NDK ⇒ CI（GitHub Actions，runner 自带 NDK 优先，回落 sdkmanager，见 build.yml 注释史）。
- 产物 `out/audio-probe` `adb push /data/local/tmp/`，`su -c` 跑；SELinux 同 bthci-bridge 已趟平（ksu 域可 transact hal 服务）。
- 首次跑通判据：probe 能打出 ≥1 个 OUTPUT 角色端口 + 含 speaker 字样的 id（探针必须自带反例意识：全 0 先怀疑探针）。


## 7. 【09-25 深夜】M0 达成 + 客户端形态的事实矩阵

**M0 验收已达成（借 `service call` 完成）**：`service call android.hardware.audio.core.IModule/default 11`
回 19164 字节、exception=0、UTF-16 端口名齐全（speaker / bt_a2dp_speaker / built_in_mic…），
原始回包已归档 `docs/getAudioPorts-reply-0925.txt`。事务码 11/15 与 §2 表全部与实包对账。

**取服务这条路，各形态在本机 ksu/root-shell 下的死活（全部今日实测）**：

| 客户端形态 | 结果 |
|---|---|
| `service call`(C++ service 二进制, su 或 shell uid) | ✅ 查询/transact 都通 |
| NDK `AServiceManager_getService(IModule/default)` | ❌ 返回 AIBinder 空壳（mImpl=0），`prepare=-38`；`associateClass` 补上(r=0)仍 -38 |
| NDK 同一函数查 **IBluetoothHci**（蓝牙桥） | ✅ isRemote=1（**同一颗 NSI，audio 就是拿不到**） |
| dlopen libbinder 的 C 探针（stub 齐：self/getContextObject/asInterface 全过） | ❌ BpServiceManager::getService 恒返回空 sp（shell uid 同） |
| DT_NEEDED 链接 libbinder 的 C 探针 | ❌ 同上一步骤时崩（`defaultServiceManager` 里 sret 陷阱仍在）+ 空 sp |

**已排除项**：SELinux 拒绝（无任何 avc，ksu 域 permissive）、服务名打错、descriptor 缺失、
线程池未启、TLS/装载差异（shell uid 一样空）、`waitForService`（同样空壳）。
**未排除的假说**（明晚从这里接）：MIUI servicemanager 对 `getService2`/`checkService2`（NDK 与裸 C++ 的新码）
**按调用方域过滤/返回空**（A15 新事务，非 AOSP 原生行为；`service` 二进制与桥恰好都不走这条路：
`service`=checkService 旧码，桥=目标 BT 服务在 vendor SM 上）。

**明晚开工序列（判据都自带反例）**：
1. C 探针换调 **checkService 的 LEGACY 事务码**（handle0 手拼 parcel：token `android.os.IServiceManager` +
   String16 名；码表从 `service` 二进制反汇编拿真值，别猜）——如果这通，M1 直接基于自建 binder 客户端做；
2. 或者：NDK 探针 `AServiceManager_getService("media.audio_policy")`（framework 原生服务）——
   若也是空壳，坐实"SM 对 NDK 路过滤"，改走 1；
3. M1 = openOutputStream(15)：参数 = 从 getAudioPorts 回包**原样搬运**的 AudioPort/AudioConfig 字节
   （service call 拿 hex → 我们回写同款字节），全程免逆结构。


## 8. 【09-25 深夜 ★M0 正式闭环】audio-probe-ndk 全通

`su -c /data/local/tmp/audio-probe-ndk` → prepare st=0 / transact(11) st=0 / exception=0 /
reply=19164B（与 service call 金样字节数一致，52 端口 + speaker/bt_a2dp_speaker 等名字在包内）。

**"空壳"公案定案**：NDK binder 规矩 = `AServiceManager_getService` 拿到的是**无 class 裸句柄**，
不先 `AIBinder_Class_define(descriptor)+associateClass` 就 Prepare 必 `-38`（桥源码注释原话，
第一轮就记下了却没执行到位）。`impl=0x0` 诊断线是按猜测偏移读 ABBinder 私有布局的**红鲱鱼**，
布局本身没验证过——"用未验证的偏移下结论"和探针不自证是同一种病。

**下一步 M1**（全零静音帧验证，禁外放）：
1. 从 §8 的 19164B 回包里切出 speaker device-port 段（id=23, name="speaker",
   profile 48k/S16_LE/2ch，dumpsys 已有对账），构造 openOutputStream(15) 的 AudioConfig：
   **直接搬运回包字节**，不逆结构；
2. IStreamOut 方法码表已就位（§2）；数据环 createMmapBuffer 的码用同一手法反汇编拿；
3. ring 写入用零帧，判据=readBack 指针推进 + HAL 无错，不看耳朵。


## 9. 【09-25 深夜】M1b 战况与转场决策（r_submix 试验场）

- 两次手搓 AudioConfig 试探均 `st=0x80000008(EX_TRANSACTION_FAILED)`——HAL 在 unmarshal 前部即拒，
  **未建流未出声**（对用户 A2DP 视频流零扰动已验证；此前一次杂音系用户视频自身，误会解除）。
- 取"标准 wire 序"的静态路线全部碰壁：`libaudioaidlcommon.so`/`android.hardware.audio.common-V3-ndk.so`
  都只是薄桩（readFromParcel 在 NDK 后端=header inline，编进每个使用者体内）；
  `libmedia.so` 亦无。=> 反汇编 HAL 体内联代码成本过高，弃。
- **定案：M1 试验场转移到 `IModule/r_submix`**（虚拟混音模块：无硬件、无声、随便试错），
  字段序在它身上试对之后原样移植 `default`。它的 AudioPort/AudioPortConfig 同型同序。
- 附带情报：`android.hardware.audio.core-V2-ndk.so` 导出 52 个 `readFromParcel`，含
  **StreamDescriptor/FMQ/AudioBuffer/Position/Reply/Command** 全套——M2 的 ring 协议直接 dlsym 这些
  读回包（它们是真码不是 inline）。
- 用户在看视频（A2DP 活跃）：今晚对 `default` 模块零试探；只读查询不限。
