/* audio-probe-ndk：复刻蓝牙桥的精确调用序（只 dlopen libbinder_ndk；getService→
 * Class_define(noop 三件套)→associateClass→Prepare→writeNoException→transact）。
 * 桥在同类操作下成功，本探针若空壳则变量在本进程环境而非调用式。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AIBinder AIBinder;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;

static struct {
  void* h;
  AIBinder* (*SM_getService)(const char*);
  void (*Proc_setMax)(uint32_t);
  void (*Proc_startPool)(void);
  void* (*Class_define)(const char*, void*, void*, void*);
  int (*Associate)(AIBinder*, const void*);
  int (*Prepare)(AIBinder*, AParcel**);
  int (*Transact)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t);
  int (*Parcel_readInt32)(const AParcel*, int32_t*);
  size_t (*Parcel_dataSize)(const AParcel*);
  void (*IncStrong)(AIBinder*);
  void (*DecStrong)(AIBinder*);
} N;

#define S(f, n) do { *(void**)&N.f = dlsym(N.h, n); if (!N.f) { fprintf(stderr, "missing %s\n", n); return 1; } } while (0)

static void noop_destroy(void* u) { (void)u; }
static void* noop_create(void) { return (void*)1; }
static int noop_transact(AIBinder* b, uint32_t c, const AParcel* in, AParcel* out) {
  (void)b; (void)c; (void)in; (void)out; return 0;
}

/* M1a：在 11 号回包里找 name=="speaker" 的 AudioPortConfig 元素，
 * 原字节搬运进 openOutputStream(15)：parcel = ex0 | <obj bytes> | rate | fmt | mode | flags…
 * 判据：reply 有 binder 对象头（0x40 起）且异常=0。不写流=不出声。 */
static int find_speaker(const uint8_t* b, int len, int* hdr_off, int* size) {
  static const uint8_t pat[] = {0x07,0,0,0,'s',0,'p',0,'e',0,'a',0,'k',0,'e',0,'r',0,0,0};
  int o = 12;  /* ex|count|total 之后 */
  while (o + 8 <= len) {
    int ver, sz; memcpy(&ver, b+o, 4); memcpy(&sz, b+o+4, 4);
    if (ver != 1 || sz < 12 || sz > 4096 || o + sz > len) return -1;
    if (memmem(b + o + 8, sz - 8, pat, sizeof pat)) { *hdr_off = o - 4; *size = 4 + sz; return 0; } /* 含 vector 计数头? 元素头从 ver 起 */
    o += sz;
  }
  return -1;
}

/* ===== M1b：speaker 字节搬运开流（无声验证：只 open，不写数据）=====
 * 1) transact(11) 全字节抠回（AParcel_readByte 循环 + setDataPosition 回零）
 * 2) 扫 UTF-16 "speaker\0"，向前回贴 [ver=1][size] 对象头，切出元素
 * 3) openOutputStream(15) parcel = ex0 + 元素字节 + {rate 存在位=1, 48000, fmtType=1(PCM),
 *    pcm=1(INT_16), mode=0, flags=0}（AudioConfig 头部字段按常见顺序试探；失败就打印异常码）
 * 判据：reply exception==0 且头 4 字节是 flat binder 对象(0x40) = 流已开成。 */
/* AParcel 内部就是 { android::Parcel* }（libbinder_ndk 结构公开约定）：
 * 直接借内层 Parcel 的 readInplace/dataSize —— readByte 会在 binder 对象边界停，
 * 而回包里 object 之后还有我们要的 speaker 元素，必须整块拿。 */
static int parcel_spill(void* out, uint8_t* buf, int cap, int* lenOut) {
  static void* lb = 0;
  if (!lb) lb = dlopen("/system/lib64/libbinder.so", RTLD_NOW | RTLD_GLOBAL);
  void* inner = *(void**)out;
  size_t (*dsize)(const void*) = (size_t(*)(const void*))dlsym(lb, "_ZNK7android6Parcel8dataSizeEv");
  int (*setPos)(const void*, size_t) = (void*)dlsym(lb, "_ZNK7android6Parcel11setDataPositionEm");
  int (*rdI32)(const void*, int32_t*) = (void*)dlsym(lb, "_ZNK7android6Parcel9readInt32EPi");
  if (!inner || !dsize || !setPos || !rdI32) { printf("spill: inner=%p %p/%p/%p\n", inner, (void*)dsize, (void*)setPos, (void*)rdI32); return -1; }
  int n = (int)dsize(inner);
  if (n > cap - 4) n = cap - 4;
  for (int i = 0; i + 4 <= n; i += 4) {
    setPos(inner, i);
    int32_t v;
    if (rdI32(inner, &v)) { printf("spill: word %d blocked\n", i / 4); return -2; }
    memcpy(buf + i, &v, 4);
  }
  *lenOut = n;
  return 0;
}

