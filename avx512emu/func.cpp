// Function-level translation: decode the whole function around a faulting
// AVX-512 instruction, find every AVX-512/opmask instruction in it, work out
// which AVX2 writes must also clear the emulated zmm upper half, group short
// instructions with neighbours so each group can take a 5-byte jump, emit the
// translated groups and patch the original code.
#include "rt.h"
namespace emu { void cov_note(uint64_t fstart); }   // runtime.cpp: AVX512EMU_COVLIST
#include "xlate.h"
#include "evexinfo.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/mman.h>

namespace emu {
using namespace x86;

enum : uint8_t { F_ENTRY = 1, F_SPECIAL = 2, F_FIXUP = 4, F_COVERED = 8, F_BAD = 16 };

struct FI {
    uint64_t addr;
    Insn in;
    uint8_t flags;
    uint16_t vuse, vdef, vlive;  // ymm0-15: read, fully written, live after (out)
    uint16_t guse, gdef, glive;  // GPRs: read (over-approximated), fully written, live after (out)
    uint16_t huse, hdef, hlive;  // zmm16-31 (bit r-16): read (over-approximated), fully written, live after
    uint16_t zlive;              // zmm0-15 bits 256..511 read later (before being redefined): live after
    int16_t def;        // vector register written (0..15), -1 none, -2 all
    uint32_t rd512;     // registers whose upper half is read
    int32_t target;     // index of direct branch target inside the function, -1 none
};

static int find_index(FI* fi, int n, uint64_t a) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (fi[m].addr == a) return m; if (fi[m].addr < a) lo = m + 1; else hi = m - 1; }
    return -1;
}

static uint64_t branch_target(const FI& f) {
    int64_t rel = f.in.rel_size == 1 ? (int8_t)((const uint8_t*)f.addr)[f.in.imm_off] : f.in.imm;
    if (f.in.rel_size == 4) { int32_t r; memcpy(&r, (const uint8_t*)f.addr + f.in.imm_off, 4); rel = r; }
    return f.addr + f.in.len + rel;
}

static uint8_t* genbuf; static size_t gencap;   // generation buffer (translate_group)
// ---- profiling: every Desc, top entries by interpreter executions
static Desc** all_descs;
static uint64_t n_descs;
static void register_desc(Desc* d) {
    if (!all_descs) all_descs = (Desc**)mmap(nullptr, sizeof(Desc*) << 22, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (n_descs < (1u << 22)) all_descs[n_descs++] = d;
}
void profile_tick() {
    if (log_level < 2 || !all_descs) return;
    static volatile int busy;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;
    Desc* top[40] = {}; uint64_t n = n_descs;
    for (uint64_t i = 0; i < n; i++) {
        Desc* d = all_descs[i];
        if (!d->count) continue;
        int j = 39; if (top[j] && top[j]->count >= d->count) continue;
        while (j > 0 && (!top[j - 1] || top[j - 1]->count < d->count)) { top[j] = top[j - 1]; j--; }
        top[j] = d;
    }
    prof_report();
    elog("avx512emu[%d]: hottest interpreted instructions:\n", getpid());
    for (int j = 0; j < 40 && top[j]; j++) {
        ObjInfo o; bool f = find_object(top[j]->pc, o);
        char hx[48]; int k = 0; for (int b = 0; b < top[j]->in.len && k < 44; b++) k += snprintf(hx + k, sizeof hx - k, "%02x", top[j]->bytes[b]);
        elog("  %12lu %s+%#lx %s\n", (unsigned long)top[j]->count, f ? (o.name[0] ? strrchr(o.name, '/') + 1 : "main") : "?", (unsigned long)(top[j]->pc - (f ? o.base : 0)), hx);
    }
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

// ---- local EFLAGS liveness (status flags only), conservative
enum { FL_ALL = 0x8d5, FL_CF = 1, FL_ZF = 0x40 };
// Instruction writes all status flags without reading any.
static bool flag_full_writer(const Insn& in) {
    if (in.enc == VEX) return in.map == 1 && (in.opc == 0x2e || in.opc == 0x2f) && in.pp <= 1;  // vucomis/vcomis
    if (in.enc != LEGACY) return in.enc == EVEX && in.map == 1 && (in.opc == 0x2e || in.opc == 0x2f) && in.pp <= 1;
    if (in.map == 0) {
        uint8_t o = in.opc;
        if (o <= 0x3d && (o & 7) <= 5) { int grp = o >> 3; return grp != 2 && grp != 3; }   // add or and sub xor cmp (not adc sbb)
        if (o == 0x84 || o == 0x85 || o == 0xa8 || o == 0xa9) return true;                    // test
        if (o == 0x80 || o == 0x81 || o == 0x83) return in.reg3 != 2 && in.reg3 != 3;
        if (o == 0xf6 || o == 0xf7) return in.reg3 == 0 || in.reg3 == 1 || in.reg3 == 3;      // test, neg
        if (o == 0x69 || o == 0x6b) return true;                                              // imul
        return false;
    }
    if (in.map == 1) {
        if ((in.opc == 0x2e || in.opc == 0x2f) && in.rep == 0) return true;                   // ucomis/comis
        if (in.opc == 0xaf) return true;                                                      // imul
        if (in.rep == 0xf3 && (in.opc == 0xb8 || in.opc == 0xbc || in.opc == 0xbd)) return true; // popcnt tzcnt lzcnt
    }
    return false;
}
static uint32_t jcc_reads(const Insn& in) {
    int cc = in.map == 0 ? (in.opc - 0x70) : (in.opc - 0x80);
    static const uint32_t r[8] = {0x800, FL_CF, FL_ZF, FL_CF | FL_ZF, 0x80, 0x4, 0x880, 0x8c0};
    return r[(cc >> 1) & 7];
}
// Instruction neither reads nor writes status flags.
static bool flag_neutral(const Insn& in) {
    if (in.enc == EVEX) return !(in.map == 1 && (in.opc == 0x2e || in.opc == 0x2f));
    if (in.enc == VEX) {
        if (in.map == 1) return !(in.opc == 0x2e || in.opc == 0x2f || in.opc == 0x98 || in.opc == 0x99);
        if (in.map == 2) return !(in.opc == 0xf2 || in.opc == 0xf3 || in.opc == 0xf5 || in.opc == 0xf7 || in.opc == 0x17 || in.opc == 0x0e || in.opc == 0x0f);
        return !(in.opc >= 0x60 && in.opc <= 0x63);
    }
    if (in.map == 0) {
        uint8_t o = in.opc;
        return (o >= 0x88 && o <= 0x8b) || o == 0x8d || o == 0xc6 || o == 0xc7 || (o >= 0xb0 && o <= 0xbf) ||
               (o >= 0x50 && o <= 0x5f) || o == 0x90 || o == 0x63 || o == 0x98 || o == 0x99;
    }
    if (in.map == 1) {
        uint8_t o = in.opc;
        if (o == 0x2e || o == 0x2f) return false;
        return (o >= 0x10 && o <= 0x2d) || (o >= 0x50 && o <= 0x7f) || (o >= 0xc2 && o <= 0xc6) || o >= 0xd0 ||
               o == 0xb6 || o == 0xb7 || o == 0xbe || o == 0xbf || o == 0x0d;
    }
    if (in.map == 2) return true;
    return !(in.opc >= 0x60 && in.opc <= 0x63);
}
static uint32_t flags_needed_from(FI* fi, int n, int j, int depth) {
    // long straight runs (k logic early in a 400-instruction block, first flag writer at its end) need a
    // long scan: at the limit everything counts as live and k ops take the slow vector path
    for (int steps = 0; steps < 1024 && j >= 0 && j < n; steps++) {
        const FI& f = fi[j];
        if (flag_full_writer(f.in) || f.in.flow == F_CALL || f.in.flow == F_RET || (f.in.flow == F_INDIRECT && f.in.reg3 == 2)) return 0; // flags are not preserved across calls/returns
        if (f.in.enc == VEX && f.in.map == 1 && (f.in.opc == 0x98 || f.in.opc == 0x99)) return 0; // kortest/ktest write all
        if (f.in.flow == F_JCC && !(f.in.map == 0 && f.in.opc >= 0xe0)) {
            if (depth <= 0) return FL_ALL;
            return jcc_reads(f.in) | flags_needed_from(fi, n, j + 1, depth - 1) | (f.target >= 0 ? flags_needed_from(fi, n, f.target, depth - 1) : FL_ALL);
        }
        if (f.in.flow == F_JMP) { if (f.target < 0 || depth <= 0) return FL_ALL; j = f.target; depth--; continue; }
        if (f.in.flow != F_NONE || !flag_neutral(f.in)) return FL_ALL;
        j++;
    }
    return FL_ALL;
}
// Status flags that may be read after instruction i (which falls through).
static uint32_t flags_live_after(FI* fi, int n, int i) { return flags_needed_from(fi, n, i + 1, 3); }

// ---- vector register liveness (ymm0-15, 256-bit granularity), conservative:
// every register an instruction names counts as read; only VEX/EVEX writes
// (which replace the whole register) count as writes; calls, syscalls,
// unknown control flow and leaving the function keep everything live except
// that ret keeps only xmm0/xmm1.
static uint16_t named_vregs(const Insn& in) {
    uint16_t m = 0;
    if (in.map == 0 && in.enc == LEGACY) return 0;   // one-byte opcodes name no xmm register
    if (in.has_modrm) {
        m |= 1u << (reg_of(in) & 15);
        if (in.mod == 3) m |= 1u << (rm_reg_of(in) & 15);
        if (in.has_sib && in.mod != 3) m |= 1u << (index_of(in) & 15); // VSIB (over-approximation for GPR index)
    }
    if (in.enc != LEGACY) m |= 1u << (in.vvvv & 15);
    if (in.enc == LEGACY) m |= 1u;                   // implicit xmm0 (blendv, pcmpestr)
    if (in.enc == VEX && in.map == 3 && in.imm_size) m |= 1u << ((in.imm8 >> 4) & 15); // 4th register operand (blendv)
    return m;
}
// ---- GPR liveness, conservative: exact use/def only for common simple instructions; anything
// else reads every GPR. Calls read everything. At ret only rsi, rdi, r9, r10, r11 are dead
// (LLVM may return values in rax, rdx, rcx, r8; callee-saved registers belong to the caller).
static bool call_abi = true;      // AVX512EMU_CALLABI=0: calls read and keep every register (no ABI assumption)
static bool is_call(const FI& f) { return f.in.flow == F_CALL || (f.in.flow == F_INDIRECT && f.in.reg3 == 2); }
static const uint16_t G_ALL = 0xffff;
static void gpr_usedef(const Insn& in, uint16_t& use, uint16_t& def) {
    use = 1u << 4; def = 0;                                   // rsp
    if (in.has_modrm && in.mod != 3) { if (has_base(in)) use |= 1u << base_of(in); if (has_index(in)) use |= 1u << index_of(in); }
    int reg = reg_of(in) & 15, rm = rm_reg_of(in) & 15;
    uint16_t R = 1u << reg, M = in.mod == 3 ? uint16_t(1u << rm) : 0;
    bool full = in.W || !in.opsize;                           // 32/64-bit write (zero-extends): full
    if (in.enc != LEGACY) {                                   // VEX/EVEX: any field may name a GPR
        if (in.has_modrm) use |= R | M;
        use |= 1u << (in.vvvv & 15);
        if (in.enc == VEX && in.map == 2 && in.opc == 0xf6) use |= 1u << 2;   // mulx reads rdx
        return;
    }
    if (in.map == 0) {
        uint8_t o = in.opc;
        if (o < 0x40 && (o & 7) <= 3) {                       // ALU r/m forms
            int op = o >> 3; bool byte = !(o & 1), todst_reg = o & 2;
            if (op == 6 && !byte && in.mod == 3 && reg == rm && (o == 0x31 || o == 0x33)) { if (full) def = R; else use |= R; return; } // xor r,r
            use |= R | M; return;
        }
        if (o < 0x40 && ((o & 7) == 4 || (o & 7) == 5)) { use |= 1u; return; }                 // op al/eax, imm
        if (o >= 0x50 && o <= 0x57) { use |= 1u << ((o & 7) | (in.B << 3)); return; }          // push r
        if (o >= 0x58 && o <= 0x5f) { def = 1u << ((o & 7) | (in.B << 3)); return; }           // pop r
        if (o == 0x63) { use |= M; if (in.W) def = R; else use |= R; return; }                  // movsxd
        if (o == 0x68 || o == 0x6a || (o >= 0x70 && o <= 0x7f) || o == 0x90 || o == 0x9c || o == 0x9d) return;
        if (o == 0x69 || o == 0x6b) { use |= M; if (full) def = R; else use |= R; return; }     // imul r, r/m, imm
        if (o >= 0x80 && o <= 0x83) { use |= M; return; }
        if (o == 0x84 || o == 0x85) { use |= R | M; return; }
        if (o == 0x86 || o == 0x87) { use |= R | M; return; }
        if (o == 0x88) { use |= R | M; return; }
        if (o == 0x89) { use |= R; if (in.mod == 3) { if (full) def = M; else use |= M; } return; }
        if (o == 0x8a) { use |= R | M; return; }
        if (o == 0x8b) { use |= M; if (full) def = R; else use |= R; return; }
        if (o == 0x8d) { if (full) def = R; else use |= R; return; }                             // lea: address only
        if (o >= 0x91 && o <= 0x97) { use |= 1u | (1u << ((o & 7) | (in.B << 3))); return; }   // xchg rax, r
        if (o == 0x98) { use |= 1u; return; }
        if (o == 0x99) { use |= 1u; if (full) def = 1u << 2; else use |= 1u << 2; return; }      // cdq/cqo
        if (o == 0xa8 || o == 0xa9) { use |= 1u; return; }
        if (o >= 0xb0 && o <= 0xb7) { use |= 1u << ((o & 7) | (in.B << 3)); return; }          // mov r8, imm
        if (o >= 0xb8 && o <= 0xbf) { uint16_t r = 1u << ((o & 7) | (in.B << 3)); if (full) def = r; else use |= r; return; }
        if (o == 0xc0 || o == 0xc1 || o == 0xd0 || o == 0xd1) { use |= M; return; }
        if (o == 0xd2 || o == 0xd3) { use |= M | (1u << 1); return; }
        if (o == 0xc6) { use |= M; return; }
        if (o == 0xc7) { if (in.mod == 3) { if (full) def = M; else use |= M; } return; }
        if (o == 0xc9) { use |= 1u << 5; return; }                                              // leave
        if (o == 0xf6 || o == 0xf7) { int e = in.reg3; use |= M; if (e >= 4) use |= 1u | (1u << 2); return; }
        if (o == 0xfe || o == 0xff) { int e = in.reg3; if (e <= 1 || e == 6) { use |= M; return; } }
    }
    if (in.map == 1 && in.enc == LEGACY) {
        uint8_t o = in.opc;
        if (o == 0x1f || o == 0x18 || o == 0x0d || (o >= 0x80 && o <= 0x8f)) return;          // nop, prefetch, jcc
        if (o >= 0x40 && o <= 0x4f) { use |= R | M; return; }                                   // cmov
        if (o >= 0x90 && o <= 0x9f) { use |= M; return; }                                       // setcc
        if (o == 0xb6 || o == 0xb7 || o == 0xbe || o == 0xbf) { use |= M; if (full) def = R; else use |= R; return; }
        if (o == 0xaf || o == 0xa3 || o == 0xab || o == 0xb3 || o == 0xbb || o == 0xbc || o == 0xbd || o == 0xb8 || o == 0xc0 || o == 0xc1) { use |= R | M; return; }
        if (o == 0xa4 || o == 0xac || o == 0xba) { use |= R | M; return; }
        if (o == 0xa5 || o == 0xad) { use |= R | M | (1u << 1); return; }
        if (o >= 0xc8 && o <= 0xcf) { use |= 1u << ((o & 7) | (in.B << 3)); return; }          // bswap
        if (o == 0xb0 || o == 0xb1) { use |= R | M | 1u; return; }                              // cmpxchg
        // SSE (vector registers; some forms name a GPR in reg or rm)
        if ((o >= 0x10 && o <= 0x17) || (o >= 0x28 && o <= 0x2f) || (o >= 0x50 && o <= 0x7f) || o == 0xc2 || (o >= 0xc4 && o <= 0xc6) || o >= 0xd0) { use |= R | M; return; }
    }
    use = G_ALL;                                              // unknown: reads everything
}
static uint16_t g_ret_live = G_ALL & ~((1u << 6) | (1u << 7) | (1u << 9) | (1u << 10) | (1u << 11));
static void gliveness(FI* fi, int n) {
    static bool init; if (!init) { init = true; const char* e = getenv("AVX512EMU_GRETLIVE"); if (e) g_ret_live = (uint16_t)strtoul(e, nullptr, 0); }
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        gpr_usedef(f.in, f.guse, f.gdef);
        if (f.in.flow == F_CALL || f.in.flow == F_INDIRECT || f.in.flow == F_STOP) { f.guse = G_ALL; f.gdef = 0; }
        if (call_abi && is_call(f)) {   // SysV: rdi rsi rdx rcx r8 r9 (and al for varargs) in, caller-saved clobbered
            uint16_t u = (1u << 7) | (1u << 6) | (1u << 2) | (1u << 1) | (1u << 8) | (1u << 9) | 1u | (1u << 4);
            if (f.in.has_modrm) { if (f.in.mod == 3) u |= 1u << (rm_reg_of(f.in) & 15); else { if (has_base(f.in)) u |= 1u << base_of(f.in); if (has_index(f.in)) u |= 1u << index_of(f.in); } }
            f.guse = u; f.gdef = uint16_t(1u | (1u << 1) | (1u << 2) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));
        }
        else if (f.in.flow == F_RET || f.in.flow == F_JMP || f.in.flow == F_JCC) {           // direct branches, ret
            f.guse = 1u << 4; f.gdef = 0;
            if (f.in.map == 0 && f.in.opc >= 0xe0 && f.in.opc <= 0xe3) f.guse |= 1u << 1;    // loop*/jrcxz read rcx
        }
        f.glive = 0;
    }
    bool changed = true; int rounds = 0;
    while (changed && rounds < 200) {
        changed = false; rounds++;
        for (int i = n - 1; i >= 0; i--) {
            FI& f = fi[i];
            uint16_t out;
            auto in_of = [&](int j) -> uint16_t { return j >= 0 && j < n ? uint16_t(fi[j].guse | (fi[j].glive & ~fi[j].gdef)) : G_ALL; };
            switch (f.in.flow) {
            case F_NONE: case F_CALL: out = in_of(i + 1); break;
            case F_JCC: out = in_of(i + 1) | (f.target >= 0 ? in_of(f.target) : G_ALL); break;
            case F_JMP: out = f.target >= 0 ? in_of(f.target) : G_ALL; break;
            case F_RET: out = g_ret_live; break;
            default: out = (call_abi && is_call(f)) ? in_of(i + 1) : G_ALL; break;
            }
            out |= f.glive;
            if (out != f.glive) { f.glive = out; changed = true; }
        }
    }
    if (changed) for (int i = 0; i < n; i++) fi[i].glive = G_ALL;
}
// Upper halves (bits 256..511) of zmm0-15: read by 512-bit EVEX reads, by calls (zmm0-7 may carry
// arguments) and at ret (zmm0-3 may carry results); written by every EVEX/VEX write of the register.
// An EVEX write whose upper half is dead needs no zeroing (zero_tail).
static bool group_zlive = true;    // AVX512EMU_ZLIVE=0: zero always (zmm0-7 and function-wide 512-bit reads)
static void zliveness(FI* fi, int n) {
    uint16_t* use = (uint16_t*)scratch_alloc(sizeof(uint16_t) * n); uint16_t* def = (uint16_t*)scratch_alloc(sizeof(uint16_t) * n);
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        use[i] = uint16_t(f.rd512); def[i] = 0;
        int d = f.def;
        if (f.in.enc != LEGACY) { if (d == -2) def[i] = 0xffff; else if (d >= 0 && d < 16) def[i] = uint16_t(1u << d); }
        if (f.in.flow == F_CALL) { use[i] |= 0x00ff; def[i] = 0; }
        if (call_abi && is_call(f)) { use[i] = 0x00ff; def[i] = 0xffff; }
        if (f.in.flow == F_INDIRECT || f.in.flow == F_STOP) { use[i] = 0xffff; def[i] = 0; }
        f.zlive = 0;
    }
    bool changed = true; int rounds = 0;
    while (changed && rounds < 200) {
        changed = false; rounds++;
        for (int i = n - 1; i >= 0; i--) {
            FI& f = fi[i];
            uint16_t out;
            auto in_of = [&](int j) -> uint16_t { return j >= 0 && j < n ? uint16_t(use[j] | (fi[j].zlive & ~def[j])) : 0xffff; };
            switch (f.in.flow) {
            case F_NONE: case F_CALL: out = in_of(i + 1); break;
            case F_JCC: out = in_of(i + 1) | (f.target >= 0 ? in_of(f.target) : 0xffff); break;
            case F_JMP: out = f.target >= 0 ? in_of(f.target) : 0xffff; break;
            case F_RET: out = 0x000f; break;
            default: out = (call_abi && is_call(f)) ? in_of(i + 1) : 0xffff; break;
            }
            out |= f.zlive;
            if (out != f.zlive) { f.zlive = out; changed = true; }
        }
    }
    if (changed) for (int i = 0; i < n; i++) fi[i].zlive = 0xffff;
}
static uint16_t ret_live = 0x000f; // at ret only xmm0-3 may carry values (LLVM returns vectors in up to 4); AVX512EMU_RETLIVE=0xffff: all
static void vliveness(FI* fi, int n) {
    static bool init; if (!init) { init = true; const char* e = getenv("AVX512EMU_RETLIVE"); if (e) ret_live = (uint16_t)strtoul(e, nullptr, 0); }
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        f.vuse = named_vregs(f.in);
        f.vdef = 0;
        if (f.in.enc != LEGACY && !(f.in.enc == VEX && is_special(f.in))) {
            int d = vec_dst(f.in);
            if (d == -2) { f.vdef = 0xffff; f.vuse = 0; }          // vzeroupper/vzeroall: treat as full write (upper halves)
            else if (d >= 0 && d < 16) f.vdef = 1u << d;
        }
        if (f.in.enc == VEX && f.in.map == 1 && f.in.opc == 0x77) { f.vdef = f.in.L ? 0xffff : 0; f.vuse = f.in.L ? 0 : 0xffff; }
        if (f.in.flow == F_CALL || f.in.flow == F_INDIRECT) f.vuse = 0xffff;
        if (call_abi && is_call(f)) { f.vuse = 0x00ff; f.vdef = 0xffff; }   // SysV: args in xmm0-7, all vector registers clobbered
        f.vlive = 0;                          // least fixpoint: grows to the live set
    }
    // backward fixpoint
    bool changed = true; int rounds = 0;
    while (changed && rounds < 200) {
        changed = false; rounds++;
        for (int i = n - 1; i >= 0; i--) {
            FI& f = fi[i];
            uint16_t out;
            auto in_of = [&](int j) -> uint16_t { return j >= 0 && j < n ? uint16_t(fi[j].vuse | (fi[j].vlive & ~fi[j].vdef)) : 0xffff; };
            switch (f.in.flow) {
            case F_NONE: out = in_of(i + 1); break;
            case F_JCC: out = in_of(i + 1) | (f.target >= 0 ? in_of(f.target) : 0xffff); break;
            case F_JMP: out = f.target >= 0 ? in_of(f.target) : 0xffff; break;
            case F_RET: out = ret_live; break;
            default: out = (call_abi && is_call(f)) ? in_of(i + 1) : 0xffff; break;           // calls (ABI: to the return point), indirect, stops
            }
            out |= f.vlive;                   // monotone
            if (out != f.vlive) { f.vlive = out; changed = true; }
        }
    }
    if (changed) for (int i = 0; i < n; i++) fi[i].vlive = 0xffff;   // no fixpoint: give up
}

