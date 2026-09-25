// audio-probe：M0 探针 —— binder 客户端直连 vendor 音频 HAL，调 getAudioPorts(11) 验证链路。
// 事务码取证见 直连音频HAL方案.md §2（全部从设备自己的 android.hardware.audio.core-V2-ndk.so 反汇编）。
// 判据（自证）：服务可达 + transact rc=0 + reply exception==0 + ports 数 >0；
// 任何一步失败都要打明哪一步，不许把"没读到"报成"没有"。
// 用法：su -c '/data/local/tmp/audio-probe [service]'
#include <dlfcn.h>
#include <string>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct AIBinder AIBinder;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;

static struct {
  AIBinder* (*GetService)(const char*);
  AIBinder* (*WaitForService)(const char*);
  void (*DecStrong)(AIBinder*);
  binder_status_t (*Prepare)(AIBinder*, AParcel**);
  binder_status_t (*Transact)(AIBinder*, transaction_code_t, AParcel**, AParcel**, uint32_t);
  bool (*StatusIsOk)(const AStatus*);
  const char* (*StatusMessage)(const AStatus*);
  void (*StatusDelete)(AStatus*);
  binder_status_t (*ReadInt32)(const AParcel*, int32_t*);
  size_t (*DataSize)(const AParcel*);
  void (*ParcelDelete)(AParcel*);
} B;
static void (*StartThreadPool)() = NULL;

static int load(void) {
  // 必须绝对路径：/data 二进制的默认命名空间会把裸名解析到 /vendor 那份（它走 vndbinder，framework 服务查成空壳 impl=NULL）
  void* h = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
#define S(field, name) do { *(void**)&B.field = dlsym(h, name); \
    if (!B.field) { fprintf(stderr, "missing %s\n", name); return 1; } } while (0)
  *(void**)&StartThreadPool = dlsym(h, "ABinderProcess_startThreadPool");
  if (StartThreadPool) StartThreadPool();   // 关键：不启线程池，getService 返回无法挂 linkToDeath 的空壳(impl=NULL)
  S(GetService, "AServiceManager_getService");
  // waitForService 也拿一份做后备（getService 返回 null 时才用）
  *(void**)&B.WaitForService = dlsym(h, "AServiceManager_waitForService");
  S(DecStrong, "AIBinder_decStrong");
  S(Prepare, "AIBinder_prepareTransaction");
  S(Transact, "AIBinder_transact");
  S(StatusIsOk, "AStatus_isOk");
  S(StatusMessage, "AStatus_getMessage");
  S(StatusDelete, "AStatus_delete");
  S(ReadInt32, "AParcel_readInt32");
  S(DataSize, "AParcel_getDataSize");
  S(ParcelDelete, "AParcel_delete");
#undef S
  return 0;
}

