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


## 10. 【09-25 深夜 ★架构改道】AAudio 捷径实测通过，直连 HAL 降为后备

### 10.1 直连路线的最后事实（存档，别再从头猜）

反汇编设备自带 `/vendor/lib64/android.hardware.audio.core-V2-ndk.so`（md5 `1518b404…41b2`，
与设备逐字节一致）里**服务端**那份 `IModule::OpenOutputStreamArguments::readFromParcel`（`0x37670`），
得到 code=15 的权威入参布局（`BpModule::openOutputStream@0x3bdf0` 确认码 15 + `writeInt32(0)` 异常位 +
`Arguments::writeToParcel`）：

| 顺序 | 字段 | 读端落点 | 备注 |
|---|---|---|---|
| 1 | `i32 size` | 栈 | 校验 `size>=4`；且**每读一个字段前先查"已消费>=size"→ 跳末尾并返回 OK**（可用它做信封自检） |
| 2 | `i32 A` | `this+0` | 语义未定（试过 0/1/2/23/53） |
| 3 | `i32 presence` | 栈 | **必须非 0**，否则直接 `return 0x80000008`（= 我们一直看到的 `-2147483640`） |
| 4 | `SourceMetadata` | `this+8` | 自身=`[size][AParcel_readParcelableArray]`，数组=`[head][len][元素…]`（head/len 都试过） |
| 5 | `i32 presence` | 栈 | AudioOffloadInfo，=0 合法 |
| 6 | `i64` | `this+0x88` | 置 0 |
| 7/8 | 两个可空 binder | `+0x90/+0xa0` | `IStreamCallback` / `IStreamOutEventCallback`，写 `i32 0` |

**结论**：`ARGS2` 扫描（`A∈{0,1,2,53}×head∈{0,1}×pres=1` + 只发 `[size=8][A]` 的信封自检变体）
**全部 `st=0x80000008`**。按读端逻辑，最后那个变体本该在读完 A 后就"跳末尾返回 OK"，却也失败
⇒ **拒因不在 readFromParcel 体内**，而在更早的一层。剩余候选（下次再啃）：
`BpModule` 给 `AIBinder_transact` 传的 `flags=0x10000000`（我们一直传 0）；或该服务对 code 15
走的是 `FLAG_SENDING_REPLIES`/异步回包形态。要出声前须先约时间。

### 10.2 ★改道理由：audioserver 活着时，AAudio 一发即通

同一时刻设备上 `init.svc.audioserver=running`（双态共存），纯 NDK 探针 `aaudio-probe`：

```
STEP1 openStream rc=0
STEP2 sr=48000 ch=2 fmt=2 perf=10 sharing=1 burst=3844 cap=7688 device=2
STEP3 requestStart rc=0
STEP4 wrote totalFrames=153760 in 40 writes; framesWritten=153760 xrun=0
```

即 **48k/立体声/S16、LOW_LATENCY、拿到 EXCLUSIVE(mmap) 环、800ms 内 15.4 万帧零 xrun、输出设备 id=2**，
全程喂全零静音帧（无声音）。这条路的 binder/AGM/system_suspend/音量/路由全由平台自己干，
不需要 §10.1 那套手搓布局，也不需要 M2 的 AudioRingBuffer 协议。

**新架构**：容器 PipeWire（混音、按应用音量）→ sink 的 monitor → `pw-cat -r` 裸 PCM →
FIFO（`/data/local/tmp/audio.fifo`，两侧同路径可见）→ `aa-bridge`（bionic，dlopen libaaudio）→ AAudio → audioserver。

### 10.2b 桥本体已跑通（喂全零 = 静音，无声）

设备侧 `mkfifo` + `dd if=/dev/zero | aa-bridge`（MS=4000）：

```
STREAM sr=48000 ch=2 fmt=2 burst=3844 cap=7688 device=2
FIFO open，开始搬运（喂零=静音）
t=2225ms fed=99944 framesWritten=99944 xrun=0
FIFO EOF（容器侧收流）
DONE fed=157604 framesWritten=157604 xrun=0
```

即 **M1（开流）+ M2（数据面）在这条路上一起解决了**：不需要 createMmapBuffer 事务码、
不需要 AudioRingBuffer 协议、不需要手搓 AudioConfig。剩下的是接线与真实音频试听。

### 10.2c 传输层定案：环回 TCP（容器 rootfs 是 loop 镜像，FIFO 跨不过去）

- 容器 `/` 是 `/dev/block/loop51` 的 ext4 镜像 ⇒ 两侧**看不到同一个路径**，FIFO 方案作废（保留为后备）。
- 实测**容器与安卓共享 netns**：容器 `nc -l -p 44777` 能被安卓侧 `toybox nc 127.0.0.1 44777` 连上并收到数据
  ⇒ 用环回 TCP，无需任何挂载或 SELinux 放行文件路径。
- 容器侧默认 sink 就是 `#55 Anland remote speaker`（`wpctl status` 带 `*`），
  `pw-cat -r -a --target=55 --format=s16 --rate=48000 --channels=2 -` 4 s 抓到 749568 B 且**全零**
  （= monitor 可用，且当下无应用出声，安全）。
- 全链路实测（桥 `PORT=44777` 监听 + 容器喂 monitor，约 6 s）：

```
STREAM sr=48000 ch=2 fmt=2 burst=3844 cap=7688 device=2
SOURCE ready（喂零=静音）
t=2016ms fed=96100 framesWritten=96100 xrun=0
t=4021ms fed=192200 framesWritten=192200 xrun=0
DONE fed=284456 framesWritten=284456 xrun=0
```

