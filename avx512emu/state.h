// Per-thread emulator state, reached from translated code as %fs:offset.
// AVX2 hardware holds the low 256 bits of zmm0-15; the rest lives here.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "x86dec.h"

namespace emu {

struct alignas(64) TState {
    uint8_t hw[16][32];     // ymm0-15 saved by the helper thunk (must stay first: thunk.S)
    uint8_t zhi[16][32];    // bits 256..511 of zmm0-15
    uint8_t zx[16][64];     // zmm16-31
    uint64_t k[8];          // opmask registers
    uint8_t ysave[16][32];  // inline translations: spill slots for scratch ymm
    uint64_t gsave[16];     // inline translations: spill slots for scratch GPRs
    uint32_t mxcsr_save, mxcsr_tmp;
    uint64_t misc[8];
    uint8_t tmp[128];
    // constants (initialized from the TLS image in every thread)
    uint32_t c_bitd[2][8];  // 1 << (8c + j)
    uint64_t c_bitq[2][4];  // 1 << (4c + j)
    uint16_t c_bitw[16];    // 1 << j
    uint8_t c_bspread[32];  // byte j <- byte j/8 (within each 128-bit lane after a dword broadcast)
    uint8_t c_bbit[32];     // 1 << (j % 8)
    uint32_t c_ones[8];
    uint32_t c_sign32[8];
    uint64_t c_sign64[4];
    uint8_t c_zero[64];
    uint32_t c_sign0[8];    // sign bit in dword 0 only
    uint64_t c_sign0q[4];   // sign bit in qword 0 only
    uint64_t c_kmask[4];    // 0xff, 0xffff, 0xffffffff, all (k widths 8/16/32/64)
    float c_2p63f; uint32_t pad0; double c_2p63d, c_2p32d;
    alignas(32) float c_m1f[8], c_2p31f[8], c_2p32f[8];
    uint64_t c_4330[4], c_4530[4];
    double c_2p84p52[4];
    double c_m1d[4], c_2p64d[4], c_2pm32d[4], c_2p32dv[4];
    uint32_t c_pack02[8];
    uint64_t depth;         // >0 while the interpreter runs on this thread
    alignas(32) uint32_t c_lane0[8];   // all ones in dword 0 only (scalar compare -> lane mask)
    uint64_t c_lane0q[4];   // all ones in qword 0 only
    alignas(32) uint8_t c_b0f[32];   // 0x0f in every byte (vpshufb index without bit 7)
    alignas(32) uint32_t c_q01[8];   // 0, 1, 0, 1, ... (qword index -> dword index pair)
    double c_2p84p63p52[4];          // 2^84 + 2^63 + 2^52 (signed int64 -> double)
    alignas(32) uint8_t xbuf[128];   // inline translations: vector elements moved through GPRs (vcvt(u)qq2ps, masked vmovdqu8/16)
    alignas(32) uint8_t c_popnib[32]; // number of set bits of 0..15 (vpshufb table, both 128-bit lanes)
    alignas(32) uint8_t c_b01[32];   // 1 in every byte
    alignas(32) uint16_t c_w01[16];  // 1 in every word
    alignas(32) uint8_t c_trunc[5][32]; // vpshufb: low byte/word of each word/dword/qword to the lane start (wb db qb dw qw)
};

extern __thread TState tst __attribute__((tls_model("initial-exec")));
int32_t tls_offset();       // %fs-relative offset of tst

// Static description of one translated instruction (lives in the code cache).
struct Desc {
    uint64_t pc;            // original address
    uint8_t bytes[16];
    x86::Insn in;
    uint32_t flags;
    uint64_t count;         // interpreter executions (profiling)
};

// Desc.flags: pc is relative to g_xl_base (offline segment, see offline.h) - the segment stays unmodified
static const uint32_t DF_XLREL = 1u << 31;
// Desc.flags: not an instruction - a group-entry state capture (AVX512EMU_CAPTURE, see CapInfo)
static const uint32_t DF_CAPTURE = 1u << 30;

// AVX512EMU_CAPTURE=DIR: every translated group starts with a countdown; at 0 the thunk records the entry
// state (registers, opmasks, MXCSR, memory windows around each memory operand) for offline replay
// (gharness2 -r). CapOp: one memory operand of the group; kind 0 base+index*scale+disp, 1 VSIB with dword
// indices, 2 VSIB qword, 3 rip-relative (abs); +16 the base register is written earlier in the group
// (its entry value only approximates the address: a window around the register value is kept too).
struct CapOp { uint64_t abs; int32_t disp; int8_t base, index, scale, kind; };
struct CapInfo {
    Desc d;                 // first: the thunk passes &d
    int64_t countdown;      // decremented at every group entry; capture at 0
    uint64_t start, end;    // group [start, end)
    uint32_t ncap, nops;
    CapOp ops[];
};
extern uint64_t g_xl_base;
inline uint64_t desc_pc(const Desc* d) { return d->pc + ((d->flags & DF_XLREL) ? g_xl_base : 0); }

// Stack frame built by emu_thunk (thunk.S).
struct Frame {
    uint64_t rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags, ret, rax;
};
static_assert(offsetof(Frame, rax) == 128, "thunk layout");

} // namespace emu

extern "C" void emu_thunk();
extern "C" void emu_interp(emu::Frame* f, const emu::Desc* d);
