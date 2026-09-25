/* activity-stub：给轮内被 `AudioPolicyService::UidPolicy::registerSelf()` 等死的
 * `activity` 服务挂一个**最小桩 binder**，让 audioserver 能走完初始化并注册
 * media.audio_flinger / media.aaudio（实测栈见工作总结 §38 C-2 与本轮 asprobe 输出）。
 * 只服务这条等待路径：descriptor 对上，任何事务一律回"成功+空回复"（registerUidObserver 是 void 返回）。
 * ALIVE 秒后自动退出并删不掉就随进程消失——交还后真 system_server 自己注册 activity。
 * 用法：su -c '/data/local/tmp/activity-stub'   （ALIVE=45 STUB_NAME=activity 可调）
 * 前提：su 域允许 add_service("activity")（SELinux 若拒即打印失败码，不改 enforcing 状态）。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void AIBinder;
typedef void AParcel;
typedef void AIBinder_Class;

static struct {
  void* h;
  const AIBinder_Class* (*Class_define)(const char*, void*, void*, void*);
  AIBinder* (*Binder_new)(const AIBinder_Class*, void*);
  int32_t (*AddService)(AIBinder*, const char*);
  int32_t (*RemoveService)(const char*);
  void (*Proc_setMax)(uint32_t);
  void (*Proc_startPool)(void);
  void (*IncStrong)(AIBinder*);
} N;

#define S(field, name) do { *(void**)&N.field = dlsym(N.h, name); \
  if (!N.field) fprintf(stderr, "missing %s\n", name); } while (0)

static void noop_create(void* arg) { (void)arg; }
static void noop_destroy(void* arg) { (void)arg; }
/* 一律"成功+空回复"：调用方需要的 void 方法（registerUidObserver 等）即可通过。
 * 注意：手搓 AIBinder_Class 时回复的异常头**没人替我们写**（本设备 libbinder_ndk 不导出
 * AParcel_writeNoException / writeExceptionCode），所以显式 writeInt32(out,0) = EX_NONE。 */
static int32_t on_transact(AIBinder* binder, uint32_t code, const AParcel* in, AParcel* out) {
  (void)binder;
  int (*wI32)(AParcel*, int32_t) = (void*)dlsym(N.h, "AParcel_writeInt32");
  int (*rI32)(const AParcel*, int32_t*) = (void*)dlsym(N.h, "AParcel_readInt32");
  size_t (*dsz)(const AParcel*) = (void*)dlsym(N.h, "AParcel_getDataSize");
  if (in && rI32) {
    int32_t a = -1, b = -1, c = -1;
    rI32(in, &a); rI32(in, &b); rI32(in, &c);
    printf("STUB-REQ code=0x%x size=%zu 前三个 int=%d %d %d\n", code,
           dsz ? dsz(in) : 0, a, b, c);
  } else {
    printf("STUB-REQ code=0x%x\n", code);
  }
  fflush(stdout);
  if (out && wI32) wI32(out, 0);            /* exception = EX_NONE */
  return 0;
}

int main(void) {
  const char* name = getenv("STUB_NAME") ? getenv("STUB_NAME") : "activity";
  const char* desc = getenv("STUB_DESC") ? getenv("STUB_DESC") : "android.app.IActivityManager";
  int alive = getenv("ALIVE") ? atoi(getenv("ALIVE")) : 45;
  N.h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  S(Class_define, "AIBinder_Class_define");
  S(Binder_new, "AIBinder_new");
  S(AddService, "AServiceManager_addService");
  S(RemoveService, "AServiceManager_removeService");
  S(Proc_setMax, "ABinderProcess_setThreadPoolMaxThreadCount");
  S(Proc_startPool, "ABinderProcess_startThreadPool");
  S(IncStrong, "AIBinder_incStrong");
  if (!N.Class_define || !N.Binder_new || !N.AddService) return 3;

  N.Proc_setMax(2);
  N.Proc_startPool();
  const AIBinder_Class* cls = N.Class_define(desc, (void*)noop_create, (void*)noop_destroy,
                                             (void*)on_transact);
  if (!cls) { fprintf(stderr, "Class_define 失败\n"); return 4; }
  AIBinder* b = N.Binder_new(cls, NULL);
  if (!b) { fprintf(stderr, "AIBinder_new 失败\n"); return 5; }
  N.IncStrong(b);
  int32_t st = N.AddService(b, name);
  printf("ADD %s st=%d %s\n", name, st,
         st == 0 ? "（已挂上，等 audioserver 解阻塞）" : "（挂不上：多半 SELinux 拦 add_service）");
  fflush(stdout);
  if (st != 0) return 6;
  for (int i = 0; i < alive; i += 5) { sleep(5); printf("ALIVE %ds\n", i + 5); fflush(stdout); }
  if (N.RemoveService) N.RemoveService(name);
  printf("STUB-EXIT\n");
  return 0;
}
