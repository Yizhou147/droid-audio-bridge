/* argsloop：把"猜字段序"变成"以平台为裁判的数据迭代"——一次构建，设备上反复喂参数，不用 CI。
 * 做法：IN=逗号分隔 int32 → 写进 AParcel → 交给**平台自己的**
 *       OpenOutputStreamArguments::readFromParcel(AParcel const*)；rc==0 才算结构可收。
 *       再把解出的结构体用 writeToParcel 回环打包并打印 ⇒ 每格被读成什么、结构体落成什么样，全可见。
 * 用法：su -c 'IN=0,44,1,8,0,0,0,0,0,0,0 /data/local/tmp/argsloop [lib路径] [service名]'
 *       TRANSACT=1 才真的发事务（默认关；且只对指定 service 发，绝不放音——args 不含任何采样数据）。
 * 只读、不发声。 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

typedef void AParcel;
typedef void AIBinder;

/* 非平凡按值返回走隐藏 x8 槽（§本项目老坑）：openOutputStream / getAudioPorts 都这样 */
__asm__(
".text\n"
".globl call_sret3\n"
"call_sret3:\n"      /* (fn, a0, a1, a2, sret) */
"  mov x8, x4\n"
"  mov x5, x0\n"
"  mov x0, x1\n"
"  mov x1, x2\n"
"  mov x2, x3\n"
"  br  x5\n"
".text\n.globl call_sret4\ncall_sret4:\n  mov x8, x5\n  mov x7, x0\n  mov x0, x1\n  mov x1, x2\n  mov x2, x3\n  mov x3, x4\n  br  x7\n"
".previous\n"
);
extern void call_sret3(void* fn, void* a0, void* a1, void* a2, void* sret);
extern void call_sret4(void* fn, void* a0, void* a1, void* a2, void* a3, void* sret);

/* 符号插桩：可执行文件导出的同名符号在全局作用域里优先，平台 Bp 码经 PLT 调 AIBinder_transact
 * 时会先进这里 ⇒ 转发给真实现，并在返回前把 reply parcel 抄下来（里面有 IStreamOut 与 FMQ 的 fd）。 */
static void spill(AParcel* p, uint8_t* buf, int cap, int* lenOut);   /* 定义在后面 */
static int g_cap;                      /* 1=正在跑我们自己触发的那次 Bp 调用 */
static uint8_t g_reply[8192];
static int g_reply_len;
static AIBinder* g_stream;
static int g_fds[8]; static int g_nfd;
static int (*real_tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t);

__attribute__((visibility("default")))
int AIBinder_transact(AIBinder* binder, uint32_t code, AParcel** in, AParcel** out, uint32_t flags) {
  if (!real_tx) real_tx = (int (*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))
      dlsym(RTLD_NEXT, "AIBinder_transact");
  int r = real_tx(binder, code, in, out, flags);
  if (g_cap && code == 15 && r == 0 && out && *out) {
    AParcel* op = *out;
    g_reply_len = 0;
    spill(op, g_reply, sizeof g_reply, &g_reply_len);
    int (*rb)(const AParcel*, AIBinder**) =
      (int (*)(const AParcel*, AIBinder**))dlsym(RTLD_DEFAULT, "AParcel_readStrongBinder");
    int (*rfd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(RTLD_DEFAULT, "AParcel_readParcelFileDescriptor");
    int (*ri)(const AParcel*, int32_t*) = (int(*)(const AParcel*, int32_t*))dlsym(RTLD_DEFAULT, "AParcel_readInt32");
    int32_t ex = -1;
    if (ri) ri(op, &ex);
    if (rb) { AIBinder* st = NULL; if (rb(op, &st) == 0 && st) g_stream = st; }
    (void)rfd;
    printf("  [插桩] reply=%d 字节 ex=%d stream=%p\n", g_reply_len, ex, (void*)g_stream);
    fflush(stdout);
  }
  return r;
}


static void noop_create(void* a) { (void)a; }
static void noop_destroy(void* a) { (void)a; }
static int32_t noop_transact(AIBinder* b, uint32_t c, const void* in, void* out) {
  (void)b; (void)c; (void)in; (void)out; return 0;
}

static struct {
  void* ndk;
  AParcel* (*Parcel_create)(void);
  size_t (*Parcel_dataSize)(const AParcel*);
  int32_t (*Parcel_setPos)(AParcel*, int32_t);
  int32_t (*Parcel_readByte)(const AParcel*, int8_t*);
  int32_t (*Parcel_writeInt32)(AParcel*, int32_t);
} N;

/* ===== GOT 钩子：不依赖符号插桩（那条被 bionic 的 NEEDED 查找顺序废掉，见 §21）=====
 * 做法：在 core-V4-ndk.so 自己的映射里，搜出所有 8 字节槽 == 真 AIBinder_transact 地址的
 * 位置（就是它的 GOT 槽），mprotect 后改成 my_tx。只动本进程内存，不碰任何文件/分区。 */
static void* g_orig_tx;
static int g_gotcap;
static uint8_t g_greply[8192];
static int g_greply_len;
static AIBinder* g_gstream;
static int g_capcode = 15;                      /* APC 用：额外抄这个码的回包 */
static int g_devid = -1;               /* LOAD2 建出来的"扬声器设备端口"config id，当 patch 的 sink */
static uint8_t g_cfg[200000]; static int g_cfg_len;   /* getAudioPortConfigs / setAudioPortConfig 回包 */
static int g_nq;                       /* GOT 抓到的 FMQ 队列数 */
static int g_qfd[4];
static void* g_qmem[4];
static size_t g_qsz[4];
static void qput(int fd, void* m, size_t sz) {
  if (g_nq < 4 && m && m != MAP_FAILED) { g_qfd[g_nq] = fd; g_qmem[g_nq] = m; g_qsz[g_nq] = sz; g_nq++; }
}
static int my_tx(AIBinder* b, uint32_t code, AParcel** in, AParcel** out, uint32_t flags) {
  int (*o)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
    (int (*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))g_orig_tx;
  int r = o(b, code, in, out, flags);
  if (g_gotcap && code == g_capcode && code != 15 && r == 0 && out && *out) {
    /* AudioPortConfig 里没有 fd/binder ⇒ 整包可逐字节抄（这些码的回包没有对象区） */
    g_cfg_len = 0;
    spill(*out, g_cfg, (int)sizeof g_cfg, &g_cfg_len);
    N.Parcel_setPos(*out, 0);
    printf("  CAP 码%d 抄到 %d 字节\n", code, g_cfg_len); fflush(stdout);
  }
  if (g_gotcap && code == 15 && r == 0 && out && *out) {
    AParcel* op = *out;
    g_greply_len = 0;
    spill(op, g_greply, sizeof g_greply, &g_greply_len);
    int (*rb)(const AParcel*, AIBinder**) =
      (int (*)(const AParcel*, AIBinder**))dlsym(N.ndk, "AParcel_readStrongBinder");
    int (*ri)(const AParcel*, int32_t*) = (int(*)(const AParcel*, int32_t*))dlsym(N.ndk, "AParcel_readInt32");
    N.Parcel_setPos(op, 0);          /* spill 把读位置推到了尾部 —— 不归零后面全是假失败 */
    int32_t ex = -1;
    if (ri) ri(op, &ex);
    if (rb) { AIBinder* st = NULL; if (rb(op, &st) == 0 && st) g_gstream = st; }
    N.Parcel_setPos(op, 0);          /* 交还给平台代码自己解析 */
    printf("  [GOT] reply=%d 字节 ex=%d stream=%p\n", g_greply_len, ex, (void*)g_gstream);
    for (int q = 0; q < g_greply_len; q += 4) {
      printf("%02x:%08x ", q, *(uint32_t*)(g_greply + q));
      if ((q / 4) % 8 == 7) printf("\n");
    }
    printf("\n");
    /* 真正判 fd 的办法：让 AParcel 自己在每个位置尝试读 ParcelFileDescriptor ——
     * 只有对象表里那个位置是 TYPE_FD 才会成功，比"整数看着像 fd"可靠得多。 */
    int (*rfdp)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
    for (int pos = 0; rfdp && pos + 4 <= g_greply_len; pos += 4) {
      int fd = -1;
      N.Parcel_setPos(op, pos);
      if (rfdp(op, &fd) == 0 && fd >= 0 && fd < 4096) {
        struct stat sb;
        char path[64], link[256];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t nl = readlink(path, link, sizeof link - 1);
        if (nl > 0) link[nl] = 0; else snprintf(link, sizeof link, "?");
        int rc2 = fstat(fd, &sb);
        printf("  [GOT] PFD@%d = %d -> %s size=%lld\n", pos, fd, link,
               rc2 == 0 ? (long long)sb.st_size : -1LL);
        { /* ashmem 的 st_size 恒为 0 ⇒ 只能倍增试探真实大小，再 dump 队列头部 */
          size_t sz = 4096, good = 0;
          void* m = MAP_FAILED;
          while (sz <= (1u << 26)) {
            void* t = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (t == MAP_FAILED) break;
            if (m != MAP_FAILED) munmap(m, good);
            m = t; good = sz; sz *= 2;
          }
          { /* grantor 的事件字在 16+extent 处（q2 是 +16400）⇒ 必须比"整数倍试探"再多映一页 */
            void* m2 = mmap(NULL, good + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m2 != MAP_FAILED) { munmap(m, good); m = m2; good += 4096; }
          }
          printf("  [GOT]   mmap 大小=%zu 地址=%p\n", good, m);
          qput(fd, m, good);
          if (m != MAP_FAILED) {
            const uint8_t* q = (const uint8_t*)m;
            for (int t = 0; t < 48; t += 8)
              printf("  [GOT]   +%d=0x%016llx\n", t, (unsigned long long)*(const uint64_t*)(q + t));
          }
        }
      }
    }
    N.Parcel_setPos(op, 0);
    fflush(stdout);
    fflush(stdout);
  }
  return r;
}
static const char* g_mod = "android.hardware.audio.core-V4-ndk.so";

/* scudo 的堆指针带 0xb400… 顶层 tag，任何"按数值范围判指针"的写法都会把它们全滤掉
 * （STREAM 扫描就是这么连续几轮扫到空的）。唯一可信的判据：这个地址在我自己进程的
 * 某条可读映射里。映射表读一次缓存起来。 */
static struct { unsigned long a, b; char r; } g_mmap[16384];
static int g_nmmap;
static void load_maps(void) {
  if (g_nmmap) return;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f) && g_nmmap < (int)(sizeof g_mmap / sizeof *g_mmap)) {
    unsigned long a, b; char perm[8];
    if (sscanf(line, "%lx-%lx %4s", &a, &b, perm) != 3) continue;
    g_mmap[g_nmmap].a = a; g_mmap[g_nmmap].b = b; g_mmap[g_nmmap].r = (perm[0] == 'r');
    g_nmmap++;
  }
  fclose(f);
}
/* fd 猎取：把一块内存里所有"0..4096 的小整数"当 fd 试 fstat，并读 /proc/self/fd/N 的指向。
 * openOutputStream 的回包有 533 字节，远超"一个 binder 句柄"所需 —— 平台解析后，
 * 里面的 fd（若有）要么落在 Return 结构里，要么还在回包的对象表里，两种都扫。 */
static void hunt_fds(const char* tag, const void* buf, int len) {
  int hits = 0;
  for (int k = 0; k + 4 <= len; k += 4) {
    int v; memcpy(&v, (const char*)buf + k, 4);
    if (v <= 0 || v > 1024) continue;
    struct stat sb;
    if (fstat(v, &sb) != 0) continue;
    char path[256], link[256];
    snprintf(path, sizeof path, "/proc/self/fd/%d", v);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n > 0) link[n] = 0; else snprintf(link, sizeof link, "?");
    printf("  %s+%d = fd %d  ->  %s  size=%lld\n", tag, k, v, link, (long long)sb.st_size);
    hits++;
  }
  if (!hits) printf("  %s: 没有像 fd 的值\n", tag);
  fflush(stdout);
}

/* 在一个 AIDL Bp 对象里找真句柄：某个 word 的 vptr 属于 libbinder*（即 AIBinder 的实现类）。
 * 不猜成员偏移 —— BpStreamOut 的句柄实测在 obj+32，猜 +8 拿到的是引用计数。 */
static int readable(void* p);
static void* find_binder_in(void* obj, const char* tag) {
  if (!readable(obj)) { printf("  %s: 对象不可读 %p\n", tag, obj); fflush(stdout); return NULL; }
  for (int k = 0; k < 24; k++) {
    void* w = *(void**)((char*)obj + 8 * k);
    if (!readable(w)) continue;
    Dl_info dw = {0};
    if (dladdr(*(void**)w, &dw) && dw.dli_fname &&
        (strstr(dw.dli_fname, "libbinder") || (dw.dli_sname && strstr(dw.dli_sname, "BpBinder")))) {
      printf("  %s: AIBinder 在 obj+%d = %p（vptr 在 %s）\n", tag, 8 * k, w,
             strrchr(dw.dli_fname, '/') + 1); fflush(stdout);
      return w;
    }
  }
  printf("  %s: 对象里没找到 libbinder 句柄\n", tag); fflush(stdout);
  return NULL;
}
static int g_nrd;
static int64_t nowms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000; }
/* 空字符串也算"已设置"⇒ getenv 真值判断是陷阱（本轮 WALL/CSLOT 都被它坑过）*/
static int flag(const char* n) { const char* v = getenv(n); return v && v[0]; }
/* 裁判：HAL 进程里所有 write_* 工作线程的累计 CPU（jiffies）。
 * 我们那条流的工作线程 CPU 从 0 开始涨 ⇒ 它真的消费了我们写的东西。 */
