/* aa-bridge：容器侧 PCM → AAudio 外放（接管轮音频桥的实际产品形态）。
 * 取流两种：PORT=<n> 在 127.0.0.1:<n> 监听（容器与安卓共享 netns，实测互通）；
 * 或给一个 FIFO 路径（容器 rootfs 是 loop 镜像、两侧看不到同一个路径，故仅作后备）。
 * 数据面：读到的裸 PCM 攒满一个 burst 就 AAudioStream_write，阻塞写自带背压。
 * audioserver 活着即可，不需要碰 vendor HAL 的 binder 布局（见方案 §10）。
 * 用法：su -c 'PORT=44777 /data/local/tmp/aa-bridge'
 *       su -c '/data/local/tmp/aa-bridge /data/local/tmp/audio.fifo'
 * 调参：SR=48000 CH=2 FMT=2(PCM_I16)|1(PCM_FLOAT) APM=10(LOW_LATENCY)|0 SHR=0 MS=0(不限量)
 * 验证期一律喂 /dev/zero（静音），不发声。 */
#include <dlfcn.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

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
  int32_t (*open)(AAudioStreamBuilder*, AAudioStream**);
  int32_t (*start)(AAudioStream*);
  int64_t (*write)(AAudioStream*, const void*, int32_t, int64_t);
  int32_t (*stop)(AAudioStream*);
  int32_t (*close)(AAudioStream*);
  int32_t (*getSampleRate)(const AAudioStream*);
  int32_t (*getChannelCount)(const AAudioStream*);
  int32_t (*getFormat)(const AAudioStream*);
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
static int64_t nowms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int open_source(const char* path, int port) {
  if (port <= 0) return open(path, O_RDONLY);      /* FIFO：阻塞等容器侧打开写端 */
  /* 容器与安卓共享 netns（实测 toybox nc ↔ 容器 nc 互通）⇒ 环回 TCP 是最省事的传输 */
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (struct sockaddr*)&a, sizeof a) || listen(s, 4)) { close(s); return -1; }
  int c = accept(s, NULL, NULL);                   /* 单消费者：一次只收一个 feeder */
  close(s);
  if (c >= 0) {
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int big = 1 << 20;
    setsockopt(c, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
  }
  return c;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/data/local/tmp/audio.fifo";
  signal(SIGPIPE, SIG_IGN);
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
  S(open, "AAudioStreamBuilder_openStream");
  S(start, "AAudioStream_requestStart");
  S(write, "AAudioStream_write");
  S(stop, "AAudioStream_requestStop");
  S(close, "AAudioStream_close");
  S(getSampleRate, "AAudioStream_getSampleRate");
  S(getChannelCount, "AAudioStream_getChannelCount");
  S(getFormat, "AAudioStream_getFormat");
  S(burst, "AAudioStream_getFramesPerBurst");
  S(capacity, "AAudioStream_getBufferCapacityInFrames");
  S(xrun, "AAudioStream_getXRunCount");
  S(written, "AAudioStream_getFramesWritten");
  S(deviceId, "AAudioStream_getDeviceId");
  *(void**)&A.resultText = dlsym(A.h, "AAudio_convertResultToText");
  if (!A.create || !A.open || !A.start || !A.write || !A.burst || !A.written) return 3;

  const int sr = env("SR", 48000), ch = env("CH", 2), fmt = env("FMT", 2);
  const int apm = env("APM", 10), shr = env("SHR", 0);
  const int ms = env("MS", 0);                     /* 0 = 一直跑 */
  const int bpf = ch * (fmt == 1 ? 4 : 2);

  AAudioStreamBuilder* slot = NULL;
  AAudioStreamBuilder* b = A.create((void**)&slot);
  if (!b) b = slot;
  if (!b) { fprintf(stderr, "createStreamBuilder 失败\n"); return 4; }
  A.setDirection(b, 0);                            /* OUTPUT */
  A.setSampleRate(b, sr);
  A.setChannelCount(b, ch);
  A.setFormat(b, fmt);
  A.setPerformanceMode(b, apm);
  A.setSharingMode(b, shr);
  if (A.setUsage) A.setUsage(b, 1);                /* MEDIA */
  AAudioStream* st = NULL;
  int32_t rc = A.open(b, &st);
  if (rc != 0 || !st) { fprintf(stderr, "openStream rc=%d %s\n", rc,
                                  A.resultText ? A.resultText(rc) : ""); return 5; }
  printf("STREAM sr=%d ch=%d fmt=%d burst=%d cap=%d device=%d\n",
         A.getSampleRate(st), A.getChannelCount(st), A.getFormat(st),
         A.burst(st), A.capacity(st), A.deviceId ? A.deviceId(st) : -1);
  fflush(stdout);
  rc = A.start(st);
  if (rc != 0) { fprintf(stderr, "requestStart rc=%d %s\n", rc,
                          A.resultText ? A.resultText(rc) : ""); A.close(st); return 6; }

  int fd = open_source(path, env("PORT", 0));
  if (fd < 0) { fprintf(stderr, "取流失败 %s%s: %s\n", env("PORT", 0) ? "tcp-listen" : path,
                          "", strerror(errno)); A.stop(st); A.close(st); return 7; }
  printf("SOURCE ready（喂零=静音）\n"); fflush(stdout);

  int burst = A.burst(st); if (burst <= 0) burst = 192;
  size_t capbytes = (size_t)burst * bpf;
  uint8_t* buf = malloc(capbytes);
  size_t have = 0;
  int64_t t0 = nowms(), tReport = t0, framesFed = 0;
  while (1) {
    if (ms && nowms() - t0 > ms) break;
    if (have < capbytes) {
      ssize_t n = read(fd, buf + have, capbytes - have);
      if (n == 0) { printf("FIFO EOF（容器侧收流）\n"); break; }
      if (n < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "read: %s\n", strerror(errno)); break;
      }
      have += (size_t)n;
      continue;
    }
    int64_t w = A.write(st, buf, burst, 100 * 1000 * 1000);
    if (w < 0) { fprintf(stderr, "write rc=%lld %s\n", (long long)w,
                          (A.resultText && w > -1000) ? A.resultText((int)w) : ""); break; }
    memmove(buf, buf + (size_t)w * bpf, have - (size_t)w * bpf);
    have -= (size_t)w * bpf;
    framesFed += w;
    if (nowms() - tReport > 2000) {
      printf("t=%lldms fed=%lld framesWritten=%lld xrun=%d\n",
             (long long)(nowms() - t0), (long long)framesFed,
             (long long)A.written(st), A.xrun(st));
      fflush(stdout);
      tReport = nowms();
    }
  }
  printf("DONE fed=%lld framesWritten=%lld xrun=%d\n", (long long)framesFed,
         (long long)A.written(st), A.xrun(st));
  close(fd); A.stop(st); A.close(st); free(buf);
  return 0;
}
