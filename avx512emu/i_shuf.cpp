// Shuffles, unpacks, packs, permutes, aligns, compress/expand, multishift.
#include "interp.h"
#include <limits>

namespace emu {

template <class T> static T gete(const Z& z, int i) { T v; memcpy(&v, z.b + i * sizeof(T), sizeof(T)); return v; }
template <class T> static void sete(Z& z, int i, T v) { memcpy(z.b + i * sizeof(T), &v, sizeof(T)); }

// a = vvvv, b = rm (broadcast with element es)
static void src2(IC& c, Z& a, Z& b, int es) { a = c.R(c.vvvv()); c.rm_src(b, c.vl, es, false); }

static void unpck(IC& c, int es, bool hi) {
    Z a, b, r; memset(&r, 0, sizeof r); src2(c, a, b, es);
    int per = 16 / es; // elements per lane
    for (int l = 0; l < c.vl / 16; l++)
        for (int j = 0; j < per / 2; j++) {
            int s = l * per + j + (hi ? per / 2 : 0);
            memcpy(r.b + (l * per + 2 * j) * es, a.b + s * es, es);
            memcpy(r.b + (l * per + 2 * j + 1) * es, b.b + s * es, es);
        }
    c.write(c.reg(), r, es, c.vl);
}

template <class S, class D> static D sat(S v) {
    if (v < (S)std::numeric_limits<D>::min()) return std::numeric_limits<D>::min();
    if (v > (S)std::numeric_limits<D>::max()) return std::numeric_limits<D>::max();
    return (D)v;
}
template <class S, class D> static void pack(IC& c) {
    const int ss = sizeof(S), ds = sizeof(D);
    Z a, b, r; memset(&r, 0, sizeof r); src2(c, a, b, ss);
    int per = 16 / ss;
    for (int l = 0; l < c.vl / 16; l++)
        for (int j = 0; j < per; j++) {
            sete<D>(r, l * 2 * per + j, sat<int64_t, D>(gete<S>(a, l * per + j)));
            sete<D>(r, l * 2 * per + per + j, sat<int64_t, D>(gete<S>(b, l * per + j)));
        }
    c.write(c.reg(), r, ds, c.vl);
}

// valign: concat (a:b), shift right by imm elements
static void valign(IC& c, int es) {
    Z a, b, r; memset(&r, 0, sizeof r); src2(c, a, b, es);
    int n = c.vl / es, sh = c.imm() & (n - 1);
    for (int i = 0; i < n; i++) {
        int s = i + sh;
        memcpy(r.b + i * es, s < n ? b.b + s * es : a.b + (s - n) * es, es);
    }
    c.write(c.reg(), r, es, c.vl);
}

// variable permute across the vector: dst[i] = tbl[idx[i] & (n-1)], idx = vvvv, tbl = rm
template <class T> static void permvar(IC& c) {
    const int es = sizeof(T), n = c.vl / es;
    Z a, b, r; memset(&r, 0, sizeof r); src2(c, a, b, es);
    for (int i = 0; i < n; i++) sete<T>(r, i, gete<T>(b, int(gete<T>(a, i) & T(n - 1))));
    c.write(c.reg(), r, es, c.vl);
}

// two-table permute. i2: index in reg, tables vvvv / rm. t2: index vvvv, tables reg / rm.
template <class T> static void perm2(IC& c, bool i2) {
    const int es = sizeof(T), n = c.vl / es;
    Z t2; c.rm_src(t2, c.vl, es, false);
    Z idx = i2 ? c.R(c.reg()) : c.R(c.vvvv());
    Z t1 = i2 ? c.R(c.vvvv()) : c.R(c.reg());
    Z r; memset(&r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        T x = gete<T>(idx, i);
        int j = int(x & T(n - 1));
        sete<T>(r, i, (x & T(n)) ? gete<T>(t2, j) : gete<T>(t1, j));
    }
    c.write(c.reg(), r, es, c.vl);
}

static void shuf128(IC& c, int es) { // vshuff32x4 etc.
    Z a, b, r; memset(&r, 0, sizeof r); src2(c, a, b, es);
    int imm = c.imm();
    if (c.vl == 64) {
        memcpy(r.b + 0, a.b + 16 * (imm & 3), 16);
        memcpy(r.b + 16, a.b + 16 * ((imm >> 2) & 3), 16);
        memcpy(r.b + 32, b.b + 16 * ((imm >> 4) & 3), 16);
        memcpy(r.b + 48, b.b + 16 * ((imm >> 6) & 3), 16);
    } else {
        memcpy(r.b + 0, a.b + 16 * (imm & 1), 16);
        memcpy(r.b + 16, b.b + 16 * ((imm >> 1) & 1), 16);
    }
    c.write(c.reg(), r, es, c.vl);
}

static void compress(IC& c, int es) {
    const int n = c.vl / es;
    const Z& s = c.R(c.reg());
    Z r; memset(&r, 0, sizeof r);
    int k = 0;
    for (int i = 0; i < n; i++) if (c.mbit(i)) { memcpy(r.b + k * es, s.b + i * es, es); k++; }
    if (c.mem) { c.stn(c.ea, r.b, k * es); return; }
    Z d = c.R(c.rmreg());
    Z o; memset(&o, 0, sizeof o);
    memcpy(o.b, r.b, k * es);
    if (!c.in.z) memcpy(o.b + k * es, d.b + k * es, (n - k) * es);
    c.write_raw(c.rmreg(), o, c.vl);
}

static void expand(IC& c, int es) {
    const int n = c.vl / es;
    Z s; memset(&s, 0, sizeof s);
    int cnt = 0; for (int i = 0; i < n; i++) if (c.mbit(i)) cnt++;
    if (c.mem) c.ldn(s.b, c.ea, cnt * es); else s = c.R(c.rmreg());
    Z r; memset(&r, 0, sizeof r);
    int k = 0;
    for (int i = 0; i < n; i++) if (c.mbit(i)) { memcpy(r.b + i * es, s.b + k * es, es); k++; }
    c.write(c.reg(), r, es, c.vl);
}

bool fam_shuf(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    const int W = in.W, imm = c.imm();
    if (in.map == 1) {
        switch (in.pp << 8 | in.opc) {
        case 0x0c6: { if (W) return false; // vshufps
            Z a, b, r; src2(c, a, b, 4);
            for (int l = 0; l < c.vl / 16; l++) {
                r.d[l * 4 + 0] = a.d[l * 4 + (imm & 3)];
                r.d[l * 4 + 1] = a.d[l * 4 + ((imm >> 2) & 3)];
                r.d[l * 4 + 2] = b.d[l * 4 + ((imm >> 4) & 3)];
                r.d[l * 4 + 3] = b.d[l * 4 + ((imm >> 6) & 3)];
            }
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0x1c6: { if (!W) return false; // vshufpd
            Z a, b, r; src2(c, a, b, 8);
            for (int i = 0; i < c.vl / 8; i++) { int l = i & ~1; r.q[i] = ((i & 1) ? b : a).q[l + ((imm >> i) & 1)]; }
            c.write(c.reg(), r, 8, c.vl); return true; }
        case 0x014: if (W) return false; unpck(c, 4, false); return true;
        case 0x015: if (W) return false; unpck(c, 4, true); return true;
        case 0x114: if (!W) return false; unpck(c, 8, false); return true;
        case 0x115: if (!W) return false; unpck(c, 8, true); return true;
        case 0x160: unpck(c, 1, false); return true;
        case 0x161: unpck(c, 2, false); return true;
        case 0x162: if (W) return false; unpck(c, 4, false); return true;
        case 0x16c: if (!W) return false; unpck(c, 8, false); return true;
        case 0x168: unpck(c, 1, true); return true;
        case 0x169: unpck(c, 2, true); return true;
        case 0x16a: if (W) return false; unpck(c, 4, true); return true;
        case 0x16d: if (!W) return false; unpck(c, 8, true); return true;
        case 0x163: pack<int16_t, int8_t>(c); return true;     // vpacksswb
        case 0x16b: if (W) return false; pack<int32_t, int16_t>(c); return true; // vpackssdw
        case 0x167: pack<int16_t, uint8_t>(c); return true;    // vpackuswb
        case 0x170: { if (W) return false; // vpshufd
            Z s, r; c.rm_src(s, c.vl, 4, false);
            for (int i = 0; i < c.vl / 4; i++) r.d[i] = s.d[(i & ~3) + ((imm >> (2 * (i & 3))) & 3)];
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0x270: case 0x370: { // vpshufhw / vpshuflw
            Z s, r; c.rm_src(s, c.vl, 2, false); r = s;
            int base = in.pp == 2 ? 4 : 0;
            for (int l = 0; l < c.vl / 16; l++)
                for (int j = 0; j < 4; j++) r.w[l * 8 + base + j] = s.w[l * 8 + base + ((imm >> (2 * j)) & 3)];
            c.write(c.reg(), r, 2, c.vl); return true; }
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x00: { // vpshufb
            Z a, b, r; src2(c, a, b, 1);
            for (int i = 0; i < c.vl; i++) r.b[i] = (b.b[i] & 0x80) ? 0 : a.b[(i & ~15) + (b.b[i] & 15)];
            c.write(c.reg(), r, 1, c.vl); return true; }
        case 0x2b: if (W) return false; pack<int32_t, uint16_t>(c); return true; // vpackusdw
        case 0x0c: { if (W) return false; // vpermilps var
            Z a, b, r; src2(c, a, b, 4);
            for (int i = 0; i < c.vl / 4; i++) r.d[i] = a.d[(i & ~3) + (b.d[i] & 3)];
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0x0d: { if (!W) return false; // vpermilpd var
            Z a, b, r; src2(c, a, b, 8);
            for (int i = 0; i < c.vl / 8; i++) r.q[i] = a.q[(i & ~1) + ((b.q[i] >> 1) & 1)];
            c.write(c.reg(), r, 8, c.vl); return true; }
        case 0x16: case 0x36: // vpermps/pd, vpermd/q
            if (c.vl == 16) return false;
            if (W) permvar<uint64_t>(c); else permvar<uint32_t>(c); return true;
        case 0x8d: if (W) permvar<uint16_t>(c); else permvar<uint8_t>(c); return true; // vpermw / vpermb
        case 0x75: if (W) perm2<uint16_t>(c, true); else perm2<uint8_t>(c, true); return true;
        case 0x76: case 0x77: if (W) perm2<uint64_t>(c, true); else perm2<uint32_t>(c, true); return true;
        case 0x7d: if (W) perm2<uint16_t>(c, false); else perm2<uint8_t>(c, false); return true;
        case 0x7e: case 0x7f: if (W) perm2<uint64_t>(c, false); else perm2<uint32_t>(c, false); return true;
        case 0x8a: case 0x8b: compress(c, W ? 8 : 4); return true;   // vcompressps/pd, vpcompressd/q
        case 0x63: compress(c, W ? 2 : 1); return true;              // vpcompressb/w
        case 0x88: case 0x89: expand(c, W ? 8 : 4); return true;     // vexpandps/pd, vpexpandd/q
        case 0x62: expand(c, W ? 2 : 1); return true;                // vpexpandb/w
        case 0x83: { if (!W) return false; // vpmultishiftqb
            Z a, b, r; src2(c, a, b, 8);
            for (int i = 0; i < c.vl / 8; i++)
                for (int j = 0; j < 8; j++) {
                    int s = a.b[i * 8 + j] & 63;
                    uint64_t q = b.q[i];
                    r.b[i * 8 + j] = uint8_t(s ? (q >> s) | (q << (64 - s)) : q);
                }
            c.write(c.reg(), r, 1, c.vl); return true; }
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        switch (in.opc) {
        case 0x00: case 0x01: { if (!W || c.vl == 16) return false; // vpermq / vpermpd imm
            Z s, r; c.rm_src(s, c.vl, 8, false);
            for (int i = 0; i < c.vl / 8; i++) r.q[i] = s.q[(i & ~3) + ((imm >> (2 * (i & 3))) & 3)];
            c.write(c.reg(), r, 8, c.vl); return true; }
        case 0x04: { if (W) return false; // vpermilps imm
            Z s, r; c.rm_src(s, c.vl, 4, false);
            for (int i = 0; i < c.vl / 4; i++) r.d[i] = s.d[(i & ~3) + ((imm >> (2 * (i & 3))) & 3)];
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0x05: { if (!W) return false; // vpermilpd imm
            Z s, r; c.rm_src(s, c.vl, 8, false);
            for (int i = 0; i < c.vl / 8; i++) r.q[i] = s.q[(i & ~1) + ((imm >> i) & 1)];
            c.write(c.reg(), r, 8, c.vl); return true; }
        case 0x03: valign(c, W ? 8 : 4); return true;
        case 0x0f: { // vpalignr
            Z a, b, r; src2(c, a, b, 1);
            for (int l = 0; l < c.vl / 16; l++) {
                uint8_t t[32]; memcpy(t, b.b + l * 16, 16); memcpy(t + 16, a.b + l * 16, 16);
                for (int j = 0; j < 16; j++) r.b[l * 16 + j] = (j + imm < 32) ? t[j + imm] : 0;
            }
            c.write(c.reg(), r, 1, c.vl); return true; }
        case 0x23: case 0x43: if (c.vl == 16) return false; shuf128(c, W ? 8 : 4); return true;
        }
        return false;
    }
    return false;
}

} // namespace emu
