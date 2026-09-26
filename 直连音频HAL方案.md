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

## 12. 【09-26 上午·静音推进】桩链把轮内 audioserver 救活了，AAudio 仍差一步；顺带修掉一个我自己的 ABI 错

### 12.1 桩链进展（全程零帧，一声未出）

`activity-stub` 支持 `STUB_NAME/STUB_DESC` 复用，轮内按 audioserver 的等待顺序**逐个补桩**：

| 桩 | 解开的那道等待 |
|---|---|
| `activity` (`android.app.IActivityManager`) | `UidPolicy::registerSelf()` |
| `sensor_privacy` (`android.hardware.ISensorPrivacyManager`) | `SensorPrivacyPolicy::registerSelf()`（同 `onFirstRef` 里下一行） |
| `permission` (`android.permission.IPermissionControl`) | `AudioTrackImpl::getCallingPackageName()`（客户端侧，libbinder `PermissionController::getService` 在 sleep 重试） |

结果：**audioserver 主线程跑完 `onFirstRef`，三个服务全部注册**且进程稳定不重启：

```
audioserver pid: 27017 -> 27017
media.audio_flinger : Service media.audio_flinger: found
media.aaudio        : Service media.aaudio: found
media.audio_policy  : Service media.audio_policy: found
```
HAL 侧也在正常应答（`AHAL_Module_QTI getAAudioMixerBurstCount: returning 2` 等，default/usb/r_submix/bluetooth 四模块枚举齐全）。

**还差的一步**：`aaudio-probe` 在轮内 `openStream` 仍失败 —— `APM=10(LOW_LATENCY)` 回
`-881 AAUDIO_ERROR_NO_SERVICE`，`APM=0/30`（传统 AudioTrack 路径）回 `-898 AAUDIO_ERROR_ILLEGAL_ARGUMENT`。
最大嫌疑＝**`permission` 桩的答案不够真**：`getPackagesForUid` 需要一个真包名（我们回空/补零 ⇒
传统路径判"参数非法"，direct 路径判"无服务"）。⇒ 下一步：让 `permission` 桩对 `getPackagesForUid`
回一个真 String16 数组（`EX_NONE | presence=1 | count=1 | len+UTF-16("com.android.shell")`），
而不是通用补零。次优先嫌疑：轮内 `hwservicemanager` 起不来（`init.svc.hwservicemanager=stopped`，
`ctl.start` 不生效），HIDL 侧查询全空。

### 12.2 我自己探针的真 bug（已修，并要**重新核验 anland 那两次结论**）

`AAudioStream_write` / `AAudioStream_getFramesWritten` 在本设备**返回 int32**（thunk 就一条
`b AAudioStreamBuilder_getFormat` 后 `ret`，只填 w0），我却按 `int64_t` 声明 ⇒ 高 32 位是垃圾；
另外 VERDICT 那行在 `A.close(st)` 之后还调 `A.written(st)`（用后读）。已提交修复（返回类型统一
int32，本量级无损；去掉用后读）。

**因此要如实修正 §10.2/§10.2b/§10.2c 的数字**："15.4 万帧""572756 帧/xrun=0" 这些计数的
低 32 位可信、但报告值不可全信；而 **`openStream rc=0` 与"你确实听到了声音"这两条是真结论**
（都在 open/write 正常返回的路径上）。⇒ 待办：拿修好的二进制在 anland 态**重跑一次**同一判据，
把数字重新钉一遍。

### 12.3 再下一层：桩链是 treadmill，不该继续补（判断依据）

- `permission` 桩**一次都没被调**（`grep -c STUB-REQ = 0`）⇒ `-881/-898` 与它无关。
- 关掉 mmap（`setprop aaudio.mmap_policy 0`）后传统路不再报错而是**挂在**：
  `AudioStreamBuilder::build → AudioSystem::getMmapPolicyInfos →
   mediautils::ServiceHandler::get<IAudioPolicyService>` 的 `condition_variable::wait_until`。
- 不是 SELinux：`logcat -b events` 里客户端 `find media.audio_policy` **无 avc 拒绝**
  （只有 `hal_audio_default` 读 `vendor_pd_locater_dbg_prop`/debugfs 这类 anland 也有的噪音）。
