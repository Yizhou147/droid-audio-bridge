/* argsdump：拿"平台自己打包 openOutputStream 入参"的**地面真值**，终结猜字段序。
 * 做法：dlopen AIDL 客户端库 → 调 OpenOutputStreamArguments::writeToParcel(全零结构体)
 *       → 把 AParcel 倒成带偏移的十六进制表。全零对 std::string(SSO)/vector/shared_ptr 都是
 *       合法空值，所以这份字节流就是"结构骨架"：哪几格是 size/presence/版本，直接可见。
 * 用法：su -c '/data/local/tmp/argsdump [库路径]'
 *   默认按顺序试：/system/lib64/android.hardware.audio.core-V4-ndk.so
 *                 /vendor/lib64/android.hardware.audio.core-V2-ndk.so
 * 只读打包，不发任何事务、不出声。 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void AParcel;

static struct {
  void* ndk;
  AParcel* (*Parcel_create)(void);
  size_t (*Parcel_dataSize)(const AParcel*);
  int32_t (*Parcel_setPos)(AParcel*, int32_t);
  int32_t (*Parcel_readByte)(const AParcel*, int8_t*);
} N;

#define S(field, name) do { *(void**)&N.field = dlsym(N.ndk, name); \
  if (!N.field) fprintf(stderr, "missing %s\n", name); } while (0)

static int spill(AParcel* p, uint8_t* buf, int cap, int* lenOut) {
  int n = (int)N.Parcel_dataSize(p), i = 0, stall = 0;
  while (i < n) {
    N.Parcel_setPos(p, i);
    int8_t c;
    if (N.Parcel_readByte(p, &c) == 0) { buf[i++] = (uint8_t)c; stall = 0; continue; }
    /* 对齐垫：NDK 游标跳过 pad，补 0xEE 占位并前跳（与探针同一手法） */
    stall++;
    if (stall > 4 && i + 4 < n) { memset(buf + i, 0xEE, 4); i += 4; stall = 0; continue; }
    if (stall > 20) break;
  }
  *lenOut = i;
  return 0;
}

static void dump(const char* tag, AParcel* p) {
  static uint8_t buf[8192];
  int len = 0;
  spill(p, buf, sizeof buf, &len);
  printf("---- %s: %d 字节 ----\n", tag, len);
  for (int o = 0; o < len; o += 16) {
    printf("%04d: ", o);
    for (int k = 0; k < 16 && o + k < len; k++) printf("%02x ", buf[o + k]);
    for (int k = 0; k < 16 && o + k < len; k++) {
      uint8_t c = buf[o + k];
      putchar((c >= 0x20 && c < 0x7f) ? c : '.');
    }
    printf("\n");
  }
  printf("%d 个 int32（含可疑 size/presence 标 *）：", len / 4);
  for (int o = 0; o + 4 <= len; o += 4) {
    int32_t v; memcpy(&v, buf + o, 4);
    printf("[%d]%d%s ", o, v, (v >= 4 && v <= 4096) ? "*" : "");
  }
  printf("\n");
  fflush(stdout);
}

int main(int argc, char** argv) {
  N.ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.ndk) { fprintf(stderr, "dlopen libbinder_ndk: %s\n", dlerror()); return 2; }
  S(Parcel_create, "AParcel_create");
  S(Parcel_dataSize, "AParcel_getDataSize");
  S(Parcel_setPos, "AParcel_setDataPosition");
  S(Parcel_readByte, "AParcel_readByte");
  if (!N.Parcel_create || !N.Parcel_dataSize || !N.Parcel_setPos || !N.Parcel_readByte) return 3;

  static const char* defs[] = {
    "/system/lib64/android.hardware.audio.core-V4-ndk.so",
    "/vendor/lib64/android.hardware.audio.core-V2-ndk.so", NULL };
  const char* libs[3] = { NULL, NULL, NULL };
  if (argc > 1) { libs[0] = argv[1]; } else { for (int i = 0; defs[i]; i++) libs[i] = defs[i]; }

  for (int i = 0; libs[i]; i++) {
    void* h = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("== %s dlopen 失败: %s\n", libs[i], dlerror()); fflush(stdout); continue; }
    printf("== %s 载入成功\n", libs[i]); fflush(stdout);
    /* 全零 args：V2/V4 的 Arguments 都远小于这个尺寸，给足余量 */
    static uint8_t args[2048];
    memset(args, 0, sizeof args);
    const char* wname =
      "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel";
    int (*writeArgs)(const void*, AParcel*) = (int (*)(const void*, AParcel*))dlsym(h, wname);
    if (!writeArgs) { printf("  没有导出 Arguments::writeToParcel\n"); fflush(stdout); continue; }
    /* MAP 模式：逐词打点（其余保持合法 0）⇒ 反查"结构体第 k 个 int 落到 parcel 哪个偏移"。
     * 一次进程内跑完 24 个词，不重复 dlopen。 */
    if (getenv("MAP")) {
      for (int k = 0; k < 24; k++) {
        memset(args, 0, sizeof args);
        int32_t mark = 0x5100 + k;
        memcpy(args + 4 * k, &mark, 4);
        AParcel* pm = N.Parcel_create();
        if (!pm) break;
        int stm = writeArgs(args, pm);
        static uint8_t b[4096]; int len = 0;
        spill(pm, b, sizeof b, &len);
        int found = -1;
        for (int o = 0; o + 4 <= len; o += 4) { int32_t v; memcpy(&v, b + o, 4); if (v == mark) { found = o; break; } }
        int32_t sz = 0; if (len >= 4) memcpy(&sz, b, 4);
        printf("MAP k=%2d st=%d parcel_size=%d mark@=%d %s\n", k, stm, sz, found,
               found < 0 ? (stm ? "(该词是对象/指针，写时报错或被丢弃)" : "(未出现：被跳过或当作对象读)") : "");
        fflush(stdout);
      }
      continue;
    }
    AParcel* p = N.Parcel_create();
    if (!p) { printf("  AParcel_create 失败\n"); continue; }
    int st = writeArgs(args, p);
    printf("  writeToParcel st=%d\n", st); fflush(stdout);
    dump(libs[i], p);
  }
  return 0;
}
