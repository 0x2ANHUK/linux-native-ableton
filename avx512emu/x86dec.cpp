#include "x86dec.h"
#include <string.h>

namespace x86 {

// Operand layout codes for legacy opcodes.
enum : uint8_t {
    N = 0,      // no modrm, no imm
    M = 1,      // modrm
    I8 = 2,     // imm8
    IZ = 4,     // imm16/32 (operand size)
    I16 = 8,
    BAD = 16,   // invalid in 64-bit mode / prefix handled elsewhere
    MI8 = M | I8, MIZ = M | IZ,
    IV = 32,    // B8..BF: imm32, or imm64 with REX.W
    MOFFS = 64, // A0..A3
    GRP3 = 128, // F6/F7: imm only for /0 /1
};

static const uint8_t one[256] = {
    // 00-0F
    M, M, M, M, I8, IZ, BAD, BAD, M, M, M, M, I8, IZ, BAD, BAD,
    // 10-1F
    M, M, M, M, I8, IZ, BAD, BAD, M, M, M, M, I8, IZ, BAD, BAD,
    // 20-2F
    M, M, M, M, I8, IZ, BAD, BAD, M, M, M, M, I8, IZ, BAD, BAD,
    // 30-3F
    M, M, M, M, I8, IZ, BAD, BAD, M, M, M, M, I8, IZ, BAD, BAD,
    // 40-4F REX (handled before lookup)
    BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD,
    // 50-5F
    N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N,
    // 60-6F
    BAD, BAD, BAD, M, BAD, BAD, BAD, BAD, IZ, MIZ, I8, MI8, N, N, N, N,
    // 70-7F jcc rel8
    I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8, I8,
    // 80-8F
    MI8, MIZ, BAD, MI8, M, M, M, M, M, M, M, M, M, M, M, M,
    // 90-9F
    N, N, N, N, N, N, N, N, N, N, BAD, N, N, N, N, N,
    // A0-AF
    MOFFS, MOFFS, MOFFS, MOFFS, N, N, N, N, I8, IZ, N, N, N, N, N, N,
    // B0-BF
    I8, I8, I8, I8, I8, I8, I8, I8, IV, IV, IV, IV, IV, IV, IV, IV,
    // C0-CF
    MI8, MI8, I16, N, BAD, BAD, MI8, MIZ, I16 | I8, N, I16, N, N, I8, BAD, N,
    // D0-DF
    M, M, M, M, BAD, BAD, BAD, N, M, M, M, M, M, M, M, M,
    // E0-EF
    I8, I8, I8, I8, I8, I8, I8, I8, IZ, IZ, BAD, I8, N, N, N, N,
    // F0-FF
    BAD, N, BAD, BAD, N, N, M | GRP3, M | GRP3, N, N, N, N, N, N, M, M,
};

// 0F map: modrm presence and imm8.
static uint8_t two_info(uint8_t op) {
    if (op >= 0x80 && op <= 0x8F) return IZ; // jcc rel32 (always 32 in 64-bit)
    switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x37:
    case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return N;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        return N;
    case 0x04: case 0x0A: case 0x0C: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x36: case 0x39: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x7A: case 0x7B:
        return BAD;
    case 0x0F: return MI8; // 3DNow!
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        return MI8;
    default: return M;
    }
}

