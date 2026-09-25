// audio-probe-raw v2：不走 defaultServiceManager()（实测本进程在其 +92 的静态写处 SIGSEGV），
// 手动搭：ProcessState::self() → getContextObject(null sp) → os::IServiceManager::asInterface
// → BpServiceManager::getService(std::string) → BpBinder::transact(code)。
// 函数指针全部来自 /system/lib64 绝对路径 dlopen。用法：su -c 'audio-probe-raw [service] [txcode]'
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>

typedef int32_t status_t;
struct Parcel;

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

  auto ProcessState_self = (void*(*)())R(lb, "_ZN7android12ProcessState4selfEv");
  auto getContextObject = (void*(*)(void*, const void*))R(lb, "_ZN7android12ProcessState16getContextObjectERKNS_2spINS_7IBinderEEE");
  auto smAsInterface = (void*(*)(const void*))R(lb, "_ZN7android2os15IServiceManager11asInterfaceERKNS_2spINS_7IBinderEEE");
  auto smGetService = (status_t (*)(void*, const std::string*, void**))R(lb, "_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
  auto transact = (status_t (*)(void*, uint32_t, const Parcel*, Parcel*, uint32_t))R(lb, "_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
  auto parcelCtor = (void(*)(Parcel*))R(lb, "_ZN7android6ParcelC1Ev");
  auto parcelDtor = (void(*)(Parcel*))R(lb, "_ZN7android6ParcelD1Ev");
  auto writeToken = (status_t(*)(Parcel*, const void*))R(lb, "_ZN7android6Parcel19writeInterfaceTokenERKNS_8String16E");
  auto readInt32 = (status_t(*)(const Parcel*, int32_t*))R(lb, "_ZNK7android6Parcel9readInt32EPi");
  auto dataSize = (size_t(*)(const Parcel*))R(lb, "_ZNK7android6Parcel8dataSizeEv");
  auto s16Ctor = (void(*)(void*, const char*))R(lu, "_ZN7android8String16C1EPKc");
  auto s16Dtor = (void(*)(void*))R(lu, "_ZN7android8String16D1Ev");
  auto incStrong = (void(*)(void*, const void*))R(lu, "_ZNK7android7RefBase9incStrongEPKv");
  if (!ProcessState_self || !getContextObject || !smAsInterface || !smGetService || !transact) return 3;

  void* ps = ProcessState_self();
  printf("PS=%p\n", ps); fflush(stdout);
  char nullSp[8] = {};                        // sp<IBinder> = 单指针
  void* ctxRef = getContextObject(ps, nullSp);
  if (!ctxRef) { fprintf(stderr, "STEP1-FAIL getContextObject null\n"); return 4; }
  void* smWrap = smAsInterface(ctxRef);       // sp<IServiceManager>；首格=裸指针
  if (!smWrap) { fprintf(stderr, "STEP2-FAIL asInterface null\n"); return 5; }
  void* smBinder = *(void**)smWrap;
  printf("STEP1-2-OK sm=%p\n", smBinder); fflush(stdout);

  auto name = new std::string(svc);
  char slot[8] = {};
  status_t st = smGetService(smBinder, name, (void**)slot);
  void* binder = *(void**)slot;
  if (st != 0 || !binder) { fprintf(stderr, "STEP3-FAIL getService st=%d binder=%p\n", st, binder); return 6; }
  printf("STEP3-OK binder=%p\n", binder); fflush(stdout);
  incStrong(binder, svc);

  char bufIn[256], bufOut[8192];
  Parcel* in = (Parcel*)bufIn; Parcel* out = (Parcel*)bufOut;
  parcelCtor(in); parcelCtor(out);
  char tok[16] = {}; char desc[128];
  snprintf(desc, sizeof desc, "%s", svc);
  char* slash = strrchr(desc, '/'); if (slash) *slash = 0;
  s16Ctor(tok, desc);
  st = writeToken(in, tok);
  if (st) { fprintf(stderr, "STEP4-FAIL writeInterfaceToken st=%d\n", st); return 7; }
  st = transact(binder, code, (const Parcel*)in, out, 0);
  printf("STEP5 transact(0x%x) st=%d reply-size=%zu\n", code, st, dataSize((const Parcel*)out));
  if (st == 0) {
    int32_t ex = -1, n = -1;
    readInt32((const Parcel*)out, &ex);
    readInt32((const Parcel*)out, &n);
    printf("STEP6 exception=%d array-header=%d  %s\n", ex, n,
           (ex == 0 && n > 0) ? "VERDICT-OK: binder 链路通，端口表可读" : "VERDICT-PARTIAL: 链路通但首字段布局待核（M1 再解）");
  }
  s16Dtor(tok);
  parcelDtor(in); parcelDtor(out);
  delete name;
  return st == 0 ? 0 : 8;
}
