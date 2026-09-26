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
  int (*setPos)(const void*, size_t) = (void*)dlsym(N.h, "AParcel_setDataPosition");
  int (*rdByte)(const void*, int8_t*) = (void*)dlsym(N.h, "AParcel_readByte");
  size_t (*dsize)(const void*) = (size_t(*)(const void*))N.Parcel_dataSize;
  if (!setPos || !rdByte) { printf("spill: no pos/byte api\n"); return -1; }
  int n = (int)dsize(out);
  if (n > cap) n = cap;
  int i = 0, stall = 0;
  while (i < n) {
    setPos(out, (size_t)i);
    int8_t c;
    if (rdByte(out, &c) == 0) { buf[i++] = (uint8_t)c; stall = 0; continue; }
    /* 读不动：多半撞对象界 —— 步进 4 探路，连 16 步不动才算失败 */
    stall++;
    if (stall > 4 && i + 4 < n) { buf[i] = buf[i+1] = buf[i+2] = buf[i+3] = 0xEE; i += 4; stall = 0; continue; }
    if (stall > 20) { if (i + 8 >= n) { *lenOut = i; return 0; } printf("spill stuck @%d\n", i); return -2; }
  }
  *lenOut = n;
  return 0;
}

static int find_elem_named(const uint8_t* b, int len, const char* want, int* start, int* size) {
  static const uint8_t pat[] = {'s',0,'p',0,'e',0,'a',0,'k',0,'e',0,'r',0,0,0};
  (void)want;
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
        static uint8_t blob[65536]; int blen=0;
        int src = parcel_spill(out, blob, sizeof blob, &blen);
        int es=-1, esz=-1;
        /* 扫第一个 ver=1 && 48<=size<=200 且含 "speaker"/name 的元素当模板 */
        for (int o8 = 8; o8 + 8 <= blen; ) {
          int32_t v2, s2; memcpy(&v2, blob+o8, 4); memcpy(&s2, blob+o8+4, 4);
          if (v2 != 1 || s2 < 48 || s2 > 4096 || o8+s2 > blen) { printf("M1c walk bad @%d %d %d\n", o8, v2, s2); break; }
          int fr = find_elem_named(blob, blen, "speaker", &es, &esz);
          (void)fr;
          es = o8; esz = s2;
          break;  /* 模板=第一个元素（AudioPortConfig/或 port） */
        }
        if (src == 0 && esz > 0) {
          printf("M1c template elem@%d size=%d (blen=%d)\n", es, esz, blen); fflush(stdout);
          AParcel* in2 = NULL; AParcel* out2 = NULL;
          int (*wI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
          int (*wByte)(AParcel*, int8_t) = (void*)dlsym(N.h, "AParcel_writeByte");
          if (N.Prepare(b, &in2) == 0) {
            wI32(in2, 0);
            for (int i = 0; i < esz; i++) wByte(in2, (int8_t)blob[es + i]);   /* port=AudioPortConfig 原样 */
            wI32(in2, 1); wI32(in2, 48000);      /* Int sampleRate */
            wI32(in2, 1); wI32(in2, 1);           /* format{PCM} 粗试 */
            wI32(in2, 0); wI32(in2, 0);           /* mode, flags */
            uint32_t txf = (uint32_t)strtoul(getenv("TXF") ? getenv("TXF") : "0", NULL, 0);  /* 0x10=ACCEPT_FDS：openOutputStream 回包带 FMQ 的 fd */
          int st2 = N.Transact(b, 15, &in2, &out2, txf);
            printf("M1c openOutputStream st=%d\n", st2); fflush(stdout);
            if (out2) {
              int32_t ex2=-1, h2=-1;
              N.Parcel_readInt32(out2,&ex2); N.Parcel_readInt32(out2,&h2);
              printf("M1c reply ex=%d first=%#x %s\n", ex2, (unsigned)h2,
                     (ex2==0 && h2==0x40) ? "VERDICT-M1: 流开成！" : "按码续修");
            }
          }
        }
      }
      if (getenv("GETPORT")) {
        /* 校准判据：getAudioPort(int) 只带一个 int32，用它一次定死"AIDL 参数块有没有 size 前缀"。
         * A=只写 int（无信封）；B=写 [size=8][int]（有信封）。谁回 ex=0 谁就是这套 AIDL 的规矩。 */
        int id = atoi(getenv("GETPORT"));
        int (*wI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
        for (int v = 0; v < 2; v++) {
          AParcel* in3 = NULL; AParcel* out3 = NULL;
          if (N.Prepare(b, &in3) != 0) break;
          wI32(in3, 0);
          if (v == 1) wI32(in3, 8);
          wI32(in3, id);
          int s3 = N.Transact(b, 9, &in3, &out3, 0);
          size_t sz3 = (out3 && N.Parcel_dataSize) ? N.Parcel_dataSize(out3) : 0;
          int32_t e3 = -1;
          if (out3) N.Parcel_readInt32(out3, &e3);
          printf("CAL9 %s st=%d ex=%d reply=%zu => %s\n", v==0?"A 无信封":"B 有信封", s3, e3, sz3,
                 (s3==0 && e3==0) ? "此形状正确" : (s3!=0 ? "被拒" : "回了异常"));
          fflush(stdout);
        }
      }
      if (getenv("ARGS2")) {
        /* 09-25 反汇编设备自带 core-V2 的 OpenOutputStreamArguments::readFromParcel 得到的权威服务端布局：
         *   [size][i32 A][pres][SourceMetadata][pres][AudioOffloadInfo?][i64][IStreamCallback][IStreamOutEventCallback]
         * 每读一个字段前先查"已消费 >= size"，是则跳到末尾并返回 OK（⇒ size 可用来做信封自检）。
         * SourceMetadata::readFromParcel = [size][AParcel_readParcelableArray]，数组 = [head][len][元素...]。
         * 下面把 A / head / pres 组合扫一遍，看哪一个能过 unmarshal。 */
        AParcel* in2;
        int (*wI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
        int (*wI64)(AParcel*, int64_t) = (void*)dlsym(N.h, "AParcel_writeInt64");
        struct var { int a, head, smPres, onlyEnv; };
        static const struct var vs[] = {
          {0, 1, 1, 0}, {1, 1, 1, 0}, {2, 1, 1, 0}, {53, 1, 1, 0},
          {0, 0, 1, 0},                  /* 数组 head=0（上一轮已试，预期 EX_MARSHAL） */
          {0, 1, 1, 1},                  /* 只发 [size=8][A]：验信封自检 */
        };
        for (unsigned k = 0; k < sizeof vs / sizeof *vs; k++) {
          const struct var* v = &vs[k];
          AParcel* out2 = NULL; in2 = NULL;
          if (N.Prepare(b, &in2) != 0) { printf("ARGS2 prepare 失败\n"); break; }
          wI32(in2, 0);                  /* exception 位 */
          if (v->onlyEnv) {
            wI32(in2, 8); wI32(in2, v->a);
          } else {
            int smBody = v->head ? 8 : 4;              /* [head][len] 或 [head=0] */
            int smSize = 4 + smBody;                   /* + 自己的 size 字段 */
            int total = 4 + 4 + 4 + smSize + 4 + 8 + 4 + 4;
            wI32(in2, total);            /* 参数块总字节（含本字段） */
            wI32(in2, v->a);             /* 字段 A */
            wI32(in2, v->smPres);        /* SourceMetadata presence */
            wI32(in2, smSize);           /* SourceMetadata size */
            wI32(in2, v->head);          /* 数组 head */
            if (v->head) wI32(in2, 0);   /* 数组 len = 0 */
            wI32(in2, 0);                /* AudioOffloadInfo presence = 无 */
            if (wI64) wI64(in2, 0); else { wI32(in2, 0); wI32(in2, 0); }
            wI32(in2, 0);                /* IStreamCallback null */
            wI32(in2, 0);                /* IStreamOutEventCallback null */
          }
          int st2 = N.Transact(b, 15, &in2, &out2, 0);
          int32_t ex2 = -1, h2 = -1;
          if (out2) { N.Parcel_readInt32(out2, &ex2); N.Parcel_readInt32(out2, &h2); }
          printf("ARGS2 TXF=%#x A=%d head=%d pres=%d env=%d → st=%d ex=%d first=%#x %s\n",
                 txf, v->a, v->head, v->smPres, v->onlyEnv, st2, ex2, (unsigned)h2,
                 (st2 == 0 && ex2 == 0) ? "VERDICT-M1: unmarshal 过了，看 first" : "");
          fflush(stdout);
          if (st2 == 0 && ex2 == 0 && out2) {
            static uint8_t rb[4096]; int rl = 0;
            if (parcel_spill(out2, rb, sizeof rb, &rl) == 0) {
              printf("ARGS2 reply %d 字节：", rl);
              for (int i = 0; i < rl && i < 96; i++) printf("%02x ", rb[i]);
              printf("\n"); fflush(stdout);
            }
          }
          if (st2 != 0) {   /* 失败也要看回包：EX_SERVICE_SPECIFIC 会带 errorCode+String16 原因 */
            size_t rsz = (out2 && N.Parcel_dataSize) ? N.Parcel_dataSize(out2) : 0;
            printf("ARGS2-FAIL reply-size=%zu", rsz);
            if (out2 && rsz > 0 && rsz <= 512) {
              static uint8_t fb[512]; int fl = 0;
              if (parcel_spill(out2, fb, sizeof fb, &fl) == 0) {
                printf(" 头%d字节:", fl < 64 ? fl : 64);
                for (int i = 0; i < fl && i < 64; i++) printf(" %02x", fb[i]);
                printf("\n 可打印:");
                for (int i = 0; i < fl; i++) if (fb[i] >= 0x20 && fb[i] < 0x7f) putchar((char)fb[i]);
              }
            }
            printf("\n"); fflush(stdout);
          }
        }
      }
      if (0) {
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
