// Floating point: arithmetic, logic, FMA, min/max, sqrt, compares setting
// EFLAGS, and the AVX-512 FP specials (getexp, getmant, scalef, rndscale,
// rcp14, rsqrt14, range).
#include "interp.h"
#include <math.h>
#include <immintrin.h>

namespace emu {

template <class T> static T fmin_x86(T a, T b) { return a < b ? a : b; }
// a op b with x86 operand order (NaN propagation picks the first operand)
static inline float fadd(float a, float b) { __asm__("vaddss %2, %1, %0" : "=x"(a) : "x"(a), "x"(b)); return a; }
static inline double fadd(double a, double b) { __asm__("vaddsd %2, %1, %0" : "=x"(a) : "x"(a), "x"(b)); return a; }
static inline float fmul(float a, float b) { __asm__("vmulss %2, %1, %0" : "=x"(a) : "x"(a), "x"(b)); return a; }
static inline double fmul(double a, double b) { __asm__("vmulsd %2, %1, %0" : "=x"(a) : "x"(a), "x"(b)); return a; }
template <class T> static T fmax_x86(T a, T b) { return a > b ? a : b; }

template <class T> struct FB;
template <> struct FB<float> { typedef uint32_t U; enum { EXP = 8, MAN = 23, BIAS = 127 }; static constexpr U QBIT = 0x400000u, SIGN = 0x80000000u, INDEF = 0xffc00000u; };
template <> struct FB<double> { typedef uint64_t U; enum { EXP = 11, MAN = 52, BIAS = 1023 }; static constexpr U QBIT = 0x8000000000000ull, SIGN = 0x8000000000000000ull, INDEF = 0xfff8000000000000ull; };
template <class T> static typename FB<T>::U bits(T x) { typename FB<T>::U u; memcpy(&u, &x, sizeof x); return u; }
template <class T> static T fromb(typename FB<T>::U u) { T x; memcpy(&x, &u, sizeof x); return x; }
template <class T> static T qnan(T x) { return fromb<T>(bits(x) | FB<T>::QBIT); }
static bool daz() { return _mm_getcsr() & 0x40; }
template <class T> static T daz_in(T x) { // denormal inputs read as zero when DAZ
    typedef typename FB<T>::U U; U u = bits(x);
    U e = (u >> FB<T>::MAN) & ((U(1) << FB<T>::EXP) - 1), m = u & ((U(1) << FB<T>::MAN) - 1);
    if (e == 0 && m && daz()) return fromb<T>(u & FB<T>::SIGN);
    return x;
}

// ---- getexp: floor(log2|x|)
template <class T> static T getexp(T x) {
    typedef typename FB<T>::U U; x = daz_in(x); U u = bits(x);
    U e = (u >> FB<T>::MAN) & ((U(1) << FB<T>::EXP) - 1), m = u & ((U(1) << FB<T>::MAN) - 1);
    if (e == (U(1) << FB<T>::EXP) - 1) { if (m) return qnan(x); return T(INFINITY); }
    if (e == 0 && m == 0) return T(-INFINITY);
    if (e == 0) { int lz = 0; while (!(m & (U(1) << (FB<T>::MAN - 1 - lz)))) lz++; return T(int(1 - FB<T>::BIAS - 1 - lz)); }
    return T(int(e) - FB<T>::BIAS);
}

// ---- getmant (Intel GetNormalizedMantissa)
template <class T> static T getmant(T x, int imm) {
    typedef typename FB<T>::U U; x = daz_in(x); U u = bits(x);
    const U EM = (U(1) << FB<T>::EXP) - 1, MM = (U(1) << FB<T>::MAN) - 1;
    U e = (u >> FB<T>::MAN) & EM, m = u & MM; bool neg = u >> (sizeof(T) * 8 - 1);
    int interv = imm & 3, sc = (imm >> 2) & 3;
    if (e == EM && m) return qnan(x);
    bool zero = e == 0 && m == 0;
    if ((sc & 2) && neg && !zero) return fromb<T>(FB<T>::INDEF);
    U sign = (sc & 1) ? 0 : (neg ? FB<T>::SIGN : 0);
    if (zero || e == EM) return fromb<T>(sign | (U(FB<T>::BIAS) << FB<T>::MAN)); // +-1.0
    int ue = int(e) - FB<T>::BIAS;
    if (e == 0) { ue = 1 - FB<T>::BIAS; while (!(m & (U(1) << FB<T>::MAN))) { m <<= 1; ue--; } m &= MM; }
    U ne = FB<T>::BIAS;
    switch (interv) {
    case 0: break;                                                   // [1,2)
    case 1: ne = (ue & 1) ? FB<T>::BIAS - 1 : FB<T>::BIAS; break;    // [1/2,2)
    case 2: ne = FB<T>::BIAS - 1; break;                             // [1/2,1)
    case 3: ne = (m >> (FB<T>::MAN - 1)) ? FB<T>::BIAS - 1 : FB<T>::BIAS; break; // [3/4,3/2)
    }
    return fromb<T>(sign | (ne << FB<T>::MAN) | m);
}

template <class T> static bool is_snan(T x) { typedef typename FB<T>::U U; U u = bits(x); return isnan(x) && !(u & (U(1) << (sizeof(T) == 4 ? 22 : 51))); }
// ---- scalef: x * 2^floor(y)
// hardware (probe): a QNaN x with y = +inf gives +inf, with y = -inf gives +0 (an SNaN x still gives its QNaN)
template <class T> static T scalef(T x, T y) {
    x = daz_in(x); y = daz_in(y);
    if (isnan(x)) { if (!is_snan(x) && isinf(y)) return y > 0 ? T(INFINITY) : T(0); return qnan(x); }
    if (isnan(y)) return qnan(y);
    if (isinf(y)) {
        if (y > 0) { if (x == 0) return fromb<T>(FB<T>::INDEF); return x * T(INFINITY); }
        if (isinf(x)) return fromb<T>(FB<T>::INDEF);
        return x * T(0);
    }
    T f = floor(y);
    if (f > 4000) f = 4000; if (f < -4000) f = -4000;
    int n = int(f);
    // exact single rounding: scalbn rounds correctly in the current mode
    T r = sizeof(T) == 4 ? T(scalbnf(float(x), n)) : T(scalbn(double(x), n));
    if (_mm_getcsr() & 0x8000) { // FTZ: tiny results flush
        typedef typename FB<T>::U U; U u = bits(r); U e = (u >> FB<T>::MAN) & ((U(1) << FB<T>::EXP) - 1);
        if (e == 0) r = fromb<T>(u & FB<T>::SIGN);
    }
    return r;
}

// ---- rndscale: 2^-M * round(2^M * x), imm: [7:4] M, [2] use MXCSR, [1:0] mode
template <class T> static T rndscale(T x, int imm) {
    x = daz_in(x);
    if (isnan(x)) return qnan(x);
    int M = (imm >> 4) & 15;
    unsigned csr = _mm_getcsr();
    int mode = (imm & 4) ? int((csr >> 13) & 3) : (imm & 3);
    const int MAN = sizeof(T) == 4 ? 23 : 52;
    if (isinf(x) || fabs(x) >= T(ldexp(1.0, MAN - M))) return x;   // already integral at this scale (also: no overflow)
    // the scaling must not be flushed (FTZ) or rounded (RC): compute with a clean MXCSR
    _mm_setcsr(0x1F80);
    T s = T(ldexp(1.0, M));
    T y = x * s, r;
    switch (mode) { case 0: r = nearbyint(y); break; case 1: r = floor(y); break; case 2: r = ceil(y); break; default: r = trunc(y); break; }
    r = r / s;
    _mm_setcsr(csr);
    if (r == 0) r = copysign(T(0), x);
    return r;
}

// ---- rcp14 / rsqrt14: not bit exact with hardware; full precision then truncated to 14+ bits
template <class T> static T rcp14(T x) {
    x = daz_in(x);
    if (isnan(x)) return qnan(x);
    if (x == 0) return copysign(T(INFINITY), x);
    if (isinf(x)) return copysign(T(0), x);
    return T(1) / x;
}
template <class T> static T rsqrt14(T x) {
    x = daz_in(x);
    if (isnan(x)) return qnan(x);
    if (x == 0) return copysign(T(INFINITY), x);
    if (x < 0) return fromb<T>(FB<T>::INDEF);
    if (isinf(x)) return T(0);
    return T(1) / sqrt(x);
}

// ---- range (vrangeps/pd)
// NaN rules and ties verified against hardware (probe over all imm / special values): an SNaN operand gives its QNaN
// (no sign control); one QNaN gives the other operand, two QNaNs give src1's QNaN (both then sign-controlled);
// equal magnitudes: min-abs takes the negative operand, max-abs the positive one.
template <class T> static T range(T a, T b, int imm) {
    a = daz_in(a); b = daz_in(b);   // hardware: DAZ applies to the inputs, FTZ not to the result
    if (is_snan(a)) return qnan(a);
    if (is_snan(b)) return qnan(b);
    int op = imm & 3, sc = (imm >> 2) & 3;
    T r;
    if (isnan(a) && isnan(b)) r = qnan(a);
    else if (isnan(a)) r = b;
    else if (isnan(b)) r = a;
    else switch (op) {
    case 0: r = a <= b ? a : b; if (a == b && a == 0) r = signbit(a) ? a : b; break;  // min
    case 1: r = a >= b ? a : b; if (a == b && a == 0) r = signbit(a) ? b : a; break;  // max
    case 2: r = fabs(a) < fabs(b) ? a : fabs(b) < fabs(a) ? b : (signbit(a) ? a : b); break;   // min abs
    default: r = fabs(a) > fabs(b) ? a : fabs(b) > fabs(a) ? b : (signbit(a) ? b : a); break;  // max abs
    }
    switch (sc) { case 0: r = copysign(r, a); break; case 1: break; case 2: r = fabs(r); break; case 3: r = -fabs(r); break; }
    return r;
}

static void ucomi(IC& c, bool dbl) {
    double a, b;
    if (dbl) { a = c.R(c.reg()).g[0]; b = c.mem ? c.ld<double>(c.ea) : c.R(c.rmreg()).g[0]; }
    else { a = c.R(c.reg()).f[0]; b = c.mem ? c.ld<float>(c.ea) : c.R(c.rmreg()).f[0]; }
    if (!dbl) { float fa = c.R(c.reg()).f[0], fb = c.mem ? c.ld<float>(c.ea) : c.R(c.rmreg()).f[0]; fa = daz_in(fa); fb = daz_in(fb); a = fa; b = fb; }
    else { a = daz_in(a); b = daz_in(b); }
    uint64_t f;
    if (isnan(a) || isnan(b)) f = ZF | PF | CF;
    else if (a > b) f = 0;
    else if (a < b) f = CF;
    else f = ZF;
    c.set_flags(f, ZF | PF | CF | OF | SF | AF);
}

template <class T> static void fma_family(IC& c, int kind, int order, bool scalar) {
    // kind: 6 maddsub 7 msubadd 8 madd a msub c nmadd e nmsub (scalar 9 b d f)
    auto f = [kind, order](T d, T a, T b, int i) -> T {
        T x, y, z;
        if (order == 0) { x = d; y = b; z = a; }        // 132: d*rm + vvvv
        else if (order == 1) { x = a; y = d; z = b; }   // 213: vvvv*d + rm
        else { x = a; y = b; z = d; }                   // 231: vvvv*rm + d
        bool even = !(i & 1);
        switch (kind) {
        case 6: return even ? (T)std::fma(x, y, -z) : (T)std::fma(x, y, z);
        case 7: return even ? (T)std::fma(x, y, z) : (T)std::fma(x, y, -z);
        case 8: return (T)std::fma(x, y, z);
        case 0xa: return (T)std::fma(x, y, -z);
        case 0xc: return (T)std::fma(-x, y, z);
        default: return (T)std::fma(-x, y, -z);
        }
    };
    RcScope rs(evex_rc(c.in));
    if (scalar) sc3d<T>(c, [&](T d, T a, T b) { return f(d, a, b, 0); });
    else ew3d<T>(c, f);
}

bool fam_fp(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    int W = in.W;
    if (in.map == 1) {
        int key = in.pp << 8 | in.opc;
        RcScope rs((key & 0xff) >= 0x51 && (key & 0xff) <= 0x5f ? evex_rc(in) : -1);
        switch (key) {
        // packed single
        case 0x051: if (W) return false; ew2<float>(c, [](float b, int) { return sqrtf(b); }); return true;
        case 0x054: if (W) return false; ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a & b; }); return true;
        case 0x055: if (W) return false; ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return ~a & b; }); return true;
        case 0x056: if (W) return false; ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a | b; }); return true;
        case 0x057: if (W) return false; ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a ^ b; }); return true;
        case 0x058: if (W) return false; ew3<float>(c, [](float a, float b, int) { return fadd(a, b); }); return true;
        case 0x059: if (W) return false; ew3<float>(c, [](float a, float b, int) { return fmul(a, b); }); return true;
        case 0x05c: if (W) return false; ew3<float>(c, [](float a, float b, int) { return a - b; }); return true;
        case 0x05d: if (W) return false; ew3<float>(c, [](float a, float b, int) { return fmin_x86(daz_in(a), daz_in(b)); }); return true;
        case 0x05e: if (W) return false; ew3<float>(c, [](float a, float b, int) { return a / b; }); return true;
        case 0x05f: if (W) return false; ew3<float>(c, [](float a, float b, int) { return fmax_x86(daz_in(a), daz_in(b)); }); return true;
        // packed double
        case 0x151: if (!W) return false; ew2<double>(c, [](double b, int) { return sqrt(b); }); return true;
        case 0x154: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a & b; }); return true;
        case 0x155: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return ~a & b; }); return true;
        case 0x156: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a | b; }); return true;
        case 0x157: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a ^ b; }); return true;
        case 0x158: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return fadd(a, b); }); return true;
        case 0x159: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return fmul(a, b); }); return true;
        case 0x15c: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return a - b; }); return true;
        case 0x15d: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return fmin_x86(daz_in(a), daz_in(b)); }); return true;
        case 0x15e: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return a / b; }); return true;
        case 0x15f: if (!W) return false; ew3<double>(c, [](double a, double b, int) { return fmax_x86(daz_in(a), daz_in(b)); }); return true;
        // scalar single
        case 0x251: if (W) return false; sc3<float>(c, [](float, float b) { return sqrtf(b); }); return true;
        case 0x258: if (W) return false; sc3<float>(c, [](float a, float b) { return fadd(a, b); }); return true;
        case 0x259: if (W) return false; sc3<float>(c, [](float a, float b) { return fmul(a, b); }); return true;
        case 0x25c: if (W) return false; sc3<float>(c, [](float a, float b) { return a - b; }); return true;
        case 0x25d: if (W) return false; sc3<float>(c, [](float a, float b) { return fmin_x86(daz_in(a), daz_in(b)); }); return true;
        case 0x25e: if (W) return false; sc3<float>(c, [](float a, float b) { return a / b; }); return true;
        case 0x25f: if (W) return false; sc3<float>(c, [](float a, float b) { return fmax_x86(daz_in(a), daz_in(b)); }); return true;
        // scalar double
        case 0x351: if (!W) return false; sc3<double>(c, [](double, double b) { return sqrt(b); }); return true;
        case 0x358: if (!W) return false; sc3<double>(c, [](double a, double b) { return fadd(a, b); }); return true;
        case 0x359: if (!W) return false; sc3<double>(c, [](double a, double b) { return fmul(a, b); }); return true;
        case 0x35c: if (!W) return false; sc3<double>(c, [](double a, double b) { return a - b; }); return true;
        case 0x35d: if (!W) return false; sc3<double>(c, [](double a, double b) { return fmin_x86(daz_in(a), daz_in(b)); }); return true;
        case 0x35e: if (!W) return false; sc3<double>(c, [](double a, double b) { return a / b; }); return true;
        case 0x35f: if (!W) return false; sc3<double>(c, [](double a, double b) { return fmax_x86(daz_in(a), daz_in(b)); }); return true;
        case 0x02e: case 0x02f: if (W) return false; ucomi(c, false); return true;
        case 0x12e: case 0x12f: if (!W) return false; ucomi(c, true); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        int o = in.opc;
        if ((o >= 0x96 && o <= 0x9f) || (o >= 0xa6 && o <= 0xaf) || (o >= 0xb6 && o <= 0xbf)) {
            int order = o < 0xa0 ? 0 : (o < 0xb0 ? 1 : 2);
            int kind = o & 15;
            bool scalar = kind & 1 && kind >= 9;
            if (kind == 9 || kind == 0xb || kind == 0xd || kind == 0xf) { kind -= 1; scalar = true; } else scalar = false;
            if (W) fma_family<double>(c, kind, order, scalar); else fma_family<float>(c, kind, order, scalar);
            return true;
        }
        switch (o) {
        case 0x2c: { RcScope rs(evex_rc(in)); if (W) ew3<double>(c, [](double a, double b, int) { return scalef(a, b); }); else ew3<float>(c, [](float a, float b, int) { return scalef(a, b); }); return true; }
        case 0x2d: { RcScope rs(evex_rc(in)); if (W) sc3<double>(c, [](double a, double b) { return scalef(a, b); }); else sc3<float>(c, [](float a, float b) { return scalef(a, b); }); return true; }
        case 0x42: if (W) ew2<double>(c, [](double b, int) { return getexp(b); }); else ew2<float>(c, [](float b, int) { return getexp(b); }); return true;
        case 0x43: if (W) sc3<double>(c, [](double, double b) { return getexp(b); }); else sc3<float>(c, [](float, float b) { return getexp(b); }); return true;
        case 0x4c: if (W) ew2<double>(c, [](double b, int) { return rcp14(b); }); else ew2<float>(c, [](float b, int) { return rcp14(b); }); return true;
        case 0x4d: if (W) sc3<double>(c, [](double, double b) { return rcp14(b); }); else sc3<float>(c, [](float, float b) { return rcp14(b); }); return true;
        case 0x4e: if (W) ew2<double>(c, [](double b, int) { return rsqrt14(b); }); else ew2<float>(c, [](float b, int) { return rsqrt14(b); }); return true;
        case 0x4f: if (W) sc3<double>(c, [](double, double b) { return rsqrt14(b); }); else sc3<float>(c, [](float, float b) { return rsqrt14(b); }); return true;
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        int imm = c.imm();
        switch (in.opc) {
        case 0x08: if (W) return false; ew2<float>(c, [imm](float b, int) { return rndscale(b, imm); }); return true;
        case 0x09: if (!W) return false; ew2<double>(c, [imm](double b, int) { return rndscale(b, imm); }); return true;
        case 0x0a: if (W) return false; sc3<float>(c, [imm](float, float b) { return rndscale(b, imm); }); return true;
        case 0x0b: if (!W) return false; sc3<double>(c, [imm](double, double b) { return rndscale(b, imm); }); return true;
        case 0x26: if (W) ew2<double>(c, [imm](double b, int) { return getmant(b, imm); }); else ew2<float>(c, [imm](float b, int) { return getmant(b, imm); }); return true;
        case 0x27: if (W) sc3<double>(c, [imm](double, double b) { return getmant(b, imm); }); else sc3<float>(c, [imm](float, float b) { return getmant(b, imm); }); return true;
        case 0x50: if (W) ew3<double>(c, [imm](double a, double b, int) { return range(a, b, imm); }); else ew3<float>(c, [imm](float a, float b, int) { return range(a, b, imm); }); return true;
        case 0x51: if (W) sc3<double>(c, [imm](double a, double b) { return range(a, b, imm); }); else sc3<float>(c, [imm](float a, float b) { return range(a, b, imm); }); return true;
        }
        return false;
    }
    return false;
}

} // namespace emu