- 真因方向＝**audioserver 的 binder 线程被别的 `waitForService` 占住**：同一时间窗里看到
  `Waited one second for android.frameworks.sensorservice.ISensorManager/default`
  与 `Binder transaction to android.media.IAudioFlingerService ... took 5004ms. Reply bytes: 0`。
  `ISensorManager` 由 system_server 提供 ⇒ 轮内必缺 ⇒ 再补一个桩 ⇒ 再缺下一个。
- ⇒ **结论（待用户定方向）**：继续补桩是"往无底洞里填沙"，且假答案可能让 AudioPolicy 静默走偏。
  两条更靠谱的岔路：
  A) **让 audioserver（连带它依赖的 system_server 侧服务）压根不被 stop**：把轮里的整颗 `stop`
     换成按需逐个停（至少保 `surfaceflinger`+`zygote` 死、其余音频相关留着）——改动面大、需实测；
  B) **回到直连 HAL**（§10.1 已存档权威布局，只差 `flags=0x10000000` 这一层没试）——
     轮里 vendor HAL 本来就活着，不依赖任何 system_server 服务。

## 13. 【09-26 上午·静音】openOutputStream 试探的真实进展：三颗错码各归其位，但形状仍差一处

全部在 `IModule/r_submix` 上、零载荷无 fd、纯只读/静音；判据＝事务码 + HAL 是否打出实现日志/线程数。

| 试验（r_submix） | 载荷 | 结果 |
|---|---|---|
| code 11 `getAudioPorts`（0 入参） | `[0]` | st=0，回包 ex=0、**下一 int=4**、816B ⇒ 回包的 args-size 只含自身 ⇒ size 只覆盖 in-args |
| code 9 `getAudioPort(int)` | `[0][id]` | **st=0、16B 回复、ex=-3**；HAL 打 `getAudioPort: port id 0 not found` |
| code 9 同上 | `[0][8][id]` | 同上（多余 int 被忽略）⇒ **单 int 方法：第一个 int 就是参数，没有 size 信封、没有异常头** |
| code 15 ARGS2 | `[0][44][0][1][12][1][0][0][i64][0][0]` | `st=0x80000008` |
| code 15 ARGS4（删异常头） | `[44][0][1][12]…` | `st=-22` |
| code 15 ARGS5（删掉被当成 presence 的那个 int） | `[0][44][1][12][1][0][0][i64][0][0]` | **仍是 `0x80000008`** ⇒ "presence 错位"模型**证伪** |
| 任意方法 + `flags=0x10`(ACCEPT_FDS) | — | 一律 `-22` 且无回包 ⇒ **fd 假设撤回**（NDK 这条路不吃这个 flag） |
| HAL 是否被调到 | code 15 各形状 | `audiohalservice.qti` 线程 27→27、日志零条 ⇒ 全部死在**服务端 unmarshal**，实现没进去 |

已排除：字段数不对（-22 与 0x80000008 的切换证明异常头/参数起点判定是对的）、fd、`0x10000000` 这个 flags（11/18/15 全都传它，而 11 通）。
仍未定：`Arguments` 里 **SourceMetadata 的 `head/len` 口径**、i64 之前是否还有字段、以及"可继承 parcelable"的 `[size][version]` 顺序。

⇒ **纪律**：不再靠猜形状试错（每轮一次 CI 太贵）。下一步只有两条正路：
A) 拿**地面真值**——dlopen 平台自带 AIDL 客户端（`libaudiohal`/`AudioHalAidl`）在桥进程里直调，让平台自己打包（成了就连手搓都不用）；
B) 回到"让 audioserver（及 system_server 侧依赖）不被 stop"的路，AAudio 直接复用平台栈（anland 态已真出声，只差轮内服务保活）。

## 14. 【09-26 上午·B 路负结果，先记录再决策】轮内"冷启动整套框架"天生残缺

在同一轮里依次补齐后实测（全零静音，无一次出声）：

