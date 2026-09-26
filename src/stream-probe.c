/* stream-probe：直连 HAL 的数据面（M2）取证，全程零采样＝无声。
 * 链路（全部用原始 NDK binder，不碰 C++ ABI）：
 *   1) 用 argsloop 已验证的 88B 骨架发 openOutputStream(code 15) —— 词表同 AUTO：
 *      IN='A;1;8;0;0;L:2048;B;B'（size 自动回填；B=写一个 strong binder；L:n=int64）
 *   2) 从回包里 AParcel_readStrongBinder 取 IStreamOut 句柄
 *   3) 对它发 code 1 = getStreamCommon，倒回包并找出其中的 fd（fcntl 校验）
 *   4) 报出 fd/大小/可读偏移，供下一步 mmap + 写零帧
 * 用法：su -c 'SVC=android.hardware.audio.core.IModule/default IN="62;1;8;0;0;L:2048;B;B" /data/local/tmp/stream-probe'
 * 注意：只 open 不 write，理论上不建 AGM 播放图；功放上电偶发"咔哒"属已知（用户已同意此阶段）。 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void AIBinder;
typedef void AParcel;

static struct {
  void* ndk;
  void (*Proc_setMax)(uint32_t);
  void (*Proc_start)(void);
  AIBinder* (*SM_get)(const char*);
  void (*IncStrong)(AIBinder*);
  void* (*Class_define)(const char*, void*, void*, void*);
  int (*Associate)(AIBinder*, const void*);
  int (*Prepare)(AIBinder*, AParcel**);
  int (*Tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t);
  int (*wI32)(AParcel*, int32_t);
  int (*wI64)(AParcel*, int64_t);
  int (*wBinder)(AParcel*, AIBinder*);
  int (*rI32)(const AParcel*, int32_t*);
  int (*rBinder)(const AParcel*, AIBinder**);
  int (*rFd)(const AParcel*, int*);
  size_t (*dsize)(const AParcel*);
  int (*setPos)(AParcel*, int32_t);
} N;

#define S(f, name) do { *(void**)&N.f = dlsym(N.ndk, name); if (!N.f) fprintf(stderr, "missing %s\n", name); } while (0)

static void noop_create(void* a) { (void)a; }
static void noop_destroy(void* a) { (void)a; }
static int32_t noop_transact(AIBinder* b, uint32_t c, const AParcel* in, AParcel* out) {
  (void)b; (void)c; (void)in; (void)out; return 0;
}

/* 词表 → parcel（与 argsloop 同一语法：整数 / B / L:<n>），先写 size 占位最后回填 */
static void pack(AParcel* p, char* spec) {
  int32_t start = (int32_t)N.dsize(p);
  N.wI32(p, 0);
  for (char* tok = strtok(spec, ";"); tok; tok = strtok(NULL, ";")) {
    if (tok[0] == 'B' && !tok[1]) N.wBinder(p, NULL);
    else if (tok[0] == 'L') {
      int64_t v = tok[1] == ':' ? (int64_t)strtoll(tok + 2, NULL, 0) : 0;
      N.wI64 ? N.wI64(p, v) : (void)(N.wI32(p, (int32_t)v), N.wI32(p, (int32_t)(v >> 32)));
    } else N.wI32(p, (int32_t)strtol(tok, NULL, 0));
  }
  int32_t end = (int32_t)N.dsize(p);
  N.setPos(p, start);
  N.wI32(p, end - start);
  N.setPos(p, end);
}

