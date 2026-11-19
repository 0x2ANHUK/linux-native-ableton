// Offline translation ("XL segment"): translated code for a whole ELF object, made ahead of time by tools
// xloff and stored in an extra PT_LOAD segment of a rewritten copy of the object. The original code in the
// copy is already patched (jmp into the segment). At start the emulator library finds the segment
// (offline_attach in func.cpp), applies the fixups below and registers everything as if it had translated
// the functions itself; when the segment does not match this library (ABI), it restores the original bytes
// and the functions are translated lazily as usual.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "state.h"
#include "x86dec.h"

namespace emu {

static const char XL_MAGIC[8] = {'A', 'V', 'X', '5', 'X', 'L', 'S', 'G'};
static const uint32_t XL_VERSION = 3;   // 3: map table sorted by original address
static const int32_t XL_TLS_BUILD = 0x40000000;   // TState offset used while generating (fixed up to the real one)

// all offsets are relative to the start of the segment; addresses are image-relative (vaddr)
struct XlHeader {
    char magic[8];
    uint32_t version, abi;
    uint64_t seg_vaddr, seg_size;
    int32_t tls_build; uint32_t pad;
    uint32_t n_tls, n_reloc, n_thunk, n_desc, n_map, n_fn, n_patch, n_group;
    uint64_t off_tls;     // u32[n_tls]: offsets of disp32 fields holding XL_TLS_BUILD + field offset
    uint64_t off_reloc;   // u32[n_reloc]: offsets of u64 slots holding a vaddr (add the load base)
    uint64_t off_thunk;   // u32[n_thunk]: offsets of u64 slots for &emu_thunk
    uint64_t off_desc;    // u32[n_desc]: offsets of Desc (pc is a vaddr)
    uint64_t off_map;     // u32[2 * n_map]: original vaddr, translated vaddr
    uint64_t off_fn;      // u32[n_fn]: translated function starts (vaddr)
    uint64_t off_patch;   // u32[3 * n_patch]: vaddr, length, offset of the original bytes (from off_patchbytes)
    uint64_t off_patchbytes;
    uint64_t off_group;   // u32[4 * n_group]: code vaddr, code length, original vaddr, specials
};

// fingerprint of everything the generated code depends on in the library
static inline uint32_t xl_abi() {
    uint32_t h = 2166136261u;
    auto m = [&](uint64_t v) { h = (h ^ uint32_t(v)) * 16777619u; h = (h ^ uint32_t(v >> 32)) * 16777619u; };
    m(XL_VERSION); m(sizeof(TState)); m(sizeof(Desc)); m(sizeof(x86::Insn)); m(sizeof(Frame));
#define F(x) m(offsetof(TState, x))
    F(hw); F(zhi); F(zx); F(k); F(ysave); F(gsave); F(mxcsr_save); F(misc); F(tmp); F(c_bitd); F(c_bitq); F(c_bitw);
    F(c_bspread); F(c_bbit); F(c_ones); F(c_sign32); F(c_sign64); F(c_zero); F(c_sign0); F(c_sign0q); F(c_kmask);
    F(c_2p63f); F(c_2p63d); F(c_2p32d); F(c_m1f); F(c_2p31f); F(c_2p32f); F(c_4330); F(c_4530); F(c_2p84p52);
    F(c_m1d); F(c_2p64d); F(c_2pm32d); F(c_2p32dv); F(c_pack02); F(depth);
#undef F
    m(offsetof(Desc, pc)); m(offsetof(Desc, in)); m(offsetof(Desc, flags)); m(offsetof(Desc, count)); m(DF_XLREL);
    return h;
}

} // namespace emu