| 步骤 | 结果 |
|---|---|
| 补 4 个桩（activity / sensor_privacy / permission / ISensorManager 后 audioserver 冷启动 | `media.audio_flinger`、`media.aaudio`、`media.audio_policy` 全部 found；探针不再挂死、**立刻**回 `-881 AAUDIO_ERROR_NO_SERVICE` |
| 让栈沉降 55 s（HAL 28154、audioserver 28153 双次采样不变） | 仍 `-881` ⇒ 不是重启窗口竞态 |
| `dumpsys media.audio_policy` / `media.audio_flinger` | 引擎有 `Available output devices (3)`、`AudioOut_D`（handle 13，设备 0x2 SPEAKER）存在 ⇒ "没有输出"不成立，`-881`（NAME_NOT_FOUND 侧）另有其因 |
| 拉起 `hwservicemanager`（HAL 日志出现过 `... without hwservicemanager`） | **拉不起来**：`ctl.start hwservicemanager` 后仍 `stopped`、无 pid ⇒ 与 §38 记的 `system_suspend`"ctl.stop 后 start 拉不起"同类 |

⇒ **B 路真正的结论**：`stop` 之后靠 `ctl.start` 复原**不可能完备**——`hwservicemanager`/`system_suspend` 这类服务一旦被整颗 `stop` 带走就无法按名复活，
而音频栈要用到它们；再加 4 个桩也只是把 audioserver 的初始化糊过去，AAudio 仍拿不到流。
⇒ 于是 B 只剩一种可行形态：**不让 `stop` 碰它们**（把整颗 `stop` 改成按需逐个停），这需要改 `desk-takeover.sh` 并实跑一轮验证。
A 路（直连 HAL）不受此影响：HAL 在轮里本来就活着且可服务（`getAudioPorts` 稳定回 19164B/816B），只差 `Arguments` 那一处布局。

### 14.1 矩阵也是同码 ⇒ 失败点不在载荷字段序（重要负结果）

`ARGS6` 七种字段形状（有/无 SM presence、SM size 8/12/16、head 0/1、len 0/1、有无两个 binder 尾、
先 version 还是先 presence）**全部 `st=0x80000008`、回包 0 字节**；而 `ARGS2 env=1`（只发 size+A）
也是同码。唯一能改变错码的动作是**删掉异常头**（→ -22）。副作用侧同时为零：
`audiohalservice.qti` fd 数 74→74、`/dev/shm` 计数 0→0、线程数不变、HAL 零日志。

⇒ 结论：**服务端在"读完异常头之后、进入任何字段分支之前"就统一失败**，与载荷内容无关。
这把"猜字段序"这条路彻底关掉；剩下的可能是
(i) 该服务对 code 15 的**签名/版本≠我们反汇编的 V2 `OpenOutputStreamArguments`**（Android 16 的
    `openOutputStream(IOStreamConfig, out IStreamOut, IStreamCallback, IStreamOutEventCallback)` 形态），
    需要先确认运行中 HAL 的**实际 AIDL 版本**（`getInterfaceVersion`=码 0xFEFFFFFF，可直接问）；
(ii) 或该 HAL 把 `openOutputStream` 实现为**不支持/需先决条件**，在未分配任何资源前就抛
    `EX_SERVICE_SPECIFIC`（`0x80000000|8` 正是它的编码）。
下一步（便宜且判据明确）：**先问 `getInterfaceVersion` 与 `getInterfaceHash`**（码 0xFEFFFFFF / 0xFEFFFFFE，
都是零参数、无 fd），把 (i) 钉死；若版本不是 2，就直接按真实版本的 Arguments 重排。

## 15. 【09-26 决策】直连 HAL 方向**放弃**（成本实测），改走"不让 stop 杀音频栈"

A 路做到底的成本被实测钉死：
- `argsdump` 拿到平台真值：args 块 **88 字节**，其中两个回调槽是 **24 字节的 `flat_binder_object`**（不是 int）
  ⇒ 解释了我七种手搓形状全同码（`0x80000008`）的原因：块长与对象槽都不对。
- `hp-open`（构造平台 `BpModule` + x8 sret 垫片直调 `openOutputStream`）**事务成功**：`st=0`、无异常，
  `getAudioPorts` 自检证明 Bp 对象与 sret 处理可信（vector 被真实填出 1200B）。
- **但什么都没发生**：HAL 线程 35→35、fd 74→74、`/dev/shm` 0→0、`ret` 全零 ⇒ 全零 config 被实现
  静默走"返回空对象"的路径。要真开流必须在 C++ 里构造**嵌套 parcelable 真实对象**
  （AudioConfig→AudioPortConfig→AudioPort→`std::vector`/`std::string`/`sp<>`），等于自己实现半个
  `libaudiohal`。加上数据面还要自己写 AudioRingBuffer/FMQ。
