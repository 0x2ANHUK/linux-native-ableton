// gharness ELF [-n TRIALS] [-f MAXFUNCS] [-s SKIP] [-v]: group-level differential test.
// Maps the ELF's segments, runs the function analysis on every function with
// AVX-512 code, and for each translation group runs the original instructions
// natively (AVX-512 host) and the translated group from the same random state,
// then compares GPRs, flags, memory, opmasks, zmm16-31 and the ymm registers
// live after the group.
#include "func.cpp"
#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string>
#include <map>
#include <set>
#include <immintrin.h>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stddef.h>
#include <sys/wait.h>
#include <vector>
#include <sys/time.h>

using namespace emu;

struct alignas(64) HS { uint64_t gpr[16]; uint64_t flags; uint64_t pad[7]; uint8_t z[32][64]; uint64_t k[8]; };
extern "C" { void h_enter(HS*); void h_enter2(HS*); void h_exit(); void h_exit2(); extern uint64_t g_code; }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static sigjmp_buf jb;
static volatile int fault_sig;
static volatile uint64_t fault_addr, fault_pc;
// sandbox for -F / -r: wild jumps with random state can reach any byte sequence in the image (wrfsbase, syscall)
static uint64_t g_fsbase, g_gsbase; static unsigned g_mxcsr0;
static inline uint64_t rdfs() { uint64_t v; __asm__ volatile("rdfsbase %0" : "=r"(v)); return v; }
static inline void wrfs(uint64_t v) { __asm__ volatile("wrfsbase %0" :: "r"(v)); }
static inline uint64_t rdgs() { uint64_t v; __asm__ volatile("rdgsbase %0" : "=r"(v)); return v; }
static inline void wrgs(uint64_t v) { __asm__ volatile("wrgsbase %0" :: "r"(v)); }
static bool sandbox_on;
static inline void sandbox_restore() { if (!sandbox_on) return; if (rdfs() != g_fsbase) wrfs(g_fsbase); if (rdgs() != g_gsbase) wrgs(g_gsbase); }
extern __thread unsigned char fx_tls_pad[16384];
static void fx_tls_reset() { memset(fx_tls_pad, 0, sizeof fx_tls_pad); }
static volatile int in_run = 1;   // -F: 0 outside a run (a fault there is the harness's own: report and stop)
static void on_fault(int sig, siginfo_t* si, void* ucv) {
    sandbox_restore();
    if (!in_run) { ucontext_t* uc = (ucontext_t*)ucv; char b[160]; int n = snprintf(b, sizeof b, "harness fault outside a run: sig %d addr %p pc %#llx\n", sig, si ? si->si_addr : nullptr, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP]);
        if (write(2, b, n) < 0) {} _exit(9); }
    fault_sig = sig; fault_addr = si ? (uint64_t)si->si_addr : 0; fault_pc = (uint64_t)((ucontext_t*)ucv)->uc_mcontext.gregs[REG_RIP]; siglongjmp(jb, 1); }

static const size_t DATA = 1 << 20;
static uint8_t* data;
static uint8_t mem0[DATA], memA[DATA];
static int trials = 4, verbose = 0;
static long groups_tested, groups_failed, trials_run, trials_skipped, groups_skipped, groups_regions;
static ObjInfo obj;
static uint64_t dump_addr, only_addr; static int time_only;
static uint64_t tramp_exit, tramp_exit2; // jmp *abs near the mapped image
static uint64_t make_tramp(uint64_t near, uint64_t target) {
    uint8_t* t = cache_alloc(near, 16, 16);
    t[0] = 0xFF; t[1] = 0x25; memset(t + 2, 0, 4); memcpy(t + 6, &target, 8);
    return (uint64_t)t;
}

// GH_EDGE=1: a quarter of the 8-byte slots get edge values (zeros, infinities, NaNs, denormals, conversion limits,
// integer extremes), per 4-byte half or as one double / int64
static const uint32_t edge32[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7fa00000u, 0x00000001u,
    0x807fffffu, 0x00800000u, 0x7f7fffffu, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0x4f000000u, 0xcf000000u, 0x5f000000u,
    0x4f800000u, 0xdf000000u, 0x7fffffffu, 0xffffffffu, 0x00000080u, 0x0000007fu, 0x00008000u, 0x00007fffu, 0x000000ffu,
    0x0000ffffu, 0x80008000u, 0x7f807f80u, 0x3effffffu, 0x4b000000u, 0x4b400001u};
static const uint64_t edge64[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0xfff8000000000000ull, 0x7ff4000000000000ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
    0x3ff0000000000000ull, 0xbff0000000000000ull, 0x41e0000000000000ull, 0xc1e0000000000000ull, 0x43e0000000000000ull,
    0xc3e0000000000000ull, 0x41f0000000000000ull, 0x43f0000000000000ull, 0x7fffffffffffffffull, 0xffffffffffffffffull,
    0x00000000ffffffffull, 0x0000000080000000ull, 0x3fdfffffffffffffull, 0x4330000000000000ull, 0x4338000000000001ull};
static int edge_on = -1;
static void fill(uint8_t* p, size_t n) {
    if (edge_on < 0) edge_on = getenv("GH_EDGE") != nullptr;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v; int m = rnd() % 8;
        if (edge_on && rnd() % 4 == 0) {
            if (rnd() % 2) v = edge64[rnd() % (sizeof edge64 / 8)];
            else v = edge32[rnd() % (sizeof edge32 / 4)] | uint64_t(edge32[rnd() % (sizeof edge32 / 4)]) << 32;
            memcpy(p + i, &v, 8); continue;
        }
        if (m < 3) { float f[2] = {float((int64_t)(rnd() % 20000) - 10000) / float(1 + rnd() % 100), float(rnd() % 1000) / 7.f}; memcpy(&v, f, 8); }
        else if (m < 5) { double d = double((int64_t)(rnd() % 2000000) - 1000000) / double(1 + rnd() % 1000); memcpy(&v, &d, 8); }
        else if (m == 5) v = rnd() % 64 | (rnd() % 64) << 32;
        else v = rnd();
        memcpy(p + i, &v, 8);
    }
}

static bool guard_mode; static bool force_mode_flag;   // -g: an unmapped page after data; about half the GPRs point into its last 256 bytes, prefix masks
static void random_state(HS& s) {
    memset(&s, 0, sizeof s);
    for (int r = 0; r < 32; r++) fill(s.z[r], 64);
    for (int r = 0; r < 8; r++) { s.k[r] = rnd(); if (rnd() % 4 == 0) s.k[r] = ~0ull; }
    if (edge_on) for (int r = 1; r < 8; r++) { int e = rnd() % 6; if (e == 0) s.k[r] = 0; else if (e == 1) s.k[r] = 0x5555555555555555ull; else if (e == 2) s.k[r] = 1; else if (e == 3) s.k[r] = 1ull << (rnd() % 64); }
    for (int r = 0; r < 16; r++) s.gpr[r] = (uint64_t)data + DATA / 4 + (rnd() % (DATA / 2)) / 64 * 64;
    if (region_mode) for (int r = 0; r < 16; r++) if (r != RSP && rnd() % 5 < 2) s.gpr[r] = rnd() % 64;   // loops index with small integers
    s.gpr[RSP] = (uint64_t)data + DATA - 65536;
    if (guard_mode) {
        for (int r = 0; r < 16; r++) if (r != RSP && rnd() % 2) s.gpr[r] = (uint64_t)data + DATA - rnd() % 256;
        for (int r = 1; r < 8; r++) if (rnd() % 2) { int j = rnd() % 65; s.k[r] = j == 64 ? ~0ull : (1ull << j) - 1; }
    }
    s.flags = 0x202 | (rnd() & 0x8d5);
    static int ba = -1; if (ba < 0) ba = getenv("FX_BUFARGS") != nullptr;   // difftest k=5: pointer args -> float buffers, counts 64
    if (ba && force_mode_flag) {
        static const int ar[] = {RDI, RSI, RDX, RCX, R8, R9};
        static int bn = -1; if (bn < 0) bn = getenv("FX_BUFN") ? atoi(getenv("FX_BUFN")) : 4;   // first BN args are buffers, the rest counts
        for (int i = 0; i < 6; i++) s.gpr[ar[i]] = (i < bn) ? (uint64_t)data + 4096 + uint64_t(i) * (DATA / 7) : 1 + rnd() % 300;
    }
}

