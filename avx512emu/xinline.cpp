// Inline AVX2 translations of common AVX-512 instructions. Anything not
// handled here returns false and goes to the interpreter.
//
// Register model: hardware ymm0-15 hold bits 0..255 of zmm0-15; bits 256..511
// of zmm0-15 (tst.zhi), all of zmm16-31 (tst.zx) and k0-7 (tst.k) live in the
// thread state, reached as %fs:offset. Scratch registers are spilled to
// tst.ysave / tst.gsave and restored at the end; EFLAGS are never changed
// unless the instruction itself defines them.
#include "xlate.h"
#include "evexinfo.h"
#include "rt.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

namespace emu {
using namespace x86;

namespace {

struct Opd { bool reg; int r; Mem m; };
inline Opd R_(int r) { Opd o; o.reg = true; o.r = r; return o; }
inline Opd M_(const Mem& m) { Opd o; o.reg = false; o.r = -1; o.m = m; return o; }

#define TOFF(f) int32_t(offsetof(TState, f))

uint32_t mask_key_(int k, int es, int c, int L, bool inv) { return 1u | uint32_t(k & 7) << 1 | uint32_t(es) << 4 | uint32_t(c & 3) << 9 | uint32_t(L & 1) << 11 | uint32_t(inv) << 12; }
// all-ones lanes where k bit is set (inv: where clear) for element size es, chunk c, into ymm s
void emit_maskvec(Asm& a, int s, int es, int c, int L, int k, bool inv, int kgpr) {
    int32_t T = tls_offset();
    auto tls = [&](int32_t off) { return Mem::fs(T + off); };
    auto kreg = [&](int kk, int byte) { return tls(TOFF(k) + 8 * kk + byte); };
    Mem kz = tls(TOFF(c_zero)), m;
    switch (es) {
    case 4: { if (kgpr >= 0) { a.vex(1, 1, 0, 0, 0x6E, s, 0, kgpr, nullptr); a.vex(2, 1, 0, L, 0x58, s, 0, s, nullptr); }   // vmovd + vpbroadcastd
              else { m = kreg(k, 0); a.vex(2, 1, 0, L, 0x58, s, 0, 0, &m); }                                                 // vpbroadcastd
        Mem sel = tls(TOFF(c_bitd) + 32 * c); a.vex(1, 1, 0, L, 0xDB, s, s, 0, &sel);
        a.vex(1, 1, 0, L, 0x76, s, s, 0, inv ? &kz : &sel); break; }                          // vpcmpeqd
    case 8: { if (kgpr >= 0) { a.vex(1, 1, 0, 0, 0x6E, s, 0, kgpr, nullptr); a.vex(2, 1, 0, L, 0x59, s, 0, s, nullptr); }   // vmovd + vpbroadcastq (k < 2^8)
              else { m = kreg(k, 0); a.vex(2, 1, 0, L, 0x59, s, 0, 0, &m); }                                                 // vpbroadcastq
        Mem sel = tls(TOFF(c_bitq) + 32 * c); a.vex(1, 1, 0, L, 0xDB, s, s, 0, &sel);
        a.vex(2, 1, 0, L, 0x29, s, s, 0, inv ? &kz : &sel); break; }                          // vpcmpeqq
    case 2: { m = kreg(k, 2 * c); a.vex(2, 1, 0, L, 0x79, s, 0, 0, &m);                       // vpbroadcastw
        Mem sel = tls(TOFF(c_bitw)); a.vex(1, 1, 0, L, 0xDB, s, s, 0, &sel);
        a.vex(1, 1, 0, L, 0x75, s, s, 0, inv ? &kz : &sel); break; }                          // vpcmpeqw
    default: { m = kreg(k, 4 * c); a.vex(2, 1, 0, L, 0x58, s, 0, 0, &m);                      // vpbroadcastd
        Mem sp = tls(TOFF(c_bspread)); a.vex(2, 1, 0, L, 0x00, s, s, 0, &sp);                 // vpshufb
        Mem sel = tls(TOFF(c_bbit)); a.vex(1, 1, 0, L, 0xDB, s, s, 0, &sel);
        a.vex(1, 1, 0, L, 0x74, s, s, 0, inv ? &kz : &sel); break; }                          // vpcmpeqb
    }
}
void emit_maskvec_key_(Asm& a, int reg, uint32_t key) {
    emit_maskvec(a, reg, (key >> 4) & 31, (key >> 9) & 3, (key >> 11) & 1, (key >> 1) & 7, (key >> 12) & 1, -1);
}

struct G {
    Asm& a; const Desc& d; const Insn& in; const XlateOpts& o;
    int32_t T;
    uint32_t vavoid = 0, vtaken = 0, vspilled = 0;
    uint32_t gavoid = 1u << RSP, gtaken = 0, gspilled = 0;
    int vl;
    bool fail = false;
    bool fsave = false;
    void save_flags() { if (fsave || !o.flags_live) return; a.lea(RSP, Mem{RSP, -1, 0, -128}); a.pushfq(); fsave = true; }
    G(Asm& a_, const Desc& d_, const XlateOpts& o_) : a(a_), d(d_), in(d_.in), o(o_) {
        T = tls_offset();
        vl = (in.enc == EVEX) ? ((in.b && in.mod == 3) ? 64 : (16 << (in.L > 2 ? 2 : in.L))) : (in.L ? 32 : 16);
        auto av = [this](int r) { if (r >= 0 && r < 16) vavoid |= 1u << r; };
        for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) vavoid |= 1u << o.xmap[i];
        if (o.mc_reg >= 0) vavoid |= 1u << o.mc_reg;
        for (int i = 0; i < XlateOpts::NKC; i++) if (o.kc_gpr[i] >= 0) gavoid |= 1u << o.kc_gpr[i];
        av(reg_of(in)); av(in.vvvv);
        if (in.mod == 3) av(rm_reg_of(in));
        if (in.has_modrm && in.mod != 3) {
            if (has_base(in)) gavoid |= 1u << base_of(in);
            if (in.has_sib) gavoid |= 1u << index_of(in);
        }
        if (in.enc == VEX) { // opmask instructions: reg/rm are k registers except in kmov to/from a GPR
            if (in.map == 1 && in.opc == 0x93) gavoid |= 1u << (reg_of(in) & 15);
            if (in.map == 1 && in.opc == 0x92 && in.mod == 3) gavoid |= 1u << (rm_reg_of(in) & 15);
        } else {
            gavoid |= 1u << (reg_of(in) & 15);
            if (in.mod == 3) gavoid |= 1u << (rm_reg_of(in) & 15);
        }
    }
    Mem tls(int32_t off) const { return Mem::fs(T + off); }
    Mem zhi(int r) const { return tls(TOFF(zhi) + 32 * r); }
    Mem zx(int r, int byte) const { return tls(TOFF(zx) + 64 * (r - 16) + byte); }
    Mem kreg(int k, int byte = 0) const { return tls(TOFF(k) + 8 * k + byte); }
    Mem omem(int add = 0) const {
        Mem m;
        int32_t disp = mem_disp(in);
        if (in.riprel) { m.rip = true; m.target = d.pc + in.len + int64_t(disp) + add; }
        else {
            m.base = has_base(in) ? base_of(in) : -1;
            m.index = has_index(in) ? index_of(in) : -1;
            m.scale = scale_of(in);
            m.disp = disp + add;
            if (fsave && m.base == RSP) m.disp += 136;
        }
        if (in.seg == 0x64 || in.seg == 0x65) m.seg = in.seg;
        m.adsize32 = in.adsize;
        return m;
    }
    // ---- scratch registers
    int scratch() {
        for (int r = 15; r >= 0; r--) { // dead or group-saved registers first: no save/restore needed
            uint32_t b = 1u << r;
            if (((vavoid | vtaken) & b) || !((o.vdead | o.vpre) & b)) continue;
            vtaken |= b;
            if (o.vpre & b) o.vused |= b;
            o.scr_taken++;
            return r;
        }
        for (int r = 15; r >= 0; r--) {
            uint32_t b = 1u << r;
            if ((vavoid | vtaken) & b) continue;
            vtaken |= b;
            if (!(vspilled & b)) { vspilled |= b; a.vex(1, 2, 0, 1, 0x7F, r, 0, 0, &tmp(tls(TOFF(ysave) + 32 * r))); }
            o.scr_taken++;
            return r;
        }
        fail = true;
        return 15;
    }
    void release(int r) { if (r >= 0 && r == o.mc_reg) { o.mc_busy = false; return; } if (r >= 0) vtaken &= ~(1u << r); }
    int gscratch() {
        static const int order[] = {RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX, RBP, R12, R13, R14, R15};
        for (int r = 15; r >= 0; r--) {                      // dead GPRs first: nothing to save
            uint32_t b = 1u << r;
            if (!(o.gdead & b) || ((gavoid | gtaken) & b) || r == RSP) continue;
            gtaken |= b;
            return r;
        }
        for (int r = 15; r >= 0; r--) {                      // group-saved GPRs next: no spill
            uint32_t b = 1u << r;
            if (!(o.gpre & b) || ((gavoid | gtaken) & b)) continue;
            gtaken |= b; o.gused |= b;
            return r;
        }
        for (int r : order) {
            uint32_t b = 1u << r;
            if ((gavoid | gtaken) & b) continue;
            gtaken |= b;
            if (!(gspilled & b)) { gspilled |= b; a.mov_store64(tls(TOFF(gsave) + 8 * r), r); }
            return r;
        }
        fail = true;
        return RAX;
    }
    int gscratch_fixed(int r) { // a specific GPR (rax for lahf/sahf)
        uint32_t b = 1u << r;
        if ((gavoid | gtaken) & b) return -1;
        gtaken |= b;
        if (!(gspilled & b) && !(o.gdead & b)) { gspilled |= b; a.mov_store64(tls(TOFF(gsave) + 8 * r), r); }
        return r;
    }
    void finish() {
        for (int r = 0; r < 16; r++) if (vspilled & (1u << r)) a.vex(1, 2, 0, 1, 0x6F, r, 0, 0, &tmp(tls(TOFF(ysave) + 32 * r)));
        for (int r = 0; r < 16; r++) if (gspilled & (1u << r)) a.mov_load64(r, tls(TOFF(gsave) + 8 * r));
        if (fsave) { a.popfq(); a.lea(RSP, Mem{RSP, -1, 0, 128}); }
    }
    Mem mtmp;
    const Mem& tmp(const Mem& m) { mtmp = m; return mtmp; }

