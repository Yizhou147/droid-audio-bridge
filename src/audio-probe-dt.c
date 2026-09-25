/* audio-probe-dt：DT_NEEDED 链接版（v3）。
 * 前作实证：libbinder 的静态/TLS 在 dlopen 后加载不可用（defaultServiceManager/self 必崩；
 * dlopen 版 getService 恒空）；`service` 二进制靠启动期链接才正常 ⇒ 本探针把 libbinder
 * 链进 DT_NEEDED（CI 用 device-libs/ 里的真设备库当链接桩，运行时加载系统原生那份）。
 * sp<T> 值返回走 x8 隐藏槽：stub_call3 垫槽（09-25 反汇编 getService prologue 实锤）。
 * 用法：audio-probe-dt [service] [txcode]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char* ptr; size_t size; size_t cap_lsb1; } cxs;  /* std::string 长模式 ABI */

extern void* dsm_get(void) __asm__("_ZN7android21defaultServiceManagerEv");
extern void* ps_self(void) __asm__("_ZN7android12ProcessState4selfEv");
extern void* ps_ctxobj(void* self, const void* sp) __asm__("_ZN7android12ProcessState16getContextObjectERKNS_2spINS_7IBinderEEE");
extern int32_t bpsm_getService(void* self, const cxs* name, void** out)
  __asm__("_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
extern int32_t bpsm_checkService(void* self, const cxs* name, void** out)
  __asm__("_ZN7android2os16BpServiceManager12checkServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
extern int32_t bpb_transact(void* self, uint32_t code, const void* in, void* out, uint32_t flags)
  __asm__("_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
extern void parcel_ctor(void*) __asm__("_ZN7android6ParcelC1Ev");
extern int32_t parcel_wtoken(void*, const void*) __asm__("_ZN7android6Parcel19writeInterfaceTokenERKNS_8String16E");
extern int32_t parcel_ri32(const void*, int32_t*) __asm__("_ZNK7android6Parcel9readInt32EPi");
extern size_t parcel_dsize(const void*) __asm__("_ZNK7android6Parcel8dataSizeEv");
extern void s16_ctor(void*, const char*) __asm__("_ZN7android8String16C1EPKc");   /* libutils */

/* getService/checkService 实际按值返回 sp<IBinder>（x8=sret）：用 stub 拿槽里的指针。 */
extern void* stub_call0(void* fn);
extern void* stub_call3(void* fn, void* a0, void* a1, void* a2);
__asm__(
".text\n.globl stub_call0\nstub_call0:\n  stp x29, x30, [sp, #-32]!\n  mov x29, sp\n  add x8, sp, #16\n  mov x9, x0\n  blr x9\n  ldr x0, [sp, #16]\n  ldp x29, x30, [sp], #32\n  ret\n.globl stub_call3\nstub_call3:\n"
"  stp x29, x30, [sp, #-32]!\n"
"  mov x29, sp\n"
"  add x8, sp, #16\n"
"  mov x9, x0\n"
"  mov x0, x1\n"
"  mov x1, x2\n"
"  mov x2, x3\n"
"  blr x9\n"
"  ldr x0, [sp, #16]\n"
"  ldp x29, x30, [sp], #32\n"
"  ret\n.previous\n");

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/default";
  uint32_t code = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 11;

  /* self()/dsm() 都是值返回 sp / 含 sret 的版本：一律 stub_call0 垫 x8 槽 */
  void* ps = stub_call0((void*)ps_self);
  printf("DIAG ps=%p\n", ps); fflush(stdout);
  void* smRef = dsm_get();   /* 若这行崩，改 stub_call0 再战 */
  void* sm = smRef ? *(void**)smRef : NULL;
  printf("DIAG smRef=%p sm=%p\n", smRef, sm); fflush(stdout);
  if (!sm) { fprintf(stderr, "STEP1-FAIL defaultServiceManager 空\n"); return 4; }

  size_t n = strlen(svc);
  char* heap = malloc(n + 1); memcpy(heap, svc, n + 1);
  cxs name = { heap, n, ((n + 1) << 1) | 1 };
  char slot[8] = {};
  void* binder = stub_call3((void*)bpsm_getService, sm, &name, slot);
  if (!binder) { char slot2[8] = {}; binder = stub_call3((void*)bpsm_checkService, sm, &name, slot2); }
  printf("STEP2 binder=%p\n", binder); fflush(stdout);
  if (!binder) { fprintf(stderr, "STEP2-FAIL getService/checkService 都空\n"); return 5; }

  static char bufIn[256], bufOut[32768];
  parcel_ctor(bufIn); parcel_ctor(bufOut);
  char tok[16] = {}; char desc[128];
  snprintf(desc, sizeof desc, "%s", svc);
  char* slash = strrchr(desc, '/'); if (slash) *slash = 0;
  s16_ctor(tok, desc);
  int32_t st = parcel_wtoken(bufIn, tok);
  if (st) { fprintf(stderr, "STEP3-FAIL writeInterfaceToken st=%d\n", st); return 6; }
  st = bpb_transact(binder, code, bufIn, bufOut, 0);
  printf("STEP3 transact(0x%x) st=%d reply=%zu\n", code, st, parcel_dsize(bufOut));
  if (st == 0) {
    int32_t ex = -1, m = -1;
    parcel_ri32(bufOut, &ex); parcel_ri32(bufOut, &m);
    printf("STEP4 exception=%d header=%d  %s\n", ex, m,
           (ex == 0 && m > 0) ? "VERDICT-OK: 自建客户端全通（M1 可开流）" : "VERDICT-PARTIAL: 首字段待核");
  }
  return st == 0 ? 0 : 7;
}