static std::string hex(const uint8_t* p, int n) { std::string s; char b[4]; for (int i = 0; i < n; i++) { snprintf(b, 4, "%02x", p[i]); s += b; } return s; }

// build the native copy of [g0, g1) into buf, ending with a jmp to h_exit
static uint64_t build_native(FI* fi, int g0, int g1) {
    uint8_t* code = cache_alloc(fi[g0].addr, 4096 + (g1 - g0) * 32, 64);
    if (!code) return 0;
    uint64_t* slots = (uint64_t*)code;
    Asm a(code + 64, 4096 + (g1 - g0) * 32 - 64, (uint64_t)code + 64);
    int ci = 0;
    for (int i = g0; i < g1; i++) if (!emit_copy(a, fi[i], &slots[ci++ & 7])) return 0;
    a.jmp(tramp_exit);
    if (a.overflow) return 0;
    __builtin___clear_cache((char*)code, (char*)code + 4096);
    return (uint64_t)code + 64;
}

// region: internal branches stay in the copy; branches to the head or out of it leave (one pass through the loop)
static uint64_t build_native_region(FI* fi, int g0, int g1) {
    size_t cap = 8192 + (g1 - g0) * 32;
    uint8_t* code = cache_alloc(fi[g0].addr, cap, 64);
    if (!code) return 0;
    uint64_t* slots = (uint64_t*)code;
    std::vector<uint64_t> pos(g1 - g0 + 1, 0);
    for (int pass = 0; pass < 2; pass++) {
        Asm a(code + 64, cap - 64, (uint64_t)code + 64);
        int ci = 0;
        for (int i = g0; i < g1; i++) {
            pos[i - g0] = a.pc();
            const FI& f = fi[i];
            if ((f.in.flow == F_JCC || f.in.flow == F_JMP) && !(f.in.map == 0 && f.in.opc >= 0xe0)) {
                int tg = f.target;
                uint64_t dest = (pass == 1 && tg > g0 && tg < g1) ? pos[tg - g0] : (pass == 0 ? a.pc() : tramp_exit);
                if (f.in.flow == F_JCC) a.jcc((f.in.map == 0) ? (f.in.opc - 0x70) : (f.in.opc - 0x80), dest); else a.jmp(dest);
            } else if (!emit_copy(a, f, &slots[ci++ & 7])) return 0;
        }
        a.jmp(tramp_exit);
        if (a.overflow) return 0;
    }
    __builtin___clear_cache((char*)code, (char*)code + cap);
    return (uint64_t)code + 64;
}