    // ---- emit helpers
    void vx(int map, int pp, int W, int L, int opc, int dst, int src1, const Opd& s2, int imm = -1) {
        if (s2.reg) a.vex(map, pp, W, L, opc, dst, src1, s2.r, nullptr, imm);
        else a.vex(map, pp, W, L, opc, dst, src1, 0, &s2.m, imm);
    }
    void load(int dst, const Mem& m, int L) { a.vex(1, 2, 0, L, 0x6F, dst, 0, 0, &m); }   // vmovdqu
    void store(const Mem& m, int src, int L) { a.vex(1, 2, 0, L, 0x7F, src, 0, 0, &m); }  // vmovdqu
    void movrr(int d, int s, int L) { if (d != s) a.vex(1, 1, 0, L, 0x6F, d, 0, s, nullptr); } // vmovdqa
    void zero(int r) { a.vex(1, 1, 0, 1, 0xEF, r, r, r, nullptr); }                          // vpxor ymm
    // operand of register r, chunk c (32 bytes each)
    Opd chunk(int r, int c) {
        if (r < 16) {
            if (c == 0) return R_(r);
            if (o.zhi_zero & (1u << r)) return M_(tls(TOFF(c_zero)));
            return M_(zhi(r));
        }
        if (c == 0 && o.xmap[r - 16] >= 0) return R_(o.xmap[r - 16]);
        return M_(zx(r, 32 * c));
    }
    // hardware register holding the low 256 bits of r (xmm0-15, or a zmm16-31 the group maps), else -1
    int hreg(int r) const { if (r < 16) return r; if (r < 32 && o.xmap[r - 16] >= 0) return o.xmap[r - 16]; return -1; }
    int in_reg(const Opd& x, int L) { if (x.reg) return x.r; int s = scratch(); load(s, x.m, L); return s; }
    // write result register res into chunk c of register r (L: 0 xmm, 1 ymm)
    void put(int r, int c, int res, int L) {
        if (r < 16 && c == 0) movrr(r, res, L);
        else if (r < 16) store(zhi(r), res, 1);
        else if (c == 0 && o.xmap[r - 16] >= 0) movrr(o.xmap[r - 16], res, L);   // VEX xmm write zeroes bits 128..255
        else store(zx(r, 32 * c), res, L);
    }
    // zero the bits of register r above vlbytes (EVEX writes zero them)
    void zero_tail(int r, int vlbytes) {
        if (vlbytes >= 64) return;
        if (r < 16) {
            if (!(o.rd512 & (1u << r))) return;
            int z = scratch(); zero(z); store(zhi(r), z, 1); release(z);
            return;
        }
        bool need256 = vlbytes < 32 && (o.rd256 & (1u << r)), need512 = (o.rd512 & (1u << r)) != 0;
        if (!need256 && !need512) return;
        int z = scratch(); zero(z);
        if (vlbytes == 16 && o.xmap[r - 16] < 0) store(zx(r, 16), z, 0);
        store(zx(r, 32), z, 1);
        release(z);
    }
    // k-in-GPR cache (see XlateOpts::kc_gpr)
    int kc(int k) const { for (int i = 0; i < XlateOpts::NKC; i++) if (o.kc_gpr[i] >= 0 && o.kc_k[i] == k) return o.kc_gpr[i]; return -1; }
    void kc_note(int k, int gr) {   // k was just stored from gr: newest entry (an older entry of k is dropped, the oldest goes)
        if (gr < 0 || gr == RSP || (gspilled & (1u << gr))) return;
        int8_t g2[XlateOpts::NKC], k2[XlateOpts::NKC]; int n = 0; g2[n] = int8_t(gr); k2[n++] = int8_t(k);
        for (int i = 0; i < XlateOpts::NKC && n < XlateOpts::NKC; i++) if (o.kc_gpr[i] >= 0 && o.kc_k[i] != k && o.kc_gpr[i] != gr) { g2[n] = o.kc_gpr[i]; k2[n++] = o.kc_k[i]; }
        for (int i = 0; i < XlateOpts::NKC; i++) { o.kc_gpr[i] = i < n ? g2[i] : -1; o.kc_k[i] = i < n ? k2[i] : -1; }
        o.kc_set = true;
    }
    // embedded broadcast {1toN} of a rip-relative read-only constant: a 32-byte pre-broadcast copy in the group's
    // constant pool, used as a full-width memory operand (no vbroadcast into a scratch register)
    bool cpool_bcast(int bs, Mem& out) {
        if (!in.riprel || !o.cpool || (bs != 4 && bs != 8)) return false;
        Mem m = omem();
        if (!ro_mem(m.target, bs)) return false;
        uint8_t e[32]; for (int i = 0; i < 32; i += bs) memcpy(e + i, (const void*)m.target, bs);
        int k = 0;
        for (; k < o.cpool_n; k++) if (!memcmp(o.cpool + 32 * k, e, 32)) break;
        if (k == o.cpool_n) { if (k >= o.cpool_cap) return false; memcpy(o.cpool + 32 * k, e, 32); o.cpool_n++; }
        out = Mem(); out.rip = true; out.target = (uint64_t)(o.cpool + 32 * k);
        return true;
    }
    // scalar masking with the lane mask of k[aaa] cached by the compare that produced it (scalar seed, mc_seed bit 2):
    // element 0 = mask ? res : other, elements 1.. stay res; 2 insns instead of vmovd/shift/xor/blend on k
    bool scalar_blend_cached(int res, const Opd& other, int es) {
        if (!(o.mc_seed & 4) || o.mc_reg < 0 || o.mc_busy || o.mc_key != mask_key_(in.aaa, es, 0, 0, false)) return false;
        int M = maskvec_k(es, 0, 0, in.aaa);
        int Z = scratch();
        if (other.reg) a.vex(3, 1, 0, 0, es == 8 ? 0x0D : 0x0C, Z, res, other.r, nullptr, 1);     // vblendps/pd: Z = [other0, res1..]
        else a.vex(3, 1, 0, 0, es == 8 ? 0x0D : 0x0C, Z, res, 0, &tmp(other.m), 1);
        vx(3, 1, 0, 0, es == 8 ? 0x4B : 0x4A, res, Z, R_(res), M << 4);                           // vblendvps/pd res = M ? res : Z
        release(Z); release(M);
        return true;
    }
    // vmovd k[kk] -> xmm m (from the cached GPR when it holds k[kk]: no reload of a just-stored slot)
    void kmovd(int m, int kk) {
        int gr = kc(kk);
        if (gr >= 0) a.vex(1, 1, 0, 0, 0x6E, m, 0, gr, nullptr);
        else a.vex(1, 1, 0, 0, 0x6E, m, 0, 0, &tmp(kreg(kk)));
    }
    // all-ones lanes where k[aaa] bit is set (inv: where clear) for element size es, chunk c
    int maskvec(int es, int c, int L, bool inv) { return maskvec_k(es, c, L, in.aaa, inv); }
    // Embedded broadcast {1toN} under a mask: AVX-512 reads the element only when one of the `lanes` lanes is
    // active (fault suppression). vmaskmov loads element 0 under "k has an active lane", then it is broadcast
    // to L. Returns the scratch register holding the broadcast value.
    int bcast_masked(int es, int L, int lanes) {
        int m = scratch(), z = scratch(), e = scratch();
        kmovd(m, in.aaa);
        if (lanes < 32) a.vex(1, 1, 0, 0, 0x72, 6, m, m, nullptr, 32 - lanes);           // vpslld: drop k bits above the lanes
        if (es == 8) a.vex(2, 1, 0, 0, 0x29, z, m, 0, &tmp(tls(TOFF(c_zero))));          // vpcmpeqq: all ones if no lane is active
        else a.vex(1, 1, 0, 0, 0x76, z, m, 0, &tmp(tls(TOFF(c_zero))));                  // vpcmpeqd
        a.vex(1, 1, 0, 0, 0xDF, z, z, 0, &tmp(tls(es == 8 ? TOFF(c_sign0q) : TOFF(c_sign0))));  // vpandn: element 0 sign = a lane is active
        a.vex(2, 1, 0, 0, es == 8 ? 0x2D : 0x2C, e, z, 0, &tmp(omem()));                // vmaskmovpd/ps: element 0, or 0
        if (es == 4) a.vex(2, 1, 0, L, 0x18, m, 0, e, nullptr);                         // vbroadcastss
        else if (L) a.vex(2, 1, 0, 1, 0x19, m, 0, e, nullptr);                          // vbroadcastsd ymm
        else a.vex(1, 3, 0, 0, 0x12, m, 0, e, nullptr);                                 // vmovddup xmm
        release(z); release(e);
        return m;
    }
    int maskvec_k(int es, int c, int L, int k, bool inv = false) {
        // a mask vector cached in o.mc_reg by the group (same k, size, chunk, width, polarity) is reused;
        // gathers clear their mask register, so they always build their own
        uint32_t key = mask_key_(k, es, c, L, inv);
        bool gather = in.map == 2 && in.pp == 1 && in.opc >= 0x90 && in.opc <= 0x93;
        if (o.mc_reg >= 0 && !gather && !o.mc_busy) {
            o.mc_busy = true;
            if (o.mc_key == key) return o.mc_reg;
            o.mc_key = 0;
            emit_maskvec(a, o.mc_reg, es, c, L, k, inv, (es == 4 || es == 8) ? kc(k) : -1);
            o.mc_key = key;
            return o.mc_reg;
        }
        int s = scratch();
        emit_maskvec(a, s, es, c, L, k, inv, (es == 4 || es == 8) ? kc(k) : -1);
        return s;
    }
    // apply EVEX masking to chunk result `res` (a scratch) for dst register r, chunk c
    void apply_mask(int res, int r, int c, int es, int L) {
        if (!in.aaa) return;
        if (in.z) { int m = maskvec(es, c, L, false); a.vex(1, 1, 0, L, 0xDB, res, res, m, nullptr); release(m); return; }
        Opd old = chunk(r, c);
        if (old.reg && (o.mc_seed & 1)) {
            int m = maskvec(es, c, L, false);
            vx(3, 1, 0, L, 0x4C, res, old.r, R_(res), m << 4);                       // vpblendvb res = m ? res : old
            release(m); return;
        }
        int m = maskvec(es, c, L, true);
        vx(3, 1, 0, L, 0x4C, res, res, old, m << 4);                                // vpblendvb res = m ? old : res
        release(m);
    }
};

// ---------------------------------------------------------------------------
// Table of EVEX forms whose VEX encoding computes the same per element.
enum Form : uint8_t { RVM, RM, VMI, RVMD, ST, SCAL, SCALD, BC, SHC, LDV };
struct VOp {
    uint8_t vmap, vpp, vopc; int8_t vW; // VEX encoding to emit
    Form form; uint8_t es, bs;          // mask element size, broadcast size (0: none)
    bool imm, inlane = true;
    bool elemwise = true;   // dst element i depends only on source element i (fault suppression applies)
};

static bool lookup(const Insn& in, VOp& v) {
    auto S = [&](int map, int pp, int opc, int W, Form f, int es, int bs, bool imm = false) {
        v.vmap = map; v.vpp = pp; v.vopc = opc; v.vW = W; v.form = f; v.es = es; v.bs = bs; v.imm = imm; v.inlane = true;
        bool cross = (map == 1 && (opc == 0xc6 || opc == 0x14 || opc == 0x15 || (opc >= 0x60 && opc <= 0x6d) || opc == 0x70 || opc == 0x12 || opc == 0x16 || opc == 0xf6)) ||
                     (map == 2 && (opc == 0x00 || opc == 0x2b || opc == 0x36 || opc == 0x16 || opc == 0x04)) || (map == 3);
        v.elemwise = !cross;
        return true;
    };
    int W = in.W;
    if (in.map == 1) {
        int k = in.pp << 8 | in.opc;
        switch (k) {
        // packed single (W0) / double (W1)
        case 0x058: case 0x059: case 0x05c: case 0x05d: case 0x05e: case 0x05f: case 0x054: case 0x055: case 0x056: case 0x057: case 0x014: case 0x015: {
            static int inj = -1; if (inj < 0) inj = getenv("AVX512EMU_TEST_INJECT") != nullptr;   // test of the tests: vaddps -> vsubps
            return !W && S(1, 0, (inj && in.opc == 0x58) ? 0x5c : in.opc, 0, RVM, 4, 4); }
        case 0x158: case 0x159: case 0x15c: case 0x15d: case 0x15e: case 0x15f: case 0x154: case 0x155: case 0x156: case 0x157: case 0x114: case 0x115:
            return W && S(1, 1, in.opc, 0, RVM, 8, 8);
        case 0x051: return !W && S(1, 0, 0x51, 0, RM, 4, 4);
        case 0x151: return W && S(1, 1, 0x51, 0, RM, 8, 8);
        case 0x0c6: return !W && S(1, 0, 0xC6, 0, RVM, 4, 4, true);
        case 0x1c6: return W && S(1, 1, 0xC6, 0, RVM, 8, 8, true);
        // moves
        // aligned forms (movaps/movapd/movdqa32/64) keep the alignment check: VEX movaps / movapd / movdqa
        case 0x010: case 0x028: return !W && S(1, 0, (in.opc == 0x28 && in.mod != 3) ? 0x28 : 0x10, 0, RM, 4, 0);
        case 0x110: case 0x128: return W && S(1, 1, (in.opc == 0x28 && in.mod != 3) ? 0x28 : 0x10, 0, RM, 8, 0);
        case 0x011: case 0x029: return !W && S(1, 0, 0x11, 0, ST, 4, 0);
        case 0x111: case 0x129: return W && S(1, 1, 0x11, 0, ST, 8, 0);
        case 0x16f: case 0x26f: return S(1, (in.pp == 1 && in.mod != 3) ? 1 : 2, 0x6F, 0, RM, W ? 8 : 4, 0);
        case 0x17f: case 0x27f: return S(1, 2, 0x7F, 0, ST, W ? 8 : 4, 0);
        case 0x36f: return (!in.aaa || in.mod == 3) && S(1, 2, 0x6F, 0, RM, W ? 2 : 1, 0);   // masked memory: gen_movdqu8m
        case 0x37f: return (!in.aaa || in.mod == 3) && S(1, 2, 0x7F, 0, ST, W ? 2 : 1, 0);
        case 0x212: case 0x216: return !W && S(1, 2, in.opc, 0, RM, 4, 0);  // movsldup/movshdup
        case 0x312: return W && (in.mod == 3 || !in.aaa) && S(1, 3, 0x12, 0, RM, 8, 0);    // movddup (reg; memory unmasked: VEX reads the same m64/m256 bytes; 2026-11-14)
        // integer
        case 0x1fe: case 0x1fa: case 0x1db: case 0x1df: case 0x1eb: case 0x1ef: case 0x162: case 0x16a:
            if (in.opc == 0xfe || in.opc == 0xfa || in.opc == 0x62 || in.opc == 0x6a) { if (W) return false; }
            return S(1, 1, in.opc, 0, RVM, W ? 8 : 4, W ? 8 : 4);
        case 0x1d4: case 0x1fb: case 0x1f4: case 0x16c: case 0x16d: return W && S(1, 1, in.opc, 0, RVM, 8, 8);
        case 0x1fc: case 0x1f8: case 0x1d8: case 0x1dc: case 0x1e0: case 0x1e8: case 0x1ec: case 0x1da: case 0x1de: case 0x160: case 0x168: case 0x163: case 0x167:
            return S(1, 1, in.opc, 0, RVM, 1, 0);
        case 0x1fd: case 0x1f9: case 0x1d9: case 0x1dd: case 0x1e3: case 0x1e9: case 0x1ed: case 0x1ea: case 0x1ee: case 0x1d5: case 0x1e4: case 0x1e5: case 0x161: case 0x169: case 0x16b: case 0x1f5:
            return S(1, 1, in.opc, 0, RVM, in.opc == 0x6b || in.opc == 0xf5 ? 4 : 2, in.opc == 0x6b ? 4 : 0);
        case 0x170: return !W && S(1, 1, 0x70, 0, RM, 4, 4, true);   // pshufd
        case 0x270: return S(1, 2, 0x70, 0, RM, 2, 0, true);          // pshufhw
        case 0x370: return S(1, 3, 0x70, 0, RM, 2, 0, true);          // pshuflw
        // shift by immediate (dst in vvvv)
        case 0x171: if (in.reg3 == 2 || in.reg3 == 4 || in.reg3 == 6) return S(1, 1, 0x71, 0, VMI, 2, 0, true); return false;
        case 0x172: if (!W && (in.reg3 == 2 || in.reg3 == 4 || in.reg3 == 6)) return S(1, 1, 0x72, 0, VMI, 4, 4, true);
                    if (in.reg3 == 0 || in.reg3 == 1) return S(1, 1, 0x72, 0, VMI, W ? 8 : 4, W ? 8 : 4, true);   // vpror/vprol d/q: two shifts + or
                    return false;
        case 0x173: if (W && (in.reg3 == 2 || in.reg3 == 6)) return S(1, 1, 0x73, 0, VMI, 8, 8, true);
                    if (in.reg3 == 3 || in.reg3 == 7) return !in.aaa && S(1, 1, 0x73, 0, VMI, 1, 0, true); return false;
        // shift by xmm count
        case 0x1d1: case 0x1e1: case 0x1f1: return S(1, 1, in.opc, 0, SHC, 2, 0);
        case 0x1d2: case 0x1f2: return !W && S(1, 1, in.opc, 0, SHC, 4, 0);
        case 0x1e2: return !W && S(1, 1, 0xE2, 0, SHC, 4, 0);
        case 0x1d3: case 0x1f3: return W && S(1, 1, in.opc, 0, SHC, 8, 0);
        // scalar
        case 0x258: case 0x259: case 0x25c: case 0x25d: case 0x25e: case 0x25f: case 0x251: return !W && S(1, 2, in.opc, 0, SCAL, 4, 0);
        case 0x358: case 0x359: case 0x35c: case 0x35d: case 0x35e: case 0x35f: case 0x351: return W && S(1, 3, in.opc, 0, SCAL, 8, 0);
        case 0x25a: return !W && S(1, 2, 0x5A, 0, SCAL, 8, 0);   // cvtss2sd
        case 0x35a: return W && S(1, 3, 0x5A, 0, SCAL, 4, 0);    // cvtsd2ss
        case 0x05b: return !W && S(1, 0, 0x5B, 0, RM, 4, 4);     // cvtdq2ps
        case 0x15b: return !W && S(1, 1, 0x5B, 0, RM, 4, 4);     // cvtps2dq
        case 0x25b: return !W && S(1, 2, 0x5B, 0, RM, 4, 4);     // cvttps2dq
        case 0x1e6: case 0x3e6: return W && !in.aaa && in.L <= 1 && S(1, in.pp, 0xE6, 0, RM, 4, 8);   // vcvttpd2dq / vcvtpd2dq (<= 256-bit source)
        }
        return false;
    }
    if (in.map == 2 && in.pp == 1) {
        int o = in.opc;
        if ((o >= 0x96 && o <= 0xbf) && (o & 0xf) >= 6) {
            int kind = o & 15;
            bool scalar = kind == 9 || kind == 0xb || kind == 0xd || kind == 0xf;
            return S(2, 1, o, W, scalar ? SCALD : RVMD, W ? 8 : 4, scalar ? 0 : (W ? 8 : 4));
        }
        switch (o) {
        case 0x00: case 0x04: case 0x0b: case 0x38: case 0x3c: case 0x1c: return o == 0x1c ? S(2, 1, o, 0, RM, 1, 0) : S(2, 1, o, 0, RVM, o == 0x04 || o == 0x0b ? 2 : 1, 0);
        case 0x3a: case 0x3e: case 0x1d: return o == 0x1d ? S(2, 1, o, 0, RM, 2, 0) : S(2, 1, o, 0, RVM, 2, 0);
        case 0x1e: return !W && S(2, 1, o, 0, RM, 4, 4);
        case 0x39: case 0x3b: case 0x3d: case 0x3f: case 0x40: return !W && S(2, 1, o, 0, RVM, 4, 4);
        case 0x28: return W && S(2, 1, o, 0, RVM, 8, 8);
        case 0x2b: return !W && S(2, 1, o, 0, RVM, 2, 4);
        case 0x45: case 0x47: return S(2, 1, o, W, RVM, W ? 8 : 4, W ? 8 : 4);
        case 0x46: return !W && S(2, 1, o, 0, RVM, 4, 4);
        case 0x36: case 0x16: if (W) return false; S(2, 1, o, 0, RVM, 4, 4); v.inlane = false; return true;
        case 0x18: return !W && S(2, 1, 0x18, 0, BC, 4, 0);          // vbroadcastss
        case 0x19: return W && S(2, 1, 0x19, 0, BC, 8, 0);           // vbroadcastsd (VEX W0)
        case 0x58: return !W && S(2, 1, 0x58, 0, BC, 4, 0);          // vpbroadcastd
        case 0x59: return W && S(2, 1, 0x59, 0, BC, 8, 0);           // vpbroadcastq (VEX W0)
        case 0x78: return !W && S(2, 1, 0x78, 0, BC, 1, 0);
        case 0x79: return !W && S(2, 1, 0x79, 0, BC, 2, 0);
        case 0x1a: case 0x5a: return in.mod != 3 && S(2, 1, o, 0, BC, W ? 8 : 4, 0); // 128-bit tile
        case 0x1b: case 0x5b: return in.mod != 3 && S(2, 1, o, 0, BC, W ? 8 : 4, 0); // 256-bit tile (zmm only)
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1) {
        switch (in.opc) {
        case 0x00: case 0x01: return W && S(3, 1, in.opc, 1, RM, 8, 8, true);       // vpermq/vpermpd imm (per 256-bit half)
        case 0x04: return !W && S(3, 1, 0x04, 0, RM, 4, 4, true);
        case 0x05: return W && S(3, 1, 0x05, 0, RM, 8, 8, true);
        case 0x0f: return S(3, 1, 0x0F, 0, RVM, 1, 0, true);
        case 0x08: return !W && (in.imm8 & 0xf0) == 0 && S(3, 1, 0x08, 0, RM, 4, 4, true);
        case 0x09: return W && (in.imm8 & 0xf0) == 0 && S(3, 1, 0x09, 0, RM, 8, 8, true);
        case 0x0a: return !W && (in.imm8 & 0xf0) == 0 && S(3, 1, 0x0A, 0, SCAL, 4, 0, true);
        case 0x0b: return W && (in.imm8 & 0xf0) == 0 && S(3, 1, 0x0B, 0, SCAL, 8, 0, true);
        case 0x21: return !W && !in.aaa && !in.b && S(3, 1, 0x21, 0, RVM, 4, 0, true);   // vinsertps (xmm16-31)
        }
    }
    return false;
}

static bool gen_vexable(G& g, const VOp& v) {
    const Insn& in = g.in;
    Asm& a = g.a;
    int vl = g.vl;
    if (in.b && in.mod == 3) return false;       // embedded rounding / sae: interpreter
    if (in.b && !v.bs && v.form != SCAL && v.form != SCALD) return false;
    if (!v.inlane && vl == 64) return false;
    bool mem = in.mod != 3;
    int nch = vl == 64 ? 2 : 1, L = vl >= 32 ? 1 : 0;
    int dst = reg_of(in), src1 = in.vvvv, rm = mem ? -1 : rm_reg_of(in);
    int imm = v.imm ? in.imm8 : -1;
    int W = v.vW;
    Mem poolm; bool usepool = false;
    auto rmchunk = [&](int c, int bc) -> Opd {
        if (usepool) return M_(poolm);
        if (bc >= 0) return R_(bc);
        if (mem) return M_(g.omem(32 * c));
        return g.chunk(rm, c);
    };
    // masked memory sources need fault suppression: only 4/8-byte elements (vmaskmov)
    auto masked_load = [&](int c, int es) -> Opd {
        int m = g.maskvec(es, c, L, false), s = g.scratch();
        a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, s, m, 0, &g.tmp(g.omem(32 * c)));  // vmaskmovps/pd
        g.release(m);
        return R_(s);
    };
    int bcst = -1;
    if (in.b && mem && (v.form == RVM || v.form == RVMD || v.form == RM || v.form == VMI) && g.cpool_bcast(v.bs, poolm)) usepool = true;
    else if (in.b && mem && in.aaa) bcst = g.bcast_masked(v.bs, L, vl / v.bs);       // fault suppression
    else if (in.b && mem) {
        bcst = g.scratch();
        if (v.bs == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));     // vbroadcastss
        else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));       // vbroadcastsd ymm
        else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));              // vmovddup xmm
    }
    switch (v.form) {
    case RVM: case RVMD: case RM: {
        if (mem && in.aaa && bcst < 0 && !usepool && v.es < 4 && v.elemwise) return false;
        for (int c = 0; c < nch; c++) {
            uint32_t before = g.vtaken;
            Opd s2 = (mem && in.aaa && bcst < 0 && !usepool && v.elemwise) ? masked_load(c, v.es) : rmchunk(c, bcst);
            if (!mem && !s2.reg && v.vmap == 3 && v.vopc == 0x21) { int t = g.scratch(); g.load(t, s2.m, 0); s2 = R_(t); } // vinsertps: the memory form reads element 0, not count_s
            int s1 = v.form == RM ? 0 : g.in_reg(g.chunk(src1, c), L);
            bool direct = g.hreg(dst) >= 0 && c == 0 && !in.aaa;
            int res = direct ? g.hreg(dst) : g.scratch();
            if (v.form == RVMD && !direct) { Opd old = g.chunk(dst, c); if (old.reg) g.movrr(res, old.r, L); else g.load(res, old.m, L); }
            g.vx(v.vmap, v.vpp, W, L, v.vopc, res, s1, s2, imm);
            g.apply_mask(res, dst, c, v.es, L);
            g.put(dst, c, res, L);
            g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0); // free this chunk's temporaries
        }
        g.zero_tail(dst, vl);
        return true;
    }
    case VMI: { // shift by imm: dst = vvvv, src = rm
        if (mem && in.aaa && bcst < 0 && !usepool) return false;
        int d = in.vvvv;
        for (int c = 0; c < nch; c++) {
            uint32_t before = g.vtaken;
            Opd s2 = rmchunk(c, bcst);
            if (!s2.reg) { int t = g.scratch(); g.load(t, s2.m, L); s2 = R_(t); }   // VEX shift-by-imm takes registers only
            int res = (g.hreg(d) >= 0 && c == 0 && !in.aaa) ? g.hreg(d) : g.scratch();
            if (v.vopc == 0x72 && in.reg3 < 2) { // rotate: (x << l) | (x >> (bits - l)), l = left count
                int bits = v.es * 8, n = imm & (bits - 1), l = in.reg3 == 1 ? n : (bits - n) & (bits - 1);
                int op = v.es == 8 ? 0x73 : 0x72;
                if (l == 0) g.movrr(res, s2.r, L);
                else {
                    int t = g.scratch();
                    g.vx(1, 1, 0, L, op, 6, t, s2, l);           // vpsll{d,q} t = x << l
                    g.vx(1, 1, 0, L, op, 2, res, s2, bits - l);  // vpsrl{d,q} res = x >> (bits - l)
                    a.vex(1, 1, 0, L, 0xEB, res, res, t, nullptr); // vpor
                    g.release(t);
                }
            } else
            g.vx(v.vmap, v.vpp, W, L, v.vopc, in.reg3, res, s2, imm);  // VEX: reg field = /ext, vvvv = dst
            // the VEX form writes vvvv: our call passed dst as vvvv, ext as reg
            g.apply_mask(res, d, c, v.es, L);
            g.put(d, c, res, L);
            g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
        }
        g.zero_tail(d, vl);
        return true;
    }
    case SHC: { // shift by xmm count: count operand is the low 128 bits of rm, same for all chunks
        if (in.aaa && mem) return false;
        Opd cnt = mem ? M_(g.omem()) : g.chunk(rm, 0);
        for (int c = 0; c < nch; c++) {
            int s1 = g.in_reg(g.chunk(src1, c), L);
            int res = (g.hreg(dst) >= 0 && c == 0 && !in.aaa) ? g.hreg(dst) : g.scratch();
            if (!cnt.reg && !mem) { int t = g.scratch(); g.load(t, cnt.m, 0); cnt = R_(t); }
            g.vx(v.vmap, v.vpp, W, L, v.vopc, res, s1, cnt, -1);
            g.apply_mask(res, dst, c, v.es, L);
            g.put(dst, c, res, L);
            if (!g.chunk(src1, c).reg) g.release(s1);
            if (res != dst) g.release(res);
        }
        g.zero_tail(dst, vl);
        return true;
    }
    case ST: { // rm <- reg
        int src = dst;
        if (mem) {
            for (int c = 0; c < nch; c++) {
                int s = g.in_reg(g.chunk(src, c), L);
                bool algn = (in.map == 1 && in.opc == 0x29) || (in.map == 1 && in.opc == 0x7f && in.pp == 1);
                if (!in.aaa) a.vex(1, 0, 0, L, algn ? 0x29 : 0x11, s, 0, 0, &g.tmp(g.omem(32 * c)));  // vmovaps (aligned forms) / vmovups
                else if (v.es >= 4) {
                    int m = g.maskvec(v.es, c, L, false);
                    a.vex(2, 1, 0, L, v.es == 8 ? 0x2F : 0x2E, s, m, 0, &g.tmp(g.omem(32 * c)));  // vmaskmovps/pd store
                    g.release(m);
                } else return false;
                if (!g.chunk(src, c).reg) g.release(s);
            }
            return true;
        }
        for (int c = 0; c < nch; c++) {
            int s = g.in_reg(g.chunk(src, c), L);
            int res = (g.hreg(rm) >= 0 && c == 0 && !in.aaa) ? g.hreg(rm) : g.scratch();
            g.movrr(res, s, L);
            g.apply_mask(res, rm, c, v.es, L);
            g.put(rm, c, res, L);
            if (res != rm) g.release(res);
            if (!g.chunk(src, c).reg) g.release(s);
        }
        g.zero_tail(rm, vl);
        return true;
    }
    case SCAL: case SCALD: {
        if (in.b && !mem) return false;
        // VEX scalar op computes [op, src1 upper]; masking acts on element 0 only
        int s1 = g.in_reg(g.chunk(src1, 0), 0);
        if (mem && in.aaa) return false; // fault suppression on scalar memory: interpreter
        Opd s2 = mem ? M_(g.omem()) : g.chunk(rm, 0);
        if (!s2.reg && !mem) { int t = g.scratch(); g.load(t, s2.m, 0); s2 = R_(t); }
        int res;
        if (v.form == SCALD) {
            res = (dst < 16 && !in.aaa) ? dst : g.scratch();
            if (res != dst) { Opd old = g.chunk(dst, 0); if (old.reg) g.movrr(res, old.r, 0); else g.load(res, old.m, 0); }
        } else res = (dst < 16 && !in.aaa) ? dst : g.scratch();
        g.vx(v.vmap, v.vpp, W, 0, v.vopc, res, v.form == SCALD ? s1 : s1, s2, imm);
        Opd other0 = in.z ? M_(g.tls(TOFF(c_zero))) : g.chunk(dst, 0);
        if (in.aaa && g.scalar_blend_cached(res, other0, v.es)) {}
        else if (in.aaa) {
            // element 0: mask bit ? res : (z ? 0 : old dst); elements 1..3 stay from res
            int m = g.scratch();
            g.kmovd(m, in.aaa);                     // vmovd k -> xmm
            if (v.es == 8) { a.vex(1, 1, 0, 0, 0x73, 6, m, m, nullptr, 63); a.vex(1, 1, 0, 0, 0xEF, m, m, 0, &g.tmp(g.tls(TOFF(c_sign0q)))); }
            else { a.vex(1, 1, 0, 0, 0x72, 6, m, m, nullptr, 31); a.vex(1, 1, 0, 0, 0xEF, m, m, 0, &g.tmp(g.tls(TOFF(c_sign0)))); }
            Opd other = in.z ? M_(g.tls(TOFF(c_zero))) : g.chunk(dst, 0);
            g.vx(3, 1, 0, 0, v.es == 8 ? 0x4B : 0x4A, res, res, other, m << 4);           // vblendvpd/ps res = m ? other : res
            g.release(m);
        }
        g.put(dst, 0, res, 0);
        g.zero_tail(dst, 16);
        return true;
    }
    case BC: {
        // broadcast source: element / tile from xmm or memory; chunks identical
        int tile = (in.opc == 0x1a || in.opc == 0x5a) ? 16 : (in.opc == 0x1b || in.opc == 0x5b) ? 32 : 0;
        if (tile == 32 && vl != 64) return false;
        if (mem && in.aaa) { if (tile) return false; if (v.es < 4) return false; }
        int s = g.scratch();
        if (tile == 32) g.load(s, g.omem(), 1);
        else if (tile == 16) { if (L) a.vex(2, 1, 0, 1, 0x1A, s, 0, 0, &g.tmp(g.omem())); else g.load(s, g.omem(), 0); }
        else {
            Opd src = mem ? M_(g.omem()) : g.chunk(rm, 0);
            if (!src.reg) { if (!mem) { g.load(s, src.m, 0); src = R_(s); } }
            if (in.opc == 0x19 && L == 0) return false;
            g.vx(2, 1, 0, L, v.vopc, s, 0, src, -1);
        }
        for (int c = 0; c < nch; c++) {
            int res = (g.hreg(dst) >= 0 && c == 0 && !in.aaa) ? g.hreg(dst) : g.scratch();
            g.movrr(res, s, L);
            g.apply_mask(res, dst, c, v.es, L);
            g.put(dst, c, res, L);
            if (res != dst) g.release(res);
        }
        g.zero_tail(dst, vl);
        return true;
    }
    default: return false;
    }
}

