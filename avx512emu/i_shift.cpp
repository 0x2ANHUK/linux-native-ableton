// Shifts and rotates: by immediate, by xmm count, per element (variable),
// byte shifts per 128-bit lane, rotates, and the VBMI2 double shifts.
#include "interp.h"
#include <type_traits>

namespace emu {

// kind: 0 logical left, 1 logical right, 2 arithmetic right; count unbounded
template <class T> static T sh(T v, uint64_t n, int kind) {
    typedef typename std::make_signed<T>::type S;
    const unsigned bits = sizeof(T) * 8;
    if (kind == 2) { if (n >= bits) n = bits - 1; return T(S(v) >> n); }
    if (n >= bits) return 0;
    return kind == 0 ? T(v << n) : T(v >> n);
}
template <class T> static T rol(T v, unsigned n) { const unsigned b = sizeof(T) * 8; n &= b - 1; return n ? T((v << n) | (v >> (b - n))) : v; }
template <class T> static T ror(T v, unsigned n) { const unsigned b = sizeof(T) * 8; n &= b - 1; return n ? T((v >> n) | (v << (b - n))) : v; }
// high part of (hi:lo) << n, n < bits
template <class T> static T shld(T hi, T lo, unsigned n) { const unsigned b = sizeof(T) * 8; n &= b - 1; return n ? T((hi << n) | (lo >> (b - n))) : hi; }
// low part of (hi:lo) >> n
template <class T> static T shrd(T hi, T lo, unsigned n) { const unsigned b = sizeof(T) * 8; n &= b - 1; return n ? T((lo >> n) | (hi << (b - n))) : lo; }

// immediate shift/rotate: dst = vvvv, src = rm
template <class T> static void imm_op(IC& c, int op) {
    const int es = sizeof(T), n = c.vl / es;
    unsigned cnt = c.imm();
    Z s, r; memset(&r, 0, sizeof r);
    c.rm_src(s, c.vl, es);
    for (int i = 0; i < n; i++) {
        T v; memcpy(&v, s.b + i * es, es);
        T o;
        switch (op) {
        case 0: o = sh<T>(v, cnt, 0); break;
        case 1: o = sh<T>(v, cnt, 1); break;
        case 2: o = sh<T>(v, cnt, 2); break;
        case 3: o = rol<T>(v, cnt); break;
        default: o = ror<T>(v, cnt); break;
        }
        memcpy(r.b + i * es, &o, es);
    }
    c.write(c.vvvv(), r, es, c.vl);
}

// shift by the low qword of xmm/m128: dst = reg, src = vvvv
template <class T> static void xmm_shift(IC& c, int kind) {
    const int es = sizeof(T), n = c.vl / es;
    uint64_t cnt = c.mem ? c.ld<uint64_t>(c.ea) : c.R(c.rmreg()).q[0];
    Z a = c.R(c.vvvv()), r; memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) { T v; memcpy(&v, a.b + i * es, es); T o = sh<T>(v, cnt, kind); memcpy(r.b + i * es, &o, es); }
    c.write(c.reg(), r, es, c.vl);
}

// per-element: dst = reg, src = vvvv, count = rm
template <class T> static void var_op(IC& c, int op) {
    ew3<T>(c, [op](T a, T b, int) -> T {
        switch (op) {
        case 0: return sh<T>(a, uint64_t(b), 0);
        case 1: return sh<T>(a, uint64_t(b), 1);
        case 2: return sh<T>(a, uint64_t(b), 2);
        case 3: return rol<T>(a, unsigned(b));
        default: return ror<T>(a, unsigned(b));
        }
    });
}

