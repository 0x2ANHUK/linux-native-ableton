// libavx512emu.so: process-wide part. Catches SIGILL from AVX-512 / opmask
// instructions, translates the function around them, patches the code and
// resumes. Keeps its SIGILL/SIGTRAP handlers in place when the program
// installs its own (those are chained for anything that is not ours).
#include "rt.h"
#include "xlate.h"
#include "interp.h"
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sched.h>
#include <ucontext.h>
#include <unistd.h>
#include <time.h>
#include <linux/membarrier.h>
#include <asm/prctl.h>

namespace emu {

Stats stats;
bool no_inline;
static bool wrap_signals = true;
int log_level = 1;
static int log_fd = 2;

#ifdef EMU_DIFFTEST
bool diff_quiet;
#endif
void elog(const char* fmt, ...) {
#ifdef EMU_DIFFTEST
    if (diff_quiet) return;   // test children: no syscalls of our own
#endif
    if (log_level <= 0) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    ssize_t w = write(log_fd, buf, n); (void)w;
}

// ---- lock (signal handlers of several threads)
static volatile int glock;
static void lock() { while (__atomic_exchange_n(&glock, 1, __ATOMIC_ACQUIRE)) { while (glock) __builtin_ia32_pause(); } }
static void unlock() { __atomic_store_n(&glock, 0, __ATOMIC_RELEASE); }
// Asynchronous signals stay blocked while this thread holds the lock: a handler running untranslated
// AVX-512 code would raise SIGILL on the same thread and spin on the lock it holds (deadlock; found by
// mech/sig: timer signals whose handler uses AVX-512). Synchronous signals (faults) stay deliverable.
struct SigBlock {
    sigset_t old;
    SigBlock() {
        sigset_t all; sigfillset(&all);
        sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS); sigdelset(&all, SIGFPE); sigdelset(&all, SIGILL); sigdelset(&all, SIGTRAP);
        sigprocmask(SIG_BLOCK, &all, &old);
    }
    ~SigBlock() { sigprocmask(SIG_SETMASK, &old, nullptr); }
};
// fork() copies only the calling thread: a lock another thread held at that
// moment stays taken in the child, whose first translation then spins
// forever (Live forks a worker while it still translates at start-up).
// The lock is taken around fork, so the code cache and tables are also
// consistent in the child.
static sigset_t fork_old;
static void fork_prepare() {   // as SigBlock, kept across fork() and restored in parent and child
    sigset_t all; sigfillset(&all); sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS); sigdelset(&all, SIGFPE); sigdelset(&all, SIGILL); sigdelset(&all, SIGTRAP);
    sigprocmask(SIG_BLOCK, &all, &fork_old); lock();
}
static void fork_parent() { unlock(); sigprocmask(SIG_SETMASK, &fork_old, nullptr); }
static void fork_child() { __atomic_store_n(&glock, 0, __ATOMIC_RELEASE); sigprocmask(SIG_SETMASK, &fork_old, nullptr); }