// ---- C++ 兜底：ProcessState::self → getContextObject → asInterface → getService(std::string) → transact ----
static int raw_fallback(const char* svc, uint32_t code) {
  void* lb = dlopen("/system/lib64/libbinder.so", RTLD_NOW | RTLD_GLOBAL);
  void* lu = dlopen("/system/lib64/libutils.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lb || !lu) { fprintf(stderr, "F-STEP0 dlopen %s\n", dlerror()); return 20; }
  auto sym=[&](void* h, const char* n){ void* p=dlsym(h,n); if(!p) fprintf(stderr,"F missing %s\n",n); return p; };
  typedef int32_t st_t;
  auto selfFn = (void*(*)())sym(lb, "_ZN7android12ProcessState4selfEv");
  auto getCtx = (void*(*)(void*, const void*))sym(lb, "_ZN7android12ProcessState16getContextObjectERKNS_2spINS_7IBinderEEE");
  auto asIfc  = (void*(*)(const void*))sym(lb, "_ZN7android2os15IServiceManager11asInterfaceERKNS_2spINS_7IBinderEEE");
  auto getService = (st_t(*)(void*, const std::string*, void**))sym(lb, "_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
  auto transact = (st_t(*)(void*, uint32_t, const void*, void*, uint32_t))sym(lb, "_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
  auto pCtor = (void(*)(void*))sym(lb, "_ZN7android6ParcelC1Ev");
  auto wTok  = (st_t(*)(void*, const void*))sym(lb, "_ZN7android6Parcel19writeInterfaceTokenERKNS_8String16E");
  auto rI32  = (st_t(*)(const void*, int32_t*))sym(lb, "_ZNK7android6Parcel9readInt32EPi");
  auto dSize = (size_t(*)(const void*))sym(lb, "_ZNK7android6Parcel8dataSizeEv");
  auto s16C  = (void(*)(void*, const char*))sym(lu, "_ZN7android8String16C1EPKc");
  void* ps = selfFn();
  printf("F PS=%p\n", ps); fflush(stdout);
  if (!ps) return 21;
  char nullSp[8] = {};
  void* ctx = getCtx(ps, nullSp);
  if (!ctx) { fprintf(stderr, "F-STEP1 getContextObject null\n"); return 22; }
  void* smWrap = asIfc(ctx);
  if (!smWrap || !*(void**)smWrap) { fprintf(stderr, "F-STEP2 asInterface null\n"); return 23; }
  void* sm = *(void**)smWrap;
  std::string name(svc);
  char slot[8] = {};
  st_t st = getService(sm, &name, (void**)slot);
  void* binder = *(void**)slot;
  printf("F-STEP3 getService st=%d binder=%p\n", st, binder); fflush(stdout);
  if (st != 0 || !binder) return 24;
  static char bufIn[256], bufOut[16384];
  pCtor(bufIn); pCtor(bufOut);
  char tok[16] = {}; char desc[128];
  snprintf(desc, sizeof desc, "%s", svc);
  char* slash = strrchr(desc, '/'); if (slash) *slash = 0;
  s16C(tok, desc);
  st = wTok(bufIn, tok);
  if (st) { fprintf(stderr, "F-STEP4 writeToken st=%d\n", st); return 25; }
  st = transact(binder, code, bufIn, bufOut, 0);
  printf("F-STEP5 transact(0x%x) st=%d size=%zu\n", code, st, dSize(bufOut));
  if (st == 0) {
    int32_t ex=-1, n=-1;
    rI32(bufOut, &ex); rI32(bufOut, &n);
    printf("F-STEP6 exception=%d header=%d  %s\n", ex, n, (ex==0&&n>0)?"VERDICT-OK: 链路通":"VERDICT-PARTIAL: 布局待核");
  }
  return st == 0 ? 0 : 26;
}

// 对照桥的原文："裸句柄需 associateClass 才能 prepare"。
// 真值实验：直接抄桥的 IBluetoothHci descriptor 给 audio-probe 用（它必过），再试 AAudio service。
static int assoc_experiment(AIBinder* b, const char* tag) {
  void* h = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
  auto assoc = (int(*)(AIBinder*, void*))dlsym(h, "AIBinder_associateClass");
  auto cdef = (void*(*)(const char*, void*, void*, void*))dlsym(h, "AIBinder_Class_define");
  auto cdtor = (void(*)(void*))dlsym(h, "AIBinder_Class_setOnDestroy");
  (void)cdtor;
  if (!assoc || !cdef) { fprintf(stderr, "%s: no class syms\n", tag); return 9; }
  static int dummy;
  void* cls = cdef(tag, nullptr, nullptr, nullptr);
  if (!cls) { fprintf(stderr, "%s: define failed\n", tag); return 10; }
  int r = assoc(b, cls);
  printf("DIAG %s associateClass r=%d\n", tag, r); fflush(stdout);
  AParcel* in = nullptr;
  auto prep = (int(*)(AIBinder*, AParcel**))dlsym(h, "AIBinder_prepareTransaction");
  int st = prep(b, &in);
  printf("DIAG %s prepare-after-assoc st=%d\n", tag, st); fflush(stdout);
  return st;
}

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/default";
  const uint32_t code = 11;  // getAudioPorts
  if (load()) return 2;
  // 蓝牙桥验证过 getService 这条路；waitForService 只做后备。
  AIBinder* mod = B.GetService(svc);
  if (!mod && B.WaitForService) mod = B.WaitForService(svc);
  if (!mod) { fprintf(stderr, "STEP1-FAIL getService(%s)：SELinux 或 servicemanager 挡了\n", svc); return 3; }
  printf("STEP1-OK service found: %s\n", svc);
  // 诊断：AIBinder(ABBinder) 布局 [0]=vptr [8]=sp<IBinder> mImpl。
  // 反汇编证明 prepare 的 -38 分支条件就是 mImpl==NULL，打印实况定位是谁造的空壳。
  void* vptr = *(void**)mod;
  void* impl = *(void**)((char*)mod + 8);
  printf("DIAG mod=%p vptr=%p impl=%p\n", (void*)mod, vptr, (void*)impl);
  /* 桥的定式：Class_define(descriptor)+associateClass 之后再 Prepare（缺这步必 -38） */
  {
    void* hh = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW);
    auto cdef = (void*(*)(const char*, void*, void*, void*))dlsym(hh, "AIBinder_Class_define");
    auto assoc = (int(*)(AIBinder*, void*))dlsym(hh, "AIBinder_associateClass");
    char desc[128]; snprintf(desc, sizeof desc, "%s", svc);
    char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
    void* cls = cdef(desc, nullptr, nullptr, nullptr);
    int ar = assoc(mod, cls);
    printf("DIAG associate(%s) r=%d\n", desc, ar); fflush(stdout);
  }
  AParcel* in = NULL;
  binder_status_t rc = B.Prepare(mod, &in);
  if (rc != 0) {
    fprintf(stderr, "STEP2-FAIL prepare rc=%d ⇒ 转 C++ 兜底路（本进程 libbinder 已被 NDK 拉热，直接用它的导出符号）\n", rc);
    return raw_fallback(svc, code);
  }
  AParcel* out = NULL;
  rc = B.Transact(mod, code, &in, &out, 0);
  if (rc != 0 || !out) { fprintf(stderr, "STEP3-FAIL transact(11) rc=%d out=%p\n", rc, (void*)out); return 5; }
  printf("STEP3-OK transact rc=0 reply-size=%zu bytes\n", B.DataSize(out));
  int32_t ex = -1;
  if (B.ReadInt32(out, &ex) != 0) { fprintf(stderr, "STEP4-FAIL 读 exception 失败\n"); return 6; }
  if (ex != 0) { fprintf(stderr, "STEP4-FAIL HAL 回异常码 %d（对照 StatusException 枚举）\n", ex); return 7; }
  int32_t n = -1;
  if (B.ReadInt32(out, &n) != 0) { fprintf(stderr, "STEP5-FAIL 读数组头失败（reply 布局与预期不符，别当端口数用）\n"); return 8; }
  printf("STEP5-OK exception=0 array-header=%d\n", n);
  printf("VERDICT: link alive; n>0 且后续解析对齐 = 端口枚举成功（M1 起再做全字段解析）\n");
  if (B.ParcelDelete) {}
  B.DecStrong(mod);
  return 0;
}