bool fam_shift(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    const int W = in.W;
    if (in.map == 1 && in.pp == 1) {
        int ext = in.reg3;
        switch (in.opc) {
        case 0x71: // word: /2 srl /4 sra /6 sll
            if (ext == 2) imm_op<uint16_t>(c, 1); else if (ext == 4) imm_op<uint16_t>(c, 2); else if (ext == 6) imm_op<uint16_t>(c, 0); else return false;
            return true;
        case 0x72: // /0 ror /1 rol /2 srl /4 sra /6 sll
            switch (ext) {
            case 0: if (W) imm_op<uint64_t>(c, 4); else imm_op<uint32_t>(c, 4); return true;
            case 1: if (W) imm_op<uint64_t>(c, 3); else imm_op<uint32_t>(c, 3); return true;
            case 2: if (W) return false; imm_op<uint32_t>(c, 1); return true;
            case 4: if (W) imm_op<uint64_t>(c, 2); else imm_op<uint32_t>(c, 2); return true;
            case 6: if (W) return false; imm_op<uint32_t>(c, 0); return true;
            }
            return false;
        case 0x73: // /2 srlq /3 srldq /6 sllq /7 slldq
            if (ext == 2 || ext == 6) { if (!W) return false; imm_op<uint64_t>(c, ext == 2 ? 1 : 0); return true; }
            if (ext == 3 || ext == 7) {
                unsigned n = c.imm(); Z s, r; memset(&r, 0, sizeof r);
                c.rm_src(s, c.vl, 16, false);
                for (int l = 0; l < c.vl; l += 16)
                    for (int j = 0; j < 16; j++) {
                        int from = ext == 3 ? j + int(n) : j - int(n);
                        r.b[l + j] = (n < 16 && from >= 0 && from < 16) ? s.b[l + from] : 0;
                    }
                c.write_raw(c.vvvv(), r, c.vl);
                return true;
            }
            return false;
        case 0xd1: xmm_shift<uint16_t>(c, 1); return true;
        case 0xd2: if (W) return false; xmm_shift<uint32_t>(c, 1); return true;
        case 0xd3: if (!W) return false; xmm_shift<uint64_t>(c, 1); return true;
        case 0xe1: xmm_shift<uint16_t>(c, 2); return true;
        case 0xe2: if (W) xmm_shift<uint64_t>(c, 2); else xmm_shift<uint32_t>(c, 2); return true;
        case 0xf1: xmm_shift<uint16_t>(c, 0); return true;
        case 0xf2: if (W) return false; xmm_shift<uint32_t>(c, 0); return true;
        case 0xf3: if (!W) return false; xmm_shift<uint64_t>(c, 0); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x10: if (!W) return false; var_op<uint16_t>(c, 1); return true;
        case 0x11: if (!W) return false; var_op<uint16_t>(c, 2); return true;
        case 0x12: if (!W) return false; var_op<uint16_t>(c, 0); return true;
        case 0x45: if (W) var_op<uint64_t>(c, 1); else var_op<uint32_t>(c, 1); return true;
        case 0x46: if (W) var_op<uint64_t>(c, 2); else var_op<uint32_t>(c, 2); return true;
        case 0x47: if (W) var_op<uint64_t>(c, 0); else var_op<uint32_t>(c, 0); return true;
        case 0x14: if (W) var_op<uint64_t>(c, 4); else var_op<uint32_t>(c, 4); return true;
        case 0x15: if (W) var_op<uint64_t>(c, 3); else var_op<uint32_t>(c, 3); return true;
        // vpshldv / vpshrdv: dst = reg (also source), vvvv, count rm
        case 0x70: if (!W) return false; ew3d<uint16_t>(c, [](uint16_t d, uint16_t a, uint16_t b, int) { return shld<uint16_t>(d, a, b); }); return true;
        case 0x71: if (W) ew3d<uint64_t>(c, [](uint64_t d, uint64_t a, uint64_t b, int) { return shld<uint64_t>(d, a, unsigned(b)); });
                   else ew3d<uint32_t>(c, [](uint32_t d, uint32_t a, uint32_t b, int) { return shld<uint32_t>(d, a, b); }); return true;
        case 0x72: if (!W) return false; ew3d<uint16_t>(c, [](uint16_t d, uint16_t a, uint16_t b, int) { return shrd<uint16_t>(a, d, b); }); return true;
        case 0x73: if (W) ew3d<uint64_t>(c, [](uint64_t d, uint64_t a, uint64_t b, int) { return shrd<uint64_t>(a, d, unsigned(b)); });
                   else ew3d<uint32_t>(c, [](uint32_t d, uint32_t a, uint32_t b, int) { return shrd<uint32_t>(a, d, b); }); return true;
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        unsigned n = c.imm();
        switch (in.opc) { // vpshld / vpshrd imm: src1 = vvvv, src2 = rm
        case 0x70: if (!W) return false; ew3<uint16_t>(c, [n](uint16_t a, uint16_t b, int) { return shld<uint16_t>(a, b, n); }); return true;
        case 0x71: if (W) ew3<uint64_t>(c, [n](uint64_t a, uint64_t b, int) { return shld<uint64_t>(a, b, n); });
                   else ew3<uint32_t>(c, [n](uint32_t a, uint32_t b, int) { return shld<uint32_t>(a, b, n); }); return true;
        case 0x72: if (!W) return false; ew3<uint16_t>(c, [n](uint16_t a, uint16_t b, int) { return shrd<uint16_t>(b, a, n); }); return true;
        case 0x73: if (W) ew3<uint64_t>(c, [n](uint64_t a, uint64_t b, int) { return shrd<uint64_t>(b, a, n); });
                   else ew3<uint32_t>(c, [n](uint32_t a, uint32_t b, int) { return shrd<uint32_t>(b, a, n); }); return true;
        }
        return false;
    }
    return false;
}

} // namespace emu