// zhi dataflow: every AVX2 (VEX) write of a register whose stale emulated
// upper half may later be read as 512 bits becomes a fixup site.
static void mark_fixups(FI* fi, int n) {
    uint32_t any512 = 0;
    for (int i = 0; i < n; i++) any512 |= fi[i].rd512;
    if (!any512) return;
    // per VEX def of r (r in any512), search forward until r is redefined
    int* stack = (int*)scratch_alloc(sizeof(int) * (n + 1));
    uint32_t* seen = (uint32_t*)scratch_alloc(sizeof(uint32_t) * n);
    uint32_t epoch = 0;
    memset(seen, 0, sizeof(uint32_t) * n);
    for (int d = 0; d < n; d++) {
        if (fi[d].in.enc != VEX || is_special(fi[d].in)) continue;
        int dr = fi[d].def;
        if (dr == -1) continue;
        uint32_t regs = dr == -2 ? any512 : (dr < 16 ? (1u << dr) & any512 : 0);
        if (!regs) continue;
        // search each register separately (cheap: usually short)
        for (int r = 0; r < 16; r++) {
            if (!(regs & (1u << r))) continue;
            epoch++;
            int sp = 0;
            auto push = [&](int j) { if (j >= 0 && j < n && seen[j] != epoch) { seen[j] = epoch; stack[sp++] = j; } };
            const FI& f0 = fi[d];
            if (f0.in.flow == F_NONE || f0.in.flow == F_CALL || f0.in.flow == F_JCC) push(d + 1);
            bool hit = false;
            while (sp && !hit) {
                int j = stack[--sp];
                const FI& f = fi[j];
                if (f.rd512 & (1u << r)) { hit = true; break; }
                bool kill = f.in.flow == F_CALL || f.def == r || (f.def == -2 && r < 16);
                if (f.in.enc == EVEX && f.def == r) kill = true;
                if (kill) continue;
                switch (f.in.flow) {
                case F_NONE: push(j + 1); break;
                case F_JCC: push(j + 1); if (f.target >= 0) push(f.target); break;
                case F_JMP: if (f.target >= 0) push(f.target); break;
                default: break;
                }
            }
            if (hit) { fi[d].flags |= F_FIXUP; stats.fixups++; break; }
        }
    }
}