// ---- replay (-r FILE): group-entry records captured in a real run (runtime AVX512EMU_CAPTURE), one process per
// file. The ELF is mapped at the captured base; memory windows go back to their addresses (pages outside the image
// are mapped fresh, filled with random bytes, then the captured bytes), the stack gets 64 KB below rsp. The
// original instructions run natively and the translation runs, both from the captured state; compared as above.
struct CapRec { uint64_t pc, end, gpr[16], rflags; uint32_t mxcsr; uint8_t z[32][64]; uint64_t k[8]; std::vector<std::pair<uint64_t, std::string>> win; };
static bool replay_mode; static uint64_t cap_base;
static std::map<uint64_t, std::vector<CapRec>> caps;     // by offset of the group start
static long rp_records, rp_matched, rp_failed_groups, rp_conflict, rp_native_fault, rp_trans_fault;
static std::set<uint64_t> rp_done;
static uint64_t img_lo, img_hi, text_lo, text_hi;
static bool load_caps(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return false;
    std::vector<uint8_t> b; uint8_t tmp[1 << 16]; size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + r);
    fclose(f);
    size_t o = 0;
    struct H { uint32_t magic, len; uint64_t base, pc, end, gpr[16], rflags; uint32_t mxcsr, nwin; };
    while (o + sizeof(H) <= b.size()) {
        H h; memcpy(&h, &b[o], sizeof h);
        if (h.magic != 0x31435841 || o + h.len > b.size()) break;
        if (!cap_base) cap_base = h.base;
        if (h.base == cap_base) {
            CapRec c; c.pc = h.pc; c.end = h.end; memcpy(c.gpr, h.gpr, sizeof c.gpr); c.rflags = h.rflags; c.mxcsr = h.mxcsr;
            size_t q = o + sizeof h; memcpy(c.z, &b[q], sizeof c.z); q += sizeof c.z; memcpy(c.k, &b[q], 64); q += 64;
            for (uint32_t i = 0; i < h.nwin; i++) {
                uint64_t a; uint32_t l; memcpy(&a, &b[q], 8); memcpy(&l, &b[q + 8], 4);
                c.win.push_back({a, std::string((const char*)&b[q + 16], l)}); q += 16 + ((l + 7) & ~7u);
            }
            caps[h.pc - h.base].push_back(std::move(c)); rp_records++;
        }
        o += h.len;
    }
    return true;
}
static bool replay_group(FI* fi, int n, int g0, int g1) {
    auto it = caps.find(fi[g0].addr - obj.base);
    if (it == caps.end()) return true;
    rp_done.insert(it->first);
    while (g1 > g0 && fi[g1 - 1].in.flow != F_NONE) g1--;
    if (g1 <= g0) return true;
    for (int i = g0; i < g1; i++) if (fi[i].in.seg == 0x64 || fi[i].in.seg == 0x65) return true;
    uint64_t native = build_native(fi, g0, g1);
    GhCtx ctx; ctx.exit = tramp_exit2; ctx.entry = 0;
    gh = &ctx; bool ok = install_group(fi, g0, g1, false); gh = nullptr;
    if (!native || !ok || !ctx.entry) return true;
    uint16_t live = fi[g1 - 1].vlive; uint32_t flive = flags_live_after(fi, n, g1 - 1);
    uint32_t stale_hi = 0;
    for (int i = g0; i < g1; i++) {
        int d = vec_dst(fi[i].in);
        if (fi[i].in.enc == VEX && !(fi[i].flags & F_SPECIAL)) { if (d >= 0 && d < 16 && !(fi[i].flags & F_FIXUP)) stale_hi |= 1u << d; if (d == -2) stale_hi |= 0xffff; }
        else if (fi[i].in.enc == EVEX && d >= 0 && d < 16) stale_hi &= ~(1u << d);
    }
    bool failed = false;
    for (CapRec& c : it->second) {
        if (failed) break;
        // memory: our pages (outside the image) + windows inside the image
        std::vector<uint64_t> pages; bool conflict = false;
        auto own_page = [&](uint64_t pg) -> bool {
            for (uint64_t p : pages) if (p == pg) return true;
            if (pg >= img_lo && pg < img_hi) return true;
            void* m = mmap((void*)pg, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m != (void*)pg) { if (m != MAP_FAILED) munmap(m, 4096); return false; }
            fill((uint8_t*)pg, 4096); pages.push_back(pg); return true;
        };
        uint64_t sp = c.gpr[RSP];
        for (uint64_t pg = (sp - 65536) & ~0xfffull; pg <= ((sp + 512) & ~0xfffull) && !conflict; pg += 4096) if (!own_page(pg)) conflict = true;
        std::vector<std::pair<uint64_t, uint64_t>> imgw;
        for (auto& w : c.win) {
            uint64_t lo = w.first, hi = w.first + w.second.size();
            if (lo < text_hi && hi > text_lo) continue;                       // never overwrite code
            for (uint64_t pg = lo & ~0xfffull; pg < hi && !conflict; pg += 4096) if (!own_page(pg)) conflict = true;
            if (conflict) break;
            memcpy((void*)lo, w.second.data(), w.second.size());
            if (lo >= img_lo && hi <= img_hi) imgw.push_back({lo, hi});
        }
        if (conflict) { rp_conflict++; for (uint64_t p : pages) munmap((void*)p, 4096); continue; }
        // snapshot of everything compared
        std::vector<std::pair<uint64_t, uint64_t>> cmp; for (uint64_t p : pages) cmp.push_back({p, p + 4096}); for (auto& w : imgw) cmp.push_back(w);
        std::vector<std::string> init, after;
        for (auto& r : cmp) init.push_back(std::string((const char*)r.first, r.second - r.first));
        static HS A, B; memset(&A, 0, sizeof A);
        memcpy(A.gpr, c.gpr, sizeof c.gpr); A.flags = c.rflags; memcpy(A.z, c.z, sizeof c.z); memcpy(A.k, c.k, 64);
        B = A;
        unsigned csr0 = _mm_getcsr();
        g_code = native; fault_sig = 0; bool nfault = false;
        { struct itimerval tv = {{0, 0}, {0, 50000}}; setitimer(ITIMER_REAL, &tv, nullptr); }
        _mm_setcsr(c.mxcsr);
        if (sigsetjmp(jb, 1) == 0) h_enter(&A); else nfault = true;
        _mm_setcsr(csr0);
        { struct itimerval z0 = {}; setitimer(ITIMER_REAL, &z0, nullptr); }
        if (nfault) { rp_native_fault++; for (uint64_t p : pages) munmap((void*)p, 4096); continue; }
        for (auto& r : cmp) after.push_back(std::string((const char*)r.first, r.second - r.first));
        for (size_t i = 0; i < cmp.size(); i++) memcpy((void*)cmp[i].first, init[i].data(), init[i].size());
        for (int i = 0; i < 16; i++) memcpy(tst.zhi[i], c.z[i] + 32, 32);
        for (int i = 0; i < 16; i++) memcpy(tst.zx[i], c.z[16 + i], 64);
        memcpy(tst.k, c.k, 64);
        g_code = ctx.entry; fault_sig = 0; std::string why;
        { struct itimerval tv = {{0, 0}, {0, 500000}}; setitimer(ITIMER_REAL, &tv, nullptr); }
        _mm_setcsr(c.mxcsr);
        if (sigsetjmp(jb, 1) == 0) h_enter2(&B); else why = " translated code faulted (sig " + std::to_string(fault_sig) + ")";
        _mm_setcsr(csr0);
        { struct itimerval z0 = {}; setitimer(ITIMER_REAL, &z0, nullptr); }
        if (why.empty()) {
            for (int i = 0; i < 16; i++) memcpy(B.z[i] + 32, tst.zhi[i], 32);
            for (int i = 0; i < 16; i++) memcpy(B.z[16 + i], tst.zx[i], 64);
            memcpy(B.k, tst.k, 64);
            char tb[160];
            for (int r = 0; r < 16; r++) if (r != RSP && ((fi[g1 - 1].glive >> r) & 1) && A.gpr[r] != B.gpr[r]) { snprintf(tb, sizeof tb, " gpr%d %lx/%lx", r, (unsigned long)A.gpr[r], (unsigned long)B.gpr[r]); why += tb; }
            if ((A.flags ^ B.flags) & flive) { snprintf(tb, sizeof tb, " flags %lx/%lx", (unsigned long)A.flags & 0x8d5, (unsigned long)B.flags & 0x8d5); why += tb; }
            for (int k = 0; k < 8; k++) if (A.k[k] != B.k[k]) { snprintf(tb, sizeof tb, " k%d %lx/%lx", k, (unsigned long)A.k[k], (unsigned long)B.k[k]); why += tb; }
            for (int r = 0; r < 16; r++) {
                if ((live >> r) & 1) { if (memcmp(A.z[r], B.z[r], 32)) why += " ymm" + std::to_string(r) + "[" + hex(A.z[r], 32) + " / " + hex(B.z[r], 32) + "]"; }
                bool hi_check = ((fn_rd512 | 0xff) >> r & 1) && !((stale_hi >> r) & 1) && ((fi[g1 - 1].zlive >> r) & 1);
                if (hi_check && memcmp(A.z[r] + 32, B.z[r] + 32, 32)) why += " zhi" + std::to_string(r);
            }
            for (int r = 16; r < 32; r++) {
                if (!((fi[g1 - 1].hlive >> (r - 16)) & 1)) continue;
                if (memcmp(A.z[r], B.z[r], 16)) why += " xmm" + std::to_string(r);
                if ((fn_rd256 >> r & 1) && memcmp(A.z[r] + 16, B.z[r] + 16, 16)) why += " zx" + std::to_string(r) + "[16..31]";
                if ((fn_rd512 >> r & 1) && memcmp(A.z[r] + 32, B.z[r] + 32, 32)) why += " zx" + std::to_string(r) + "[32..63]";
            }
            for (size_t i = 0; i < cmp.size() && why.find(" mem@") == std::string::npos; i++)
                for (uint64_t a = cmp[i].first; a < cmp[i].second; a++) {
                    if (a >= sp - 32768 && a < sp - 128) continue;
                    if (((const uint8_t*)a)[0] != (uint8_t)after[i][a - cmp[i].first]) { snprintf(tb, sizeof tb, " mem@%#lx (sp%+ld)", (unsigned long)a, (long)(a - sp)); why += tb; break; }
                }
        } else rp_trans_fault++;
        rp_matched++;
        if (!why.empty()) {
            failed = true; rp_failed_groups++;
            printf("FAIL replay group %#lx-%#lx (%d insns):%s\n", (unsigned long)(fi[g0].addr - obj.base), (unsigned long)(fi[g1 - 1].addr + fi[g1 - 1].in.len - obj.base), g1 - g0, why.substr(0, 600).c_str());
        }
        for (uint64_t p : pages) munmap((void*)p, 4096);
    }
    return true;
}