static void dump_write_threads(const char* tag) {
  DIR* d = opendir("/proc");
  if (!d) return;
  struct dirent* de;
  while ((de = readdir(d))) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    char path[160];
    snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
    FILE* f = fopen(path, "rb");
    if (!f) continue;
    char cb[256] = {0};
    int rn = (int)fread(cb, 1, sizeof cb - 1, f);
    fclose(f);
    if (rn <= 0 || !strstr(cb, "audiohalservice")) continue;
    snprintf(path, sizeof path, "/proc/%s/task", de->d_name);
    DIR* td = opendir(path);
    if (!td) continue;
    struct dirent* te;
    while ((te = readdir(td))) {
      char cp[192]; snprintf(cp, sizeof cp, "/proc/%s/task/%s/comm", de->d_name, te->d_name);
      FILE* cf = fopen(cp, "r");
      if (!cf) continue;
      char comm[64] = {0};
      if (fgets(comm, sizeof comm, cf)) {}
      fclose(cf);
      comm[strcspn(comm, "\r\n")] = 0;
      if (strncmp(comm, "write", 5)) continue;
      char sp[192]; snprintf(sp, sizeof sp, "/proc/%s/task/%s/stat", de->d_name, te->d_name);
      FILE* sf = fopen(sp, "r");
      if (!sf) continue;
      char buf[1024] = {0};
      int sn = (int)fread(buf, 1, sizeof buf - 1, sf);
      fclose(sf);
      long cpu = 0;
      char state = '?';
      if (sn > 0) {
        char* p2 = strrchr(buf, ')');
        if (p2) {
          char* w = p2 + 1;
          for (int k = 0; k < 14 && w; k++) {
            while (*w == ' ') w++;
            char* nx = strchr(w, ' ');
            if (nx) *nx = 0;
            if (k == 0) state = *w;
            if (k == 11 || k == 12) cpu += atol(w);
            if (!nx) break;
            w = nx + 1;
          }
        }
      }
      printf("  THR %s: tid=%s comm=%s state=%c cpu=%ld\n", tag, te->d_name, comm, state, cpu);
    }
    closedir(td);
    break;
  }
  closedir(d);
  fflush(stdout);
}

static int readable(void* p) {
  unsigned long v = (unsigned long)p & ((1UL << 48) - 1);   /* 去掉 scudo/MTE 的顶层 tag */
  int res = 0;
  if ((unsigned long)p >= 0x1000 && v >= 0x1000) {
    load_maps();
    for (int i = 0; i < g_nmmap; i++)
      if (v >= g_mmap[i].a && v + 8 <= g_mmap[i].b && g_mmap[i].r) { res = 1; break; }
  }
  if (getenv("DBG") && g_nrd < 30) {
    printf("  readable(%p→%lx)=%d 映射段=%d\n", p, v, res, g_nmmap); fflush(stdout);
    g_nrd++;
  }
  return res;
}
static int install_got_hook(const char* modname, void* real) {
  /* 槽地址 = 模块载入基址 + .rela.plt 给的静态偏移（AIBinder_transact = 0x5b400，
   * 由 pull 下来的 core-V4-ndk.so 反解 PLT 得到；调用点 openOutputStream+0xdc → bl plt(0x55ed8)）。 */
  unsigned long slotoff = strtoul(getenv("GOTOFF") ? getenv("GOTOFF") : "5b400", NULL, 16);
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return -1;
  char line[512];
  unsigned long bias = ~0UL;
  int segs = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long a, b2; char perm[8];
    if (sscanf(line, "%lx-%lx %4s", &a, &b2, perm) != 3) continue;
    if (!strstr(line, modname)) continue;
    segs++;
    printf("  [GOT] 映射 %lx-%lx %s\n", a, b2, perm);
    if (a < bias) bias = a;
  }
  fclose(f);
  if (bias == ~0UL) { printf("  [GOT] 模块没载入\n"); return -2; }
  unsigned long slot = bias + slotoff;
  printf("  [GOT] 段=%d bias=%lx 槽=%lx（+%lx）应含 real=%p\n", segs, bias, slot, slotoff, real);
  void* cur = *(void**)slot;
  Dl_info di = {0}, dr = {0};
  if (dladdr(cur, &di)) printf("  [GOT] 槽现值=%p 是 %s（%s）\n", cur, di.dli_sname ? di.dli_sname : "?", di.dli_fname);
  else printf("  [GOT] 槽现值=%p dladdr 失败\n", cur);
  if (dladdr(real, &dr)) printf("  [GOT] real 在 %s（%s）\n", dr.dli_fname, dr.dli_sname ? dr.dli_sname : "?");
  if (cur != real) {
    printf("  [GOT] 不匹配：链接器把槽解析成了别的东西。用现值当真身继续挂钩。\n");
    g_orig_tx = cur;
  }
  if (mprotect((void*)(slot & ~0xfffUL), 0x2000, PROT_READ | PROT_WRITE) != 0) {
    printf("  [GOT] mprotect 失败: %s\n", strerror(errno)); return -3;
  }
  *(void**)slot = (void*)my_tx;
  printf("  [GOT] 打桩槽 @%lx 写后读回=%p\n", slot, *(void**)slot); fflush(stdout);
  return 0;
}


static void spill(AParcel* p, uint8_t* buf, int cap, int* lenOut) {
  int n = (int)N.Parcel_dataSize(p), i = 0, stall = 0;
  *lenOut = 0;
  while (i < n) {
    N.Parcel_setPos(p, i);
    int8_t c;
    if (N.Parcel_readByte(p, &c) == 0) { buf[i++] = (uint8_t)c; stall = 0; continue; }
    stall++;
    if (stall > 4 && i + 4 < n) { memset(buf + i, 0xEE, 4); i += 4; stall = 0; continue; }
    if (stall > 20) break;
  }
  *lenOut = i;
}

static void dumpInts(const char* tag, const uint8_t* b, int len) {
  printf("%s(%d 字节): ", tag, len);
  for (int o = 0; o + 4 <= len; o += 4) {
    int32_t v; memcpy(&v, b + o, 4);
    printf("%d ", v);
  }
  printf("\n");
  fflush(stdout);
}

