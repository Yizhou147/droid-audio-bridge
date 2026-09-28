[中文](README.md) | English

# droid-audio-bridge

Make sound come out of the **built-in speakers** in the Linux desktop on a Xiaomi Pad 8 Pro
(SM8750/piano) while it is in **DRM takeover mode**.

The idea in one sentence: the `stop` performed during a takeover round kills audioserver
(class core), but the vendor audio HAL `audiohalservice.qti` (class hal) stays alive — this
project acts as its binder client, opening an output stream directly through
`android.hardware.audio.core.IModule/default`. Data flows through the HAL's shared-memory ring
(AudioRingBuffer), completely bypassing the Android framework.

- Fact baseline / AIDL transaction code table / milestones / red lines:
  [`直连音频HAL方案.md`](直连音频HAL方案.md) (design doc, Chinese)
- Build: GitHub Actions (NDK cross-compile); artifacts are pushed to `/data/local/tmp/` and run there
- Sibling projects: droid-drm-takeover (display/input/network takeover), droid-bluetooth-bridge
  (Bluetooth), droid-pc-keyboard (keyboard)

> Verified only on the Xiaomi Pad 8 Pro (HyperOS / android15-6.6 vendor).
