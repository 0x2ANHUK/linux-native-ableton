// Translation of one AVX-512 (EVEX) or opmask (VEX k*) instruction to AVX2
// code: inline where implemented, otherwise a call into the interpreter.
#pragma once
#include "asm.h"
#include "state.h"

namespace emu {

struct XlateOpts {
    uint64_t continue_at = 0;  // address after the instruction (for the caller's jmp)
    uint64_t thunk_slot = 0;   // address of a 64-bit slot holding &emu_thunk (rip-reachable)
    uint32_t zhi_zero = 0;     // zmm0-15 whose bits 256..511 are known to be zero here
    bool flags_dead = false;   // EFLAGS not live after this instruction
    uint32_t flags_live = 0x8d5; // EFLAGS bits (CF PF AF ZF SF OF) that may be read after this instruction
    int8_t xmap[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; // zmm16+i low 256 bits held in ymm xmap[i] for the group
    uint32_t gpre = 0;         // GPRs saved by the enclosing group: free scratch here
    int8_t mc_reg = -1;        // ymm the group keeps for a cached mask vector (dyn map), -1 none
    mutable uint32_t mc_key = 0;  // what mc_reg holds (mask_key), 0 = nothing; updated by the translation
    mutable bool mc_busy = false;
    int mc_seed = 1;           // bit0: blend with non-inverted mask, bit1: a k-writing compare leaves its lane mask in mc_reg (AVX512EMU_MCSEED=0: off)
    mutable bool mc_wrote_k = false;
    uint8_t* cpool = nullptr;  // group constant pool: 32-byte pre-broadcast copies of read-only {1toN} operands
    int cpool_cap = 0;         // entries
    mutable int cpool_n = 0;  // this translation wrote k and set mc_key for the new value
    // k-in-GPR cache: GPR kc_gpr[i] holds the value of k[kc_k[i]] (the TState slot stays authoritative; the group
    // invalidates the cache when the GPR or the k register may change). kc_set: this translation set it.
    static const int NKC = 3;
    mutable int8_t kc_gpr[NKC] = {-1, -1, -1}, kc_k[NKC] = {-1, -1, -1};   // entry i: GPR kc_gpr[i] holds k[kc_k[i]]; [0] newest
    mutable bool kc_set = false;
    uint32_t gdead = 0;        // GPRs dead here (not read before written again): free scratch, no save
    mutable uint32_t vused = 0, gused = 0;  // which vpre/gpre registers the code used (two-pass build)
    mutable int scr_taken = 0;             // vector scratch registers the translation took (dyn map: demand per instruction)
    bool xmap_any() const { for (int i = 0; i < 16; i++) if (xmap[i] >= 0) return true; return false; }
    uint32_t vpre = 0;         // ymm0-15 saved by the enclosing group: free scratch here
    uint32_t vdead = 0;        // ymm0-15 not read after this instruction before being overwritten (free scratch)
    uint32_t rd512 = ~0u;      // registers read as 512 bits somewhere in the function (zero their tails only then)
    uint32_t rd256 = ~0u;      // registers read as 256 bits or more
    bool no_inline = false;    // always use the interpreter
};

// Emits code for d (not the jump back). Returns false and sets *why if impossible.
bool xlate_insn(x86::Asm& a, const Desc& d, const XlateOpts& o, const char** why);
// Emits a call of the interpreter for d.
void emit_interp_call(x86::Asm& a, const Desc& d, const XlateOpts& o);
void emu_init_tables();
bool xlate_inline(x86::Asm& a, const Desc& d, const XlateOpts& o);

extern uint64_t g_interp_sites;   // translations that call the interpreter (statistics)
uint32_t mask_key(int k, int es, int c, int L, bool inv);
void emit_maskvec_key(x86::Asm& a, int reg, uint32_t key);
bool writes_k(const x86::Insn& in);
bool evex_pure_write(const x86::Insn& in);
} // namespace emu