// ---- scratch arena
static uint8_t* scratch_base;
static size_t scratch_used, scratch_cap = 512u << 20;
void* scratch_alloc(size_t n) {
    if (!scratch_base) {
        scratch_base = (uint8_t*)mmap(nullptr, scratch_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (scratch_base == MAP_FAILED) { scratch_base = nullptr; return nullptr; }
        madvise(scratch_base, 8u << 20, 23 /* MADV_POPULATE_WRITE */);   // no page faults in typical translations
    }
    n = (n + 63) & ~size_t(63);
    if (scratch_used + n > scratch_cap) return nullptr;
    void* p = scratch_base + scratch_used; scratch_used += n; return p;
}
void scratch_reset() {
    if (scratch_base && scratch_used > (64u << 20)) madvise(scratch_base, scratch_used, MADV_DONTNEED);
    scratch_used = 0;
}

// ---- code cache: chunks near the code they serve
struct Chunk { uint8_t* base; size_t used, cap, populated; };
// keep 4 MB of the chunk ahead of its use faulted in (translation then touches no fresh pages)
static void populate_ahead(Chunk& c) {
    const size_t step = 4u << 20;
    while (c.populated < c.cap && c.populated < c.used + step) {
        size_t n = c.cap - c.populated < step ? c.cap - c.populated : step;
        madvise(c.base + c.populated, n, 23 /* MADV_POPULATE_WRITE */);
        c.populated += n;
    }
}
static Chunk chunks[256];
static int nchunks;
static bool near_enough(uint64_t a, uint64_t b, uint64_t span) {
    uint64_t lo = a < b ? a : b, hi = a < b ? b : a;
    return hi + span - lo < (1ull << 31) - (1ull << 20);
}
static int last_chunk = -1;
// Gives back the end of the most recent allocation: [p, end) was not used.
void cache_trim(uint8_t* p) {
    if (last_chunk < 0) return;
    Chunk& c = chunks[last_chunk];
    if (p >= c.base && p <= c.base + c.used) c.used = size_t(p - c.base);
}
uint8_t* cache_alloc(uint64_t nearp, size_t n, size_t align) {
    for (int i = 0; i < nchunks; i++) {
        Chunk& c = chunks[i];
        size_t off = (c.used + align - 1) & ~(align - 1);
        if (off + n <= c.cap && near_enough((uint64_t)c.base, nearp, c.cap)) { c.used = off + n; last_chunk = i; populate_ahead(c); return c.base + off; }
    }
    if (nchunks == 256) return nullptr;
    size_t cap = 32u << 20;
    if (n > cap) cap = (n + 0xfffff) & ~size_t(0xfffff);
    // probe for free space around nearp, nearest first
    uint64_t base = nearp & ~0xffffull;
    for (uint64_t d = 1ull << 24; d < (1ull << 31) - cap - (64u << 20); d += 1ull << 24) {
        for (int s = 0; s < 2; s++) {
            uint64_t a = s ? base - d - cap : base + d;
            if (s && base < d + cap) continue;
            void* p = mmap((void*)a, cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if ((uint64_t)p != a) { munmap(p, cap); continue; }
            Chunk& c = chunks[nchunks++];
            c.base = (uint8_t*)p; c.cap = cap; c.used = 0; c.populated = 0; last_chunk = nchunks - 1;
            size_t off = 0; c.used = off + n;
            populate_ahead(c);
            return c.base;
        }
    }
    elog("avx512emu: no code cache space near %#lx\n", (unsigned long)nearp);
    return nullptr;
}

void cache_add_region(uint8_t* base, size_t cap, size_t used) {
    if (nchunks == 256) return;
    Chunk& c = chunks[nchunks++]; c.base = base; c.cap = cap; c.used = used; c.populated = cap; last_chunk = nchunks - 1;
}
static bool in_cache(uint64_t pc) {
    for (int i = 0; i < nchunks; i++) if (pc >= (uint64_t)chunks[i].base && pc < (uint64_t)chunks[i].base + chunks[i].cap) return true;
    return false;
}

// ---- address map (open addressing, never shrinks)
struct Ent { uint64_t k, v; };
static Ent* map_tab;
static uint64_t map_cap = 1ull << 22, map_count;
// Keeps nearby addresses on nearby slots (few pages touched per function); high bits only shift the window.
static uint64_t hsh(uint64_t k) { return k + (k >> 22) * 0x9E3779B1ull; }
static void map_init() {
    if (map_tab) return;
    map_tab = (Ent*)mmap(nullptr, map_cap * sizeof(Ent), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}
void map_put(uint64_t k, uint64_t v) {
    map_init();
    if (map_count * 2 >= map_cap) { elog("avx512emu: address map full\n"); return; }
    for (uint64_t i = hsh(k) & (map_cap - 1);; i = (i + 1) & (map_cap - 1)) {
        if (map_tab[i].k == k) { __atomic_store_n(&map_tab[i].v, v, __ATOMIC_RELEASE); return; }
        if (map_tab[i].k == 0) { map_tab[i].v = v; __atomic_store_n(&map_tab[i].k, k, __ATOMIC_RELEASE); map_count++; return; }
    }
}
void map_foreach(void (*fn)(uint64_t, uint64_t, void*), void* ctx) {
    if (map_tab) for (uint64_t i = 0; i < map_cap; i++) if (map_tab[i].k) fn(map_tab[i].k, map_tab[i].v, ctx);
}
// offline segments: their address map stays in the segment (sorted u32 pairs, image-relative), searched on a miss
struct XlMap { uint64_t base, lo, hi; const uint32_t* pairs; uint32_t n; };
static XlMap xl_maps[8]; static int n_xl_maps;
void map_add_sorted(uint64_t base, uint64_t lo, uint64_t hi, const uint32_t* pairs, uint32_t n) {
    if (n_xl_maps < 8) { xl_maps[n_xl_maps] = XlMap{base, lo, hi, pairs, n}; __atomic_store_n(&n_xl_maps, n_xl_maps + 1, __ATOMIC_RELEASE); }
}
static uint64_t xl_map_get(uint64_t k) {
    for (int m = 0; m < n_xl_maps; m++) {
        const XlMap& x = xl_maps[m];
        if (k < x.lo || k >= x.hi) continue;
        uint32_t key = uint32_t(k - x.base); uint32_t lo = 0, hi = x.n;
        while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (x.pairs[2 * mid] < key) lo = mid + 1; else hi = mid; }
        if (lo < x.n && x.pairs[2 * lo] == key) return x.base + x.pairs[2 * lo + 1];
    }
    return 0;
}
uint64_t map_get(uint64_t k) {
    if (!map_tab) return n_xl_maps ? xl_map_get(k) : 0;
    for (uint64_t i = hsh(k) & (map_cap - 1);; i = (i + 1) & (map_cap - 1)) {
        uint64_t kk = __atomic_load_n(&map_tab[i].k, __ATOMIC_ACQUIRE);
        if (kk == k) return __atomic_load_n(&map_tab[i].v, __ATOMIC_ACQUIRE);
        if (kk == 0) return n_xl_maps ? xl_map_get(k) : 0;
    }
}

// ---- code patching
static int mem_fd = -1;
bool poke(uint64_t addr, const void* src, size_t n) {
    if (mem_fd < 0) mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (mem_fd >= 0 && pwrite(mem_fd, src, n, (off_t)addr) == (ssize_t)n) return true;
    // fallback: make the page writable for a moment
    uint64_t pg = addr & ~0xfffull, pe = (addr + n + 0xfff) & ~0xfffull;
    if (mprotect((void*)pg, pe - pg, PROT_READ | PROT_WRITE | PROT_EXEC)) { elog("avx512emu: cannot patch %#lx (%d)\n", (unsigned long)addr, errno); return false; }
    memcpy((void*)addr, src, n);
    mprotect((void*)pg, pe - pg, PROT_READ | PROT_EXEC);
    return true;
}
static bool membarrier_ok;
void sync_cores() {
    if (membarrier_ok) syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0);
    else __sync_synchronize();
}

// ---- sampling profiler
struct PGroup { uint64_t stub; uint32_t len, n; uint64_t orig; uint64_t hits; };
static PGroup* pgroups; static uint32_t npgroups;
static bool prof_on;
static uint64_t ps_total, ps_cache, ps_lib, ps_other;
static uint64_t lib_lo, lib_hi;
// AVX512EMU_PERFMAP=1: every group in /tmp/perf-<pid>.map, so perf names the translated code
// ("xl:<object>+<offset of the original code>").
static int perfmap_fd = -2;
void perfmap_group(uint64_t code, uint32_t len, uint64_t orig) {
    if (perfmap_fd == -2) { const char* e = getenv("AVX512EMU_PERFMAP"); perfmap_fd = -1;
        if (e && (*e == '1' || *e == '2')) { char path[64]; snprintf(path, sizeof path, "/tmp/perf-%d.map", getpid()); perfmap_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); } }
    if (perfmap_fd < 0) return;
    ObjInfo o; bool f = find_object(orig, o);
    const char* nm = f ? (o.name[0] ? (strrchr(o.name, '/') ? strrchr(o.name, '/') + 1 : o.name) : "main") : "?";
    char line[160]; int k = snprintf(line, sizeof line, "%lx %x xl:%s+%#lx\n", (unsigned long)code, len, nm, (unsigned long)(orig - (f ? o.base : 0)));
    if (k > 0 && write(perfmap_fd, line, k) < 0) {}
    // AVX512EMU_PERFMAP=2: also the code itself (/tmp/xlcode-<pid>.bin, index .idx: addr len offset orig)
    static int bin_fd = -2, idx_fd = -2; static uint64_t bin_off;
    if (bin_fd == -2) { const char* e = getenv("AVX512EMU_PERFMAP"); bin_fd = idx_fd = -1;
        if (e && *e == '2') { char path[64];
            snprintf(path, sizeof path, "/tmp/xlcode-%d.bin", getpid()); bin_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            snprintf(path, sizeof path, "/tmp/xlcode-%d.idx", getpid()); idx_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); } }
    if (bin_fd >= 0 && idx_fd >= 0) {
        if (write(bin_fd, (const void*)code, len) < 0) {}
        k = snprintf(line, sizeof line, "%lx %x %lx %lx\n", (unsigned long)code, len, (unsigned long)bin_off, (unsigned long)(orig - (f ? o.base : 0)));
        if (k > 0 && write(idx_fd, line, k) < 0) {}
        bin_off += len;
    }
}
void prof_register_group(uint64_t stub, uint32_t len, uint64_t orig, uint32_t n) {
    if (!prof_on) return;
    if (!pgroups) pgroups = (PGroup*)mmap(nullptr, sizeof(PGroup) << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (npgroups < (1u << 20)) { PGroup& g = pgroups[npgroups]; g.stub = stub; g.len = len; g.orig = orig; g.n = n; g.hits = 0; __atomic_store_n(&npgroups, npgroups + 1, __ATOMIC_RELEASE); }
}
static void on_prof(int, siginfo_t*, void* ucv) {
    uint64_t pc = ((ucontext_t*)ucv)->uc_mcontext.gregs[REG_RIP];
    __atomic_fetch_add(&ps_total, 1, __ATOMIC_RELAXED);
    if (pc >= lib_lo && pc < lib_hi) { __atomic_fetch_add(&ps_lib, 1, __ATOMIC_RELAXED); return; }
    uint32_t n = __atomic_load_n(&npgroups, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) if (pc >= pgroups[i].stub && pc < pgroups[i].stub + pgroups[i].len) {
        __atomic_fetch_add(&pgroups[i].hits, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&ps_cache, 1, __ATOMIC_RELAXED); return; }
    __atomic_fetch_add(&ps_other, 1, __ATOMIC_RELAXED);
}
void prof_report() {
    if (!prof_on || !ps_total) return;
    elog("avx512emu[%d]: samples %lu: translated %.1f%% emulator-lib %.1f%% other %.1f%%\n", getpid(), (unsigned long)ps_total,
         100.0 * ps_cache / ps_total, 100.0 * ps_lib / ps_total, 100.0 * ps_other / ps_total);
    PGroup* top[25] = {};
    for (uint32_t i = 0; i < npgroups; i++) {
        PGroup* g = &pgroups[i]; if (!g->hits) continue;
        int j = 24; if (top[j] && top[j]->hits >= g->hits) continue;
        while (j > 0 && (!top[j - 1] || top[j - 1]->hits < g->hits)) { top[j] = top[j - 1]; j--; }
        top[j] = g;
    }
    for (int j = 0; j < 25 && top[j]; j++) {
        ObjInfo o; bool f = find_object(top[j]->orig, o);
        elog("  %6lu main+%#lx stub %u bytes, %u specials\n", (unsigned long)top[j]->hits, (unsigned long)(top[j]->orig - (f ? o.base : 0)), top[j]->len, top[j]->n);
    }
}

// ---- signal handlers of the program: a handler running translated code on
// the same thread would overwrite the emulator state of the code it
// interrupted (zmm16-31, upper halves, k registers, spill slots, the
// interpreter's register file). The program's handlers are wrapped so that
// state is saved before and restored after them.
static const size_t TSAVE = offsetof(TState, c_bitd);
struct SigSave { uint8_t t[TSAVE]; Z v[32]; };
static __thread SigSave sig_save[3] __attribute__((tls_model("initial-exec")));
static __thread int sig_depth __attribute__((tls_model("initial-exec")));
static struct sigaction app_act[65];
static void wrap_handler(int sig, siginfo_t* si, void* uc) {
    int d = sig_depth++;
    if (d < 3) { memcpy(sig_save[d].t, &tst, TSAVE); memcpy(sig_save[d].v, interp_V, sizeof sig_save[d].v); }
    const struct sigaction& a = app_act[sig];
    if (a.sa_flags & SA_SIGINFO) a.sa_sigaction(sig, si, uc);
    else if (a.sa_handler != SIG_IGN && a.sa_handler != SIG_DFL) a.sa_handler(sig);
    if (d < 3) { memcpy(&tst, sig_save[d].t, TSAVE); memcpy(interp_V, sig_save[d].v, sizeof sig_save[d].v); }
    sig_depth = d;
}

// ---- signals
typedef int (*sigaction_fn)(int, const struct sigaction*, struct sigaction*);
static sigaction_fn real_sigaction;
static struct sigaction app_ill, app_trap, app_segv, app_bus, app_fpe; // what the program asked for
static bool installed;

static void chain(int sig, siginfo_t* si, void* uc, const struct sigaction& app) {
    if ((app.sa_flags & SA_SIGINFO) && app.sa_sigaction) { app.sa_sigaction(sig, si, uc); return; }
    if (app.sa_handler == SIG_IGN) return;
    if (app.sa_handler && app.sa_handler != SIG_DFL) { app.sa_handler(sig); return; }
    // default: restore and re-raise (returning re-executes the faulting instruction)
    struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL;
    real_sigaction(sig, &d, nullptr);
}

// ---- AVX512EMU_CAPTURE=DIR: group-entry state records for offline replay (format: see gharness.cpp, -r)
// record: "AXC1" u32 len | u64 obj_base, pc, end | u64 gpr[16] (rax rcx rdx rbx rsp rbp rsi rdi r8..r15) | u64 rflags |
//         u32 mxcsr, nwin | u8 z[32][64] | u64 k[8] | windows: u64 addr, u32 len, u32 0, bytes (padded to 8)
static int cap_fd = -1; static volatile int cap_lock_v; static uint64_t cap_records, cap_bytes, cap_max = ~0ull;
static int64_t cap_next(uint32_t n) { static const int64_t next[] = {1, 14, 240, 3840}; return n <= 4 ? next[n - 1] : (int64_t(1) << 62); }
struct CapWin { uint64_t lo, hi; };
static int cap_add(CapWin* w, int n, int cap, uint64_t lo, uint64_t hi) {
    if (lo > hi || hi - lo > 65536) return n;
    for (int i = 0; i < n; i++) if (lo <= w[i].hi && hi >= w[i].lo) { if (lo < w[i].lo) w[i].lo = lo; if (hi > w[i].hi) w[i].hi = hi; return n; }
    if (n < cap) { w[n].lo = lo; w[n].hi = hi; n++; }
    return n;
}
// copy [lo, hi) of this process into buf; unreadable pages are skipped: returns the readable sub-windows
static int cap_read(uint64_t lo, uint64_t hi, uint8_t* buf, CapWin* out, int cap) {
    int n = 0; uint64_t a = lo;
    while (a < hi && n < cap) {
        uint64_t e = (a | 0xfff) + 1; if (e > hi) e = hi;
        struct iovec l = {buf + (a - lo), size_t(e - a)}, r = {(void*)a, size_t(e - a)};
        bool ok = syscall(SYS_process_vm_readv, getpid(), &l, 1, &r, 1, 0) == (long)(e - a);
        if (ok) { if (n && out[n - 1].hi == a) out[n - 1].hi = e; else { out[n].lo = a; out[n].hi = e; n++; } }
        a = e;
    }
    return n;
}
void capture_group(Frame* f, const Desc* d) {
    CapInfo* ci = (CapInfo*)d;
    if (cap_fd == -2) { ci->countdown = int64_t(1) << 62; return; }
    ci->ncap++; ci->countdown = cap_next(ci->ncap);
    SigBlock blocked;
    while (__atomic_exchange_n(&cap_lock_v, 1, __ATOMIC_ACQUIRE)) {}
    if (cap_fd == -1) {
        const char* dir = getenv("AVX512EMU_CAPTURE"); char path[600]; char exe[64] = "?";
        { int fd = open("/proc/self/comm", O_RDONLY); if (fd >= 0) { long r = read(fd, exe, sizeof exe - 1); if (r > 0) exe[r - (exe[r - 1] == '\n')] = 0; close(fd); } }
        snprintf(path, sizeof path, "%s/cap-%s-%d.bin", dir && *dir ? dir : "/tmp", exe, getpid());
        cap_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        const char* m = getenv("AVX512EMU_CAPTURE_MB"); cap_max = (m ? strtoul(m, 0, 10) : 2048) << 20; /* strtoul: no glibc 2.38 symbol */
        if (cap_fd < 0) cap_fd = -2;
    }
    ObjInfo o;
    if (cap_fd >= 0 && cap_bytes < cap_max && find_object(ci->start, o)) {
        TState& s = tst;
        uint64_t g[16] = {f->rax, f->rcx, f->rdx, f->rbx, uint64_t(f) + 264, f->rbp, f->rsi, f->rdi, f->r8, f->r9, f->r10, f->r11, f->r12, f->r13, f->r14, f->r15};
        static uint8_t z[32][64];
        for (int r = 0; r < 16; r++) { memcpy(z[r], s.hw[r], 32); memcpy(z[r] + 32, s.zhi[r], 32); }
        for (int r = 0; r < 16; r++) memcpy(z[16 + r], s.zx[r], 64);
        CapWin w[96]; int nw = 0;
        nw = cap_add(w, nw, 96, g[4] - 1024, g[4] + 512);                       // stack incl. red zone
        for (uint32_t i = 0; i < ci->nops; i++) {
            const CapOp& op = ci->ops[i]; int kind = op.kind & 15;
            if (kind == 3) { nw = cap_add(w, nw, 96, op.abs - 128, op.abs + 192); continue; }
            uint64_t b = op.base >= 0 ? g[op.base & 15] : 0;
            if (kind == 0) {
                uint64_t a = b + (op.index >= 0 ? g[op.index & 15] << op.scale : 0) + int64_t(op.disp);
                nw = cap_add(w, nw, 96, a - 128, a + 192);
            } else {                                                              // VSIB: every lane
                int es = kind == 2 ? 8 : 4;
                for (int l = 0; l < 64 / es; l++) {
                    int64_t ix = es == 8 ? ((int64_t*)z[op.index & 31])[l] : ((int32_t*)z[op.index & 31])[l];
                    uint64_t a = b + (uint64_t(ix) << op.scale) + int64_t(op.disp);
                    nw = cap_add(w, nw, 96, a - 16, a + 16);
                }
            }
            if (op.kind & 16) {                                                   // base written earlier in the group
                if (op.base >= 0) nw = cap_add(w, nw, 96, b - 256, b + 768);
                if (kind == 0 && op.index >= 0) nw = cap_add(w, nw, 96, g[op.index & 15] - 256, g[op.index & 15] + 768);
            }
        }
        static uint8_t buf[1 << 20]; static CapWin rw[512]; static uint8_t page[65536 + 4096];
        size_t len = 0; int nrw = 0;
        struct { uint32_t magic, len; uint64_t base, pc, end, gpr[16], rflags; uint32_t mxcsr, nwin; } h;
        len = sizeof h + sizeof z + 64;
        for (int i = 0; i < nw && nrw < 500; i++) {
            CapWin sub[32]; int ns = cap_read(w[i].lo, w[i].hi, page, sub, 32);
            for (int j = 0; j < ns && nrw < 512; j++) {
                size_t l = sub[j].hi - sub[j].lo, need = 16 + ((l + 7) & ~size_t(7));
                if (len + need > sizeof buf) break;
                uint64_t a = sub[j].lo; uint32_t l32 = uint32_t(l), zero = 0;
                memcpy(buf + len, &a, 8); memcpy(buf + len + 8, &l32, 4); memcpy(buf + len + 12, &zero, 4);
                memcpy(buf + len + 16, page + (sub[j].lo - w[i].lo), l); memset(buf + len + 16 + l, 0, need - 16 - l);
                len += need; nrw++;
            }
        }
        h.magic = 0x31435841; h.len = uint32_t(len); h.base = o.base; h.pc = ci->start; h.end = ci->end;
        memcpy(h.gpr, g, sizeof g); h.rflags = f->rflags; h.mxcsr = __builtin_ia32_stmxcsr(); h.nwin = uint32_t(nrw);
        memcpy(buf, &h, sizeof h); memcpy(buf + sizeof h, z, sizeof z); memcpy(buf + sizeof h + sizeof z, s.k, 64);
        if (write(cap_fd, buf, len) == (long)len) { cap_records++; cap_bytes += len; }
    }
    __atomic_store_n(&cap_lock_v, 0, __ATOMIC_RELEASE);
}

// AVX512EMU_SLOWUS: log translations (and lock waits) longer than this (default 2000 us; log level 2)
static uint64_t slow_ns() { static int64_t v = -1; if (v < 0) { const char* e = getenv("AVX512EMU_SLOWUS"); v = (e ? strtol(e, nullptr, 10) : 2000) * 1000; /* strtol: 64-bit, no glibc 2.38 symbol */ } return uint64_t(v); }
static void on_sigill(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv;
    uint64_t pc = uc->uc_mcontext.gregs[REG_RIP];
    int saved_errno = errno;
    stats.sigill++;
    uint64_t to = map_get(pc);
    if (!to) {
        x86::Insn in;
        if (x86::decode((const uint8_t*)pc, 15, in) && is_special(in)) {
            SigBlock blocked;
            struct timespec tw, t0, t1; clock_gettime(CLOCK_MONOTONIC, &tw);
            lock();
            clock_gettime(CLOCK_MONOTONIC, &t0);
            to = translate_at(pc);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            uint64_t ns = uint64_t(t1.tv_sec - t0.tv_sec) * 1000000000ull + uint64_t(t1.tv_nsec - t0.tv_nsec);
            uint64_t wns = uint64_t(t0.tv_sec - tw.tv_sec) * 1000000000ull + uint64_t(t0.tv_nsec - tw.tv_nsec);
            stats.xlate_ns += ns; if (ns > stats.xlate_max_ns) stats.xlate_max_ns = ns;
            if (wns > stats.wait_max_ns) stats.wait_max_ns = wns;
            // audio thread? (realtime policy, or Live's audio thread names when rtkit gave no RT priority)
            char tname[16] = {0}; prctl(PR_GET_NAME, tname, 0, 0, 0);
            int pol = sched_getscheduler(0);
            bool rt = pol == SCHED_FIFO || pol == SCHED_RR || !strncmp(tname, "alsa-audio", 10) || !strncmp(tname, "AudioCalc", 9);
            if (rt) { stats.rt_n++; stats.rt_ns += ns; if (ns > stats.rt_max_ns) stats.rt_max_ns = ns; }
            if (log_level >= 2 && (ns > slow_ns() || (rt && ns > 200000) || wns > slow_ns())) {
                ObjInfo o; bool f = find_object(pc, o); struct timespec wall; clock_gettime(CLOCK_REALTIME, &wall);
                elog("avx512emu[%d]: %s translation %.2f ms (lock wait %.2f ms) at %s+%#lx thread %s/%ld %s t=%ld.%06ld (groups so far %lu, chunks %d)\n",
                     getpid(), rt ? "RT" : "slow", ns / 1e6, wns / 1e6, f ? (o.name[0] ? o.name : "main") : "?", f ? (unsigned long)(pc - o.base) : 0ul,
                     tname, (long)syscall(SYS_gettid), pol == SCHED_FIFO ? "fifo" : pol == SCHED_RR ? "rr" : "other",
                     (long)wall.tv_sec, (long)(wall.tv_nsec / 1000), (unsigned long)stats.groups, nchunks); }
            unlock();
        }
    }
    errno = saved_errno;
    if (to) { uc->uc_mcontext.gregs[REG_RIP] = (greg_t)to; return; }
    {
        const uint8_t* b = (const uint8_t*)pc; ObjInfo o; bool f = find_object(pc, o);
        elog("avx512emu: SIGILL at %#lx (%s+%#lx) not handled: %02x %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long)pc,
             f ? o.name : "?", f ? (unsigned long)(pc - o.base) : 0ul, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    chain(sig, si, ucv, app_ill);
}

#ifdef EMU_DIFFTEST
bool pretranslate_function(uint64_t fstart);
bool diff_trap(uint64_t pc, void* uc);   // difftest.cpp (dev builds): seed traps, and faults in test children
// translation of one function under the lock (test children are single-threaded)
bool diff_translate(uint64_t f) { lock(); bool ok = pretranslate_function(f); unlock(); return ok; }
// memory owned by the runtime (pairs lo, hi)
int diff_rt_ranges(uint64_t* r, int max) {
    int k = 0;
    auto add = [&](uint64_t lo, uint64_t hi) { if (lo && k + 2 <= 2 * max) { r[k++] = lo; r[k++] = hi; } };
    add((uint64_t)scratch_base, (uint64_t)scratch_base + scratch_cap);
    add((uint64_t)map_tab, (uint64_t)map_tab + map_cap * sizeof(Ent));
    add((uint64_t)pgroups, (uint64_t)pgroups + (sizeof(PGroup) << 20));
    for (int i = 0; i < nchunks; i++) add((uint64_t)chunks[i].base, (uint64_t)chunks[i].base + chunks[i].cap);
    return k / 2;
}
sigaction_fn diff_real_sigaction() { return real_sigaction; }
// a forked child: /proc/self/mem opened by the parent writes the parent's memory
void diff_child_reset() { if (mem_fd >= 0) close(mem_fd); mem_fd = -1; }
int diff_mem_fd() { if (mem_fd < 0) mem_fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC); return mem_fd; }
#endif
static unsigned long trapjmp_n;   // jmps written at trapping mid-group entries
static bool trapjmp_on() { static int v = -1; if (v < 0) { const char* e = getenv("AVX512EMU_TRAPJMP"); v = !(e && *e == '0'); } return v; }
static void on_sigtrap(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv;
    uint64_t pc = uc->uc_mcontext.gregs[REG_RIP] - 1;
    stats.sigtrap++;
    uint64_t to = map_get(pc);
#ifdef EMU_DIFFTEST
    if (!to && diff_trap(pc, ucv)) return;
#endif
    // Every mapped address is an instruction start we patched (int3 while patching, int3 fill, or
    // the group's jmp): a trap there is ours even if the byte has meanwhile become the jmp.
    // a trap at a mapped address = a jump into a patched group past its entry (int3 fill), e.g. a switch-table
    // target the analysis did not see: write a jmp to the translation there, so later jumps do not trap. Only when
    // pc..pc+4 are int3 fill with no other mapped instruction start inside (nothing can be executing those bytes);
    // displacement first, then the opcode byte (a thread at pc sees int3 or the complete jmp).
    if (to && trapjmp_on()) {
        const uint8_t* b = (const uint8_t*)pc; int64_t rel = int64_t(to) - int64_t(pc + 5);
        bool ok = rel == int32_t(rel);
        for (int i = 0; ok && i < 5; i++) if (b[i] != 0xCC || (i && map_get(pc + i))) ok = false;
        if (ok) {
            int32_t r32 = int32_t(rel); uint8_t e9 = 0xE9;
            if (poke(pc + 1, &r32, 4)) { sync_cores(); if (poke(pc, &e9, 1)) { sync_cores(); trapjmp_n++; } }
        }
    }
    if (to && stats.sigtrap >= 1024 && !(stats.sigtrap & (stats.sigtrap - 1)))
        elog("avx512emu[%d]: sigtrap #%lu at %#lx -> %#lx\n", getpid(), (unsigned long)stats.sigtrap, (unsigned long)pc, (unsigned long)to);
    if (to) { uc->uc_mcontext.gregs[REG_RIP] = (greg_t)to; return; }
    chain(sig, si, ucv, app_trap);
}

static bool in_cache(uint64_t pc);
static void on_fault(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv;
    uint64_t pc = uc->uc_mcontext.gregs[REG_RIP];
    ObjInfo o; bool f = find_object(pc, o);
    elog("avx512emu[%d]: signal %d at pc %#lx (%s+%#lx)%s addr %#lx rsp %#lx\n", getpid(), sig, (unsigned long)pc,
         f ? (o.name[0] ? o.name : "main") : "?", f ? (unsigned long)(pc - o.base) : 0ul, in_cache(pc) ? " [translated code]" : "",
         (unsigned long)si->si_addr, (unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
    if (in_cache(pc)) {
        const uint8_t* b = (const uint8_t*)pc;
        elog("  code: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9]);
    }
    // return address chain for context
    uint64_t* sp = (uint64_t*)uc->uc_mcontext.gregs[REG_RSP];
    for (int i = 0; i < 24; i++) { uint64_t v = sp[i]; ObjInfo q; if (v > 0x10000 && find_object(v, q)) elog("  [rsp+%d] %#lx (%s+%#lx)\n", i * 8, (unsigned long)v, q.name[0] ? q.name : "main", (unsigned long)(v - q.base)); }
    struct sigaction& app = sig == SIGSEGV ? app_segv : sig == SIGBUS ? app_bus : app_fpe;
    chain(sig, si, ucv, app);
}

static void install() {
    trapjmp_on();   // read the switch outside the signal handler
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigill; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    real_sigaction(SIGILL, &sa, &app_ill);
    sa.sa_sigaction = on_sigtrap;
    real_sigaction(SIGTRAP, &sa, &app_trap);
    if (log_level >= 2) {
        sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        real_sigaction(SIGSEGV, &sa, &app_segv); real_sigaction(SIGBUS, &sa, &app_bus); real_sigaction(SIGFPE, &sa, &app_fpe);
    }
    installed = true;
}

// ---- pre-translation: the functions a run translated are listed in ~/.cache/avx512emu/<program>-<size>.list;
// the next run translates them in a background thread before they are first called (no first-use pauses).
// AVX512EMU_PRETRANSLATE=0 off.
uint32_t translated_functions(uint64_t* out, uint32_t max);
bool pretranslate_function(uint64_t fstart);
static char pre_path[512]; static int pre_pid;
struct ObjBase { const char* name; uint64_t base; bool found; };
static int objbase_cb(struct dl_phdr_info* info, size_t, void* d) {
    ObjBase* b = (ObjBase*)d; const char* n = info->dlpi_name ? info->dlpi_name : "";
    if ((b->name[0] == '-' && !b->name[1] && !n[0]) || (n[0] && !strcmp(n, b->name))) { b->base = info->dlpi_addr; b->found = true; return 1; }
    return 0;
}
static void* pretrans_thread(void*) {
    int fd = open(pre_path, O_RDONLY | O_CLOEXEC); if (fd < 0) return nullptr;
    static char buf[1 << 22]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd); if (n <= 0) return nullptr; buf[n] = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int done = 0;
    for (char* line = buf; line && *line; ) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        char* sp = strrchr(line, ' ');
        if (sp) { *sp = 0; ObjBase b{line, 0, false}; dl_iterate_phdr(objbase_cb, &b);
            if (b.found) { uint64_t a = b.base + strtoul(sp + 1, nullptr, 16); /* strtoul: 64-bit, no glibc 2.38 symbol */ { SigBlock blocked; lock(); if (pretranslate_function(a)) done++; unlock(); } } }
        line = nl ? nl + 1 : nullptr;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (log_level >= 2) elog("avx512emu[%d]: pre-translated %d functions in %.1f ms (%s)\n", getpid(), done, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, pre_path);
    return nullptr;
}
// AVX512EMU_COVLIST=FILE (coverage runs): every translated function appended at once ("<object> <offset>", as the
// pre-translation list), so a run that is killed still leaves its list.
static int cov_fd = -2;
void cov_note(uint64_t fstart) {
    if (cov_fd == -2) { const char* e = getenv("AVX512EMU_COVLIST"); cov_fd = e && *e ? open(e, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1; }
    if (cov_fd < 0 || !fstart) return;
    ObjInfo o; if (!find_object(fstart, o)) return;
    static char exe[64];
    if (!exe[0]) { char p[400]; ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1); p[n > 0 ? n : 0] = 0; const char* b = strrchr(p, '/'); snprintf(exe, sizeof exe, "%s", b ? b + 1 : "?"); }
    char line[480]; int k = snprintf(line, sizeof line, "%s %s %lx\n", exe, o.name[0] ? o.name : "-", (unsigned long)(fstart - o.base));
    if (k > 0 && k < (int)sizeof line && write(cov_fd, line, k) < 0) {}
}
static void pretrans_save() {
    if (!pre_path[0] || getpid() != pre_pid) return;   // forked children do not write
    static uint64_t fns[1 << 16]; uint32_t nf = translated_functions(fns, 1 << 16);
    if (!nf) return;
    // merge with the old list (same lines are kept once)
    static char old[1 << 22]; ssize_t no = 0; int fd = open(pre_path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { no = read(fd, old, sizeof old - 1); close(fd); if (no < 0) no = 0; } old[no] = 0;
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.%d", pre_path, getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); if (fd < 0) return;
    if (no > 0 && write(fd, old, no) < 0) {}
    for (uint32_t i = 0; i < nf; i++) {
        ObjInfo o; if (!find_object(fns[i], o)) continue;
        char line[400]; int k = snprintf(line, sizeof line, "%s %lx\n", o.name[0] ? o.name : "-", (unsigned long)(fns[i] - o.base));
        if (k <= 0 || k >= (int)sizeof line) continue;
        if (no > 0 && strstr(old, line)) continue;
        if (write(fd, line, k) < 0) {}
    }
    close(fd); rename(tmp, pre_path);
}
static void pretrans_init() {
    const char* e = getenv("AVX512EMU_PRETRANSLATE"); if (e && *e == '0') return;
    char exe[400]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1); if (n <= 0) return; exe[n] = 0;
    struct stat st; if (stat(exe, &st)) return;
    const char* base = strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe;
    const char* xc = getenv("XDG_CACHE_HOME"); const char* home = getenv("HOME");
    char dir[400];
    if (xc && *xc) snprintf(dir, sizeof dir, "%s/avx512emu", xc); else if (home && *home) snprintf(dir, sizeof dir, "%s/.cache/avx512emu", home); else return;
    char parent[400]; snprintf(parent, sizeof parent, "%s", dir); char* sl = strrchr(parent, '/'); if (sl) { *sl = 0; mkdir(parent, 0755); }
    mkdir(dir, 0755);
    snprintf(pre_path, sizeof pre_path, "%s/%s-%lld.list", dir, base, (long long)st.st_size);
    pre_pid = getpid();
    for (char* c = pre_path + strlen(dir) + 1; *c; c++) if (*c == ' ') *c = '_';
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (access(pre_path, R_OK) == 0) pthread_create(&th, &at, pretrans_thread, nullptr);
    pthread_attr_destroy(&at);
}

// ---- AVX2 mode: a CPU with some AVX-512 but not all Live uses (Skylake-X, Cascade Lake, Cooper Lake, Cannon
// Lake; the launcher preloads this library when a subset is missing). There the program's AVX-512 code would run
// natively except where an instruction faults, and the translated groups keep zmm16-31, the upper halves and the
// opmasks in emulated state while native code uses the real registers. Instead the CPU is made to look like an
// AVX2 CPU, as on CPUs without AVX-512, where every path is tested:
//   - CPUID faulting (arch_prctl ARCH_SET_CPUID 0, per thread, inherited by new threads, reset by execve): every
//     CPUID of the program traps (SIGSEGV) and is answered with the AVX-512 feature bits cleared, so CPUID
//     dispatchers (libraries, plug-ins) take their AVX2 paths;
//   - every function with AVX-512 / opmask code in the program's own files (the install folder: the executable
//     under .../push3_opt/..., its libraries, compat_lib) is translated before the program runs, and the objects
//     a later dlopen adds likewise, so none of it runs natively.
// The C library keeps its own choice (made before this library is loaded): its AVX-512 string functions are
// native on this CPU and keep to the calling convention. AVX512EMU_AVX2MODE=0 turns this off (translation on
// SIGILL only, as before); =1 turns it on for a CPU with all of AVX-512 too (tests). AVX512EMU_SCAN=exe also
// translates the main executable of a program outside an install folder (tests).
static bool avx2_mode, cpuid_trap;
static volatile uint64_t cpuid_n;
static char scope_root[2][512];   // install folder, real path and the path the program was started through
static bool scope_exe;            // AVX512EMU_SCAN=exe
static bool host_has_avx512() {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return false;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!((c >> 27) & 1)) return false;   // OSXSAVE
    unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 0xe6) != 0xe6) return false;   // the OS saves the AVX-512 state
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b >> 16) & 1;                    // AVX512F
}
static void masked_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    syscall(SYS_arch_prctl, ARCH_SET_CPUID, 1);
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
    syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0);
    if (leaf == 7 && sub == 0) {
        // EBX: F DQ IFMA PF ER CD BW VL; ECX: VBMI VBMI2 VNNI BITALG VPOPCNTDQ; EDX: 4VNNIW 4FMAPS VP2INTERSECT FP16
        r[1] &= ~((1u << 16) | (1u << 17) | (1u << 21) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 30) | (1u << 31));
        r[2] &= ~((1u << 1) | (1u << 6) | (1u << 11) | (1u << 12) | (1u << 14));
        r[3] &= ~((1u << 2) | (1u << 3) | (1u << 8) | (1u << 23));
    } else if (leaf == 7 && sub == 1) {
        r[0] &= ~(1u << 5);    // AVX512_BF16
        r[3] &= ~(1u << 19);   // AVX10
    } else if (leaf == 0xd && sub == 0) r[0] &= ~0xe0u;   // XCR0 bits the CPU supports: no opmask/ZMM state
}
static void on_cpuid_segv(int sig, siginfo_t* si, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv; const uint8_t* pc = (const uint8_t*)uc->uc_mcontext.gregs[REG_RIP];
    if (cpuid_trap && si->si_code == SI_KERNEL && pc[0] == 0x0f && pc[1] == 0xa2) {
        uint32_t r[4]; masked_cpuid((uint32_t)uc->uc_mcontext.gregs[REG_RAX], (uint32_t)uc->uc_mcontext.gregs[REG_RCX], r);
        uc->uc_mcontext.gregs[REG_RAX] = r[0]; uc->uc_mcontext.gregs[REG_RBX] = r[1];
        uc->uc_mcontext.gregs[REG_RCX] = r[2]; uc->uc_mcontext.gregs[REG_RDX] = r[3];
        uc->uc_mcontext.gregs[REG_RIP] += 2;
        uint64_t n = __atomic_add_fetch(&cpuid_n, 1, __ATOMIC_RELAXED);
        if (log_level >= 2 && !(n & (n - 1)) && n >= 64) elog("avx512emu[%d]: CPUID answered %lu times\n", getpid(), (unsigned long)n);
        return;
    }
    if (log_level >= 2) on_fault(sig, si, ucv); else chain(sig, si, ucv, app_segv);
}
// [path] lies in the install folder (or is the main executable with AVX512EMU_SCAN=exe).
static bool in_scope(const char* path) {
    if (!path || !*path) return scope_exe || scope_root[0][0];   // the main executable (lies in the install folder)
    for (const char* root : scope_root) {
        size_t n = strlen(root);
        if (n && !strncmp(path, root, n) && path[n] == '/') return true;
    }
    if (scope_root[0][0]) {   // a path through other links: its real path
        char real[PATH_MAX];
        if (realpath(path, real)) { size_t n = strlen(scope_root[0]); return !strncmp(real, scope_root[0], n) && real[n] == '/'; }
    }
    return false;
}
// [fs, fe) has an instruction the emulator translates (decoded from the start; anything undecodable: let the
// function analysis decide).
static bool has_special(uint64_t fs, uint64_t fe) {
    for (uint64_t a = fs; a < fe; ) {
        x86::Insn in;
        int len = x86::decode((const uint8_t*)a, int(fe - a < 15 ? fe - a : 15), in);
        if (len <= 0) return true;
        if (is_special(in)) return true;
        a += len;
    }
    return false;
}
static uint64_t scanned[1024]; static int nscanned;
struct ScanObj { uint64_t base, exec; char name[256]; };
// Translates every function with AVX-512 code in the objects in scope that were not scanned yet. Returns the
// number of functions translated.
static int scan_pass() {
    static ScanObj objs[1024]; int nobj = 0;
    struct Ctx { ScanObj* objs; int* n; } ctx{objs, &nobj};
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* d) -> int {
        Ctx* c = (Ctx*)d; if (*c->n >= 1024) return 0;
        for (int i = 0; i < nscanned; i++) if (scanned[i] == info->dlpi_addr) return 0;
        uint64_t exec = 0;
        for (int i = 0; i < info->dlpi_phnum; i++)
            if (info->dlpi_phdr[i].p_type == PT_LOAD && (info->dlpi_phdr[i].p_flags & PF_X)) { exec = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr; break; }
        if (!exec) return 0;
        ScanObj& o = c->objs[(*c->n)++]; o.base = info->dlpi_addr; o.exec = exec;
        snprintf(o.name, sizeof o.name, "%s", info->dlpi_name ? info->dlpi_name : "");
        return 0; }, &ctx);
    int done = 0;
    for (int k = 0; k < nobj; k++) {
        if (nscanned < 1024) scanned[nscanned++] = objs[k].base;
        if (!in_scope(objs[k].name)) continue;
        ObjInfo o; if (!find_object(objs[k].exec, o) || !o.eh_frame_hdr) continue;
        const uint8_t* h = (const uint8_t*)o.eh_frame_hdr;
        if (h[0] != 1 || h[3] != 0x3b) continue;   // datarel|sdata4 table (find_function's)
        uint32_t count; const uint8_t* p = h + 4;
        if (h[1] == 0x1b) p += 4; else continue;   // eh_frame_ptr: pcrel|sdata4
        if (h[2] != 0x03) continue;                // fde_count: udata4
        memcpy(&count, p, 4); p += 4;
        const int32_t* tab = (const int32_t*)p;
        int fn = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t start = o.eh_frame_hdr + int64_t(tab[2 * i]), fs, fe;
            if (!find_function(o, start, fs, fe) || fs != start || !has_special(fs, fe)) continue;
            SigBlock blocked; lock(); if (pretranslate_function(fs)) fn++; unlock();
        }
        done += fn;
        if (log_level >= 2 && fn) elog("avx512emu[%d]: AVX2 mode: %s: %d functions translated\n", getpid(), o.name[0] ? o.name : "main", fn);
    }
    return done;
}
static void avx2_mode_init() {
    const char* e = getenv("AVX512EMU_AVX2MODE");
    if (e && *e == '0') return;
    if (!(e && *e == '1') && !host_has_avx512()) return;
    avx2_mode = true;
    const char* sc = getenv("AVX512EMU_SCAN"); scope_exe = sc && !strcmp(sc, "exe");
    // the install folder: the part of the executable's path before /push3_opt/
    char exe[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char* cut = strstr(exe, "/push3_opt/");
        if (cut) { *cut = 0; snprintf(scope_root[0], sizeof scope_root[0], "%s", exe); }
    }
    const char* link = getenv("LIVE_LINK_ROOT");   // the launcher's link without hidden folders
    if (scope_root[0][0] && link && *link) snprintf(scope_root[1], sizeof scope_root[1], "%s", link);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_cpuid_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    struct sigaction prev; real_sigaction(SIGSEGV, &sa, &prev);
    if (log_level < 2) app_segv = prev;
    cpuid_trap = syscall(SYS_arch_prctl, ARCH_SET_CPUID, 0) == 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int done = (scope_root[0][0] || scope_exe) ? scan_pass() : 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elog("avx512emu[%d]: AVX2 mode (this CPU has only part of AVX-512): CPUID %s, %d functions translated in %.0f ms\n",
         getpid(), cpuid_trap ? "masked" : "NOT masked (no CPUID faulting)", done,
         (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

void count_report();
static void stats_at_exit() {
    pretrans_save();
    prof_report();
    count_report();
    if (log_level >= 2)
        elog("avx512emu[%d]: translation time %.1f ms total, %.2f ms max; audio threads %lu translations %.1f ms max %.2f; lock wait max %.2f ms\n", getpid(),
             stats.xlate_ns / 1e6, stats.xlate_max_ns / 1e6, (unsigned long)stats.rt_n, stats.rt_ns / 1e6, stats.rt_max_ns / 1e6, stats.wait_max_ns / 1e6),
        elog("avx512emu[%d]: sigill %lu sigtrap %lu functions %lu groups %lu specials %lu fixups %lu int3 %lu single %lu failures %lu\n",
             getpid(), (unsigned long)stats.sigill, (unsigned long)stats.sigtrap, (unsigned long)stats.functions, (unsigned long)stats.groups,
             (unsigned long)stats.specials, (unsigned long)stats.fixups, (unsigned long)stats.int3_sites, (unsigned long)stats.fallback_sites, (unsigned long)stats.failures);
    if (log_level >= 2 && cap_records) elog("avx512emu[%d]: captured %lu group entries, %.1f MB\n", getpid(), (unsigned long)cap_records, cap_bytes / 1048576.0);
    if (log_level >= 2 && trapjmp_n) elog("avx512emu[%d]: jmps written at trapping entries %lu\n", getpid(), trapjmp_n);
}

__attribute__((constructor(101))) static void init() {
    real_sigaction = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (!real_sigaction) return;
    const char* lv = getenv("AVX512EMU_LOG");
    if (lv) log_level = atoi(lv);
    const char* ws = getenv("AVX512EMU_SIGWRAP");
    if (ws && *ws == '0') wrap_signals = false;
    const char* ni = getenv("AVX512EMU_INLINE");
    if (ni && *ni == '0') no_inline = true;
    const char* lf = getenv("AVX512EMU_LOGFILE");
    if (lf) { int fd = open(lf, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644); if (fd >= 0) log_fd = fd; }
    if (getenv("AVX512EMU_PROF")) {
        prof_on = true;
        Dl_info di; if (dladdr((void*)&init, &di)) { ObjInfo o; if (find_object((uint64_t)&init, o)) { lib_lo = o.lo; lib_hi = o.hi; } }
        struct sigaction pa; memset(&pa, 0, sizeof pa); pa.sa_sigaction = on_prof; pa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&pa.sa_mask);
        if (real_sigaction) real_sigaction(SIGPROF, &pa, nullptr);
        struct itimerval it; it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000; it.it_value = it.it_interval;
        setitimer(ITIMER_PROF, &it, nullptr);
    }
    membarrier_ok = syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) == 0;
    map_init();
    scratch_alloc(64); scratch_reset();    // map and pre-fault the arena now, not on the first SIGILL
    offline_attach();                      // translated code made ahead of time (offline segment), if any
    {   // first code cache chunk next to the main executable, faulted in before the program runs
        struct Ctx { uint64_t lo; } ctx{0};
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* d) -> int {
            for (int i = 0; i < info->dlpi_phnum; i++) if (info->dlpi_phdr[i].p_type == PT_LOAD && (info->dlpi_phdr[i].p_flags & PF_X)) {
                ((Ctx*)d)->lo = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr; return 1; }
            return 0; }, &ctx);
        if (ctx.lo) { uint8_t* p = cache_alloc(ctx.lo, 64); if (p) cache_trim(p); }
    }
    pthread_atfork(fork_prepare, fork_parent, fork_child);
    install();
    atexit(stats_at_exit);
    avx2_mode_init();
    if (!avx2_mode) pretrans_init();   // AVX2 mode translated everything already
    cov_note(0);   // opens AVX512EMU_COVLIST now: the program may change its environment later
}

} // namespace emu