static int find_speaker_elem(const uint8_t* b, int len, int* start, int* size) {
  static const uint8_t pat[] = {'s',0,'p',0,'e',0,'a',0,'k',0,'e',0,'r',0,0,0};
  for (int t = 16; t + (int)sizeof pat < len; t++) {
    if (!memcmp(b + t, pat, sizeof pat)) {
      /* 回扫对象头：int32 size 紧跟 int32 ver==1，且元素区覆盖 t */
      for (int o = t - 8; o > 12 && o >= t - 300; o -= 4) {
        int32_t ver, sz; memcpy(&ver, b + o, 4); memcpy(&sz, b + o + 4, 4);
        if (ver == 1 && sz >= 40 && o + sz >= t + (int)sizeof pat) { *start = o - 4; *size = sz + 8; return 0; } /* -4: 带上外层的 name 头? 先按 [ver][size] 起 */
      }
      return -2;
    }
  }
  return -3;
}

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/default";
  uint32_t code = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 11;
  N.h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  S(SM_getService, "AServiceManager_getService");
  S(Proc_setMax, "ABinderProcess_setThreadPoolMaxThreadCount");
  S(Proc_startPool, "ABinderProcess_startThreadPool");
  S(Class_define, "AIBinder_Class_define");
  S(Associate, "AIBinder_associateClass");
  S(Prepare, "AIBinder_prepareTransaction");
  S(Transact, "AIBinder_transact");
  S(Parcel_readInt32, "AParcel_readInt32");
  S(Parcel_dataSize, "AParcel_getDataSize");
  S(IncStrong, "AIBinder_incStrong");
  S(DecStrong, "AIBinder_decStrong");

  N.Proc_setMax(4);
  N.Proc_startPool();
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = N.Class_define(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (!cls) { fprintf(stderr, "Class_define failed\n"); return 3; }
  AIBinder* b = N.SM_getService(svc);
  if (!b) { fprintf(stderr, "getService null\n"); return 4; }
  N.Associate(b, cls);
  N.IncStrong(b);
  AParcel* in = NULL; AParcel* out = NULL;
  int st = N.Prepare(b, &in);
  printf("DIAG prepare st=%d\n", st); fflush(stdout);
  if (st == 0) {
    /* AIDL 调用写异常位 =0 */
    int (*writeI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
    if (writeI32) writeI32(in, 0);
    st = N.Transact(b, code, &in, &out, 0);
    printf("DIAG transact st=%d out=%p\n", st, (void*)out); fflush(stdout);
    if (out) {
      int32_t ex = -1, m = -1;
      N.Parcel_readInt32(out, &ex);
      N.Parcel_readInt32(out, &m);
      printf("DIAG exception=%d header=%d size=%zu\n", ex, m, N.Parcel_dataSize(out));
      printf("VERDICT: %s\n", (ex == 0) ? "OK 链路全通" : "链路通但异常非0");
      if (getenv("OPEN_STREAM")) {
        /* 不搬 blob（read 侧无导出）——照 dumpsys 明文手搓 AudioConfig：
           port=AudioPortConfig{id:53 portId:23 rate:48000 STEREO S16 out flags:0 device:speaker}
           + rate{1,48000} + format{1,PCM=1,INT_16=1} + mode0 + flags0 。
           对象头 [ver=1][size] 先按 196 占位，HAL 若校验 size 会报 EX_MARSHAL→再精修。 */
        AParcel* in2 = NULL; AParcel* out2 = NULL;
        int (*wI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
        int (*wStr16)(AParcel*, const void*) = (void*)dlsym(N.h, "AParcel_writeString");
        if (N.Prepare(b, &in2) == 0) {
          wI32(in2, 0);                 /* exception */
          /* AudioPortConfig 对象 */
          wI32(in2, 1);                 /* version */
          int sizeSlot = 0; wI32(in2, 0x7f7f);  /* size 占位（AIDL 读端按 size 跳，字段自洽即可） */
          (void)sizeSlot;
          wI32(in2, 53);                /* id */
          wI32(in2, 23);                /* portId */
          wI32(in2, 1); wI32(in2, 48000);       /* Int sampleRate = 48000 */
          wI32(in2, 1); wI32(in2, 0); wI32(in2, 3); /* ChannelLayoutMask = stereo(3) */
          wI32(in2, 1);                                /* format 存在 */
          wI32(in2, 1); wI32(in2, 1); wI32(in2, 0);   /* PCM, INT_16, encoding NONE */
          wI32(in2, 0);                                /* gain null */
          wI32(in2, 0); wI32(in2, 0);                  /* flags{input:0, output:0} */
          /* ext: device 变体 tag=? 先按 AudioPortDeviceExt{device{type OUT_SPEAKER=2? dumpsys 对账},addr""} 的常见平铺 */
          wI32(in2, 1);                 /* ext presence */
          wI32(in2, 0x14000000);        /* 试探：device.type=OUT_SPEAKER(20<<24 打包占位) —— 失败就按异常码回修 */
          wI32(in2, 0);                 /* connection null-ish */
          wI32(in2, 0);                 /* address "" */
          wI32(in2, 0);                 /* encodedFormats 空 vector */
          wI32(in2, 0); wI32(in2, 0); wI32(in2, 0); /* encapsulationModes/types + flags */
          /* AudioConfig 尾字段 */
          wI32(in2, 1); wI32(in2, 48000);  /* Int{48000} */
          wI32(in2, 0);                    /* audioMode NORMAL */
          wI32(in2, 0);                    /* flags */
          int st2 = N.Transact(b, 15, &in2, &out2, 0);
          printf("M1b openOutputStream st=%d\n", st2); fflush(stdout);
          if (out2) {
            int32_t ex2 = -1, hdr2 = -1;
            N.Parcel_readInt32(out2, &ex2);
            N.Parcel_readInt32(out2, &hdr2);
            printf("M1b reply ex=%d first=%#x %s\n", ex2, (unsigned)hdr2,
                   (ex2 == 0 && hdr2 == 0x40) ? "VERDICT-M1: 流开成(flat binder)" : "按 ex 码修字段序");
          }
        }
      }
    }
  }
  N.DecStrong(b);
  return 0;
}