⇒ 用户拍板：**不再走这条路**。A 的探索工具（argsdump/hp-open/argsloop）留在仓库里当证据与备用，
  不再为它跑 CI。

B 路的真实障碍也记清楚（§14）：**整颗 `stop` 之后 `hwservicemanager`/`system_suspend` 按名拉不回来**，
所以"轮内冷启动整套框架"不可能完备 ⇒ 唯一可行形态是**根本别 stop 它们**：
把 `desk-takeover.sh` 的 `run "stop"` 换成按需逐个停（至少保 audioserver / hwservicemanager /
system_suspend 存活），AAudio 复用 anland 态已验证的真链路（音量/路由/蓝牙全带）。
风险＝漏停某服务（当初用整颗 stop 就是为了干净），需实跑一轮定判据；备份 `desk-takeover.sh.bak-0926-audio`。

## 16. 【09-26 ★A 路实际已通到"差一个合法 id"】字节级复刻平台打包 + HAL 当裁判

- 用 `.rela.plt` + stub 的 `adrp/ldr` 反查 GOT，才拿到**真实**的读/写端调用顺序（objdump 按"最近前符号"贴 PLT 名，此前所有"调用顺序"结论都被它带偏过两次）。
- 由此定正：`Arguments` 块 = `[size][字段A=presId][pres=1][portConfig 块][pres(offload)=0][i64][IStreamCallback 对象][IStreamOutEventCallback 对象]`，
  **两个回调是 24 字节 `flat_binder_object`**（这就是真值 88 ≠ 我手搓 44 的全部原因；`AParcel_writeByte` 每字节还占 4 字节格，`H:` 得按 int32 写）。
- `argsloop AUTO` 自动回填 size ⇒ **算出块长正好 88**，与平台 `writeToParcel` 真值逐字节一致；
  `Arguments::readFromParcel` 收（`READ st=0`）、`BpModule::openOutputStream` 送达（`TRANSACT st=0`），
  并且 **HAL 实现真的跑起来并念出读到什么**：
  `openOutputStream: r_submix: port config id 8, has offload info? 0, buffer size 0 frames`
  → `port config id 8 does not correspond to an output mix port`（id 0/1/2/3/9/10/16/53 一律 `Line 972 Failed`）。
- 判明 r_submix 当不了试验场：它当前只有 1 条 port config（id 8），且**没有"输出 mix port"**——
  mix port 是按流存在的；轮里 audioserver 死了 ⇒ 没人往 submix 里放音 ⇒ 永远不会出现输出 mix port。
- ⇒ 下一步必须在 `IModule/default`（真喇叭所在模块）上取/试合法 port config id（dumpsys 里
  speaker device port id=23、portConfig id=53@48000/INT_16/STEREO）。**注意：在 default 上开流会走
  AGM 建图 + 给 smart amp 上电，属于"可能有一声/咔哒"的动作类别 —— 虽仍不写任何采样，做之前必须先跟用户约。**

## 17. 【09-26 11:02 ★M1 达成】接管轮内直连 HAL 开输出流成功（零采样＝无声）

`argsloop AUTO` 的 88 字节骨架 + 扫 portConfig id（56..100）后：

```
尾: hal=31538  fd: 74 → 77   threads: 35 → 36   /dev/shm: 0 → 0
```

- **fd +3、线程 +1 = HAL 真的建立了输出流**（AOSP/QTI 的 StreamOut 会开 FMQ/共享内存 fd 与混音线程）。
- 全程 **一个采样都没写** ⇒ 绝对无声；也没走 AGM 播放图（shm 未变，符合"只 open 不 write"）。
- 通过 `findPortIdForNewStream` 的 id（无报错）：**57、62、63、69、74、75、77、78、85、88、89、92**；
  被拒原因可分类：`already has a stream opened on it`（55/59/60/61 —— **接管前 audioserver 遗留的僵尸流**，
  `stop` 杀的、没走 close）、`does not correspond to a mix port`（53=喇叭 device port、58）、`not found`。
- 已知映射样本：`id 62 → portId 8, 48k, INT_24`；`id 63 → portId 11, 48k, INT_32`；`id 78 → portId 21, 16k`。

### 17.1 遗留与下一步（M2）