// VEX/EVEX map 1 instructions with imm8.
static bool vex_map1_imm(uint8_t op) {
    return (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
}

// modrm, sib, displacement starting at p[i]. Returns new index or -1.
static int modrm_tail(const uint8_t* p, int i, int avail, Insn& in) {
    if (i >= avail) return -1;
    in.has_modrm = true;
    in.modrm = p[i++];
    in.mod = in.modrm >> 6; in.reg3 = (in.modrm >> 3) & 7; in.rm3 = in.modrm & 7;
    in.disp_size = 0;
    if (in.mod != 3) {
        if (in.rm3 == 4) {
            if (i >= avail) return -1;
            in.has_sib = true; in.sib = p[i++];
            if (in.mod == 0 && (in.sib & 7) == 5) in.disp_size = 4;
        } else if (in.mod == 0 && in.rm3 == 5) {
            in.riprel = true; in.disp_size = 4;
        }
        if (in.mod == 1) in.disp_size = 1;
        if (in.mod == 2) in.disp_size = 4;
    }
    in.disp_off = (uint8_t)i;
    if (in.disp_size) {
        if (i + in.disp_size > avail) return -1;
        if (in.disp_size == 1) in.disp_raw = (int8_t)p[i];
        else { int32_t d; memcpy(&d, p + i, 4); in.disp_raw = d; }
        i += in.disp_size;
    }
    return i;
}

int decode(const uint8_t* p, int avail, Insn& in) {
    memset(&in, 0, sizeof in);
    int i = 0;
    if (avail > 15) avail = 15;
    // legacy prefixes
    for (;; i++) {
        if (i >= avail) return 0;
        uint8_t c = p[i];
        if (c == 0x66) in.opsize = 1;
        else if (c == 0x67) in.adsize = 1;
        else if (c == 0xF2 || c == 0xF3) in.rep = c;
        else if (c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 || c == 0x65) in.seg = c;
        else if (c == 0xF0) {}
        else break;
    }
    in.npfx = (uint8_t)i;
    uint8_t c = p[i];
    if (c >= 0x40 && c <= 0x4F) {
        in.rex = c; i++;
        if (i >= avail) return 0;
        c = p[i];
        in.W = (in.rex >> 3) & 1; in.R = (in.rex >> 2) & 1; in.X = (in.rex >> 1) & 1; in.B = in.rex & 1;
    }
    if (c == 0xC4 || c == 0xC5 || c == 0x62) {
        if (in.rex) return 0; // REX before VEX/EVEX is invalid
        if (c == 0xC5) {
            if (i + 2 > avail) return 0;
            uint8_t b1 = p[i + 1];
            in.enc = VEX; in.R = !(b1 >> 7); in.vvvv = (~b1 >> 3) & 15; in.L = (b1 >> 2) & 1; in.pp = b1 & 3; in.map = 1;
            i += 2;
        } else if (c == 0xC4) {
            if (i + 3 > avail) return 0;
            uint8_t b1 = p[i + 1], b2 = p[i + 2];
            in.enc = VEX; in.R = !(b1 >> 7); in.X = !((b1 >> 6) & 1); in.B = !((b1 >> 5) & 1);
            in.map = b1 & 31; in.W = b2 >> 7; in.vvvv = (~b2 >> 3) & 15; in.L = (b2 >> 2) & 1; in.pp = b2 & 3;
            if (in.map < 1 || in.map > 3) return 0;
            i += 3;
        } else {
            if (i + 4 > avail) return 0;
            uint8_t p0 = p[i + 1], p1 = p[i + 2], p2 = p[i + 3];
            if ((p0 & 0x08) || !(p1 & 0x04)) return 0;
            in.enc = EVEX;
            in.R = !(p0 >> 7); in.X = !((p0 >> 6) & 1); in.B = !((p0 >> 5) & 1); in.Rp = !((p0 >> 4) & 1);
            in.map = p0 & 7;
            in.W = p1 >> 7; in.vvvv = (~p1 >> 3) & 15; in.pp = p1 & 3;
            in.z = p2 >> 7; in.L = (p2 >> 5) & 3; in.b = (p2 >> 4) & 1; in.Vp = !((p2 >> 3) & 1); in.aaa = p2 & 7;
            in.vvvv |= in.Vp << 4;
            if (in.map < 1 || in.map > 3) return 0; // FP16 maps 5/6 not handled
            i += 4;
        }
        if (i >= avail) return 0;
        in.opc_off = (uint8_t)i;
        in.opc = p[i++];
        if (in.enc == VEX && in.map == 1 && in.opc == 0x77) { in.len = (uint8_t)i; in.disp_off = in.imm_off = (uint8_t)i; return i; } // vzeroupper/vzeroall
        i = modrm_tail(p, i, avail, in);
        if (i < 0) return 0;
        bool imm = in.map == 3 || (in.map == 1 && vex_map1_imm(in.opc));
        if (imm) { in.imm_off = (uint8_t)i; in.imm_size = 1; i += 1; }
        if (i > avail) return 0;
        if (imm) { in.imm8 = p[in.imm_off]; in.imm = (int8_t)in.imm8; }
        in.flow = F_NONE;
        in.len = (uint8_t)i;
        return i;
    }
    // legacy
    uint8_t info;
    in.map = 0;
    if (c == 0x0F) {
        if (++i >= avail) return 0;
        c = p[i];
        if (c == 0x38) { in.map = 2; if (++i >= avail) return 0; c = p[i]; info = M; }
        else if (c == 0x3A) { in.map = 3; if (++i >= avail) return 0; c = p[i]; info = MI8; }
        else { in.map = 1; info = two_info(c); }
    } else {
        info = one[c];
    }
    in.opc_off = (uint8_t)i;
    in.opc = c;
    i++;
    if (info & BAD) return 0;
    if (info & M) {
        i = modrm_tail(p, i, avail, in);
        if (i < 0) return 0;
    } else in.disp_off = (uint8_t)i;
    in.imm_off = (uint8_t)i;
    int imm = 0;
    if (info & I16) imm += 2;
    if (info & I8) imm += 1;
    if (info & IZ) imm += (in.map == 1) ? 4 : ((in.opsize && !in.W) ? 2 : 4);
    if (info & IV) imm += in.W ? 8 : (in.opsize ? 2 : 4);
    if (info & MOFFS) { in.disp_size = in.adsize ? 4 : 8; in.disp_off = (uint8_t)i; imm = 0; i += in.disp_size; in.imm_off = (uint8_t)i; }
    if (info & GRP3) { if (in.reg3 < 2) imm += (c == 0xF6) ? 1 : ((in.opsize && !in.W) ? 2 : 4); }
    // 0F map special cases: 0F 01 /x with mod=3 has no extra bytes; 0F 0F 3DNow imm8 ok
    in.imm_size = (uint8_t)imm;
    i += imm;
    if (i > avail) return 0;
    if (imm) {
        in.imm8 = p[in.imm_off];
        uint64_t v = 0; int sz = imm > 8 ? 8 : imm;
        for (int k = 0; k < sz; k++) v |= uint64_t(p[in.imm_off + k]) << (8 * k);
        if (sz < 8) { uint64_t sb = 1ull << (8 * sz - 1); v = (v ^ sb) - sb; }
        in.imm = int64_t(v);
    }
    in.len = (uint8_t)i;

    // control flow
    in.flow = F_NONE;
    if (in.map == 0) {
        if (c >= 0x70 && c <= 0x7F) { in.flow = F_JCC; in.rel_size = 1; }
        else if (c >= 0xE0 && c <= 0xE3) { in.flow = F_JCC; in.rel_size = 1; }
        else if (c == 0xEB) { in.flow = F_JMP; in.rel_size = 1; }
        else if (c == 0xE9) { in.flow = F_JMP; in.rel_size = 4; }
        else if (c == 0xE8) { in.flow = F_CALL; in.rel_size = 4; }
        else if (c == 0xC3 || c == 0xC2 || c == 0xCB || c == 0xCA || c == 0xCF) in.flow = F_RET;
        else if (c == 0xFF && (in.reg3 >= 2 && in.reg3 <= 5)) in.flow = F_INDIRECT;
        else if (c == 0xCC || c == 0xCD || c == 0xF4 || c == 0xF1 || c == 0xCE) in.flow = F_STOP;
        else if (c == 0xC7 && in.modrm == 0xF8) in.flow = F_STOP; // xbegin
    } else if (in.map == 1) {
        if (c >= 0x80 && c <= 0x8F) { in.flow = F_JCC; in.rel_size = 4; }
        else if (c == 0x0B || c == 0x05 || c == 0x07 || c == 0x34 || c == 0x35 || c == 0xFF || c == 0xB9) in.flow = F_STOP;
    }
    if (in.opsize && (in.flow == F_JCC || in.flow == F_JMP || in.flow == F_CALL)) in.flow = F_STOP; // odd forms
    return i;
}

} // namespace x86