// ---- vmovss / vmovsd (all forms)
static bool gen_movs(G& g, int es) {
    const Insn& in = g.in; Asm& a = g.a;
    bool mem = in.mod != 3;
    int pp = es == 8 ? 3 : 2;
    bool store = in.opc == 0x11;
    if (store && mem) {
        int s = g.in_reg(g.chunk(reg_of(in), 0), 0);
        if (!in.aaa) { a.vex(1, pp, 0, 0, 0x11, s, 0, 0, &g.tmp(g.omem())); return true; }
        int m = g.scratch(); // vmaskmovps/pd with element 0 only
        g.kmovd(m, in.aaa);
        if (es == 8) a.vex(1, 1, 0, 0, 0x73, 6, m, m, nullptr, 63); else a.vex(1, 1, 0, 0, 0x72, 6, m, m, nullptr, 31);
        a.vex(1, 1, 0, 0, 0xDB, m, m, 0, &g.tmp(g.tls(es == 8 ? TOFF(c_sign0q) : TOFF(c_sign0))));
        a.vex(2, 1, 0, 0, es == 8 ? 0x2F : 0x2E, s, m, 0, &g.tmp(g.omem()));
        return true;
    }
    int dst = store ? rm_reg_of(in) : reg_of(in);
    int src = store ? reg_of(in) : (mem ? -1 : rm_reg_of(in));
    int res = g.scratch();
    if (mem) {
        if (in.aaa) { // load element 0 under mask (fault suppression): vmaskmovps
            int m = g.scratch();
            g.kmovd(m, in.aaa);
            if (es == 8) a.vex(1, 1, 0, 0, 0x73, 6, m, m, nullptr, 63); else a.vex(1, 1, 0, 0, 0x72, 6, m, m, nullptr, 31);
            a.vex(1, 1, 0, 0, 0xDB, m, m, 0, &g.tmp(g.tls(es == 8 ? TOFF(c_sign0q) : TOFF(c_sign0))));
            a.vex(2, 1, 0, 0, es == 8 ? 0x2D : 0x2C, res, m, 0, &g.tmp(g.omem()));        // [mem0 or 0, 0, 0, 0]
            if (!in.z) { // element 0 from the old dst when the mask bit is clear
                int t = g.scratch();
                Opd old = g.chunk(dst, 0);
                if (old.reg) g.movrr(t, old.r, 0); else g.load(t, old.m, 0);
                a.vex(3, 1, 0, 0, es == 8 ? 0x0D : 0x0C, t, t, 0, &g.tmp(g.tls(TOFF(c_zero))), es == 8 ? 0x2 : 0xE); // [old0, 0...]
                a.vex(3, 1, 0, 0, es == 8 ? 0x4B : 0x4A, res, t, res, nullptr, m << 4);       // res = m ? res : t
                g.release(t);
            }
            g.release(m);
        } else a.vex(1, pp, 0, 0, 0x10, res, 0, 0, &g.tmp(g.omem()));
    } else {
        int s1 = g.in_reg(g.chunk(in.vvvv, 0), 0);
        int s2 = g.in_reg(g.chunk(src, 0), 0);
        a.vex(1, pp, 0, 0, 0x10, res, s1, s2, nullptr);  // [src0, vvvv upper]
        Opd other0 = in.z ? M_(g.tls(TOFF(c_zero))) : g.chunk(dst, 0);
        if (in.aaa && g.scalar_blend_cached(res, other0, es)) {}
        else if (in.aaa) {
            int m = g.scratch();
            g.kmovd(m, in.aaa);
            if (es == 8) { a.vex(1, 1, 0, 0, 0x73, 6, m, m, nullptr, 63); a.vex(1, 1, 0, 0, 0xEF, m, m, 0, &g.tmp(g.tls(TOFF(c_sign0q)))); }
            else { a.vex(1, 1, 0, 0, 0x72, 6, m, m, nullptr, 31); a.vex(1, 1, 0, 0, 0xEF, m, m, 0, &g.tmp(g.tls(TOFF(c_sign0)))); }
            Opd other = in.z ? M_(g.tls(TOFF(c_zero))) : g.chunk(dst, 0);
            g.vx(3, 1, 0, 0, es == 8 ? 0x4B : 0x4A, res, res, other, m << 4);
            g.release(m);
        }
    }
    g.put(dst, 0, res, 0);
    g.zero_tail(dst, 16);
    return true;
}

// ---- compares into an opmask
static bool gen_cmpk(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    bool mem = in.mod != 3;
    int kd = reg_of(in) & 7;
    int vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int kind; // 0 ps 1 pd 2 ss 3 sd 4 int
    int es, vmap = 1, vpp = 1, vopc, imm = -1, umm = 0; bool inv = false, swap = false, uns = false;
    if (in.map == 1 && in.opc == 0xC2) {
        if (in.b && !mem) return false;
        kind = in.pp; es = (kind == 1 || kind == 3) ? 8 : 4;
        if ((kind == 0 || kind == 2) && in.W) return false;
        if ((kind == 1 || kind == 3) && !in.W) return false;
        vmap = 1; vpp = in.pp; vopc = 0xC2; imm = in.imm8 & 31;
    } else if (in.map == 1 && (in.opc == 0x76 || in.opc == 0x66)) { if (in.W) return false; kind = 4; es = 4; vopc = in.opc; }
    else if (in.map == 2 && in.pp == 1 && (in.opc == 0x29 || in.opc == 0x37)) { if (!in.W) return false; kind = 4; es = 8; vmap = 2; vopc = in.opc; }
    else if (in.map == 1 && (in.opc == 0x74 || in.opc == 0x64)) { kind = 4; es = 1; vopc = in.opc; }
    else if (in.map == 1 && (in.opc == 0x75 || in.opc == 0x65)) { kind = 4; es = 2; vopc = in.opc; }
    else if (in.map == 3 && in.pp == 1 && (in.opc == 0x3e || in.opc == 0x3f)) {
        // vpcmp{u}b (W0) / vpcmp{u}w (W1); unsigned le/lt via vpminu/vpmaxu + vpcmpeq
        kind = 4; es = in.W ? 2 : 1; bool u = in.opc == 0x3e;
        int eq = es == 1 ? 0x74 : 0x75, gt = es == 1 ? 0x64 : 0x65, p = in.imm8 & 7;
        if (!u) switch (p) {
            case 0: vopc = eq; break; case 4: vopc = eq; inv = true; break;
            case 6: vopc = gt; break; case 2: vopc = gt; inv = true; break;
            case 1: vopc = gt; swap = true; break; case 5: vopc = gt; swap = true; inv = true; break;
            default: return false; }
        else switch (p) {
            case 0: vopc = eq; break; case 4: vopc = eq; inv = true; break;
            case 2: umm = 1; break; case 6: umm = 1; inv = true; break;        // le: min(a,b) == a
            case 5: umm = 2; break; case 1: umm = 2; inv = true; break;        // nlt: max(a,b) == a
            default: return false; }
        vopc = umm ? eq : vopc;
    }
    else if (in.map == 3 && in.pp == 1 && (in.opc == 0x1e || in.opc == 0x1f)) {
        kind = 4; es = in.W ? 8 : 4; uns = in.opc == 0x1e;
        int p = in.imm8 & 7;
        switch (p) {
        case 0: vopc = 0x76; break;                        // eq
        case 4: vopc = 0x76; inv = true; break;            // ne
        case 6: vopc = 0x66; break;                        // nle: a > b
        case 2: vopc = 0x66; inv = true; break;            // le: !(a > b)
        case 1: vopc = 0x66; swap = true; break;           // lt: b > a
        case 5: vopc = 0x66; swap = true; inv = true; break; // nlt: !(b > a)
        default: return false;
        }
        if (es == 8) { vmap = 2; vopc = vopc == 0x76 ? 0x29 : 0x37; }
    } else if (in.map == 2 && (in.pp == 1 || in.pp == 2) && (in.opc == 0x26 || in.opc == 0x27)) {
        kind = 5; es = in.opc == 0x27 ? (in.W ? 8 : 4) : (in.W ? 2 : 1);
        inv = in.pp == 1; // testm: nonzero
        vopc = 0;
    } else return false;
    if ((kind == 4 || kind == 5) && in.b && !mem) return false;
    if (es == 2 && nch == 2) return false;   // 32 word lanes: interpreter
    bool scalar = kind == 2 || kind == 3;
    if (scalar) { nch = 1; L = 0; }
    // Masked compares suppress faults on masked-off elements like other loads: 4/8-byte elements are read with
    // vmaskmov under the mask (masked-off lanes read as 0; the result is masked anyway), 1/2-byte: interpreter.
    bool mload = false;
    if (mem && in.aaa && !in.b && !scalar) { if (es < 4) return false; mload = true; }
    if (mem && scalar && in.aaa) return false;
    int bcst = -1;
    Mem cpm; bool cpu = in.b && mem && !scalar && g.cpool_bcast(es, cpm);
    if (in.b && mem && !cpu && in.aaa) bcst = g.bcast_masked(es, L, nch * (16 << L) / es);  // fault suppression
    else if (in.b && mem && !cpu) {
        bcst = g.scratch();
        if (es == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));
        else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));
        else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));
    }
    int gpr[2] = {-1, -1}; bool seed = false;
    for (int c = 0; c < nch; c++) {
        Opd s2;
        if (cpu) s2 = M_(cpm);
        else if (bcst >= 0) s2 = R_(bcst);
        else if (mem && mload) {
            int m = g.maskvec(es, c, L, false), t = g.scratch();
            a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, t, m, 0, &g.tmp(g.omem(32 * c)));   // vmaskmovpd/ps
            g.release(m); s2 = R_(t);
        }
        else if (mem) s2 = M_(g.omem(32 * c));
        else s2 = g.chunk(rm_reg_of(in), c);
        int res = g.scratch();
        if (scalar && mem) { int t = g.scratch(); a.vex(1, es == 8 ? 3 : 2, 0, 0, 0x10, t, 0, 0, &g.tmp(s2.m)); s2 = R_(t); }
        int s1 = g.in_reg(g.chunk(in.vvvv, c), L);
        if (uns) { // flip sign bits of both operands
            int t2 = g.scratch();
            Mem sg = g.tls(es == 8 ? TOFF(c_sign64) : TOFF(c_sign32));
            if (s2.reg) g.movrr(t2, s2.r, L); else g.load(t2, s2.m, L);
            a.vex(1, 1, 0, L, 0xEF, t2, t2, 0, &sg);
            a.vex(1, 1, 0, L, 0xEF, res, s1, 0, &sg);
            if (swap) g.vx(vmap, vpp, 0, L, vopc, res, t2, R_(res));
            else g.vx(vmap, vpp, 0, L, vopc, res, res, R_(t2));
            g.release(t2);
        } else if (umm) {
            int mm = es == 1 ? (umm == 1 ? 0xDA : 0xDE) : (umm == 1 ? 0x3A : 0x3E);   // vpminub/vpmaxub, vpminuw/vpmaxuw
            g.vx(es == 1 ? 1 : 2, 1, 0, L, mm, res, s1, s2);
            g.vx(1, 1, 0, L, vopc, res, res, R_(s1));
        } else if (swap) {
            int t2 = s2.reg ? s2.r : g.scratch();
            if (!s2.reg) g.load(t2, s2.m, L);
            g.vx(vmap, vpp, 0, L, vopc, res, t2, R_(s1));
            if (!s2.reg) g.release(t2);
        } else if (kind == 5) {
            g.vx(1, 1, 0, L, 0xDB, res, s1, s2);
            Mem z = g.tls(TOFF(c_zero));
            if (es == 8) a.vex(2, 1, 0, L, 0x29, res, res, 0, &z);
            else a.vex(1, 1, 0, L, es == 4 ? 0x76 : es == 2 ? 0x75 : 0x74, res, res, 0, &z);
        } else g.vx(vmap, vpp, 0, L, vopc, res, s1, s2, imm);
        if (inv) a.vex(1, 1, 0, L, 0xEF, res, res, 0, &g.tmp(g.tls(TOFF(c_ones))));
        if (in.aaa) { int m = g.maskvec(es == 1 ? 1 : es, c, L, false); a.vex(1, 1, 0, L, 0xDB, res, res, m, nullptr); g.release(m); }
        if (scalar) a.vex(1, 1, 0, 0, 0xDB, res, res, 0, &g.tmp(g.tls(es == 8 ? TOFF(c_lane0q) : TOFF(c_lane0))));   // = maskvec of the new k
        int gr = g.gscratch(); if (gr < 0) return false;
        if (es == 2) {   // words -> bytes (vpacksswb with zero), lanes together (vpermq), then vpmovmskb
            a.vex(1, 1, 0, L, 0x63, res, res, 0, &g.tmp(g.tls(TOFF(c_zero))));
            if (L) a.vex(3, 1, 1, 1, 0x00, res, 0, res, nullptr, 0x08);
            a.vex(1, 1, 0, 0, 0xD7, gr, 0, res, nullptr);
        } else if (es == 1) a.vex(1, 1, 0, L, 0xD7, gr, 0, res, nullptr);            // vpmovmskb
        else a.vex(1, es == 8 ? 1 : 0, 0, L, 0x50, gr, 0, res, nullptr);     // vmovmskps/pd
        gpr[c] = gr;
        if (nch == 1 && (!scalar || (g.o.mc_seed & 4)) && (es == 4 || es == 8) && (g.o.mc_seed & 2) && g.o.mc_reg >= 0 && !g.o.mc_busy && res != g.o.mc_reg) {
            g.movrr(g.o.mc_reg, res, L); seed = true;   // the compare result is the new k's lane mask
        }
        g.release(res);
        if (!g.chunk(in.vvvv, c).reg) g.release(s1);
        if (s2.reg && s2.r != bcst && !(g.chunk(rm_reg_of(in), c).reg && !mem)) g.release(s2.r);
    }
    Mem k = g.kreg(kd);
    if (nch == 1) {
        if (es == 1 && L == 1) a.mov_store32(k, gpr[0]), a.mov_store_imm32(g.kreg(kd, 4), 0);
        else { a.mov_store64(k, gpr[0]); g.kc_note(kd, gpr[0]); }  // movmsk zero-extends
        if (seed) { g.o.mc_key = mask_key_(kd, es, 0, L, false); g.o.mc_wrote_k = true; }
    } else {
        if (es == 1) { a.mov_store32(k, gpr[0]); a.mov_store32(g.kreg(kd, 4), gpr[1]); }
        else if (es == 8) { // lo4 | hi4 << 4
            Mem m1; m1.base = -1; m1.index = gpr[1]; m1.scale = 3; m1.disp = 0; a.lea(gpr[1], m1);   // *8
            Mem m2; m2.base = gpr[0]; m2.index = gpr[1]; m2.scale = 1; m2.disp = 0; a.lea(gpr[0], m2); // + *16
            a.mov_store64(k, gpr[0]);
        } else { // lo8 | hi8 << 8
            a.mov_store_imm64(k, 0);
            a.mov_store8(k, gpr[0]); a.mov_store8(g.kreg(kd, 1), gpr[1]);
        }
    }
    return true;
}

// ---- opmask instructions (VEX k*)
static int kw_logic(const Insn& in) { if (in.pp == 0) return in.W ? 64 : 16; if (in.pp == 1) return in.W ? 32 : 8; return 0; }
static int kidx(int w) { return w == 8 ? 0 : w == 16 ? 1 : w == 32 ? 2 : 3; }

static void kload(G& g, int gr, const Mem& m, int w) {
    if (w == 8) g.a.movzx8(gr, m); else if (w == 16) g.a.movzx16(gr, m); else if (w == 32) g.a.mov_load32(gr, m); else g.a.mov_load64(gr, m);
}

// k[kk] (width w, zero-extended) into gr: from the cached GPR when it holds k[kk]
static void kget(G& g, int gr, int kk, int w) {
    int s = g.kc(kk);
    if (s < 0) { kload(g, gr, g.kreg(kk), w); return; }
    Asm& a = g.a;
    if (w == 8) a.movzx_rr8(gr, s); else if (w == 16) a.leg(0, 0, 1, 0xB7, gr, s, nullptr); else if (w == 32) a.mov_rr32(gr, s); else if (gr != s) a.mov_rr64(gr, s);
}

