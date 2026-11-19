// Conversions: FP <-> FP, FP <-> integer (packed and scalar, signed and
// unsigned), sign/zero extension (vpmovsx/zx) and narrowing (vpmov*).
#include "interp.h"
#include <math.h>
#include <limits>
#include <immintrin.h>

namespace emu {

template <class T> static T gete(const Z& z, int i) { T v; memcpy(&v, z.b + i * sizeof(T), sizeof(T)); return v; }
template <class T> static void sete(Z& z, int i, T v) { memcpy(z.b + i * sizeof(T), &v, sizeof(T)); }

// FP -> signed integer, x86 semantics (indefinite = min value). trunc or MXCSR rounding.
static int32_t f2i32(float x, bool t) { __m128 v = _mm_set_ss(x); return t ? _mm_cvttss_si32(v) : _mm_cvtss_si32(v); }
static int32_t f2i32(double x, bool t) { __m128d v = _mm_set_sd(x); return t ? _mm_cvttsd_si32(v) : _mm_cvtsd_si32(v); }
static int64_t f2i64(float x, bool t) { __m128 v = _mm_set_ss(x); return t ? _mm_cvttss_si64(v) : _mm_cvtss_si64(v); }
static int64_t f2i64(double x, bool t) { __m128d v = _mm_set_sd(x); return t ? _mm_cvttsd_si64(v) : _mm_cvtsd_si64(v); }
// FP -> unsigned integer: indefinite = all ones
template <class D, class T> static D f2u(T x, bool t) {
    if (isnan(x)) return std::numeric_limits<D>::max();
    T r = t ? trunc(x) : nearbyint(x);
    const T lim = sizeof(D) == 4 ? T(4294967296.0) : T(18446744073709551616.0);
    if (r < 0 || r >= lim) return std::numeric_limits<D>::max();
    return (D)r;
}

// packed conversion S -> D; n = vl / max(sizeof S, sizeof D)
template <class S, class D, class F> static void pconv(IC& c, F f) {
    const int ss = sizeof(S), ds = sizeof(D), n = c.vl / (ss > ds ? ss : ds);
    RcScope rs(evex_rc(c.in));
    Z s, r; memset(&r, 0, sizeof r);
    c.rm_src(s, n * ss, ss);
    for (int i = 0; i < n; i++) sete<D>(r, i, f(gete<S>(s, i)));
    c.write(c.reg(), r, ds, n * ds);
}

// scalar FP <- integer (GPR or memory, W selects 64-bit)
template <class D> static void si2f(IC& c, bool uns) {
    RcScope rs(evex_rc(c.in));
    uint64_t v;
    if (c.mem) v = c.in.W ? c.ld<uint64_t>(c.ea) : c.ld<uint32_t>(c.ea);
    else v = c.in.W ? c.gpr(c.rmreg() & 15) : uint32_t(c.gpr(c.rmreg() & 15));
    D d;
    if (c.in.W) d = uns ? D(v) : D(int64_t(v));
    else d = uns ? D(uint32_t(v)) : D(int32_t(uint32_t(v)));
    Z r = c.R(c.vvvv()); memcpy(r.b, &d, sizeof d);
    c.write_raw(c.reg(), r, 16);
}

// scalar FP -> integer GPR
template <class T> static void f2si(IC& c, bool trunc_, bool uns) {
    RcScope rs(evex_rc(c.in));
    T x = c.mem ? c.ld<T>(c.ea) : gete<T>(c.R(c.rmreg()), 0);
    uint64_t v;
    if (c.in.W) v = uns ? f2u<uint64_t>(x, trunc_) : uint64_t(f2i64(x, trunc_));
    else v = uns ? f2u<uint32_t>(x, trunc_) : uint32_t(f2i32(x, trunc_));
    c.set_gpr64(c.reg() & 15, v);
}

// scalar FP -> FP (vcvtss2sd / vcvtsd2ss), masked
template <class S, class D> static void s2s(IC& c) {
    RcScope rs(evex_rc(c.in));
    Z r = c.R(c.vvvv());
    if (c.mbit(0)) {
        S x = c.mem ? c.ld<S>(c.ea) : gete<S>(c.R(c.rmreg()), 0);
        D d = D(x); memcpy(r.b, &d, sizeof d);
    } else if (c.in.z) memset(r.b, 0, sizeof(D));
    else memcpy(r.b, c.R(c.reg()).b, sizeof(D));
    c.write_raw(c.reg(), r, 16);
}

// vpmovsx / vpmovzx: S (narrow) -> D (wide)
template <class S, class D> static void extend(IC& c) {
    const int ss = sizeof(S), ds = sizeof(D), n = c.vl / ds;
    Z s, r; memset(&r, 0, sizeof r);
    c.rm_src(s, n * ss, ss);
    for (int i = 0; i < n; i++) sete<D>(r, i, D(gete<S>(s, i)));
    c.write(c.reg(), r, ds, c.vl);
}

// narrowing vpmov*: reg (S) -> rm (D); mode 0 truncate, 1 signed sat, 2 unsigned sat
template <class S, class D> static void narrow(IC& c, int mode) {
    typedef typename std::make_signed<S>::type SS;
    typedef typename std::make_unsigned<S>::type SU;
    typedef typename std::make_signed<D>::type DS;
    typedef typename std::make_unsigned<D>::type DU;
    const int ss = sizeof(S), ds = sizeof(D), n = c.vl / ss;
    const Z& s = c.R(c.reg());
    Z r; memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        S x = gete<S>(s, i); D d;
        if (mode == 0) d = D(x);
        else if (mode == 1) {
            SS v = SS(x);
            d = D(v < SS(std::numeric_limits<DS>::min()) ? std::numeric_limits<DS>::min() : v > SS(std::numeric_limits<DS>::max()) ? std::numeric_limits<DS>::max() : DS(v));
        } else {
            SU v = SU(x);
            d = D(v > SU(std::numeric_limits<DU>::max()) ? std::numeric_limits<DU>::max() : DU(v));
        }
        sete<D>(r, i, d);
    }
    if (c.mem) { if (c.in.aaa == 0) c.stn(c.ea, r.b, n * ds); else c.store_masked(r, ds, n * ds); }
    else c.write(c.rmreg(), r, ds, n * ds);
}