static bool relocatable(const FI& f) {
    const Insn& in = f.in;
    if (in.flow == F_STOP) return false;
    if (in.flow == F_INDIRECT) {
        // jmp/call through memory or register: fine unless it uses rsp (call pushes first)
        if (in.reg3 == 2 || in.reg3 == 3) { // call
            if (in.mod == 3) return (in.rm3 | (in.B << 3)) != RSP;
            if (has_base(in) && base_of(in) == RSP) return false;
            if (has_index(in) && index_of(in) == RSP) return false;
        }
        return in.reg3 != 3 && in.reg3 != 5; // no far forms
    }
    if (in.map == 0 && (in.opc == 0x9c || in.opc == 0x9d)) return true;
    return true;
}

// Emits a copy of a normal instruction at the assembler's position.
static Mem tmem_;
static const Mem& tmp_mem(const Mem& m) { tmem_ = m; return tmem_; }
static bool group_scratch = true;
static bool group_xmap = true;
// AVX512EMU_COUNT=1 (measurement): every group counts its entries; the top ones are logged at exit.
struct GCount { uint64_t orig; uint32_t len, ninsn; uint64_t* cnt; uint64_t orig_end; };
static GCount* gcounts; static uint32_t ngcounts; static int count_on = -1;
// counters in their own mapping, far from the program's code, data and heap (hint 0x5e0000000000)
static uint64_t* cnt_arena; static size_t cnt_used;
static uint64_t* cnt_alloc() {
    const size_t N = 1u << 22;
    if (!cnt_arena) { void* p = mmap((void*)0x5e0000000000ull, N * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return nullptr; cnt_arena = (uint64_t*)p; }
    if (cnt_used >= N) return nullptr;
    uint64_t* r = cnt_arena + cnt_used++; *r = 0; return r;
}
void count_report() {
    if (count_on <= 0 || !ngcounts) return;
    GCount top[60] = {};
    for (uint32_t i = 0; i < ngcounts; i++) {
        uint64_t c = *gcounts[i].cnt; int j = 59; if (top[j].cnt && *top[j].cnt >= c) continue;
        while (j > 0 && (!top[j - 1].cnt || *top[j - 1].cnt < c)) { top[j] = top[j - 1]; j--; }
        top[j] = gcounts[i];
    }
    elog("avx512emu[%d]: group entries (top 60): count orig insns codebytes\n", getpid());
    for (int j = 0; j < 60 && top[j].cnt; j++) { ObjInfo o; bool f = find_object(top[j].orig, o);
        elog("  %12lu main+%#lx %u %u\n", (unsigned long)*top[j].cnt, (unsigned long)(top[j].orig - (f ? o.base : 0)), top[j].ninsn, top[j].len); }
}
// Loop regions (AVX512EMU_REGION=0 off): a call-free loop with AVX-512 code is one group with internal
// branches; its back-edge stays in translated code. Every entry from outside gets its own jmp patch.
static bool group_regions = false;   // off: no measurable gain on the 5950X, and the harness found wrong results in some regions
static bool region_mode = false;
bool gh_no_regions = false;          // harness switch          // install_group: building a region
static int* region_E = nullptr; static int region_nE = 0;   // external entries (indices, sorted, first = g0)
static bool group_dyn = true;   // AVX512EMU_DYNMAP=0: one static xmap per group (the old way)
static bool group_gdead = true;
static bool group_mcache = true;
static bool group_reloc = true;   // AVX512EMU_RELOC=0: no register-to-register moves of mapped zmm16-31 // AVX512EMU_MCACHE=0: no mask vector cache  // AVX512EMU_GDEAD=0: no dead-GPR scratch
static bool group_kcache = true;  // k values kept in a scratch GPR between instructions (AVX512EMU_KCACHE=0: off)
static bool group_cpool = true;   // read-only {1toN} constants from a pre-broadcast pool (AVX512EMU_CPOOL=0: off)
static int group_mcseed = 7;   // compares leave their lane mask in the mask vector cache (AVX512EMU_MCSEED=0: off)
static bool group_mcgreedy = true; // mask vector cache takes a register even when only the scratch demand is free (AVX512EMU_MCGREEDY=0)
static bool group_tailj = true;  // groups take a following jcc/jmp (AVX512EMU_TAILJ=0: off)
static int group_gap = 64;      // normal instructions a group spans to reach the next special (AVX512EMU_GAP; was 4)

// Vector registers 0-31 an EVEX/VEX instruction names (exact for EVEX: zmm16-31 kept apart; may
// over-approximate where a field is a k register or GPR, which only costs a load or a restore).
static uint32_t named_vregs32(const Insn& in) {
    if (in.enc != EVEX) return named_vregs(in);
    uint32_t m = 0;
    m |= 1u << (reg_of(in) & 31);
    m |= 1u << (in.vvvv & 31);
    if (in.mod == 3) m |= 1u << (rm_reg_of(in) & 31);
    if (in.has_sib && in.mod != 3) m |= 1u << ((((in.sib >> 3) & 7) | (in.X << 3) | (in.Vp << 4)) & 31);
    return m;
}

// zmm16-31 liveness: every named field counts as read, except the destination of a pure write
// (named only once); calls and unknown flow read all; at ret none is live (caller-saved, no returns).
static bool group_hilive = true;   // AVX512EMU_HILIVE=0: hi registers always live (no dead stores/loads skipped)
static void hliveness(FI* fi, int n) {
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        f.huse = f.hdef = 0; f.hlive = 0;
        if (f.in.enc == EVEX) {
            uint32_t m = named_vregs32(f.in);
            f.huse = uint16_t(m >> 16);
            int d = vec_dst(f.in);
            if (d >= 16 && d < 32 && evex_pure_write(f.in)) {
                int occ = (reg_of(f.in) == d) + ((f.in.vvvv & 31) == d) + (f.in.mod == 3 && rm_reg_of(f.in) == d) +
                          (f.in.has_sib && f.in.mod != 3 && ((((f.in.sib >> 3) & 7) | (f.in.X << 3) | (f.in.Vp << 4)) & 31) == d);
                if (occ == 1) f.huse &= ~uint16_t(1u << (d - 16));
                f.hdef = uint16_t(1u << (d - 16));
            }
        }
        if (f.in.flow == F_CALL || f.in.flow == F_INDIRECT || f.in.flow == F_STOP) { f.huse = 0xffff; f.hdef = 0; }
        if (call_abi && is_call(f)) { f.huse = 0; f.hdef = 0xffff; }   // zmm16-31: no arguments, clobbered
        if (!group_hilive) { f.huse = 0xffff; f.hdef = 0; }
    }
    bool changed = true; int rounds = 0;
    while (changed && rounds < 200) {
        changed = false; rounds++;
        for (int i = n - 1; i >= 0; i--) {
            FI& f = fi[i];
            uint16_t out;
            auto in_of = [&](int j) -> uint16_t { return j >= 0 && j < n ? uint16_t(fi[j].huse | (fi[j].hlive & ~fi[j].hdef)) : 0xffff; };
            switch (f.in.flow) {
            case F_NONE: case F_CALL: out = in_of(i + 1); break;
            case F_JCC: out = in_of(i + 1) | (f.target >= 0 ? in_of(f.target) : 0xffff); break;
            case F_JMP: out = f.target >= 0 ? in_of(f.target) : 0xffff; break;
            case F_RET: out = group_hilive ? 0 : 0xffff; break;
            default: out = (call_abi && is_call(f)) ? in_of(i + 1) : 0xffff; break;
            }
            out |= f.hlive;
            if (out != f.hlive) { f.hlive = out; changed = true; }
        }
    }
    if (changed) for (int i = 0; i < n; i++) fi[i].hlive = 0xffff;
}

extern bool g_xl_pic;
static bool emit_copy(Asm& a, const FI& f, uint64_t* call_slot) {
    const Insn& in = f.in;
    const uint8_t* p = (const uint8_t*)f.addr;
    if (in.flow == F_JCC || in.flow == F_JMP) {
        uint64_t t = branch_target(f);
        if (in.map == 0 && in.opc >= 0xe0 && in.opc <= 0xe3) { // loop/jrcxz rel8
            a.bytes(p, in.npfx + (in.rex ? 1 : 0)); a.b(in.opc); a.b(2); a.b(0xeb); a.b(5); a.jmp(t);
            return !a.overflow;
        }
        if (in.flow == F_JMP) { a.jmp(t); return !a.overflow; }
        int cc = (in.map == 0) ? (in.opc - 0x70) : (in.opc - 0x80);
        a.jcc(cc, t);
        return !a.overflow;
    }
    // offline (position-independent): lea -8(%rsp),%rsp; push %rax; lea ret(%rip),%rax; mov %rax,8(%rsp); pop %rax
    auto push_ret_pic = [&](uint64_t ret) {
        a.lea(RSP, Mem{RSP, -1, 0, -8}); a.b(0x50);
        Mem r; r.rip = true; r.target = ret; a.lea(RAX, r);
        a.mov_store64(Mem{RSP, -1, 0, 8}, RAX); a.b(0x58);
    };
    if (in.flow == F_CALL) { // push return address, jump: return lands in the original code
        uint64_t t = branch_target(f);
        if (g_xl_pic) { push_ret_pic(f.addr + in.len); a.jmp(t); return !a.overflow; }
        *call_slot = f.addr + in.len;
        Mem m; m.rip = true; m.target = (uint64_t)call_slot;
        a.leg(0, 0, 0, 0xFF, 6, 0, &m);   // push slot(%rip)
        a.jmp(t);
        return !a.overflow;
    }
    if (in.flow == F_INDIRECT && (in.reg3 == 2)) { // call *x: push return address, jmp *x
        if (g_xl_pic) {
            push_ret_pic(f.addr + in.len);
        } else {
        *call_slot = f.addr + in.len;
        Mem m; m.rip = true; m.target = (uint64_t)call_slot;
        a.leg(0, 0, 0, 0xFF, 6, 0, &m);
        }
        // re-emit as jmp *x: same bytes with /4 instead of /2
        int start = a.n;
        a.bytes(p, in.len);
        a.buf[start + (in.opc_off + 1)] = (uint8_t)((in.modrm & ~0x38) | (4 << 3));
        if (in.riprel) {
            int64_t t = int64_t(f.addr + in.len) + in.disp_raw;
            int64_t nd = t - int64_t(a.pc0 + start + in.len);
            if (nd != int32_t(nd)) return false;
            int32_t d32 = int32_t(nd); memcpy(a.buf + start + in.disp_off, &d32, 4);
        }
        return !a.overflow;
    }
    int start = a.n;
    a.bytes(p, in.len);
    if (in.riprel) {
        int64_t t = int64_t(f.addr + in.len) + in.disp_raw;
        int64_t nd = t - int64_t(a.pc0 + start + in.len);
        if (nd != int32_t(nd)) return false;
        int32_t d32 = int32_t(nd); if (start + in.disp_off + 4 <= a.cap) memcpy(a.buf + start + in.disp_off, &d32, 4);
    }
    return !a.overflow;
}

static void emit_zhi_zero(Asm& a, int r) {
    int32_t off = tls_offset() + int32_t(offsetof(TState, zhi)) + 32 * r;
    for (int i = 0; i < 4; i++) a.mov_store_imm64(Mem::fs(off + 8 * i), 0);
}

// Test mode (gharness): build groups without patching; they exit to gh_exit.
struct GhCtx { uint64_t exit, entry, code; int len; };
GhCtx* gh = nullptr;
// Called instead of installing each group when set (gharness).
bool (*group_hook)(FI* fi, int n, int g0, int g1, bool int3_only) = nullptr;

// offline tool (xloff): told about every installed group (descs, call slots, thunk slot, code)
void (*xl_group_hook)(uint8_t* mem, int nspecial, int ncall, uint64_t code, int len, uint64_t orig) = nullptr;
uint64_t g_xl_thunk_slot = 0;   // offline: one shared slot for &emu_thunk (no per-group slot to fix up)
bool g_xl_pic = false;          // offline: calls push their return address without an absolute slot

// Pending code patches of the function being translated.
struct Patch { uint64_t start, end, stub; uint64_t* starts; int n; bool int3_only; };
static Patch* patches; static int npatches;
static void apply_patches() {
    if (!npatches) return;
    uint8_t cc = 0xCC;
    for (int k = 0; k < npatches; k++) for (int i = 0; i < patches[k].n; i++) poke(patches[k].starts[i], &cc, 1);
    sync_cores();
    bool filled = false;
    for (int k = 0; k < npatches; k++) {
        Patch& p = patches[k];
        if (p.int3_only) continue;
        int64_t rel = int64_t(p.stub) - int64_t(p.start + 5);
        if (rel != int32_t(rel)) { p.int3_only = true; continue; } // int3 entry still works
        uint8_t buf[64]; memset(buf, 0xCC, sizeof buf);
        int32_t r32 = int32_t(rel); memcpy(buf + 1, &r32, 4);
        int len = int(p.end - p.start);
        if (len > 1) poke(p.start + 1, buf + 1, len - 1 < 63 ? len - 1 : 63);
        for (int off = 64; off < len; off += 64) { uint8_t c2[64]; memset(c2, 0xCC, 64); poke(p.start + off, c2, len - off < 64 ? len - off : 64); }
        filled = true;
    }
    if (filled) {
        sync_cores();
        uint8_t e9 = 0xE9;
        for (int k = 0; k < npatches; k++) if (!patches[k].int3_only) poke(patches[k].start, &e9, 1);
        sync_cores();
    }
    npatches = 0;
}

