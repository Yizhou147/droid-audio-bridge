// audio-probe：M0 探针 —— binder 客户端直连 vendor 音频 HAL，调 getAudioPorts(11) 验证链路。
// 事务码取证见 直连音频HAL方案.md §2（全部从设备自己的 android.hardware.audio.core-V2-ndk.so 反汇编）。
// 判据（自证）：服务可达 + transact rc=0 + reply exception==0 + ports 数 >0；
// 任何一步失败都要打明哪一步，不许把"没读到"报成"没有"。
// 用法：su -c '/data/local/tmp/audio-probe [service]'
#include <dlfcn.h>
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
  void* h = dlopen("libbinder_ndk.so", RTLD_NOW);
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
  AParcel* in = NULL;
  binder_status_t rc = B.Prepare(mod, &in);
  if (rc != 0) { fprintf(stderr, "STEP2-FAIL prepare rc=%d (impl 为空 ⇒ getService 给了壳，改走 service-manager-raw)\n", rc); return 4; }
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
