/* hp-open：不手搓 parcel —— 直接构造平台的 BpModule 并调 openOutputStream，
 * 让 AIDL 生成码自己打包/解回包（地面真值 argsdump 已证明我们对手骨架长度是 88 而非 44）。
 * 判据：st/ex；成功则 ret 里会是平台解好的 Return（含 IStreamOut/StreamDescriptor）。
 * 用法：su -c '/data/local/tmp/hp-open [service]'   默认 r_submix（虚拟、无声）
 * 全程不写任何采样数据，绝不出声。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void AIBinder;
typedef void AParcel;

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/r_submix";
  void* ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!ndk) { fprintf(stderr, "dlopen libbinder_ndk: %s\n", dlerror()); return 2; }
  void (*Proc_setMax)(uint32_t) = (void(*) (uint32_t))dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void (*)(void))dlsym(ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder* (*)(const char*))dlsym(ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void (*)(AIBinder*))dlsym(ndk, "AIBinder_incStrong");
  Proc_setMax && Proc_setMax(4);
  if (Proc_start) Proc_start();
  if (!SM_get) { fprintf(stderr, "no getService\n"); return 3; }
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService(%s) 空\n", svc); return 4; }
  IncStrong && IncStrong(b);
  printf("DIAG binder=%p svc=%s\n", (void*)b, svc); fflush(stdout);

  static const char* cores[] = {
    "/system/lib64/android.hardware.audio.core-V4-ndk.so",
    "/vendor/lib64/android.hardware.audio.core-V2-ndk.so", NULL };
  void* h = NULL;
  for (int i = 0; cores[i]; i++) { h = dlopen(cores[i], RTLD_NOW | RTLD_GLOBAL);
    if (h) { printf("DIAG 用 %s\n", cores[i]); break; } fprintf(stderr, "dlopen %s: %s\n", cores[i], dlerror()); }
  if (!h) return 5;

  void (*CtorBpModule)(void* self, AIBinder** spSlot) =
    (void (*)(void*, AIBinder**))dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModuleC1ERKN3ndk10SpAIBinderE");
  int (*openOut)(void* self, const void* args, void* ret) =
    (int (*)(void*, const void*, void*))dlsym(h,
      "_ZN4aidl7android8hardware5audio4core8BpModule16openOutputStreamERKNS3_7IModule25OpenOutputStreamArgumentsEPNS5_22OpenOutputStreamReturnE");
  if (!CtorBpModule || !openOut) { fprintf(stderr, "缺符号 ctor=%p open=%p\n", (void*)CtorBpModule, (void*)openOut); return 6; }

  /* BpModule 对象：vptr + SpAIBinder(+可能别的)，给 128B 余量。SpAIBinder 就是 {AIBinder*}，
   * 我们已自己 incStrong，直接把指针塞进槽里交给 ctor（它会再 incStrong，可接受）。 */
  static char bp[128]; memset(bp, 0, sizeof bp);
  static char args[2048]; memset(args, 0, sizeof args);
  static char ret[2048]; memset(ret, 0, sizeof ret);
  CtorBpModule(bp, &b);
  printf("DIAG BpModule 构造完 vptr=%p\n", *(void**)bp); fflush(stdout);

  int st = openOut(bp, args, ret);
  printf("OPENOUT st=%d (0=OK; 负数=binder/AIDL 状态码) ret 前 32 字节：", st);
  for (int i = 0; i < 32; i++) printf("%02x ", (unsigned char)ret[i]);
  printf("\nVERDICT: %s\n", st == 0 ? "★平台打包的 openOutputStream 事务成功（下一步填字段+取 FMQ）"
                                     : "事务失败，但打包由平台做⇒错义在参数内容，可逐词填");
  fflush(stdout);
  return st == 0 ? 0 : 7;
}