static bool gen_kreg(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    int kr = reg_of(in) & 7, kv = in.vvvv & 7, km = in.rm3;
    bool mem = in.mod != 3;
    if (in.map == 1) {
        switch (in.opc) {
        case 0x90: { int w = kw_logic(in); if (!w) return false;
            int gr = g.gscratch(); if (gr < 0) return false;
            if (mem) kload(g, gr, g.omem(), w); else kget(g, gr, km, w);
            a.mov_store64(g.kreg(kr), gr); g.kc_note(kr, gr); return true; }
        case 0x91: { int w = kw_logic(in); if (!w || !mem) return false;
            int gr = g.kc(kr);
            if (gr < 0) { gr = g.gscratch(); if (gr < 0) return false; a.mov_load64(gr, g.kreg(kr)); }
            Mem m = g.omem();
            if (w == 8) a.mov_store8(m, gr); else if (w == 16) a.mov_store16(m, gr); else if (w == 32) a.mov_store32(m, gr); else a.mov_store64(m, gr);
            return true; }
        case 0x92: { // k <- gpr
            if (mem) return false;
            int w; if (in.pp == 0 && !in.W) w = 16; else if (in.pp == 1 && !in.W) w = 8; else if (in.pp == 3) w = in.W ? 64 : 32; else return false;
            int src = rm_reg_of(in) & 15;
            int gr = g.gscratch(); if (gr < 0) return false;
            if (w == 8) a.movzx_rr8(gr, src); else if (w == 16) a.leg(0, 0, 1, 0xB7, gr, src, nullptr); else if (w == 32) a.mov_rr32(gr, src); else a.mov_rr64(gr, src);
            a.mov_store64(g.kreg(kr), gr); g.kc_note(kr, gr); return true; }
        case 0x93: { // gpr <- k
            if (mem) return false;
            int w; if (in.pp == 0 && !in.W) w = 16; else if (in.pp == 1 && !in.W) w = 8; else if (in.pp == 3) w = in.W ? 64 : 32; else return false;
            int dst = reg_of(in) & 15;
            if (dst == RSP) return false;
            kget(g, dst, km, w); return true; }
        case 0x41: case 0x42: case 0x45: case 0x46: case 0x47: case 0x4a: case 0x44: case 0x4b: {
            int w = kw_logic(in);
            if (in.opc == 0x4b) { if (in.pp == 1 && !in.W) w = 16; else if (in.pp == 0 && !in.W) w = 32; else if (in.pp == 0 && in.W) w = 64; else return false; }
            if (!w || mem) return false;
            Mem km_ = g.tls(TOFF(c_kmask) + 8 * kidx(w));
            if ((in.opc == 0x46 || in.opc == 0x47) && kv == km) { // kxnor/kxor of a register with itself: constant
                if (in.opc == 0x47 || w == 0) { a.mov_store_imm64(g.kreg(kr), 0); return true; }
                if (w == 64) a.mov_store_imm64(g.kreg(kr), -1);
                else if (w == 32) { a.mov_store_imm32(g.kreg(kr), 0xffffffffu); a.mov_store_imm32(g.kreg(kr, 4), 0); }
                else a.mov_store_imm64(g.kreg(kr), int32_t((1u << w) - 1));
                return true;
            }
            // integer path when no flags are read afterwards: no GPR<->vector crossing (the k
            // values were just stored from a GPR: vmovmskps; a vector reload stalls on forwarding)
            if (!g.o.flags_live && in.opc != 0x4b) {
                int gr = g.gscratch(); if (g.fail) return false;
                kget(g, gr, in.opc == 0x44 ? km : kv, 64);
                if (in.opc == 0x42 || in.opc == 0x44) a.not_r(1, gr);
                if (in.opc != 0x44) {
                    int op = (in.opc == 0x41 || in.opc == 0x42) ? 0x20 : in.opc == 0x45 ? 0x08 : in.opc == 0x4a ? 0x00 : 0x30;
                    if (g.kc(km) >= 0) a.alu_rr(op, 1, gr, g.kc(km)); else a.alu_rm(op, 1, gr, g.kreg(km));
                }
                if (in.opc == 0x46) a.not_r(1, gr);
                if (w == 8) a.movzx_rr8(gr, gr); else if (w == 16) a.leg(0, 0, 1, 0xB7, gr, gr, nullptr); else if (w == 32) a.mov_rr32(gr, gr);
                a.mov_store64(g.kreg(kr), gr); g.kc_note(kr, gr);
                return true;
            }
            int s = g.scratch();
            if (in.opc == 0x44) { // knot
                a.vex(1, 2, 0, 0, 0x7E, s, 0, 0, &g.tmp(g.kreg(km)));                   // vmovq
                a.vex(1, 1, 0, 0, 0xEF, s, s, 0, &g.tmp(g.tls(TOFF(c_ones))));
            } else if (in.opc == 0x4b) { // kunpck: (vvvv & h) << h | (rm & h)
                int h = w / 2; Mem hm = g.tls(TOFF(c_kmask) + 8 * kidx(h));
                int t = g.scratch();
                a.vex(1, 2, 0, 0, 0x7E, s, 0, 0, &g.tmp(g.kreg(kv)));
                a.vex(1, 1, 0, 0, 0xDB, s, s, 0, &hm);
                a.vex(1, 1, 0, 0, 0x73, 6, s, s, nullptr, h);                           // vpsllq $h
                a.vex(1, 2, 0, 0, 0x7E, t, 0, 0, &g.tmp(g.kreg(km)));
                a.vex(1, 1, 0, 0, 0xDB, t, t, 0, &hm);
                a.vex(1, 1, 0, 0, 0xEB, s, s, t, nullptr);
                g.release(t);
            } else {
                a.vex(1, 2, 0, 0, 0x7E, s, 0, 0, &g.tmp(g.kreg(kv)));
                int op = in.opc == 0x41 ? 0xDB : in.opc == 0x42 ? 0xDF : in.opc == 0x45 ? 0xEB : in.opc == 0x4a ? 0xD4 : 0xEF;
                a.vex(1, 1, 0, 0, op, s, s, 0, &g.tmp(g.kreg(km)));                    // reads 16 bytes; low 8 used
                if (in.opc == 0x46) a.vex(1, 1, 0, 0, 0xEF, s, s, 0, &g.tmp(g.tls(TOFF(c_ones))));
            }
            a.vex(1, 1, 0, 0, 0xDB, s, s, 0, &km_);
            a.vex(1, 1, 0, 0, 0xD6, s, 0, 0, &g.tmp(g.kreg(kr)));                       // vmovq store
            g.release(s);
            return true; }
        case 0x98: case 0x99: { // kortest / ktest
            int w = kw_logic(in); if (!w || mem) return false;
            int W64 = w == 64;
            if (!(g.o.flags_live & ~0x40u)) {   // only ZF read afterwards: one test
                Mem kmem = g.kreg(kr);
                if (in.opc == 0x98 && kr == km && g.kc(kr) >= 0) {   // the value is in a GPR: no reload
                    int gr = g.gscratch(); if (g.fail) return false;
                    kget(g, gr, kr, w); a.alu_rr(0x08, 1, gr, gr); return true;
                }
                if (in.opc == 0x98 && kr == km) {
                    if (w == 8) a.leg(0, 0, 0, 0xF6, 0, 0, &kmem, 1, 0xff);
                    else if (w == 16) a.leg(1, 0, 0, 0xF7, 0, 0, &kmem, 2, 0xffff);
                    else if (w == 32) a.leg(0, 0, 0, 0xF7, 0, 0, &kmem, 4, 0xffffffffu);
                    else a.leg(0, 1, 0, 0x83, 7, 0, &kmem, 1, 0);           // cmpq $0
                    return true;
                }
                int gr = g.gscratch(); if (g.fail) return false;
                kget(g, gr, kr, w);
                if (g.kc(km) >= 0) { int g2 = g.gscratch(); if (g.fail) return false; kget(g, g2, km, w);
                    if (in.opc == 0x98) a.alu_rr(0x08, 1, gr, g2); else a.leg(0, 1, 0, 0x85, g2, gr, nullptr); return true; }
                Mem m2 = g.kreg(km);
                if (in.opc == 0x98) { if (w == 64) a.alu_rm(0x08, 1, gr, m2); else { int g2 = g.gscratch(); kload(g, g2, m2, w); a.alu_rr(0x08, 0, gr, g2); } }
                else a.leg(0, W64, 0, 0x85, gr, 0, &m2);                    // test (k[rm] & k[reg])
                return true;
            }
            int ra = g.gscratch_fixed(RAX), rc = g.gscratch_fixed(RCX), rd = g.gscratch_fixed(RDX);
            if (ra < 0 || rc < 0 || rd < 0) return false;
            kload(g, RAX, g.kreg(kr), w); kload(g, RCX, g.kreg(km), w);
            if (in.opc == 0x98) {
                a.alu_rr(0x08, W64, RAX, RCX);           // or: ZF = (v == 0), OF = CF = 0
                a.setcc(4, RDX);                         // dl = ZF
                if (w == 64 || w == 32) a.alu_ri(7, W64, RAX, -1); else a.alu_ri(7, 0, RAX, int32_t((1u << w) - 1));
                a.setcc(4, RCX);                         // cl = (v == all ones)
            } else {
                a.mov_rr64(RDX, RAX);
                a.alu_rr(0x20, W64, RAX, RCX);           // a & b
                a.setcc(4, RAX);                         // al = ZF
                a.not_r(W64, RDX);
                a.alu_rr(0x20, W64, RDX, RCX);           // ~a & b
                a.setcc(4, RCX);                         // cl = CF
                a.movzx_rr8(RDX, RAX);                   // dl = ZF
            }
            a.shift_ri(4, 0, RDX, 6);                    // dl <<= 6
            a.alu_rr(0x08, 0, RCX, RDX);                 // cl |= dl (also OF = 0)
            a.leg(0, 0, 0, 0x88, RCX, 4, nullptr);       // mov %cl, %ah
            a.sahf();                                    // SF ZF AF PF CF <- ah; OF stays 0
            return true; }
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1 && in.opc >= 0x30 && in.opc <= 0x33 && !mem) {
        int w = (in.opc & 1) ? (in.W ? 64 : 32) : (in.W ? 16 : 8);
        bool left = in.opc >= 0x32;
        int n = in.imm8;
        if (n >= w) { a.mov_store_imm64(g.kreg(kr), 0); return true; }
        if (!g.o.flags_live) {   // integer path (see the k logic ops)
            int gr = g.gscratch(); if (g.fail) return false;
            kget(g, gr, km, w);
            if (n) a.shift_ri(left ? 4 : 5, 1, gr, n);
            if (w == 8) a.movzx_rr8(gr, gr); else if (w == 16) a.leg(0, 0, 1, 0xB7, gr, gr, nullptr); else if (w == 32) a.mov_rr32(gr, gr);
            a.mov_store64(g.kreg(kr), gr); g.kc_note(kr, gr);
            return true;
        }
        int s = g.scratch();
        a.vex(1, 2, 0, 0, 0x7E, s, 0, 0, &g.tmp(g.kreg(km)));
        a.vex(1, 1, 0, 0, 0xDB, s, s, 0, &g.tmp(g.tls(TOFF(c_kmask) + 8 * kidx(w))));
        if (n) a.vex(1, 1, 0, 0, 0x73, left ? 6 : 2, s, s, nullptr, n);
        a.vex(1, 1, 0, 0, 0xDB, s, s, 0, &g.tmp(g.tls(TOFF(c_kmask) + 8 * kidx(w))));
        a.vex(1, 1, 0, 0, 0xD6, s, 0, 0, &g.tmp(g.kreg(kr)));
        return true;
    }
    return false;
}


// ---- vcvttss2usi / vcvttsd2usi -> r32/r64
static bool gen_cvt2usi(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b) return false;
    bool dbl = in.pp == 3; int pp = dbl ? 3 : 2;
    int gd = reg_of(in) & 15; if (gd == RSP) return false;
    g.save_flags();
    int x;
    if (in.mod != 3) { x = g.scratch(); a.vex(1, pp, 0, 0, 0x10, x, 0, 0, &g.tmp(g.omem())); }
    else x = g.in_reg(g.chunk(rm_reg_of(in), 0), 0);
    int g2 = g.gscratch();
    a.vex(1, pp, 1, 0, 0x2C, gd, 0, x, nullptr);                     // vcvttss2si r64
    if (in.W) {
        int t = g.scratch();
        a.vex(1, pp, 0, 0, 0x5C, t, x, 0, &g.tmp(g.tls(dbl ? TOFF(c_2p63d) : TOFF(c_2p63f)))); // x - 2^63
        a.vex(1, pp, 1, 0, 0x2C, g2, 0, t, nullptr);
        a.test_rr(1, gd, gd); int j1 = a.jcc_fwd(9);                  // a >= 0: done
        a.test_rr(1, g2, g2); int j2 = a.jcc_fwd(8);                  // b < 0: invalid
        a.mov_rr64(gd, g2); a.bts_ri(1, gd, 63); int j3 = a.jmp_fwd();
        a.bind(j2); a.leg(0, 1, 0, 0xC7, 0, gd, nullptr, 4, 0xffffffffu); // -1
        a.bind(j1); a.bind(j3);
    } else {
        a.mov_rr64(g2, gd); a.shift_ri(5, 1, g2, 32); int j = a.jcc_fwd(4);
        a.mov_imm32(gd, 0xffffffffu);
        a.bind(j); a.mov_rr32(gd, gd);
    }
    return true;
}

// ---- vcvtusi2ss / vcvtusi2sd
static bool gen_cvtusi2(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    bool dbl = in.pp == 3; int pp = dbl ? 3 : 2, W = in.W;
    int dst = reg_of(in);
    if (W && !dbl) g.save_flags();
    int gs = g.gscratch();
    if (in.mod != 3) { if (W) a.mov_load64(gs, g.omem()); else a.mov_load32(gs, g.omem()); }
    else { int r = rm_reg_of(in) & 15; if (W) a.mov_rr64(gs, r); else a.mov_rr32(gs, r); }
    int s1 = g.in_reg(g.chunk(in.vvvv, 0), 0);
    int res = g.scratch();
    if (!W) a.vex(1, pp, 1, 0, 0x2A, res, s1, gs, nullptr);            // exact: value < 2^32 as int64
    else if (dbl) {                                                     // hi*2^32 + lo: one rounding
        int g2 = g.gscratch();
        a.mov_rr32(g2, gs);
        a.vex(3, 3, 1, 0, 0xF0, gs, 0, gs, nullptr, 32);                 // rorx $32
        a.mov_rr32(gs, gs);
        int t = g.scratch();
        a.vex(1, 3, 1, 0, 0x2A, t, t, gs, nullptr);
        a.vex(1, 3, 0, 0, 0x59, t, t, 0, &g.tmp(g.tls(TOFF(c_2p32d))));
        a.vex(1, 3, 1, 0, 0x2A, res, s1, g2, nullptr);
        a.vex(1, 3, 0, 0, 0x58, res, res, t, nullptr);
    } else {                                                            // u64 -> float: halve with sticky bit
        int g2 = g.gscratch();
        a.test_rr(1, gs, gs); int j = a.jcc_fwd(8);
        a.vex(1, 2, 1, 0, 0x2A, res, s1, gs, nullptr); int j2 = a.jmp_fwd();
        a.bind(j);
        a.mov_rr64(g2, gs); a.shift_ri(5, 1, gs, 1); a.alu_ri(4, 1, g2, 1); a.alu_rr(0x08, 1, gs, g2);
        a.vex(1, 2, 1, 0, 0x2A, res, s1, gs, nullptr);
        a.vex(1, 2, 0, 0, 0x58, res, res, res, nullptr);
        a.bind(j2);
    }
    g.put(dst, 0, res, 0);
    g.zero_tail(dst, 16);
    return true;
}

// ---- vpbroadcastb/w/d/q from a GPR
static bool gen_bcgpr(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    int es = in.opc == 0x7a ? 1 : in.opc == 0x7b ? 2 : (in.W ? 8 : 4);
    int dst = reg_of(in), src = rm_reg_of(in) & 15, vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int s = g.scratch();
    a.vex(1, 1, es == 8 ? 1 : 0, 0, 0x6E, s, 0, src, nullptr);        // vmovd/vmovq
    int op = es == 1 ? 0x78 : es == 2 ? 0x79 : es == 4 ? 0x58 : 0x59;
    a.vex(2, 1, 0, L, op, s, 0, s, nullptr);
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int res = (g.hreg(dst) >= 0 && c == 0 && !in.aaa) ? g.hreg(dst) : g.scratch();
        g.movrr(res, s, L);
        g.apply_mask(res, dst, c, es, L);
        g.put(dst, c, res, L);
        g.vtaken = before;
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- vpternlogd/q: f = A ? f1(B,C) : f0(B,C)  (A = dst, B = vvvv, C = rm)
static void two_input(G& g, int d, int f, int B, const Opd& C, int L) {
    Asm& a = g.a;
    auto cmpl = [&](int base) { two_input(g, d, base, B, C, L); a.vex(1, 1, 0, L, 0xEF, d, d, 0, &g.tmp(g.tls(TOFF(c_ones)))); };
    switch (f) {
    case 0x0: a.vex(1, 1, 0, L, 0xEF, d, d, d, nullptr); return;
    case 0xf: a.vex(1, 1, 0, L, 0x76, d, d, d, nullptr); return;
    case 0x8: g.vx(1, 1, 0, L, 0xDB, d, B, C); return;
    case 0xe: g.vx(1, 1, 0, L, 0xEB, d, B, C); return;
    case 0x6: g.vx(1, 1, 0, L, 0xEF, d, B, C); return;
    case 0x2: g.vx(1, 1, 0, L, 0xDF, d, B, C); return;          // ~B & C
    case 0x4: {                                                  // B & ~C
        if (C.reg) { a.vex(1, 1, 0, L, 0xDF, d, C.r, B, nullptr); return; }
        g.load(d, C.m, L); a.vex(1, 1, 0, L, 0xDF, d, d, B, nullptr); return; }
    case 0xc: g.movrr(d, B, L); return;
    case 0xa: if (C.reg) g.movrr(d, C.r, L); else g.load(d, C.m, L); return;
    case 0x7: cmpl(0x8); return;
    case 0x1: cmpl(0xe); return;
    case 0x9: cmpl(0x6); return;
    case 0x3: cmpl(0xc); return;
    case 0x5: cmpl(0xa); return;
    case 0xb: cmpl(0x4); return;
    case 0xd: cmpl(0x2); return;
    }
}

static bool gen_ternlog(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    int es = in.W ? 8 : 4, dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    bool mem = in.mod != 3;
    int imm = in.imm8, f0 = imm & 15, f1 = imm >> 4;
    int bcst = -1;
    if (in.b && mem) {
        bcst = g.scratch();
        if (es == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));
        else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));
        else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        Opd C;
        if (bcst >= 0) C = R_(bcst);
        else if (mem && in.aaa) {
            int m = g.maskvec(es, c, L, false), s = g.scratch();
            a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, s, m, 0, &g.tmp(g.omem(32 * c)));
            g.release(m); C = R_(s);
        } else if (mem) C = M_(g.omem(32 * c));
        else C = g.chunk(rm_reg_of(in), c);
        int B = g.in_reg(g.chunk(in.vvvv, c), L);
        int res = g.scratch();
        if (f0 == f1) two_input(g, res, f0, B, C, L);
        else {
            int A = g.in_reg(g.chunk(dst, c), L);
            int t1 = g.scratch();
            two_input(g, res, f0, B, C, L);
            two_input(g, t1, f1, B, C, L);
            a.vex(1, 1, 0, L, 0xEF, t1, t1, res, nullptr);
            a.vex(1, 1, 0, L, 0xDB, t1, t1, A, nullptr);
            a.vex(1, 1, 0, L, 0xEF, res, res, t1, nullptr);
        }
        g.apply_mask(res, dst, c, es, L);
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- vpermt2d/ps/q/pd, vpermi2d/ps/q/pd (128/256 bit)
static bool gen_perm2(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    int vl = g.vl; if (vl == 64) return false;
    if (in.b) return false;
    int L = vl == 32 ? 1 : 0;
    bool t2 = in.opc >= 0x7d;
    int dst = reg_of(in);
    int idx = g.in_reg(g.chunk(t2 ? in.vvvv : dst, 0), L);
    int T1 = g.in_reg(g.chunk(t2 ? dst : in.vvvv, 0), L);
    Opd T2 = in.mod != 3 ? M_(g.omem()) : g.chunk(rm_reg_of(in), 0);
    if (in.mod != 3 && in.aaa) return false; // fault suppression: interpreter
    int p1 = g.scratch(), p2 = g.scratch(), m = g.scratch();
    if (in.W) {   // vpermt2pd/q, vpermi2pd/q: 64-bit elements
        int x = g.scratch();
        a.vex(1, 1, 0, L, 0x73, 6, x, idx, nullptr, 1);               // vpsllq x = idx << 1
        if (L) {
            a.vex(1, 1, 0, 1, 0x70, x, 0, x, nullptr, 0xA0);          // vpshufd: [2i, 2i] per qword
            a.vex(1, 1, 0, 1, 0xFE, x, x, 0, &g.tmp(g.tls(TOFF(c_q01))));   // vpaddd: dword index pair [2i, 2i+1]
            a.vex(2, 1, 0, 1, 0x16, p1, x, T1, nullptr);              // vpermps p1 = T1[x]
            g.vx(2, 1, 0, 1, 0x16, p2, x, T2);
            a.vex(1, 1, 0, 1, 0x73, 6, m, idx, nullptr, 61);          // vpsllq $61: table bit (2) to sign
        } else {
            a.vex(2, 1, 0, 0, 0x0D, p1, T1, x, nullptr);              // vpermilpd p1 = T1[idx bit 0] (control bit 1)
            int t = T2.reg ? T2.r : g.scratch(); if (!T2.reg) g.load(t, T2.m, 0);
            a.vex(2, 1, 0, 0, 0x0D, p2, t, x, nullptr);
            a.vex(1, 1, 0, 0, 0x73, 6, m, idx, nullptr, 62);          // vpsllq $62: table bit (1) to sign
        }
        a.vex(3, 1, 0, L, 0x4B, p1, p1, p2, nullptr, m << 4);         // vblendvpd p1 = m ? p2 : p1
        g.apply_mask(p1, dst, 0, 8, L);
        g.put(dst, 0, p1, L);
        g.zero_tail(dst, vl);
        return true;
    }
    if (L) {
        a.vex(2, 1, 0, 1, 0x16, p1, idx, T1, nullptr);                // vpermps p1 = T1[idx]
        g.vx(2, 1, 0, 1, 0x16, p2, idx, T2);
        a.vex(1, 1, 0, 1, 0x72, 6, m, idx, nullptr, 28);              // vpslld $28 -> table bit to sign
    } else {
        a.vex(2, 1, 0, 0, 0x0C, p1, T1, idx, nullptr);                // vpermilps p1 = T1[idx]
        int t = T2.reg ? T2.r : g.scratch(); if (!T2.reg) g.load(t, T2.m, 0);
        a.vex(2, 1, 0, 0, 0x0C, p2, t, idx, nullptr);
        a.vex(1, 1, 0, 0, 0x72, 6, m, idx, nullptr, 29);
    }
    a.vex(3, 1, 0, L, 0x4A, p1, p1, p2, nullptr, m << 4);             // vblendvps p1 = m ? p2 : p1
    g.apply_mask(p1, dst, 0, 4, L);
    g.put(dst, 0, p1, L);
    g.zero_tail(dst, vl);
    return true;
}