// Registers the current function reads at >= 256 / 512 bits (set per function).
static uint32_t fn_rd256 = ~0u, fn_rd512 = ~0u;

// Builds and installs one group [g0, g1). Returns false on failure.
static int n_all;   // instruction count of the function being translated
static bool install_group(FI* fi, int g0, int g1, bool int3_only) {
    uint64_t start = fi[g0].addr, end = fi[g1 - 1].addr + fi[g1 - 1].in.len;
    // descriptors and call slots live in the cache next to the code
    int nspecial = 0, ncall = 0;
    for (int i = g0; i < g1; i++) { if (fi[i].flags & F_SPECIAL) nspecial++; if (fi[i].in.flow == F_CALL || fi[i].in.flow == F_INDIRECT) ncall++; }
    if (count_on < 0) { const char* e = getenv("AVX512EMU_COUNT"); count_on = e && *e == '1'; }
    int nbc = 0; if (group_cpool) for (int i = g0; i < g1; i++) if ((fi[i].flags & F_SPECIAL) && fi[i].in.enc == EVEX && fi[i].in.b && fi[i].in.mod != 3 && fi[i].in.riprel) nbc++;
    size_t pool_off = (sizeof(Desc) * nspecial + 8 * ncall + 16 + (count_on > 0 ? 8 : 0) + 31) & ~size_t(31);
    size_t datasz = pool_off + 32 * size_t(nbc);
    // AVX512EMU_CAPTURE: group-entry capture record (CapInfo) after the constant pool
    static int capture_on = -1; if (capture_on < 0) capture_on = getenv("AVX512EMU_CAPTURE") != nullptr;
    int ncapops = 0;
    if (capture_on && !gh && !g_xl_thunk_slot)
        for (int i = g0; i < g1; i++) { const Insn& q = fi[i].in; if (q.has_modrm && q.mod != 3 && !(q.enc == LEGACY && q.map == 0 && q.opc == 0x8d) && ncapops < 64) ncapops++; }
    size_t cap_off = datasz;
    bool cap = capture_on && !gh && !g_xl_thunk_slot && ncapops >= 0;
    if (cap) datasz += (sizeof(CapInfo) + sizeof(CapOp) * ncapops + 63) & ~size_t(63);
    size_t codecap = 64 + (g1 - g0) * 32 + nspecial * 900 + (group_dyn ? size_t(g1 - g0) * 560 : 0) + (region_mode ? size_t(g1 - g0) * 900 : 0);   // dyn: transitions + entry stubs
    uint8_t* mem = cache_alloc(start, datasz + codecap, 64);
    if (!mem) return false;
    Desc* descs = (Desc*)mem;
    uint64_t* slots = (uint64_t*)(mem + sizeof(Desc) * nspecial);
    uint64_t* thunk_slot = slots + ncall;
    *thunk_slot = (uint64_t)emu_thunk;
    uint64_t* cnt_slot = count_on > 0 ? cnt_alloc() : nullptr;   // far from program data (an out-of-bounds read must not see counters)
    if (cnt_slot) *cnt_slot = 0;
    uint8_t* code = mem + datasz;
    code = (uint8_t*)(((uint64_t)code + 15) & ~15ull);
    CapInfo* ci_cap = nullptr;
    if (cap) {
        ci_cap = (CapInfo*)(mem + cap_off);
        memset(ci_cap, 0, sizeof(CapInfo));
        ci_cap->d.pc = start; ci_cap->d.flags = DF_CAPTURE; ci_cap->d.in = fi[g0].in;
        ci_cap->countdown = 1; ci_cap->start = start; ci_cap->end = end;
        uint16_t written = 0; int n = 0;
        for (int i = g0; i < g1 && n < ncapops; i++) {
            const Insn& q = fi[i].in;
            if (q.has_modrm && q.mod != 3 && !(q.enc == LEGACY && q.map == 0 && q.opc == 0x8d)) {
                CapOp& op = ci_cap->ops[n++];
                op.disp = mem_disp(q); op.scale = int8_t(scale_of(q));
                op.base = has_base(q) ? int8_t(base_of(q)) : -1;
                bool vsib = q.map == 2 && q.pp == 1 && ((q.opc >= 0x90 && q.opc <= 0x93) || (q.opc >= 0xa0 && q.opc <= 0xa3)) && q.has_sib;
                if (q.riprel) { op.kind = 3; op.abs = fi[i].addr + q.len + int64_t(op.disp); op.base = -1; op.index = -1; }
                else if (vsib) { op.kind = (q.opc & 1) ? 2 : 1; op.index = int8_t(index_of(q) | (q.enc == EVEX ? (q.Vp << 4) : 0)); }
                else { op.kind = 0; op.index = has_index(q) ? int8_t(index_of(q)) : -1; }
                if ((op.base >= 0 && ((written >> op.base) & 1)) || (op.kind == 0 && op.index >= 0 && ((written >> op.index) & 1))) op.kind |= 16;
            }
            written |= fi[i].gdef;
            if (q.has_modrm && q.mod == 3 && q.enc == LEGACY) written |= uint16_t(1u << (rm_reg_of(q) & 15));   // over-approximate
        }
        ci_cap->nops = uint32_t(n);
    }
    // generate into a reused buffer (no page faults on fresh cache memory), copy the exact size afterwards
    if (gencap < codecap) { if (genbuf) munmap(genbuf, gencap); gencap = codecap < (2u << 20) ? (2u << 20) : (codecap + 0xffff) & ~size_t(0xffff);
        genbuf = (uint8_t*)mmap(nullptr, gencap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0); }
    Asm a(genbuf, (int)codecap - 16, (uint64_t)code);
    XlateOpts o; o.thunk_slot = g_xl_thunk_slot ? g_xl_thunk_slot : (uint64_t)thunk_slot; o.no_inline = no_inline; o.mc_seed = group_mcseed;
    if (nbc) { o.cpool = mem + pool_off; o.cpool_cap = nbc; }
    o.rd512 = fn_rd512 | 0xff;     // zmm0-7 may carry 512-bit arguments/results across calls
    o.rd256 = fn_rd256 | 0xff;
    // group-level scratch: registers no instruction of the group names, saved once
    uint16_t named = 0; int nsp = 0;
    for (int i = g0; i < g1; i++) {
        named |= named_vregs(fi[i].in);
        if (fi[i].in.enc == EVEX && fi[i].in.has_sib && fi[i].in.mod != 3) named |= 1u << ((((fi[i].in.sib >> 3) & 7) | (fi[i].in.X << 3)) & 15);
        if (fi[i].flags & F_SPECIAL) nsp++;
    }
    uint16_t pre = 0;
    if (nsp >= 2 && group_scratch && !group_dyn) for (int r = 15, k = 0; r >= 0 && k < 4; r--) if (!(named & (1u << r))) { pre |= 1u << r; k++; }
    o.vpre = pre;
    // map the most used zmm16-31 of the group onto free hardware registers
    uint16_t xwritten = 0; uint32_t xmapped = 0;
    if (group_xmap && !group_dyn) {
        int uses[16] = {};
        for (int i = g0; i < g1; i++) {
            if (!(fi[i].flags & F_SPECIAL) || fi[i].in.enc != EVEX) continue;
            const Insn& q = fi[i].in;
            auto u = [&](int r) { if (r >= 16 && r < 32) uses[r - 16]++; };
            u(reg_of(q)); u(q.vvvv); if (q.mod == 3) u(rm_reg_of(q));
            if (q.has_sib && q.mod != 3 && q.map == 2 && ((q.opc >= 0x90 && q.opc <= 0x93) || (q.opc >= 0xa0 && q.opc <= 0xa3))) u(((q.sib >> 3) & 7) | (q.X << 3) | (q.Vp << 4));
            int dd = vec_dst(q); if (dd >= 16 && dd < 32) xwritten |= 1u << (dd - 16);
        }
        uint16_t freeh = uint16_t(~named & ~pre);
        for (int k = 0; k < 6; k++) {
            int best = -1; for (int i = 0; i < 16; i++) if (o.xmap[i] < 0 && uses[i] >= 3 && (best < 0 || uses[i] > uses[best])) best = i;
            if (best < 0) break;
            int h = -1; for (int r = 15; r >= 0; r--) if (freeh & (1u << r)) { h = r; break; }
            if (h < 0) break;
            freeh &= ~(1u << h); o.xmap[best] = (int8_t)h; xmapped |= 1u << best; uses[best] = 0;
        }
    }
    // GPRs no instruction of the group names (implicit users excluded wholesale)
    uint16_t gnamed = (1u << RSP) | (1u << RBP) | (1u << RAX) | (1u << RCX) | (1u << RDX) | (1u << RSI) | (1u << RDI);
    for (int i = g0; i < g1; i++) {
        const Insn& q = fi[i].in;
        if (q.has_modrm) { gnamed |= 1u << (reg_of(q) & 15); if (q.mod == 3) gnamed |= 1u << (rm_reg_of(q) & 15); }
        if (q.has_modrm && q.mod != 3) { if (has_base(q)) gnamed |= 1u << base_of(q); if (q.has_sib) gnamed |= 1u << (index_of(q) & 15); }
        if (q.enc == VEX) gnamed |= 1u << (q.vvvv & 15);
        if (!q.has_modrm && q.enc == LEGACY) gnamed |= 1u << ((q.opc & 7) | (q.B << 3)); // push/pop/mov imm/xchg reg in opcode
    }
    uint16_t gpre = 0;
    if (nsp >= 2 && group_scratch) for (int r = 15, k = 0; r >= 0 && k < 2; r--) if (!(gnamed & (1u << r))) { gpre |= 1u << r; k++; }
    int di = 0, ci = 0;
    uint64_t* stubpos = (uint64_t*)scratch_alloc(sizeof(uint64_t) * (g1 - g0));
    uint64_t* bodypos = (uint64_t*)scratch_alloc(sizeof(uint64_t) * (g1 - g0));
    int32_t T = tls_offset();
    auto ysv = [&](int r) { return Mem::fs(T + int32_t(offsetof(TState, ysave)) + 32 * r); };
    auto gsv = [&](int r) { return Mem::fs(T + int32_t(offsetof(TState, gsave)) + 8 * r); };
    auto zxmem = [&](int i) { return Mem::fs(T + int32_t(offsetof(TState, zx)) + 64 * i); };
    auto save_pre = [&]() {
        for (int r = 0; r < 16; r++) if (pre & (1u << r)) a.vex(1, 2, 0, 1, 0x7F, r, 0, 0, &tmp_mem(ysv(r)));
        for (int r = 0; r < 16; r++) if (gpre & (1u << r)) a.mov_store64(gsv(r), r);
    };
    auto restore_pre = [&]() {
        for (int r = 0; r < 16; r++) if (pre & (1u << r)) a.vex(1, 2, 0, 1, 0x6F, r, 0, 0, &tmp_mem(ysv(r)));
        for (int r = 0; r < 16; r++) if (gpre & (1u << r)) a.mov_load64(r, gsv(r));
    };
    auto map_in = [&]() { for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) {
        a.vex(1, 2, 0, 1, 0x7F, o.xmap[i], 0, 0, &tmp_mem(ysv(o.xmap[i])));
        a.vex(1, 2, 0, 1, 0x6F, o.xmap[i], 0, 0, &tmp_mem(zxmem(i))); } };
    auto map_out = [&]() { for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) {
        if (xwritten & (1u << i)) a.vex(1, 2, 0, 1, 0x7F, o.xmap[i], 0, 0, &tmp_mem(zxmem(i)));
        a.vex(1, 2, 0, 1, 0x6F, o.xmap[i], 0, 0, &tmp_mem(ysv(o.xmap[i]))); } };
    // ---- dynamic register map (group_dyn): every hardware ymm is, at each instruction, either
    // ARCH (holds its own architectural value), BORROWED (its value saved in ysave or dead; free
    // scratch for the translations) or MAPPED (borrowed and holding the low 256 bits of a zmm16-31).
    // Registers change state only where an instruction needs them: an instruction that names ymm H
    // gets H back as ARCH; a zmm16-31 it uses is loaded into a free or evicted register (the one
    // named again latest, dead ones first: no save). Leaving the group returns all to ARCH.
    const int L = g1 - g0;
    uint16_t* d_named = nullptr; uint16_t* d_hi = nullptr; uint16_t* d_livein = nullptr; int* d_nxt = nullptr;
    uint16_t* d_hin = nullptr; uint16_t* d_huse = nullptr;   // zmm16-31 live before instruction k / read by it
    int8_t* s_own = nullptr; uint16_t* s_borrow = nullptr; uint16_t* s_saved = nullptr;
    if (group_dyn) {
        d_named = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L); d_hi = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L);
        d_livein = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L); d_hin = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L);
        d_huse = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L); d_nxt = (int*)scratch_alloc(sizeof(int) * 16 * (L + 1));
        s_own = (int8_t*)scratch_alloc(16 * L); s_borrow = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L); s_saved = (uint16_t*)scratch_alloc(sizeof(uint16_t) * L);
        for (int k = 0; k < L; k++) {
            const FI& f = fi[g0 + k];
            bool sp = f.flags & F_SPECIAL;
            uint32_t m = sp ? named_vregs32(f.in) : named_vregs(f.in);
            if (!sp && f.in.enc == VEX && f.in.map == 1 && f.in.opc == 0x77) m = 0xffff;   // vzeroupper/vzeroall
            d_named[k] = uint16_t(m & 0xffff); d_hi[k] = sp ? uint16_t(m >> 16) : 0;
            d_livein[k] = uint16_t(f.vuse | (f.vlive & ~f.vdef));
            d_hin[k] = uint16_t(f.huse | (f.hlive & ~f.hdef)); d_huse[k] = f.huse;
        }
        for (int h = 0; h < 16; h++) d_nxt[16 * L + h] = 1 << 20;
        for (int k = L - 1; k >= 0; k--) for (int h = 0; h < 16; h++) d_nxt[16 * k + h] = (d_named[k] & (1u << h)) ? k : d_nxt[16 * (k + 1) + h];
    }
    int8_t own[16]; uint16_t borrow = 0, saved = 0, dirty = 0; int8_t hmap[16];
    int cache_h = -1; uint32_t cache_key = 0;   // ymm holding a mask vector (mask_key) across instructions
    int8_t* s_mreg = group_dyn ? (int8_t*)scratch_alloc(L) : nullptr; uint32_t* s_mkey = group_dyn ? (uint32_t*)scratch_alloc(sizeof(uint32_t) * L) : nullptr;
    int nmasked = 0; if (group_dyn) for (int i = g0; i < g1; i++) if ((fi[i].flags & F_SPECIAL) && fi[i].in.enc == EVEX && (fi[i].in.aaa || ((group_mcseed & 2) && writes_k(fi[i].in)))) nmasked++;
    const bool mcache_on = group_dyn && group_mcache && nmasked >= 2;
    uint16_t* s_dirty = group_dyn ? (uint16_t*)scratch_alloc(sizeof(uint16_t) * L) : nullptr;
    // k-in-GPR cache state before each instruction (entry stubs reload the GPR from the k slot)
    const int NKC = XlateOpts::NKC;
    int8_t* s_kcg = (int8_t*)scratch_alloc(L * NKC); int8_t* s_kck = (int8_t*)scratch_alloc(L * NKC); bool kc_any = false;
    auto kc_drop = [&]() { for (int q = 0; q < NKC; q++) { o.kc_gpr[q] = -1; o.kc_k[q] = -1; } };
    auto kc_drop_gprs = [&](uint32_t m) { for (int q = 0; q < NKC; q++) if (o.kc_gpr[q] >= 0 && ((m >> o.kc_gpr[q]) & 1)) { o.kc_gpr[q] = -1; o.kc_k[q] = -1; } };
    uint8_t* is_join = (uint8_t*)scratch_alloc(L); memset(is_join, 0, L);
    if (region_mode) for (int i = g0; i < g1; i++) { int tg = fi[i].target; if ((fi[i].in.flow == F_JCC || fi[i].in.flow == F_JMP) && tg >= g0 && tg < g1) is_join[tg - g0] = 1; }
    if (region_mode) for (int e = 0; e < region_nE; e++) if (region_E[e] >= g0 && region_E[e] < g1) is_join[region_E[e] - g0] = 1;
    auto ysv_m = [&](int h) -> const Mem& { return tmp_mem(ysv(h)); };
    auto zx_m = [&](int r) -> const Mem& { return tmp_mem(zxmem(r)); };
    auto d_reset = [&]() { for (int h = 0; h < 16; h++) { own[h] = -1; hmap[h] = -1; } borrow = saved = dirty = 0; cache_h = -1; cache_key = 0; };
    uint16_t hl_here = 0xffff;    // zmm16-31 live at the current transition point (dirty dead ones are not stored)
    auto d_unmap = [&](int h) {   // MAPPED -> BORROWED
        int r = own[h]; if (r < 0) return;
        if ((dirty & (1u << r)) && (hl_here & (1u << r))) a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &zx_m(r));
        dirty &= ~(1u << r); hmap[r] = -1; own[h] = -1;
    };
    auto d_arch = [&](int h, bool need = true) {    // -> ARCH (need: its value is read later)
        d_unmap(h);
        if (!(borrow & (1u << h))) return;
        if ((saved & (1u << h)) && need) a.vex(1, 2, 0, 1, 0x6F, h, 0, 0, &ysv_m(h));
        borrow &= ~(1u << h); saved &= ~(1u << h);
    };
    auto d_borrow = [&](int h, bool live) {   // ARCH -> BORROWED
        if (borrow & (1u << h)) return;
        if (live) { a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &ysv_m(h)); saved |= 1u << h; }
        borrow |= 1u << h;
    };
    auto d_all_arch = [&](uint16_t live) { cache_h = -1; cache_key = 0; for (int h = 0; h < 16; h++) d_arch(h, (live >> h) & 1); };
    auto d_uses_soon = [&](int k, int r, int limit) { int c = 0; for (int j = k + 1; j < L && j < k + limit; j++) if (d_hi[j] & (1u << r)) c++; return c; };
    auto d_next_hi = [&](int k, int r) { for (int j = k + 1; j < L; j++) if (d_hi[j] & (1u << r)) return j; return 1 << 20; };
    // scratch demand per instruction: pass 0 offers 3 and records what the translation took
    uint8_t* d_need = group_dyn ? (uint8_t*)scratch_alloc(L) : nullptr;
    int d_pass = 0;
    // transitions before instruction k (special: sp)
    // a k-writing EVEX compare followed (within the group) by a masked instruction: its result can seed the mask cache
    auto seed_here = [&](int k) {
        if (!(group_mcseed & 2) || !writes_k(fi[g0 + k].in)) return false;
        for (int j = g0 + k + 1; j < g1 && j < g0 + k + 33; j++) if (fi[j].in.enc == EVEX && fi[j].in.aaa) return true;
        return false;
    };
    auto d_step = [&](int k, bool sp, uint16_t vdead_k) {
        static int nscr_env = -2; if (nscr_env == -2) { const char* e = getenv("AVX512EMU_NSCR"); nscr_env = e ? atoi(e) : -1; }
        const int NSCR = nscr_env >= 0 ? nscr_env : (d_pass == 0 ? 3 : d_need[k]);
        uint16_t named = d_named[k], live = d_livein[k];
        hl_here = d_hin[k];
        if (is_join[k]) cache_key = 0;   // reached by branches too: the cached mask is not known there
        if (cache_h >= 0 && (named & (1u << cache_h))) { cache_h = -1; cache_key = 0; }   // the instruction needs that register
        if (group_reloc) for (int h = 0; h < 16; h++) {   // a needed zmm16-31 in a register the instruction names: move it to a free one
            int r = own[h];
            if (r < 0 || !(named & (1u << h)) || !(hl_here & (1u << r))) continue;
            int nx = d_next_hi(k - 1, r); if (nx >= k + 24) continue;
            int h2 = -1;
            for (int q = 15; q >= 0; q--) if (!(named & (1u << q)) && own[q] < 0 && q != cache_h && ((borrow & (1u << q)) || !(live & (1u << q)))) { h2 = q; if (borrow & (1u << q)) break; }
            if (h2 < 0) continue;
            d_borrow(h2, false);
            a.vex(1, 1, 0, 1, 0x6F, h2, 0, h, nullptr);                // vmovdqa ymm h -> h2
            own[h2] = (int8_t)r; hmap[r] = (int8_t)h2; own[h] = -1;
        }
        for (int h = 0; h < 16; h++) if ((named & (1u << h)) && (borrow & (1u << h))) d_arch(h, (live >> h) & 1);   // only written: no restore
        if (!sp) return;
        auto is_free = [&](int h) { return h != cache_h && !(named & (1u << h)) && own[h] < 0 && ((borrow & (1u << h)) || (vdead_k & (1u << h))); };
        for (int r = 0; r < 16; r++) {
            if (!(d_hi[k] & (1u << r)) || hmap[r] >= 0) continue;
            int nmapped = 0; for (int h = 0; h < 16; h++) if (own[h] >= 0) nmapped++;
            int nnamed = __builtin_popcount(named);
            int best = -1; int bcost = 99, bnext = -1;
            if (nmapped + 1 + nnamed + (NSCR > 2 ? NSCR : 2) <= 16)   // keep 2 registers for scratch/spills
                for (int h = 0; h < 16; h++) {
                    if ((named & (1u << h)) || own[h] >= 0 || h == cache_h) continue;
                    int cost = (borrow & (1u << h)) ? 0 : ((live & (1u << h)) ? 2 : 0);
                    int nx = d_nxt[16 * (k + 1) + h];
                    if (cost < bcost || (cost == bcost && nx > bnext)) { best = h; bcost = cost; bnext = nx; }
                }
            if (best < 0) {   // evict the mapped register whose next use is farthest
                int far = -1;
                for (int h = 0; h < 16; h++) { int o2 = own[h]; if (o2 < 0 || (d_hi[k] & (1u << o2))) continue;
                    int nx = d_next_hi(k, o2); if (nx > far) { far = nx; best = h; } }
                if (best < 0) continue;                               // R stays in memory
                if (far <= k + 1 && d_uses_soon(k, r, 8) == 0) continue;
                d_unmap(best); bcost = 0;
            }
            static int mapmin = -1, mapwin = 16; if (mapmin < 0) { const char* e = getenv("AVX512EMU_MAPMIN"); mapmin = e ? atoi(e) : 2; const char* w = getenv("AVX512EMU_MAPWIN"); mapwin = w ? atoi(w) : 32; }
            if (bcost == 2 && d_uses_soon(k, r, mapwin) < mapmin) continue;   // too few uses: not worth a save/restore
            d_borrow(best, live & (1u << best));
            if (d_huse[k] & (1u << r)) a.vex(1, 2, 0, 1, 0x6F, best, 0, 0, &zx_m(r));   // a pure write needs no old value
            own[best] = (int8_t)r; hmap[r] = (int8_t)best;
        }
        // scratch for the translation: free (borrowed/dead) registers, else borrow the latest-named
        int nfree = 0; for (int h = 0; h < 16; h++) if (is_free(h)) nfree++;
        while (nfree < NSCR) {
            int best = -1, bnext = -1;
            for (int h = 0; h < 16; h++) {
                if ((named & (1u << h)) || own[h] >= 0 || (borrow & (1u << h)) || (vdead_k & (1u << h)) || h == cache_h) continue;
                int nx = d_nxt[16 * (k + 1) + h]; if (nx > bnext) { bnext = nx; best = h; }
            }
            if (best < 0) break;
            d_borrow(best, live & (1u << best)); nfree++;
        }
        // a register for the mask vector cache (masked instruction, group with >= 2 of them), only if one is spare
        if (mcache_on && cache_h < 0 && fi[g0 + k].in.enc == EVEX && (fi[g0 + k].in.aaa || seed_here(k)) && (nfree > NSCR || (group_mcgreedy && nfree >= NSCR && NSCR > 0))) {
            int best = -1;
            for (int h = 0; h < 16; h++) if (is_free(h) && (best < 0 || ((borrow & (1u << h)) && !(borrow & (1u << best))))) best = h;
            if (best >= 0) { d_borrow(best, live & (1u << best)); cache_h = best; cache_key = 0; }
        }
    };
    auto d_snapshot = [&](int k) { for (int h = 0; h < 16; h++) s_own[16 * k + h] = own[h]; s_borrow[k] = borrow; s_saved[k] = saved; s_mreg[k] = (int8_t)cache_h; s_mkey[k] = cache_key; if (s_dirty) s_dirty[k] = dirty; };
    auto d_establish = [&](int k) {   // from all-ARCH (entry into the middle) to the state at k
        for (int h = 0; h < 16; h++) {
            if (!(s_borrow[k] & (1u << h))) continue;
            if (s_saved[k] & (1u << h)) a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &ysv_m(h));
            if (s_own[16 * k + h] >= 0) a.vex(1, 2, 0, 1, 0x6F, h, 0, 0, &zx_m(s_own[16 * k + h]));
        }
        if (s_mreg[k] >= 0 && s_mkey[k]) emit_maskvec_key(a, s_mreg[k], s_mkey[k]);   // rebuild the cached mask
    };
    // ---- region mode: internal branches (deferred stubs reconcile the state to the target's)
    struct DS { int8_t own[16]; uint16_t borrow, saved, dirty; };
    auto d_capture = [&]() { DS x; for (int h = 0; h < 16; h++) x.own[h] = own[h]; x.borrow = borrow; x.saved = saved; x.dirty = dirty; return x; };
    auto reconcile = [&](const DS& src, int k) {   // from src to the state at instruction k (memory moves only)
        uint16_t hl = d_hin[k], ll = d_livein[k], db = s_borrow[k], dsv = s_saved[k], ddt = s_dirty[k];
        int8_t dmap[16]; for (int r = 0; r < 16; r++) dmap[r] = -1;
        for (int h = 0; h < 16; h++) if (s_own[16 * k + h] >= 0) dmap[s_own[16 * k + h]] = (int8_t)h;
        int8_t smap[16]; for (int r = 0; r < 16; r++) smap[r] = -1;
        for (int h = 0; h < 16; h++) if (src.own[h] >= 0) smap[src.own[h]] = (int8_t)h;
        for (int r = 0; r < 16; r++) { int h = smap[r]; if (h < 0) continue; bool keep = dmap[r] == h;
            if (((src.dirty >> r) & 1) && !(keep && ((ddt >> r) & 1)) && ((hl >> r) & 1)) a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &zx_m(r)); }
        for (int h = 0; h < 16; h++) if (!((src.borrow >> h) & 1) && ((db >> h) & 1) && ((dsv >> h) & 1)) a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &ysv_m(h));
        for (int h = 0; h < 16; h++) if (((src.borrow >> h) & 1) && !((db >> h) & 1) && ((src.saved >> h) & 1) && ((ll >> h) & 1)) a.vex(1, 2, 0, 1, 0x6F, h, 0, 0, &ysv_m(h));
        for (int r = 0; r < 16; r++) { int h = dmap[r]; if (h < 0 || smap[r] == h || !((hl >> r) & 1)) continue; a.vex(1, 2, 0, 1, 0x6F, h, 0, 0, &zx_m(r)); }
    };
    auto exit_from = [&](const DS& src, int tj) {   // leave the group from state src towards function index tj (-1 unknown)
        uint16_t hl = 0xffff, ll = 0xffff;
        if (tj >= 0 && tj < n_all) { hl = uint16_t(fi[tj].huse | (fi[tj].hlive & ~fi[tj].hdef)); ll = uint16_t(fi[tj].vuse | (fi[tj].vlive & ~fi[tj].vdef)); }
        for (int h = 0; h < 16; h++) { int r = src.own[h]; if (r >= 0 && ((src.dirty >> r) & 1) && ((hl >> r) & 1)) a.vex(1, 2, 0, 1, 0x7F, h, 0, 0, &zx_m(r)); }
        for (int h = 0; h < 16; h++) if (((src.borrow >> h) & 1) && ((src.saved >> h) & 1) && ((ll >> h) & 1)) a.vex(1, 2, 0, 1, 0x6F, h, 0, 0, &ysv_m(h));
    };
    struct BR { int off; DS src; int tg; uint64_t taddr; bool internal; };
    BR* brs = (BR*)scratch_alloc(sizeof(BR) * (L + 1)); int nbr = 0;
    bool d_any = false;
    // two passes: the first finds which group scratch registers are used; the second keeps only those
    for (int pass = 0; pass < 2; pass++) {
        a.n = 0; a.overflow = false; di = 0; ci = 0; o.cpool_n = 0;
        o.vpre = pre; o.gpre = gpre; o.vused = 0; o.gused = 0;
        uint64_t entry0 = a.pc();
        if (cnt_slot) {   // lea -128(%rsp),%rsp; push rax, rcx; movabs $cnt,%rax; rcx = (%rax) + 1 (lea, no flags); store; pop; lea 128(%rsp),%rsp
            a.lea(RSP, Mem{RSP, -1, 0, -128}); a.b(0x50); a.b(0x51); a.mov_imm64(RAX, (uint64_t)cnt_slot);
            a.mov_load64(RCX, Mem{RAX, -1, 0, 0}); a.lea(RCX, Mem{RCX, -1, 0, 1}); a.mov_store64(Mem{RAX, -1, 0, 0}, RCX);
            a.b(0x59); a.b(0x58); a.lea(RSP, Mem{RSP, -1, 0, 128});
        }
        if (ci_cap) {   // countdown without touching the flags: pushfq, sub, pop rax keeps ZF, jnz over the capture call
            Mem cm; cm.rip = true; cm.target = (uint64_t)&ci_cap->countdown;
            a.lea(RSP, Mem{RSP, -1, 0, -128}); a.pushfq(); a.b(0x50);
            a.mov_load64(RAX, cm); a.alu_ri(5, 1, RAX, 1); a.mov_store64(cm, RAX); a.b(0x58);
            int jskip = a.jcc_fwd(5);
            a.popfq(); a.lea(RSP, Mem{RSP, -1, 0, 128});
            emit_interp_call(a, ci_cap->d, o);
            int jdone = a.jmp_fwd();
            a.bind(jskip); a.popfq(); a.lea(RSP, Mem{RSP, -1, 0, 128});
            a.bind(jdone);
        }
        save_pre(); map_in();
        bool left = false;
        if (group_dyn) { d_reset(); d_pass = pass; }
        nbr = 0; kc_drop();
        for (int i = g0; i < g1; i++) {
            FI& f = fi[i];
            uint16_t vdead_i = uint16_t(~f.vlive) & ~f.vuse;
            if (group_dyn) { d_step(i - g0, (f.flags & F_SPECIAL) != 0, vdead_i); d_snapshot(i - g0); if (borrow) d_any = true; }
            if (!group_kcache || is_join[i - g0]) kc_drop();
            for (int q = 0; q < NKC; q++) { s_kcg[(i - g0) * NKC + q] = o.kc_gpr[q]; s_kck[(i - g0) * NKC + q] = o.kc_k[q]; if (o.kc_gpr[q] >= 0 && i > g0) kc_any = true; }
            stubpos[i - g0] = bodypos[i - g0] = a.pc();
            if (i == g0) stubpos[0] = entry0;
            if (f.flags & F_SPECIAL) {
                Desc& d = descs[di++];
                memset(&d, 0, sizeof d);
                if (pass == 1) register_desc(&d);
                d.pc = f.addr; memcpy(d.bytes, (const void*)f.addr, f.in.len); d.in = f.in;
                o.continue_at = f.addr + f.in.len;
                o.vdead = vdead_i;
                o.rd512 = group_zlive ? ((fn_rd512 & ~0xffffu) | f.zlive) : (fn_rd512 | 0xff);
                o.gdead = group_gdead ? uint16_t(~f.glive & ~f.guse & ~(1u << RSP)) : 0;
                if (group_dyn) {
                    auto set_opts = [&]() {
                        uint16_t mapped = 0;
                        for (int r = 0; r < 16; r++) { o.xmap[r] = hmap[r]; if (hmap[r] >= 0) mapped |= 1u << hmap[r]; }
                        uint16_t cb = cache_h >= 0 ? uint16_t(1u << cache_h) : 0;
                        o.vpre = borrow & ~mapped & ~cb; o.vdead = vdead_i & ~mapped & ~cb;
                        o.mc_reg = (int8_t)cache_h; o.mc_key = cache_key; o.mc_busy = false;
                    };
                    set_opts();
                    // trial: does it translate inline with this many registers mapped? If not, give the
                    // mapped registers this instruction does not use back as scratch (else: interpreter)
                    int nmap_now = 0; for (int h = 0; h < 16; h++) if (own[h] >= 0) nmap_now++;
                    if (!o.no_inline && nmap_now >= 8) {   // only then can the translation run out of scratch
                        static uint8_t trial[16384];
                        Asm ta(trial, sizeof trial - 16, a.pc());
                        uint32_t vu = o.vused, gu = o.gused; int8_t kg[NKC], kk[NKC]; memcpy(kg, o.kc_gpr, NKC); memcpy(kk, o.kc_k, NKC);
                        bool ok = xlate_inline(ta, d, o) && !ta.overflow;
                        o.vused = vu; o.gused = gu; o.mc_key = cache_key; o.mc_busy = false; o.mc_wrote_k = false; memcpy(o.kc_gpr, kg, NKC); memcpy(o.kc_k, kk, NKC);
                        if (!ok) { for (int h = 0; h < 16; h++) if (own[h] >= 0 && !(d_hi[i - g0] & (1u << own[h]))) d_unmap(h); set_opts(); }
                    }
                }
            o.flags_live = flags_live_after(fi, n_all, i);
                const char* why = "";
                o.scr_taken = 0; o.kc_set = false;
                if (!xlate_insn(a, d, o, &why)) { elog("avx512emu: cannot translate at %#lx: %s\n", (unsigned long)f.addr, why); return false; }
                if (!o.kc_set && writes_k(f.in)) kc_drop();                       // k changed by other code
                kc_drop_gprs(f.gdef | f.guse);                                     // the GPRs themselves
                if (group_dyn) {
                    cache_key = writes_k(f.in) ? (o.mc_wrote_k ? o.mc_key : 0) : o.mc_key;
                    o.mc_reg = -1; o.mc_key = 0;
                    if (pass == 0) d_need[i - g0] = uint8_t(o.scr_taken > 4 ? 4 : o.scr_taken);
                    int dd = vec_dst(f.in); if (dd >= 16 && dd < 32 && hmap[dd - 16] >= 0) dirty |= 1u << (dd - 16);
                    for (int r = 0; r < 16; r++) o.xmap[r] = -1;
                    o.vpre = 0;
                }
            } else if (region_mode && group_dyn && (f.in.flow == F_JCC || f.in.flow == F_JMP) && !(gh && i == g1 - 1)) {
                int tg = f.target; bool internal = tg >= g0 && tg < g1 && !(gh && tg == g0);   // harness: back to the head = leave
                BR& b = brs[nbr++]; b.src = d_capture(); b.tg = tg; b.taddr = branch_target(f); b.internal = internal;
                if (f.in.flow == F_JCC) { int cc = (f.in.map == 0) ? (f.in.opc - 0x70) : (f.in.opc - 0x80); a.jcc(cc, a.pc()); }
                else a.jmp(a.pc());
                b.off = a.n - 4;
                if (f.in.flow == F_JMP) d_reset();   // the next instruction is not reached by falling through
                kc_drop();
            } else {
                if (i == g1 - 1 && f.in.flow != F_NONE) { if (group_dyn) { hl_here = d_hin[i - g0]; d_all_arch(d_livein[i - g0]); } map_out(); restore_pre(); left = true; }   // leaving the group
                if (!emit_copy(a, f, &slots[ci])) return false;
                if (f.in.flow != F_NONE) kc_drop(); else kc_drop_gprs(f.gdef | f.guse);
                if (f.in.flow == F_CALL || f.in.flow == F_INDIRECT) ci++;
            }
            if (f.flags & F_FIXUP) {
                if (f.def == -2) { for (int r = 0; r < 16; r++) emit_zhi_zero(a, r); }
                else if (f.def >= 0 && f.def < 16) emit_zhi_zero(a, f.def);
            }
        }
        bool falls = fi[g1 - 1].in.flow == F_NONE || fi[g1 - 1].in.flow == F_JCC;
        if (falls) { if (!left) { if (group_dyn) { hl_here = fi[g1 - 1].hlive; d_all_arch(fi[g1 - 1].vlive); } map_out(); restore_pre(); } a.jmp(gh ? gh->exit : end); }
        // entries into the middle of the group (int3 while patching): set up first
        if (pre || gpre || o.xmap_any() || d_any || region_mode || kc_any) for (int i = g0 + 1; i < g1; i++) { stubpos[i - g0] = a.pc(); save_pre(); map_in(); if (group_dyn) d_establish(i - g0);
            for (int q = 0; q < NKC; q++) { int gq = s_kcg[(i - g0) * NKC + q]; if (gq >= 0) a.mov_load64(gq, Mem::fs(tls_offset() + int32_t(offsetof(TState, k)) + 8 * s_kck[(i - g0) * NKC + q])); }
            a.jmp(bodypos[i - g0]); }
        for (int b = 0; b < nbr; b++) {   // region branch stubs
            uint64_t at = a.pc(); int32_t rel = int32_t(int64_t(at) - int64_t((uint64_t)code + brs[b].off + 4));
            if (brs[b].off + 4 <= (int)codecap) memcpy(genbuf + brs[b].off, &rel, 4);
            if (brs[b].internal) { reconcile(brs[b].src, brs[b].tg - g0); a.jmp(bodypos[brs[b].tg - g0]); }
            else { exit_from(brs[b].src, brs[b].tg); restore_pre(); a.jmp(gh ? gh->exit : brs[b].taddr); }
        }
        if (pass == 0) { pre &= (uint16_t)o.vused; gpre &= (uint16_t)o.gused; }
    }
    if (a.overflow) { elog("avx512emu: group too large at %#lx\n", (unsigned long)start); return false; }
    memcpy(code, genbuf, a.n);
    cache_trim(code + a.n);
    __builtin___clear_cache((char*)code, (char*)code + a.n);
    if (xl_group_hook && !gh) xl_group_hook(mem, nspecial, ncall, (uint64_t)code, a.n, start);
    if (gh) { gh->entry = stubpos[0]; gh->code = (uint64_t)code; gh->len = a.n; return true; }
    for (int i = g0; i < g1; i++) map_put(fi[i].addr, stubpos[i - g0]);
    prof_register_group((uint64_t)code, (uint32_t)a.n, start, (uint32_t)nspecial);
    if (cnt_slot) {
        if (!gcounts) gcounts = (GCount*)mmap(nullptr, sizeof(GCount) << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (ngcounts < (1u << 20)) gcounts[ngcounts++] = GCount{start, (uint32_t)a.n, (uint32_t)(g1 - g0), cnt_slot, end};
    }
    perfmap_group((uint64_t)code, (uint32_t)a.n, start);
    // patching is batched per function (apply_patches): int3s, sync, fill, sync, jmps, sync
    if (region_mode) {   // one patch per external entry: [E_j, E_j+1)
        for (int e = 0; e < region_nE; e++) {
            int a0 = region_E[e], a1 = e + 1 < region_nE ? region_E[e + 1] : g1;
            Patch& p = patches[npatches++];
            p.start = fi[a0].addr; p.end = fi[a1 - 1].addr + fi[a1 - 1].in.len; p.stub = stubpos[a0 - g0]; p.int3_only = false;
            p.starts = (uint64_t*)scratch_alloc(sizeof(uint64_t) * (a1 - a0)); p.n = a1 - a0;
            for (int i = a0; i < a1; i++) p.starts[i - a0] = fi[i].addr;
        }
        stats.groups++;
        return true;
    }
    Patch& p = patches[npatches++];
    p.start = start; p.end = end; p.stub = stubpos[0]; p.int3_only = int3_only;
    p.starts = (uint64_t*)scratch_alloc(sizeof(uint64_t) * (g1 - g0)); p.n = g1 - g0;
    for (int i = g0; i < g1; i++) p.starts[i - g0] = fi[i].addr;
    if (int3_only) stats.int3_sites++; else stats.groups++;
    return true;
}

