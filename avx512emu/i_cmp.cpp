// Instructions writing an opmask: FP and integer compares, vptestm/nm,
// vpmov*2m, vfpclass; and vpmovm2* (vector from opmask).
#include "interp.h"
#include <math.h>
#include <immintrin.h>
#include <type_traits>

namespace emu {

static void setk(IC& c, uint64_t bits, int n) {
    if (n < 64) bits &= (1ull << n) - 1;
    if (c.in.aaa) bits &= c.K[c.in.aaa];
    c.K[c.reg() & 7] = bits;
}

template <class T> static T daz(T x) {
    if (!(_mm_getcsr() & 0x40)) return x;
    if (x != 0 && fabs(x) < (sizeof(T) == 4 ? T(1.17549435e-38f) : T(2.2250738585072014e-308))) return copysign(T(0), x);
    return x;
}

template <class T> static bool fcmp(T a, T b, int p) {
    a = daz(a); b = daz(b);
    bool un = isnan(a) || isnan(b);
    bool eq = !un && a == b, lt = !un && a < b, gt = !un && a > b;
    switch (p & 15) {
    case 0: return eq;
    case 1: return lt;
    case 2: return lt || eq;
    case 3: return un;
    case 4: return !eq;
    case 5: return !lt;
    case 6: return !(lt || eq);
    case 7: return !un;
    case 8: return eq || un;
    case 9: return lt || un;
    case 10: return lt || eq || un;
    case 11: return false;
    case 12: return lt || gt;
    case 13: return gt || eq;
    case 14: return gt;
    default: return true;
    }
}

template <class T> static void vcmp(IC& c, bool scalar) {
    const int es = sizeof(T), p = c.imm() & 31;
    Z a = c.R(c.vvvv()), b;
    if (scalar) {
        memset(&b, 0, sizeof b);
        if (c.mem) c.ldn(b.b, c.ea, es); else b = c.R(c.rmreg());
        T x, y; memcpy(&x, a.b, es); memcpy(&y, b.b, es);
        setk(c, fcmp(x, y, p) ? 1 : 0, 1);
        return;
    }
    c.rm_src(b, c.vl, es);
    const int n = c.vl / es; uint64_t m = 0;
    for (int i = 0; i < n; i++) { T x, y; memcpy(&x, a.b + i * es, es); memcpy(&y, b.b + i * es, es); if (fcmp(x, y, p)) m |= 1ull << i; }
    setk(c, m, n);
}

// integer compare, predicate 0 eq 1 lt 2 le 3 false 4 ne 5 nlt 6 nle 7 true
template <class T> static void icmp(IC& c, int p) {
    const int es = sizeof(T), n = c.vl / es;
    Z a = c.R(c.vvvv()), b; c.rm_src(b, c.vl, es);
    uint64_t m = 0;
    for (int i = 0; i < n; i++) {
        T x, y; memcpy(&x, a.b + i * es, es); memcpy(&y, b.b + i * es, es);
        bool r;
        switch (p & 7) {
        case 0: r = x == y; break; case 1: r = x < y; break; case 2: r = x <= y; break; case 3: r = false; break;
        case 4: r = x != y; break; case 5: r = x >= y; break; case 6: r = x > y; break; default: r = true; break;
        }
        if (r) m |= 1ull << i;
    }
    setk(c, m, n);
}

template <class T> static void ptest(IC& c, bool neg) {
    const int es = sizeof(T), n = c.vl / es;
    Z a = c.R(c.vvvv()), b; c.rm_src(b, c.vl, es);
    uint64_t m = 0;
    for (int i = 0; i < n; i++) { T x, y; memcpy(&x, a.b + i * es, es); memcpy(&y, b.b + i * es, es); if (((x & y) != 0) != neg) m |= 1ull << i; }
    setk(c, m, n);
}

static void mov2m(IC& c, int es) {
    const int n = c.vl / es; const Z& s = c.R(c.rmreg()); uint64_t m = 0;
    for (int i = 0; i < n; i++) if (s.b[i * es + es - 1] & 0x80) m |= 1ull << i;
    c.K[c.reg() & 7] = m;
}
static void movm2(IC& c, int es) {
    const int n = c.vl / es; uint64_t k = c.K[c.in.rm3]; Z r; memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) if (k >> i & 1) memset(r.b + i * es, 0xff, es);
    c.write_raw(c.reg(), r, c.vl);
}