// ---- gathers (128/256 bit) via AVX2 gathers
static bool gen_gather(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (!in.aaa || g.vl == 64 || !in.has_sib || in.seg || in.adsize) return false;
    bool qidx = in.opc & 1;
    int esd = in.W ? 8 : 4, esi = qidx ? 8 : 4;
    int n = g.vl / (esd > esi ? esd : esi), dbytes = n * esd, ibytes = n * esi;
    int L = g.vl == 32 ? 1 : 0, Ld = dbytes == 32 ? 1 : 0, Li = ibytes == 32 ? 1 : 0;
    int vr = ((in.sib >> 3) & 7) | (in.X << 3) | (in.Vp << 4);
    int dst = reg_of(in);
    if (vr < 16) g.vavoid |= 1u << vr;
    int ix = vr;
    if (vr >= 16) { Opd xo = g.chunk(vr, 0); if (xo.reg) ix = xo.r; else { ix = g.scratch(); g.load(ix, xo.m, Li); } }
    int res = g.scratch();
    { Opd old = g.chunk(dst, 0); if (old.reg) g.movrr(res, old.r, Ld); else g.load(res, old.m, Ld); }
    int m = g.maskvec(esd, 0, Ld, false);
    Mem vm;
    vm.base = has_base(in) ? base_of(in) : -1;
    vm.index = ix; vm.scale = scale_of(in); vm.disp = mem_disp(in);
    a.vex(2, 1, in.W, L, in.opc, res, m, 0, &vm);
    g.put(dst, 0, res, Ld);
    g.zero_tail(dst, dbytes);
    a.mov_store_imm64(g.kreg(in.aaa), 0);
    return true;
}



// ---- byte permutes (AVX512VBMI) with vpshufb: vpshufb uses index bits 0-3 within a 128-bit lane (bit 7 zeroes, so it
// is cleared first); bit 4 picks the other lane / table half, bit 5 the second table (vpsllw by 3 / 2 moves it to bit 7
// of the same byte for vpblendvb).
// T (16 bytes per lane, both lanes = the 32-byte table T32) [idx & 31] -> res; m = idx & 0x0f, s = idx << 3
static void permb32(G& g, int res, int T32, int m, int s, int t2) {
    Asm& a = g.a;
    a.vex(3, 1, 1, 1, 0x00, res, 0, T32, nullptr, 0x44);   // vpermq res = [T.lo, T.lo]
    a.vex(3, 1, 1, 1, 0x00, t2, 0, T32, nullptr, 0xEE);    // vpermq t2 = [T.hi, T.hi]
    a.vex(2, 1, 0, 1, 0x00, res, res, m, nullptr);         // vpshufb
    a.vex(2, 1, 0, 1, 0x00, t2, t2, m, nullptr);
    a.vex(3, 1, 0, 1, 0x4C, res, res, t2, nullptr, s << 4); // vpblendvb res = s ? t2 : res
}
// vpermb (map2 66 8D W0): dst = rm[vvvv]; vpermt2b (map2 66 7D W0): dst = (dst:rm)[vvvv]; vpermi2b (75 W0): dst = (vvvv:rm)[dst]
static bool gen_permbytes(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (g.vl == 64 || in.b) return false;
    bool mem = in.mod != 3;
    if (mem && in.aaa) return false;
    int L = g.vl == 32 ? 1 : 0, dst = reg_of(in);
    bool two = in.opc != 0x8D, i2 = in.opc == 0x75;
    uint32_t before = g.vtaken;
    int I = g.in_reg(g.chunk(i2 ? dst : in.vvvv, 0), L);
    if (in.W) {   // word forms: index w (low log2(words) bits) -> byte indices 2w, 2w+1, then the byte permute
        int nw = (L ? 16 : 8) * (two ? 2 : 1);
        int t = g.scratch(), u = g.scratch();
        if (g.fail) return false;
        a.vex(1, 1, 0, L, 0x75, u, u, u, nullptr);                        // vpcmpeqw: all ones
        a.vex(1, 1, 0, L, 0x71, 2, u, u, nullptr, 16 - __builtin_ctz(nw)); // vpsrlw: nw - 1
        a.vex(1, 1, 0, L, 0xDB, t, I, u, nullptr);                         // w
        a.vex(1, 1, 0, L, 0x71, 6, t, t, nullptr, 1);                      // 2w
        a.vex(1, 1, 0, L, 0x71, 6, u, t, nullptr, 8);                      // 2w in the high byte too
        a.vex(1, 1, 0, L, 0xEB, t, t, u, nullptr);
        g.load(u, g.tls(TOFF(c_w01)), L);
        a.vex(1, 1, 0, L, 0x71, 6, u, u, nullptr, 8);                      // 0x0100
        a.vex(1, 1, 0, L, 0xFC, t, t, u, nullptr);                         // vpaddb: 2w, 2w + 1
        g.release(u);
        I = t;
    }
    int A = two ? g.in_reg(g.chunk(i2 ? in.vvvv : dst, 0), L) : -1;
    int B; if (mem) { B = g.scratch(); g.load(B, g.omem(), L); } else B = g.in_reg(g.chunk(rm_reg_of(in), 0), L);
    int m = g.scratch(), s = g.scratch(), res = g.scratch(), t2 = g.scratch();
    if (g.fail) return false;
    a.vex(1, 1, 0, L, 0xDB, m, I, 0, &g.tmp(g.tls(TOFF(c_b0f))));     // vpand m = idx & 0x0f
    if (!two) {
        if (!L) a.vex(2, 1, 0, 0, 0x00, res, B, m, nullptr);           // 16-byte table: one vpshufb
        else { a.vex(1, 1, 0, 1, 0x71, 6, s, I, nullptr, 3); permb32(g, res, B, m, s, t2); }
    } else if (!L) {   // two 16-byte tables: bit 4 picks B
        a.vex(1, 1, 0, 0, 0x71, 6, s, I, nullptr, 3);
        a.vex(2, 1, 0, 0, 0x00, res, A, m, nullptr);
        a.vex(2, 1, 0, 0, 0x00, t2, B, m, nullptr);
        a.vex(3, 1, 0, 0, 0x4C, res, res, t2, nullptr, s << 4);
    } else {           // two 32-byte tables: bit 4 lane, bit 5 table
        int rb = g.scratch(); if (g.fail) return false;
        a.vex(1, 1, 0, 1, 0x71, 6, s, I, nullptr, 3);
        permb32(g, res, A, m, s, t2);
        permb32(g, rb, B, m, s, t2);
        a.vex(1, 1, 0, 1, 0x71, 6, s, I, nullptr, 2);
        a.vex(3, 1, 0, 1, 0x4C, res, res, rb, nullptr, s << 4);
    }
    g.apply_mask(res, dst, 0, in.W ? 2 : 1, L);
    g.put(dst, 0, res, L);
    g.vtaken = before;
    g.zero_tail(dst, g.vl);
    return true;
}

// ---- vpmovqd (map2 F3 35 W0), register destination, no mask: qwords -> their low dwords
static bool gen_pmovqd(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.aaa || g.vl == 64) return false;
    int L = g.vl == 32 ? 1 : 0, src = reg_of(in), dst = rm_reg_of(in);
    int S = g.in_reg(g.chunk(src, 0), L), t = g.scratch();
    if (g.fail) return false;
    a.vex(1, 1, 0, L, 0x70, t, 0, S, nullptr, 0x08);           // vpshufd: [d0, d2, d0, d0] per lane
    if (L) a.vex(3, 1, 1, 1, 0x00, t, 0, t, nullptr, 0x08);    // vpermq [q0, q2, ..] -> [d0 d2 d4 d6]
    if (in.mod != 3) {                                          // store 16 (ymm) or 8 (xmm) bytes
        if (L) a.vex(1, 2, 0, 0, 0x7F, t, 0, 0, &g.tmp(g.omem()));   // vmovdqu m128
        else a.vex(1, 1, 0, 0, 0xD6, t, 0, 0, &g.tmp(g.omem()));     // vmovq m64
        return true;
    }
    if (!L) a.vex(1, 2, 0, 0, 0x7E, t, 0, t, nullptr);         // vmovq: keep 2 dwords, zero the rest
    g.put(dst, 0, t, 0);
    g.zero_tail(dst, 16);
    return true;
}

// ---- vpmovdw register form (map2 F3 33 W0), no mask: dwords truncated to words (high words cleared, vpackusdw)
static bool gen_pmovdw(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.mod != 3 || in.aaa || g.vl == 64) return false;
    int L = g.vl == 32 ? 1 : 0, src = reg_of(in), dst = rm_reg_of(in);
    int S = g.in_reg(g.chunk(src, 0), L), t = g.scratch(), z = g.scratch();
    if (g.fail) return false;
    a.vex(1, 1, 0, 0, 0xEF, z, z, z, nullptr);                        // vpxor z
    a.vex(3, 1, 0, L, 0x0E, t, S, z, nullptr, 0xAA);                  // vpblendw: high word of each dword = 0
    if (L) { a.vex(3, 1, 0, 1, 0x39, t, 0, z, nullptr, 1); a.vex(2, 1, 0, 0, 0x2B, t, t, z, nullptr); }  // vextracti128 z; vpackusdw lo, hi
    else a.vex(2, 1, 0, 0, 0x2B, t, t, z, nullptr);                   // vpackusdw t, 0 (upper half zero)
    g.put(dst, 0, t, 0);
    g.zero_tail(dst, 16);
    return true;
}
// ---- vpmovw2m (map2 F3 29 W1): k = sign bits of the words
static bool gen_pmovw2m(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.mod != 3 || g.vl == 64) return false;
    int L = g.vl == 32 ? 1 : 0, kd = reg_of(in) & 7;
    int S = g.in_reg(g.chunk(rm_reg_of(in), 0), L), t = g.scratch();
    int gr = g.gscratch(); if (g.fail || gr < 0) return false;
    if (in.opc == 0x39 || !in.W) {   // vpmovd2m (vmovmskps), vpmovq2m (vmovmskpd), vpmovb2m (vpmovmskb): bits zero-extended
        if (in.opc == 0x39) a.vex(1, in.W ? 1 : 0, 0, L, 0x50, gr, 0, S, nullptr);
        else a.vex(1, 1, 0, L, 0xD7, gr, 0, S, nullptr);
        a.mov_store64(g.kreg(kd), gr); g.kc_note(kd, gr);
        return true;
    }
    if (L) { a.vex(3, 1, 0, 1, 0x39, S, 0, t, nullptr, 1); a.vex(1, 1, 0, 0, 0x63, t, S, t, nullptr); }   // vextracti128; vpacksswb lo, hi
    else a.vex(1, 1, 0, 0, 0x63, t, S, S, nullptr);
    a.vex(1, 1, 0, 0, 0xD7, gr, 0, t, nullptr);                // vpmovmskb (16 bits)
    if (!L) a.movzx_rr8(gr, gr);
    a.mov_store64(g.kreg(kd), gr); g.kc_note(kd, gr);
    return true;
}

// ---- scatters (map2 66 A0 vpscatterdd/dq, A1 vpscatterqd/qq, A2 vscatterdps/dpd, A3 vscatterqps/qpd), xmm/ymm:
// element by element in order (later elements win on equal addresses), skipping clear mask bits; k = 0 at the end.
// Index from vpextrd (sign-extended, movsxd) / vpextrq, data from vpextrd/vextractps / vpextrq; bt + jnc per
// element (live flags are saved).
static bool gen_scatter(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (!in.aaa || g.vl == 64 || !in.has_sib || in.seg || in.adsize) return false;
    bool qidx = in.opc & 1;
    int esd = in.W ? 8 : 4, esi = qidx ? 8 : 4;
    int n = g.vl / (esd > esi ? esd : esi);           // elements
    int ibytes = n * esi, dbytes = n * esd;
    int vr = ((in.sib >> 3) & 7) | (in.X << 3) | (in.Vp << 4);
    if (vr < 16) g.vavoid |= 1u << vr;
    int ix = vr; if (vr >= 16) { Opd xo = g.chunk(vr, 0); if (xo.reg) ix = xo.r; else { ix = g.scratch(); g.load(ix, xo.m, ibytes == 32); } }
    int V = g.in_reg(g.chunk(reg_of(in), 0), dbytes == 32);
    int ihi = ibytes == 32 ? g.scratch() : -1, vhi = dbytes == 32 ? g.scratch() : -1;
    int gk = g.gscratch(), gi = g.gscratch(), gv = g.gscratch();
    if (g.fail || gk < 0 || gi < 0 || gv < 0) return false;
    if (ihi >= 0) a.vex(3, 1, 0, 1, 0x39, ix, 0, ihi, nullptr, 1);   // vextracti128 upper indices
    if (vhi >= 0) a.vex(3, 1, 0, 1, 0x39, V, 0, vhi, nullptr, 1);    // vextracti128 upper data
    g.save_flags();
    kget(g, gk, in.aaa, 16);
    int iper = 16 / esi, dper = 16 / esd;                             // elements per 128-bit chunk
    for (int i = 0; i < n; i++) {
        a.leg(0, 1, 1, 0xBA, 4, gk, nullptr, 1, i);                  // bt gk, i
        int at = a.n; a.jcc(3, a.pc());                               // jnc skip (patched below)
        int xs = i < iper ? ix : ihi, xv = i < dper ? V : vhi;
        if (qidx) a.vex(3, 1, 1, 0, 0x16, xs, 0, gi, nullptr, i % iper);                          // vpextrq
        else { a.vex(3, 1, 0, 0, 0x16, xs, 0, gi, nullptr, i % iper); a.leg(0, 1, 0, 0x63, gi, gi, nullptr); }   // vpextrd + movsxd
        a.vex(3, 1, esd == 8 ? 1 : 0, 0, 0x16, xv, 0, gv, nullptr, i % dper);                      // vpextrd/q data
        Mem m = g.omem(); m.index = gi; m.scale = scale_of(in);       // base/disp (RSP-adjusted after save_flags)
        if (esd == 8) a.mov_store64(m, gv); else a.mov_store32(m, gv);
        int64_t rel = int64_t(a.n) - int64_t(at + 6);
        for (int b = 0; b < 4; b++) a.buf[at + 2 + b] = uint8_t(uint32_t(int32_t(rel)) >> (8 * b));
    }
    a.mov_store_imm64(g.kreg(in.aaa), 0);
    return true;
}

// ---- vmovlps/vmovlpd/vmovhps/vmovhpd stores (map1 13 / 17, pp 0/1) - e.g. from xmm16-31 (no VEX form)
static bool gen_movlhst(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.mod == 3 || in.aaa || in.b) return false;
    int S = g.in_reg(g.chunk(reg_of(in), 0), 0);
    if (g.fail) return false;
    a.vex(1, in.pp, 0, 0, in.opc, S, 0, 0, &g.tmp(g.omem()));
    return true;
}

// ---- vinsertf32x4/64x2, vinserti32x4/64x2 (map3 66 18 / 38) and vextract* (19 / 39), 256-bit only
static bool gen_insext128(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (g.vl != 32 || in.b) return false;
    bool mem = in.mod != 3, ins = in.opc == 0x18 || in.opc == 0x38;
    int es = in.W ? 8 : 4, imm = in.imm8 & 1;
    if (ins) {
        if (mem && in.aaa) return false;
        int dst = reg_of(in);
        int A = g.in_reg(g.chunk(in.vvvv, 0), 1), res = g.scratch();
        if (g.fail) return false;
        if (mem) a.vex(3, 1, 0, 1, 0x18, res, A, 0, &g.tmp(g.omem()), imm);
        else { int B = g.in_reg(g.chunk(rm_reg_of(in), 0), 0); if (g.fail) return false; a.vex(3, 1, 0, 1, 0x18, res, A, B, nullptr, imm); }
        g.apply_mask(res, dst, 0, es, 1);
        g.put(dst, 0, res, 1);
        g.zero_tail(dst, 32);
        return true;
    }
    int S = g.in_reg(g.chunk(reg_of(in), 0), 1);
    if (g.fail) return false;
    if (mem) { if (in.aaa) return false; a.vex(3, 1, 0, 1, 0x19, S, 0, 0, &g.tmp(g.omem()), imm); return true; }
    int dst = rm_reg_of(in), res = g.scratch();
    if (g.fail) return false;
    a.vex(3, 1, 0, 1, 0x19, S, 0, res, nullptr, imm);                 // vextractf128 res = S.half
    g.apply_mask(res, dst, 0, es, 0);
    g.put(dst, 0, res, 0);
    g.zero_tail(dst, 16);
    return true;
}

