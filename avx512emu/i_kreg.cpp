// Opmask instructions (VEX-encoded k*).
#include "interp.h"

namespace emu {

static int kwidth_logic(const x86::Insn& in) { // pp0 W0 w, pp0 W1 q, pp1 W0 b, pp1 W1 d
    if (in.pp == 0) return in.W ? 64 : 16;
    if (in.pp == 1) return in.W ? 32 : 8;
    return 0;
}
static uint64_t wmask(int bits) { return bits >= 64 ? ~0ull : ((1ull << bits) - 1); }

bool fam_kreg(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::VEX) return false;
    int kr = x86::reg_of(in) & 7, kv = in.vvvv & 7, km = in.rm3; // rm k register: only rm3 (B ignored)
    if (in.map == 1) {
        switch (in.opc) {
        case 0x41: case 0x42: case 0x45: case 0x46: case 0x47: case 0x4a: {
            int w = kwidth_logic(in); if (!w || c.mem) return false;
            uint64_t a = c.K[kv], b = c.K[km], r;
            switch (in.opc) { case 0x41: r = a & b; break; case 0x42: r = ~a & b; break; case 0x45: r = a | b; break;
                              case 0x46: r = ~(a ^ b); break; case 0x47: r = a ^ b; break; default: r = a + b; break; }
            c.K[kr] = r & wmask(w); return true; }
        case 0x44: { int w = kwidth_logic(in); if (!w || c.mem) return false; c.K[kr] = ~c.K[km] & wmask(w); return true; }
        case 0x4b: { // kunpck
            if (c.mem) return false;
            int h; if (in.pp == 1 && !in.W) h = 8; else if (in.pp == 0 && !in.W) h = 16; else if (in.pp == 0 && in.W) h = 32; else return false;
            c.K[kr] = ((c.K[kv] & wmask(h)) << h) | (c.K[km] & wmask(h)); return true; }
        case 0x90: { int w = kwidth_logic(in); if (!w) return false;
            uint64_t v = 0; if (c.mem) c.ldn(&v, c.ea, w / 8); else v = c.K[km];
            c.K[kr] = v & wmask(w); return true; }
        case 0x91: { int w = kwidth_logic(in); if (!w || !c.mem) return false; uint64_t v = c.K[kr]; c.stn(c.ea, &v, w / 8); return true; }
        case 0x92: case 0x93: {
            int w; if (in.pp == 0 && !in.W) w = 16; else if (in.pp == 1 && !in.W) w = 8; else if (in.pp == 3) w = in.W ? 64 : 32; else return false;
            if (c.mem) return false;
            int g = x86::rm_reg_of(in) & 15;
            if (in.opc == 0x92) c.K[kr] = c.gpr(g) & wmask(w);
            else { int gd = x86::reg_of(in) & 15; c.set_gpr64(gd, c.K[km] & wmask(w)); }
            return true; }
        case 0x98: case 0x99: {
            int w = kwidth_logic(in); if (!w || c.mem) return false;
            uint64_t a = c.K[kr] & wmask(w), b = c.K[km] & wmask(w), f = 0;
            if (in.opc == 0x98) { uint64_t o = a | b; if (o == 0) f |= ZF; if (o == wmask(w)) f |= CF; }
            else { if ((a & b) == 0) f |= ZF; if ((~a & b & wmask(w)) == 0) f |= CF; }
            c.set_flags(f, ZF | CF | OF | SF | AF | PF); return true; }
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1 && in.opc >= 0x30 && in.opc <= 0x33 && !c.mem) {
        int w = (in.opc & 1) ? (in.W ? 64 : 32) : (in.W ? 16 : 8);
        bool left = in.opc >= 0x32;
        int n = c.imm();
        uint64_t v = c.K[km] & wmask(w);
        uint64_t r = n >= w ? 0 : (left ? (v << n) : (v >> n));
        c.K[kr] = r & wmask(w); return true;
    }
    return false;
}

} // namespace emu