1. **这些流是我开出来的、没关** ⇒ 端口会被占（`maxActiveStreamCount: 1`）；重启安卓或杀 `audiohalservice.qti` 即清。
2. `ret`（`OpenOutputStreamReturn`）在 `TRANSACT st=0` 后前 6 个 int 仍是 0 ⇒ 下一步：把 ret 缓冲**按平台
   `Return::readFromParcel` 的偏移**读（或直接倒 ret 的 64 字节 hex），定位 `StreamDescriptor`/FMQ 指针；
   之后用 `createMmapBuffer`（码待取）或直接写 FMQ 的 AudioRingBuffer，**仍只写零帧**，判据＝HAL 侧
   readBack 指针/`/dev/shm` 增长、且 `dumpsys` 里流活跃。
3. 目标要挑**通向喇叭的 mix port**（需从 dumpsys 的 patch/route 里对出来），或先用任意一个把数据面打通。

## 18. 【09-26 M2 起点】手发包被排除项清单 + 正解＝从 Return 内存抠 IStreamOut

`stream-probe`（纯原始 NDK binder：open→取 stream binder→getStreamCommon→扫 fd）实测：

| 试验 | 结果 |
|---|---|
| 手发 88B 骨架 flags=0 | `st=0x80000008`（marshal 失败） |
| 手发同上 flags=0x10 / 0x10000010 | `st=-22`（BAD_VALUE，**ACCEPT_FDS 这条路本身被拒**） |
| 手发同上 flags=0x10000000（平台用的值） | 仍 `st=0x80000008` |
| **平台 `BpModule::openOutputStream`（§17）** | **成功**（HAL fd+3、线程+1） |

⇒ 差异不在 flags，而在**打包细节**（我手写的字节与平台 `writeToParcel` 的产物仍有出入：`B` 槽占 24B 数据的对齐、i64 前对齐等；`AParcel_writeByte` 每字节占 4 格这类坑）。
而"我手发字节 → 平台 `Arguments::readFromParcel` 解成结构体 → 平台 `BpModule` 重新打包发出"这条链**已经证明可用**（§17 的成功正是这条）。

### 18.1 M2 的正解（不再手发包）
1. openOutputStream 用 `BpModule`（平台打包）；
2. 从 `OpenOutputStreamReturn` 缓冲区里**扫出指针**：候选指针首槽 vptr 若等于库内 `BpStreamOut` 的 vtable 地址即命中，
   `+8` 处即 `ndk::SpAIBinder`（= `AIBinder*`）——命中判据：拿它对 **code 1 `getStreamCommon`** 发原始事务，
   返回 `st=0` 且回包非空即证明是 stream 句柄；
3. `getStreamCommon` 回包里用 `AParcel_readParcelFileDescriptor` 扫出 FMQ 的 fd（`fcntl(F_GETFD)` 校验）
   —— **该 API 已确认存在**；
4. `mmap(fd)` 后只写**全零帧**，判据＝HAL 侧 readBack 推进 / `/dev/shm` 出现 AGM 缓冲（现在 shm 一直 0，
   因为从未 write）。

### 18.2 已确认的静态事实（省得再查）
- `IStreamOut` 码表（PLT 反查 GOT 得到，可信）：1 `getStreamCommon`、2 `updateMetadata`、3 `updateOffloadMetadata`、
  4/5 `get/setHwVolume`、8/9 `get/setDualMonoMode`、10 `getRecommendedLatencyModes`、11 `setLatencyMode`、
  12/13 `get/setPlaybackRateParameters`、`getInterfaceHash=0xFEFFFFFE`；**没有 write()、没有 createMmapBuffer**
  ⇒ 数据面只能走 `StreamDescriptor`/`AudioRingBuffer`（FMQ），其 fd 从 `getStreamCommon` 回包里取。
- 遗留：我在 `default` 上开了 10+ 条流没关（fd 74→77、线程 35→36，端口被 `maxActiveStreamCount:1` 占住）；
  清法＝重启安卓或 `pkill -x android.hardware.audio.service`（**属改动设备状态，等用户点头**）。

## 19. 【09-26 11:12 干净 sweep】修正 §17/§18 的两处归因，并给出卡点

每个 id **单独清日志 + 单独记 fd 增量**重跑（12 个 id）：

```
底 fd=77 thr=36
id=54 already has a stream opened      id=55 already has a stream opened
id=56 existing port config id 56 not found   id=57 同 not found
id=58 does not correspond to a mix port
id=59/60/61/62/63 already has a stream opened
id=64/65 not found
```