// ---- forced execution (-F): whole functions from their entry, native first, then translated (patched in place
// as at run time) from the same random state; the return address is the exit stub. Compared: rax, rdx, rbx, rbp,
// r12-r15, rsp, flags-free; ymm0/ymm1 (return values); the data buffer and the image's writable segments. Both
// faulting at the same address counts as the same outcome; timeouts (50 ms) are skipped.
// seccomp: syscalls from below the harness's own code or between it and the libraries (image, caches, data, arena,
// captured pages) trap (SIGSYS -> a fault outcome); harness and library code get an allowlist, the rest EPERM
static void sandbox_install() {
    g_fsbase = rdfs(); g_gsbase = rdgs(); g_mxcsr0 = _mm_getcsr();
    extern char __executable_start[], etext[];
    uint64_t hlo = (uint64_t)__executable_start, hhi = (uint64_t)etext;
    static const int allow[] = {SYS_clone, SYS_clone3, SYS_wait4, SYS_kill, SYS_pipe2, SYS_read, SYS_close, SYS_alarm, SYS_write, SYS_mmap, SYS_munmap, SYS_mprotect, SYS_madvise, SYS_mremap, SYS_brk, SYS_rt_sigreturn,
        SYS_rt_sigprocmask, SYS_rt_sigaction, SYS_setitimer, SYS_getitimer, SYS_exit, SYS_exit_group, SYS_futex, SYS_membarrier,
        SYS_clock_gettime, SYS_gettid, SYS_getpid, SYS_sched_yield, SYS_sigaltstack, SYS_newfstatat, SYS_fstat, SYS_tgkill};
    std::vector<sock_filter> f;
    auto ins = [&](uint16_t code, uint32_t k, uint8_t jt, uint8_t jf) { f.push_back(sock_filter{code, jt, jf, k}); };
    // arch
    ins(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch), 0, 0);
    ins(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    ins(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS, 0, 0);
    // ip: high word >= 0x7000 (libraries, vdso) or [hlo, hhi) (harness, < 4 GB) -> allowlist; else trap
    ins(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer) + 4, 0, 0);
    ins(BPF_JMP | BPF_JGE | BPF_K, 0x7000, 6, 0);                 // -> allowlist
    ins(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0);
    ins(BPF_RET | BPF_K, SECCOMP_RET_TRAP, 0, 0);
    ins(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, instruction_pointer), 0, 0);
    ins(BPF_JMP | BPF_JGE | BPF_K, uint32_t(hlo), 0, 1);
    ins(BPF_JMP | BPF_JGE | BPF_K, uint32_t(hhi), 0, 1);
    ins(BPF_RET | BPF_K, SECCOMP_RET_TRAP, 0, 0);
    // allowlist
    ins(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr), 0, 0);
    int n = sizeof allow / sizeof allow[0];
    for (int i = 0; i < n; i++) ins(BPF_JMP | BPF_JEQ | BPF_K, uint32_t(allow[i]), uint8_t(n - i), 0);
    ins(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1, 0, 0);            // EPERM
    ins(BPF_RET | BPF_K, SECCOMP_RET_ALLOW, 0, 0);
    // write: only stdout / stderr
    struct sock_fprog prog = {uint16_t(f.size()), f.data()};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog)) { perror("seccomp"); _exit(4); }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigaction(SIGSYS, &sa, 0);
    sandbox_on = true;
}
static int fx_cov_fd = -1;
static bool force_mode; static uint64_t wr_lo, wr_hi, force_limit = ~0ull;
static long fx_crash, fx_tls, fx_funcs, fx_trials, fx_same_ret, fx_same_fault, fx_timeout, fx_fail, fx_xlfail;
static std::vector<uint8_t> wr0, wrA;
// imports for -F: relocations applied; a few pure libc/libm functions are real, allocation is capped, frees are
// no-ops, everything else returns 0 (both runs see the same)
// bump arena at a fixed address, reset before every run: both runs get the same addresses and contents
static uint8_t* fx_arena; static size_t fx_used; static const size_t FX_ARENA = 64u << 20;
static size_t fx_open;   // arena bytes readable/writable; the rest is PROT_NONE (wild pointers fault the same in both runs)
static void* fx_malloc(size_t n) {
    if (!fx_arena || n > (16u << 20)) return nullptr;
    size_t a = (fx_used + 63) & ~size_t(63); if (a + n > FX_ARENA) return nullptr;
    size_t need = (a + n + 4095) & ~size_t(4095);
    if (need > fx_open) { mprotect(fx_arena + fx_open, need - fx_open, PROT_READ | PROT_WRITE); fx_open = need; }
    fx_used = a + (n ? n : 1); memset(fx_arena + a, 0xA5, n); return fx_arena + a;
}
static void* fx_calloc(size_t a, size_t b) { if (a && b > (16u << 20) / a) return nullptr; void* p = fx_malloc(a * b); if (p) memset(p, 0, a * b); return p; }
static void* fx_realloc(void*, size_t n) { return fx_malloc(n); }
static void fx_free(void*) {}
static int fx_memalign(void** p, size_t, size_t n) { *p = fx_malloc(n); return *p ? 0 : 12; }
static void* fx_aligned(size_t, size_t n) { return fx_malloc(n); }
extern "C" void fx_ret0();
__asm__(".text\n.globl fx_ret0\nfx_ret0: xor %eax, %eax\n xor %edx, %edx\n vxorps %xmm0, %xmm0, %xmm0\n ret\n");
static long fx_imports_real, fx_imports_stub;
static void apply_relocs(const uint8_t* file, uint64_t base) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)file; const Elf64_Shdr* sh = (const Elf64_Shdr*)(file + eh->e_shoff);
    const Elf64_Sym* dsym = nullptr; const char* dstr = nullptr;
    for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_DYNSYM) { dsym = (const Elf64_Sym*)(file + sh[i].sh_offset); dstr = (const char*)(file + sh[sh[i].sh_link].sh_offset); }
    static const char* real[] = {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp", "strchr", "strrchr", "memchr",
        "sin", "cos", "tan", "exp", "exp2", "log", "log2", "log10", "pow", "sqrt", "floor", "ceil", "round", "fmod", "atan", "atan2", "tanh", "sinh", "cosh", "hypot",
        "sinf", "cosf", "tanf", "expf", "exp2f", "logf", "log2f", "log10f", "powf", "sqrtf", "floorf", "ceilf", "roundf", "fmodf", "atanf", "atan2f", "tanhf", "sinhf", "coshf", "hypotf",
        "lrint", "lrintf", "llrint", "rint", "rintf", "nearbyint", "nearbyintf", "trunc", "truncf", "frexp", "ldexp", "ldexpf", "modf", "modff", "asin", "acos", "asinf", "acosf",
        "expm1", "log1p", "expm1f", "log1pf", "cbrt", "cbrtf", "fma", "fmaf", "lround", "lroundf", "sincos", "sincosf", nullptr};
    auto resolve = [&](const char* n) -> uint64_t {
        if (!strcmp(n, "malloc") || !strcmp(n, "_Znwm") || !strcmp(n, "_Znam") || !strcmp(n, "_ZnwmRKSt9nothrow_t")) return (uint64_t)fx_malloc;
        if (!strcmp(n, "_ZnwmSt11align_val_t") || !strcmp(n, "_ZnamSt11align_val_t") || !strcmp(n, "aligned_alloc")) return (uint64_t)fx_aligned;
        if (!strcmp(n, "calloc")) return (uint64_t)fx_calloc;
        if (!strcmp(n, "realloc")) return (uint64_t)fx_realloc;
        if (!strcmp(n, "posix_memalign")) return (uint64_t)fx_memalign;
        if (!strcmp(n, "free") || !strncmp(n, "_ZdlPv", 6) || !strncmp(n, "_ZdaPv", 6)) return (uint64_t)fx_free;
        for (int i = 0; real[i]; i++) if (!strcmp(n, real[i])) { void* f = dlsym(RTLD_DEFAULT, n); if (f) { fx_imports_real++; return (uint64_t)f; } }
        fx_imports_stub++; return (uint64_t)fx_ret0;
    };
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_RELA) continue;
        const Elf64_Rela* r = (const Elf64_Rela*)(file + sh[i].sh_offset); size_t nr = sh[i].sh_size / sizeof(Elf64_Rela);
        for (size_t k = 0; k < nr; k++) {
            uint32_t ty = ELF64_R_TYPE(r[k].r_info), si = ELF64_R_SYM(r[k].r_info);
            uint64_t* at = (uint64_t*)(base + r[k].r_offset);
            if (ty == R_X86_64_RELATIVE) *at = base + r[k].r_addend;
            else if ((ty == R_X86_64_JUMP_SLOT || ty == R_X86_64_GLOB_DAT || ty == R_X86_64_64) && dsym) {
                const Elf64_Sym& sy = dsym[si];
                if (sy.st_shndx != SHN_UNDEF) *at = base + sy.st_value + (ty == R_X86_64_64 ? r[k].r_addend : 0);
                else *at = resolve(dstr + sy.st_name);
            }
        }
    }
}