284456 帧 / 6 s ≈ 47.4k 帧/s ⇒ **实时率正确、零 xrun、桥+TCP+PipeWire 三方都不掉帧**。
胶水脚本：`bin/aa-feeder.sh`（容器侧，自动挑默认 sink、断线重连）。

### 10.2d ★真出声已确认（09-26 00:10，用户口述"有声音"）

前置取证：`dumpsys media.audio_flinger` 显示音乐输出线程 `Output devices: 0x2 (AUDIO_DEVICE_OUT_SPEAKER)`
⇒ 确认会走板载喇叭而非任何耳机（当时 A2DP 无活动设备），放音前才跟用户约的时间。
实放：容器 `pw-play /tmp/beep.wav`（默认 sink 音量先降到 0.15）→ monitor → `nc` → `aa-bridge PORT=44777`
→ AAudio：**用户听到声音**，桥侧 `framesWritten` 与 fed 同步推进、`xrun=0`。
用完已把音量复原 0.95 / 解除 mute，设备上无残留进程。

### 10.3 待验（需要用户点头才能做的那一步）

- 接管轮里 Android 被 `stop`（class core 全停），届时 `start audioserver` 能否干净起来 =
  这条改道成立的**唯一前提**。§38 记过 audioserver 卡住的迹象，必须实跑一轮 takeover 才能定论，
  而跑 takeover 会杀掉/复活当前桌面 ⇒ **先征求用户同意**，不要自作主张。
- 若 `start audioserver` 不行，回退到 §10.1 的直连 HAL（继续攻 flags 那一层）。
- 容器侧接线（desk-takeover / desk-stop）、FIFO 权限、以及首帧真实音频试听：**约时间**再做。


## 11. 【09-26 00:37 轮 ★定案】轮内 audioserver 的真阻塞点 = `waitForService("activity")`

第二轮（改用 `sudo setsid` 正常起，GPU 已恢复）把 §38 那条旧结论**用栈证实了**：

```
#03 libbinder: CppBackendShim::waitForService(String16 const&)
#04 libactivitymanager_aidl: ActivityManager::getService()
#05 libactivitymanager_aidl: ActivityManager::linkToDeath()
#06 libaudiopolicyservice: AudioPolicyService::UidPolicy::registerSelf()
#07 libaudiopolicyservice: AudioPolicyService::onFirstRef()
#08 /system/bin/audioserver main+528
```

- 表现：`init.svc.audioserver=running`、进程活着（State S），但
  `service check media.audio_flinger` / `media.aaudio` = **not found** ⇒ AAudio `openStream` 卡死
  （本轮 `probe-rc=124`、桥拿不到流）。**轮内一声没出过**，全程零帧/超时。
- **上一轮（00:18）的 `AAUDIO-OK` 是竞态运气**：那次 `stop` 后 1–2 s 就 `ctl.start audioserver`，
  当时 system_server 还没走完退出、`activity` 名字还挂在 servicemanager 上。不能当设计。
- ⇒ `AUDIO_BRIDGE=1` 里那句 `ctl.start system_suspend + ctl.start audioserver` **本身不够**。

### 11.1 正在试的解法：轮内挂一个 `activity` 最小桩 binder

`src/activity-stub.c`：dlopen libbinder_ndk，`AIBinder_Class_define("android.app.IActivityManager")`
+ `AIBinder_new` + `AServiceManager_addService(b, "activity")`，`ALIVE` 秒后退出（交还时真 system_server
自己注册 `activity`，桩不留场）。已实测到的事实：

1. **SELinux 放行**：su 域 `add_service("activity")` 返回 `st=0`（没碰 enforcing）。
2. 挂上后 audioserver **立刻解阻塞并开始调用桩**（日志出现 `STUB-REQ code=0x6 / 0x2 / 0x4`，反复）。
3. 但 `media.audio_flinger` / `media.aaudio` 仍未注册 ⇒ 桩的**回复不被接受**：手搓 `AIBinder_Class`
   时异常头没人替我们写，而本设备 libbinder_ndk **不导出** `AParcel_writeNoException`/`writeExceptionCode`
   （只导出 `AParcel_writeStatusHeader`）。已改成显式 `AParcel_writeInt32(out,0)` = EX_NONE
   并顺带打请求码/前三个 int（提交 `ed6b4b7`），**下次开机第一件事就是在轮内验这条**。

### 11.2 两条教训（我自己的错，写死在这）

- **绝不用 `systemd-run` 起接管轮**：transient 单元默认 `DevicePolicy=auto` + `DeviceAllow` 空
  ⇒ 不在白名单的字符设备 open 全被 cgroup 拦（renderD128/kgsl/snd/input 一起废），
  kwin 直接软渲染（实测 `MESA-EGL: failed to open /dev/dri/renderD128: 权限不够` + `CAP_SYS_NICE` 被拒）。
  跑轮只能用 `desk-takeover.sh` 自带的 setsid 脱钩（`sudo setsid bash ...`）。
- **`test -r/-w` 不是设备可用性判据**：它走 `access(2)`，看不见 cgroup deny；判 GPU 节点必须**真 open**
  （`dd if=/dev/dri/renderD128 of=/dev/null bs=1 count=0`）。desk-takeover 里那条 `GPU-PERM` 探针
  因此历史上 FAIL 68 次也不可信，待改。本轮真 open 成功（节点 `crw-rw-rw- root:graphics`）。