由此定案三件事：
1. **§17 的"哪个 id 成功了"归因错了**：那是 `logcat -t 3` 窗口太窄造成的假象（同一条错误被下一条覆盖）。
   真实情况＝11:02 那次 sweep 里**至少开出了 3 条流**（fd 74→77、线程 35→36 是真的），但**具体哪个 id** 没钉住。
2. **`st=0` 不是成功证据**：QTI 的 `openOutputStream` 失败时**吞掉错误、回 OK+空对象**
   （`port config id 92 ... not found` 之后仍 `TRANSACT st=0`、`ret` 全零）。
   ⇒ 唯一可信判据是 **HAL 的 fd/线程/`/dev/shm` 增量**（+ 之后 readBack 推进）。
3. **字段编码已被 HAL 原样念出，确认无误**：`port config id 92, has offload info? 0, **buffer size 2048 frames**`
   （id / offload presence / i64 buffer size 三处都对上）⇒ 88B 骨架这块可以定案收尾。

**当前卡点**：所有合法 mix port 都被"已开流"占满（我 sweep 开的 + 接管前 audioserver 的僵尸流），
`STREAM` 模式因此拿不到句柄（回包空）。**要清场只能重启音频 HAL**
（`pkill -x android.hardware.audio.service` 之类，init 会自动拉回，属"改动设备运行状态"，等你点头再做）。
清场后一次只试一个 id：fd 增长＝开成 → 抽 `IStreamOut` 句柄 → `getStreamCommon` 取 FMQ fd → mmap → 写零帧。

## 20. 【09-26 11:2x】RAWX 隔离实验：原始 binder 路线被卡在 aux/objects 层

1. **id=63 是真正能开流的 portConfig**（HAL 重启后重放：`fd 74→77`、线程 +1、无任何报错）；
   54–62 是 HAL 启动时**自己预占**的（`already has a stream opened on it`）⇒ 53/58 非 mix、56/57/64/65 not found。
2. **数据包无罪**：`RAWX` 用 `AIBinder_prepareTransaction` + **平台自己的** `Arguments::writeToParcel`
   生成请求（与 Bp 路径同源、逐字节一致），再用我的原始 `AIBinder_transact`：
   flags=0 / 0x10000000 / 0x10000010 ⇒ 全失败（`0x80000008`、`-22`），而同一结构体交给
   `BpModule::openOutputStream` **就成功**。
3. 又纠正一处我今天反复踩的坑：objdump 给 `bl` 贴的 PLT 名不可信，用 GOT 反查后确认
   Bp 路径调用链就是 `AIBinder_prepareTransaction → AParcel_writeInt32 → Arguments::writeToParcel
   → AIBinder_transact(flags=0x10000000) → AParcel_readStatusHeader → AStatus_isOk` —— 与我做的**没有区别**。
   ⇒ 剩下的唯一差别只能是 **parcel 的 objects/aux 区**（两个 null binder 槽在 aux 里有真实条目，
   我用 `H:` 写零或 `writeStrongBinder(NULL)` 都造不出与平台一致的 aux 状态；`readFromParcel` 解出的
   结构体里那两个槽已被物化成对象，再由平台写回 ⇒ aux 与我手包不同）。
   ⇒ **原始 binder 路线（自己发 transact 再 `AParcel_readStrongBinder` 取流句柄）到此走不通。**

### 20.1 还能走的两条（都不需要 C++ ABI 逆向）
- **A：符号插桩（interposition）**：在我自己的可执行文件里导出同名 `AIBinder_transact`，
  平台 Bp 码通过 PLT 调用时会绑到我的实现 ⇒ 我**转发**并顺手把 `reply` parcel 倒出来
  （读 `AParcel_readParcelFileDescriptor` 拿 FMQ fd）。既保住"平台打包"，又拿到原始回包。
- **B：继续用平台方法往下调**：`BpStreamOut::getStreamCommon(this, &sp)`（对象就在 `ret+0`，
  已见其指针），再由 `StreamCommon`/`StreamDescriptor` 一层层交回平台函数解 —— 但取 fd 仍需布局，
  所以 **A 更划算**。

## 21. 【09-26】CAP 插桩为何没触发（+ 唯一可行的替代：改 GOT）