static AIBinder* associate_as(const char* svc, const char* desc) {
  AIBinder* b = N.SM_get(svc);
  if (!b) return NULL;
  N.IncStrong(b);
  const void* cls = N.Class_define(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  N.Associate(b, cls);
  return b;
}

int main(int argc, char** argv) {
  (void)argc;
  const char* svc = getenv("SVC") ? getenv("SVC") : "android.hardware.audio.core.IModule/default";
  char* spec = strdup(getenv("IN") ? getenv("IN") : "62;1;8;0;0;L:2048;B;B");
  uint32_t flags = (uint32_t)strtoul(getenv("TXF") ? getenv("TXF") : "0x10", NULL, 0);  /* 0x10=ACCEPT_FDS */
  N.ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.ndk) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  S(Proc_setMax, "ABinderProcess_setThreadPoolMaxThreadCount");
  S(Proc_start, "ABinderProcess_startThreadPool");
  S(SM_get, "AServiceManager_getService");
  S(IncStrong, "AIBinder_incStrong");
  S(Class_define, "AIBinder_Class_define");
  S(Associate, "AIBinder_associateClass");
  S(Prepare, "AIBinder_prepareTransaction");
  S(Tx, "AIBinder_transact");
  S(wI32, "AParcel_writeInt32");
  S(wI64, "AParcel_writeInt64");
  S(wBinder, "AParcel_writeStrongBinder");
  S(rI32, "AParcel_readInt32");
  S(rBinder, "AParcel_readStrongBinder");
  S(rFd, "AParcel_readParcelFileDescriptor");
  S(dsize, "AParcel_getDataSize");
  S(setPos, "AParcel_setDataPosition");
  if (!N.Tx || !N.Prepare || !N.wI32 || !N.rBinder || !N.dsize) return 3;
  N.Proc_setMax(4);
  N.Proc_start();

  AIBinder* mod = associate_as(svc, "android.hardware.audio.core.IModule");
  if (!mod) { fprintf(stderr, "getService(%s) 空\n", svc); return 4; }

  /* 1) openOutputStream */
  AParcel* in = NULL; AParcel* out = NULL;
  if (N.Prepare(mod, &in)) { fprintf(stderr, "prepare 失败\n"); return 5; }
  N.wI32(in, 0);                       /* 异常头 */
  pack(in, spec);
  int st = N.Tx(mod, 15, &in, &out, flags);
  size_t osz = out ? N.dsize(out) : 0;
  printf("STEP1 open st=%d reply=%zu flags=%#x\n", st, osz, flags); fflush(stdout);
  if (st != 0 || !out) {
    /* 兜底：有些实现不接受 ACCEPT_FDS（实测 -22），退回 flags=0 再试一次 */
    in = NULL; out = NULL;
    if (N.Prepare(mod, &in)) return 5;
    N.wI32(in, 0);
    spec = strdup(getenv("IN") ? getenv("IN") : "62;1;8;0;0;L:2048;B;B");
    pack(in, spec);
    st = N.Tx(mod, 15, &in, &out, 0);
    osz = out ? N.dsize(out) : 0;
    printf("STEP1b open st=%d reply=%zu flags=0\n", st, osz); fflush(stdout);
    if (st != 0 || !out) return 6;
  }
  int32_t ex = -1;
  N.rI32(out, &ex);
  AIBinder* stream = NULL;
  int rbs = N.rBinder(out, &stream);
  printf("STEP2 reply: ex=%d readStrongBinder st=%d stream=%p\n", ex, rbs, (void*)stream);
  fflush(stdout);
  if (!stream) return 7;
  N.IncStrong(stream);

  /* 2) getStreamCommon(code 1) */
  AParcel* in2 = NULL; AParcel* out2 = NULL;
  if (N.Prepare(stream, &in2)) { fprintf(stderr, "prepare(stream) 失败\n"); return 8; }
  N.wI32(in2, 0);
  int st2 = N.Tx(stream, 1, &in2, &out2, flags);
  size_t s2 = out2 ? N.dsize(out2) : 0;
  printf("STEP3 getStreamCommon st=%d reply=%zu\n", st2, s2); fflush(stdout);
  if (st2 != 0 || !out2) return 9;
  int32_t ex2 = -1;
  N.rI32(out2, &ex2);

  /* 3) 顺序扫回包：每 4 字节试着读 int32 与 fd，把 fd 找出来（FMQ 的共享内存） */
  printf("STEP4 ex=%d 起逐槽扫描：\n", ex2); fflush(stdout);
  for (int32_t pos = 4; pos + 4 <= (int)s2; pos += 4) {
    N.setPos(out2, pos);
    int32_t v = 0xdeadbeef;
    int ri = N.rI32(out2, &v);
    int fd = -1;
    N.setPos(out2, pos);
    int rf = N.rFd ? N.rFd(out2, &fd) : -1;
    int okfd = (rf == 0 && fd >= 0 && fd < 1024 && fcntl(fd, F_GETFD) >= 0);
    if (ri == 0 && (okfd || (v >= 0 && v < 0x400000))) {
      printf("  @%-4d int=%-12d  fd=%d %s\n", pos, v, okfd ? fd : -1, okfd ? "<< 真 fd!" : "");
      fflush(stdout);
    }
  }
  printf("DONE\n");
  return 0;
}