// Decodes [fstart, fend) and marks entries, specials and fixups. pc (or 0) must be an instruction start.
int analyze_function(uint64_t fstart, uint64_t fend, uint64_t pc, FI** out) {
    static bool cinit; if (!cinit) { cinit = true; const char* e = getenv("AVX512EMU_CALLABI"); call_abi = !(e && *e == '0'); }
    size_t maxn = (fend - fstart) / 1 + 1;
    if (maxn > 4000000) return -1;
    FI* fi = (FI*)scratch_alloc(sizeof(FI) * (maxn > 200000 ? 200000 : maxn));
    if (!fi) return -1;
    int n = 0, cap = maxn > 200000 ? 200000 : (int)maxn;
    uint64_t a = fstart;
    bool pc_ok = pc == 0;
    while (a < fend && n < cap) {
        FI& f = fi[n];
        int l = decode((const uint8_t*)a, int(fend - a < 15 ? fend - a : 15), f.in);
        if (!l) break;
        f.addr = a; f.flags = 0; f.target = -1;
        if (a == pc) pc_ok = true;
        n++; a += l;
    }
    if (!pc_ok) { elog("avx512emu: %#lx is not an instruction start in %#lx-%#lx (decoded %d)\n", (unsigned long)pc, (unsigned long)fstart, (unsigned long)fend, n); return -1; }
    fi[0].flags |= F_ENTRY;
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        f.def = (int16_t)vec_dst(f.in);
        f.rd512 = reads512(f.in);
        if (is_special(f.in)) f.flags |= F_SPECIAL;
        if (f.in.flow == F_JCC || f.in.flow == F_JMP || f.in.flow == F_CALL) {
            uint64_t t = branch_target(f);
            if (t >= fstart && t < fend) {
                int j = find_index(fi, n, t);
                if (j >= 0) { fi[j].flags |= F_ENTRY; if (f.in.flow != F_CALL) f.target = j; }
            }
        }
        if (f.in.flow == F_CALL || (f.in.flow == F_INDIRECT && f.in.reg3 == 2)) { if (i + 1 < n) fi[i + 1].flags |= F_ENTRY; }
        if ((f.in.flow == F_JMP || f.in.flow == F_RET || f.in.flow == F_INDIRECT || f.in.flow == F_STOP) && i + 1 < n) fi[i + 1].flags |= F_ENTRY;
    }
    mark_fixups(fi, n);
    vliveness(fi, n);
    gliveness(fi, n);
    hliveness(fi, n);
    zliveness(fi, n);
    *out = fi;
    return n;
}