- `AIBinder_transact` 确实导出进了可执行文件的 `.dynsym`，但**插桩没被调用**（`[插桩]` 一行没有）。
- 原因＝bionic 的查找顺序：`core-V4-ndk.so` 的 `DT_NEEDED` 里就有 `libbinder_ndk.so`，
  符号在**它自己的依赖链里就满足了**，走不到可执行文件所在的全局作用域 ⇒ classic interposition 在这条链上无效。
- 可行的替代＝**改 GOT**：从 `/proc/self/maps` 拿 `core-V4-ndk.so` 的加载基址，
  按 `readelf -r` 里那条 `R_AARCH64_JUMP_SLOT`（`AIBinder_transact`）的 GOT 偏移，
  `mprotect` 后把槽写成我们自己的钩子（**只改本进程内存，不碰任何文件/分区**）。
  钩子里 `转发 + 抄 reply` ⇒ 拿到 `IStreamOut` 句柄与 FMQ 的 fd。
- 本轮结论不变且已实证：**id=63 能在接管轮内开成流**（`fd 74→77`，线程 +1，零采样＝无声；
  已两次重启 HAL 复现）。M2 只剩"从平台手里接过 reply"这一步。

## 22. 【09-26 12:4x ★M2 闭环 + 数据通道现身】GOT 钩子打通，回包里就是三块 FMQ

### 22.1 前几轮 "GOT 一个槽都扫不到" 的真因（一次诊断彻底解决）

新诊断版把 bias、每段映射、槽现值和 `dladdr` 结果全打出来，结论：

```
[GOT] 段=4 bias=7aa12de000 槽=7aa1339400（+5b400）应含 real=0x7aa1427b88
[GOT] 槽现值=0x5693642d40 是 AIBinder_transact（/data/local/tmp/argsloop）   ← 注意 fname
[GOT] real 在 /system/lib64/libbinder_ndk.so（AIBinder_transact）
```

**core-V4-ndk.so 里 `AIBinder_transact` 的 GOT 槽装的不是 libbinder_ndk 的地址，而是我自己可执行文件
`/data/local/tmp/argsloop` 里的一个 PLT 桩。** 起因：build.sh 给 argsloop 加了 `-Wl,--export-dynamic`，
于是"我只 import 不定义"的这个符号，被链接器当成**由可执行文件提供的全局定义**（PLT 桩即定义），
core-V4 的引用按全局作用域优先绑到了它身上。

⇒ 一次性解释掉两条老结论：§21 的"插桩从来不触发"和"值扫描零命中"都不是路线错，
而是**槽里根本没有我以为的那个值**。顺带说明：bionic 的 lazy-PLT 猜测（§21 结尾）是多余的，
这库是 full-RELRO/BIND_NOW，槽在载入时就写好了。

### 22.2 定位槽的静态依据（可复用于任何 .so）

`objdump` 的 `@plt` 标签在这里**全部错位**（已知病：标签取最近的前置符号）。正确做法
= 解 `.rela.plt` 拿"槽偏移→符号名"，再解 `.plt` 指令流拿"桩→槽"：

| 事实 | 值 |
|---|---|
| `AIBinder_transact` 的 GOT 槽静态 vaddr | **0x5b400**（`.got.plt` 起 0x5b2f0） |
| 该符号的 PLT 桩 | 0x55ed8（`adrp/ldr/add/br`，16B 一条，符号标签落在 adrp-8） |
| 调用点 | `BpModule::openOutputStream+0xdc → bl 0x55ed8`；同函数 0x36f80 那条 `bl 0x55ea8` 是 **AIBinder_prepareTransaction**（objdump 标成 `AParcel_writeStrongBinder@plt+0x8`，错的） |

槽地址 = 最小映射起点 + 0x5b400，`mprotect` 后写钩子，实测读回成功。

### 22.3 钩子抓到权威回包（接管轮内，零采样＝无声）

```
[GOT] reply=533 字节 ex=0
[PFD@168 = 9 -> /dev/ashmem254f0b20-… ]  [PFD@332 = 10 -> 同]  [PFD@492 = 11 -> 同]
mmap 大小：fd9=4096  fd10=4096  fd11=16384      头部 48 字节当前全 0（还没人写读过）
TRANSACT(平台自己解析) ScopedAStatus st=0，HAL fd 74→77
```