using namespace emu;

// The program's own SIGILL/SIGTRAP handlers are recorded and chained.
extern "C" __attribute__((visibility("default"))) int sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    if (!real_sigaction) real_sigaction = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (installed && (sig == SIGILL || sig == SIGTRAP || (avx2_mode && sig == SIGSEGV) || (log_level >= 2 && (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE)))) {
        struct sigaction& app = sig == SIGILL ? app_ill : sig == SIGTRAP ? app_trap : sig == SIGSEGV ? app_segv : sig == SIGBUS ? app_bus : app_fpe;
        if (old) *old = app;
        if (act) app = *act;
        return 0;
    }
    if (sig > 0 && sig < 65 && sig != SIGPROF && wrap_signals) {
        struct sigaction prev;
        if (real_sigaction(sig, nullptr, &prev)) return -1;
        bool was_wrapped = (prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction == wrap_handler;
        if (old) *old = was_wrapped ? app_act[sig] : prev;
        if (!act) return 0;
        bool custom = (act->sa_flags & SA_SIGINFO) ? act->sa_sigaction != nullptr : (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN);
        if (!custom) return real_sigaction(sig, act, nullptr);
        app_act[sig] = *act;
        struct sigaction w = *act;
        w.sa_flags |= SA_SIGINFO; w.sa_sigaction = wrap_handler;
        return real_sigaction(sig, &w, nullptr);
    }
    return real_sigaction(sig, act, old);
}
// AVX2 mode: the objects a dlopen adds are translated before it returns.
extern "C" __attribute__((visibility("default"))) void* dlopen(const char* file, int flags) {
    typedef void* (*dlopen_fn)(const char*, int);
    static dlopen_fn real = (dlopen_fn)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
    if (!real) real = (dlopen_fn)dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    void* h = real(file, flags);
    if (h && emu::avx2_mode) {
        int done = emu::scan_pass();
        if (done && emu::log_level >= 2) emu::elog("avx512emu[%d]: dlopen %s: %d functions translated\n", getpid(), file ? file : "-", done);
    }
    return h;
}
extern "C" __attribute__((visibility("default"))) sighandler_t signal(int sig, sighandler_t h) {
    struct sigaction sa, old; memset(&sa, 0, sizeof sa);
    sa.sa_handler = h; sa.sa_flags = SA_RESTART; sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, &old)) return SIG_ERR;
    return old.sa_handler;
}