#ifdef EMU_DIFFTEST
static void fn_seen_pre(); static uint64_t fn_set_addr(); static uint64_t fn_set_bytes();
// all lazily made allocations, so that a test child that translates has the same address layout as one that does not
void diff_prealloc() {
    if (!all_descs) all_descs = (Desc**)mmap(nullptr, sizeof(Desc*) << 22, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (!gcounts) gcounts = (GCount*)mmap(nullptr, sizeof(GCount) << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (gencap < (8u << 20)) { if (genbuf) munmap(genbuf, gencap); gencap = 8u << 20;
        genbuf = (uint8_t*)mmap(nullptr, gencap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0); }
    fn_seen_pre();
}
int diff_fn_ranges(uint64_t* r, int max) {
    int k = 0;
    auto add = [&](uint64_t lo, uint64_t hi) { if (lo && k + 2 <= 2 * max) { r[k++] = lo; r[k++] = hi; } };
    add((uint64_t)all_descs, (uint64_t)all_descs + (sizeof(Desc*) << 22));
    add((uint64_t)gcounts, (uint64_t)gcounts + (sizeof(GCount) << 20));
    add((uint64_t)genbuf, (uint64_t)genbuf + gencap);
    add((uint64_t)cnt_arena, (uint64_t)cnt_arena + (8ull << 22));
    add((uint64_t)fn_set_addr(), (uint64_t)fn_set_addr() + fn_set_bytes());
    return k / 2;
}
// groups translated in this process: lo, hi (original addresses), entry count
uint32_t diff_counts(uint64_t* out, uint32_t max) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < ngcounts && k < max; i++) { out[3 * k] = gcounts[i].orig; out[3 * k + 1] = gcounts[i].orig_end; out[3 * k + 2] = *gcounts[i].cnt; k++; }
    return k;
}
#endif
static double ms_now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
// ---- functions translated in this process (for the pre-translation list, and to skip repeats)
static uint64_t* fn_set; static const uint32_t FN_SET = 1u << 17; static uint32_t fn_count;
static bool fn_seen(uint64_t a, bool add) {
    if (!fn_set) { fn_set = (uint64_t*)mmap(nullptr, sizeof(uint64_t) * FN_SET, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0); if (fn_set == MAP_FAILED) { fn_set = nullptr; return false; } }
    uint32_t h = uint32_t((a * 0x9E3779B97F4A7C15ull) >> 47) & (FN_SET - 1);
    for (uint32_t i = 0; i < FN_SET; i++, h = (h + 1) & (FN_SET - 1)) {
        if (fn_set[h] == a) return true;
        if (!fn_set[h]) { if (add && fn_count < FN_SET / 2) { fn_set[h] = a; fn_count++; } return false; }
    }
    return false;
}
#ifdef EMU_DIFFTEST
static void fn_seen_pre() { fn_seen(1, false); }
static uint64_t fn_set_addr() { return (uint64_t)fn_set; }
static uint64_t fn_set_bytes() { return sizeof(uint64_t) * FN_SET; }
#endif
// all recorded function starts (absolute); the caller holds the translation lock
uint32_t translated_functions(uint64_t* out, uint32_t max) {
    uint32_t k = 0; if (!fn_set) return 0;
    for (uint32_t i = 0; i < FN_SET && k < max; i++) if (fn_set[i]) out[k++] = fn_set[i];
    return k;
}
static bool translate_function(uint64_t fstart, uint64_t fend, uint64_t pc);
// translate the function starting at fstart ahead of time (background thread, translation lock held)
bool pretranslate_function(uint64_t fstart) {
    if (fn_seen(fstart, false)) return false;
    ObjInfo o; uint64_t fs, fe;
    if (!find_object(fstart, o) || !find_function(o, fstart, fs, fe) || fs != fstart) return false;
    scratch_reset();
    bool ok = translate_function(fs, fe, 0);
    scratch_reset();
    return ok;
}
static bool translate_function(uint64_t fstart, uint64_t fend, uint64_t pc) {
    fn_seen(fstart, true);
    double t0 = ms_now();
    FI* fi; int n = analyze_function(fstart, fend, pc, &fi);
    double t1 = ms_now();
    if (n < 0) return false;
    static int rdmask = -1; if (rdmask < 0) { const char* e = getenv("AVX512EMU_RDMASK"); rdmask = !(e && *e == '0'); }
    static int gx = -1; if (gx < 0) { const char* e = getenv("AVX512EMU_XMAP"); gx = !(e && *e == '0'); group_xmap = gx; }
    static int gs = -1; if (gs < 0) { const char* e = getenv("AVX512EMU_GROUPSCRATCH"); gs = !(e && *e == '0'); group_scratch = gs; }
    static int gd = -1; if (gd < 0) { const char* e = getenv("AVX512EMU_DYNMAP"); gd = !(e && *e == '0'); group_dyn = gd;
        const char* gdv = getenv("AVX512EMU_GDEAD"); group_gdead = !(gdv && *gdv == '0');
        const char* mcv = getenv("AVX512EMU_MCACHE"); group_mcache = !(mcv && *mcv == '0');
        const char* zlv = getenv("AVX512EMU_ZLIVE"); group_zlive = !(zlv && *zlv == '0');
        const char* hlv = getenv("AVX512EMU_HILIVE"); group_hilive = !(hlv && *hlv == '0');
        const char* cav = getenv("AVX512EMU_CALLABI"); call_abi = !(cav && *cav == '0');
        const char* rgv = getenv("AVX512EMU_REGION"); group_regions = rgv && *rgv == '1';
        const char* rlv = getenv("AVX512EMU_RELOC"); group_reloc = !(rlv && *rlv == '0');
        const char* tjv = getenv("AVX512EMU_TAILJ"); group_tailj = !(tjv && *tjv == '0');
        const char* kcv = getenv("AVX512EMU_KCACHE"); group_kcache = !(kcv && *kcv == '0');
        const char* mgv = getenv("AVX512EMU_MCGREEDY"); group_mcgreedy = !(mgv && *mgv == '0');
        const char* cpv = getenv("AVX512EMU_CPOOL"); group_cpool = !(cpv && *cpv == '0');
        const char* msv = getenv("AVX512EMU_MCSEED"); group_mcseed = msv && *msv ? atoi(msv) : 7;
        const char* g = getenv("AVX512EMU_GAP"); if (g) group_gap = atoi(g); else if (!group_dyn) group_gap = 4; }
    fn_rd256 = rdmask ? 0 : ~0u; fn_rd512 = rdmask ? 0 : ~0u;
    patches = (Patch*)scratch_alloc(sizeof(Patch) * (n + 1)); npatches = 0;
    n_all = n;
    for (int i = 0; i < n; i++) { fn_rd256 |= reads_wide(fi[i].in, 32); fn_rd512 |= reads_wide(fi[i].in, 64); }
    // loop regions first
    if (group_dyn && group_regions && !gh_no_regions) {
        int* npred = (int*)scratch_alloc(sizeof(int) * (n + 1)); memset(npred, 0, sizeof(int) * (n + 1));
        for (int j = 0; j < n; j++) if ((fi[j].in.flow == F_JCC || fi[j].in.flow == F_JMP) && fi[j].target >= 0) npred[fi[j].target]++;
        int* pstart = (int*)scratch_alloc(sizeof(int) * (n + 2)); pstart[0] = 0; for (int j = 0; j < n; j++) pstart[j + 1] = pstart[j] + npred[j];
        int* plist = (int*)scratch_alloc(sizeof(int) * (pstart[n] + 1)); int* pfill = (int*)scratch_alloc(sizeof(int) * (n + 1)); memset(pfill, 0, sizeof(int) * (n + 1));
        for (int j = 0; j < n; j++) if ((fi[j].in.flow == F_JCC || fi[j].in.flow == F_JMP) && fi[j].target >= 0) { int t = fi[j].target; plist[pstart[t] + pfill[t]++] = j; }
        struct RC { int t, s; }; RC* rc = (RC*)scratch_alloc(sizeof(RC) * (n + 1)); int nrc = 0;
        for (int sidx = 0; sidx < n; sidx++) {
            const FI& b = fi[sidx];
            if (!(b.in.flow == F_JCC || b.in.flow == F_JMP) || b.target < 0 || b.target > sidx || sidx - b.target > 600) continue;
            int t = b.target; bool ok = true, sp = false;
            for (int q = t; q <= sidx && ok; q++) {
                const FI& f = fi[q];
                if (f.in.flow == F_CALL || f.in.flow == F_RET || f.in.flow == F_INDIRECT || f.in.flow == F_STOP) ok = false;
                else if (!(f.flags & F_SPECIAL) && !relocatable(f)) ok = false;
                else if (f.in.map == 0 && f.in.opc >= 0xe0 && f.in.opc <= 0xe3) ok = false;
                else if ((f.flags & F_COVERED) || map_get(f.addr)) ok = false;
                if (f.flags & F_SPECIAL) sp = true;
            }
            if (ok && sp) rc[nrc++] = RC{t, sidx};
        }
        // outermost first; skip overlaps
        for (int x = 0; x < nrc; x++) for (int y = x + 1; y < nrc; y++) if (rc[y].s - rc[y].t > rc[x].s - rc[x].t) { RC tmp = rc[x]; rc[x] = rc[y]; rc[y] = tmp; }
        int* E = (int*)scratch_alloc(sizeof(int) * (n + 1));
        for (int x = 0; x < nrc; x++) {
            int t = rc[x].t, sidx = rc[x].s; bool free_ = true;
            for (int q = t; q <= sidx; q++) if (fi[q].flags & F_COVERED) { free_ = false; break; }
            if (!free_) continue;
            int nE = 0; E[nE++] = t;
            for (int q = t + 1; q <= sidx; q++) {
                bool ext = false;
                for (int pp = pstart[q]; pp < pstart[q + 1]; pp++) { int j = plist[pp]; if (j < t || j > sidx) ext = true; }
                if (ext) E[nE++] = q;
            }
            bool spaced = true;
            for (int e = 0; e < nE; e++) { uint64_t a0 = fi[E[e]].addr, a1 = e + 1 < nE ? fi[E[e + 1]].addr : fi[sidx].addr + fi[sidx].in.len; if (a1 - a0 < 5) spaced = false; }
            if (!spaced) continue;
            region_mode = true; region_E = E; region_nE = nE;
            bool okr;
            if (group_hook) { group_hook(fi, n, t, sidx + 1, false); okr = true; }
            else okr = install_group(fi, t, sidx + 1, false);
            region_mode = false; region_nE = 0;
            if (!okr) { stats.failures++; continue; }
            for (int q = t; q <= sidx; q++) fi[q].flags |= F_COVERED;
            stats.specials++; stats.regions++;
        }
    }
    // groups
    for (int i = 0; i < n; i++) {
        FI& f = fi[i];
        if (!(f.flags & (F_SPECIAL | F_FIXUP)) || (f.flags & F_COVERED)) continue;
        if (map_get(f.addr)) { f.flags |= F_COVERED; continue; } // already translated (shared code)
        int g0 = i, g1 = i + 1, len = f.in.len;
        // extend forward: merge following specials, reach 5 bytes
        while (g1 < n && !(fi[g1].flags & F_ENTRY) && !(fi[g1].flags & F_COVERED) && relocatable(fi[g1])) {
            bool want = len < 5 || (fi[g1].flags & (F_SPECIAL | F_FIXUP));
            if (!want) {
                // also take a few normal instructions if another special follows soon
                int k = g1, l2 = 0;
                while (k < n && k < g1 + group_gap && !(fi[k].flags & (F_SPECIAL | F_FIXUP | F_ENTRY)) && fi[k].in.flow == F_NONE && relocatable(fi[k])) { l2++; k++; }
                if (k < n && k < g1 + group_gap && (fi[k].flags & (F_SPECIAL | F_FIXUP)) && !(fi[k].flags & (F_ENTRY | F_COVERED))) { g1 = k; for (int q = i; q < g1; q++) {} len = int(fi[g1 - 1].addr + fi[g1 - 1].in.len - fi[g0].addr); continue; }
                break;
            }
            len += fi[g1].in.len; g1++;
            if (fi[g1 - 1].in.flow != F_NONE) break;
        }
        // take the branch that follows (vcmp; kortest; jcc in a loop): the group then jumps to the branch
        // target itself instead of back to the original jcc - one taken jump less per pass (AVX512EMU_TAILJ)
        if (group_tailj && len >= 5 && fi[g1 - 1].in.flow == F_NONE) {
            int k = g1;
            auto plain = [&](const FI& x) { return !(x.flags & (F_SPECIAL | F_FIXUP | F_ENTRY | F_COVERED)) && relocatable(x); };
            while (k < n && k < g1 + 4 && fi[k].in.flow == F_NONE && plain(fi[k])) k++;
            if (k < n && plain(fi[k]) && ((fi[k].in.flow == F_JCC && !(fi[k].in.map == 0 && fi[k].in.opc >= 0xe0)) || fi[k].in.flow == F_JMP)) {
                for (int q = g1; q <= k; q++) len += fi[q].in.len;
                g1 = k + 1;
            }
        }
        // extend backward when still short
        while (len < 5 && g0 > 0 && !(fi[g0].flags & F_ENTRY) && !(fi[g0 - 1].flags & F_COVERED) && fi[g0 - 1].in.flow == F_NONE && relocatable(fi[g0 - 1])) {
            g0--; len += fi[g0].in.len;
        }
        bool int3_only = len < 5;
        if (int3_only) { g0 = i; g1 = i + 1; }
        if (group_hook) { group_hook(fi, n, g0, g1, int3_only); for (int q = g0; q < g1; q++) fi[q].flags |= F_COVERED; continue; }
        if (!install_group(fi, g0, g1, int3_only)) { stats.failures++; apply_patches(); return false; }
        for (int q = g0; q < g1; q++) fi[q].flags |= F_COVERED;
        stats.specials++;
    }
    double t2 = ms_now();
    apply_patches();
    double t3 = ms_now();
    if (t3 - t0 > 2 && log_level >= 2) elog("avx512emu[%d]: function %#lx-%#lx (%d insns): analyze %.2f ms, generate %.2f ms, patch %.2f ms\n", getpid(),
        (unsigned long)fstart, (unsigned long)fend, n, t1 - t0, t2 - t1, t3 - t2);
    stats.functions++;
    cov_note(fstart);
    if (log_level >= 2 && (stats.functions & 63) == 0)
        elog("avx512emu[%d]: xlate %.1f ms (max %.2f) functions %lu groups %lu specials %lu fixups %lu int3 %lu failures %lu sigill %lu sigtrap %lu\n", getpid(),
             stats.xlate_ns / 1e6, stats.xlate_max_ns / 1e6,
             (unsigned long)stats.functions, (unsigned long)stats.groups, (unsigned long)stats.specials, (unsigned long)stats.fixups,
             (unsigned long)stats.int3_sites, (unsigned long)stats.failures, (unsigned long)stats.sigill, (unsigned long)stats.sigtrap),
        elog("avx512emu[%d]: audio-thread translations %lu, %.1f ms, max %.2f ms; lock wait max %.2f ms\n", getpid(),
             (unsigned long)stats.rt_n, stats.rt_ns / 1e6, stats.rt_max_ns / 1e6, stats.wait_max_ns / 1e6);
    return true;
}