template <class T> static bool fpclass(T x, int imm) {
    typedef typename std::conditional<sizeof(T) == 4, uint32_t, uint64_t>::type U;
    const int MAN = sizeof(T) == 4 ? 23 : 52, EXP = sizeof(T) == 4 ? 8 : 11;
    U u; memcpy(&u, &x, sizeof u);
    bool neg = u >> (sizeof(T) * 8 - 1);
    U e = (u >> MAN) & ((U(1) << EXP) - 1), m = u & ((U(1) << MAN) - 1);
    bool nan = e == (U(1) << EXP) - 1 && m, qnan = nan && (m >> (MAN - 1)), snan = nan && !qnan;
    bool inf = e == (U(1) << EXP) - 1 && !m, zero = e == 0 && !m, den = e == 0 && m;
    if (den && (_mm_getcsr() & 0x40)) { den = false; zero = true; }   // hardware VFPCLASS honours MXCSR.DAZ
    bool r = false;
    if ((imm & 1) && qnan) r = true;
    if ((imm & 2) && zero && !neg) r = true;
    if ((imm & 4) && zero && neg) r = true;
    if ((imm & 8) && inf && !neg) r = true;
    if ((imm & 16) && inf && neg) r = true;
    if ((imm & 32) && den) r = true;
    if ((imm & 64) && neg && !nan && !inf && !zero) r = true;
    if ((imm & 128) && snan) r = true;
    return r;
}

template <class T> static void vfpclass(IC& c, bool scalar) {
    const int es = sizeof(T), imm = c.imm();
    if (scalar) {
        T x; if (c.mem) x = c.ld<T>(c.ea); else memcpy(&x, c.R(c.rmreg()).b, es);
        setk(c, fpclass(x, imm) ? 1 : 0, 1);
        return;
    }
    Z b; c.rm_src(b, c.vl, es);
    const int n = c.vl / es; uint64_t m = 0;
    for (int i = 0; i < n; i++) { T x; memcpy(&x, b.b + i * es, es); if (fpclass(x, imm)) m |= 1ull << i; }
    setk(c, m, n);
}

bool fam_cmp(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    const int W = in.W;
    if (in.map == 1) {
        if (in.opc == 0xc2) {
            switch (in.pp) {
            case 0: if (W) return false; vcmp<float>(c, false); return true;
            case 1: if (!W) return false; vcmp<double>(c, false); return true;
            case 2: if (W) return false; vcmp<float>(c, true); return true;
            default: if (!W) return false; vcmp<double>(c, true); return true;
            }
        }
        if (in.pp != 1) return false;
        switch (in.opc) {
        case 0x74: icmp<uint8_t>(c, 0); return true;
        case 0x75: icmp<uint16_t>(c, 0); return true;
        case 0x76: if (W) return false; icmp<uint32_t>(c, 0); return true;
        case 0x64: icmp<int8_t>(c, 6); return true;
        case 0x65: icmp<int16_t>(c, 6); return true;
        case 0x66: if (W) return false; icmp<int32_t>(c, 6); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x29: if (!W) return false; icmp<uint64_t>(c, 0); return true;
        case 0x37: if (!W) return false; icmp<int64_t>(c, 6); return true;
        case 0x26: if (W) ptest<uint16_t>(c, false); else ptest<uint8_t>(c, false); return true;
        case 0x27: if (W) ptest<uint64_t>(c, false); else ptest<uint32_t>(c, false); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 2) {
        switch (in.opc) {
        case 0x26: if (W) ptest<uint16_t>(c, true); else ptest<uint8_t>(c, true); return true;
        case 0x27: if (W) ptest<uint64_t>(c, true); else ptest<uint32_t>(c, true); return true;
        case 0x28: if (c.mem) return false; movm2(c, W ? 2 : 1); return true;
        case 0x38: if (c.mem) return false; movm2(c, W ? 8 : 4); return true;
        case 0x29: if (c.mem) return false; mov2m(c, W ? 2 : 1); return true;
        case 0x39: if (c.mem) return false; mov2m(c, W ? 8 : 4); return true;
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        int p = c.imm();
        switch (in.opc) {
        case 0x3f: if (W) icmp<int16_t>(c, p); else icmp<int8_t>(c, p); return true;
        case 0x3e: if (W) icmp<uint16_t>(c, p); else icmp<uint8_t>(c, p); return true;
        case 0x1f: if (W) icmp<int64_t>(c, p); else icmp<int32_t>(c, p); return true;
        case 0x1e: if (W) icmp<uint64_t>(c, p); else icmp<uint32_t>(c, p); return true;
        case 0x66: if (W) vfpclass<double>(c, false); else vfpclass<float>(c, false); return true;
        case 0x67: if (W) vfpclass<double>(c, true); else vfpclass<float>(c, true); return true;
        }
        return false;
    }
    return false;
}

} // namespace emu
