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
    }
  }
  N.DecStrong(b);
  return 0;
}
