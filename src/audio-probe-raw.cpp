// audio-probe-raw：不走 AServiceManager_*（那条路在本进程返回 impl=NULL 的空壳，实测），
// 直接 dlopen libbinder.so 用 C++ 导出符号：defaultServiceManager → BpServiceManager::getService
// → BpBinder::transact → Parcel 读回包。`service call`(ksu) 能通 = 这条 C++ 路必通（同一个二进制逻辑）。
// ABI 依赖：sp<T>=单指针；Parcel 只 malloc 足够空间后 placement-ctor，不猜字段。
// 用法：su -c '/data/local/tmp/audio-probe-raw [service] [txcode]'
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <memory>

typedef int32_t status_t;
struct RefBase { virtual ~RefBase(); };           // 只借 vptr 布局，不调它的虚表
struct IBinder : RefBase {};
struct IServiceManager : IBinder {};
struct Parcel;                                    // opaque

static void* lb = NULL;
static void* lu = NULL;
static void* lcpp = NULL;

struct Api {
  // _ZN7android21defaultServiceManagerEv() -> sp<IServiceManager>&（返回静态对象引用）
  void* (*DefaultSM)();
  status_t (*BpsmGetService)(void* self, const std::string* name, void** outBinder);
  void (*IncStrong)(void* self, const void* id);
  void (*DecStrong)(void* self, const void* id);
  status_t (*BpTransact)(void* self, uint32_t code, const Parcel* in, Parcel* out, uint32_t flags);
  void (*ParcelCtor)(Parcel*);
  void (*ParcelDtor)(Parcel*);
  status_t (*WriteInt32)(Parcel*, int32_t);
  status_t (*WriteUint64)(Parcel*, uint64_t);
  status_t (*ReadInt32)(const Parcel*, int32_t*);
  status_t (*ReadInterfaceToken)(const Parcel*, void* string16Out); // 备用
  void (*String16Ctor)(void* self, const char* utf8);
  void (*String16Dtor)(void* self);
  status_t (*WriteInterfaceToken)(Parcel*, const void* string16);
  size_t (*GetDataSize)(const Parcel*);
} A;

static void* resolve(void* h, const char* n) {
  void* p = dlsym(h, n);
  if (!p) { fprintf(stderr, "missing %s\n", n); }
  return p;
}

int main(int argc, char** argv) {
  const char* svc = argc > 1 ? argv[1] : "android.hardware.audio.core.IModule/default";
  uint32_t code = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 11;
  lb = dlopen("libbinder.so", RTLD_NOW | RTLD_GLOBAL);
  lu = dlopen("libutils.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lb || !lu) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
  A.DefaultSM = resolve(lb, "_ZN7android21defaultServiceManagerEv");
  A.BpsmGetService = resolve(lb, "_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE");
  A.IncStrong = resolve(lb, "_ZNK7android7RefBase9incStrongEPKv");
  A.DecStrong = resolve(lb, "_ZNK7android7RefBase10decStrongEPKv");
  A.BpTransact = resolve(lb, "_ZN7android8BpBinder8transactEjRKNS_6ParcelEPS1_j");
  A.ParcelCtor = resolve(lb, "_ZN7android6ParcelC1Ev");
  A.ParcelDtor = resolve(lb, "_ZN7android6ParcelD1Ev");
  A.WriteInt32 = resolve(lb, "_ZN7android6Parcel11writeInt32Ei");
  A.WriteUint64 = resolve(lb, "_ZN7android6Parcel11writeUint64Em");
  A.ReadInt32 = resolve(lb, "_ZNK7android6Parcel9readInt32EPi");
  A.String16Ctor = resolve(lu, "_ZN7android8String16C1EPKc");
  A.String16Dtor = resolve(lu, "_ZN7android8String16D1Ev");
  A.WriteInterfaceToken = resolve(lb, "_ZN7android6Parcel19writeInterfaceTokenERKNS_8String16E");
  A.GetDataSize = resolve(lb, "_ZNK7android6Parcel11getDataSizeEv");
  if (!A.DefaultSM || !A.BpsmGetService || !A.BpTransact || !A.ParcelCtor || !A.ReadInt32) return 3;

  // 1) sm 单例
  void* smref = A.DefaultSM();               // sp<IServiceManager>& → 存的是裸指针
  void* sm = *(void**)smref;
  if (!sm) { fprintf(stderr, "STEP1-FAIL defaultServiceManager null\n"); return 4; }
  // 2) getService（std::string 用与平台一致的 libc++；SSO 布局一致）
  auto name = new std::string(svc);
  char binderSlot[16] = {};                  // sp<IBinder> = {m_ptr}，多留一格防 ABI 差
  status_t st = A.BpsmGetService(sm, name, (void**)binderSlot);
  void* binder = *(void**)binderSlot;
  if (st != 0 || !binder) { fprintf(stderr, "STEP2-FAIL getService st=%d binder=%p\n", st, binder); return 5; }
  printf("STEP1-2-OK sm=%p binder=%p service=%s\n", sm, binder, svc);
  A.IncStrong(binder, svc);                  // 持住引用（id 传什么都行，RefBase 忽略）

  // 3) 组 in parcel：interface token（descriptor '/' 前段）+ 无参方法只到 token
  char pbufIn[512]; char pbufOut[4096];
  Parcel* in = (Parcel*)pbufIn; Parcel* out = (Parcel*)pbufOut;
  A.ParcelCtor(in); A.ParcelCtor(out);
  char s16buf[32] = {};                       // String16{size_t len; char16_t* data} 16B
  char desc[128]; snprintf(desc, sizeof desc, "%s", svc);
  char* slash = strrchr(desc, '/'); if (slash) *slash = 0;   // IModule/default → android.hardware.audio.core.IModule
  A.String16Ctor(s16buf, desc);
  st = A.WriteInterfaceToken(in, s16buf);
  if (st != 0) { fprintf(stderr, "STEP3-FAIL writeInterfaceToken st=%d\n", st); return 6; }
  // 4) transact
  st = A.BpTransact(binder, code, (const Parcel*)in, out, 0);
  printf("STEP4 transact(0x%x) st=%d reply-size=%zu\n", code, st, A.GetDataSize ? A.GetDataSize(out) : 0);
  if (st == 0) {
    int32_t ex = -1;
    status_t r = A.ReadInt32((const Parcel*)out, &ex);
    printf("STEP5 exception-slot rc=%d val=%d%s\n", r, ex, ex == 0 ? " (OK=服务正常回了异常位0)" : " (非0=异常码或首字段不是异常，需对布局)");
    int32_t n = -1;
    r = A.ReadInt32((const Parcel*)out, &n);
    printf("STEP6 next-int32 rc=%d val=%d  (getAudioPorts 期望=数组头; 若像字节数请核对)\n", r, n);
  }
  A.String16Dtor(s16buf);
  A.ParcelDtor(in); A.ParcelDtor(out);
  A.DecStrong(binder, svc);
  delete name;
  return st == 0 ? 0 : 7;
}
