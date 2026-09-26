/* argsloop：把"猜字段序"变成"以平台为裁判的数据迭代"——一次构建，设备上反复喂参数，不用 CI。
 * 做法：IN=逗号分隔 int32 → 写进 AParcel → 交给**平台自己的**
 *       OpenOutputStreamArguments::readFromParcel(AParcel const*)；rc==0 才算结构可收。
 *       再把解出的结构体用 writeToParcel 回环打包并打印 ⇒ 每格被读成什么、结构体落成什么样，全可见。
 * 用法：su -c 'IN=0,44,1,8,0,0,0,0,0,0,0 /data/local/tmp/argsloop [lib路径] [service名]'
 *       TRANSACT=1 才真的发事务（默认关；且只对指定 service 发，绝不放音——args 不含任何采样数据）。
 * 只读、不发声。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void AParcel;
typedef void AIBinder;

/* 非平凡按值返回走隐藏 x8 槽（§本项目老坑）：openOutputStream / getAudioPorts 都这样 */
__asm__(
".text\n"
".globl call_sret3\n"
"call_sret3:\n"      /* (fn, a0, a1, a2, sret) */
"  mov x8, x4\n"
"  mov x5, x0\n"
"  mov x0, x1\n"
"  mov x1, x2\n"
"  mov x2, x3\n"
"  br  x5\n"
".previous\n"
);
extern void call_sret3(void* fn, void* a0, void* a1, void* a2, void* sret);

static void noop_create(void* a) { (void)a; }
static void noop_destroy(void* a) { (void)a; }
static int32_t noop_transact(AIBinder* b, uint32_t c, const void* in, void* out) {
  (void)b; (void)c; (void)in; (void)out; return 0;
}

static struct {
  void* ndk;
  AParcel* (*Parcel_create)(void);
  size_t (*Parcel_dataSize)(const AParcel*);
  int32_t (*Parcel_setPos)(AParcel*, int32_t);
  int32_t (*Parcel_readByte)(const AParcel*, int8_t*);
  int32_t (*Parcel_writeInt32)(AParcel*, int32_t);
} N;

static void spill(AParcel* p, uint8_t* buf, int cap, int* lenOut) {
  int n = (int)N.Parcel_dataSize(p), i = 0, stall = 0;
  *lenOut = 0;
  while (i < n) {
    N.Parcel_setPos(p, i);
    int8_t c;
    if (N.Parcel_readByte(p, &c) == 0) { buf[i++] = (uint8_t)c; stall = 0; continue; }
    stall++;
    if (stall > 4 && i + 4 < n) { memset(buf + i, 0xEE, 4); i += 4; stall = 0; continue; }
    if (stall > 20) break;
  }
  *lenOut = i;
}

static void dumpInts(const char* tag, const uint8_t* b, int len) {
  printf("%s(%d 字节): ", tag, len);
  for (int o = 0; o + 4 <= len; o += 4) {
    int32_t v; memcpy(&v, b + o, 4);
    printf("%d ", v);
  }
  printf("\n");
  fflush(stdout);
}

