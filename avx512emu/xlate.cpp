#include "xlate.h"
#include "evexinfo.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

namespace emu {

using namespace x86;

void emit_interp_call(Asm& a, const Desc& d, const XlateOpts& o) {
    Mem m;
    a.lea(RSP, Mem{RSP, -1, 0, -128});
    a.b(0x50);                                         // push %rax
    m = Mem(); m.rip = true; m.target = (uint64_t)&d;
    a.lea(RAX, m);                                     // lea desc(%rip),%rax
    m = Mem(); m.rip = true; m.target = o.thunk_slot;
    a.leg(0, 0, 0, 0xFF, 2, 0, &m);                    // call *slot(%rip)
    a.b(0x58);                                         // pop %rax
    a.lea(RSP, Mem{RSP, -1, 0, 128});
}

bool xlate_inline(Asm& a, const Desc& d, const XlateOpts& o);
uint64_t g_interp_sites;   // translations that call the interpreter (statistics)

bool xlate_insn(Asm& a, const Desc& d, const XlateOpts& o, const char** why) {
    uint32_t mk = o.mc_key;
    if (!o.no_inline && xlate_inline(a, d, o)) return true;
    o.mc_key = mk; o.mc_busy = false; o.mc_wrote_k = false;   // the failed inline attempt's mask cache use was discarded with its code
    if (!o.thunk_slot) { *why = "no thunk"; return false; }
    g_interp_sites++;
    { static int dbg = -1; if (dbg < 0) dbg = getenv("AVX512EMU_DEBUG_INTERP") != nullptr;
      if (dbg) { fprintf(stderr, "INTERP %lx", (unsigned long)d.pc); for (int i = 0; i < d.in.len; i++) fprintf(stderr, " %02x", d.bytes[i]);
                 int nm = 0; for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) nm++; fprintf(stderr, " mapped=%d vpre=%x vdead=%x\n", nm, o.vpre, o.vdead); } }
    int32_t T = tls_offset();
    auto zxm = [&](int i) { Mem m = Mem::fs(T + int32_t(offsetof(TState, zx)) + 64 * i); return m; };
    for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) { Mem m = zxm(i); a.vex(1, 2, 0, 1, 0x7F, o.xmap[i], 0, 0, &m); }  // flush for the interpreter
    emit_interp_call(a, d, o);
    for (int i = 0; i < 16; i++) if (o.xmap[i] >= 0) { Mem m = zxm(i); a.vex(1, 2, 0, 1, 0x6F, o.xmap[i], 0, 0, &m); }
    return true;
}

} // namespace emu