// No function bounds: translate just the instruction at pc (patched if >= 5 bytes, else int3).
static bool translate_single(uint64_t pc) {
    fn_rd256 = fn_rd512 = ~0u;
    FI* fi = (FI*)scratch_alloc(sizeof(FI));
    if (!decode((const uint8_t*)pc, 15, fi[0].in)) return false;
    fi[0].addr = pc; fi[0].flags = F_SPECIAL; fi[0].def = -1; fi[0].rd512 = 0; fi[0].target = -1; fi[0].vlive = 0xffff; fi[0].vuse = 0xffff; fi[0].glive = 0xffff; fi[0].guse = 0xffff; fi[0].gdef = 0; fi[0].hlive = 0xffff; fi[0].huse = 0xffff; fi[0].hdef = 0; fi[0].zlive = 0xffff;
    stats.fallback_sites++;
    n_all = 1;
    patches = (Patch*)scratch_alloc(sizeof(Patch) * 2); npatches = 0;
    bool ok = install_group(fi, 0, 1, fi[0].in.len < 5);
    apply_patches();
    return ok;
}

uint64_t translate_at(uint64_t pc) {
    uint64_t s = map_get(pc);
    if (s) return s;
    scratch_reset();
    ObjInfo o;
    uint64_t fs, fe;
    bool ok = false;
    bool fo = find_object(pc, o);
    if (!fo) elog("avx512emu: no object for %#lx\n", (unsigned long)pc);
    else if (!find_function(o, pc, fs, fe)) { if (log_level >= 2) elog("avx512emu: no function bounds for %#lx in %s\n", (unsigned long)pc, o.name); }
    else {
        ok = translate_function(fs, fe, pc);
        if (!ok) elog("avx512emu: function %#lx-%#lx in %s not translated, single instruction instead\n", (unsigned long)fs, (unsigned long)fe, o.name);
    }
    if (!ok && !map_get(pc)) ok = translate_single(pc);
    scratch_reset();
    return map_get(pc);
}

} // namespace emu