bool fam_cvt(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    const int W = in.W;
    if (in.map == 1) {
        switch (in.pp << 8 | in.opc) {
        case 0x05a: if (W) return false; pconv<float, double>(c, [](float x) { return double(x); }); return true;     // vcvtps2pd
        case 0x15a: if (!W) return false; pconv<double, float>(c, [](double x) { return float(x); }); return true;     // vcvtpd2ps
        case 0x05b: // vcvtdq2ps / vcvtqq2ps
            if (W) pconv<int64_t, float>(c, [](int64_t x) { return float(x); });
            else pconv<int32_t, float>(c, [](int32_t x) { return float(x); });
            return true;
        case 0x15b: if (W) return false; pconv<float, int32_t>(c, [](float x) { return f2i32(x, false); }); return true; // vcvtps2dq
        case 0x25b: if (W) return false; pconv<float, int32_t>(c, [](float x) { return f2i32(x, true); }); return true;  // vcvttps2dq
        case 0x078: case 0x079: { bool t = in.opc == 0x78; // vcvt(t)ps2udq / vcvt(t)pd2udq
            if (W) pconv<double, uint32_t>(c, [t](double x) { return f2u<uint32_t>(x, t); });
            else pconv<float, uint32_t>(c, [t](float x) { return f2u<uint32_t>(x, t); });
            return true; }
        case 0x178: case 0x179: { bool t = in.opc == 0x78; // vcvt(t)ps2uqq / vcvt(t)pd2uqq
            if (W) pconv<double, uint64_t>(c, [t](double x) { return f2u<uint64_t>(x, t); });
            else pconv<float, uint64_t>(c, [t](float x) { return f2u<uint64_t>(x, t); });
            return true; }
        case 0x17a: case 0x17b: { bool t = in.opc == 0x7a; // vcvt(t)ps2qq / vcvt(t)pd2qq
            if (W) pconv<double, int64_t>(c, [t](double x) { return f2i64(x, t); });
            else pconv<float, int64_t>(c, [t](float x) { return f2i64(x, t); });
            return true; }
        case 0x27a: // vcvtudq2pd / vcvtuqq2pd
            if (W) pconv<uint64_t, double>(c, [](uint64_t x) { return double(x); });
            else pconv<uint32_t, double>(c, [](uint32_t x) { return double(x); });
            return true;
        case 0x37a: // vcvtudq2ps / vcvtuqq2ps
            if (W) pconv<uint64_t, float>(c, [](uint64_t x) { return float(x); });
            else pconv<uint32_t, float>(c, [](uint32_t x) { return float(x); });
            return true;
        case 0x2e6: // vcvtdq2pd / vcvtqq2pd
            if (W) pconv<int64_t, double>(c, [](int64_t x) { return double(x); });
            else pconv<int32_t, double>(c, [](int32_t x) { return double(x); });
            return true;
        case 0x1e6: if (!W) return false; pconv<double, int32_t>(c, [](double x) { return f2i32(x, true); }); return true;  // vcvttpd2dq
        case 0x3e6: if (!W) return false; pconv<double, int32_t>(c, [](double x) { return f2i32(x, false); }); return true; // vcvtpd2dq
        case 0x22a: si2f<float>(c, false); return true;   // vcvtsi2ss
        case 0x32a: si2f<double>(c, false); return true;  // vcvtsi2sd
        case 0x27b: si2f<float>(c, true); return true;    // vcvtusi2ss
        case 0x37b: si2f<double>(c, true); return true;   // vcvtusi2sd
        case 0x22c: f2si<float>(c, true, false); return true;   // vcvttss2si
        case 0x22d: f2si<float>(c, false, false); return true;  // vcvtss2si
        case 0x32c: f2si<double>(c, true, false); return true;  // vcvttsd2si
        case 0x32d: f2si<double>(c, false, false); return true; // vcvtsd2si
        case 0x278: f2si<float>(c, true, true); return true;    // vcvttss2usi
        case 0x279: f2si<float>(c, false, true); return true;   // vcvtss2usi
        case 0x378: f2si<double>(c, true, true); return true;   // vcvttsd2usi
        case 0x379: f2si<double>(c, false, true); return true;  // vcvtsd2usi
        case 0x25a: if (W) return false; s2s<float, double>(c); return true;  // vcvtss2sd
        case 0x35a: if (!W) return false; s2s<double, float>(c); return true; // vcvtsd2ss
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x20: extend<int8_t, int16_t>(c); return true;
        case 0x21: extend<int8_t, int32_t>(c); return true;
        case 0x22: extend<int8_t, int64_t>(c); return true;
        case 0x23: extend<int16_t, int32_t>(c); return true;
        case 0x24: extend<int16_t, int64_t>(c); return true;
        case 0x25: if (W) return false; extend<int32_t, int64_t>(c); return true;
        case 0x30: extend<uint8_t, uint16_t>(c); return true;
        case 0x31: extend<uint8_t, uint32_t>(c); return true;
        case 0x32: extend<uint8_t, uint64_t>(c); return true;
        case 0x33: extend<uint16_t, uint32_t>(c); return true;
        case 0x34: extend<uint16_t, uint64_t>(c); return true;
        case 0x35: if (W) return false; extend<uint32_t, uint64_t>(c); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 2 && !W) {
        int hi = in.opc >> 4, lo = in.opc & 15;
        if ((hi != 1 && hi != 2 && hi != 3) || lo > 5) return false;
        int mode = hi == 3 ? 0 : (hi == 2 ? 1 : 2);
        switch (lo) {
        case 0: narrow<uint16_t, uint8_t>(c, mode); return true;
        case 1: narrow<uint32_t, uint8_t>(c, mode); return true;
        case 2: narrow<uint64_t, uint8_t>(c, mode); return true;
        case 3: narrow<uint32_t, uint16_t>(c, mode); return true;
        case 4: narrow<uint64_t, uint16_t>(c, mode); return true;
        case 5: narrow<uint64_t, uint32_t>(c, mode); return true;
        }
    }
    return false;
}

} // namespace emu