static void force_test(uint64_t fs, uint64_t fe) {
    static int vb = -1; if (vb < 0) vb = getenv("FX_TRACE") != nullptr;
    if (vb) { fprintf(stderr, "fn %#lx\n", (unsigned long)(fs - obj.base)); }
    std::vector<HS> st(trials);
    for (int t = 0; t < trials; t++) random_state(st[t]);
    struct Out { int kind; uint64_t faddr; int sig; HS s; };   // kind 0 returned, 1 fault, 2 timeout
    auto run = [&](HS& S, bool xl, Out& o) {
        uint64_t sp = S.gpr[RSP] - 8; S.gpr[RSP] = sp;
        uint64_t ret = xl ? tramp_exit2 : tramp_exit; memcpy((void*)sp, &ret, 8);
        if (xl) { for (int i = 0; i < 16; i++) memcpy(tst.zhi[i], S.z[i] + 32, 32); for (int i = 0; i < 16; i++) memcpy(tst.zx[i], S.z[16 + i], 64); memcpy(tst.k, S.k, 64); }
        static int arw = -1; if (arw < 0) arw = getenv("FX_ARENA_RW") != nullptr;
        if (fx_arena && fx_open) { memset(fx_arena, 0, fx_open); if (!arw) { mprotect(fx_arena, fx_open, PROT_NONE); fx_open = 0; } }   // padding too
        if (getenv("FX_TRACE")) fprintf(stderr, "  run %s\n", xl ? "translated" : "native");
        fx_tls_reset();
        g_code = fs; fault_sig = 0; fault_addr = 0; o.kind = 0; fx_used = 0;
        { struct itimerval it = {{0, 0}, {0, 50000}}; setitimer(ITIMER_REAL, &it, nullptr); }
        in_run = 1;
        static int e2 = -1; if (e2 < 0) e2 = getenv("FX_ENTER2") != nullptr;
        if (sigsetjmp(jb, 1) == 0) { if (xl && e2) h_enter2(&S); else h_enter(&S); sandbox_restore(); in_run = 0; }   // full hardware state in both runs: native callees see the same registers
        else { in_run = 0; o.kind = fault_sig == SIGALRM ? 2 : 1; o.faddr = fault_addr; o.sig = fault_sig; }
        if (getenv("FX_TRACE")) fprintf(stderr, "  done kind %d sig %d addr %#lx pc %#lx (image +%#lx)\n", o.kind, fault_sig, (unsigned long)fault_addr, (unsigned long)fault_pc, (unsigned long)(fault_pc - obj.base));
        { struct itimerval z0 = {}; setitimer(ITIMER_REAL, &z0, nullptr); }
        _mm_setcsr(g_mxcsr0); __asm__ volatile("fninit");
        if (xl && o.kind == 0) { for (int i = 0; i < 16; i++) memcpy(S.z[i] + 32, tst.zhi[i], 32); for (int i = 0; i < 16; i++) memcpy(S.z[16 + i], tst.zx[i], 64); memcpy(S.k, tst.k, 64); }
        o.s = S;
    };
    std::vector<Out> A(trials); std::vector<std::vector<uint8_t>> memNat(trials), wrNat(trials);
    std::vector<std::vector<uint8_t>> mem0v(trials), arNat(trials);
    memcpy(wr0.data(), (void*)wr_lo, wr_hi - wr_lo);
    for (int t = 0; t < trials; t++) {
        fill(mem0, DATA); mem0v[t].assign(mem0, mem0 + DATA);
        memcpy(data, mem0, DATA); memcpy((void*)wr_lo, wr0.data(), wr_hi - wr_lo);
        HS S = st[t]; run(S, false, A[t]);
        memNat[t].assign(data, data + DATA); wrNat[t].assign((uint8_t*)wr_lo, (uint8_t*)wr_hi);
        arNat[t].assign(fx_arena, fx_arena + (fx_arena ? fx_used : 0));
    }
    memcpy((void*)wr_lo, wr0.data(), wr_hi - wr_lo);
    scratch_reset();
    static int nn = -1; if (nn < 0) nn = getenv("FX_NATIVE2") != nullptr;   // control: native vs native (harness determinism)
    if (!nn && !translate_function(fs, fe, 0)) { fx_xlfail++; return; }
    fx_funcs++;
    for (int t = 0; t < trials; t++) {
        memcpy(data, mem0v[t].data(), DATA); memcpy((void*)wr_lo, wr0.data(), wr_hi - wr_lo);
        HS S = st[t]; Out B; run(S, !nn, B);
        fx_trials++;
        std::string why; char tb[200];
        const Out& a = A[t];
        if (a.kind == 2 || B.kind == 2) { fx_timeout++; continue; }
        if (a.kind != B.kind) { snprintf(tb, sizeof tb, " native %s, translated %s (sig %d addr %#lx)", a.kind ? "faulted" : "returned", B.kind ? "faulted" : "returned", B.sig, (unsigned long)B.faddr); why = tb; }
        else if (a.kind == 1) { if (a.sig != B.sig || (a.sig == SIGSEGV && a.faddr != B.faddr) || (a.sig == SIGBUS && a.faddr != B.faddr)) { /* SIGFPE/SIGILL: si_addr is the pc */ snprintf(tb, sizeof tb, " fault differs: native sig %d %#lx, translated sig %d %#lx", a.sig, (unsigned long)a.faddr, B.sig, (unsigned long)B.faddr); why = tb; } }
        else {
            static const int cmpr[] = {RAX, RDX, RBX, RBP, RSP, R12, R13, R14, R15};
            for (int r : cmpr) if (a.s.gpr[r] != B.s.gpr[r]) { snprintf(tb, sizeof tb, " gpr%d %lx/%lx", r, (unsigned long)a.s.gpr[r], (unsigned long)B.s.gpr[r]); why += tb; }
            for (int r = 0; r < 2; r++) if (memcmp(a.s.z[r], B.s.z[r], 16)) why += " xmm" + std::to_string(r);
            uint64_t sp = st[t].gpr[RSP] - 8 - (uint64_t)data;
            for (size_t i = 0; i < DATA; i++) { if (i >= sp - 32768 && i < sp + 8) continue; if (memNat[t][i] != data[i]) { snprintf(tb, sizeof tb, " mem@%+ld", (long)i - (long)sp); why += tb; break; } }
            if (fx_arena && (arNat[t].size() != fx_used || memcmp(arNat[t].data(), fx_arena, fx_used))) {
                size_t i = 0; while (i < fx_used && i < arNat[t].size() && arNat[t][i] == fx_arena[i]) i++;
                if (getenv("FX_DUMP")) { FILE* df = fopen("/tmp/claude-1000/fx-nat.bin", "wb"); fwrite(arNat[t].data(), 1, arNat[t].size(), df); fclose(df);
                    df = fopen("/tmp/claude-1000/fx-xl.bin", "wb"); fwrite(fx_arena, 1, fx_used, df); fclose(df); }
                snprintf(tb, sizeof tb, " heap(arena: used %zu/%zu, first diff +%zu: %s / %s)", arNat[t].size(), fx_used, i,
                         i < arNat[t].size() ? hex(&arNat[t][i], 16).c_str() : "-", i < fx_used ? hex(fx_arena + i, 16).c_str() : "-"); why += tb; }
            if (memcmp(wrNat[t].data(), (void*)wr_lo, wr_hi - wr_lo)) {
                for (size_t i = 0; i < wr_hi - wr_lo; i++) if (wrNat[t][i] != ((uint8_t*)wr_lo)[i]) { snprintf(tb, sizeof tb, " global@%#lx", (unsigned long)(wr_lo + i - obj.base)); why += tb; break; }
            }
        }
        if (why.empty() && a.kind == 1 && getenv("FX_FAULTS")) {
            uint64_t f = a.faddr; const char* c = f < 0x10000 ? "null/small" : (f >= (uint64_t)data && f < (uint64_t)data + DATA + 4096) ? "data" :
                (f >= img_lo && f < img_hi) ? "image" : "other";
            printf("FAULT %s sig %d %#lx fn %#lx\n", c, a.sig, (unsigned long)f, (unsigned long)(fs - obj.base)); }
        if (why.empty()) { if (a.kind == 0) fx_same_ret++; else fx_same_fault++; }
        else { fx_fail++; printf("FAIL function %#lx trial %d:%s\n", (unsigned long)(fs - obj.base), t, why.substr(0, 600).c_str()); fflush(stdout); break; }
    }
    memcpy((void*)wr_lo, wr0.data(), wr_hi - wr_lo);
}

