/* audio-probe-raw (纯 C)：不引入任何 C++ 运行时符号进本进程，避免抢占平台库的 new/std::*。
 * 手造 libc++ std::string（long 模式 ABI：{ptr,size,cap<<1|1}，24B）只喂给 libbinder 读，不释放。
 * 链路：ProcessState::self → getContextObject(空sp) → os::IServiceManager::asInterface
 *       → BpServiceManager::getService(string) → BpBinder::transact(code) → Parcel 读回包。
 * 用法：su -c '/data/local/tmp/audio-probe-raw [service] [txcode]' */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char* ptr; size_t size; size_t cap_lsb1; } cxstring; /* std::string, long 模式 */

/* AAPCS64：按值返回 sp<T>（非平凡类型）走隐藏返回槽 x8。直接 (void*(*)()) 调这些函数
 * 会让 x8=垃圾 → libbinder 往垃圾指针里写（09-25 的 self()+84 段错误真因）。
 * 这两个 stub 负责垫好 x8 槽再转交结果。 */
extern void* stub_call0(void* fn);
extern void* stub_call1(void* fn, void* a0);
extern void* stub_call2(void* fn, void* a0, void* a1);   /* 成员函数：x0=this, x1=arg, x8=sret */
__asm__(
".text\n"
".globl stub_call0\n"
"stub_call0:\n"
"  stp x29, x30, [sp, #-32]!\n"
"  mov x29, sp\n"
"  add x8, sp, #16\n"
"  mov x9, x0\n"
"  blr x9\n"
"  ldr x0, [sp, #16]\n"
"  ldp x29, x30, [sp], #32\n"
"  ret\n"
".globl stub_call1\n"
"stub_call1:\n"
"  stp x29, x30, [sp, #-32]!\n"
"  mov x29, sp\n"
"  add x8, sp, #16\n"
"  mov x9, x0\n"
"  mov x0, x1\n"
"  blr x9\n"
"  ldr x0, [sp, #16]\n"
"  ldp x29, x30, [sp], #32\n"
"  ret\n"
".globl stub_call2\n"
"stub_call2:\n"
"  stp x29, x30, [sp, #-32]!\n"
"  mov x29, sp\n"
"  add x8, sp, #16\n"
"  mov x9, x0\n"
"  mov x0, x1\n"
"  mov x1, x2\n"
"  blr x9\n"
"  ldr x0, [sp, #16]\n"
"  ldp x29, x30, [sp], #32\n"
"  ret\n"
".previous\n"
);

static void* R(void* h, const char* n) {
  void* p = dlsym(h, n);
  if (!p) fprintf(stderr, "missing %s\n", n);
  return p;
}

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/default";
  uint32_t code = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 11;

  void* lb = dlopen("/system/lib64/libbinder.so", RTLD_NOW | RTLD_GLOBAL);
  void* lu = dlopen("/system/lib64/libutils.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lb || !lu) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 2; }

  void* (*ProcessState_self)(void) = R(lb, "_ZN7android12ProcessState4selfEv");
  void* (*getContextObject)(void*, const void*) = R(lb, "_ZN7android12ProcessState16getContextObjectERKNS_2spINS_7IBinderEEE");
  void* (*smAsInterface)(const void*) = R(lb, "_ZN7android2os15IServiceManager11asInterfaceERKNS_2spINS_7IBinderEEE");
  int32_t (*smGetService)(void*, const cxstring*, void**) = R(lb, "_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
  int32_t (*transact)(void*, uint32_t, const void*, void*, uint32_t) = R(lb, "_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
  void (*parcelCtor)(void*) = R(lb, "_ZN7android6ParcelC1Ev");
  int32_t (*writeToken)(void*, const void*) = R(lb, "_ZN7android6Parcel19writeInterfaceTokenERKNS_8String16E");
  int32_t (*readInt32)(const void*, int32_t*) = R(lb, "_ZNK7android6Parcel9readInt32EPi");
  size_t (*dataSize)(const void*) = R(lb, "_ZNK7android6Parcel8dataSizeEv");
  void (*s16Ctor)(void*, const char*) = R(lu, "_ZN7android8String16C1EPKc");
  if (!ProcessState_self || !getContextObject || !smAsInterface || !smGetService || !transact) return 3;

  void* ps = stub_call0(ProcessState_self);      /* 返回 sp<ProcessState>：值返回走 x8 槽 */
  if (!ps) { fprintf(stderr, "STEP0-FAIL ProcessState::self null（open /dev/binder 失败？）\n"); return 4; }
  printf("PS=%p\n", ps); fflush(stdout);

  char nullSp[8] = {};
  void* ctxSp = stub_call2(getContextObject, ps, nullSp);  /* 值返回 sp<IBinder>：ctxSp=槽里的 binder 指针 */
  if (!ctxSp) { fprintf(stderr, "STEP1-FAIL getContextObject null\n"); return 4; }
  void* sm = stub_call1(smAsInterface, &ctxSp);           /* 参数是 const sp<IBinder>& → 传槽地址，不是槽值 */
  if (!sm) { fprintf(stderr, "STEP2-FAIL asInterface null\n"); return 5; }
  printf("STEP1-2-OK ctx=%p sm=%p\n", ctxSp, sm); fflush(stdout);
  { void** w = (void**)sm; printf("DIAG sm[0]=%p sm[1]=%p sm[2]=%p sm[3]=%p\n", w[0], w[1], w[2], w[3]); }

  size_t n = strlen(svc);
  char* heap = malloc(n + 1); memcpy(heap, svc, n + 1);
  cxstring s = { heap, n, ((n + 1) << 1) | 1 };     /* long 模式：cap 左移一位，最低位=1 */
  char slot[8] = {};
  int32_t st = smGetService(sm, &s, (void**)slot);
  void* binder = *(void**)slot;
  if (st != 0 || !binder) { fprintf(stderr, "STEP3-FAIL getService st=%d binder=%p\n", st, binder); return 6; }
  printf("STEP3-OK binder=%p\n", binder); fflush(stdout);

  static char bufIn[256], bufOut[16384];
  parcelCtor(bufIn); parcelCtor(bufOut);
  char tok[16] = {}; char desc[128];
  snprintf(desc, sizeof desc, "%s", svc);
  char* slash = strrchr(desc, '/'); if (slash) *slash = 0;
  s16Ctor(tok, desc);
  st = writeToken(bufIn, tok);
  if (st) { fprintf(stderr, "STEP4-FAIL writeInterfaceToken st=%d\n", st); return 7; }
  st = transact(binder, code, bufIn, bufOut, 0);
  printf("STEP5 transact(0x%x) st=%d reply-size=%zu\n", code, st, dataSize(bufOut));
  if (st == 0) {
    int32_t ex = -1, m = -1;
    readInt32(bufOut, &ex);
    readInt32(bufOut, &m);
    printf("STEP6 exception=%d array-header=%d  %s\n", ex, m,
           (ex == 0 && m > 0) ? "VERDICT-OK: binder 链路通，端口表可读" : "VERDICT-PARTIAL: 链路通但首字段布局待核（M1 再解）");
  }
  return st == 0 ? 0 : 8;
}
