// Moves, loads, stores, broadcasts, inserts and extracts.
#include "interp.h"

namespace emu {

static void load_vec(IC& c, int es) { // dst = rm (masked, zeroing), fault suppression
    Z v; c.rm_src(v, c.vl, es);
    c.write(c.reg(), v, es, c.vl);
}
static void store_vec(IC& c, int es) { // rm = reg
    const Z& v = c.R(c.reg());
    if (c.mem) { if (c.in.aaa == 0) c.stn(c.ea, v.b, c.vl); else c.store_masked(v, es, c.vl); }
    else c.write(c.rmreg(), v, es, c.vl);
}

// vmovss / vmovsd
static void movs(IC& c, int es, bool store) {
    if (!store) {
        if (c.mem) {
            Z r; memset(&r, 0, sizeof r);
            if (c.mbit(0)) c.ldn(r.b, c.ea, es);
            else if (!c.in.z) memcpy(r.b, c.R(c.reg()).b, es);
            c.write_raw(c.reg(), r, 16);
        } else {
            Z r = c.R(c.vvvv());
            if (c.mbit(0)) memcpy(r.b, c.R(c.rmreg()).b, es);
            else if (c.in.z) memset(r.b, 0, es);
            else memcpy(r.b, c.R(c.reg()).b, es);
            c.write_raw(c.reg(), r, 16);
        }
    } else {
        if (c.mem) { if (c.mbit(0)) c.stn(c.ea, c.R(c.reg()).b, es); }
        else {
            Z r = c.R(c.vvvv());
            if (c.mbit(0)) memcpy(r.b, c.R(c.reg()).b, es);
            else if (c.in.z) memset(r.b, 0, es);
            else memcpy(r.b, c.R(c.rmreg()).b, es);
            c.write_raw(c.rmreg(), r, 16);
        }
    }
}

static void broadcast(IC& c, int es, int tbytes) { // tile of tbytes from rm, masked by es
    uint8_t t[64]; memset(t, 0, sizeof t);
    if (c.mem && c.in.aaa) { // fault suppression: source element j only if a lane using it is active
        for (int j = 0; j < tbytes / es; j++) {
            bool used = false;
            for (int i = j; i < c.vl / es && !used; i += tbytes / es) used = c.mbit(i);
            if (used) c.ldn(t + j * es, c.ea + j * es, es);
        }
    } else if (c.mem) c.ldn(t, c.ea, tbytes); else memcpy(t, c.R(c.rmreg()).b, tbytes);
    Z v; for (int i = 0; i < c.vl; i += tbytes) memcpy(v.b + i, t, tbytes);
    c.write(c.reg(), v, es, c.vl);
}
static void broadcast_gpr(IC& c, int es) {
    uint64_t g = c.gpr(c.rmreg() & 15);
    Z v; for (int i = 0; i < c.vl; i += es) memcpy(v.b + i, &g, es);
    c.write(c.reg(), v, es, c.vl);
}

// insert a 128/256-bit piece: dst = vvvv; dst.piece[imm] = rm
static void insert(IC& c, int es, int piece) {
    Z v = c.R(c.vvvv()); uint8_t t[32];
    if (c.mem) c.ldn(t, c.ea, piece); else memcpy(t, c.R(c.rmreg()).b, piece);
    int idx = c.in.len ? 0 : 0; (void)idx;
    int sel = 0; // immediate
    sel = c.imm() & ((c.vl / piece) - 1);
    memcpy(v.b + sel * piece, t, piece);
    c.write(c.reg(), v, es, c.vl);
}
// extract: rm = reg.piece[imm]
static void extract(IC& c, int es, int piece) {
    int sel = c.imm() & ((c.vl / piece) - 1);
    Z v; memset(&v, 0, sizeof v); memcpy(v.b, c.R(c.reg()).b + sel * piece, piece);
    if (c.mem) { if (c.in.aaa == 0) c.stn(c.ea, v.b, piece); else c.store_masked(v, es, piece); }
    else c.write(c.rmreg(), v, es, piece);
}

bool fam_mov(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX) return false;
    int W = in.W;
    if (in.map == 1) {
        switch (in.pp << 8 | in.opc) {
        case 0x010: case 0x028: if (W) return false; load_vec(c, 4); return true;          // vmovups/aps
        case 0x011: case 0x029: if (W) return false; store_vec(c, 4); return true;
        case 0x110: case 0x128: if (!W) return false; load_vec(c, 8); return true;         // vmovupd/apd
        case 0x111: case 0x129: if (!W) return false; store_vec(c, 8); return true;
        case 0x16f: load_vec(c, W ? 8 : 4); return true;                                   // vmovdqa32/64
        case 0x17f: store_vec(c, W ? 8 : 4); return true;
        case 0x26f: load_vec(c, W ? 8 : 4); return true;                                   // vmovdqu32/64
        case 0x27f: store_vec(c, W ? 8 : 4); return true;
        case 0x36f: load_vec(c, W ? 2 : 1); return true;                                   // vmovdqu8/16
        case 0x37f: store_vec(c, W ? 2 : 1); return true;
        case 0x210: if (W) return false; movs(c, 4, false); return true;                   // vmovss
        case 0x211: if (W) return false; movs(c, 4, true); return true;
        case 0x310: if (!W) return false; movs(c, 8, false); return true;                  // vmovsd
        case 0x311: if (!W) return false; movs(c, 8, true); return true;
        case 0x012: case 0x112: { // vmovhlps (reg) / vmovlps, vmovlpd (load)
            Z r = c.R(c.vvvv());
            if (c.mem) c.ldn(r.b, c.ea, 8); else memcpy(r.b, c.R(c.rmreg()).b + 8, 8);
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x013: case 0x113: if (!c.mem) return false; c.stn(c.ea, c.R(c.reg()).b, 8); return true; // vmovlps/lpd store
        case 0x016: case 0x116: { // vmovhps/hpd load, vmovlhps
            Z r = c.R(c.vvvv());
            if (c.mem) c.ldn(r.b + 8, c.ea, 8); else memcpy(r.b + 8, c.R(c.rmreg()).b, 8);
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x017: case 0x117: if (!c.mem) return false; c.stn(c.ea, c.R(c.reg()).b + 8, 8); return true; // vmovhps/hpd store
        case 0x16e: { // vmovd/q xmm <- r/m
            Z r; memset(&r, 0, sizeof r);
            if (c.mem) c.ldn(r.b, c.ea, W ? 8 : 4); else { uint64_t g = c.gpr(c.rmreg() & 15); memcpy(r.b, &g, W ? 8 : 4); }
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x17e: { // vmovd/q r/m <- xmm
            if (c.mem) c.stn(c.ea, c.R(c.reg()).b, W ? 8 : 4);
            else { uint64_t g = 0; memcpy(&g, c.R(c.reg()).b, W ? 8 : 4); c.set_gpr64(c.rmreg() & 15, g); }
            return true; }
        case 0x27e: { if (!W) return false; Z r; memset(&r, 0, sizeof r); // vmovq xmm <- xmm/m64
            if (c.mem) c.ldn(r.b, c.ea, 8); else memcpy(r.b, c.R(c.rmreg()).b, 8);
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x1d6: { if (!W) return false; // vmovq xmm/m64 <- xmm
            if (c.mem) c.stn(c.ea, c.R(c.reg()).b, 8);
            else { Z r; memset(&r, 0, sizeof r); memcpy(r.b, c.R(c.reg()).b, 8); c.write_raw(c.rmreg(), r, 16); }
            return true; }
        case 0x212: case 0x216: { if (W) return false; // vmovsldup / vmovshdup
            Z s; c.rm_src(s, c.vl, 4); Z r; int o = in.opc == 0x16;
            for (int i = 0; i < c.vl / 4; i += 2) { r.d[i] = s.d[i + o]; r.d[i + 1] = s.d[i + o]; }
            c.write(c.reg(), r, 4, c.vl); return true; }
        case 0x312: { if (!W) return false; // vmovddup
            Z s; memset(&s, 0, sizeof s);
            if (c.vl == 16) { if (c.mem) c.ldn(s.b, c.ea, 8); else memcpy(s.b, c.R(c.rmreg()).b, 8); }
            else c.rm_src(s, c.vl, 8, false);
            Z r; for (int i = 0; i < c.vl / 8; i += 2) { r.q[i] = s.q[i]; r.q[i + 1] = s.q[i]; }
            c.write(c.reg(), r, 8, c.vl); return true; }
        case 0x1c4: { // vpinsrw
            Z r = c.R(c.vvvv()); uint16_t v;
            if (c.mem) v = c.ld<uint16_t>(c.ea); else v = uint16_t(c.gpr(c.rmreg() & 15));
            r.w[c.imm() & 7] = v; c.write_raw(c.reg(), r, 16); return true; }
        case 0x1c5: { // vpextrw r32 <- xmm (reg form)
            if (c.mem) return false;
            c.set_gpr64(c.reg() & 15, c.R(c.rmreg()).w[c.imm() & 7]); return true; }
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        switch (in.opc) {
        case 0x18: if (W) return false; broadcast(c, 4, 4); return true;                  // vbroadcastss
        case 0x19: broadcast(c, W ? 8 : 4, 8); return true;                                // vbroadcastsd / f32x2
        case 0x1a: if (!c.mem) return false; broadcast(c, W ? 8 : 4, 16); return true;      // vbroadcastf32x4/f64x2
        case 0x1b: if (!c.mem) return false; broadcast(c, W ? 8 : 4, 32); return true;      // vbroadcastf32x8/f64x4
        case 0x58: if (W) return false; broadcast(c, 4, 4); return true;                  // vpbroadcastd
        case 0x59: broadcast(c, W ? 8 : 4, 8); return true;                                // vpbroadcastq / i32x2
        case 0x5a: if (!c.mem) return false; broadcast(c, W ? 8 : 4, 16); return true;      // vbroadcasti32x4/i64x2
        case 0x5b: if (!c.mem) return false; broadcast(c, W ? 8 : 4, 32); return true;      // vbroadcasti32x8/i64x4
        case 0x78: if (W) return false; broadcast(c, 1, 1); return true;                  // vpbroadcastb
        case 0x79: if (W) return false; broadcast(c, 2, 2); return true;                  // vpbroadcastw
        case 0x7a: if (W || c.mem) return false; broadcast_gpr(c, 1); return true;
        case 0x7b: if (W || c.mem) return false; broadcast_gpr(c, 2); return true;
        case 0x7c: if (c.mem) return false; broadcast_gpr(c, W ? 8 : 4); return true;
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        switch (in.opc) {
        case 0x18: case 0x38: insert(c, W ? 8 : 4, 16); return true;   // vinsert[fi]32x4 / 64x2
        case 0x1a: case 0x3a: insert(c, W ? 8 : 4, 32); return true;   // vinsert[fi]32x8 / 64x4
        case 0x19: case 0x39: extract(c, W ? 8 : 4, 16); return true;  // vextract[fi]32x4 / 64x2
        case 0x1b: case 0x3b: extract(c, W ? 8 : 4, 32); return true;  // vextract[fi]32x8 / 64x4
        case 0x14: { // vpextrb
            uint8_t v = c.R(c.reg()).b[c.imm() & 15];
            if (c.mem) c.st<uint8_t>(c.ea, v); else c.set_gpr64(c.rmreg() & 15, v); return true; }
        case 0x15: { // vpextrw (mem/reg)
            uint16_t v = c.R(c.reg()).w[c.imm() & 7];
            if (c.mem) c.st<uint16_t>(c.ea, v); else c.set_gpr64(c.rmreg() & 15, v); return true; }
        case 0x16: { // vpextrd/q
            if (W) { uint64_t v = c.R(c.reg()).q[c.imm() & 1]; if (c.mem) c.st(c.ea, v); else c.set_gpr64(c.rmreg() & 15, v); }
            else { uint32_t v = c.R(c.reg()).d[c.imm() & 3]; if (c.mem) c.st(c.ea, v); else c.set_gpr64(c.rmreg() & 15, v); }
            return true; }
        case 0x17: { // vextractps
            uint32_t v = c.R(c.reg()).d[c.imm() & 3]; if (c.mem) c.st(c.ea, v); else c.set_gpr64(c.rmreg() & 15, v); return true; }
        case 0x20: { // vpinsrb
            Z r = c.R(c.vvvv()); r.b[c.imm() & 15] = c.mem ? c.ld<uint8_t>(c.ea) : uint8_t(c.gpr(c.rmreg() & 15));
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x22: { // vpinsrd/q
            Z r = c.R(c.vvvv());
            if (W) r.q[c.imm() & 1] = c.mem ? c.ld<uint64_t>(c.ea) : c.gpr(c.rmreg() & 15);
            else r.d[c.imm() & 3] = c.mem ? c.ld<uint32_t>(c.ea) : uint32_t(c.gpr(c.rmreg() & 15));
            c.write_raw(c.reg(), r, 16); return true; }
        case 0x21: { // vinsertps
            int imm = c.imm(); uint32_t v = c.mem ? c.ld<uint32_t>(c.ea) : c.R(c.rmreg()).d[(imm >> 6) & 3];
            Z r = c.R(c.vvvv()); r.d[(imm >> 4) & 3] = v;
            for (int i = 0; i < 4; i++) if (imm & (1 << i)) r.d[i] = 0;
            c.write_raw(c.reg(), r, 16); return true; }
        }
        return false;
    }
    return false;
}

} // namespace emu
