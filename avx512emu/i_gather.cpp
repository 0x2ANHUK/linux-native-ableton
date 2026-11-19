// Gathers and scatters (VSIB addressing, opmask consumed).
#include "interp.h"

namespace emu {

bool fam_gather(IC& c) {
    const x86::Insn& in = c.in;
    if (in.enc != x86::EVEX || in.map != 2 || in.pp != 1 || !c.mem || !in.has_sib) return false;
    bool gather = in.opc >= 0x90 && in.opc <= 0x93, scatter = in.opc >= 0xa0 && in.opc <= 0xa3;
    if (!gather && !scatter) return false;
    const int des = in.W ? 8 : 4, ies = (in.opc & 1) ? 8 : 4;
    const int n = c.vl / (des > ies ? des : ies);
    const int vidx = ((in.sib >> 3) & 7) | (in.X << 3) | (in.Vp << 4);
    const int sc = x86::scale_of(in);
    const Z idx = c.R(vidx);
    uint64_t& k = c.K[in.aaa];
    auto addr = [&](int i) -> uint64_t {
        int64_t v = ies == 8 ? idx.sq[i] : int64_t(idx.sd[i]);
        uint64_t a = c.ea + (uint64_t(v) << sc);
        return in.adsize ? uint32_t(a) : a;
    };
    if (gather) {
        Z r; memset(&r, 0, sizeof r);
        memcpy(r.b, c.R(c.reg()).b, n * des);
        for (int i = 0; i < n; i++) {
            if (!((k >> i) & 1)) continue;
            c.ldn(r.b + i * des, addr(i), des);
            k &= ~(1ull << i);
        }
        k = 0;
        c.write_raw(c.reg(), r, n * des);
    } else {
        const Z d = c.R(c.reg());
        for (int i = 0; i < n; i++) {
            if (!((k >> i) & 1)) continue;
            c.stn(addr(i), d.b + i * des, des);
            k &= ~(1ull << i);
        }
        k = 0;
    }
    return true;
}

} // namespace emu
