// hudos-run loader (aarch64 freestanding) — 在 qemu-user guest 内加载 hudos ELF
// 用法: loader <guest.elf> <gw_vaddr_hex> <main_vaddr_hex> [entry_code_blob_path]
// 布局: GB=0x20000000 (guest 段), 0x20020000 (分发器代码页 RWX), 0x21000000 (guest 栈)
typedef unsigned long long u64;
typedef unsigned short u16;
typedef unsigned int u32;
typedef long long i64;
static const u64 GB = 0x20000000u;
static const u64 CODE_AREA = 0x20080000u;   // 分发器 blob 固定加载地址
static const u64 FB_BASE = 0x21100000u;     // 帧缓冲 guest 页(W*H*4, 默认 3MB)
static const u64 GSTACK = 0x21000000u;

extern long sys6(long n, long a, long b, long c, long d, long e);
extern long sys7(long n, long a, long b, long c, long d, long e, long f);
static void mcpy(void *d, const void *s, u64 n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
}
static u64 htoi(const char *s) {
    u64 v = 0;
    while (*s) {
        v <<= 4;
        char c = *s++;
        v |= (c >= '0' && c <= '9') ? c - '0' : ((c | 0x20) - 'a' + 10);
    }
    return v;
}
// ---- ELF 解析 ----
struct ph { u32 type, flags; u64 off, va, fsz, msz; };
static int load_guest(u64 fd, u64 gw, struct ph *out, int maxph) {
    unsigned char hdr[64];
    sys6(67, fd, (u64)hdr, 64, 0, 0);              // pread 头部
    u64 e_phoff = *(u64 *)(hdr + 32);
    u64 esz = *(u16 *)(hdr + 54);
    u64 ephnum = *(u16 *)(hdr + 56);
    if (ephnum > (u64)maxph) ephnum = maxph;
    unsigned char phb[512];
    sys6(67, fd, (u64)phb, esz * ephnum, e_phoff, 0);
    int n = 0;
    for (u64 i = 0; i < ephnum; i++) {
        unsigned char *p = phb + i * esz;
        u32 t = *(u32 *)p;
        if (t != 1) continue;                       // 只处理 LOAD
        struct ph *o = &out[n++];
        o->type = t;
        o->flags = *(u32 *)(p + 4);
        o->off = *(u64 *)(p + 8);
        o->va = *(u64 *)(p + 16);
        o->fsz = *(u64 *)(p + 32);
        o->msz = *(u64 *)(p + 40);
    }
    return n;
}
static void map_seg_rw(u64 fd, const struct ph *s) {
    // mmap/mprotect 需页对齐: 基址向下取整, 长度按页上取整
    u64 base = (GB + s->va) & ~4095ull;
    u64 end = (GB + s->va + s->msz + 4095) & ~4095ull;
    sys7(222, base, end - base, 7, 0x32 /*PRIVATE|ANON|FIXED*/, (u64)-1, 0);
    if (s->fsz) sys6(67, fd, GB + s->va, s->fsz, s->off, 0);   // pread 文件内容
}
static void prot_seg(const struct ph *s) {
    // ELF PF_X=1/PF_W=2/PF_R=4  ->  PROT_EXEC=4/PROT_WRITE=2/PROT_READ=1
    u64 base = (GB + s->va) & ~4095ull;
    u64 end = (GB + s->va + s->msz + 4095) & ~4095ull;
    u64 prot = ((s->flags & 1) ? 4 : 0) | ((s->flags & 2) ? 2 : 0) | ((s->flags & 4) ? 1 : 0);
    sys6(226, base, end - base, prot, 0, 0);
}
static void dbg(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    sys6(64, 2, (u64)s, n, 0, 0);
}
static void prnum(u64 v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) { dbg("0"); return; }
    while (v && i > 0) { b[--i] = '0' + (v % 10); v /= 10; }
    dbg(b + i);
}
int main_c(int argc, const char **argv) {
    dbg("[loader] start argc=");
    prnum((u64)argc);
    dbg("\n[loader] argv[1]='");
    if (argc > 1) dbg(argv[1]);
    dbg("'\n");
    if (argc < 7) { dbg("[loader] 用法: loader <elf> <sys> <main> <disp> <W> <H> [args...]\n"); return 2; }
    dbg("[loader] open guest\n");
    // aarch64 无 open(2): 56 = openat(dirfd=-100=AT_FDCWD, path, flags, mode)
    u64 fd = sys6(56, (u64)-100, (u64)argv[1], 0, 0, 0);
    if ((i64)fd < 0) { dbg("[loader] open FAIL errno="); prnum(0 - fd); dbg("\n"); return 3; }
    dbg("[loader] open ok, loading\n");
    u64 gw = htoi(argv[2]);
    u64 mainva = htoi(argv[3]);
    u64 sc_w = htoi(argv[5]);          // 屏幕宽
    u64 sc_h = htoi(argv[6]);          // 屏幕高
    if (!sc_w || !sc_h) { sc_w = 1024; sc_h = 768; }
    struct ph phs[8];
    int n = load_guest(fd, GB, phs, 8);
    for (int i = 0; i < n; i++) map_seg_rw(fd, &phs[i]);   // 先全部 RW(可写补丁)
    sys6(57, fd, 0, 0, 0, 0);                          // close
    // 分发器代码页 (RWX) — 从 argv[4] 文件读入
    u64 cfd = sys6(56, (u64)-100, (u64)argv[4], 0, 0, 0);
    u64 blob = 0;
    if ((i64)cfd >= 0) {
        unsigned char tmp[4096];
        u64 r;
        u64 page = CODE_AREA;
        sys7(222, page, 8192, 7, 0x32, (u64)-1, 0);
        while ((r = sys6(63, cfd, (u64)tmp, 4096, 0, 0)) > 0) {
            mcpy((void *)page, tmp, r);
            page += r;
            if (page - CODE_AREA > 8192) break;
        }
        blob = CODE_AREA;
        sys6(57, cfd, 0, 0, 0, 0);
    }
    if (!blob) return 4;
    // 帧缓冲页(RW, W*H*4): 写入分发器头部(W,H,FB_BASE) —— 头部在 blob 起点
    u64 fblen = sc_w * sc_h * 4;
    sys7(222, FB_BASE, fblen, 3 /*RW*/, 0x32, (u64)-1, 0);
    *(u32 *)(CODE_AREA + 0) = (u32)sc_w;
    *(u32 *)(CODE_AREA + 4) = (u32)sc_h;
    *(u64 *)(CODE_AREA + 8) = FB_BASE;
    // 网关补丁(仍在 RW 页内): gw 处写 b。入口=blob+0x10(头部16B后的 _dispatch)
    // AArch64: b 目标 = 本指令地址(PC) + disp —— 差量 = 入口 - (GB+gw)，无 +4
    u64 entry = blob + 0x10;
    u64 disp = entry - (GB + gw);
    u32 ins = 0x14000000u | ((u32)((disp >> 2) & 0x3FFFFFF));
    *(u32 *)(GB + gw) = ins;
    dbg("[loader] patch target ok\n");
    // 统一按段 flags 设最终权限
    for (int i = 0; i < n; i++) prot_seg(&phs[i]);
    dbg("[loader] loaded, jumping main\n");
    // guest 栈(必须先映射): 0x21000000 起 32KB RW
    sys7(222, GSTACK, 0x8000, 3 /*RW*/, 0x32, (u64)-1, 0);
    // guest argv: argv[0]=guest 路径(argv[1]), 其后转发 loader 的 argv[7..]
    u64 gargc = 1 + ((argc > 7) ? (argc - 7) : 0);
    u64 sp = (GSTACK + 0x4000) & ~15ull;
    sp -= 8 * (gargc + 1);
    u64 *av = (u64 *)sp;
    av[0] = (u64)argv[1];
    for (int i = 0; i < (int)gargc - 1; i++) av[1 + i] = (u64)argv[7 + i];
    av[gargc] = 0;
    // 直接调 guest main(argc, argv) —— main 返回后 exit
    u64 (*gmain)(u64, u64) = (u64(*)(u64, u64))(GB + mainva);
    u64 rc = gmain(gargc, (u64)av);
    sys6(93, rc, 0, 0, 0, 0);
    return 0; // 不可达
}
