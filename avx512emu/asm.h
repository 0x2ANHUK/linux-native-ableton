// Small x86-64 assembler for the code the translator emits: VEX vector
// instructions, a few GPR moves, jumps. Memory operands can be %fs-relative
// (the emulator's thread state), relative to an original instruction's
// address registers, or rip-relative to an absolute target.
#pragma once
#include <stdint.h>
#include <string.h>

namespace x86 {

struct Mem {
    int8_t base = -1, index = -1; // GPR numbers, -1 none
    uint8_t scale = 0;            // log2
    int32_t disp = 0;
    uint8_t seg = 0;              // 0x64 = %fs, 0x65 = %gs, 0 none
    bool rip = false;
    uint64_t target = 0;          // rip-relative absolute target
    bool adsize32 = false;        // 0x67 prefix
    static Mem fs(int32_t off) { Mem m; m.seg = 0x64; m.disp = off; return m; }
    Mem plus(int32_t d) const { Mem m = *this; if (m.rip) m.target += d; else m.disp += d; return m; }
};

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

struct Asm {
    uint8_t* buf;     // where bytes are written
    uint64_t pc0;     // address the buffer will run at
    int n = 0, cap;
    bool overflow = false;
    Asm(uint8_t* b, int c, uint64_t at) : buf(b), pc0(at), cap(c) {}
    uint64_t pc() const { return pc0 + n; }
    void b(uint8_t v) { if (n < cap) buf[n] = v; else overflow = true; n++; }
    void d32(uint32_t v) { for (int i = 0; i < 4; i++) b(uint8_t(v >> (8 * i))); }
    void d64(uint64_t v) { for (int i = 0; i < 8; i++) b(uint8_t(v >> (8 * i))); }
    void bytes(const uint8_t* p, int k) { for (int i = 0; i < k; i++) b(p[i]); }

    // modrm + sib + disp for reg field `r` (low 3 bits used) and memory m.
    // Returns position of a rip-relative disp32 to fix after the imm (or -1).
    int modrm_mem(int r, const Mem& m) {
        r &= 7;
        if (m.rip) { b(uint8_t(0x05 | (r << 3))); int at = n; d32(0); return at; }
        if (m.base < 0 && m.index < 0) { b(uint8_t(0x04 | (r << 3))); b(0x25); d32(uint32_t(m.disp)); return -1; }
        int base = m.base, idx = m.index;
        bool sib = idx >= 0 || base < 0 || (base & 7) == 4;
        int mod;
        if (base < 0) mod = 0;
        else if (m.disp == 0 && (base & 7) != 5) mod = 0;
        else if (m.disp >= -128 && m.disp <= 127) mod = 1;
        else mod = 2;
        if (sib) {
            b(uint8_t((mod << 6) | (r << 3) | 4));
            b(uint8_t((m.scale << 6) | ((idx >= 0 ? idx : 4) & 7) << 3 | (base >= 0 ? base & 7 : 5)));
        } else {
            b(uint8_t((mod << 6) | (r << 3) | (base & 7)));
        }
        if (base < 0) d32(uint32_t(m.disp));
        else if (mod == 1) b(uint8_t(m.disp));
        else if (mod == 2) d32(uint32_t(m.disp));
        return -1;
    }
    void fix_rip(int at, const Mem& m) {
        if (at < 0) return;
        int64_t rel = int64_t(m.target) - int64_t(pc());
        if (rel != int32_t(rel)) overflow = true;
        uint32_t v = uint32_t(int32_t(rel));
        for (int i = 0; i < 4; i++) if (at + i < cap) buf[at + i] = uint8_t(v >> (8 * i));
    }
    void prefixes(const Mem* m) { if (m && m->seg) b(m->seg); if (m && m->adsize32) b(0x67); }