// ---- vblendmps/pd, vpblendmb/w/d/q: dst = k ? rm : vvvv (zeroing: k ? rm : 0)
// ---- vpmullq (AVX512DQ, no AVX2 form): lo64(a*b) = a_lo*b_lo + ((a_hi*b_lo + a_lo*b_hi) << 32), all mod 2^64
static bool gen_pmullq(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    bool mem = in.mod != 3;
    if (mem && in.aaa && !in.b) return false;   // masked full-width memory: fault suppression -> interpreter
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int bcst = -1; Mem pm; bool pool = false;
    if (in.b && mem) {
        if (g.cpool_bcast(8, pm)) pool = true;
        else { bcst = g.scratch(); if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem())); else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem())); }
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int A = g.in_reg(g.chunk(in.vvvv, c), L), B;
        if (bcst >= 0) B = bcst;
        else if (pool) { B = g.scratch(); g.load(B, pm, L); }
        else if (mem) { B = g.scratch(); g.load(B, g.omem(32 * c), L); }
        else B = g.in_reg(g.chunk(rm_reg_of(in), c), L);
        int t1 = g.scratch(), t2 = g.scratch(), res = g.scratch();
        if (g.fail) return false;
        a.vex(1, 1, 0, L, 0x73, 2, t1, A, nullptr, 32);      // vpsrlq  t1 = a >> 32
        a.vex(1, 1, 0, L, 0xF4, t1, t1, B, nullptr);         // vpmuludq t1 = a_hi * b_lo
        a.vex(1, 1, 0, L, 0x73, 2, t2, B, nullptr, 32);      // vpsrlq  t2 = b >> 32
        a.vex(1, 1, 0, L, 0xF4, t2, t2, A, nullptr);         // vpmuludq t2 = b_hi * a_lo
        a.vex(1, 1, 0, L, 0xD4, t1, t1, t2, nullptr);        // vpaddq
        a.vex(1, 1, 0, L, 0x73, 6, t1, t1, nullptr, 32);     // vpsllq  t1 <<= 32
        a.vex(1, 1, 0, L, 0xF4, res, A, B, nullptr);         // vpmuludq res = a_lo * b_lo
        a.vex(1, 1, 0, L, 0xD4, res, res, t1, nullptr);      // vpaddq
        g.apply_mask(res, dst, c, 8, L);
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// vpmaxsq / vpminsq / vpmaxuq / vpminuq (map 2, 66, W1: 3D 39 3F 3B) and vpabsq (1F W1): AVX2 has no 64-bit
// min/max/abs: vpcmpgtq (unsigned: both operands with the sign bit flipped) + vblendvpd
static bool gen_minmaxq(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    bool mem = in.mod != 3, absq = in.opc == 0x1F;
    if (mem && in.aaa && !in.b) return false;   // masked full-width memory: fault suppression -> interpreter
    bool uns = in.opc == 0x3F || in.opc == 0x3B, mx = in.opc == 0x3D || in.opc == 0x3F;
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int bcst = -1; Mem pm; bool pool = false;
    if (in.b && mem) {
        if (g.cpool_bcast(8, pm)) pool = true;
        else { bcst = g.scratch(); if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem())); else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem())); }
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int A = absq ? -1 : g.in_reg(g.chunk(in.vvvv, c), L), B;
        if (bcst >= 0) B = bcst;
        else if (pool) { B = g.scratch(); g.load(B, pm, L); }
        else if (mem) { B = g.scratch(); g.load(B, g.omem(32 * c), L); }
        else B = g.in_reg(g.chunk(rm_reg_of(in), c), L);
        int t = g.scratch(), res = g.scratch();
        if (g.fail) return false;
        if (absq) {
            a.vex(1, 1, 0, L, 0xEF, res, res, res, nullptr);           // vpxor res = 0
            a.vex(2, 1, 0, L, 0x37, t, res, B, nullptr);               // vpcmpgtq t = 0 > b
            a.vex(1, 1, 0, L, 0xFB, res, res, B, nullptr);             // vpsubq res = -b
            a.vex(3, 1, 0, L, 0x4B, res, B, res, nullptr, t << 4);     // vblendvpd res = t ? -b : b
        } else {
            if (uns) {
                int xa = g.scratch(); if (g.fail) return false;
                a.vex(1, 1, 0, L, 0xEF, xa, A, 0, &g.tmp(g.tls(TOFF(c_sign64))));   // vpxor xa = a ^ sign
                a.vex(1, 1, 0, L, 0xEF, t, B, 0, &g.tmp(g.tls(TOFF(c_sign64))));    // vpxor t = b ^ sign
                a.vex(2, 1, 0, L, 0x37, t, xa, t, nullptr);                          // vpcmpgtq t = a > b
                g.release(xa);
            } else a.vex(2, 1, 0, L, 0x37, t, A, B, nullptr);                        // vpcmpgtq t = a > b
            if (mx) a.vex(3, 1, 0, L, 0x4B, res, B, A, nullptr, t << 4);             // max: t ? a : b
            else a.vex(3, 1, 0, L, 0x4B, res, A, B, nullptr, t << 4);                // min: t ? b : a
        }
        g.apply_mask(res, dst, c, 8, L);
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// valignd / valignq xmm/ymm (map 3, 66, 03): (src1:src2) shifted right by imm elements. xmm: vpalignr; ymm:
// vperm2i128 brings the middle 128 bits together, then vpalignr per lane
static bool gen_valign(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b || g.vl == 64) return false;
    bool mem = in.mod != 3;
    if (mem && in.aaa) return false;
    int es = in.W ? 8 : 4, vl = g.vl, L = vl == 32 ? 1 : 0, dst = reg_of(in);
    int bytes = (in.imm8 & (vl / es - 1)) * es;
    int A = g.in_reg(g.chunk(in.vvvv, 0), L), B;
    if (mem) { B = g.scratch(); g.load(B, g.omem(0), L); } else B = g.in_reg(g.chunk(rm_reg_of(in), 0), L);
    int res = g.scratch();
    if (g.fail) return false;
    if (!L) a.vex(3, 1, 0, 0, 0x0F, res, A, B, nullptr, bytes);                     // vpalignr res = (a:b) >> bytes
    else {
        int t = g.scratch(); if (g.fail) return false;
        a.vex(3, 1, 0, 1, 0x46, t, B, A, nullptr, 0x21);                              // vperm2i128 t = [b.hi, a.lo]
        if (bytes < 16) a.vex(3, 1, 0, 1, 0x0F, res, t, B, nullptr, bytes);           // lanes: (b.hi:b.lo), (a.lo:b.hi)
        else a.vex(3, 1, 0, 1, 0x0F, res, A, t, nullptr, bytes - 16);                 // lanes: (a.lo:b.hi), (a.hi:a.lo)
    }
    g.apply_mask(res, dst, 0, es, L);
    g.put(dst, 0, res, L);
    g.zero_tail(dst, vl);
    return true;
}

// vpinsrb/w/d/q (map 3 20/22, map 1 C4): any registers (xmm16-31 too): source and result through scratch registers
static bool gen_pinsr(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (g.vl != 16 || in.aaa) return false;
    int dst = reg_of(in);
    int A = g.in_reg(g.chunk(in.vvvv, 0), 0), res = g.scratch();
    if (g.fail) return false;
    int map = in.map, opc = in.opc, W = in.opc == 0x22 ? in.W : 0;
    if (in.mod != 3) a.vex(map, 1, W, 0, opc, res, A, 0, &g.tmp(g.omem()), in.imm8);
    else a.vex(map, 1, W, 0, opc, res, A, in.rm3 | (in.B << 3), nullptr, in.imm8);     // GPR source
    g.put(dst, 0, res, 0);
    g.zero_tail(dst, 16);
    return true;
}

// zmm halves and quarters: vinsert/vextract f64x4/f32x8/i64x4/i32x8 (1A 1B 3A 3B) and 128-bit ones with a zmm
// (18 19 38 39 at vl 64): ymm chunks moved/combined; vinsertf128/vextractf128 within a chunk
static bool gen_insext512(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (g.vl != 64 || in.b) return false;
    bool mem = in.mod != 3, q256 = (in.opc & 0x0f) >= 0x0a, ins = (in.opc & 1) == 0;
    int es = in.W ? 8 : 4, lane = in.imm8 & (q256 ? 1 : 3);
    if (ins) {
        if (mem && in.aaa) return false;
        int dst = reg_of(in), r[2];
        for (int c = 0; c < 2; c++) {
            r[c] = g.scratch(); if (g.fail) return false;
            int S = g.in_reg(g.chunk(in.vvvv, c), 1); if (g.fail) return false;
            if (q256) {
                if (c == lane) { if (mem) g.load(r[c], g.omem(), 1); else { int B = g.in_reg(g.chunk(rm_reg_of(in), 0), 1); if (g.fail) return false; g.movrr(r[c], B, 1); } }
                else g.movrr(r[c], S, 1);
            } else if (c == (lane >> 1)) {
                if (mem) a.vex(3, 1, 0, 1, 0x18, r[c], S, 0, &g.tmp(g.omem()), lane & 1);
                else { int B = g.in_reg(g.chunk(rm_reg_of(in), 0), 0); if (g.fail) return false; a.vex(3, 1, 0, 1, 0x18, r[c], S, B, nullptr, lane & 1); }
            } else g.movrr(r[c], S, 1);
        }
        for (int c = 0; c < 2; c++) { g.apply_mask(r[c], dst, c, es, 1); g.put(dst, c, r[c], 1); }
        g.zero_tail(dst, 64);
        return true;
    }
    int S = g.in_reg(g.chunk(reg_of(in), q256 ? lane : lane >> 1), 1);
    if (g.fail) return false;
    if (mem) {
        if (in.aaa) return false;
        if (q256) a.vex(1, 0, 0, 1, 0x11, S, 0, 0, &g.tmp(g.omem()));           // vmovups m256
        else a.vex(3, 1, 0, 1, 0x19, S, 0, 0, &g.tmp(g.omem()), lane & 1);       // vextractf128 m128
        return true;
    }
    int dst = rm_reg_of(in), res = g.scratch();
    if (g.fail) return false;
    if (q256) g.movrr(res, S, 1); else a.vex(3, 1, 0, 1, 0x19, S, 0, res, nullptr, lane & 1);
    g.apply_mask(res, dst, 0, es, q256 ? 1 : 0);
    g.put(dst, 0, res, q256 ? 1 : 0);
    g.zero_tail(dst, q256 ? 32 : 16);
    return true;
}

// vshuff32x4/64x2, vshufi32x4/64x2 (23 43) ymm/zmm: 128-bit lanes picked by imm, two vperm2i128 per zmm
static bool gen_shuf128(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b || g.vl < 32) return false;
    bool mem = in.mod != 3;
    if (mem && in.aaa) return false;
    int es = in.W ? 8 : 4, dst = reg_of(in), imm = in.imm8, vl = g.vl;
    auto src2 = [&](int c) { int B; if (mem) { B = g.scratch(); g.load(B, g.omem(32 * c), 1); } else B = g.in_reg(g.chunk(rm_reg_of(in), c), 1); return B; };
    if (vl == 32) {
        int A = g.in_reg(g.chunk(in.vvvv, 0), 1), B = src2(0), res = g.scratch();
        if (g.fail) return false;
        a.vex(3, 1, 0, 1, 0x46, res, A, B, nullptr, (imm & 1) | ((2 + ((imm >> 1) & 1)) << 4));
        g.apply_mask(res, dst, 0, es, 1); g.put(dst, 0, res, 1); g.zero_tail(dst, 32);
        return true;
    }
    int r[2];
    for (int c = 0; c < 2; c++) {
        int l0 = (imm >> (4 * c)) & 3, l1 = (imm >> (4 * c + 2)) & 3;
        int X, Y;
        if (c == 0) { X = g.in_reg(g.chunk(in.vvvv, l0 >> 1), 1); Y = g.in_reg(g.chunk(in.vvvv, l1 >> 1), 1); }
        else { X = src2(l0 >> 1); Y = src2(l1 >> 1); }
        r[c] = g.scratch();
        if (g.fail) return false;
        a.vex(3, 1, 0, 1, 0x46, r[c], X, Y, nullptr, (l0 & 1) | ((2 + (l1 & 1)) << 4));   // vperm2i128
    }
    for (int c = 0; c < 2; c++) { g.apply_mask(r[c], dst, c, es, 1); g.put(dst, c, r[c], 1); }
    g.zero_tail(dst, 64);
    return true;
}

// vpdpwssd (map 2 52 W0): dst += pairs of int16 products; vpmaddwd wraps the -32768*-32768*2 case the same way
// vpshldvd/q (map 2 71): dst = high half of ((dst:src2) << (src3 & mask)) = (dst << c) | ((src2 >> 1) >> (bits-1-c))
static bool gen_dpshld(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    bool dp = in.opc == 0x52;
    if (dp && in.W) return false;
    int es = dp ? 4 : (in.W ? 8 : 4);
    if (in.b && in.mod == 3) return false;
    bool mem = in.mod != 3;
    if (mem && in.aaa && !in.b) return false;
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int bcst = -1; Mem pm; bool pool = false;
    if (in.b && mem) {
        if (g.cpool_bcast(es, pm)) pool = true;
        else { bcst = g.scratch(); a.vex(2, 1, 0, L, es == 8 ? 0x59 : 0x58, bcst, 0, 0, &g.tmp(g.omem())); }   // vpbroadcastq/d
    }
    int sh = es == 8 ? 58 : 27;
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int D = g.in_reg(g.chunk(dst, c), L), A = g.in_reg(g.chunk(in.vvvv, c), L), B;
        if (bcst >= 0) B = bcst;
        else if (pool) { B = g.scratch(); g.load(B, pm, L); }
        else if (mem) { B = g.scratch(); g.load(B, g.omem(32 * c), L); }
        else B = g.in_reg(g.chunk(rm_reg_of(in), c), L);
        int res = g.scratch();
        if (g.fail) return false;
        if (dp) {
            a.vex(1, 1, 0, L, 0xF5, res, A, B, nullptr);                         // vpmaddwd
            a.vex(1, 1, 0, L, 0xFE, res, res, D, nullptr);                       // vpaddd
        } else {
            int t = g.scratch(), u = g.scratch(); if (g.fail) return false;
            int sl = es == 8 ? 0x73 : 0x72;                                      // vpsllq/vpsrlq imm (73 /6 /2), d: 72
            a.vex(1, 1, 0, L, sl, 6, t, B, nullptr, sh); a.vex(1, 1, 0, L, sl, 2, t, t, nullptr, sh);   // t = c
            a.vex(2, 1, es == 8, L, 0x47, res, D, t, nullptr);                   // vpsllv res = dst << c
            a.vex(1, 1, 0, L, 0xEF, t, B, 0, &g.tmp(g.tls(TOFF(c_ones))));      // t = ~src3
            a.vex(1, 1, 0, L, sl, 6, t, t, nullptr, sh); a.vex(1, 1, 0, L, sl, 2, t, t, nullptr, sh);   // t = bits-1-c
            a.vex(1, 1, 0, L, sl, 2, u, A, nullptr, 1);                          // u = src2 >> 1
            a.vex(2, 1, es == 8, L, 0x45, u, u, t, nullptr);                     // vpsrlv u >>= bits-1-c
            a.vex(1, 1, 0, L, 0xEB, res, res, u, nullptr);                       // vpor
            g.release(t); g.release(u);
        }
        g.apply_mask(res, dst, c, es, L);
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// vpexpandd xmm/ymm register source, masked: lane j (mask bit set) <- source element popcount(mask below j).
// Indices with BMI2: nibble mask M = pdep(m, 0x11111111) * 15, idx = pdep(0x76543210, M) (one nibble per lane),
// bytes = pdep(idx, 0x0f0f..) -> vmovq + vpmovzxbd -> vpermd; lanes with the mask bit clear: zeroed/merged by apply_mask
static bool gen_expandd(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.W || in.mod != 3 || !in.aaa || g.vl == 64) return false;
    int vl = g.vl, L = vl == 32 ? 1 : 0, n = vl / 4, dst = reg_of(in);
    g.save_flags();                                                     // and/shl/sub change flags
    int S = g.in_reg(g.chunk(rm_reg_of(in), 0), L);
    int gm = g.gscratch(), gt = g.gscratch(), gc = g.gscratch();
    int ix = g.scratch(), res = g.scratch();
    if (g.fail || gm < 0 || gt < 0 || gc < 0) return false;
    a.movzx8(gm, g.kreg(in.aaa));                                       // m = k (8 bits)
    if (n < 8) a.alu_ri(4, 0, gm, (1 << n) - 1);                       // and
    a.mov_imm32(gc, 0x11111111);
    a.vex(2, 3, 0, 0, 0xF5, gt, gm, gc, nullptr);                       // pdep gt = pdep(m, 0x11111111)
    a.mov_rr32(gm, gt); a.shift_ri(4, 0, gm, 4); a.alu_rr(0x28, 0, gm, gt);   // gm = gt * 15 (shl 4, sub)
    a.mov_imm32(gc, 0x76543210);
    a.vex(2, 3, 0, 0, 0xF5, gt, gc, gm, nullptr);                       // pdep gt = pdep(0x76543210, M)
    a.mov_imm64(gc, 0x0F0F0F0F0F0F0F0Full);
    a.vex(2, 3, 1, 0, 0xF5, gm, gt, gc, nullptr);                       // pdep gm = pdep(idx, 0x0f0f..) (64-bit)
    a.vex(1, 1, 1, 0, 0x6E, ix, 0, gm, nullptr);                        // vmovq ix, gm
    a.vex(2, 1, 0, 1, 0x31, ix, 0, ix, nullptr);                        // vpmovzxbd ymm ix
    a.vex(2, 1, 0, 1, 0x36, res, ix, S, nullptr);                       // vpermd res = S[ix]
    g.apply_mask(res, dst, 0, 4, L);
    g.put(dst, 0, res, L);
    g.zero_tail(dst, vl);
    return true;
}

static bool gen_blendm(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    int es = in.opc == 0x66 ? (in.W ? 2 : 1) : (in.W ? 8 : 4);
    if (in.mod != 3 && es < 4 && in.aaa) return false;
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    bool mem = in.mod != 3;
    int bcst = -1;
    if (in.b && mem) {
        bcst = g.scratch();
        if (es == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));
        else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));
        else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int res = g.scratch();
        if (!in.aaa) { // no mask: plain copy of rm
            if (bcst >= 0) g.movrr(res, bcst, L); else if (mem) g.load(res, g.omem(32 * c), L);
            else { Opd x = g.chunk(rm_reg_of(in), c); if (x.reg) g.movrr(res, x.r, L); else g.load(res, x.m, L); }
        } else {
            int m = g.maskvec(es, c, L, false);
            Opd src;
            if (bcst >= 0) src = R_(bcst);
            else if (mem) { int s = g.scratch(); a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, s, m, 0, &g.tmp(g.omem(32 * c))); src = R_(s); }
            else src = g.chunk(rm_reg_of(in), c);
            if (in.z) { // res = m & src
                if (src.reg) a.vex(1, 1, 0, L, 0xDB, res, m, src.r, nullptr); else a.vex(1, 1, 0, L, 0xDB, res, m, 0, &src.m);
            } else {
                int s1 = g.in_reg(g.chunk(in.vvvv, c), L);
                g.vx(3, 1, 0, L, 0x4C, res, s1, src, m << 4);   // vpblendvb res = m ? src : s1
            }
        }
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- simple forms with an identical VEX encoding: unmasked, 128-bit or scalar,
// all vector operands either in hardware registers or loaded from the thread state.
static bool gen_simple(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.aaa || in.b) return false;
    bool mem = in.mod != 3;
    int k = in.map << 12 | in.pp << 8 | in.opc;
    int W = in.W, L = in.L;
    enum { CMPF, CVTSI, XFROM, XTO_G, EXT } kind;
    int vexW = 0;
    switch (k) {
    case 0x102e: case 0x102f: if (W) return false; kind = CMPF; break;           // vucomiss / vcomiss
    case 0x112e: case 0x112f: if (!W) return false; kind = CMPF; break;          // vucomisd / vcomisd
    case 0x122a: case 0x132a: kind = CVTSI; vexW = W; break;                     // vcvtsi2ss/sd
    case 0x122c: case 0x132c: case 0x122d: case 0x132d: kind = XTO_G; vexW = W; break; // vcvt(t)ss/sd2si
    case 0x117e: kind = XTO_G; vexW = W; break;                                  // vmovd/q r/m <- xmm
    case 0x116e: kind = CVTSI; vexW = W; break;                                  // vmovd/q xmm <- r/m
    case 0x3114: case 0x3115: case 0x3116: case 0x3117: kind = EXT; vexW = W; break; // vpextrb/w/d/q, vextractps
    case 0x12e6: if (W || L > 1) return false; kind = XFROM; break;               // vcvtdq2pd
    case 0x105a: if (W || L > 1) return false; kind = XFROM; break;               // vcvtps2pd
    default:
        if (in.map == 2 && in.pp == 1 && ((in.opc >= 0x20 && in.opc <= 0x25) || (in.opc >= 0x30 && in.opc <= 0x35)) && L <= 1) { kind = XFROM; break; }
        return false;
    }
    switch (kind) {
    case CMPF: { // flags out
        int r = g.in_reg(g.chunk(reg_of(in), 0), 0);
        if (mem) a.vex(1, in.pp, 0, 0, in.opc, r, 0, 0, &g.tmp(g.omem()));
        else { int s = g.in_reg(g.chunk(rm_reg_of(in), 0), 0); a.vex(1, in.pp, 0, 0, in.opc, r, 0, s, nullptr); }
        return true; }
    case CVTSI: { // dst xmm (reg), src1 vvvv (cvtsi only), rm = GPR or memory
        int dst = reg_of(in);
        int s1 = in.opc == 0x2a ? g.in_reg(g.chunk(in.vvvv, 0), 0) : 0;
        int res = dst < 16 ? dst : g.scratch();
        if (mem) a.vex(1, in.pp, vexW, 0, in.opc, res, s1, 0, &g.tmp(g.omem()));
        else a.vex(1, in.pp, vexW, 0, in.opc, res, s1, rm_reg_of(in) & 15, nullptr);
        g.put(dst, 0, res, 0);
        g.zero_tail(dst, 16);
        return true; }
    case XTO_G: { // GPR (reg field, or rm for vmovd store) <- xmm
        if (in.opc == 0x7e) {
            int x = g.in_reg(g.chunk(reg_of(in), 0), 0);
            if (mem) a.vex(1, 1, vexW, 0, 0x7E, x, 0, 0, &g.tmp(g.omem()));
            else { if ((rm_reg_of(in) & 15) == RSP) return false; a.vex(1, 1, vexW, 0, 0x7E, x, 0, rm_reg_of(in) & 15, nullptr); }
            return true;
        }
        if (in.b) return false;
        int gd = reg_of(in) & 15; if (gd == RSP) return false;
        if (mem) a.vex(1, in.pp, vexW, 0, in.opc, gd, 0, 0, &g.tmp(g.omem()));
        else { int x = g.in_reg(g.chunk(rm_reg_of(in), 0), 0); a.vex(1, in.pp, vexW, 0, in.opc, gd, 0, x, nullptr); }
        return true; }
    case EXT: { // r/m <- xmm (reg field), imm
        int x = g.in_reg(g.chunk(reg_of(in), 0), 0);
        if (mem) a.vex(3, 1, vexW, 0, in.opc, x, 0, 0, &g.tmp(g.omem()), in.imm8);
        else { if ((rm_reg_of(in) & 15) == RSP) return false; a.vex(3, 1, vexW, 0, in.opc, x, 0, rm_reg_of(in) & 15, nullptr, in.imm8); }
        return true; }
    case XFROM: { // widening: dst (L) <- half/quarter source (xmm or memory)
        int dst = reg_of(in);
        int res = (dst < 16) ? dst : g.scratch();
        if (mem) a.vex(in.map, in.pp, 0, L, in.opc, res, 0, 0, &g.tmp(g.omem()));
        else { int s = g.in_reg(g.chunk(rm_reg_of(in), 0), 0); a.vex(in.map, in.pp, 0, L, in.opc, res, 0, s, nullptr); }
        g.put(dst, 0, res, L);
        g.zero_tail(dst, L ? 32 : 16);
        return true; }
    }
    return false;
}


