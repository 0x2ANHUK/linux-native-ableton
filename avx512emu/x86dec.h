// x86-64 instruction decoder: length and layout of any instruction, and the
// fields of EVEX (AVX-512) instructions. Enough for forward decoding of
// compiler output, relocating copied instructions, and translating EVEX ones.
#pragma once
#include <stdint.h>

namespace x86 {

enum Enc : uint8_t { LEGACY, VEX, EVEX };

enum Flow : uint8_t {
    F_NONE,     // falls through
    F_JCC,      // conditional branch, rel8/rel32 (also loop/jrcxz)
    F_JMP,      // unconditional direct jump
    F_CALL,     // direct call
    F_RET,
    F_INDIRECT, // jmp/call through register or memory
    F_STOP,     // ud2, hlt, int3, syscall-like or anything not to translate past
};

struct Insn {
    uint8_t len;          // total length, 0 = undecodable
    Enc enc;
    uint8_t npfx;         // legacy prefix bytes before REX/VEX/EVEX/opcode
    uint8_t opsize, adsize, rep, seg; // 0x66 / 0x67 seen, F2/F3 (last), segment prefix byte or 0
    uint8_t rex;          // REX byte or 0
    uint8_t map;          // 0 = one-byte, 1 = 0F, 2 = 0F38, 3 = 0F3A (VEX/EVEX: from the prefix)
    uint8_t opc;
    uint8_t opc_off;      // offset of the opcode byte
    bool has_modrm;
    uint8_t modrm, sib;
    uint8_t mod, reg3, rm3; // raw modrm fields
    bool has_sib;
    uint8_t disp_off, disp_size; // displacement position and size (0, 1, 4; 8 for moffs)
    int32_t disp_raw;     // displacement as encoded (disp8 not scaled)
    uint8_t imm_off, imm_size;   // immediate position and size (total, both for ENTER)
    uint8_t imm8;         // first immediate byte
    int64_t imm;          // immediate value (sign-extended from its size)
    bool riprel;
    Flow flow;
    uint8_t rel_size;     // branch: 1 or 4, its rel field at imm_off
    // VEX / EVEX fields (decoded, not inverted)
    uint8_t pp;           // 0 none, 1 66, 2 F3, 3 F2
    uint8_t W, L;         // L: 0 = 128, 1 = 256, 2 = 512 (EVEX L'L; for b=1 reg forms it is RC)
    uint8_t vvvv;         // 0..31 (EVEX V' included)
    uint8_t R, X, B, Rp, Vp; // extension bits as 0/1 (already un-inverted)
    uint8_t z, b, aaa;    // EVEX
};

// Decodes one instruction at p (at most `avail` bytes readable). Returns len (0 on failure).
int decode(const uint8_t* p, int avail, Insn& in);

// Register numbers (0..31) of an EVEX/VEX instruction's operands.
inline int reg_of(const Insn& in) { return in.reg3 | (in.R << 3) | (in.Rp << 4); }
// rm as a register (mod == 3): for EVEX X extends it to 0..31.
inline int rm_reg_of(const Insn& in) { return in.rm3 | (in.B << 3) | (in.enc == EVEX ? (in.X << 4) : 0); }
inline int base_of(const Insn& in) { return in.has_sib ? ((in.sib & 7) | (in.B << 3)) : (in.rm3 | (in.B << 3)); }
inline int index_of(const Insn& in) { return ((in.sib >> 3) & 7) | (in.X << 3); } // 4 = none (unless VSIB)
inline int scale_of(const Insn& in) { return in.has_sib ? (in.sib >> 6) : 0; }
inline bool has_base(const Insn& in) { return !in.riprel && !(in.mod == 0 && in.has_sib && (in.sib & 7) == 5); }
inline bool has_index(const Insn& in) { return in.has_sib && index_of(in) != 4; }

} // namespace x86