int main(int argc, char** argv) {
  char* in0 = strdup(getenv("IN") ? getenv("IN") : "0");
  char* in = in0;
  N.ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_GLOBAL);
  if (!N.ndk) { fprintf(stderr, "dlopen ndk: %s\n", dlerror()); return 2; }
  N.Parcel_create = (AParcel* (*)(void))dlsym(N.ndk, "AParcel_create");
  N.Parcel_dataSize = (size_t (*)(const AParcel*))dlsym(N.ndk, "AParcel_getDataSize");
  N.Parcel_setPos = (int32_t(*)(AParcel*, int32_t))dlsym(N.ndk, "AParcel_setDataPosition");
  N.Parcel_readByte = (int32_t(*)(const AParcel*, int8_t*))dlsym(N.ndk, "AParcel_readByte");
  N.Parcel_writeInt32 = (int32_t(*)(AParcel*, int32_t))dlsym(N.ndk, "AParcel_writeInt32");
  int (*writeBinder)(AParcel*, AIBinder*) =
    (int (*)(AParcel*, AIBinder*))dlsym(N.ndk, "AParcel_writeStrongBinder");
  int (*writeI64)(AParcel*, int64_t) = (int (*)(AParcel*, int64_t))dlsym(N.ndk, "AParcel_writeInt64");
  (void)writeBinder; (void)writeI64;
  if (!N.Parcel_create || !N.Parcel_dataSize || !N.Parcel_setPos || !N.Parcel_readByte ||
      !N.Parcel_writeInt32) { fprintf(stderr, "libbinder_ndk 符号缺\n"); return 3; }

  const char* lib = argc > 1 ? argv[1] : "/system/lib64/android.hardware.audio.core-V4-ndk.so";
  g_mod = lib;
  void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 4; }
  int (*readArgs)(void* self, const AParcel*) = (int(*)(void*, const AParcel*))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments14readFromParcelEPK7AParcel");
  int (*writeArgs)(const void*, AParcel*) = (int(*)(const void*, AParcel*))dlsym(h,
    "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
  if (!readArgs || !writeArgs) { fprintf(stderr, "缺 read/write 符号\n"); return 5; }

  /* 1) 造请求 parcel */
  AParcel* p = N.Parcel_create();
  int cnt = 0;
  if (getenv("AUTO")) {
    /* AUTO=1：IN 用分号分隔的词表，首个 size 槽由本工具回填。词：整数 / B / L / H:<hex字节> */
    extern int auto_build(void* h);
    return auto_build(h);
  }
  /* SPEC 语法：整数（可 0x…）= 一个 int32；"B" = 一个 null strong binder（24B flat 对象，
   * 平台真值里那两个回调槽就是它）；"L" = int64 0。⇒ 纯数据即可复刻 88B 骨架，不用每试一次跑 CI。 */
  for (char* tok = strtok(in, ","); tok; tok = strtok(NULL, ",")) {
    if (!strcmp(tok, "B")) { if (writeBinder) writeBinder(p, NULL); else N.Parcel_writeInt32(p, 0); }
    else if (!strcmp(tok, "L")) { if (writeI64) writeI64(p, 0);
                                 else { N.Parcel_writeInt32(p, 0); N.Parcel_writeInt32(p, 0); } }
    else N.Parcel_writeInt32(p, (int32_t)strtol(tok, NULL, 0));
    cnt++;
  }
  free(in0);
  printf("IN: %d 个词\n", cnt); fflush(stdout);
  if (getenv("DUMP")) {   /* DUMP=<service> + TR=<code>：用现有 in parcel 直接发，倒回包原始字节 */
    extern int do_dump(char** argv, int argc);
    return do_dump(argv, argc);
  }

  /* 2) 平台读端当裁判 */
  static char args[2048];
  memset(args, 0, sizeof args);
  N.Parcel_setPos(p, 0);
  int rr = readArgs(args, p);
  printf("READ st=%d  struct 前 16 个 int: ", rr);
  for (int k = 0; k < 16; k++) { int32_t v; memcpy(&v, args + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);

  /* 3) 回环：把解出的结构体再打包一次，看平台认为的"等价字节" */
  AParcel* q = N.Parcel_create();
  int wr = writeArgs(args, q);
  static uint8_t rb[4096]; int rl = 0;
  spill(q, rb, sizeof rb, &rl);
  printf("WRITE st=%d 回环 %d 字节\n", wr, rl); fflush(stdout);
  dumpInts("回环 int 流", rb, rl);

  if (!getenv("TRANSACT")) { printf("(未发事务；TRANSACT=1 才发)\n"); return 0; }

  /* 4) 可选：真的用平台 BpModule 发这个 args（对指定 service，无采样数据 ⇒ 无声） */
  const char* svc = argc > 2 ? argv[2] : "android.hardware.audio.core.IModule/r_submix";
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*Class_define)(const char*, void*, void*, void*) =
    (void*(*)(const char*, void*, void*, void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Associate)(AIBinder*, const void*) = (int(*)(AIBinder*, const void*))dlsym(N.ndk, "AIBinder_associateClass");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = Class_define(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Associate) Associate(b, cls);
  void (*CtorBp)(void*, AIBinder**) = (void(*)(void*, AIBinder**))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModuleC1ERKN3ndk10SpAIBinderE");
  void* openOut = dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModule16openOutputStreamERKNS3_7IModule25OpenOutputStreamArgumentsEPNS5_22OpenOutputStreamReturnE");
  if (!CtorBp || !openOut) { fprintf(stderr, "缺 BpModule 符号\n"); return 7; }
  static char bp[128]; memset(bp, 0, sizeof bp);
  CtorBp(bp, &b);
  static char ret[2048]; memset(ret, 0, sizeof ret);
  static char sret[32]; memset(sret, 0, sizeof sret);
  if (getenv("GOT")) {
    g_orig_tx = (void*)dlsym(N.ndk, "AIBinder_transact");
    printf("GOT 钩子安装：real=%p\n", g_orig_tx); fflush(stdout);
    install_got_hook(g_mod, g_orig_tx);
    g_gotcap = 1;
  }
  if (getenv("CAP")) { g_cap = 1; }
  call_sret3(openOut, bp, args, ret, sret);
  if (getenv("GOT")) {
    g_gotcap = 0;
    printf("GOT 结果: stream=%p reply=%d\n", (void*)g_gstream, g_greply_len); fflush(stdout);
    int (*rfd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
    AParcel* cin = NULL; AParcel* cout = NULL;
    int (*Prep2)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
    int (*Tx2)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) = (int(*)(AIBinder*,uint32_t,AParcel**,AParcel**,uint32_t))g_orig_tx;
    if (g_gstream && Prep2 && Tx2 && !Prep2(g_gstream, &cin)) {
      N.Parcel_writeInt32(cin, 0);
      int s3 = Tx2(g_gstream, 1, &cin, &cout, 0x10);
      size_t sz3 = cout ? N.Parcel_dataSize(cout) : 0;
      printf("GOT2 getStreamCommon st=%d reply=%zu\n", s3, sz3); fflush(stdout);
      if (s3 == 0 && cout) {
        for (int pos = 0; pos + 4 <= (int)sz3; pos += 4) {
          int fd = -1;
          N.Parcel_setPos(cout, pos);
          if (rfd && rfd(cout, &fd) == 0 && fd >= 0 && fd < 4096) { printf("  FMQ fd@%d = %d\n", pos, fd); fflush(stdout); }
        }
      }
    }
  }
  if (getenv("CAP")) {
    g_cap = 0;
    printf("CAP 完成: stream=%p reply=%d 字节\n", (void*)g_stream, g_reply_len); fflush(stdout);
    printf("CAP reply hex "); for (int i = 0; i < g_reply_len && i < 120; i++) printf("%02x", g_reply[i]);
    printf("\n"); fflush(stdout);
  }
  void* so = *(void**)sret;
  int st = so ? ((int32_t(*)(const void*))dlsym(N.ndk, "AStatus_getStatus"))(so) : 0;
  printf("TRANSACT st=%d ret 前 8 个 int: ", st);
  for (int k = 0; k < 8; k++) { int32_t v; memcpy(&v, ret + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  return st == 0 ? 0 : 8;
}


/* ===== 追加：DUMP / AUTO 两个模式（都用本文件上方的静态状态，通过参数传入句柄） ===== */
static int g_argc; static char** g_argv;
static AParcel* newParcel(void) { return N.Parcel_create(); }

int do_dump(char** argv, int argc) {
  (void)argv; (void)argc;
  const char* svc = getenv("DUMP");
  uint32_t code = (uint32_t)strtoul(getenv("TR") ? getenv("TR") : "10", NULL, 0);
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
  int (*Prep)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
  int (*Tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
    (int(*)(AIBinder*,uint32_t,AParcel**,AParcel**,uint32_t))dlsym(N.ndk, "AIBinder_transact");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl = strrchr(desc, '/'); if (sl) *sl = 0;
  const void* cls = CD(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Assoc) Assoc(b, cls);
  AParcel* in = NULL; AParcel* out = NULL;
  if (Prep(b, &in)) { fprintf(stderr, "prepare 失败\n"); return 7; }
  N.Parcel_writeInt32(in, 0);
  int st = Tx(b, code, &in, &out, 0);
  static uint8_t buf[65536]; int len = 0;
  if (out) spill(out, buf, sizeof buf, &len);
  printf("DUMP code=%u st=%d len=%d\n", code, st, len);
  printf("HEX ");
  for (int i = 0; i < len; i++) printf("%02x", buf[i]);
  printf("\n"); fflush(stdout);
  return st == 0 ? 0 : 8;
}

int auto_build(void* h) {
  int (*readArgs)(void*, const AParcel*) = (int(*)(void*, const AParcel*))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments14readFromParcelEPK7AParcel");
  if (!readArgs) { fprintf(stderr, "缺 readFromParcel\n"); return 5; }
  const char* spec = getenv("IN") ? getenv("IN") : "0";
  char* sp = strdup(spec);
  AParcel* p = newParcel();
  int32_t ph0 = N.Parcel_dataSize(p);
  N.Parcel_writeInt32(p, 0);                       /* size 占位 */
  size_t nbytes = 0;
  for (char* tok = strtok(sp, ";"); tok; tok = strtok(NULL, ";")) {
    if (!strcmp(tok, "B")) {
      int (*wb)(AParcel*, AIBinder*) = (int(*)(AParcel*, AIBinder*))dlsym(N.ndk, "AParcel_writeStrongBinder");
      if (wb) wb(p, NULL); else N.Parcel_writeInt32(p, 0);
      nbytes += 24;
    } else if (!strncmp(tok, "L", 1) && (tok[1] == 0 || tok[1] == ':')) {
      int (*w64)(AParcel*, int64_t) = (int(*)(AParcel*, int64_t))dlsym(N.ndk, "AParcel_writeInt64");
      int64_t v = tok[1] == ':' ? (int64_t)strtoll(tok + 2, NULL, 0) : 0;
      if (w64) w64(p, v); else { N.Parcel_writeInt32(p, (int32_t)v); N.Parcel_writeInt32(p, (int32_t)(v >> 32)); }
    } else if (!strncmp(tok, "H:", 2)) {
      /* 按 int32 写：AParcel_writeByte 每字节要占 4 字节格（上次 120B 变 480B 就是这么来的） */
      const char* q = tok + 2;
      while (q[0]) {
        char w[9] = {0};
        int k = 0;
        while (k < 8 && q[k]) { w[k] = q[k]; k++; }
        if (k < 8) { while (k < 8) w[k++] = '0'; }   /* 尾部补零对齐到 4 字节 */
        N.Parcel_writeInt32(p, (int32_t)strtoul(w, NULL, 16));
        q += 8;
      }
    } else { N.Parcel_writeInt32(p, (int32_t)strtol(tok, NULL, 0)); nbytes += 4; }
  }
  int32_t end = (int32_t)N.Parcel_dataSize(p);
  N.Parcel_setPos(p, ph0);
  N.Parcel_writeInt32(p, end - ph0);              /* 回填 size（含自身） */
  N.Parcel_setPos(p, end);
  printf("AUTO 写出块长 %d（dataSize=%zu）\n", end - ph0, N.Parcel_dataSize(p)); fflush(stdout);
  static char args[4096]; memset(args, 0, sizeof args);
  N.Parcel_setPos(p, 0);
  int rr = readArgs(args, p);
  printf("READ st=%d struct前8int=", rr);
  for (int k = 0; k < 8; k++) { int32_t v; memcpy(&v, args + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  /* 关键调试：把我手发的字节 vs 平台从"解出的结构体"再编码的字节 并排打出来，逐字节找差异 */
  {
    AParcel* q2 = N.Parcel_create();
    int (*wArgs)(const void*, AParcel*) = (int (*)(const void*, AParcel*))dlsym(h,
      "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
    if (wArgs) {
      int w2 = wArgs(args, q2);
      static uint8_t mine[4096], theirs[4096];
      int lm = 0, lt = 0;
      N.Parcel_setPos(p, 0); spill(p, mine, sizeof mine, &lm);
      N.Parcel_setPos(q2, 0); spill(q2, theirs, sizeof theirs, &lt);
      printf("MINE(%d) ", lm); for (int i = 0; i < lm; i++) printf("%02x", mine[i]); printf("\n");
      printf("THEIRS(%d) st=%d ", lt, w2); for (int i = 0; i < lt; i++) printf("%02x", theirs[i]); printf("\n");
      for (int i = 0; i < lm || i < lt; i++) {
        uint8_t a = i < lm ? mine[i] : 0xff, b2 = i < lt ? theirs[i] : 0xff;
        if (a != b2) printf("  首差@%d: mine=%02x theirs=%02x\n", i, a, b2);
        if (a != b2) break;
      }
      fflush(stdout);
    }
  }
  if (getenv("RAWX") && rr == 0) {
    /* 隔离实验：请求 parcel 由 prepareTransaction + 平台 writeToParcel 产生（字节与 Bp 路径同源），
     * 但发送用我的原始 AIBinder_transact ⇒ 区分"打包差异"与"调用方式差异"。 */
    const char* svc = getenv("SVC") ? getenv("SVC") : "android.hardware.audio.core.IModule/r_submix";
    void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
    void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
    AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
    void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
    void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
    int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
    int (*Prep)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
    int (*Tx)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
      (int(*)(AIBinder*,uint32_t,AParcel**,AParcel**,uint32_t))dlsym(N.ndk, "AIBinder_transact");
    int (*rB)(const AParcel*, AIBinder**) = (int(*)(const AParcel*, AIBinder**))dlsym(N.ndk, "AParcel_readStrongBinder");
    if (Proc_setMax) Proc_setMax(4);
    if (Proc_start) Proc_start();
    AIBinder* mb = SM_get(svc);
    if (mb && IncStrong) IncStrong(mb);
    char dsc[160]; snprintf(dsc, sizeof dsc, "%s", svc);
    char* sl3 = strrchr(dsc, '/'); if (sl3) *sl3 = 0;
    const void* cls3 = CD(dsc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
    if (Assoc && mb) Assoc(mb, cls3);
    int (*wArgs2)(const void*, AParcel*) = (int(*)(const void*, AParcel*))dlsym(h,
      "_ZNK4aidl7android8hardware5audio4core7IModule25OpenOutputStreamArguments13writeToParcelEP7AParcel");
    for (int fi = 0; fi < 3; fi++) {
      uint32_t fl = (uint32_t)(fi == 0 ? 0 : fi == 1 ? 0x10000000 : 0x10000010);
      AParcel* rin = NULL; AParcel* rout = NULL;
      if (!mb || Prep(mb, &rin)) { printf("RAWX prepare 失败\n"); break; }
      N.Parcel_writeInt32(rin, 0);
      int ws = wArgs2(args, rin);
      int rs = Tx(mb, 15, &rin, &rout, fl);
      size_t rsz = rout ? N.Parcel_dataSize(rout) : 0;
      int32_t rex = -1; AIBinder* stm = NULL;
      if (rout) { int (*rI)(const AParcel*, int32_t*) = (int(*)(const AParcel*,int32_t*))dlsym(N.ndk,"AParcel_readInt32"); if (rI) rI(rout, &rex); if (rB) rB(rout, &stm); }
      printf("RAWX flags=%#x writeArgs=%d transact=%d reply=%zu ex=%d stream=%p\n",
             fl, ws, rs, rsz, rex, (void*)stm); fflush(stdout);
    }
    return 0;
  }
  if (!getenv("TRANSACT") || rr != 0) return rr == 0 ? 0 : 9;

  /* 用平台 BpModule 发这个"平台刚解出来"的结构体 ⇒ 打包完全由平台做，我们只负责喂对的字节 */
  const char* svc = getenv("SVC") ? getenv("SVC") : "android.hardware.audio.core.IModule/r_submix";
  void (*Proc_setMax)(uint32_t) = (void(*)(uint32_t))dlsym(N.ndk, "ABinderProcess_setThreadPoolMaxThreadCount");
  void (*Proc_start)(void) = (void(*)(void))dlsym(N.ndk, "ABinderProcess_startThreadPool");
  AIBinder* (*SM_get)(const char*) = (AIBinder*(*)(const char*))dlsym(N.ndk, "AServiceManager_getService");
  void (*IncStrong)(AIBinder*) = (void(*)(AIBinder*))dlsym(N.ndk, "AIBinder_incStrong");
  void* (*CD)(const char*, void*, void*, void*) = (void*(*)(const char*,void*,void*,void*))dlsym(N.ndk, "AIBinder_Class_define");
  int (*Assoc)(AIBinder*, const void*) = (int(*)(AIBinder*,const void*))dlsym(N.ndk, "AIBinder_associateClass");
  if (Proc_setMax) Proc_setMax(4);
  if (Proc_start) Proc_start();
  AIBinder* b = SM_get(svc);
  if (!b) { fprintf(stderr, "getService 空\n"); return 6; }
  if (IncStrong) IncStrong(b);
  char desc[160]; snprintf(desc, sizeof desc, "%s", svc);
  char* sl2 = strrchr(desc, '/'); if (sl2) *sl2 = 0;
  const void* cls = CD(desc, (void*)noop_create, (void*)noop_destroy, (void*)noop_transact);
  if (Assoc) Assoc(b, cls);
  void (*CtorBp)(void*, AIBinder**) = (void(*)(void*, AIBinder**))dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModuleC1ERKN3ndk10SpAIBinderE");
  void* openOut = dlsym(h,
    "_ZN4aidl7android8hardware5audio4core8BpModule16openOutputStreamERKNS3_7IModule25OpenOutputStreamArgumentsEPNS5_22OpenOutputStreamReturnE");
  if (!CtorBp || !openOut) { fprintf(stderr, "缺 BpModule 符号\n"); return 7; }
  static char bp[128]; memset(bp, 0, sizeof bp);
  CtorBp(bp, &b);
  static char ret[4096]; memset(ret, 0, sizeof ret);
  static char sret[32];
  if (getenv("GOT")) {
    g_orig_tx = (void*)dlsym(N.ndk, "AIBinder_transact");
    /* bionic 的 lazy PLT：GOT 槽要等第一次调用才写成函数地址 ⇒ 先用 getAudioPorts 走一遍同一个槽 */
    void* gaps = dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModule13getAudioPortsEPNSt3__16vectorINS0_5media5audio6common9AudioPortENS5_9allocatorISA_EEEE");
    if (gaps) {
      static char vec[64]; memset(vec, 0, sizeof vec);
      static char ws[32];
      call_sret3(gaps, bp, vec, vec, ws);
      printf("GOT 预热 getAudioPorts vec=[%p,%p]\n", *(void**)&vec[0], *(void**)&vec[8]); fflush(stdout);
    } else printf("GOT 预热：没找到 getAudioPorts 符号\n");
    printf("GOT 安装：real=%p\n", g_orig_tx); fflush(stdout);
    install_got_hook(g_mod, g_orig_tx);
    g_gotcap = 1;
  }
  /* ===== APC=1：给容器自己建一份 AudioPortConfig =====
   * 框架把 54~63 那些现成 config 都占着（直开会被拒：ex=-3 EX_ILLEGAL_ARGUMENT +
   * HAL 明说 "already has a stream opened on it"），所以必须先自己创建一份再开流。
   * 全程让平台编解码：我只在元素头上动两个 int32（id / 读 portId），不逆完整结构。 */
  if (getenv("APC")) {
    int want = getenv("APCPORT") ? atoi(getenv("APCPORT")) : 2;   /* 2 = deep_buffer_out */
    void* gapc = dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModule19"
                  "getAudioPortConfigsEPNSt3__16vectorINS0_5media5audio6common"
                  "15AudioPortConfigENS5_9allocatorISA_EEEE");
    int (*wI32)(AParcel*, int32_t) = (int(*)(AParcel*, int32_t))N.Parcel_writeInt32;
    if (!gapc) printf("  APC: 缺 getAudioPortConfigs 符号\n");
    else {
      static char vec[64]; memset(vec, 0, sizeof vec);
      static char vs[32]; memset(vs, 0, sizeof vs);
      g_capcode = 10; g_cfg_len = 0; g_gotcap = 1;
      call_sret3(gapc, bp, vec, vec, vs);
      printf("  APC: getAudioPortConfigs 抄到 %d 字节 vec=[%p,%p]\n", g_cfg_len,
             *(void**)&vec[0], *(void**)&vec[8]); fflush(stdout);
      printf("  APC 头 20 个 int32:");
      for (int t = 0; t < 20 && t*4 < g_cfg_len; t++) printf(" %d", *(int*)(g_cfg + 4*t));
      printf("\n"); fflush(stdout);
      /* ACP3：拿平台刚解析出来的**真 config**（getAudioPortConfigs 填的 vector）当模板，
       * 只把 id 清 0 再交给 setAudioPortConfig ⇒ 其它字段（采样率/掩码/格式/flags）都是完整合法的，
       * 正好治 §"fully specified? 0"。元素长度未知 ⇒ 用 (end-begin)/9 反推并校验。 */
      void* sac = dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModule18"
                    "setAudioPortConfigERKNS0_5media5audio6common15AudioPortConfigEPS8_Pb");
      if (!sac) printf("  ACP: 缺 BpModule::setAudioPortConfig 符号\n");
      {
        char* vb = *(char**)&vec[0];
        char* ve = *(char**)&vec[8];
        long span = (long)(ve - vb);
        char* tmpl = NULL; int stride = 0;
        printf("  ACP: vector [%p..%p) span=%ld\n", (void*)vb, (void*)ve, span); fflush(stdout);
        /* stride 反推要**全元素校验**：上一版只看"最后一个元素 portId 命中"，
         * 结果挑到半错位(struct 大小 272 时用了 136)，字段读成 (null) ⇒ HAL 回
         * "requested ... do not match port's flags / not fully specified"。 */
        for (int cand = 40; cand <= 512 && !tmpl; cand += 8) {
          if (span <= 0 || span % cand) continue;
          int cnt = (int)(span / cand), good = 0;
          for (int k = 0; k < cnt; k++) {
            char* e = vb + (long)k * cand;
            int id = *(int*)e, port = *(int*)(e + 4), sr = *(int*)(e + 8), reach = *(int*)(e + 12);
            int sr_ok = (sr == 0);
            for (int q = 0; q < 8; q++) {
              static const int SRS[8] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100 };
              if (sr == SRS[q]) sr_ok = 1;
            }
            if (sr == 48000 || sr == 64000 || sr == 88200 || sr == 96000 ||
                sr == 128000 || sr == 176400 || sr == 192000) sr_ok = 1;
            if (id >= 0 && id <= 4096 && port >= 1 && port <= 60 && sr_ok &&
                reach >= 0 && reach <= 2) good++;
          }
          if (good != cnt) continue;
          for (int k = 0; k < cnt; k++) {
            char* e = vb + (long)k * cand;
            int port = *(int*)(e + 4);
            printf("    stride=%d cfg[%d] id=%d portId=%d sr=%d\n", cand, k,
                   *(int*)e, port, *(int*)(e + 8));
            if (port == want && !tmpl) { tmpl = e; stride = cand; }
          }
          if (tmpl) break;
        }
        /* NOPRE=1：不带模板，交一份**自己填**的 AudioPortConfig（接管轮里框架 config 数为 0，
         * 只能自己来）。POKE=off:val[,off:val...] 直接往 C++ 结构的任意 int32 偏移写值 ——
         * HAL 会把收到的结构逐字段明文打回来，所以偏移对不对一眼可判，几轮就能试完。 */
        if (getenv("NOPRE") && atoi(getenv("NOPRE")) == 1) {
          static char my[512]; memset(my, 0, sizeof my);
          *(int32_t*)(my + 0) = 0;                     /* id = 0 ⇒ 新建 */
          *(int32_t*)(my + 4) = want;                  /* portId */
          char* pk = getenv("POKE");
          if (pk) {
            char* w = strtok(pk, ",");
            while (w) {
              int off = 0, val = 0;
              if (sscanf(w, "%d:%d", &off, &val) == 2 && off >= 0 && off + 4 <= (int)sizeof my) {
                *(int32_t*)(my + off) = val;
                printf("  NOPRE: 写 +%d = %d\n", off, val);
              }
              w = strtok(NULL, ",");
            }
          }
          static char ro[512]; memset(ro, 0, sizeof ro);
          static char ok2[8]; static char st8[64];
          g_capcode = 18; g_cfg_len = 0;
          call_sret4(sac, bp, my, ro, ok2, st8);
          void* sv8 = *(void**)st8;
          int (*gst8)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
          printf("  NOPRE: st=%d ok=%d 回传出 8 int32:", sv8 && gst8 ? gst8(sv8) : -999, *(int*)ok2);
          for (int t = 0; t < 8; t++) printf(" %d", *(int*)(ro + 4*t));
          printf("\n"); fflush(stdout);
          int nd = getenv("DUMPRO") ? atoi(getenv("DUMPRO")) : 0;
          for (int t = 0; t + 8 <= nd; t += 8) {
            printf("  RO +%d:", t);
            for (int q = 0; q < 8; q++) printf(" %d", *(int*)(ro + t + 4*q));
            printf("\n");
          }
          fflush(stdout);
          if (*(int*)ro > 0) printf("  NOPRE: ★新 portConfigId = %d\n", *(int*)ro);
          g_capcode = 15;
          tmpl = my;   /* 复用后面的分支，不再走模板路径 */

        }
        /* SAVE=1：把选中的那份 config 从**回包字节**里切出来存盘（wire 元素 = [int32 size][字段…]）。
         * LOAD=1：反过来 —— 从文件读回字节，交给平台自己的 AudioPortConfig::readFromParcel 解析成
         * C++ 对象（optional 会正确 engage、string 会正确构造），再清 id 发码 18。
         * 这一对绕开了"C++ 结构偏移考古"，也是接管轮里框架 config 全空时唯一的模板来源。 */
        if (getenv("SAVE") && g_cfg_len > 16) {
          int sv = -1, svsz = 0;
          for (int q = 0; q + 12 <= g_cfg_len; q += 4) {
            int sz = *(int*)(g_cfg + q), id = *(int*)(g_cfg + q + 4), port = *(int*)(g_cfg + q + 8);
            /* 采样率在元素头部的哪一格取决于 optional 布局 ⇒ 不猜位置，只要头部 44 字节里
               出现过合法采样率就认定这是"完整 config"元素（上一版按固定偏移读，误命中 20 字节的碎片） */
            int srk = 0;
            for (int z = 8; z + 4 <= 48 && q + z + 4 <= g_cfg_len; z += 4) {
              int v = *(int*)(g_cfg + q + z);
              if (v == 48000 || v == 44100 || v == 96000 || v == 192000 || v == 16000 || v == 8000) srk = 1;
            }
            if (port == want && sz >= 60 && sz <= 4096 && id >= 1 && id <= 4096 && srk) {
              sv = q; svsz = sz; break;
            }
          }
          if (sv < 0) printf("  SAVE: 回包里没有 portId=%d 的 config\n", want);
          else {
            FILE* fp = fopen(getenv("SAVE"), "wb");
            if (fp) { fwrite(g_cfg + sv, 1, (size_t)svsz, fp); fclose(fp);
                      printf("  SAVE: 存了 %d 字节（wire @%d，原 id=%d）到 %s\n", svsz, sv,
                             *(int*)(g_cfg + sv + 4), getenv("SAVE")); }
            else printf("  SAVE: 写 %s 失败\n", getenv("SAVE"));
            fflush(stdout);
          }
          if (flag("SAVEONLY")) {   /* 只抄模板就收工：绝不开流，不占端口 */
            g_gotcap = 0;
            printf("  SAVE: SAVEONLY ⇒ 不开流直接退出\n"); fflush(stdout);
            return 0;
          }
        }
        char* usecfg = NULL;
        if (getenv("LOAD")) {
          void* raf = dlsym(h, "_ZN4aidl7android5media5audio6common15AudioPortConfig14readFromParcelEPK7AParcel");
          FILE* fp = fopen(getenv("LOAD"), "rb");
          if (!raf) printf("  LOAD: 缺 AudioPortConfig::readFromParcel 符号\n");
          else if (!fp) printf("  LOAD: 打不开 %s（先在 anland 跑一次 SAVE）\n", getenv("LOAD"));
          else {
            static uint8_t fb[8192];
            int rn = (int)fread(fb, 1, sizeof fb, fp); fclose(fp);
            int sz = *(int*)fb;
            printf("  LOAD: 读到 %d 字节，元素声明 size=%d\n", rn, sz); fflush(stdout);
            static char obj[1024]; memset(obj, 0, sizeof obj);
            AParcel* ip = N.Parcel_create();
            for (int t = 0; t + 4 <= rn; t += 4) {
              int32_t v; memcpy(&v, fb + t, 4);
              ((int(*)(AParcel*, int32_t))N.Parcel_writeInt32)(ip, v);
            }
            N.Parcel_setPos(ip, 0);
            int rrc = ((int(*)(void*, const AParcel*))raf)(obj, ip);
            printf("  LOAD: readFromParcel rc=%d ⇒ id=%d portId=%d sr=%d mask=%d type=%d\n",
                   rrc, *(int*)obj, *(int*)(obj + 4), *(int*)(obj + 8), *(int*)(obj + 16),
                   *(int*)(obj + 24)); fflush(stdout);
            if (rrc == 0 && *(int*)(obj + 4) == want) usecfg = obj;
            *(int*)obj = 0;                                   /* id=0 ⇒ 新建 */
          }
        }
        if (usecfg && sac) {
          static char r9[512]; memset(r9, 0, sizeof r9);
          static char ok9[8]; static char st9[64];
          g_capcode = 18; g_cfg_len = 0;
          call_sret4(sac, bp, usecfg, r9, ok9, st9);
          void* sv9 = *(void**)st9;
          int (*gst9)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
          printf("  LOAD: setAudioPortConfig st=%d ok=%d ⇒ 新 id=%d portId=%d\n",
                 sv9 && gst9 ? gst9(sv9) : -999, *(int*)ok9, *(int*)r9, *(int*)(r9 + 4));
          fflush(stdout);
          if (*(int*)r9 > 0) {
            *(int32_t*)args = *(int*)r9;
            printf("  LOAD: ★args 的 portConfigId 已改成 %d\n", *(int*)r9); fflush(stdout);
          }
          g_capcode = 15;
          tmpl = usecfg;   /* 让后面模板分支不再动 vector */
        }
        /* LOAD2=<dev cfg 字节文件> DEVPORT=<端口号>：再克隆一份 config —— 这次是**扬声器设备端口**
         * （portId 23）。框架正常路径里 AudioPolicy 会把设备端口也 setAudioPortConfig 一遍，
         * 我们接管轮里没有它，patch 就没有汇点可指。建出来的 id 存 g_devid 给 PATCH 用。 */
        if (getenv("LOAD2")) {
          int want2 = getenv("DEVPORT") ? atoi(getenv("DEVPORT")) : 23;
          void* raf2 = dlsym(h, "_ZN4aidl7android5media5audio6common15AudioPortConfig14readFromParcelEPK7AParcel");
          FILE* fp2 = fopen(getenv("LOAD2"), "rb");
          if (!raf2) printf("  LOAD2: 缺 AudioPortConfig::readFromParcel\n");
          else if (!fp2) printf("  LOAD2: 打不开 %s\n", getenv("LOAD2"));
          else {
            static uint8_t fb2[8192];
            int rn2 = (int)fread(fb2, 1, sizeof fb2, fp2); fclose(fp2);
            static char obj2[1024]; memset(obj2, 0, sizeof obj2);
            AParcel* ip2 = N.Parcel_create();
            for (int t = 0; t + 4 <= rn2; t += 4) {
              int32_t v; memcpy(&v, fb2 + t, 4);
              N.Parcel_writeInt32(ip2, v);
            }
            N.Parcel_setPos(ip2, 0);
            int rc2 = ((int(*)(void*, const AParcel*))raf2)(obj2, ip2);
            printf("  LOAD2: 读到 %d 字节 readFromParcel rc=%d ⇒ id=%d portId=%d\n",
                   rn2, rc2, *(int*)obj2, *(int*)(obj2 + 4)); fflush(stdout);
            if (rc2 != 0) { printf("  LOAD2: 解析失败，不发事务\n"); }
            else if (!sac) printf("  LOAD2: 缺 setAudioPortConfig\n");
            else {
              *(int*)obj2 = 0;                                  /* id=0 ⇒ 新建 */
              static char r2[512]; memset(r2, 0, sizeof r2);
              static char ok2[8]; static char st2[64];
              int oldcap = g_capcode; g_capcode = 18; g_cfg_len = 0;
              call_sret4(sac, bp, obj2, r2, ok2, st2);
              void* sv2 = *(void**)st2;
              int (*gst2)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
              printf("  LOAD2: setAudioPortConfig(portId=%d) st=%d ok=%d ⇒ 新 id=%d portId=%d\n",
                     want2, sv2 && gst2 ? gst2(sv2) : -999, *(int*)ok2,
                     *(int*)r2, *(int*)(r2 + 4));
              /* 回包里是完整的 AudioPortConfig 字节 ⇒ 抄下来，字段对不对 HAL 的明文回显会告我们 */
              if (g_cfg_len > 0) {
                printf("  LOAD2: 回包 %d 字节:", g_cfg_len);
                for (int t = 0; t + 4 <= g_cfg_len && t < 60; t += 4) printf(" %d", *(int*)(g_cfg + t));
                printf("\n");
              }
              if (*(int*)r2 > 0) g_devid = *(int*)r2;
              g_capcode = oldcap;
            }
          }
          fflush(stdout);
        }
        if (!sac) printf("  ACP: 没有 setAudioPortConfig 符号，跳过\n");
        else if (!tmpl || getenv("LOAD") || (getenv("NOPRE") && atoi(getenv("NOPRE")) == 1)) printf("  ACP: 模板法没命中（stride 反推失败）\n");
        else {
          int oldid = *(int*)tmpl; (void)oldid;
          printf("  ACP: 模板=stride %d 里的 id=%d portId=%d ⇒ 克隆并清 id\n", stride, oldid, want); fflush(stdout);
          *(int*)tmpl = 0;
          static char res3[512]; memset(res3, 0, sizeof res3);
          static char ok3[8]; memset(ok3, 0, sizeof ok3);
          static char st7[64]; memset(st7, 0, sizeof st7);
          g_capcode = 18; g_cfg_len = 0;
          call_sret4(sac, bp, tmpl, res3, ok3, st7);
          void* sv7 = *(void**)st7;
          int (*gst7)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
          printf("  ACP3: st=%d ok=%d 新 config 头 8 int32:",
                 sv7 && gst7 ? gst7(sv7) : -999, *(int*)ok3);
          for (int t = 0; t + 8 <= (getenv("DUMPRO") ? atoi(getenv("DUMPRO")) : 0); t += 8) {
            printf("  RO +%d:", t);
            for (int q = 0; q < 8; q++) printf(" %d", *(int*)(res3 + t + 4*q));
            printf("\n");
          }
          for (int t = 0; t < 8; t++) printf(" %d", *(int*)(res3 + 4*t));
          printf("\n"); fflush(stdout);
          *(int*)tmpl = oldid;                          /* 还原平台对象 */
          int nid = *(int*)(res3 + 4);                  /* [id][portId] 里 portId 在 +4? 看打印再定 */
          printf("  ACP3: res3[0]=%d res3[1]=%d\n", *(int*)res3, *(int*)(res3+4));
          nid = *(int*)res3;
          if (nid > 0) {
            printf("  ACP3: ★新 portConfigId = %d（只有我们在用）\n", nid); fflush(stdout);
            /* auto_build 里的 args 是**平台解出来的 C++ OpenOutputStreamArguments 结构**
             * （不是 AParcel！上一版拿它当 AParcel 用 ⇒ SIGSEGV @0x58）。
             * 第一个字段就是 portConfigId ⇒ 直接改内存。 */
            *(int32_t*)args = nid;
          }
          g_capcode = 15;
        }
      }
      (void)sac;
  apc_done:
      g_capcode = 15;
    }
  }

  /* ===== PP=1：AudioPatch 的"平台代打"实验 =====
   * 手搓 4 种打包全被拒（-22 = HAL 侧 Parcel::read 失败；免 size 那版 0x80000008），
   * 所以改成：① 用平台自己的 BpModule::getAudioPatches 读出 5 条真 patch 的 C++ 对象，
   *          ② 用 AudioPatch::writeToParcel 把其中一条再编码一次 ⇒ 拿到**框架真发出去的那串字节**，
   *          ③ 之后所有 patch 都走平台读写，我们只按偏移改 id。 */
  if (flag("PP")) {
    void* gap = dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModule15"
                  "getAudioPatchesEPNSt3__16vectorINS3_10AudioPatchENS5_9allocatorIS7_EEEE");
    void* sap = dlsym(h, "_ZN4aidl7android8hardware5audio4core8BpModule13"
                  "setAudioPatchERKNS3_10AudioPatchEPS5_");
    void* prd = dlsym(h, "_ZN4aidl7android8hardware5audio4core10AudioPatch14readFromParcelEPK7AParcel");
    void* pwr = dlsym(h, "_ZNK4aidl7android8hardware5audio4core10AudioPatch13writeToParcelEP7AParcel");
    printf("  PP: gap=%p sap=%p read=%p write=%p\n", gap, sap, prd, pwr); fflush(stdout);
    static char pvec[64];
    static char pws[32];
    static char pobj[1024];
    if (flag("PCAP") && gap) {
      memset(pvec, 0, sizeof pvec);
      int oc = g_capcode; g_capcode = 8; g_cfg_len = 0;
      call_sret3(gap, bp, pvec, pvec, pws);
      g_capcode = oc;
      char* pb0 = *(char**)&pvec[0]; char* pe0 = *(char**)&pvec[8];
      long span = (long)(pe0 - pb0);
      printf("  PP: vector=[%p,%p) span=%ld 回包 %d 字节\n", (void*)pb0, (void*)pe0, span, g_cfg_len);
      for (int cnt = 1; cnt <= 8; cnt++) {
        if (span % cnt) continue;
        long st = span / cnt;
        if (st < 16 || st > 512 || st % 8) continue;
        printf("  PP: 候选 count=%d sizeof(AudioPatch)=%ld\n", cnt, st);
      }
      /* 对象头 16 个 int32；其中像指针的都把 *ptr 的前两个 int 打出来（sources/sinks 数组就在里面） */
      long stride = span > 0 ? span / (getenv("PCNT") ? atoi(getenv("PCNT")) : 1) : 0;
      int cnt = getenv("PCNT") ? atoi(getenv("PCNT")) : 1;
      if (cnt > 0 && stride > 0 && stride * cnt == span) {
        for (int e = 0; e < cnt; e++) {
          char* o = pb0 + (long)e * stride;
          printf("  PP p%d +%d:", e, (int)(o - pb0));
          for (int q = 0; q < 16; q++) printf(" %d", *(int*)(o + 4 * q));
          printf("\n");
          for (int q = 0; q + 4 <= stride; q += 4) {
            void* cand = *(void**)(o + q);
            if (!readable(cand)) continue;
            int* ip = (int*)cand;
            printf("      +%2d 指针 %p -> [%d,%d]\n", q, cand, ip[0], readable(ip + 4) ? ip[1] : -9999);
          }
        }
      }
      /* 平台再编码一次 ⇒ 这就是框架发出的 AudioPatch 字节 */
      if (pwr && stride > 0) {
        AParcel* pp2 = N.Parcel_create();
        int ws = ((int(*)(const void*, AParcel*))pwr)(pb0, pp2);
        static uint8_t pbb[512]; int pl2 = 0;
        N.Parcel_setPos(pp2, 0); spill(pp2, pbb, sizeof pbb, &pl2);
        printf("  PP writeToParcel st=%d %d 字节:", ws, pl2);
        for (int t = 0; t + 4 <= pl2; t += 4) printf(" %d", *(int*)(pbb + t));
        printf("\n");
        if (getenv("SAVEP")) {
          FILE* fp = fopen(getenv("SAVEP"), "wb");
          if (fp) { fwrite(pbb, 1, (size_t)pl2, fp); fclose(fp); printf("  PP: 存 %d 字节到 %s\n", pl2, getenv("SAVEP")); }
        }
      }
      fflush(stdout);
    }
    /* LOADP=<文件>：把存下来的字节交回平台解析成对象，按 POKE/PPOKE 改 id，再用平台 Bp 发出去。
     *   POKE=off:val    直接写对象里的 int32（off=0 就是 AudioPatch.id）
     *   PPOKE=off:val   把对象 off 处的指针跟进去，写它指向数组的第一个 int32（sources/sinks） */
    if (getenv("LOADP") && prd && sap) {
      FILE* fp = fopen(getenv("LOADP"), "rb");
      static uint8_t lb[1024];
      int ln = fp ? (int)fread(lb, 1, sizeof lb, fp) : -1;
      if (fp) fclose(fp);
      /* SAVEP 时 spill 只能读出 29/32 字节 ⇒ 先补齐到元素自己声明的长度，
       * 否则 readFromParcel 因缺尾字节返回 -61（这次侥幸能用，但别赌）。 */
      if (ln > 4) {
        int decl = *(int*)lb;
        if (decl > ln && decl <= (int)sizeof lb) {
          memset(lb + ln, 0, decl - ln);
          printf("  PP: LOADP 补齐 %d -> %d 字节\n", ln, decl);
          ln = decl;
        }
      }
      printf("  PP: LOADP %s 读到 %d 字节\n", getenv("LOADP"), ln); fflush(stdout);
      if (ln > 4) {
        AParcel* ip = N.Parcel_create();
        for (int t = 0; t + 4 <= ln; t += 4) { int32_t v; memcpy(&v, lb + t, 4); N.Parcel_writeInt32(ip, v); }
        N.Parcel_setPos(ip, 0);
        memset(pobj, 0, sizeof pobj);
        int rc = ((int(*)(void*, const AParcel*))prd)(pobj, ip);
        printf("  PP: readFromParcel rc=%d 对象头:", rc);
        for (int q = 0; q < 16; q++) printf(" %d", *(int*)(pobj + 4 * q));
        printf("\n");
        for (int q = 0; q < 64; q += 4) {
          void* cand = *(void**)(pobj + q);
          if (!readable(cand)) continue;
          int* ip2 = (int*)cand;
          printf("      +%2d 指针 %p -> [%d,%d]\n", q, cand, ip2[0], readable(ip2 + 1) ? ip2[1] : -9999);
        }
        char* pk = getenv("POKE");
        if (pk) {
          char* w1 = strtok(pk, ",");
          while (w1) {
            int off = 0, val = 0;
            if (sscanf(w1, "%d:%d", &off, &val) == 2 && off >= 0 && off + 4 <= (int)sizeof pobj) {
              *(int32_t*)(pobj + off) = val;
              printf("  PP: POKE +%d=%d\n", off, val);
            }
            w1 = strtok(NULL, ",");
          }
        }
        char* pp3 = getenv("PPOKE");
        if (pp3) {
          char* w = strtok(pp3, ",");
          while (w) {
            int off = 0, val = 0;
            if (sscanf(w, "%d:%d", &off, &val) == 2 && off + 4 <= (int)sizeof pobj) {
              void* tgt = *(void**)(pobj + off);
              if (readable(tgt)) { *(int32_t*)tgt = val; printf("  PP: PPOKE +%d -> %p = %d\n", off, tgt, val); }
              else printf("  PP: PPOKE +%d 指针不可读(%p)\n", off, tgt);
            }
            w = strtok(NULL, ",");
          }
        }
        /* PAUTO=1：实测 C++ 对象 sizeof(AudioPatch)=88，两个 int 数组在 +8 / +32（vector 的 begin）。
         * 源=我们自己的 mix portConfigId（args 的头一个 int32），汇=设备端口 configId（PSINK 或 g_devid）。
         * 上一版手搓同样字节仍被拒（-22），所以这回让平台自己发，异常码/消息一起打出来。 */
        if (flag("PAUTO")) {
          int ps = getenv("PSRC") ? atoi(getenv("PSRC")) : *(int32_t*)args;
          int pd = getenv("PSINK") ? atoi(getenv("PSINK")) : g_devid;
          void* vs = *(void**)(pobj + 8);
          void* vt = *(void**)(pobj + 32);
          if (readable(vs)) { *(int32_t*)vs = ps; printf("  PP: PAUTO sources[0]=%d（+%d）\n", ps, 8); }
          else printf("  PP: PAUTO: sources 指针不可读 %p\n", vs);
          if (readable(vt)) { *(int32_t*)vt = pd; printf("  PP: PAUTO sinks[0]=%d（+%d）\n", pd, 32); }
          else printf("  PP: PAUTO: sinks 指针不可读 %p\n", vt);
        }
        if (flag("SENDP")) {
          static char po[1024]; memset(po, 0, sizeof po);
          static char pws2[32];
          int oc2 = g_capcode; g_capcode = -1;
          call_sret3(sap, bp, pobj, po, pws2);
          g_capcode = oc2;
          void* sv = *(void**)pws2;
          int (*gst)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
          int (*gex)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getExceptionCode");
          const char* (*gmsg)(const void*) = (const char*(*)(const void*))dlsym(N.ndk, "AStatus_getMessage");
          printf("  PP: setAudioPatch st=%d ex=%d msg=\"%s\" 回写对象头:",
                 sv && gst ? gst(sv) : -999, sv && gex ? gex(sv) : -999,
                 sv && gmsg && gmsg(sv) ? gmsg(sv) : "");
          for (int q = 0; q < 8; q++) printf(" %d", *(int*)(po + 4 * q));
          printf("\n"); fflush(stdout);
        }
      }
      fflush(stdout);
    }
    if (flag("PSTOP")) { printf("  PP: PSTOP ⇒ 不开流退出\n"); fflush(stdout); return 0; }
  }

  call_sret3(openOut, bp, args, ret, sret);
  if (getenv("GOT")) {
    g_gotcap = 0;
    printf("GOT 结果: stream=%p reply=%d\n", (void*)g_gstream, g_greply_len); fflush(stdout);
    int (*rfd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
    int (*Prep2)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
    AParcel* cin = NULL; AParcel* cout = NULL;
    if (g_gstream && Prep2 && !Prep2(g_gstream, &cin)) {
      N.Parcel_writeInt32(cin, 0);
      int s3 = ((int(*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))g_orig_tx)(g_gstream, 1, &cin, &cout, 0x10);
      size_t sz3 = cout ? N.Parcel_dataSize(cout) : 0;
      printf("GOT2 getStreamCommon st=%d reply=%zu\n", s3, sz3); fflush(stdout);
      if (s3 == 0 && cout && rfd) {
        for (int pos = 0; pos + 4 <= (int)sz3; pos += 4) {
          int fd = -1;
          N.Parcel_setPos(cout, pos);
          if (rfd(cout, &fd) == 0 && fd >= 0 && fd < 4096) { printf("FMQ_FD@%d=%d\n", pos, fd); fflush(stdout); }
        }
      }
    }
  }
  /* WATCH=毫秒数：高频比对三块队列前 256 字节，抓"谁在动这块共享内存"。
     纯读、不发事务、不写一帧 ⇒ 无声。判据另有两路：
     THR 行（write_db 的累计 CPU）+ /proc/<tid>/syscall 的 futex 地址。 */
  /* GRANT=1：用平台自己的 GrantorDescriptor::readFromParcel 把回包里的 grantor 列表逐条解出来。
   * 为什么必须这样：libfmq 的 initMemory() 里
   *   mReadPtr / mWritePtr / mEvFlagWord ← mapGrantorDescr(READPTRPOS/WRITEPTRPOS/EVFLAGWORDPOS)
   *   mRing                           ← mapGrantorDescr(DATAPTRPOS)
   * 而生成端对 DATAPTRPOS 写的是 **fdIndex=1、offset=0**，其余 fdIndex=0
   * ⇒ 数据区和计数器/事件字**可能不在同一块 fd 里** —— 我们此前把三个 fd 直接当成三块队列，
   * 很可能一直是"在 metadata 区里找元素槽"，那当然没人理。 */
  if (getenv("GRANT") && g_greply_len > 0) {
    void* hfmq = dlopen("/system/lib64/android.hardware.common.fmq-V1-ndk.so", RTLD_NOW | RTLD_GLOBAL);
    int (*rdG)(void*, const AParcel*) = (int(*)(void*, const AParcel*))dlsym(
        hfmq ? hfmq : N.ndk,
        "_ZN4aidl7android8hardware6common3fmq17GrantorDescriptor14readFromParcelEPK7AParcel");
    if (!rdG) printf("  GRANT: 缺 GrantorDescriptor::readFromParcel（hfmq=%p）\n", hfmq);
    else {
      /* grantor 记录里没有 fd/binder 对象 ⇒ 原始字节可整块读出来直接看：
       * GrantorDescriptor = [int32 size][int32 fdIndex][int32 offset][int64 extent] */
      for (int pos = 0; pos + 20 <= g_greply_len; pos += 4) {
        int32_t sz = *(int32_t*)(g_greply + pos);
        if ((sz != 20 && sz != 24) || pos + sz > g_greply_len) continue;
        printf("  GRANT @%d size=%d fdIndex=%d offset=%d words:", pos, sz,
               *(int32_t*)(g_greply + pos + 4), *(int32_t*)(g_greply + pos + 8));
        for (int t = 0; t < sz; t += 4) printf(" %d", *(int32_t*)(g_greply + pos + t));
        printf("\n");
      }
      fflush(stdout);
    }
  }

  /* DUMPQ=1：把每块队列映射里**所有非零字**打出来（最多 48 个/块）。
   * 为什么需要：writeBlocking 的等待方是生产者 ⇒ 它用的写/读指针不一定在我以为的 +0/+8，
   * 先把"哪些字节非零"看清楚，再谈往哪写。 */
  if (getenv("DUMPQ") && g_nq) {
    for (int i = 0; i < g_nq; i++) {
      uint8_t* b = (uint8_t*)g_qmem[i];
      size_t lim = g_qsz[i] < 65536 ? g_qsz[i] : 65536;
      int shown = 0;
      printf("  DUMPQ q%d(fd %d, 映射 %zu) 非零字：", i, g_qfd[i], g_qsz[i]);
      for (size_t t = 0; t + 4 <= lim && shown < 48; t += 4) {
        uint32_t v = *(uint32_t*)(b + t);
        if (v) { printf(" @%zu=0x%x", t, v); shown++; }
      }
      printf("  （共显示 %d 个）\n", shown);
    }
    fflush(stdout);
  }

  if (getenv("WATCH") && getenv("GOT") && g_nq) {
    int ms = atoi(getenv("WATCH"));
    static uint8_t snap[4][256];
    int changed = 0;
    for (int i = 0; i < g_nq; i++) memcpy(snap[i], g_qmem[i], 256);
    int64_t t0 = nowms();
    int rounds = 0;
    while (nowms() - t0 < ms) {
      for (int i = 0; i < g_nq; i++) {
        uint8_t* b = (uint8_t*)g_qmem[i];
        if (memcmp(snap[i], b, 256) != 0) {
          int first = -1, n = 0;
          for (int t = 0; t < 256; t += 4) {
            uint32_t a, c; memcpy(&a, snap[i] + t, 4); memcpy(&c, b + t, 4);
            if (a != c) { if (first < 0) first = t; n++; }
          }
          printf("  WATCH +%lldms q%d 变了 %d 个字，首个 @%d：%u -> %u\n",
                 (long long)(nowms() - t0), i, n, first,
                 first >= 0 ? *(uint32_t*)(snap[i] + first) : 0,
                 first >= 0 ? *(uint32_t*)(b + first) : 0);
          memcpy(snap[i], b, 256);
          changed++;
          fflush(stdout);
        }
      }
      usleep(50000);
      rounds++;
    }
    { char ln[512];
      for (int i = 0; i < g_nq; i++) {
        FILE* mf = fopen("/proc/self/maps", "r");
        if (!mf) break;
        while (fgets(ln, sizeof ln, mf)) {
          unsigned long a = 0, b = 0;
          if (sscanf(ln, "%lx-%lx", &a, &b) == 2 && (unsigned long)g_qmem[i] == a) {
            ln[strcspn(ln, "\n")] = 0;
            printf("  我方 q%d(fd %d) 映射: %s\n", i, g_qfd[i], ln);
            break;
          }
        }
        fclose(mf);
      }
      fflush(stdout);
    }
    printf("  WATCH: %d 轮比对，共 %d 次变化\n", rounds, changed); fflush(stdout);
    dump_write_threads("WATCH末");
  }

  /* TIDS=1：把 HAL 里所有 write_* 线程的 futex 等待地址打出来（syscall 行前两列） */
  if (getenv("TIDS")) {
    DIR* d = opendir("/proc");
    if (d) {
      struct dirent* de;
      while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[160]; snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char cb[256] = {0};
        int rn = (int)fread(cb, 1, sizeof cb - 1, f);
        fclose(f);
        if (rn <= 0 || !strstr(cb, "audiohalservice")) continue;
        snprintf(path, sizeof path, "/proc/%s/task", de->d_name);
        DIR* td = opendir(path);
        if (!td) continue;
        struct dirent* te;
        while ((te = readdir(td))) {
          char cp[192]; snprintf(cp, sizeof cp, "/proc/%s/task/%s/comm", de->d_name, te->d_name);
          FILE* cf = fopen(cp, "r");
          if (!cf) continue;
          char comm[64] = {0};
          if (fgets(comm, sizeof comm, cf)) {}
          fclose(cf);
          comm[strcspn(comm, "\r\n")] = 0;
          if (strncmp(comm, "write", 5)) continue;
          char sp[192]; snprintf(sp, sizeof sp, "/proc/%s/task/%s/syscall", de->d_name, te->d_name);
          FILE* sf = fopen(sp, "r");
          char line[256] = {0};
          if (sf) { if (fgets(line, sizeof line, sf)) {} fclose(sf); }
          line[strcspn(line, "\n")] = 0;
          printf("  TIDS: %s tid=%s %s\n", comm, te->d_name, line);
          { /* 把这个 futex 地址落在 HAL 的哪条映射上打出来（带 inode），
             * 用来判定"它等的到底是不是我们手里这几块队列" */
            unsigned long ua = 0;
            sscanf(line, "%*d %lx", &ua);
            char mp[160]; snprintf(mp, sizeof mp, "/proc/%s/maps", de->d_name);
            FILE* mf = fopen(mp, "r");
            if (mf) {
              char ln[512];
              while (fgets(ln, sizeof ln, mf)) {
                unsigned long a = 0, b = 0;
                if (sscanf(ln, "%lx-%lx", &a, &b) == 2 && a <= ua && ua < b) {
                  ln[strcspn(ln, "\n")] = 0;
                  printf("    ↑ HAL 映射: %s\n", ln);
                  break;
                }
              }
              fclose(mf);
            }
          }
        }
        closedir(td);
        break;
      }
      closedir(d);
      fflush(stdout);
    }
  }

  void* so = *(void**)sret;
  int st = so ? ((int32_t(*)(const void*))dlsym(N.ndk, "AStatus_getStatus"))(so) : 0;
  printf("TRANSACT st=%d ret前6int=", st);
  for (int k = 0; k < 6; k++) { int32_t v; memcpy(&v, ret + 4 * k, 4); printf("%d ", v); }
  printf("\n"); fflush(stdout);
  if (st != 0 || !getenv("STREAM")) return st == 0 ? 0 : 8;

  /* ===== PATCH=1：给这条流接上扬声器 =====
   * HAL 的 StreamOutPrimary::configure 会去 Module::findConnectedDevices(我们的 portConfigId) 找设备，
   * 找不到就打 "no connected devices on stream!!" 并让 transfer 失败 ⇒ 数据被消费但永远到不了 PAL/扬声器。
   * 框架正常路径是 AudioPolicy 发 IModule.setAudioPatch（码 17）。
   * 回包实测格式（out/dump/d8.log，5 条真 patch）：每元素 = [1][size=32][id][1][源configId][1][汇configId][0][0]
   * ⇒ 源/汇是 **portConfigId 的 int 数组**，不是整份 AudioPortConfig
   *   （铁证：core-V2-ndk.so 里 AudioPatch::readFromParcel 调了两次 AParcel_readInt32Array）。 */
  if (flag("PATCH")) {
    int ps = getenv("PSRC") ? atoi(getenv("PSRC")) : *(int32_t*)args;
    int pd = getenv("PSINK") ? atoi(getenv("PSINK")) : g_devid;
    int pid = getenv("PATCHID") ? atoi(getenv("PATCHID")) : 0;
    uint32_t pcode = (uint32_t)(getenv("PCODE") ? strtoul(getenv("PCODE"), NULL, 0) : 17);
    int (*PrepM)(AIBinder*, AParcel**) =
      (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
    int (*TxM)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
      (int(*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))
        (g_orig_tx ? g_orig_tx : dlsym(N.ndk, "AIBinder_transact"));
    AParcel* pin = NULL; AParcel* pout = NULL;
    printf("PATCH: code=%u id=%d src=%d sink=%d\n", pcode, pid, ps, pd); fflush(stdout);
    if (!PrepM || !TxM) printf("  PATCH: 缺 prepare/transact\n");
    else if (ps <= 0 || pd <= 0) printf("  PATCH: src/sink 没准备好（src=%d sink=%d）\n", ps, pd);
    else {
      /* 四种打包组合逐个试（上一版只有"[size][字段] + flags0" ⇒ st=-22；
       * 而 -22(BAD_VALUE) 在这条链路上就是"回包里被读出了对象、我没举手接"）。
       *   bit0 = 不写顶层 objectSize；bit1 = 带 FLAG_ACCEPT_FDS(0x10)。
       * 判据：st=0 且回包第一个 int32（异常码）=0。 */
      int32_t wd[8] = { 32, pid, 1, ps, 1, pd, 0, 0 };
      const char* order = getenv("PATCHVAR") ? getenv("PATCHVAR") : "0123";
      int done = -1;
      for (const char* vp = order; *vp && done < 0; vp++) {
        int v = *vp - '0';
        if (v < 0 || v > 3) continue;
        pin = NULL; pout = NULL;
        if (PrepM(b, &pin)) { printf("  PATCH v%d: prepare 失败\n", v); break; }
        for (int q = (v & 1) ? 1 : 0; q < 8; q++) N.Parcel_writeInt32(pin, wd[q]);
        uint32_t fl = (v & 2) ? 0x10 : 0;
        int oldcap = g_capcode; g_capcode = -1;
        int rs = TxM(b, pcode, &pin, &pout, fl);
        g_capcode = oldcap;
        size_t psz = pout ? N.Parcel_dataSize(pout) : 0;
        static uint8_t pb[512]; int pl = 0;
        if (pout) { N.Parcel_setPos(pout, 0); spill(pout, pb, sizeof pb, &pl); }
        int32_t pex = (pl >= 4) ? *(int32_t*)pb : -999;
        printf("  PATCH v%d(flags=%#x 免size=%d): st=%d 回包 %zu ex=%d:", v, fl, v & 1, rs, psz, pex);
        for (int t = 4; t + 4 <= pl && t < 48; t += 4) printf(" %d", *(int*)(pb + t));
        printf("\n"); fflush(stdout);
        if (rs == 0 && pex == 0) done = v;
      }
      printf("  PATCH 结果=%s\n", done >= 0 ? "已送达" : "全部被拒"); fflush(stdout);
    }
    fflush(stdout);
  }

  /* §18.1：从 Return 缓冲里扫候选指针 ⇒ 若其 vptr 像 C++ 对象且 +8 处像 AIBinder*，
   * 就拿它对 code 1 (getStreamCommon) 发一次原始事务；st=0 且回包非空即命中 stream 句柄。 */
  int (*Prep2)(AIBinder*, AParcel**) = (int(*)(AIBinder*, AParcel**))dlsym(N.ndk, "AIBinder_prepareTransaction");
  int (*Tx2)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t) =
    (int(*)(AIBinder*, uint32_t, AParcel**, AParcel**, uint32_t))dlsym(N.ndk, "AIBinder_transact");
  int (*rFd)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readParcelFileDescriptor");
  int (*rFd2)(AParcel*, int*) = (int(*)(AParcel*, int*))dlsym(N.ndk, "AParcel_readFileDescriptor");
  /* 一次就能定形的实验：FMQ 头部要么是 [u32 writePos][u32 readPos]，要么是 [u64][u64]。
   * 写 writePos 之后如果对侧计数器动了 ⇒ 布局对 **且** 这条流真的活着。
   * 用的还是零采样数据（只动计数器），不会出声。
   *   Q=队列下标  W=字节偏移:值:宽度(4|8)  S=等待毫秒  D=1 只看不动 */
  if (getenv("W") || getenv("Q") || getenv("AW")) {
    int q = atoi(getenv("Q") ? getenv("Q") : "0");
    int woff = 0, wval = 0, wwid = 4;
    if (getenv("W")) sscanf(getenv("W"), "%d:%d:%d", &woff, &wval, &wwid);
    printf("队列抓到 %d 个；选中 %d\n", g_nq, q);
    for (int i = 0; i < g_nq; i++) {
      uint8_t* b = (uint8_t*)g_qmem[i];
      printf("  前 q%d(fd %d, %zu) :", i, g_qfd[i], g_qsz[i]);
      for (int t = 0; t < 32; t += 4) printf(" %d:%u", t, *(uint32_t*)(b + t));
      printf("\n");
    }
    if (q < g_nq && getenv("AW") && !getenv("D")) {
      /* 首包实验：元素 = AudioBuffer{u32 mSize; u32 mReserved} + 载荷（这里载荷全是 0 = 静音）。
       * 元数据按 [u32 writePos][u32 readPos] 在 +0/+4 的假设写。 */
      int aq = q, bytes = 640;
      sscanf(getenv("AW"), "%d:%d", &aq, &bytes);
      if (aq < g_nq) {
        uint8_t* b = (uint8_t*)g_qmem[aq];
        *(uint32_t*)(b + 8) = (uint32_t)bytes;      /* mSize */
        *(uint32_t*)(b + 12) = 0;                   /* mReserved */
        *(uint32_t*)(b + 0) = 1;                    /* writePos = 1 个元素 */
        printf("  AW: q%d 写了 AudioBuffer{size=%d} + writePos=1\n", aq, bytes);
        usleep((getenv("S") ? atoi(getenv("S")) : 2000) * 1000);
        for (int i = 0; i < g_nq; i++) {
          uint8_t* b2 = (uint8_t*)g_qmem[i];
          printf("  后 q%d(fd %d) :", i, g_qfd[i]);
          for (int t = 0; t < 64; t += 4) printf(" %d:%u", t, *(uint32_t*)(b2 + t));
          printf("\n");
        }
      }
    }
    if (q < g_nq && getenv("W") && !getenv("D")) {
      uint8_t* b = (uint8_t*)g_qmem[q];
      if (wwid == 8) *(uint64_t*)(b + woff) = (uint64_t)wval; else *(uint32_t*)(b + woff) = (uint32_t)wval;
      printf("  写了 q%d +%d = %d (宽%d)\n", q, woff, wval, wwid);
      if (flag("THR")) dump_write_threads("前");
      if (flag("FK")) {            /* AIDL FMQ 是 futex 通知的（AshmemFutex），只改计数器不叫醒对方 */
        size_t lim = flag("WALL") ? g_qsz[q] : (size_t)64;
        int nw = 0;
        for (size_t t = 0; t + 4 <= lim; t += 4) {
          if (getenv("FOFF") && (int)t == atoi(getenv("FOFF")))
            *(uint32_t*)((char*)g_qmem[q] + t) |= (uint32_t)(getenv("FVAL") ? atoi(getenv("FVAL")) : 2);
          syscall(SYS_futex, (char*)g_qmem[q] + t, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
          nw++;
        }
        printf("  FK: 对 q%d 的 %d 个字发了 FUTEX_WAKE\n", q, nw); fflush(stdout);
      }
      usleep((getenv("S") ? atoi(getenv("S")) : 1000) * 1000);
      if (flag("THR")) dump_write_threads("后");
      for (int i = 0; i < g_nq; i++) {
        uint8_t* b2 = (uint8_t*)g_qmem[i];
        printf("  后 q%d(fd %d) :", i, g_qfd[i]);
        for (int t = 0; t < 32; t += 4) printf(" %d:%u", t, *(uint32_t*)(b2 + t));
        printf("\n");
      }
    }
    fflush(stdout);
  }
  /* CMD=<tag>[:<payload>] —— 按 AOSP StreamDescriptor.aidl 的权威布局发一条 FMQ 命令：
   *   MQDescriptor.grantors = [u64 计数器A @0][u64 计数器B @8][元素区 @16][eventFlag u32]
   *   Command 是 @FixedSize union（8 字节）：tag 序 = halReservedExit0 getStatus1 start2
   *     burst3 drain4 standby5 pause6 flush7；Reply 是 56 字节。
   * q0=command / q1=reply / q2=data（就是回包字段顺序）。读哪条计数器是 write 还没定，
   * 用 CW=0|8 选，Reply 自己会告诉我们有没有被消费。 */
  if (getenv("CMD") && g_nq >= 2) {
    int tag = 2, payload = 0, wo = 8;                       /* 默认 start */
    sscanf(getenv("CMD"), "%d:%d", &tag, &payload);
    if (getenv("CW")) wo = atoi(getenv("CW"));
    uint8_t* cq = (uint8_t*)g_qmem[0];
    uint8_t* rq = (uint8_t*)g_qmem[1];
    uint64_t wcnt = *(uint64_t*)(cq + wo), rcnt = *(uint64_t*)(cq + (wo ? 0 : 8));
    if (flag("THR")) dump_write_threads("前");
    printf("  CMD: 命令队列 计数器+%d=%llu 另一侧=%llu  Reply@+16 前 56 字节:",
           wo, (unsigned long long)wcnt, (unsigned long long)rcnt);
    for (int t = 16; t < 72; t += 4) printf(" %d:%d", t, *(int32_t*)(rq + t));
    printf("\n");
    if (flag("CSLOT")) {                                  /* 另一种槽布局：[消息长度][tag] */
      *(int32_t*)(cq + 16) = 4;
      *(int32_t*)(cq + 20) = tag;
      printf("  CMD: 用 CSLOT 布局 [len=4][tag=%d]，计数器偏移待试\n", tag);
    } else {
      *(int32_t*)(cq + 16) = tag;                           /* union tag */
      *(int32_t*)(cq + 20) = payload;                       /* Void ⇒ 0；burst 等用它 */
    }
    /* 关键：FMQ 的读写指针**按字节**计（availableToRead()=bytes/quantum）。
     * 命令元素是 8 字节 ⇒ 推进 1 会被算成 0 个可读元素，对侧醒来又睡回去。
     * WINC 默认 8（quantum of Command）。 */
    uint64_t winc = getenv("WINC") ? (uint64_t)atoll(getenv("WINC")) : 8;
    *(uint64_t*)(cq + wo) = wcnt + winc;                    /* 生产者推进 */
    /* libfmq 的 Futex::notify() 是"先把状态字写成 1，再 FUTEX_WAKE"；
     * 只 WAKE 不改值 ⇒ 等的人在 wait() 里复查发现还是 0，又睡回去 —— 这正是
     * 前面几轮 write_db CPU 一直 0 的原因。FOFF 默认 24 = grantor 表里 q0 的事件字。 */
    /* 实测（/proc/<tid>/syscall）：HAL 的工作线程在
     *   futex(uaddr=映射+24, FUTEX_WAIT_BITSET(9, 共享而非 private), val=0, bitset=0x2)
     * ⇒ 通知必须 (a) 把字里的 **bit 0x2（WRITE_NOTIFIED）** 置上，(b) 用**共享**的
     *    FUTEX_WAKE(op=1) —— FUTEX_WAKE_PRIVATE 是另一个 key，根本唤不醒它。 */
    int foff = getenv("FOFF") ? atoi(getenv("FOFF")) : 24;
    int fval = getenv("FVAL") ? atoi(getenv("FVAL")) : 2;
    *(uint32_t*)(cq + foff) |= (uint32_t)fval;
    syscall(SYS_futex, cq + foff, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    syscall(SYS_futex, cq + wo, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
    printf("  CMD: 事件字 +%d 或上 %d 并用共享 FUTEX_WAKE 唤醒\n", foff, fval);
    if (flag("WALL"))      /* 只 WAKE，不改值 —— 改值会把刚写进去的命令/计数器全盖掉 */
      for (size_t t = 0; t + 4 <= g_qsz[0]; t += 4)
        syscall(SYS_futex, cq + t, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0);
    printf("  CMD: 写了 tag=%d payload=%d，计数器 %llu -> %llu，并对事件字(+24)发 WAKE\n",
           tag, payload, (unsigned long long)wcnt, (unsigned long long)(wcnt + 1));
    usleep((getenv("S") ? atoi(getenv("S")) : 1500) * 1000);
    if (flag("THR")) dump_write_threads("后");
    printf("  CMD: 之后 命令队列 +%d=%llu +%d=%llu\n", wo,
           *(unsigned long long*)(cq + wo), wo ? 0 : 8, *(unsigned long long*)(cq + (wo ? 0 : 8)));
    printf("  CMD: Reply@+16:");
    for (int t = 16; t < 72; t += 4) printf(" %d:%d", t, *(int32_t*)(rq + t));
    printf("\n  （Reply 字段：status fmqByteCount | observable.frames/timeNs | "
           "hardware.frames/timeNs | latencyMs xrunFrames state）\n");
    for (int i = 0; i < g_nq; i++) {
      uint8_t* b2 = (uint8_t*)g_qmem[i];
      printf("  AFTER q%d(fd %d, %zu) head32:", i, g_qfd[i], g_qsz[i]);
      for (int t = 0; t < 32 && (size_t)t < g_qsz[i]; t += 4) printf(" %d:%u", t, *(uint32_t*)(b2 + t));
      printf("\n");
    }
    fflush(stdout);
  }
  /* PULL=1：/proc/<tid>/syscall 显示 HAL 的 write_db 阻塞在"命令队列 +24 事件字、bitset 0x2
   * (READ_NOTIFIED)"⇒ 它是**生产者**，我们是消费者。那就按消费者该做的做：
   * 读元素、把读指针推到写指针、再置 READ_NOTIFIED 位 + 共享 WAKE，把它放行；
   * 然后把队列头 64 字节原样打出来 —— 它到底往这儿写了什么，就是真正的协议。 */
  if (getenv("PULL") && g_nq >= 1) {
    uint8_t* cq = (uint8_t*)g_qmem[0];
    for (int round = 0; round < 3; round++) {
      uint64_t w = *(uint64_t*)(cq + 8), r = *(uint64_t*)(cq + 0);
      printf("  PULL 轮%d: 写指针=%llu 读指针=%llu 槽@16:", round,
             (unsigned long long)w, (unsigned long long)r);
      for (int t = 16; t < 32; t += 4) printf(" %d:%u", t, *(uint32_t*)(cq + t));
      printf("\n");
      if (w != r) {                       /* 有货：取走并回报 */
        uint32_t tag = *(uint32_t*)(cq + 16), pl = *(uint32_t*)(cq + 20);
        printf("  PULL: 收到元素 tag=%u payload=%u\n", tag, pl);
        *(uint64_t*)(cq + 0) = w;         /* 消费者读指针 = 写指针 */
        *(uint32_t*)(cq + 24) |= 2;      /* READ_NOTIFIED */
        syscall(SYS_futex, cq + 24, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
      }
      fflush(stdout);
      usleep(1200000);
    }
    printf("  PULL 之后 头 64 字节:");
    for (int t = 0; t < 64; t += 4) printf(" %d:%u", t, *(uint32_t*)(cq + t));
    printf("\n"); fflush(stdout);
    dump_write_threads("PULL后");
  }
  /* SESSION=1：照 VTS 的客户端流程走完整一遍（全程零载荷＝静音）
   *   start → 读 Reply → **把回包队列的读指针按字节推进 + 置 READ 位**（否则对侧生产者会卡死）
   *   → burst 问可写字节数 → 往 dataMQ 写那么多零 → 再 burst 看 hardware.frames 是否推进
   * 队列身份按 StreamDescriptor 字段序：q0=command q1=reply q2=data；
   * 每块布局 [u64 读指针 @0][u64 写指针 @8][元素 @16][事件字 @16+容量]，指针一律按字节计。 */
  if (getenv("SESSION") && g_nq >= 3) {
    uint8_t* cq = (uint8_t*)g_qmem[0];
    uint8_t* rq = (uint8_t*)g_qmem[1];
    uint8_t* dq = (uint8_t*)g_qmem[2];
    size_t dcap = g_qsz[2] > 16408 ? 16384 : 0;
    uint32_t dflag = (uint32_t)(16 + dcap);            /* data 队列的事件字偏移 */
#define SEND_CMD(TAG, PAY) do {                                            \
      *(uint32_t*)(cq + 16) = (uint32_t)(TAG);                             \
      *(uint32_t*)(cq + 20) = (uint32_t)(PAY);                             \
      *(uint64_t*)(cq + 8) += 8;                                           \
      *(uint32_t*)(cq + 24) |= 2;                                          \
      syscall(SYS_futex, cq + 24, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);  \
    } while (0)
#define TAKE_REPLY(OUT) do {                                               \
      (OUT)[0] = *(int32_t*)(rq + 16);   /* status */                      \
      (OUT)[1] = *(int32_t*)(rq + 20);   /* fmqByteCount */                \
      (OUT)[2] = *(int32_t*)(rq + 64);   /* state */                       \
      obsFrames = *(int64_t*)(rq + 24);                                    \
      hwFrames = *(int64_t*)(rq + 40);                                     \
      latMs = *(int32_t*)(rq + 56); xrun = *(int32_t*)(rq + 60);           \
      *(uint64_t*)(rq + 0) += 56;                                          \
      *(uint32_t*)(rq + 72) |= 2;                                          \
      syscall(SYS_futex, rq + 72, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);  \
    } while (0)
    int rpy[3]; int64_t hwFrames = 0, obsFrames = 0; int latMs = -1, xrun = -1;
    SEND_CMD(2, 0);                                   /* start */
    usleep(600000);
    TAKE_REPLY(rpy);
    printf("  SESSION: start -> Reply{status=%d bytes=%d state=%d}\n", rpy[0], rpy[1], rpy[2]);
    fflush(stdout);
    /* 输出方向的正确用法：先把数据投进 dataMQ（环形位置 = 写指针 % 容量），
     * 再发 burst(N) 告诉 HAL "刚写了 N 字节"；Reply.fmqByteCount = 它真消费的字节数。
     * 载荷全零 ⇒ 全程静音。 */
    /* TONE=<hz>：写正弦而不是零（出声！只在用户点头后用）。
     * 同时把"生产者必须自己看剩余空间"补上 —— 之前固定投 1920 且不看 rp，
     * 静音时无所谓，连续投真信号会盖掉 HAL 还没读的槽。 */
    int tones = flag("TONE") ? atoi(getenv("TONE")) : 0;
    double amp = getenv("AMP") ? atof(getenv("AMP")) : 3.2e7;   /* ≈16 位满幅的 1/2000 ⇒ 很轻 */
    int rounds = getenv("ROUNDS") ? atoi(getenv("ROUNDS")) : (tones ? 60 : 8);
    int slp = getenv("SLEEPMS") ? atoi(getenv("SLEEPMS")) : (tones ? 25 : 700);
    unsigned long long gframe = 0;                      /* 相位连续，跨轮不跳变 */
    for (int iter = 0; iter < rounds; iter++) {
      uint64_t rp64 = *(uint64_t*)(dq + 0), wp64 = *(uint64_t*)(dq + 8);
      uint64_t avail = dcap - (wp64 - rp64);            /* 还能塞多少字节 */
      int want = tones ? 8192 : 1920;
      int chunk = (int)(avail < (uint64_t)want ? avail : (uint64_t)want);
      chunk -= chunk % 8;                               /* 2 声道 × int32 = 8 字节/帧 */
      if (chunk <= 0) { usleep(20000); continue; }      /* 满了就等消费者 */
      uint64_t wp = wp64 % dcap;
      if (tones) {
        int nf = chunk / 8;
        for (int f = 0; f < nf; f++) {
          int32_t v = (int32_t)(amp * sin(2.0 * M_PI * (double)tones *
                                          (double)(gframe + (unsigned long long)f) / 48000.0));
          uint64_t o1 = (wp + (uint64_t)f * 8) % dcap, o2 = (wp + (uint64_t)f * 8 + 4) % dcap;
          memcpy(dq + 16 + o1, &v, 4);
          memcpy(dq + 16 + o2, &v, 4);
        }
        gframe += (unsigned long long)nf;
      } else {
        size_t first = dcap - wp;                       /* 环形回绕 */
        memset(dq + 16 + wp, 0, (size_t)chunk < first ? (size_t)chunk : first);
        if ((size_t)chunk > first) memset(dq + 16, 0, (size_t)chunk - first);
      }
      *(uint64_t*)(dq + 8) += (uint64_t)chunk;
      *(uint32_t*)(dq + dflag) |= 2;
      syscall(SYS_futex, dq + dflag, FUTEX_WAKE, 0x7fffffff, NULL, NULL, 0);
      SEND_CMD(3, chunk);                              /* burst(N) */
      usleep((useconds_t)slp * 1000u);
      TAKE_REPLY(rpy);
      printf("  SESSION 轮%d: 投 %d 字节 burst(%d) -> Reply{status=%d 消费=%d state=%d} "
             "obs=%lld hw=%lld lat=%d xrun=%d data 读=%llu 写=%llu\n", iter, chunk, chunk,
             rpy[0], rpy[1], rpy[2], (long long)obsFrames, (long long)hwFrames, latMs, xrun,
             (unsigned long long)*(uint64_t*)(dq + 0), (unsigned long long)*(uint64_t*)(dq + 8));
      fflush(stdout);
    }
#undef SEND_CMD
#undef TAKE_REPLY
    dump_write_threads("SESSION末");   /* write_db 的累计 CPU：非 0 才说明真在搬数据 */
  }

  hunt_fds("Return(平台解析后的结构)", ret, 512);
  hunt_fds("greply(抓到的原始回包)", g_greply, g_greply_len);
  AIBinder* stream = NULL;
  printf("RETSHEAP ");
  for (int off = 0; off < 128; off += 8) { void* w = *(void**)(ret + off);
    printf("%d=%p%s ", off, w, readable(w) ? "*" : ""); }
  printf("\n"); fflush(stdout);            /* 带 * 的是我进程里真能读的地址 */
  /* DEEP=1：平台已经把整个 OpenOutputStreamReturn 解析好了 —— 结构里的指针指向的就是
   * 它自己的 FMQ 对象。挨个 dump 指向的内存，字段值（含 fd、元素大小、计数偏移）
   * 直接由平台的解析代码填好，比我逆协议可靠得多。vptr 的符号名还顺带告诉我类名。 */
  if (getenv("DEEP")) {
    for (int off = 0; off + 8 <= 256; off += 8) {
      void* pv = *(void**)(ret + off);
      if (!readable(pv)) continue;
      Dl_info di = {0};
      int hv = dladdr(*(void**)pv, &di);
      printf("  RET+%d -> %p  vptr=%p<-%s:%s\n", off, pv, *(void**)pv,
             hv && di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "-",
             hv && di.dli_sname ? di.dli_sname : "?");
      printf("      int32:");
      for (int k = 0; k < 16; k++) {
        int32_t v = *(int32_t*)((char*)pv + 4 * k);
        printf(" %d:%d", 4 * k, v);
      }
      printf("\n      ptr :");
      for (int k = 0; k < 8; k++) printf(" %d:%p", 8 * k, *(void**)((char*)pv + 8 * k));
      printf("\n");
    }
    fflush(stdout);
  }
  g_nrd = 0;
  for (int off = 0; off + 16 <= 128; off += 8) {   /* 只扫 Return 头部，别拿随机指针往别的 binder 对象发事务 */
    void* cand = *(void**)(ret + off);
    if (!readable(cand)) continue;                 /* 带 scudo tag 的堆指针，按数值范围判必全灭 */
    void* vptr = *(void**)cand;
    Dl_info dv = {0};
    int hv = dladdr(vptr, &dv);
    const char* vf = hv && dv.dli_fname ? strrchr(dv.dli_fname, '/') + 1 : "-";
    void* maybe = readable(vptr) ? *(void**)((char*)cand + 8) : NULL;   /* BpInterface 的 mRemoteBinder */
    AParcel* in3 = NULL; AParcel* out3 = NULL;
    printf("  cand ret+%d=%p vptr=%p<-%s:%s +8=%p\n", off, cand, vptr, vf,
           hv && dv.dli_sname ? dv.dli_sname : "?", maybe); fflush(stdout);
    if (getenv("OBJS") && !(hv && dv.dli_sname && strstr(dv.dli_sname, "BpStreamOut"))) continue;
    /* BpInterface 的成员偏移不猜：对象里"某个 word 的 vptr 属于 libbinder*（即 AIBinder 实现
     * 如 ACppBpBinder）"才是真句柄。扫前 24 个 word 找它。 */
    if (hv && dv.dli_sname && strstr(dv.dli_sname, "BpStreamOut")) {
      /* 静态解 vtable 走不通（.rela.dyn 是 SHT_ANDROID_RELA 紧凑格式），运行期直接问 dladdr。
       * slot k 对应事务码 k+1（AIDL Bp 类的固定规律）。 */
      for (int k = 0; k < 24; k++) {
        void* fn = *(void**)((char*)vptr + 16 + 8 * k);
        Dl_info df = {0};
        if (dladdr(fn, &df) && df.dli_sname)
          printf("  VT 码%-2d = %p %s\n", k + 1, fn, df.dli_sname);
      }
      fflush(stdout);
      for (int k = 0; k < 24; k++) {
        void* w = *(void**)((char*)cand + 8 * k);
        if (!readable(w)) continue;
        Dl_info dw = {0};
        int hw = dladdr(*(void**)w, &dw);
        const char* wf = hw && dw.dli_fname ? strrchr(dw.dli_fname, '/') + 1 : "-";
        printf("  obj+%d=%p vptr=%p<-%s:%s\n", 8 * k, w, *(void**)w, wf,
               hw && dw.dli_sname ? dw.dli_sname : "?");
        if (hw && dw.dli_fname && (strstr(dw.dli_fname, "libbinder") || strstr(dw.dli_sname ? dw.dli_sname : "", "BpBinder"))) {
          g_gstream = (AIBinder*)w;
          printf("  ==> AIBinder 候选在 obj+%d\n", 8 * k);
          break;
        }
      }
      fflush(stdout);
    }
    if (g_gstream) maybe = (void*)g_gstream;       /* 对象里认出来的句柄优先 */
    if (getenv("OBJS")) continue;                  /* OBJS=1：只看布局，一个事务都不发 */
    if (!readable(maybe)) continue;
    if (!Prep2((AIBinder*)maybe, &in3)) {
      N.Parcel_writeInt32(in3, 0);
      int s3 = Tx2((AIBinder*)maybe, 1, &in3, &out3, 0);
      size_t sz3 = out3 ? N.Parcel_dataSize(out3) : 0;
      if (s3 != 0 || sz3 <= 4)
        printf("  getStreamCommon 未成: st=%d reply=%zu\n", s3, sz3);
      if (s3 == 0 && sz3 > 4) {
        printf("STREAM 命中: ret+%d cand=%p vptr=%p binder=%p getStreamCommon st=%d reply=%zu\n",
               off, cand, vptr, maybe, s3, sz3); fflush(stdout);
        stream = (AIBinder*)maybe;
        int32_t e3 = -1;
        N.Parcel_setPos(out3, 0);
        int (*rI)(const AParcel*, int32_t*) = (int(*)(const AParcel*, int32_t*))dlsym(N.ndk, "AParcel_readInt32");
        if (rI) rI(out3, &e3);
        printf("  reply ex=%d\n", e3); fflush(stdout);
        /* UM=1：验"updateMetadata ⇒ HAL 才 start"这条假设 —— 框架自己那条流的日志顺序是
         * startSource → setAggregateSourceMetadataV7 → AHAL_StreamOut_QTI: start。
         * 这里用平台的 BpStreamOut::updateMetadata 发一个**全零** SourceMetadata：
         * 字段值先不管，只看 logcat 里会不会冒出 "start"。 */
        if (getenv("UM")) {
          void* um = dlsym(h, "_ZN4aidl7android8hardware5audio4core11BpStreamOut14"
                             "updateMetadataERKNS2_6common14SourceMetadataE");
          if (!um) printf("  UM: 缺 BpStreamOut::updateMetadata 符号\n");
          else {
            static char sm[512]; memset(sm, 0, sizeof sm);
            static char ust[64]; memset(ust, 0, sizeof ust);
            int (*gst2)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
            call_sret3(um, cand, sm, ust, ust);
            void* us = *(void**)ust;
            printf("  UM: updateMetadata status=%d\n", us && gst2 ? gst2(us) : -999);
            if (getenv("UMI")) {
              int32_t* w = (int32_t*)sm;
              /* 逐个 int32 位置试不同值，找哪个字段被 HAL 读出来（看 logcat 的 metadata 行） */
              for (int k = 0; k < 8; k++) {
                memset(sm, 0, sizeof sm); w[k] = 1; memset(ust, 0, sizeof ust);
                call_sret3(um, cand, sm, ust, ust);
                printf("    UM 试 +%d=1\n", 4 * k);
              }
            }
            fflush(stdout);
            usleep(3000000);
          }
        }
        if (getenv("MMAP")) {
          /* IStreamCommon::createMmapBuffer = 码 8（从 BpStreamCommon 的 AIDL 声明序数出来）。
           * 拿它的 fd 就能 mmap 出一块和 HAL 共享的环形区 —— 不依赖 FMQ 协议就有数据通道。
           * 逆 IStreamCommon::readFromParcel 这条路实测把进程打崩（它按 parcel 头部语义读，
           * 位置对不上就是随机行为）；改用平台自己的 BpStreamOut::getStreamCommon 拿
           * 现成的 std::shared_ptr<IStreamCommon>（libc++ 的 shared_ptr 全零即空，可直接传）。 */
          void (*getSC)(void*, void*) = (void(*)(void*, void*))dlsym(h,
            "_ZN4aidl7android8hardware5audio4core11BpStreamOut15getStreamCommonEPNSt3__110shared_ptrINS3_13IStreamCommonEEE");
          if (!getSC) printf("  MMAP: 缺 BpStreamOut::getStreamCommon 符号\n");
          else {
            static char spbuf[32]; memset(spbuf, 0, sizeof spbuf);
            static char scb[64]; memset(scb, 0, sizeof scb);
            printf("  MMAP: 调平台 getStreamCommon，obj=%p\n", cand); fflush(stdout);
            call_sret3(getSC, cand, spbuf, scb, scb);      /* 按值返回 ScopedAStatus ⇒ 隐藏 x8 */
            void* sst = *(void**)scb;
            int (*gSt)(const void*) = (int(*)(const void*))dlsym(N.ndk, "AStatus_getStatus");
            printf("  MMAP: status=%d shared_ptr 指向=%p\n", sst && gSt ? gSt(sst) : -999,
                   *(void**)spbuf); fflush(stdout);
            void* scpObj = *(void**)spbuf;
            void* scb2 = find_binder_in(scpObj, "BpStreamCommon");
            (void)scb2;
            /* 原始事务在这条句柄上 prepare 不下来的原因不追了：平台自己有
             * BpStreamCommon::createMmapBuffer(MmapBufferDescriptor*)，照 getStreamCommon 的
             * 办法调它 —— 全程由平台打包/解包，我只管看结果。 */
            void* cm = dlsym(h, "_ZN4aidl7android8hardware5audio4core14BpStreamCommon16create"
                             "MmapBufferEPNS3_20MmapBufferDescriptorE");
            if (!cm) printf("  MMAP: 缺 BpStreamCommon::createMmapBuffer 符号\n");
            else {
              static char md[256]; memset(md, 0, sizeof md);
              static char st4[64]; memset(st4, 0, sizeof st4);
              printf("  MMAP: 调 createMmapBuffer，对象=%p\n", scpObj); fflush(stdout);
              call_sret3(cm, scpObj, md, NULL, st4);
              void* sst4 = *(void**)st4;
              printf("  MMAP: createMmapBuffer status=%d\n", sst4 && gSt ? gSt(sst4) : -999);
              for (int k = 0; k < 64; k += 8)
                printf("  md+%d=%p (int32=%d)\n", k, *(void**)(md + k), *(int32_t*)(md + k));
              fflush(stdout);
              for (int k = 0; k + 4 <= (int)sizeof md; k += 4) {
                int v; memcpy(&v, md + k, 4);
                if (v <= 0 || v > 4096) continue;
                struct stat sb;
                if (fstat(v, &sb) == 0)
                  printf("  MMAP_FD@%d = %d  size=%lld mode=%o\n", k, v,
                         (long long)sb.st_size, sb.st_mode);
              }
              fflush(stdout);
            }
            if (0) {
              AParcel* i4 = NULL; AParcel* o4 = NULL;
              int (*rI2)(const AParcel*, int32_t*) =
                (int(*)(const AParcel*, int32_t*))dlsym(N.ndk, "AParcel_readInt32");
              for (int fk = 0; fk < 2; fk++) {           /* 0x10=ACCEPT_FDS，被拒（-22）则退回 0 */
                uint32_t flags = fk ? 0 : 0x10;
                i4 = NULL; o4 = NULL;
                if (Prep2((AIBinder*)scb2, &i4)) break;
                N.Parcel_writeInt32(i4, 0);
                N.Parcel_writeInt32(i4, 2048);           /* minSizeFrames */
                int s4 = Tx2((AIBinder*)scb2, 8, &i4, &o4, flags);
                size_t sz4 = o4 ? N.Parcel_dataSize(o4) : 0;
                int32_t e4 = -1;
                if (o4 && rI2) { N.Parcel_setPos(o4, 0); rI2(o4, &e4); }
                printf("  createMmapBuffer(8) flags=%#x st=%d reply=%zu ex=%d\n", flags, s4, sz4, e4);
                fflush(stdout);
                for (int pos = 4; o4 && pos + 4 <= (int)sz4; pos += 4) {
                  int fd = -1;
                  N.Parcel_setPos(o4, pos);
                  if (rFd && rFd(o4, &fd) == 0 && fd >= 0 && fd < 4096)
                    { printf("  MMAP_PFD@%d = %d\n", pos, fd); fflush(stdout); }
                  int f2 = -1;
                  N.Parcel_setPos(o4, pos);
                  if (rFd2 && rFd2(o4, &f2) == 0 && f2 >= 0 && f2 < 4096)
                    { printf("  MMAP_RAWFD@%d = %d\n", pos, f2); fflush(stdout); }
                }
                if (s4 == 0 && sz4 > 4) break;
              }
            }
          }
        }
        for (int pos = 4; pos + 4 <= (int)sz3; pos += 4) {
          int fd = -1;
          int rr = rFd ? (N.Parcel_setPos(out3, pos), rFd(out3, &fd)) : -1;
          if (rr == 0 && fd >= 0 && fd < 4096) {
            printf("  PFD@%d = %d\n", pos, fd);
            fflush(stdout);
          }
          if (rFd2) { int f2 = -1; N.Parcel_setPos(out3, pos); if (rFd2(out3, &f2) == 0 && f2 >= 0 && f2 < 4096) { printf("  FileDescriptor@%d = %d\n", pos, f2); fflush(stdout); } }
        }
        break;
      }
    }
  }
  printf(stream ? "VERDICT-M2a: 拿到 IStreamOut 句柄（可继续取 FMQ）\n" : "M2a: 未命中 stream 句柄\n");
  fflush(stdout);
  return stream ? 0 : 9;
}