// ---- vpmovm2b/w/d/q: lanes all-ones where the k bit is set
static bool gen_movm2(G& g) {
    const Insn& in = g.in;
    if (in.mod != 3) return false;
    int es = in.opc == 0x28 ? (in.W ? 2 : 1) : (in.W ? 8 : 4);
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int k = in.rm3;
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int m = g.maskvec_k(es, c, L, k);
        g.put(dst, c, m, L);
        g.vtaken = before;
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- element-wise conversions with an AVX2 sequence: vcvttps2udq, vcvtuqq2pd
static bool gen_cvt_custom(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    bool s2d = in.opc == 0xE6;           // vcvtqq2pd (F3 E6 W1)
    bool u2d = in.opc == 0x7a || s2d;    // vcvtuqq2pd (F3 W1) / else vcvttps2udq (np W0)
    int es = u2d ? 8 : 4;
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    bool mem = in.mod != 3;
    int bcst = -1;
    if (in.b && mem) {
        bcst = g.scratch();
        if (es == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));
        else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));
        else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int x;
        if (bcst >= 0) x = bcst;
        else if (mem && in.aaa) { int m = g.maskvec(es, c, L, false); x = g.scratch(); a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, x, m, 0, &g.tmp(g.omem(32 * c))); g.release(m); }
        else if (mem) { x = g.scratch(); g.load(x, g.omem(32 * c), L); }
        else x = g.in_reg(g.chunk(rm_reg_of(in), c), L);
        int res = g.scratch(), t = g.scratch(), u = g.scratch();
        if (u2d) {
            a.vex(3, 1, 0, L, 0x02, res, x, 0, &g.tmp(g.tls(TOFF(c_4330))), 0xAA);      // vpblendd: low dwords of x, high 0x43300000
            if (s2d) {   // signed: high half + 2^31 (sign bit flipped), 2^63 more subtracted below
                a.vex(1, 1, 0, L, 0xEF, t, x, 0, &g.tmp(g.tls(TOFF(c_sign64))));        // vpxor t = x ^ 2^63
                a.vex(1, 1, 0, L, 0x73, 2, t, t, nullptr, 32);                           // vpsrlq t >>= 32
            } else a.vex(1, 1, 0, L, 0x73, 2, t, x, nullptr, 32);                        // vpsrlq t = x >> 32
            a.vex(1, 1, 0, L, 0xEB, t, t, 0, &g.tmp(g.tls(TOFF(c_4530))));              // vpor
            a.vex(1, 1, 0, L, 0x5C, t, t, 0, &g.tmp(g.tls(s2d ? TOFF(c_2p84p63p52) : TOFF(c_2p84p52))));   // vsubpd (exact)
            a.vex(1, 1, 0, L, 0x58, res, t, res, nullptr);                              // vaddpd res = t + res (one rounding)
            if (s2d) {   // x == 0 -> -0.0 under round-down: +0.0 there
                a.vex(2, 1, 0, L, 0x29, u, x, 0, &g.tmp(g.tls(TOFF(c_zero))));          // vpcmpeqq u = x == 0
                a.vex(1, 1, 0, L, 0x55, res, u, res, nullptr);                          // vandnpd
            } else {
                g.load(u, g.tls(TOFF(c_sign64)), L);                                    // 0 -> -0.0 under round-down:
                a.vex(1, 1, 0, L, 0x55, res, u, res, nullptr);                          // vandnpd clears the sign (result >= 0)
            }
        } else {
            a.vex(1, 2, 0, L, 0x5B, res, 0, x, nullptr);                                // vcvttps2dq a
            a.vex(1, 0, 0, L, 0x5C, t, x, 0, &g.tmp(g.tls(TOFF(c_2p31f))));             // t = x - 2^31
            a.vex(1, 2, 0, L, 0x5B, t, 0, t, nullptr);                                  // cvtt
            a.vex(1, 1, 0, L, 0xEF, t, t, 0, &g.tmp(g.tls(TOFF(c_sign32))));            // ^ 0x80000000 -> b
            a.vex(1, 0, 0, L, 0xC2, u, x, 0, &g.tmp(g.tls(TOFF(c_2p31f))), 0x1D);       // u = x >= 2^31 (GE_OQ)
            a.vex(3, 1, 0, L, 0x4A, res, res, t, nullptr, u << 4);                     // res = u ? b : a
            a.vex(1, 0, 0, L, 0xC2, u, x, 0, &g.tmp(g.tls(TOFF(c_m1f))), 0x1E);         // u = x > -1 (GT_OQ)
            a.vex(1, 0, 0, L, 0xC2, t, x, 0, &g.tmp(g.tls(TOFF(c_2p32f))), 0x11);       // t = x < 2^32 (LT_OQ)
            a.vex(1, 1, 0, L, 0xDB, u, u, t, nullptr);                                  // valid
            a.vex(1, 1, 0, L, 0xDF, u, u, 0, &g.tmp(g.tls(TOFF(c_ones))));              // invalid = ~valid
            a.vex(1, 1, 0, L, 0xEB, res, res, u, nullptr);                              // invalid -> all ones
        }
        g.apply_mask(res, dst, c, es, L);
        g.put(dst, c, res, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}


// ---- vcvttps2uqq, vcvttpd2uqq, vcvttpd2udq, vcvtpd2ps (128/256-bit destinations from <= 256-bit sources)
static bool gen_cvt_wide(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    int vl = g.vl; if (vl == 64) return false;
    int kind; // 0 ps2uqq, 1 pd2uqq, 2 pd2udq, 3 pd2ps
    if (in.opc == 0x5a) kind = 3; else if (in.pp == 1) kind = in.W ? 1 : 0; else kind = 2;
    int L = vl == 32 ? 1 : 0;
    int dst = reg_of(in);
    bool mem = in.mod != 3;
    bool narrow = kind >= 2;                 // dst is half of vl
    int Ld = narrow ? 0 : L;                 // dst register width
    int esd = narrow ? 4 : 8;                // dst element size (masking)
    int ess = kind == 0 ? 4 : 8;             // src element size
    int n = vl / 8;                          // element count
    // load source as doubles into d (ymm when n == 4)
    int Lsrc = (n * 8 == 32) ? 1 : 0;        // width of the double vector
    int d = g.scratch();
    if (kind == 0) {
        int f;
        if (in.b && mem) { f = g.scratch(); a.vex(2, 1, 0, 0, 0x18, f, 0, 0, &g.tmp(g.omem())); }
        else if (mem && in.aaa) { int m = g.maskvec(4, 0, 0, false); f = g.scratch(); a.vex(2, 1, 0, 0, 0x2C, f, m, 0, &g.tmp(g.omem())); g.release(m); }
        else if (mem) { f = -1; a.vex(1, 0, 0, Lsrc, 0x5A, d, 0, 0, &g.tmp(g.omem())); }
        else f = g.in_reg(g.chunk(rm_reg_of(in), 0), 0);
        if (f >= 0) a.vex(1, 0, 0, Lsrc, 0x5A, d, 0, f, nullptr);          // vcvtps2pd (exact)
    } else {
        if (in.b && mem) { if (Lsrc) a.vex(2, 1, 0, 1, 0x19, d, 0, 0, &g.tmp(g.omem())); else a.vex(1, 3, 0, 0, 0x12, d, 0, 0, &g.tmp(g.omem())); }
        else if (mem && in.aaa) { int m = g.maskvec(8, 0, Lsrc, false); a.vex(2, 1, 0, Lsrc, 0x2D, d, m, 0, &g.tmp(g.omem())); g.release(m); }
        else if (mem) g.load(d, g.omem(), Lsrc);
        else { Opd x = g.chunk(rm_reg_of(in), 0); if (x.reg) g.movrr(d, x.r, Lsrc); else g.load(d, x.m, Lsrc); }
    }
    int res = g.scratch();
    if (kind == 3) {
        a.vex(1, 1, 0, Lsrc, 0x5A, res, 0, d, nullptr);                         // vcvtpd2ps
    } else {
        int tr = g.scratch(), t = g.scratch(), lo = g.scratch();
        Mem two52 = g.tls(TOFF(c_4330));
        a.vex(3, 1, 0, Lsrc, 0x09, tr, 0, d, nullptr, 0x0B);                     // vroundpd trunc
        if (kind == 2) {
            a.vex(1, 1, 0, Lsrc, 0x58, res, tr, 0, &two52);                      // + 2^52
            a.vex(1, 1, 0, Lsrc, 0xEF, res, res, 0, &two52);                     // integer bits
        } else {
            a.vex(1, 1, 0, Lsrc, 0x59, t, tr, 0, &g.tmp(g.tls(TOFF(c_2pm32d)))); // hi = trunc(tr / 2^32)
            a.vex(3, 1, 0, Lsrc, 0x09, t, 0, t, nullptr, 0x0B);
            a.vex(1, 1, 0, Lsrc, 0x59, lo, t, 0, &g.tmp(g.tls(TOFF(c_2p32dv))));
            a.vex(1, 1, 0, Lsrc, 0x5C, lo, tr, lo, nullptr);                     // lo = tr - hi*2^32
            a.vex(1, 1, 0, Lsrc, 0x58, t, t, 0, &two52); a.vex(1, 1, 0, Lsrc, 0xEF, t, t, 0, &two52);
            a.vex(1, 1, 0, Lsrc, 0x58, lo, lo, 0, &two52); a.vex(1, 1, 0, Lsrc, 0xEF, lo, lo, 0, &two52);
            a.vex(1, 1, 0, Lsrc, 0x73, 6, res, t, nullptr, 32);                  // hi << 32
            a.vex(1, 1, 0, Lsrc, 0xEB, res, res, lo, nullptr);
        }
        // invalid (NaN, x <= -1, x >= limit) -> all ones
        a.vex(1, 1, 0, Lsrc, 0xC2, t, d, 0, &g.tmp(g.tls(TOFF(c_m1d))), 0x1E);   // x > -1
        a.vex(1, 1, 0, Lsrc, 0xC2, lo, d, 0, &g.tmp(g.tls(kind == 2 ? TOFF(c_2p32dv) : TOFF(c_2p64d))), 0x11); // x < limit
        a.vex(1, 1, 0, Lsrc, 0xDB, t, t, lo, nullptr);
        a.vex(1, 1, 0, Lsrc, 0xDF, t, t, 0, &g.tmp(g.tls(TOFF(c_ones))));
        a.vex(1, 1, 0, Lsrc, 0xEB, res, res, t, nullptr);
        if (kind == 2) {                                                         // pack low dwords
            if (Lsrc) { g.load(t, g.tls(TOFF(c_pack02)), 1); a.vex(2, 1, 0, 1, 0x36, res, t, res, nullptr); }
            else { a.vex(1, 1, 0, 0, 0x70, res, 0, res, nullptr, 0x08); a.vex(1, 2, 0, 0, 0x7E, res, 0, res, nullptr); }
        }
    }
    g.apply_mask(res, dst, 0, esd, Ld);
    g.put(dst, 0, res, Ld);
    g.zero_tail(dst, Ld ? 32 : 16);
    return true;
}

// ---- vcvtuqq2ps (F2 7A W1) / vcvtqq2ps (np 5B W1): qword -> float has no AVX2 form; one element at a time
// through a GPR with vcvtsi2ss (MXCSR rounding). Unsigned >= 2^63: halve keeping the sticky bit, convert, double
// (one rounding, as in gen_cvtusi2). Results collect in xbuf+64 and load as one vector.
static bool gen_cvtqq2ps(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;          // embedded rounding
    bool mem = in.mod != 3;
    if (mem && in.aaa) return false;                 // fault suppression on masked-off elements
    bool uns = in.pp == 3;
    int vl = g.vl, n = vl / 8, dst = reg_of(in), Ld = vl == 64 ? 1 : 0;
    if (uns) g.save_flags();
    int t = g.scratch(), res = g.scratch();
    if (!mem) {                                       // register source -> xbuf[0, vl)
        int nch = vl == 64 ? 2 : 1, L = vl >= 32 ? 1 : 0;
        for (int c = 0; c < nch; c++) {
            Opd x = g.chunk(rm_reg_of(in), c);
            if (x.reg) g.store(g.tls(TOFF(xbuf) + 32 * c), x.r, L);
            else { g.load(t, x.m, L); g.store(g.tls(TOFF(xbuf) + 32 * c), t, L); }
        }
    }
    int gs = g.gscratch(), g2 = uns ? g.gscratch() : -1;
    for (int i = 0; i < n; i++) {
        a.mov_load64(gs, mem ? g.omem(in.b ? 0 : 8 * i) : g.tls(TOFF(xbuf) + 8 * i));
        if (!uns) a.vex(1, 2, 1, 0, 0x2A, t, t, gs, nullptr);                         // vcvtsi2ss t, t, r64
        else {
            a.test_rr(1, gs, gs); int j = a.jcc_fwd(8);                                  // js: >= 2^63
            a.vex(1, 2, 1, 0, 0x2A, t, t, gs, nullptr); int j2 = a.jmp_fwd();
            a.bind(j);
            a.mov_rr64(g2, gs); a.shift_ri(5, 1, gs, 1); a.alu_ri(4, 1, g2, 1); a.alu_rr(0x08, 1, gs, g2);
            a.vex(1, 2, 1, 0, 0x2A, t, t, gs, nullptr);
            a.vex(1, 2, 0, 0, 0x58, t, t, t, nullptr);                                   // * 2 (exact)
            a.bind(j2);
        }
        a.vex(1, 1, 0, 0, 0x7E, t, 0, 0, &g.tmp(g.tls(TOFF(xbuf) + 64 + 4 * i)));       // vmovd m32 <- t
    }
    if (n == 2) a.vex(1, 2, 0, 0, 0x7E, res, 0, 0, &g.tmp(g.tls(TOFF(xbuf) + 64)));    // vmovq: 2 floats, rest 0
    else g.load(res, g.tls(TOFF(xbuf) + 64), Ld);
    g.apply_mask(res, dst, 0, 4, Ld);
    if (n == 2 && in.aaa) a.vex(1, 2, 0, 0, 0x7E, res, 0, res, nullptr);              // vmovq: lanes 2..3 stay 0
    g.put(dst, 0, res, Ld);
    g.zero_tail(dst, Ld ? 32 : 16);
    return true;
}

// ---- vmovdqu8 / vmovdqu16 (F2 6F/7F) with a mask on memory. A full mask is one vector move; otherwise the
// selected elements move one at a time through a GPR (fault suppression, no read-modify-write of other bytes).
// The register value passes through xbuf. All scratch registers are taken before the first branch.
static bool gen_movdqu8m(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b || in.adsize || in.seg) return false;
    int es = in.W ? 2 : 1, vl = g.vl, n = vl / es, nch = vl == 64 ? 2 : 1, L = vl >= 32 ? 1 : 0;
    bool store = in.opc == 0x7f;
    int r = reg_of(in);
    g.save_flags();
    int v = g.scratch();
    int gm = g.gscratch(), ga = g.gscratch(), gi = g.gscratch(), gv = g.gscratch();
    Mem xb = g.tls(TOFF(xbuf));
    for (int c = 0; c < nch; c++) {                   // xbuf <- register (store) / merge base (load)
        Opd x = (!store && in.z) ? M_(g.tls(TOFF(c_zero) + 32 * c)) : g.chunk(r, c);
        if (x.reg) g.store(xb.plus(32 * c), x.r, L);
        else { g.load(v, x.m, L); g.store(xb.plus(32 * c), v, L); }
    }
    a.mov_load64(gm, g.kreg(in.aaa));
    if (n == 32) a.mov_rr32(gm, gm); else if (n < 32) a.alu_ri(4, 1, gm, int32_t((1u << n) - 1));
    if (n == 64) a.alu_ri(7, 1, gm, -1); else if (n == 32) a.alu_ri(7, 0, gm, -1); else a.alu_ri(7, 1, gm, int32_t((1u << n) - 1));
    int jpart = a.jcc_fwd(5);                         // jne: not every element selected
    for (int c = 0; c < nch; c++) {
        if (store) { g.load(v, xb.plus(32 * c), L); a.vex(1, 0, 0, L, 0x11, v, 0, 0, &g.tmp(g.omem(32 * c))); }  // vmovups
        else { g.load(v, g.omem(32 * c), L); g.store(xb.plus(32 * c), v, L); }
    }
    int jdone = a.jmp_fwd();
    a.bind(jpart);
    a.lea(ga, g.omem());
    int top = a.n;
    a.test_rr(1, gm, gm); int jend = a.jcc_fwd(4);    // jz
    a.leg(0, 1, 1, 0xBC, gi, gm, nullptr);            // bsf gi, gm
    Mem mm; mm.base = ga; mm.index = gi; mm.scale = es == 2 ? 1 : 0; mm.disp = 0;
    Mem xm = xb; xm.base = -1; xm.index = gi; xm.scale = mm.scale;
    if (store) { if (es == 1) { a.movzx8(gv, xm); a.mov_store8(mm, gv); } else { a.movzx16(gv, xm); a.mov_store16(mm, gv); } }
    else { if (es == 1) { a.movzx8(gv, mm); a.mov_store8(xm, gv); } else { a.movzx16(gv, mm); a.mov_store16(xm, gv); } }
    a.leg(0, 1, 1, 0xB3, gi, gm, nullptr);            // btr gm, gi
    a.b(0xE9); a.d32(uint32_t(top - (a.n + 4)));      // jmp top
    a.bind(jend);
    a.bind(jdone);
    if (!store) {
        for (int c = 0; c < nch; c++) { g.load(v, xb.plus(32 * c), L); g.put(r, c, v, L); }
        g.zero_tail(r, vl);
    }
    return true;
}

