/* argsloop：把"猜字段序"变成"以平台为裁判的数据迭代"——一次构建，设备上反复喂参数，不用 CI。
 * 做法：IN=逗号分隔 int32 → 写进 AParcel → 交给**平台自己的**
 *       OpenOutputStreamArguments::readFromParcel(AParcel const*)；rc==0 才算结构可收。
 *       再把解出的结构体用 writeToParcel 回环打包并打印 ⇒ 每格被读成什么、结构体落成什么样，全可见。
 * 用法：su -c 'IN=0,44,1,8,0,0,0,0,0,0,0 /data/local/tmp/argsloop [lib路径] [service名]'
 *       TRANSACT=1 才真的发事务（默认关；且只对指定 service 发，绝不放音——args 不含任何采样数据）。
 * 只读、不发声。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

/* 符号插桩：可执行文件导出的同名符号在全局作用域里优先，平台 Bp 码经 PLT 调 AIBinder_transact
 * 时会先进这里 ⇒ 转发给真实现，并在返回前把 reply parcel 抄下来（里面有 IStreamOut 与 FMQ 的 fd）。 */
static void spill(AParcel* p, uint8_t* buf, int cap, int* lenOut);   /* 定义在后面 */
static int g_cap;                      /* 1=正在跑我们自己触发的那次 Bp 调用 */
static uint8_t g_reply[8192];
static int g_reply_len;
static AIBinder* g_stream;
static int g_fds[8]; static int g_nfd;
static int (*real_tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t);