// ---- offline translation segment (see offline.h)
#include "offline.h"
#include <sys/auxv.h>
#include <elf.h>
#include <algorithm>
namespace emu {
void offline_attach() {
    const Elf64_Phdr* ph = (const Elf64_Phdr*)getauxval(AT_PHDR); int nph = (int)getauxval(AT_PHNUM);
    if (!ph) return;
    uint64_t base = 0; bool have = false;
    for (int i = 0; i < nph; i++) if (ph[i].p_type == PT_PHDR) { base = (uint64_t)ph - ph[i].p_vaddr; have = true; }
    if (!have) return;
    for (int i = 0; i < nph; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_filesz < sizeof(XlHeader)) continue;
        XlHeader* h = (XlHeader*)(base + ph[i].p_vaddr);
        if (memcmp(h->magic, XL_MAGIC, 8)) continue;
        uint8_t* seg = (uint8_t*)h;
        const char* e = getenv("AVX512EMU_OFFLINE");
        double ta = ms_now();
        int32_t dT = tls_offset() - h->tls_build;
        bool ok = !(e && *e == '0') && h->version == XL_VERSION && h->abi == xl_abi() && h->seg_vaddr == ph[i].p_vaddr && (dT == 0 || h->n_tls);
        const uint32_t* pt = (const uint32_t*)(seg + h->off_patch);
        if (!ok) {   // not usable with this library: put the original code back (lazy translation as usual)
            for (uint32_t k = 0; k < h->n_patch; k++) poke(base + pt[3 * k], seg + h->off_patchbytes + pt[3 * k + 2], pt[3 * k + 1]);
            sync_cores();
            elog("avx512emu[%d]: offline segment not used (%s); original code restored\n", getpid(), (e && *e == '0') ? "AVX512EMU_OFFLINE=0" :
                 dT && !h->n_tls ? "TState offset differs: rebuild with xloff -T <tls offset in the log>" : "built for another emulator version");
            elog("avx512emu[%d]: tls offset %d (segment built for %d)\n", getpid(), tls_offset(), h->tls_build);
            return;
        }
        g_xl_base = base;
        const uint32_t* t = (const uint32_t*)(seg + h->off_tls);
        if (dT) for (uint32_t k = 0; k < h->n_tls; k++) { int32_t v; memcpy(&v, seg + t[k], 4); v += dT; memcpy(seg + t[k], &v, 4); }
        t = (const uint32_t*)(seg + h->off_reloc);
        for (uint32_t k = 0; k < h->n_reloc; k++) *(uint64_t*)(seg + t[k]) += base;
        t = (const uint32_t*)(seg + h->off_thunk);
        for (uint32_t k = 0; k < h->n_thunk; k++) *(uint64_t*)(seg + t[k]) = (uint64_t)emu_thunk;
        t = (const uint32_t*)(seg + h->off_desc);
        for (uint32_t k = 0; k < h->n_desc; k++) { Desc* d = (Desc*)(seg + t[k]); if (!(d->flags & DF_XLREL)) { d->pc += base; register_desc(d); } }
        t = (const uint32_t*)(seg + h->off_map);
        if (h->version >= 3) {   // sorted: searched in place (no 1.3M inserts at start)
            uint64_t lo = ~0ull, hi = 0;
            for (int j = 0; j < nph; j++) if (ph[j].p_type == PT_LOAD && (ph[j].p_flags & PF_X) && j != i) { lo = std::min<uint64_t>(lo, base + ph[j].p_vaddr); hi = std::max<uint64_t>(hi, base + ph[j].p_vaddr + ph[j].p_memsz); }
            map_add_sorted(base, lo, hi, t, h->n_map);
        } else for (uint32_t k = 0; k < h->n_map; k++) map_put(base + t[2 * k], base + t[2 * k + 1]);
        t = (const uint32_t*)(seg + h->off_fn);
        for (uint32_t k = 0; k < h->n_fn; k++) fn_seen(base + t[k], true);
        t = (const uint32_t*)(seg + h->off_group);
        for (uint32_t k = 0; k < h->n_group; k++) { prof_register_group(base + t[4 * k], t[4 * k + 1], base + t[4 * k + 2], t[4 * k + 3]); perfmap_group(base + t[4 * k], t[4 * k + 1], base + t[4 * k + 2]); }
        __builtin___clear_cache((char*)seg, (char*)seg + h->seg_size);
        cache_add_region(seg, h->seg_size, h->seg_size);
        stats.functions += h->n_fn; stats.groups += h->n_group;
        if (log_level >= 1) elog("avx512emu[%d]: offline segment attached: %u functions, %u groups, tls offset %d (built %d%s), %.1f ms\n", getpid(), h->n_fn, h->n_group,
                                 tls_offset(), h->tls_build, dT ? ", fixed up" : "", ms_now() - ta);
        return;
    }
}
} // namespace emu
