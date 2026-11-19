#include "interp.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace emu {

void IC::rm_src(Z& out, int bytes, int es, bool suppress) const {
    memset(&out, 0, sizeof out);
    if (!mem) { memcpy(out.b, V[rmreg()].b, bytes); return; }
    if (in.b) { // broadcast one element (not read when every lane is masked off: fault suppression)
        if (suppress && in.aaa) { bool any = false; for (int i = 0; i < bytes / es && !any; i++) any = mbit(i); if (!any) return; }
        uint8_t e[8]; memcpy(e, (const void*)ea, es);
        for (int i = 0; i < bytes / es; i++) memcpy(out.b + i * es, e, es);
        return;
    }
    if (!suppress || in.aaa == 0) { memcpy(out.b, (const void*)ea, bytes); return; }
    for (int i = 0; i < bytes / es; i++) if (mbit(i)) memcpy(out.b + i * es, (const uint8_t*)ea + i * es, es);
}

void IC::write(int r, const Z& val, int es, int bytes, bool maskable) {
    Z t; memset(&t, 0, sizeof t);
    const Z& old = V[r];
    for (int i = 0; i < bytes / es; i++) {
        if (!maskable || mbit(i)) memcpy(t.b + i * es, val.b + i * es, es);
        else if (!in.z) memcpy(t.b + i * es, old.b + i * es, es);
    }
    V[r] = t;
}

void IC::store_masked(const Z& val, int es, int bytes) {
    for (int i = 0; i < bytes / es; i++) if (mbit(i)) memcpy((uint8_t*)ea + i * es, val.b + i * es, es);
}

RcScope::RcScope(int rc) : old(0), on(rc >= 0) {
    if (on) { old = _mm_getcsr(); _mm_setcsr((old & ~0x6000u) | (uint32_t(rc) << 13)); }
}
RcScope::~RcScope() { if (on) _mm_setcsr(old); }

static void fail(const Desc* d, const char* why) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "avx512emu: %s at %#lx:", why, (unsigned long)desc_pc(d));
    for (int i = 0; i < d->in.len && n < 240; i++) n += snprintf(buf + n, sizeof buf - n, " %02x", d->bytes[i]);
    buf[n++] = '\n';
    ssize_t w = write(2, buf, n); (void)w;
    abort();
}

// Runs one instruction on the full state. Exposed for the test harness.
bool interp_run(IC& c) {
    return fam_mov(c) || fam_fp(c) || fam_int(c) || fam_shift(c) || fam_shuf(c) || fam_cvt(c) ||
           fam_cmp(c) || fam_gather(c) || fam_misc(c) || fam_kreg(c);
}

void interp_setup(IC& c, const Desc* d) {
    const x86::Insn& in = d->in;
    c.pc = desc_pc(d);
    c.mem = in.has_modrm && in.mod != 3;
    if (in.enc == x86::EVEX) c.vl = (in.b && in.mod == 3) ? 64 : (16 << (in.L > 2 ? 2 : in.L));
    else c.vl = in.L ? 32 : 16;
    if (c.mem) {
        uint64_t a = 0;
        int32_t disp = x86::mem_disp(in);
        if (in.riprel) a = c.pc + in.len + int64_t(disp);
        else {
            if (x86::has_base(in)) a += c.gpr(x86::base_of(in));
            bool vsib = in.enc == x86::EVEX && in.map == 2 && ((in.opc >= 0x90 && in.opc <= 0x93) || (in.opc >= 0xA0 && in.opc <= 0xA3));
            if (x86::has_index(in) && !vsib) a += c.gpr(x86::index_of(in)) << x86::scale_of(in);
            if (vsib && in.has_sib) {} // per element, in fam_gather
            a += int64_t(disp);
        }
        if (in.adsize) a = uint32_t(a);
        if (in.seg == 0x64) { uint64_t fsb; __asm__("mov %%fs:0, %0" : "=r"(fsb)); a += fsb; }
        c.ea = a;
    }
}

} // namespace emu

namespace emu { __thread Z interp_V[32] __attribute__((tls_model("initial-exec"))); }
using namespace emu;

namespace emu { __attribute__((weak)) void profile_tick() {} }
namespace emu { __attribute__((weak)) void capture_group(Frame*, const Desc*) {} }   // runtime.cpp has the real one
extern "C" void emu_interp(Frame* f, const Desc* d) {
    if (__builtin_expect(d->flags & DF_CAPTURE, 0)) { capture_group(f, d); return; }
    TState& s = tst;
    if (!(d->flags & DF_XLREL)) ((Desc*)d)->count++;   // offline segment pages stay shared
    static __thread uint32_t ticks __attribute__((tls_model("initial-exec")));
    if (__builtin_expect(++ticks >= (1u << 22), 0)) { ticks = 0; profile_tick(); }
    s.depth++;
    Z* V = interp_V;
    for (int r = 0; r < 16; r++) { memcpy(V[r].b, s.hw[r], 32); memcpy(V[r].b + 32, s.zhi[r], 32); }
    for (int r = 0; r < 16; r++) memcpy(V[16 + r].b, s.zx[r], 64);
    IC c(d->in);
    c.V = V; c.K = s.k;
    uint64_t* g = &f->rcx;
    c.G[0] = &f->rax; c.G[1] = g + 0; c.G[2] = g + 1; c.G[3] = g + 2; c.G[4] = nullptr; c.G[5] = g + 3; c.G[6] = g + 4; c.G[7] = g + 5;
    for (int r = 8; r < 16; r++) c.G[r] = g + 6 + (r - 8);
    c.rsp = uint64_t(f) + 264;
    c.rflags = &f->rflags;
    interp_setup(c, d);
    if (!interp_run(c)) fail(d, "unsupported instruction");
    for (int r = 0; r < 16; r++) { memcpy(s.hw[r], V[r].b, 32); memcpy(s.zhi[r], V[r].b + 32, 32); }
    for (int r = 0; r < 16; r++) memcpy(s.zx[r], V[16 + r].b, 64);
    s.depth--;
}