static bool group_test(FI* fi, int n, int g0, int g1, bool int3_only) {
    (void)int3_only;
    if (replay_mode) return replay_group(fi, n, g0, g1);
    while (g1 > g0 && fi[g1 - 1].in.flow != F_NONE) g1--;        // stop before control flow
    if (g1 <= g0) { groups_skipped++; return true; }
    bool any_special = false;
    for (int i = g0; i < g1; i++) if (fi[i].flags & F_SPECIAL) any_special = true;
    if (!any_special) { groups_skipped++; return true; }
    for (int i = g0; i < g1; i++) if (fi[i].in.seg == 0x64 || fi[i].in.seg == 0x65) { groups_skipped++; return true; } // TLS accesses
    for (int i = g0; i < g1; i++) { const x86::Insn& q = fi[i].in;                    // AVX-512ER/PF (Xeon Phi only)
        if (q.enc == x86::EVEX && q.map == 2 && q.pp == 1 && ((q.opc >= 0xc6 && q.opc <= 0xcd) && q.opc != 0xcf)) { groups_skipped++; return true; } }
    uint64_t native = region_mode ? build_native_region(fi, g0, g1) : build_native(fi, g0, g1);
    if (region_mode) groups_regions++;
    GhCtx ctx; ctx.exit = tramp_exit2; ctx.entry = 0;
    gh = &ctx;
    bool ok = install_group(fi, g0, g1, false);
    gh = nullptr;
    if (!native || !ok || !ctx.entry) { groups_skipped++; return true; }
    if (dump_addr && fi[g0].addr - obj.base <= dump_addr && fi[g1 - 1].addr - obj.base >= dump_addr) {
        FILE* f = fopen("/tmp/claude-1000/group.bin", "wb"); fwrite((void*)ctx.code, 1, ctx.len, f); fclose(f);
        printf("dumped group %#lx: %d bytes, entry +%ld\n", (unsigned long)(fi[g0].addr - obj.base), ctx.len, (long)(ctx.entry - ctx.code));
    }
    if (getenv("GH_LIVE")) for (int q = g0; q < g1 + 6 && q < n; q++) printf("  %#lx flow=%d tgt=%d glive=%04x guse=%04x gdef=%04x vlive=%04x\n", (unsigned long)(fi[q].addr - obj.base), fi[q].in.flow, fi[q].target, fi[q].glive, fi[q].guse, fi[q].gdef, fi[q].vlive);
    uint16_t live = fi[g1 - 1].vlive;
    uint32_t flive = flags_live_after(fi, n, g1 - 1);
    // registers whose last write in the group is a plain AVX write: emulated upper bits may stay stale by design
    uint32_t stale_hi = 0;
    for (int i = g0; i < g1; i++) {
        int d = vec_dst(fi[i].in);
        if (fi[i].in.enc == VEX && !(fi[i].flags & F_SPECIAL)) { if (d >= 0 && d < 16 && !(fi[i].flags & F_FIXUP)) stale_hi |= 1u << d; if (d == -2) stale_hi |= 0xffff; }
        else if (fi[i].in.enc == EVEX && d >= 0 && d < 16) stale_hi &= ~(1u << d);
    }
    groups_tested++;
    bool failed = false; int done = 0;
    for (int t = 0; t < trials && !failed; t++) {
        static HS S, A, B;
        random_state(S); fill(mem0, DATA);
        A = S; memcpy(data, mem0, DATA);
        g_code = native; fault_sig = 0;
        { struct itimerval it = {{0, 0}, {0, 50000}}; setitimer(ITIMER_REAL, &it, nullptr); }   // loops: give up after 50 ms
        if (sigsetjmp(jb, 1) == 0) h_enter(&A); else { struct itimerval z = {}; setitimer(ITIMER_REAL, &z, nullptr); trials_skipped++; continue; }
        { struct itimerval z = {}; setitimer(ITIMER_REAL, &z, nullptr); }
        memcpy(memA, data, DATA);
        B = S; memcpy(data, mem0, DATA);
        for (int i = 0; i < 16; i++) memcpy(tst.zhi[i], S.z[i] + 32, 32);
        for (int i = 0; i < 16; i++) memcpy(tst.zx[i], S.z[16 + i], 64);
        memcpy(tst.k, S.k, 64);
        g_code = ctx.entry; fault_sig = 0;
        std::string why;
        { struct itimerval it = {{0, 0}, {0, 500000}}; setitimer(ITIMER_REAL, &it, nullptr); }
        if (sigsetjmp(jb, 1) == 0) h_enter2(&B); else why = " translated code faulted (sig " + std::to_string(fault_sig) + ")";
        { struct itimerval z = {}; setitimer(ITIMER_REAL, &z, nullptr); }
        if (why.empty()) {
            for (int i = 0; i < 16; i++) memcpy(B.z[i] + 32, tst.zhi[i], 32);
            for (int i = 0; i < 16; i++) memcpy(B.z[16 + i], tst.zx[i], 64);
            memcpy(B.k, tst.k, 64);
            char tb[160];
            for (int r = 0; r < 16; r++) if (r != RSP && ((fi[g1 - 1].glive >> r) & 1) && A.gpr[r] != B.gpr[r]) { snprintf(tb, sizeof tb, " gpr%d %lx/%lx", r, (unsigned long)A.gpr[r], (unsigned long)B.gpr[r]); why += tb; }
            if ((A.flags ^ B.flags) & flive) { snprintf(tb, sizeof tb, " flags %lx/%lx", (unsigned long)A.flags & 0x8d5, (unsigned long)B.flags & 0x8d5); why += tb; }
            for (int k = 0; k < 8; k++) if (A.k[k] != B.k[k]) { snprintf(tb, sizeof tb, " k%d %lx/%lx", k, (unsigned long)A.k[k], (unsigned long)B.k[k]); why += tb; }
            for (int r = 0; r < 16; r++) {
                if ((live >> r) & 1) { if (memcmp(A.z[r], B.z[r], 32)) why += " ymm" + std::to_string(r) + "[" + hex(A.z[r], 32) + " / " + hex(B.z[r], 32) + "]"; }
                bool hi_check = ((fn_rd512 | 0xff) >> r & 1) && !((stale_hi >> r) & 1) && ((fi[g1 - 1].zlive >> r) & 1);
                if (hi_check && memcmp(A.z[r] + 32, B.z[r] + 32, 32)) why += " zhi" + std::to_string(r);
            }
            for (int r = 16; r < 32; r++) {
                if (!((fi[g1 - 1].hlive >> (r - 16)) & 1)) continue;   // dead after the group: may differ
                if (memcmp(A.z[r], B.z[r], 16)) why += " xmm" + std::to_string(r);
                if ((fn_rd256 >> r & 1) && memcmp(A.z[r] + 16, B.z[r] + 16, 16)) why += " zx" + std::to_string(r) + "[16..31]";
                if ((fn_rd512 >> r & 1) && memcmp(A.z[r] + 32, B.z[r] + 32, 32)) why += " zx" + std::to_string(r) + "[32..63]";
            }
            uint64_t sp = S.gpr[RSP] - (uint64_t)data;
            for (size_t i = 0; i < DATA; i++) {
                if (i >= sp - 32768 && i < sp - 128) continue;  // emulator stack use below the red zone
                if (memA[i] != data[i]) { snprintf(tb, sizeof tb, " mem@%+ld", (long)i - (long)sp); why += tb; break; }
            }
        }
        trials_run++; done++;
        if (!why.empty()) {
            failed = true; groups_failed++;
            printf("FAIL group %#lx-%#lx (%d insns):%s\n", (unsigned long)(fi[g0].addr - obj.base), (unsigned long)(fi[g1 - 1].addr + fi[g1 - 1].in.len - obj.base), g1 - g0, why.substr(0, 600).c_str());
            if (verbose) {
                char cmd[256];
                snprintf(cmd, sizeof cmd, "objdump -d --no-show-raw-insn --start-address=%#lx --stop-address=%#lx %s | tail -n +8 | cut -f2- | sed 's/^/    /'",
                         (unsigned long)(fi[g0].addr - obj.base), (unsigned long)(fi[g1 - 1].addr + fi[g1 - 1].in.len - obj.base), obj.name);
                fflush(stdout); if (system(cmd)) {}
                for (int q = g0; q < g1; q++) printf("    %#lx flags%s%s%s def=%d vlive=%04x\n", (unsigned long)(fi[q].addr - obj.base), (fi[q].flags & F_SPECIAL) ? " SPECIAL" : "",
                    (fi[q].flags & F_FIXUP) ? " FIXUP" : "", (fi[q].flags & F_ENTRY) ? " ENTRY" : "", fi[q].def, fi[q].vlive);
                printf("    fn_rd512=%08x stale_hi=%04x\n", fn_rd512, stale_hi);
                FILE* df = fopen("/tmp/claude-1000/fail.bin", "wb"); fwrite((void*)ctx.code, 1, ctx.len, df); fclose(df);
            }
        }
    }
    return true;
}

