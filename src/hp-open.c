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

/* AAPCS64：非平凡类型按值返回走隐藏 x8 sret 槽。BpModule::openOutputStream 返回
 * ndk::ScopedAStatus（析构非平凡）⇒ 必须自己备好 x8，否则 callee 往垃圾地址写（实测定格）。 */
__asm__(
".text\n"
".globl call_sret3\n"
"call_sret3:\n"      /* (fn, self, args, ret, sret) */
"  mov x8, x4\n"
"  mov x5, x0\n"
"  mov x0, x1\n"
"  mov x1, x2\n"
"  mov x2, x3\n"
"  br  x5\n"
".previous\n"
);
extern void call_sret3(void* fn, void* self, const void* args, void* ret, void* sret);

static void noop_create(void* a) { (void)a; }
static void noop_destroy(void* a) { (void)a; }
static int32_t noop_transact(AIBinder* b, uint32_t c, const void* in, void* out) {
  (void)b; (void)c; (void)in; (void)out; return 0;
}

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/r_submix";
  void* ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!ndk) { fprintf(stderr, "dlopen libbinder_ndk: %s\n", dlerror()); return 2; }
  void (*Proc_setMax)(uint32_t) = (void(*) (uint32_t))dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void (*)(void))dlsym(ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder* (*)(const char*))dlsym(ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void (*)(AIBinder*))dlsym(ndk, "AIBinder_incStrong");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  if (!SM_get) { fprintf(stderr, "no getService\n"); return 3; }
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService(%s) 空\n", svc); return 4; }
  if (IncStrong) IncStrong(b);
  /* §7 的老规矩：getService 给的是无 class 裸句柄，不先 associateClass，
   * 平台 BpModule 内部 prepareTransaction 会失败并留下空 parcel ⇒ 生成码写空指针 SIGSEGV。 */
  void* (*Class_define)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(ndk, "AIBinder_Class_define");
  int (*Associate)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(ndk, "AIBinder_associateClass");
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = Class_define(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  int ast = Associate(b, cls);
  printf("DIAG associateClass(%s) st=%d\n", desc, ast); fflush(stdout);
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

  static char sret[32]; memset(sret, 0, sizeof sret);
  call_sret3((void*)openOut, bp, args, ret, sret);
  void* stobj = *(void**)sret;
  int st = -99999;
  if (!stobj) st = 0;                     /* ScopedAStatus 空 = OK */
  else {
    int32_t (*AStatus_getStatus)(const void*) = (int32_t(*)(const void*))dlsym(ndk, "AStatus_getStatus");
    const char* (*AStatus_description)(const void*) = (const char*(*)(const void*))dlsym(ndk, "AStatus_description");
    if (AStatus_getStatus) st = AStatus_getStatus(stobj);
    if (AStatus_description) printf("OPENOUT desc=%s\n", AStatus_description(stobj));
  }
  printf("OPENOUT st=%d (0=OK) sret=%p ret 前 32 字节：", st, stobj);
  for (int i = 0; i < 32; i++) printf("%02x ", (unsigned char)ret[i]);
  printf("\nVERDICT: %s\n", st == 0 ? "★平台打包的 openOutputStream 事务成功（下一步填字段+取 FMQ）"
                                     : "事务失败，但打包由平台做⇒错义在参数内容，可逐词填");
  fflush(stdout);
  return st == 0 ? 0 : 7;
}