int main(int argc, char** argv) {
  char* in0 = strdup(getenv("IN") ? getenv("IN") : "0");
  char* in = in0;
  N.ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.ndk) { fprintf(stderr, "dlopen ndk: %s\n", dlerror()); return 2; }
  N.Parcel_create = (AParcel* (*)(void))dlsym(N.ndk, "AParcel_create");
  N.Parcel_dataSize = (size_t (*)(const AParcel*))dlsym(N.ndk, "AParcel_getDataSize");
  N.Parcel_setPos = (int32_t(*)(AParcel*, int32_t))dlsym(N.ndk, "AParcel_setDataPosition");
  N.Parcel_readByte = (int32_t(*)(const AParcel*, int8_t*))dlsym(N.ndk, "AParcel_readByte");
  N.Parcel_writeInt32 = (int32_t(*)(AParcel*, int32_t))dlsym(N.ndk, "AParcel_writeInt32");
  int (*writeBinder)(AParcel*, AIBinder*) =
    (int (*)(AParcel*, AIBinder*))dlsym(N.ndk, "AParcel_writeStrongBinder");
  int (*writeI64)(AParcel*, int64_t) = (int (*)(AParcel*, int64_t))dlsym(N.ndk, "AParcel_writeInt64");
  (void)writeBinder; (void)writeI64;
  if (!N.Parcel_create || !N.Parcel_dataSize || !N.Parcel_setPos || !N.Parcel_readByte ||
      !N.Parcel_writeInt32) { fprintf(stderr, "libbinder_ndk 符号缺\n"); return 3; }

  const char* lib = argc > 1 ? argv[1] : "/system/lib64/android.hardware.audio.core-V4-ndk.so";
  void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 4; }
  int (*readArgs)(void* self, const AParcel*) = (int(*)(void*, const AParcel*))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments14readFromParcelEPK7AParcel");
  int (*writeArgs)(const void*, AParcel*) = (int(*)(const void*, AParcel*))dlsym(h,
    "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
  if (!readArgs || !writeArgs) { fprintf(stderr, "缺 read/write 符号\n"); return 5; }

  /* 1) 造请求 parcel */
  AParcel* p = N.Parcel_create();
  int cnt = 0;
  /* SPEC 语法：整数（可 0x…）= 一个 int32；"B" = 一个 null strong binder（24B flat 对象，
   * 平台真值里那两个回调槽就是它）；"L" = int64 0。⇒ 纯数据即可复刻 88B 骨架，不用每试一次跑 CI。 */
  for (char* tok = strtok(in, ","); tok; tok = strtok(NULL, ",")) {
    if (!strcmp(tok, "B")) { if (writeBinder) writeBinder(p, NULL); else N.Parcel_writeInt32(p, 0); }
    else if (!strcmp(tok, "L")) { if (writeI64) writeI64(p, 0);
                                 else { N.Parcel_writeInt32(p, 0); N.Parcel_writeInt32(p, 0); } }
    else N.Parcel_writeInt32(p, (int32_t)strtol(tok, NULL, 0));
    cnt++;
  }
  free(in0);
  printf("IN: %d 个词\n", cnt); fflush(stdout);

  /* 2) 平台读端当裁判 */
  static char args[2048];
  memset(args, 0, sizeof args);
  N.Parcel_setPos(p, 0);
  int rr = readArgs(args, p);
  printf("READ st=%d  struct 前 16 个 int: ", rr);
  for (int k = 0; k < 16; k++) { int32_t v; memcpy(&v, args + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);

  /* 3) 回环：把解出的结构体再打包一次，看平台认为的"等价字节" */
  AParcel* q = N.Parcel_create();
  int wr = writeArgs(args, q);
  static uint8_t rb[4096]; int rl = 0;
  spill(q, rb, sizeof rb, &rl);
  printf("WRITE st=%d 回环 %d 字节\n", wr, rl); fflush(stdout);
  dumpInts("回环 int 流", rb, rl);

  if (!getenv("TRANSACT")) { printf("(未发事务；TRANSACT=1 才发)\n"); return 0; }

  /* 4) 可选：真的用平台 BpModule 发这个 args（对指定 service，无采样数据 ⇒ 无声） */
  const char* svc = argc > 2 ? argv[2] : "android.hardware.audio.core.IModule/r_submix";
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*Class_define)(const char*, void*, void*, void*) =
    (void*(*)(const char*, void*, void*, void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Associate)(AIBinder*, const void*) = (int(*)(AIBinder*, const void*))dlsym(N.ndk, "AIBinder_associateClass");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = Class_define(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Associate) Associate(b, cls);
  void (*CtorBp)(void*, AIBinder**) = (void(*)(void*, AIBinder**))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModuleC1ERKN3ndk10SpAIBinderE");
  void* openOut = dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModule16openOutputStreamERKNS3_7IModule25OpenOutputStreamArgumentsEPNS5_22OpenOutputStreamReturnE");
  if (!CtorBp || !openOut) { fprintf(stderr, "缺 BpModule 符号\n"); return 7; }
  static char bp[128]; memset(bp, 0, sizeof bp);
  CtorBp(bp, &b);
  static char ret[2048]; memset(ret, 0, sizeof ret);
  static char sret[32]; memset(sret, 0, sizeof sret);
  call_sret3(openOut, bp, args, ret, sret);
  void* so = *(void**)sret;
  int st = so ? ((int32_t(*)(const void*))dlsym(N.ndk, "AStatus_getStatus"))(so) : 0;
  printf("TRANSACT st=%d ret 前 8 个 int: ", st);
  for (int k = 0; k < 8; k++) { int32_t v; memcpy(&v, ret + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  return st == 0 ? 0 : 8;
}