    // VEX instruction. map 1/2/3, pp 0..3, L 0/1, reg/vvvv vector or GPR numbers 0..15.
    // rm: register number if m == nullptr. imm < 0: none.
    void vex(int map, int pp, int W, int L, int opc, int reg, int vvvv, int rm, const Mem* m, int imm = -1) {
        prefixes(m);
        int X = 0, B = 0, R = (reg >> 3) & 1;
        if (m) { if (m->index >= 0) X = (m->index >> 3) & 1; if (m->base >= 0 && !m->rip) B = (m->base >> 3) & 1; }
        else B = (rm >> 3) & 1;
        if (map == 1 && W == 0 && X == 0 && B == 0) {
            b(0xC5); b(uint8_t((!R) << 7 | ((~vvvv) & 15) << 3 | (L << 2) | pp));
        } else {
            b(0xC4); b(uint8_t((!R) << 7 | (!X) << 6 | (!B) << 5 | map));
            b(uint8_t(W << 7 | ((~vvvv) & 15) << 3 | (L << 2) | pp));
        }
        b(uint8_t(opc));
        int at = -1;
        if (m) at = modrm_mem(reg, *m); else b(uint8_t(0xC0 | (reg & 7) << 3 | (rm & 7)));
        if (imm >= 0) b(uint8_t(imm));
        if (m) fix_rip(at, *m);
    }
    // legacy instruction: [66] [REX] [0F [38|3A]] opc modrm(reg, rm|mem) [imm]
    void leg(int opsize66, int W, int map, int opc, int reg, int rm, const Mem* m, int immsize = 0, uint64_t imm = 0, bool force_rex = false) {
        prefixes(m);
        if (opsize66) b(0x66);
        int X = 0, B = 0, R = (reg >> 3) & 1;
        if (m) { if (m->index >= 0) X = (m->index >> 3) & 1; if (m->base >= 0 && !m->rip) B = (m->base >> 3) & 1; }
        else B = (rm >> 3) & 1;
        if (W || R || X || B || force_rex) b(uint8_t(0x40 | W << 3 | R << 2 | X << 1 | B));
        if (map >= 1) b(0x0F);
        if (map == 2) b(0x38);
        if (map == 3) b(0x3A);
        b(uint8_t(opc));
        int at = -1;
        if (m) at = modrm_mem(reg, *m); else b(uint8_t(0xC0 | (reg & 7) << 3 | (rm & 7)));
        for (int i = 0; i < immsize; i++) b(uint8_t(imm >> (8 * i)));
        if (m) fix_rip(at, *m);
    }