// ---- vpsraq imm (66 72 /4 W1), by xmm count (66 E2 W1), vpsravq (66 0F38 46 W1):
// sra(x, n) = srl(x ^ s, n) ^ s with s = the sign mask of x (counts >= 64 give s, as AVX-512 does)
static bool gen_psraq(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    int kind = in.map == 2 ? 2 : in.opc == 0x72 ? 0 : 1;   // 0 imm (dst vvvv, x rm), 1 xmm count, 2 variable
    bool mem = in.mod != 3;
    if (kind == 1 && in.b) return false;
    int dst = kind == 0 ? in.vvvv : reg_of(in);
    int vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int bcst = -1;
    if (in.b && mem) {
        if (in.aaa) bcst = g.bcast_masked(8, L, vl / 8);
        else { bcst = g.scratch(); if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem())); else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem())); }
    }
    auto rmop = [&](int c) -> Opd {                    // the rm vector operand (x for kind 0, counts for kind 2)
        if (bcst >= 0) return R_(bcst);
        if (!mem) return g.chunk(rm_reg_of(in), c);
        if (in.aaa) { int m = g.maskvec(8, c, L, false), s = g.scratch(); a.vex(2, 1, 0, L, 0x2D, s, m, 0, &g.tmp(g.omem(32 * c))); g.release(m); return R_(s); }
        return M_(g.omem(32 * c));
    };
    Opd cnt;
    if (kind == 1) cnt = mem ? M_(g.omem()) : g.chunk(rm_reg_of(in), 0);
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        int x = g.in_reg(kind == 0 ? rmop(c) : g.chunk(in.vvvv, c), L);
        int s = g.scratch(), t = g.scratch();
        g.vx(1, 1, 0, L, 0x72, 4, t, R_(x), 31);                   // vpsrad t = dword signs
        a.vex(1, 1, 0, L, 0x70, s, 0, t, nullptr, 0xF5);              // vpshufd: qword sign masks
        a.vex(1, 1, 0, L, 0xEF, t, x, s, nullptr);                    // t = x ^ s
        if (kind == 0) g.vx(1, 1, 0, L, 0x73, 2, t, R_(t), in.imm8);  // vpsrlq imm
        else if (kind == 1) g.vx(1, 1, 0, L, 0xD3, t, t, cnt);        // vpsrlq by xmm
        else g.vx(2, 1, 1, L, 0x45, t, t, rmop(c));                   // vpsrlvq
        a.vex(1, 1, 0, L, 0xEF, s, t, s, nullptr);                    // ^ s
        g.apply_mask(s, dst, c, 8, L);
        g.put(dst, c, s, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- vpopcntb/w (66 0F38 54 W0/W1), vpopcntd/q (55 W0/W1): nibble table (vpshufb), then byte counts summed
// per word (vpmaddubsw), dword (+ vpmaddwd) or qword (vpsadbw)
static bool gen_popcnt(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    if (in.b && in.mod == 3) return false;
    int es = in.opc == 0x54 ? (in.W ? 2 : 1) : (in.W ? 8 : 4);
    bool mem = in.mod != 3;
    if (mem && in.aaa && es < 4 && !in.b) return false;   // fault suppression with byte/word elements
    if (in.b && es < 4) return false;
    int dst = reg_of(in), vl = g.vl, L = vl >= 32 ? 1 : 0, nch = vl == 64 ? 2 : 1;
    int bcst = -1;
    if (in.b && mem) {
        if (in.aaa) bcst = g.bcast_masked(es, L, vl / es);
        else {
            bcst = g.scratch();
            if (es == 4) a.vex(2, 1, 0, L, 0x18, bcst, 0, 0, &g.tmp(g.omem()));
            else if (L) a.vex(2, 1, 0, 1, 0x19, bcst, 0, 0, &g.tmp(g.omem()));
            else a.vex(1, 3, 0, 0, 0x12, bcst, 0, 0, &g.tmp(g.omem()));
        }
    }
    for (int c = 0; c < nch; c++) {
        uint32_t before = g.vtaken;
        Opd xs;
        if (bcst >= 0) xs = R_(bcst);
        else if (!mem) xs = g.chunk(rm_reg_of(in), c);
        else if (in.aaa) { int m = g.maskvec(es, c, L, false), s = g.scratch(); a.vex(2, 1, 0, L, es == 8 ? 0x2D : 0x2C, s, m, 0, &g.tmp(g.omem(32 * c))); g.release(m); xs = R_(s); }
        else xs = M_(g.omem(32 * c));
        int x = g.in_reg(xs, L);
        int lut = g.scratch(), lo = g.scratch(), hi = g.scratch();
        g.load(lut, g.tls(TOFF(c_popnib)), L);
        a.vex(1, 1, 0, L, 0xDB, lo, x, 0, &g.tmp(g.tls(TOFF(c_b0f))));     // lo nibbles
        g.vx(1, 1, 0, L, 0x71, 2, hi, R_(x), 4);                            // vpsrlw 4
        a.vex(1, 1, 0, L, 0xDB, hi, hi, 0, &g.tmp(g.tls(TOFF(c_b0f))));    // hi nibbles
        a.vex(2, 1, 0, L, 0x00, lo, lut, lo, nullptr);                      // vpshufb
        a.vex(2, 1, 0, L, 0x00, hi, lut, hi, nullptr);
        a.vex(1, 1, 0, L, 0xFC, lo, lo, hi, nullptr);                       // vpaddb: byte counts
        if (es == 2 || es == 4) a.vex(2, 1, 0, L, 0x04, lo, lo, 0, &g.tmp(g.tls(TOFF(c_b01))));  // vpmaddubsw: word sums
        if (es == 4) a.vex(1, 1, 0, L, 0xF5, lo, lo, 0, &g.tmp(g.tls(TOFF(c_w01))));              // vpmaddwd: dword sums
        if (es == 8) a.vex(1, 1, 0, L, 0xF6, lo, lo, 0, &g.tmp(g.tls(TOFF(c_zero))));             // vpsadbw: qword sums
        g.apply_mask(lo, dst, c, es, L);
        g.put(dst, c, lo, L);
        g.vtaken = before | (bcst >= 0 ? 1u << bcst : 0);
    }
    g.zero_tail(dst, vl);
    return true;
}

// ---- vpmov{,us,s}{wb,db,qb,dw,qw} (F3 0F38 30-34 / 10-14 / 20-24), xmm/ymm source, no mask: saturating forms
// clamp first (no qword min/max in AVX2: those stay interpreted); vpshufb packs each 128-bit lane's low parts,
// an unpack of the per-lane result size appends lane 1; register destination (rest 0) or an exact-size store
static bool gen_pmovtrunc(G& g) {
    const Insn& in = g.in; Asm& a = g.a;
    int kind = in.opc & 15, sat = in.opc >> 4;      // kind 0 wb 1 db 2 qb 3 dw 4 qw; sat 1 unsigned, 2 signed, 3 none
    static const int ess[5] = {2, 4, 8, 4, 8}, esd[5] = {1, 1, 1, 2, 2};
    if (kind > 4 || in.aaa || in.b || g.vl == 64 || in.W) return false;
    int es = ess[kind], ed = esd[kind];
    if (sat != 3 && es == 8) return false;
    int L = g.vl == 32 ? 1 : 0, k = 16 / es * ed, nbytes = k * (L ? 2 : 1);
    bool mem = in.mod != 3;
    int x = g.scratch();
    { Opd s = g.chunk(reg_of(in), 0); if (s.reg) g.movrr(x, s.r, L); else g.load(x, s.m, L); }
    if (sat != 3) {
        int c = g.scratch(), bits = ed * 8, sh = es == 4 ? 0x72 : 0x71;
        a.vex(1, 1, 0, L, es == 4 ? 0x76 : 0x75, c, c, c, nullptr);           // all ones
        if (sat == 1) {                                                         // unsigned: min(x, 2^bits - 1)
            g.vx(1, 1, 0, L, sh, 2, c, R_(c), es * 8 - bits);
            a.vex(2, 1, 0, L, es == 4 ? 0x3B : 0x3A, x, x, c, nullptr);         // vpminud / vpminuw
        } else {                                                                // signed: clamp to [-2^(bits-1), 2^(bits-1) - 1]
            int c2 = g.scratch(); g.movrr(c2, c, L);
            g.vx(1, 1, 0, L, sh, 2, c, R_(c), es * 8 - bits + 1);
            g.vx(1, 1, 0, L, sh, 6, c2, R_(c2), bits - 1);
            if (es == 4) { a.vex(2, 1, 0, L, 0x39, x, x, c, nullptr); a.vex(2, 1, 0, L, 0x3D, x, x, c2, nullptr); }  // vpminsd, vpmaxsd
            else { a.vex(1, 1, 0, L, 0xEA, x, x, c, nullptr); a.vex(1, 1, 0, L, 0xEE, x, x, c2, nullptr); }         // vpminsw, vpmaxsw
            g.release(c2);
        }
        g.release(c);
    }
    a.vex(2, 1, 0, L, 0x00, x, x, 0, &g.tmp(g.tls(TOFF(c_trunc) + 32 * kind)));   // vpshufb: low parts to the lane start
    if (L) {
        int t = g.scratch();
        a.vex(3, 1, 0, 1, 0x39, x, 0, t, nullptr, 1);                             // vextracti128 t = lane 1
        a.vex(1, 1, 0, 0, k == 8 ? 0x6C : k == 4 ? 0x62 : 0x61, x, x, t, nullptr);  // vpunpckl{qdq,dq,wd}
        g.release(t);
    }
    if (!mem) { int d = rm_reg_of(in); g.put(d, 0, x, 0); g.zero_tail(d, 16); return true; }
    if (nbytes == 16) a.vex(1, 0, 0, 0, 0x11, x, 0, 0, &g.tmp(g.omem()));          // vmovups m128
    else if (nbytes == 8) a.vex(1, 1, 0, 0, 0xD6, x, 0, 0, &g.tmp(g.omem()));      // vmovq m64
    else if (nbytes == 4) a.vex(1, 1, 0, 0, 0x7E, x, 0, 0, &g.tmp(g.omem()));      // vmovd m32
    else a.vex(3, 1, 0, 0, 0x15, x, 0, 0, &g.tmp(g.omem()), 0);                     // vpextrw m16
    return true;
}

} // namespace

// AVX512EMU_NOGEN=bits: turn off newer inline generators (diagnosis): 1 scatter, 2 vmovlps/hps store, 4 vinsert/vextract
// 128, 8 byte permutes, 16 vpmovqd, 32 vpmovw2m, 64 vpmullq, 128 vpermt2/i2 q/pd, 256 vcvtqq2pd, 512 vpmovb2m/d2m/q2m,
// 1024 vpmovdw, 2048 vcvt(u)qq2ps, 4096 masked vmovdqu8/16 memory, 8192 vpsraq, 16384 vpopcnt, 32768 vperm{,t2,i2}w,
// 65536 vpmov{,s,us}{wb,db,qb,dw,qw}
static int nogen() { static int v = -1; if (v < 0) { const char* e = getenv("AVX512EMU_NOGEN"); v = e ? atoi(e) : 0; } return v; }
bool xlate_inline(Asm& a, const Desc& d, const XlateOpts& o) {
    const Insn& in = d.in;
    int start = a.n;
    uint32_t mk0 = o.mc_key; o.mc_busy = false; o.mc_wrote_k = false;
    G g(a, d, o);
    bool ok = false;
    if (in.enc == VEX) ok = gen_kreg(g);
    else if (in.enc == EVEX) {
        if (in.map == 1 && (in.opc == 0x10 || in.opc == 0x11) && in.pp >= 2) ok = (in.pp == 2 ? !in.W : in.W) && gen_movs(g, in.pp == 2 ? 4 : 8);
        else if ((in.map == 1 && (in.opc == 0xC2 || in.opc == 0x76 || in.opc == 0x66 || in.opc == 0x74 || in.opc == 0x64 || ((in.opc == 0x75 || in.opc == 0x65) && !(nogen() & 524288)))) ||
                 (in.map == 3 && in.pp == 1 && (in.opc == 0x3e || in.opc == 0x3f) && !(nogen() & 524288)) ||
                 (in.map == 2 && in.pp == 1 && (in.opc == 0x29 || in.opc == 0x37)) || (in.map == 3 && in.pp == 1 && (in.opc == 0x1e || in.opc == 0x1f)))
            ok = gen_cmpk(g);
        else if (in.map == 2 && (in.pp == 1 || in.pp == 2) && (in.opc == 0x26 || in.opc == 0x27)) ok = gen_cmpk(g);
        else if (in.map == 1 && in.opc == 0x78 && in.pp >= 2) ok = gen_cvt2usi(g);
        else if (in.map == 1 && in.opc == 0x7b && in.pp >= 2) ok = gen_cvtusi2(g);
        else if (in.map == 2 && in.pp == 1 && in.opc >= 0x7a && in.opc <= 0x7c && in.mod == 3) ok = gen_bcgpr(g);
        else if (in.map == 3 && in.pp == 1 && in.opc == 0x25) ok = gen_ternlog(g);
        else if (in.map == 2 && in.pp == 1 && (in.opc == 0x76 || in.opc == 0x77 || in.opc == 0x7e || in.opc == 0x7f) && !(in.W && (nogen() & 128))) ok = gen_perm2(g);
        else if (in.map == 2 && in.pp == 1 && in.opc >= 0x90 && in.opc <= 0x93 && in.mod != 3) ok = gen_gather(g);
        else if (in.map == 2 && in.pp == 1 && in.opc >= 0x64 && in.opc <= 0x66) ok = gen_blendm(g);
        else if (in.map == 2 && in.pp == 1 && in.opc == 0x40 && in.W && !(nogen() & 64)) ok = gen_pmullq(g);
        else if (in.map == 2 && in.pp == 1 && (in.opc == 0x8D || in.opc == 0x7D || in.opc == 0x75) && g.vl != 64 && !in.b && !(in.mod != 3 && in.aaa) && !(nogen() & (in.W ? 32768 : 8))) ok = gen_permbytes(g);
        else if (in.map == 2 && in.pp == 2 && in.opc == 0x35 && !in.W && !in.aaa && g.vl != 64 && !(nogen() & 16)) ok = gen_pmovqd(g);
        else if (in.map == 2 && in.pp == 2 && in.opc == 0x29 && in.W && in.mod == 3 && g.vl != 64 && !(nogen() & 32)) ok = gen_pmovw2m(g);
        else if (in.map == 2 && in.pp == 2 && (in.opc == 0x39 || (in.opc == 0x29 && !in.W)) && in.mod == 3 && g.vl != 64 && !(nogen() & 512)) ok = gen_pmovw2m(g);
        else if (in.map == 2 && in.pp == 2 && in.opc == 0x33 && !in.W && in.mod == 3 && !in.aaa && g.vl != 64 && !(nogen() & 1024)) ok = gen_pmovdw(g);
        else if (in.map == 2 && in.pp == 2 && !in.W && (in.opc & 15) <= 4 && (in.opc >> 4) >= 1 && (in.opc >> 4) <= 3 && !(nogen() & 65536)) ok = gen_pmovtrunc(g);
        else if (in.map == 2 && in.pp == 1 && in.opc >= 0xA0 && in.opc <= 0xA3 && in.mod != 3 && in.aaa && g.vl != 64 && in.has_sib && !in.seg && !in.adsize && !(nogen() & 1)) ok = gen_scatter(g);
        else if (in.map == 1 && (in.opc == 0x13 || in.opc == 0x17) && in.pp <= 1 && in.mod != 3 && !in.aaa && !in.b && !(nogen() & 2)) ok = gen_movlhst(g);
        else if (in.map == 3 && in.pp == 1 && (in.opc == 0x18 || in.opc == 0x19 || in.opc == 0x38 || in.opc == 0x39) && g.vl == 32 && !in.b && !(in.mod != 3 && in.aaa) && !(nogen() & 4)) ok = gen_insext128(g);
        else if (in.map == 2 && in.pp == 2 && (in.opc == 0x28 || in.opc == 0x38)) ok = gen_movm2(g);
        else if (in.map == 1 && ((in.pp == 2 && in.opc == 0x7a && in.W) || (in.pp == 0 && in.opc == 0x78 && !in.W))) ok = gen_cvt_custom(g);
        else if (in.map == 1 && in.pp == 2 && in.opc == 0xE6 && in.W && !(nogen() & 256)) ok = gen_cvt_custom(g);
        else if (in.map == 1 && ((in.pp == 1 && in.opc == 0x78) || (in.pp == 0 && in.opc == 0x78 && in.W) || (in.pp == 1 && in.opc == 0x5a && in.W))) ok = gen_cvt_wide(g);
        else if (in.map == 1 && in.W && in.opc == (in.pp == 3 ? 0x7a : 0x5b) && (in.pp == 3 || in.pp == 0) && !(nogen() & 2048)) ok = gen_cvtqq2ps(g);
        else if (in.map == 1 && in.pp == 3 && (in.opc == 0x6f || in.opc == 0x7f) && in.mod != 3 && in.aaa && !(nogen() & 4096)) ok = gen_movdqu8m(g);
        else if (in.W && in.pp == 1 && ((in.map == 1 && ((in.opc == 0x72 && in.reg3 == 4) || in.opc == 0xe2)) || (in.map == 2 && in.opc == 0x46)) && !(nogen() & 8192)) ok = gen_psraq(g);
        else if (in.map == 2 && in.pp == 1 && (in.opc == 0x54 || in.opc == 0x55) && !(nogen() & 16384)) ok = gen_popcnt(g);
        else if (in.map == 3 && in.pp == 1 && in.opc == 0x03 && !(nogen() & 262144)) ok = gen_valign(g);
        else if (in.map == 2 && in.pp == 1 && in.opc == 0x89 && !in.W && in.mod == 3 && !(nogen() & 16777216)) ok = gen_expandd(g);
        else if (in.map == 2 && in.pp == 1 && (in.opc == 0x52 || in.opc == 0x71) && !(nogen() & 8388608)) ok = gen_dpshld(g);
        else if (in.map == 3 && in.pp == 1 && !in.b && ((in.opc & 0xde) == 0x1a || (g.vl == 64 && ((in.opc & 0xde) == 0x18))) && !(nogen() & 2097152)) ok = gen_insext512(g);
        else if (in.map == 3 && in.pp == 1 && (in.opc == 0x23 || in.opc == 0x43) && !(nogen() & 4194304)) ok = gen_shuf128(g);
        else if (in.pp == 1 && ((in.map == 3 && (in.opc == 0x20 || in.opc == 0x22)) || (in.map == 1 && in.opc == 0xC4)) && !(nogen() & 1048576)) ok = gen_pinsr(g);
        else if (in.map == 2 && in.pp == 1 && in.W && (in.opc == 0x3D || in.opc == 0x39 || in.opc == 0x3F || in.opc == 0x3B || in.opc == 0x1F) && !(nogen() & 131072)) ok = gen_minmaxq(g);
        else { VOp v; if (lookup(in, v)) ok = gen_vexable(g, v); }
        if (!ok) { a.n = start; o.mc_key = mk0; o.mc_busy = false; o.mc_wrote_k = false; G g2(a, d, o); if (gen_simple(g2) && !g2.fail) { g2.finish(); ok = !a.overflow; } if (!ok) a.n = start; }
    }
    if (ok && !g.fail) { g.finish(); if (a.overflow) ok = false; } else ok = false;
    if (!ok) { a.n = start; o.mc_key = mk0; o.mc_wrote_k = false; } // discard partial output (and what it put in the mask cache)
    o.mc_busy = false;
    return ok;
}

uint32_t mask_key(int k, int es, int c, int L, bool inv) { return mask_key_(k, es, c, L, inv); }
void emit_maskvec_key(Asm& a, int reg, uint32_t key) { emit_maskvec_key_(a, reg, key); }

} // namespace emu
