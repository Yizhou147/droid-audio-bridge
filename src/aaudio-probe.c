/* aaudio-probe：接管轮能否走"公共 NDK + 活着的 audioserver"这条捷径的判据探针。
 * 全零静音帧，不发声。链路：AAudio_createStreamBuilder → set* → openStream →
 * requestStart → write(零帧) ×N → 读回实际协商参数/framesWritten/xrun。
 * 用法：su -c '/data/local/tmp/aaudio-probe' ；可调 APM=0|10 SR= CH= FMT= MS=
 * 只 dlopen libaaudio.so（NDK 稳定 ABI），不链任何平台库。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void AAudioStreamBuilder;
typedef void AAudioStream;

static struct {
  void* h;
  AAudioStreamBuilder* (*create)(void** outSlot);
  void (*setDirection)(AAudioStreamBuilder*, int32_t);
  void (*setSampleRate)(AAudioStreamBuilder*, int32_t);
  void (*setChannelCount)(AAudioStreamBuilder*, int32_t);
  void (*setFormat)(AAudioStreamBuilder*, int32_t);
  void (*setPerformanceMode)(AAudioStreamBuilder*, int32_t);
  void (*setSharingMode)(AAudioStreamBuilder*, int32_t);
  void (*setUsage)(AAudioStreamBuilder*, int32_t);
  void (*setContentType)(AAudioStreamBuilder*, int32_t);
  int32_t (*open)(AAudioStreamBuilder*, AAudioStream**);
  int32_t (*start)(AAudioStream*);
  int64_t (*write)(AAudioStream*, const void*, int32_t, int64_t);
  int32_t (*stop)(AAudioStream*);
  int32_t (*close)(AAudioStream*);
  int32_t (*getSampleRate)(const AAudioStream*);
  int32_t (*getChannelCount)(const AAudioStream*);
  int32_t (*getFormat)(const AAudioStream*);
  int32_t (*getPerfMode)(const AAudioStream*);
  int32_t (*getSharingMode)(const AAudioStream*);
  int32_t (*burst)(const AAudioStream*);
  int32_t (*capacity)(const AAudioStream*);
  int32_t (*xrun)(const AAudioStream*);
  int64_t (*written)(const AAudioStream*);
  int32_t (*deviceId)(const AAudioStream*);
  const char* (*resultText)(int32_t);
} A;

#define S(field, name) do { *(void**)&A.field = dlsym(A.h, name); \
  if (!A.field) fprintf(stderr, "missing %s\n", name); } while (0)

static int env(const char* k, int dflt) {
  const char* v = getenv(k);
  return v ? atoi(v) : dflt;
}

int main(void) {
  A.h = dlopen("libaaudio.so", RTLD_NOW | RTLD_GLOBAL);
  if (!A.h) { fprintf(stderr, "dlopen libaaudio: %s\n", dlerror()); return 2; }
  S(create, "AAudio_createStreamBuilder");
  S(setDirection, "AAudioStreamBuilder_setDirection");
  S(setSampleRate, "AAudioStreamBuilder_setSampleRate");
  S(setChannelCount, "AAudioStreamBuilder_setChannelCount");
  S(setFormat, "AAudioStreamBuilder_setFormat");
  S(setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode");
  S(setSharingMode, "AAudioStreamBuilder_setSharingMode");
  S(setUsage, "AAudioStreamBuilder_setUsage");
  S(setContentType, "AAudioStreamBuilder_setContentType");
  S(open, "AAudioStreamBuilder_openStream");
  S(start, "AAudioStream_requestStart");
  S(write, "AAudioStream_write");
  S(stop, "AAudioStream_requestStop");
  S(close, "AAudioStream_close");
  S(getSampleRate, "AAudioStream_getSampleRate");
  S(getChannelCount, "AAudioStream_getChannelCount");
  S(getFormat, "AAudioStream_getFormat");
  S(getPerfMode, "AAudioStream_getPerformanceMode");
  S(getSharingMode, "AAudioStream_getSharingMode");
  S(burst, "AAudioStream_getFramesPerBurst");
  S(capacity, "AAudioStream_getBufferCapacityInFrames");
  S(xrun, "AAudioStream_getXRunCount");
  S(written, "AAudioStream_getFramesWritten");
  S(deviceId, "AAudioStream_getDeviceId");
  *(void**)&A.resultText = dlsym(A.h, "AAudio_convertResultToText");

  if (!A.create || !A.open || !A.start || !A.write || !A.stop || !A.close ||
      !A.setDirection || !A.setSampleRate || !A.setChannelCount || !A.setFormat ||
      !A.setPerformanceMode || !A.setSharingMode || !A.getSampleRate || !A.burst ||
      !A.written || !A.capacity || !A.xrun || !A.getPerfMode || !A.getSharingMode ||
      !A.getFormat || !A.getChannelCount) { fprintf(stderr, "关键符号缺失\n"); return 3; }

  const int dir = env("DIR", 0);              /* 0=OUTPUT */
  const int sr = env("SR", 48000);
  const int ch = env("CH", 2);
  const int fmt = env("FMT", 2);              /* 2=PCM_I16 */
  const int apm = env("APM", 10);             /* 10=LOW_LATENCY */
  const int shr = env("SHR", 0);              /* 0=SHARED */
  const int ms = env("MS", 800);

  /* 本设备的 AAudio_createStreamBuilder 走"隐藏 out 槽" flavored 实现（实测：写 [x0]，返回码在 w0）。
   * 两种 ABI 形状都兜住：返回值非空就用返回值，否则用槽里的对象指针。 */
  AAudioStreamBuilder* slot = NULL;
  AAudioStreamBuilder* b = A.create((void**)&slot);
  if (!b) b = slot;
  printf("DIAG create=%p b=%p\n", (void*)A.create, (void*)b); fflush(stdout);
  if (!b) { fprintf(stderr, "createStreamBuilder null\n"); return 3; }
  A.setDirection(b, dir);
  A.setSampleRate(b, sr);
  A.setChannelCount(b, ch);
  A.setFormat(b, fmt);
  A.setPerformanceMode(b, apm);
  A.setSharingMode(b, shr);
  if (A.setUsage) A.setUsage(b, 1);            /* MEDIA */
  if (A.setContentType) A.setContentType(b, 0);

  AAudioStream* st = NULL;
  int32_t rc = A.open(b, &st);
  printf("STEP1 openStream rc=%d%s stream=%p\n", rc,
         (rc && A.resultText) ? A.resultText(rc) : "", (void*)st);
  fflush(stdout);
  if (rc != 0 || !st) return 4;
  printf("STEP2 negotiated: sr=%d ch=%d fmt=%d perf=%d sharing=%d burst=%d cap=%d device=%d\n",
         A.getSampleRate(st), A.getChannelCount(st), A.getFormat(st), A.getPerfMode(st),
         A.getSharingMode(st), A.burst(st), A.capacity(st), A.deviceId ? A.deviceId(st) : -1);
  fflush(stdout);

  rc = A.start(st);
  printf("STEP3 requestStart rc=%d%s\n", rc, (rc && A.resultText) ? A.resultText(rc) : "");
  fflush(stdout);
  if (rc != 0) { A.close(st); return 5; }

  int burst = A.burst(st);
  if (burst <= 0) burst = 192;
  int bytesPerFrame = ch * (fmt == 1 ? 4 : 2);   /* 1=PCM_FLOAT, 2=PCM_I16 */
  /* 全零静音帧：绝不出声 */
  size_t bufCap = (size_t)burst * bytesPerFrame + 4096;
  uint8_t* buf = calloc(1, bufCap);
  int64_t total = 0; int loops = 0;
  /* 每次写一个 burst，阻塞超时 200ms；总计约 ms 毫秒 */
  while (loops * 20 < ms) {
    int64_t w = A.write(st, buf, burst, 200 * 1000 * 1000);
    if (w < 0) { printf("STEP4 write rc=%lld%s\n", (long long)w,
                         (A.resultText && w > -1000) ? A.resultText((int)w) : ""); fflush(stdout); break; }
    total += w; loops++;
  }
  int64_t wrote = A.written(st);
  int xr = A.xrun(st);
  printf("STEP4 wrote totalFrames=%lld in %d writes; framesWritten=%lld xrun=%d\n",
         (long long)total, loops, (long long)wrote, xr);
  fflush(stdout);
  A.stop(st);
  A.close(st);
  free(buf);
  printf("VERDICT: %s\n", (total > 0 && loops > 4 && wrote > 0)
         ? "AAUDIO-OK: 接管轮可走 audioserver 捷径（M1 换路）" : "AAUDIO-PARTIAL: 见上面报错");
  return total > 0 ? 0 : 6;
}
