// AES and carry-less multiply per 128-bit lane, GF(2^8) instructions, and
// moves between vectors and opmasks (vpmovm2*, vpmov*2m).
#include "interp.h"
#include <immintrin.h>

namespace emu {

static uint8_t gf_mul(uint8_t a, uint8_t b) { // GF(2^8) modulo x^8+x^4+x^3+x+1
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        uint8_t hi = a & 0x80; a <<= 1; if (hi) a ^= 0x1b; b >>= 1;
    }
    return r;
}
static uint8_t gf_inv(uint8_t x) { // x^254
    if (!x) return 0;
    uint8_t r = 1, p = x;
    for (int e = 254; e; e >>= 1) { if (e & 1) r = gf_mul(r, p); p = gf_mul(p, p); }
    return r;
}
static uint8_t affine(uint64_t A, uint8_t x, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t row = uint8_t(A >> (8 * (7 - i)));
        r |= uint8_t((__builtin_popcount(row & x) & 1) << i);
    }
    return r ^ b;
}

bool fam_misc(IC& c) {
    const x86::Insn& in = c.in;
    // VEX GFNI / VAES / VPCLMULQDQ: emulated on CPUs without them (classify.cpp is_special); same code as EVEX, no mask
    bool vexnew = in.enc == x86::VEX && in.pp == 1 && ((in.map == 2 && (in.opc == 0xcf || (in.opc >= 0xdc && in.opc <= 0xdf))) ||
                                                       (in.map == 3 && (in.opc == 0xce || in.opc == 0xcf || in.opc == 0x44)));
    if (in.enc != x86::EVEX && !vexnew) return false;
    if (in.map == 2 && in.pp == 1 && in.opc >= 0xdc && in.opc <= 0xdf && !in.W) { // vaesenc/enclast/dec/declast
        Z a = c.R(c.vvvv()), b, r; c.rm_src(b, c.vl, 16, false);
        for (int l = 0; l < c.vl / 16; l++) {
            __m128i x = _mm_loadu_si128((const __m128i*)(a.b + 16 * l)), y = _mm_loadu_si128((const __m128i*)(b.b + 16 * l)), z;
            switch (in.opc) {
            case 0xdc: z = _mm_aesenc_si128(x, y); break;
            case 0xdd: z = _mm_aesenclast_si128(x, y); break;
            case 0xde: z = _mm_aesdec_si128(x, y); break;
            default: z = _mm_aesdeclast_si128(x, y); break;
            }
            _mm_storeu_si128((__m128i*)(r.b + 16 * l), z);
        }
        c.write_raw(c.reg(), r, c.vl);
        return true;
    }
    if (in.map == 3 && in.pp == 1 && in.opc == 0x44 && !in.W) { // vpclmulqdq
        Z a = c.R(c.vvvv()), b, r; c.rm_src(b, c.vl, 16, false);
        int imm = c.imm();
        for (int l = 0; l < c.vl / 16; l++) {
            uint64_t x = a.q[2 * l + (imm & 1)], y = b.q[2 * l + ((imm >> 4) & 1)], lo = 0, hi = 0;
            for (int i = 0; i < 64; i++) if ((y >> i) & 1) { lo ^= x << i; if (i) hi ^= x >> (64 - i); }
            r.q[2 * l] = lo; r.q[2 * l + 1] = hi;
        }
        c.write_raw(c.reg(), r, c.vl);
        return true;
    }
    if (in.map == 2 && in.pp == 1 && in.opc == 0xcf && !in.W) { // vgf2p8mulb
        ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return gf_mul(a, b); });
        return true;
    }
    if (in.map == 3 && in.pp == 1 && (in.opc == 0xce || in.opc == 0xcf) && in.W) { // vgf2p8affine(inv)qb
        Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r);
        c.rm_src(b, c.vl, 8);
        uint8_t imm = uint8_t(c.imm()); bool inv = in.opc == 0xcf;
        for (int i = 0; i < c.vl; i++) {
            uint8_t x = a.b[i]; if (inv) x = gf_inv(x);
            r.b[i] = affine(b.q[i / 8], x, imm);
        }
        c.write(c.reg(), r, 1, c.vl);
        return true;
    }
    if (in.map == 2 && in.pp == 2 && in.mod == 3) {
        int es = 0;
        if (in.opc == 0x28 || in.opc == 0x29) es = in.W ? 2 : 1;
        if (in.opc == 0x38 || in.opc == 0x39) es = in.W ? 8 : 4;
        if (!es) return false;
        int n = c.vl / es;
        if (in.opc == 0x28 || in.opc == 0x38) { // vpmovm2b/w/d/q: k (rm) -> vector (reg)
            uint64_t k = c.K[in.rm3];
            Z r; memset(&r, 0, sizeof r);
            for (int i = 0; i < n; i++) if ((k >> i) & 1) memset(r.b + i * es, 0xff, es);
            c.write_raw(c.reg(), r, c.vl);
        } else { // vpmovb/w/d/q2m: vector (rm) -> k (reg)
            const Z& s = c.R(c.rmreg());
            uint64_t k = 0;
            for (int i = 0; i < n; i++) if (s.b[i * es + es - 1] & 0x80) k |= 1ull << i;
            c.K[in.reg3] = k;
        }
        return true;
    }
    return false;
}

} // namespace emu