int main(int argc, char** argv) {
    const char* path = argv[1]; if (getenv("GH_SEED")) { rng ^= strtoull(getenv("GH_SEED"), 0, 10) * 0xD1B54A32D192ED03ull; if (!rng) rng = 1; } int maxf = 1 << 30, skip = 0, stride = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f")) maxf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-k")) stride = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-t")) time_only = 1;
        else if (!strcmp(argv[i], "-d")) dump_addr = strtoull(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "-a")) only_addr = strtoull(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "-g")) guard_mode = true;
        else if (!strcmp(argv[i], "-F")) force_mode = force_mode_flag = true;
        else if (!strcmp(argv[i], "-L")) force_limit = strtoull(argv[++i], 0, 16);
        else if (!strcmp(argv[i], "-r")) { replay_mode = true; if (!load_caps(argv[++i])) { fprintf(stderr, "cannot read %s\n", argv[i]); return 2; } }
    }
    if (replay_mode && !cap_base) { printf("replay: no records\n"); return 0; }
    int fd = open(path, O_RDONLY); struct stat st; fstat(fd, &st);
    uint8_t* file = (uint8_t*)mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr* eh = (Elf64_Ehdr*)file; Elf64_Phdr* ph = (Elf64_Phdr*)(file + eh->e_phoff);
    uint64_t span = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span) span = ph[i].p_vaddr + ph[i].p_memsz;
    span = (span + 0xfff) & ~0xfffull;
    uint8_t* base = (uint8_t*)mmap((void*)(replay_mode ? cap_base : 0x200000000ull), span, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED || (replay_mode && (uint64_t)base != cap_base)) { printf("replay: base %#lx busy in this process\n", (unsigned long)cap_base); return 3; }
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) memcpy(base + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
    img_lo = (uint64_t)base; img_hi = (uint64_t)base + span;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) { text_lo = (uint64_t)base + ph[i].p_vaddr; text_hi = text_lo + ph[i].p_memsz; }
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W)) {
        uint64_t lo = ((uint64_t)base + ph[i].p_vaddr) & ~0xfffull, hi = ((uint64_t)base + ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffull;
        if (!wr_lo || lo < wr_lo) wr_lo = lo; if (hi > wr_hi) wr_hi = hi; }
    if (force_mode && getenv("FX_COVOUT")) { fx_cov_fd = open(getenv("FX_COVOUT"), O_WRONLY | O_CREAT | O_APPEND, 0644); setenv("AVX512EMU_COUNT", "1", 1); }
    if (force_mode) { in_run = 0; wr0.resize(wr_hi - wr_lo); apply_relocs(file, (uint64_t)base);
        // real segment protections (code r-x, constants r--): wild stores must not change code or constants for later
        // runs; the emulator patches code through poke() (mprotect fallback)
        for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) {
            uint64_t lo = ((uint64_t)base + ph[i].p_vaddr) & ~0xfffull, hi = ((uint64_t)base + ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffull;
            int pr = PROT_READ | ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) | ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
            if (!(ph[i].p_flags & PF_W)) mprotect((void*)lo, hi - lo, pr); }
        fx_arena = (uint8_t*)mmap((void*)0x3e0000000ull, FX_ARENA, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (fx_arena == MAP_FAILED) fx_arena = nullptr; }
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && !(ph[i].p_flags & PF_W)) ro_add((uint64_t)base + ph[i].p_vaddr, (uint64_t)base + ph[i].p_vaddr + ph[i].p_filesz);
    memset(&obj, 0, sizeof obj); obj.base = (uint64_t)base; snprintf(obj.name, sizeof obj.name, "%s", path);
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_GNU_EH_FRAME) obj.eh_frame_hdr = (uint64_t)base + ph[i].p_vaddr;
    data = (uint8_t*)mmap((void*)0x3f0000000ull, DATA + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (guard_mode) mprotect(data + DATA, 4096, PROT_NONE);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGALRM, &sa, 0);
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    group_hook = force_mode ? nullptr : group_test;
    if (force_mode || replay_mode) sandbox_install();
    tramp_exit = make_tramp((uint64_t)base, (uint64_t)h_exit); tramp_exit2 = make_tramp((uint64_t)base, (uint64_t)h_exit2);
    const uint8_t* h = (const uint8_t*)obj.eh_frame_hdr; const int32_t* tab = (const int32_t*)(h + 12);
    uint32_t count; memcpy(&count, h + 8, 4);
    int funcs = 0;
    for (uint32_t i = skip; i < count && funcs < maxf; i += stride) {
        uint64_t ip = obj.eh_frame_hdr + int64_t(tab[2 * i]), fs, fe;
        if (!find_function(obj, ip, fs, fe)) continue;
        if (only_addr && !(fs - obj.base <= only_addr && fe - obj.base > only_addr)) continue;
        if (force_mode) { if (fs - obj.base >= force_limit) continue;
            bool has = false;
            bool tls = false;
            for (uint64_t a = fs; a < fe;) { x86::Insn in; int l = x86::decode((const uint8_t*)a, int(fe - a < 15 ? fe - a : 15), in); if (!l) break; if (is_special(in)) has = true;
                if ((in.seg == 0x64 || in.seg == 0x65) && !(in.mod == 0 && in.has_sib == false && mem_disp(in) == 0x28)) tls = true; a += l; }
            if (tls) fx_tls++;   // tested: Live TLS accesses land in fx_tls_pad
            if (has) {
                // one child process per function: TLS writes of Live's code into the harness's TLS, wild stores and
                // patches stay in the child; it reports its counters through a pipe
                fflush(stdout); fflush(stderr);
                int pf[2]; if (pipe(pf)) { perror("pipe"); return 5; }
                pid_t pid = fork();
                if (pid == 0) {
                    close(pf[0]); fx_funcs = fx_xlfail = fx_trials = fx_same_ret = fx_same_fault = fx_timeout = fx_fail = 0; force_test(fs, fe);
                    if (fx_cov_fd >= 0) for (uint32_t i = 0; i < ngcounts; i++) if (gcounts[i].cnt && *gcounts[i].cnt)   // groups the translated runs entered
                        dprintf(fx_cov_fd, "%lx %lx %lu\n", (unsigned long)(gcounts[i].orig - obj.base), (unsigned long)(gcounts[i].orig_end - obj.base), (unsigned long)*gcounts[i].cnt); fflush(stdout); fflush(stderr);
                    long c[7] = {fx_funcs, fx_xlfail, fx_trials, fx_same_ret, fx_same_fault, fx_timeout, fx_fail};
                    if (write(pf[1], c, sizeof c) < 0) {} _exit(0);
                }
                close(pf[1]);
                long c[7] = {}; size_t got = 0;
                for (;;) { ssize_t r = read(pf[0], (char*)c + got, sizeof c - got); if (r <= 0) break; got += size_t(r); }
                close(pf[0]);
                int stt = 0; waitpid(pid, &stt, 0);
                if (got == sizeof c) { fx_funcs += c[0]; fx_xlfail += c[1]; fx_trials += c[2]; fx_same_ret += c[3]; fx_same_fault += c[4]; fx_timeout += c[5]; fx_fail += c[6]; }
                else { fx_crash++; if (getenv("FX_TRACE")) fprintf(stderr, "child crashed: fn %#lx status %#x\n", (unsigned long)(fs - obj.base), stt); }
                funcs++; if (funcs % 500 == 0) fprintf(stderr, "functions %d same-return %ld same-fault %ld fail %ld\n", funcs, fx_same_ret, fx_same_fault, fx_fail); }
            continue; }
        if (replay_mode) { auto lb = caps.lower_bound(fs - obj.base); if (lb == caps.end() || lb->first >= fe - obj.base) continue; }
        // only functions with AVX-512 / opmask code
        bool has = false;
        for (uint64_t a = fs; a < fe;) { x86::Insn in; int l = x86::decode((const uint8_t*)a, int(fe - a < 15 ? fe - a : 15), in); if (!l) break; if (is_special(in)) { has = true; break; } a += l; }
        if (!has) continue;
        scratch_reset();
        if (time_only) {
            GhCtx ctx; ctx.exit = tramp_exit2; gh = &ctx; group_hook = nullptr;
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            translate_function(fs, fe, 0);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            gh = nullptr;
            double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
            if (ms > 1 || only_addr) printf("%.2f ms function %#lx\n", ms, (unsigned long)(fs - obj.base));
        } else
        translate_function(fs, fe, 0);
        funcs++;
        if (funcs % 500 == 0) { fprintf(stderr, "functions %d groups %ld failed %ld\n", funcs, groups_tested, groups_failed); }
    }
    if (force_mode) {
        printf("forced: imports real %ld, stubbed %ld; functions with TLS access (tested, fx_tls_pad) %ld; child crashed (harness state destroyed by the function) %ld\n", fx_imports_real, fx_imports_stub, fx_tls, fx_crash);
        printf("forced: functions %ld (translation failed %ld), trials %ld: same return %ld, same fault %ld, timeout %ld, FAIL %ld\n",
               fx_funcs, fx_xlfail, fx_trials, fx_same_ret, fx_same_fault, fx_timeout, fx_fail);
        return 0;
    }
    if (replay_mode) {
        long unmatched = 0; for (auto& c : caps) if (!rp_done.count(c.first)) unmatched += long(c.second.size());
        printf("replay: records %ld, groups with records %zu; replayed %ld, failed groups %ld; skipped: address conflict %ld, native fault (window too small) %ld, group not formed here %ld; translated faults %ld\n",
               rp_records, caps.size(), rp_matched, rp_failed_groups, rp_conflict, rp_native_fault, unmatched, rp_trans_fault);
        return 0;
    }
    printf("functions %d groups tested %ld failed %ld skipped %ld; trials %ld (native faults skipped %ld)\n", funcs, groups_tested, groups_failed, groups_skipped, trials_run, trials_skipped);
    printf("interpreter sites built: %lu (both passes); regions tested %ld\n", (unsigned long)emu::g_interp_sites, groups_regions);
}