- 之前所有失败尝试的回包都只有 **13 字节**（ex+一个 int）；533 字节 = 真内容。
- 判 fd 的权威办法不是"整数看着像 fd"（`hunt_fds` 那版全是假阳性：fd 1 就是本进程自己的日志文件），
  而是**让 `AParcel_readParcelFileDescriptor` 逐位置试读** —— 只有对象表里那个位置真是 `TYPE_FD` 才会成。
- **ashmem 的 `st_size` 恒为 0** ⇒ 大小只能 mmap 倍增试探（4096→8192 失败即定界）。

### 22.4 IStreamOut / IStreamCommon 码表（从 Bp 类的 AIDL 声明序数出，别再猜）

`IStreamOut`（**没有任何数据通道方法**）：1 getStreamCommon, 2 updateMetadata, 3 updateOffloadMetadata,
4 getHwVolume, 5 setHwVolume, 6 getAudioDescriptionMixLevel, 7 setAudioDescriptionMixLevel,
8 getDualMonoMode, 9 setDualMonoMode, 10 getRecommendedLatencyModes, 11 setLatencyMode,
12 getPlaybackRateParameters, 13 setPlaybackRateParameters, 14 selectPresentation, (+getInterfaceVersion/Hash)。

`IStreamCommon`：1 close, 2 prepareToClose, 3 updateHwAvSyncId, 4 getVendorParameters,
5 setVendorParameters, 6 add, 7 remove, **8 createMmapBuffer**。
实测对 8 发过去 **status=-74 UNKNOWN_TRANSACTION** ⇒ 本机 HAL 不支持 mmap 路线（数据通道就是上面那三块 FMQ）。

（`BpStreamOut` 的 vtable 里 `getStreamCommon` 落在第 5 格，**vtable 序号 ≠ 事务码**，别拿它当码表。）

### 22.5 M2a：句柄怎么拿（以及为什么之前 `maybe==NULL`）

平台自己解析后的 `OpenOutputStreamReturn`（我给的 `ret` 缓冲）里：

```
RETSHEAP 0=0xb400007a…5b0*   ← vptr 解出来正是 _ZTVN4aidl…core11BpStreamOutE
```

- **`ret+0` 就是活的 `BpStreamOut` 对象** —— 之前三轮"结构里没有句柄"的判断是假的：
  我的指针过滤器写的是 `< 2^48` 才算指针，而 scudo 的堆指针带顶层 tag（`0xb40000XXXXXXXXXX`），
  **全被自己滤掉了**。改成"该地址在 `/proc/self/maps` 某条可读映射里（先去 tag）"才算数。
- 句柄在对象里的位置**不能猜**：`obj+8` 是引用计数（`0xffffffffffffffff`），
  实测 `AIBinder*` 在 **`obj+32`**（判据：它的 vptr 落在 `libbinder_ndk.so`）。`BpStreamCommon` 同样在 +32。
- 拿它对 `IStreamOut` 发 **码 1 getStreamCommon → st=0，reply=32 字节** ⇒ 再取 `IStreamCommon` 句柄。
  解析用**平台自己的 `BpStreamOut::getStreamCommon(shared_ptr*)`**（按值返回 `ScopedAStatus` ⇒ 走隐藏 x8），
  比手解回包可靠：手解那条路（`IStreamCommon::readFromParcel` + 手工 `AIBinder_readStrongBinder`）
  要么把进程打崩、要么静默失败（`AParcel_readStrongBinder` 在这两个位置都拿不到句柄，原因未追）。
- SELinux：进程仍在 `u:r:shell:s0`（KernelSU 的 su 只给 uid 不给域），
  而对 `IModule`、`IStreamOut`、`IStreamCommon` 的事务**都通了**；dmesg 里那条
  `denied { call } shell→hal_audio_default` 是噪音（同域同期调用成功）。

### 22.6 下一步（M3）

回包里的三个 164B 块 = `StreamDescriptor` + FMQ 几何。core-V4 恰好导出
`StreamDescriptor::{readFromParcel, AudioBuffer, Reply, Command, Position}` 全套解析符号 ⇒
**继续用平台当解码器**：把 `op` 定位到各块起点调这些函数，拿到
元素大小/保留计数/读写位置偏移，然后按 FMQ 协议写零帧，判据＝共享内存里的计数器推进 + HAL 无错。
M3 之后才是 `aa-bridge` 换成"HAL sink"并接进 desk-takeover/desk-stop。