__attribute__((visibility("default")))
int AIBinder_transact(AIBinder* binder, uint32_t code, AParcel** in, AParcel** out, uint32_t flags) {
  if (!real_tx) real_tx = (int (*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))
      dlsym(RTLD_NEXT, "AIBinder_transact");
  int r = real_tx(binder, code, in, out, flags);
  if (g_cap && code == 15 && r == 0 && out && *out) {
    AParcel* op = *out;
    g_reply_len = 0;
    spill(op, g_reply, sizeof g_reply, &g_reply_len);
    int (*rb)(const AParcel*, AIBinder**) =
      (int (*)(const AParcel*, AIBinder**))dlsym(N.ndk, "AParcel_readStrongBinder");
    int (*rfd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
    int (*ri)(const AParcel*, int32_t*) = (int(*)(const AParcel*, int32_t*))dlsym(N.ndk, "AParcel_readInt32");
    int32_t ex = -1;
    if (ri) ri(op, &ex);
    if (rb) { AIBinder* st = NULL; if (rb(op, &st) == 0 && st) g_stream = st; }
    (void)rfd;
    printf("  [插桩] reply=%d 字节 ex=%d stream=%p\n", g_reply_len, ex, (void*)g_stream);
    fflush(stdout);
  }
  return r;
}

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
  if (getenv("AUTO")) {
    /* AUTO=1：IN 用分号分隔的词表，首个 size 槽由本工具回填。词：整数 / B / L / H:<hex字节> */
    extern int auto_build(void* h);
    return auto_build(h);
  }
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
  if (getenv("DUMP")) {   /* DUMP=<service> + TR=<code>：用现有 in parcel 直接发，倒回包原始字节 */
    extern int do_dump(char** argv, int argc);
    return do_dump(argv, argc);
  }

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
  if (getenv("CAP")) { g_cap = 1; }
  call_sret3(openOut, bp, args, ret, sret);
  if (getenv("CAP")) {
    g_cap = 0;
    printf("CAP 完成: stream=%p reply=%d 字节\n", (void*)g_stream, g_reply_len); fflush(stdout);
    printf("CAP reply hex "); for (int i = 0; i < g_reply_len && i < 120; i++) printf("%02x", g_reply[i]);
    printf("\n"); fflush(stdout);
  }
  void* so = *(void**)sret;
  int st = so ? ((int32_t(*)(const void*))dlsym(N.ndk, "AStatus_getStatus"))(so) : 0;
  printf("TRANSACT st=%d ret 前 8 个 int: ", st);
  for (int k = 0; k < 8; k++) { int32_t v; memcpy(&v, ret + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  return st == 0 ? 0 : 8;
}


/* ===== 追加：DUMP / AUTO 两个模式（都用本文件上方的静态状态，通过参数传入句柄） ===== */
static int g_argc; static char** g_argv;
static AParcel* newParcel(void) { return N.Parcel_create(); }

int do_dump(char** argv, int argc) {
  (void)argv; (void)argc;
  const char* svc = getenv("DUMP");
  uint32_t code = (uint32_t)strtoul(getenv("TR") ? getenv("TR") : "10", NULL, 0);
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
  int (*Prep)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
  int (*Tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
    (int(*)(AIBinder*,uint32_t,AParcel**,AParcel**,uint32_t))dlsym(N.ndk, "AIBinder_transact");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = CD(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Assoc) Assoc(b, cls);
  AParcel* in = NULL; AParcel* out = NULL;
  if (Prep(b, &in)) { fprintf(stderr, "prepare 失败\n"); return 7; }
  N.Parcel_writeInt32(in, 0);
  int st = Tx(b, code, &in, &out, 0);
  static uint8_t buf[65536]; int len = 0;
  if (out) spill(out, buf, sizeof buf, &len);
  printf("DUMP code=%u st=%d len=%d\n", code, st, len);
  printf("HEX ");
  for (int i = 0; i < len; i++) printf("%02x", buf[i]);
  printf("\n"); fflush(stdout);
  return st == 0 ? 0 : 8;
}

int auto_build(void* h) {
  int (*readArgs)(void*, const AParcel*) = (int(*)(void*, const AParcel*))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments14readFromParcelEPK7AParcel");
  if (!readArgs) { fprintf(stderr, "缺 readFromParcel\n"); return 5; }
  const char* spec = getenv("IN") ? getenv("IN") : "0";
  char* sp = strdup(spec);
  AParcel* p = newParcel();
  int32_t ph0 = N.Parcel_dataSize(p);
  N.Parcel_writeInt32(p, 0);                       /* size 占位 */
  size_t nbytes = 0;
  for (char* tok = strtok(sp, ";"); tok; tok = strtok(NULL, ";")) {
    if (!strcmp(tok, "B")) {
      int (*wb)(AParcel*, AIBinder*) = (int(*)(AParcel*, AIBinder*))dlsym(N.ndk, "AParcel_writeStrongBinder");
      if (wb) wb(p, NULL); else N.Parcel_writeInt32(p, 0);
      nbytes += 24;
    } else if (!strncmp(tok, "L", 1) && (tok[1] == 0 || tok[1] == ':')) {
      int (*w64)(AParcel*, int64_t) = (int(*)(AParcel*, int64_t))dlsym(N.ndk, "AParcel_writeInt64");
      int64_t v = tok[1] == ':' ? (int64_t)strtoll(tok + 2, NULL, 0) : 0;
      if (w64) w64(p, v); else { N.Parcel_writeInt32(p, (int32_t)v); N.Parcel_writeInt32(p, (int32_t)(v >> 32)); }
    } else if (!strncmp(tok, "H:", 2)) {
      /* 按 int32 写：AParcel_writeByte 每字节要占 4 字节格（上次 120B 变 480B 就是这么来的） */
      const char* q = tok + 2;
      while (q[0]) {
        char w[9] = {0};
        int k = 0;
        while (k < 8 && q[k]) { w[k] = q[k]; k++; }
        if (k < 8) { while (k < 8) w[k++] = '0'; }   /* 尾部补零对齐到 4 字节 */
        N.Parcel_writeInt32(p, (int32_t)strtoul(w, NULL, 16));
        q += 8;
      }
    } else { N.Parcel_writeInt32(p, (int32_t)strtol(tok, NULL, 0)); nbytes += 4; }
  }
  int32_t end = (int32_t)N.Parcel_dataSize(p);
  N.Parcel_setPos(p, ph0);
  N.Parcel_writeInt32(p, end - ph0);              /* 回填 size（含自身） */
  N.Parcel_setPos(p, end);
  printf("AUTO 写出块长 %d（dataSize=%zu）\n", end - ph0, N.Parcel_dataSize(p)); fflush(stdout);
  static char args[4096]; memset(args, 0, sizeof args);
  N.Parcel_setPos(p, 0);
  int rr = readArgs(args, p);
  printf("READ st=%d struct前8int=", rr);
  for (int k = 0; k < 8; k++) { int32_t v; memcpy(&v, args + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  /* 关键调试：把我手发的字节 vs 平台从"解出的结构体"再编码的字节 并排打出来，逐字节找差异 */
  {
    AParcel* q2 = N.Parcel_create();
    int (*wArgs)(const void*, AParcel*) = (int (*)(const void*, AParcel*))dlsym(h,
      "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
    if (wArgs) {
      int w2 = wArgs(args, q2);
      static uint8_t mine[4096], theirs[4096];
      int lm = 0, lt = 0;
      N.Parcel_setPos(p, 0); spill(p, mine, sizeof mine, &lm);
      N.Parcel_setPos(q2, 0); spill(q2, theirs, sizeof theirs, &lt);
      printf("MINE(%d) ", lm); for (int i = 0; i < lm; i++) printf("%02x", mine[i]); printf("\n");
      printf("THEIRS(%d) st=%d ", lt, w2); for (int i = 0; i < lt; i++) printf("%02x", theirs[i]); printf("\n");
      for (int i = 0; i < lm || i < lt; i++) {
        uint8_t a = i < lm ? mine[i] : 0xff, b2 = i < lt ? theirs[i] : 0xff;
        if (a != b2) printf("  首差@%d: mine=%02x theirs=%02x\n", i, a, b2);
        if (a != b2) break;
      }
      fflush(stdout);
    }
  }
  if (getenv("RAWX") && rr == 0) {
    /* 隔离实验：请求 parcel 由 prepareTransaction + 平台 writeToParcel 产生（字节与 Bp 路径同源），
     * 但发送用我的原始 AIBinder_transact ⇒ 区分"打包差异"与"调用方式差异"。 */
    const char* svc = getenv("SVC") ? getenv("SVC") : "android.hardware.audio.core.IModule/r_submix";
    void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
    void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
    AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
    void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
    void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
    int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
    int (*Prep)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
    int (*Tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
      (int(*)(AIBinder*,uint32_t,AParcel**,AParcel**,uint32_t))dlsym(N.ndk, "AIBinder_transact");
    int (*rB)(const AParcel*, AIBinder**) = (int(*)(const AParcel*, AIBinder**))dlsym(N.ndk, "AParcel_readStrongBinder");
    if (Proc_setMax) Proc_setMax(4);
    if (Proc_start) Proc_start();
    AIBinder* mb = SM_get(svc);
    if (mb && IncStrong) IncStrong(mb);
    char dsc[160]; snprintf(dsc, sizeof dsc, "%s", svc);
    char* sl3 = strrchr(dsc, '/'); if (sl3) *sl3 = 0;
    const void* cls3 = CD(dsc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
    if (Assoc && mb) Assoc(mb, cls3);
    int (*wArgs2)(const void*, AParcel*) = (int(*)(const void*, AParcel*))dlsym(h,
      "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
    for (int fi = 0; fi < 3; fi++) {
      uint32_t fl = (uint32_t)(fi == 0 ? 0 : fi == 1 ? 0x10000000 : 0x10000010);
      AParcel* rin = NULL; AParcel* rout = NULL;
      if (!mb || Prep(mb, &rin)) { printf("RAWX prepare 失败\n"); break; }
      N.Parcel_writeInt32(rin, 0);
      int ws = wArgs2(args, rin);
      int rs = Tx(mb, 15, &rin, &rout, fl);
      size_t rsz = rout ? N.Parcel_dataSize(rout) : 0;
      int32_t rex = -1; AIBinder* stm = NULL;
      if (rout) { int (*rI)(const AParcel*, int32_t*) = (int(*)(const AParcel*,int32_t*))dlsym(N.ndk,"AParcel_readInt32"); if (rI) rI(rout, &rex); if (rB) rB(rout, &stm); }
      printf("RAWX flags=%#x writeArgs=%d transact=%d reply=%zu ex=%d stream=%p\n",
             fl, ws, rs, rsz, rex, (void*)stm); fflush(stdout);
    }
    return 0;
  }
  if (!getenv("TRANSACT") || rr != 0) return rr == 0 ? 0 : 9;

  /* 用平台 BpModule 发这个"平台刚解出来"的结构体 ⇒ 打包完全由平台做，我们只负责喂对的字节 */
  const char* svc = getenv("SVC") ? getenv("SVC") : "android.hardware.audio.core.IModule/r_submix";
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl2 = strrchr(desc, '/'); if (sl2) *sl2 = 0;
  const void* cls = CD(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Assoc) Assoc(b, cls);
  void (*CtorBp)(void*, AIBinder**) = (void(*)(void*, AIBinder**))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModuleC1ERKN3ndk10SpAIBinderE");
  void* openOut = dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModule16openOutputStreamERKNS3_7IModule25OpenOutputStreamArgumentsEPNS5_22OpenOutputStreamReturnE");
  if (!CtorBp || !openOut) { fprintf(stderr, "缺 BpModule 符号\n"); return 7; }
  static char bp[128]; memset(bp, 0, sizeof bp);
  CtorBp(bp, &b);
  static char ret[4096]; memset(ret, 0, sizeof ret);
  static char sret[32];
  call_sret3(openOut, bp, args, ret, sret);
  void* so = *(void**)sret;
  int st = so ? ((int32_t(*)(const void*))dlsym(N.ndk, "AStatus_getStatus"))(so) : 0;
  printf("TRANSACT st=%d ret前6int=", st);
  for (int k = 0; k < 6; k++) { int32_t v; memcpy(&v, ret + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  if (st != 0 || !getenv("STREAM")) return st == 0 ? 0 : 8;

  /* §18.1：从 Return 缓冲里扫候选指针 ⇒ 若其 vptr 像 C++ 对象且 +8 处像 AIBinder*，
   * 就拿它对 code 1 (getStreamCommon) 发一次原始事务；st=0 且回包非空即命中 stream 句柄。 */
  int (*Prep2)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
  int (*Tx2)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
    (int(*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))dlsym(N.ndk, "AIBinder_transact");
  int (*rFd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
  int (*rFd2)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readFileDescriptor");
  AIBinder* stream = NULL;
  for (int off = 0; off + 16 <= (int)sizeof(ret); off += 8) {
    void* cand = *(void**)(ret + off);
    if ((uintptr_t)cand < 0x1000 || (uintptr_t)cand >= (1UL<<48)) continue;   /* scudo 指针是 0xb400…，上界要放到 48 位 */
    void* vptr = *(void**)cand;
    if ((uintptr_t)vptr < 0x1000) continue;
    void* maybe = *(void**)((char*)cand + 8);
    if (!maybe) continue;
    AParcel* in3 = NULL; AParcel* out3 = NULL;
    printf("  cand ret+%d = %p vptr=%p +8=%p\n", off, cand, vptr, maybe); fflush(stdout);
    if (!Prep2((AIBinder*)maybe, &in3)) {
      N.Parcel_writeInt32(in3, 0);
      int s3 = Tx2((AIBinder*)maybe, 1, &in3, &out3, 0);
      size_t sz3 = out3 ? N.Parcel_dataSize(out3) : 0;
      if (s3 == 0 && sz3 > 4) {
        printf("STREAM 命中: ret+%d cand=%p vptr=%p binder=%p getStreamCommon st=%d reply=%zu\n",
               off, cand, vptr, maybe, s3, sz3); fflush(stdout);
        stream = (AIBinder*)maybe;
        int32_t e3 = -1;
        N.Parcel_setPos(out3, 0);
        int (*rI)(const AParcel*, int32_t*) = (int(*)(const AParcel*, int32_t*))dlsym(N.ndk, "AParcel_readInt32");
        if (rI) rI(out3, &e3);
        printf("  reply ex=%d\n", e3);
        for (int pos = 4; pos + 4 <= (int)sz3; pos += 4) {
          int fd = -1;
          int rr = rFd ? (N.Parcel_setPos(out3, pos), rFd(out3, &fd)) : -1;
          if (rr == 0 && fd >= 0 && fd < 4096) {
            printf("  PFD@%d = %d\n", pos, fd);
            fflush(stdout);
          }
          if (rFd2) { int f2 = -1; N.Parcel_setPos(out3, pos); if (rFd2(out3, &f2) == 0 && f2 >= 0 && f2 < 4096) { printf("  FileDescriptor@%d = %d\n", pos, f2); fflush(stdout); } }
        }
        break;
      }
    }
  }
  printf(stream ? "VERDICT-M2a: 拿到 IStreamOut 句柄（可继续取 FMQ）\n" : "M2a: 未命中 stream 句柄\n");
  fflush(stdout);
  return stream ? 0 : 9;
}

