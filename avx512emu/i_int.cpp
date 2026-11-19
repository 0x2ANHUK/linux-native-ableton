// Integer element-wise operations: add/sub (wrapping and saturating), avg,
// multiplies, multiply-add, sad, min/max, abs, logic, ternlog, blendm, VNNI,
// IFMA, lzcnt, popcnt, conflict.
#include "interp.h"
#include <limits>

namespace emu {

template <class T> static T sat(int64_t v) {
    if (v > int64_t(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    if (v < int64_t(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    return T(v);
}

template <class T> static T ternlog(T a, T b, T c, int imm) {
    T r = 0;
    for (int k = 0; k < 8; k++) {
        if (!(imm >> k & 1)) continue;
        T t = ((k & 4) ? a : T(~a)) & ((k & 2) ? b : T(~b)) & ((k & 1) ? c : T(~c));
        r |= t;
    }
    return r;
}

static void blendm(IC& c, int es) {
    Z a = c.R(c.vvvv()), b, r;
    memset(&r, 0, sizeof r);
    c.rm_src(b, c.vl, es);
    for (int i = 0; i < c.vl / es; i++) {
        if (c.mbit(i)) memcpy(r.b + i * es, b.b + i * es, es);
        else if (!c.in.z) memcpy(r.b + i * es, a.b + i * es, es);
    }
    c.write_raw(c.reg(), r, c.vl);
}

template <class T> static void conflict(IC& c) {
    const int n = c.vl / sizeof(T);
    Z s, r; memset(&r, 0, sizeof r);
    c.rm_src(s, c.vl, sizeof(T), false);
    T e[16]; memcpy(e, s.b, c.vl);
    for (int i = 0; i < n; i++) {
        T m = 0;
        for (int j = 0; j < i; j++) if (e[j] == e[i]) m |= T(1) << j;
        memcpy(r.b + i * sizeof(T), &m, sizeof(T));
    }
    c.write(c.reg(), r, sizeof(T), c.vl);
}

template <class T> static T popcnt(T v) { return T(__builtin_popcountll(uint64_t(v))); }

bool fam_int(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    const int W = in.W;
    if (in.map == 1 && in.pp == 1) {
        switch (in.opc) {
        case 0xfc: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return uint8_t(a + b); }); return true;
        case 0xfd: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return uint16_t(a + b); }); return true;
        case 0xfe: ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a + b; }); return true;
        case 0xd4: ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a + b; }); return true;
        case 0xf8: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return uint8_t(a - b); }); return true;
        case 0xf9: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return uint16_t(a - b); }); return true;
        case 0xfa: ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a - b; }); return true;
        case 0xfb: ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a - b; }); return true;
        case 0xdc: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return sat<uint8_t>(int64_t(a) + b); }); return true;
        case 0xdd: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return sat<uint16_t>(int64_t(a) + b); }); return true;
        case 0xd8: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return sat<uint8_t>(int64_t(a) - b); }); return true;
        case 0xd9: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return sat<uint16_t>(int64_t(a) - b); }); return true;
        case 0xec: ew3<int8_t>(c, [](int8_t a, int8_t b, int) { return sat<int8_t>(int64_t(a) + b); }); return true;
        case 0xed: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return sat<int16_t>(int64_t(a) + b); }); return true;
        case 0xe8: ew3<int8_t>(c, [](int8_t a, int8_t b, int) { return sat<int8_t>(int64_t(a) - b); }); return true;
        case 0xe9: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return sat<int16_t>(int64_t(a) - b); }); return true;
        case 0xe0: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return uint8_t((a + b + 1) >> 1); }); return true;
        case 0xe3: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return uint16_t((a + b + 1) >> 1); }); return true;
        case 0xd5: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return int16_t(int32_t(a) * b); }); return true;
        case 0xe5: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return int16_t((int32_t(a) * b) >> 16); }); return true;
        case 0xe4: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return uint16_t((uint32_t(a) * b) >> 16); }); return true;
        case 0xf4: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return uint64_t(uint32_t(a)) * uint32_t(b); }); return true;
        case 0xf5: { // pmaddwd
            Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r); c.rm_src(b, c.vl, 4);
            for (int i = 0; i < c.vl / 4; i++) r.d[i] = uint32_t(int32_t(a.sw[2 * i]) * b.sw[2 * i]) + uint32_t(int32_t(a.sw[2 * i + 1]) * b.sw[2 * i + 1]);
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0xf6: { // psadbw (no masking)
            Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r); c.rm_src(b, c.vl, 8, false);
            for (int i = 0; i < c.vl / 8; i++) { uint64_t s = 0; for (int k = 0; k < 8; k++) { int d = int(a.b[8 * i + k]) - int(b.b[8 * i + k]); s += d < 0 ? -d : d; } r.q[i] = s; }
            c.write_raw(c.reg(), r, c.vl); return true; }
        case 0xda: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return a < b ? a : b; }); return true;
        case 0xde: ew3<uint8_t>(c, [](uint8_t a, uint8_t b, int) { return a > b ? a : b; }); return true;
        case 0xea: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return a < b ? a : b; }); return true;
        case 0xee: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return a > b ? a : b; }); return true;
        case 0xdb: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a & b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a & b; }); return true;
        case 0xdf: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return ~a & b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return ~a & b; }); return true;
        case 0xeb: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a | b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a | b; }); return true;
        case 0xef: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a ^ b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a ^ b; }); return true;
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x04: { // pmaddubsw: vvvv unsigned bytes * rm signed bytes
            Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r); c.rm_src(b, c.vl, 2);
            for (int i = 0; i < c.vl / 2; i++) r.sw[i] = sat<int16_t>(int32_t(a.b[2 * i]) * b.sb[2 * i] + int32_t(a.b[2 * i + 1]) * b.sb[2 * i + 1]);
            c.write(c.reg(), r, 2, c.vl); return true; }
        case 0x0b: ew3<int16_t>(c, [](int16_t a, int16_t b, int) { return int16_t((((int32_t(a) * b) >> 14) + 1) >> 1); }); return true;
        case 0x1c: ew2<int8_t>(c, [](int8_t b, int) { return int8_t(b < 0 ? -b : b); }); return true;
        case 0x1d: ew2<int16_t>(c, [](int16_t b, int) { return int16_t(b < 0 ? -b : b); }); return true;
        case 0x1e: if (W) return false; ew2<uint32_t>(c, [](uint32_t b, int) { return int32_t(b) < 0 ? 0u - b : b; }); return true;
        case 0x1f: if (!W) return false; ew2<uint64_t>(c, [](uint64_t b, int) { return int64_t(b) < 0 ? 0ull - b : b; }); return true;
        case 0x28: if (!W) return false; ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return uint64_t(int64_t(int32_t(a)) * int64_t(int32_t(b))); }); return true;
        case 0x38: ew3<int8_t>(c, [](int8_t a, int8_t b, int) { return a < b ? a : b; }); return true;
        case 0x3c: ew3<int8_t>(c, [](int8_t a, int8_t b, int) { return a > b ? a : b; }); return true;
        case 0x3a: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return a < b ? a : b; }); return true;
        case 0x3e: ew3<uint16_t>(c, [](uint16_t a, uint16_t b, int) { return a > b ? a : b; }); return true;
        case 0x39: if (W) ew3<int64_t>(c, [](int64_t a, int64_t b, int) { return a < b ? a : b; }); else ew3<int32_t>(c, [](int32_t a, int32_t b, int) { return a < b ? a : b; }); return true;
        case 0x3d: if (W) ew3<int64_t>(c, [](int64_t a, int64_t b, int) { return a > b ? a : b; }); else ew3<int32_t>(c, [](int32_t a, int32_t b, int) { return a > b ? a : b; }); return true;
        case 0x3b: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a < b ? a : b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a < b ? a : b; }); return true;
        case 0x3f: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a > b ? a : b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a > b ? a : b; }); return true;
        case 0x40: if (W) ew3<uint64_t>(c, [](uint64_t a, uint64_t b, int) { return a * b; }); else ew3<uint32_t>(c, [](uint32_t a, uint32_t b, int) { return a * b; }); return true;
        case 0x44: if (W) ew2<uint64_t>(c, [](uint64_t b, int) { return b ? uint64_t(__builtin_clzll(b)) : 64ull; });
                   else ew2<uint32_t>(c, [](uint32_t b, int) { return b ? uint32_t(__builtin_clz(b)) : 32u; }); return true;
        case 0x54: if (W) ew2<uint16_t>(c, [](uint16_t b, int) { return popcnt(b); }); else ew2<uint8_t>(c, [](uint8_t b, int) { return popcnt(b); }); return true;
        case 0x55: if (W) ew2<uint64_t>(c, [](uint64_t b, int) { return popcnt(b); }); else ew2<uint32_t>(c, [](uint32_t b, int) { return popcnt(b); }); return true;
        case 0x50: case 0x51: { // vpdpbusd(s): vvvv unsigned bytes * rm signed bytes
            if (W) return false; bool s = in.opc & 1;
            ew3d<uint32_t>(c, [s](uint32_t d, uint32_t a, uint32_t b, int) {
                int64_t t = 0;
                for (int k = 0; k < 4; k++) t += int64_t(uint8_t(a >> 8 * k)) * int8_t(b >> 8 * k);
                return s ? uint32_t(sat<int32_t>(int64_t(int32_t(d)) + t)) : uint32_t(d + uint32_t(t)); });
            return true; }
        case 0x52: case 0x53: { // vpdpwssd(s)
            if (W) return false; bool s = in.opc & 1;
            ew3d<uint32_t>(c, [s](uint32_t d, uint32_t a, uint32_t b, int) {
                int64_t t = int64_t(int16_t(a)) * int16_t(b) + int64_t(int16_t(a >> 16)) * int16_t(b >> 16);
                return s ? uint32_t(sat<int32_t>(int64_t(int32_t(d)) + t)) : uint32_t(d + uint32_t(t)); });
            return true; }
        case 0x64: blendm(c, W ? 8 : 4); return true;
        case 0x65: blendm(c, W ? 8 : 4); return true;
        case 0x66: blendm(c, W ? 2 : 1); return true;
        case 0xb4: case 0xb5: { // vpmadd52luq/huq
            if (!W) return false; bool hi = in.opc & 1;
            ew3d<uint64_t>(c, [hi](uint64_t d, uint64_t a, uint64_t b, int) {
                const uint64_t M = (1ull << 52) - 1;
                unsigned __int128 p = (unsigned __int128)(a & M) * (b & M);
                return d + (hi ? uint64_t(p >> 52) & M : uint64_t(p) & M); });
            return true; }
        case 0xc4: if (W) conflict<uint64_t>(c); else conflict<uint32_t>(c); return true;
        case 0x83: { // vpmultishiftqb: byte i of qword j = 8 bits of rm.q[j] starting at vvvv.b[8j+i] & 63
            if (!W) return false;
            Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r); c.rm_src(b, c.vl, 8, false);
            for (int j = 0; j < c.vl / 8; j++) for (int i = 0; i < 8; i++) {
                unsigned sh = a.b[8 * j + i] & 63; uint64_t q = b.q[j];
                r.b[8 * j + i] = uint8_t(sh ? (q >> sh) | (q << (64 - sh)) : q);
            }
            c.write(c.reg(), r, 1, c.vl); return true; }
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1 && in.opc == 0x25) {
        int imm = c.imm();
        if (W) ew3d<uint64_t>(c, [imm](uint64_t d, uint64_t a, uint64_t b, int) { return ternlog(d, a, b, imm); });
        else ew3d<uint32_t>(c, [imm](uint32_t d, uint32_t a, uint32_t b, int) { return ternlog(d, a, b, imm); });
        return true;
    }
    return false;
}

} // namespace emu