    // ---- GPR helpers (flags untouched unless noted)
    void mov_load64(int r, const Mem& m) { leg(0, 1, 0, 0x8B, r, 0, &m); }
    void mov_store64(const Mem& m, int r) { leg(0, 1, 0, 0x89, r, 0, &m); }
    void mov_load32(int r, const Mem& m) { leg(0, 0, 0, 0x8B, r, 0, &m); }
    void mov_store32(const Mem& m, int r) { leg(0, 0, 0, 0x89, r, 0, &m); }
    void mov_store16(const Mem& m, int r) { leg(1, 0, 0, 0x89, r, 0, &m); }
    void mov_store8(const Mem& m, int r) { leg(0, 0, 0, 0x88, r, 0, &m, 0, 0, r >= 4); }
    void movzx8(int r, const Mem& m) { leg(0, 0, 1, 0xB6, r, 0, &m); }
    void movzx16(int r, const Mem& m) { leg(0, 0, 1, 0xB7, r, 0, &m); }
    void mov_rr64(int d, int s) { leg(0, 1, 0, 0x89, s, d, nullptr); }
    void mov_rr32(int d, int s) { leg(0, 0, 0, 0x89, s, d, nullptr); }
    void mov_imm64(int r, uint64_t v) { b(uint8_t(0x48 | ((r >> 3) & 1))); b(uint8_t(0xB8 | (r & 7))); d64(v); }
    void mov_imm32(int r, uint32_t v) { if (r >= 8) b(0x41); b(uint8_t(0xB8 | (r & 7))); d32(v); }
    void mov_store_imm32(const Mem& m, uint32_t v) { leg(0, 0, 0, 0xC7, 0, 0, &m, 4, v); }
    void mov_store_imm64(const Mem& m, int32_t v) { leg(0, 1, 0, 0xC7, 0, 0, &m, 4, uint32_t(v)); }
    void lea(int r, const Mem& m) { leg(0, 1, 0, 0x8D, r, 0, &m); }
    // flag-writing ALU (W: 0 = 32-bit, 1 = 64-bit)
    void alu_rr(int op8, int W, int d, int s) { leg(0, W, 0, op8 + 1, s, d, nullptr); } // op8: 0x00 add, 0x08 or, 0x20 and, 0x28 sub, 0x30 xor, 0x38 cmp
    void alu_rm(int op8, int W, int d, const Mem& m) { leg(0, W, 0, op8 + 3, d, 0, &m); }
    void alu_ri(int ext, int W, int r, int32_t v) { leg(0, W, 0, 0x81, ext, r, nullptr, 4, uint32_t(v)); } // ext: 0 add 1 or 4 and 5 sub 6 xor 7 cmp
    void shift_ri(int ext, int W, int r, int c) { leg(0, W, 0, 0xC1, ext, r, nullptr, 1, c); } // ext 4 shl, 5 shr, 7 sar, 0 rol, 1 ror
    void not_r(int W, int r) { leg(0, W, 0, 0xF7, 2, r, nullptr); } // flags untouched
    void test_rr(int W, int a, int s) { leg(0, W, 0, 0x85, s, a, nullptr); }
    void bts_ri(int W, int r, int bit) { leg(0, W, 1, 0xBA, 5, r, nullptr, 1, bit); }
    void pushfq() { b(0x9C); }
    void popfq() { b(0x9D); }
    void lahf() { b(0x9F); }
    void sahf() { b(0x9E); }
    void seto_al() { b(0x0F); b(0x90); b(0xC0); }
    void setcc(int cc, int r) { leg(0, 0, 1, 0x90 + cc, 0, r, nullptr, 0, 0, r >= 4); }
    void movzx_rr8(int d, int s) { leg(0, 0, 1, 0xB6, d, s, nullptr, 0, 0, s >= 4); }
    void popcnt_rr(int W, int d, int s) { b(0xF3); if (W || d >= 8 || s >= 8) b(uint8_t(0x40 | W << 3 | ((d >> 3) & 1) << 2 | ((s >> 3) & 1))); b(0x0F); b(0xB8); b(uint8_t(0xC0 | (d & 7) << 3 | (s & 7))); }

    // ---- control flow
    void jmp(uint64_t target) { b(0xE9); int64_t rel = int64_t(target) - int64_t(pc() + 4); if (rel != int32_t(rel)) overflow = true; d32(uint32_t(int32_t(rel))); }
    void jcc(int cc, uint64_t target) { b(0x0F); b(uint8_t(0x80 + cc)); int64_t rel = int64_t(target) - int64_t(pc() + 4); if (rel != int32_t(rel)) overflow = true; d32(uint32_t(int32_t(rel))); }
    int jcc_fwd(int cc) { b(0x0F); b(uint8_t(0x80 + cc)); int at = n; d32(0); return at; } // patch with bind()
    int jmp_fwd() { b(0xE9); int at = n; d32(0); return at; }
    void bind(int at) { uint32_t rel = uint32_t(n - (at + 4)); for (int i = 0; i < 4; i++) if (at + i < cap) buf[at + i] = uint8_t(rel >> (8 * i)); }
    void call_abs_via(int r, uint64_t target) { mov_imm64(r, target); leg(0, 0, 0, 0xFF, 2, r, nullptr); }
    void int3() { b(0xCC); }
};

} // namespace x86
