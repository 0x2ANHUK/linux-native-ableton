// Reference interpreter for AVX-512 (EVEX) and opmask (VEX k*) instructions.
// Exact x86 semantics in C++; the slow path of the translator and the model
// the inline translations are tested against.
#pragma once
#include "state.h"
#include "evexinfo.h"
#include <string.h>

namespace emu {

struct Z { union { uint8_t b[64]; int8_t sb[64]; uint16_t w[32]; int16_t sw[32]; uint32_t d[16]; int32_t sd[16];
                   uint64_t q[8]; int64_t sq[8]; float f[16]; double g[8]; }; };

extern __thread Z interp_V[32] __attribute__((tls_model("initial-exec")));  // interpreter register file (per thread)

struct IC {
    const x86::Insn& in;
    uint64_t pc;
    Z* V;                 // zmm0-31
    uint64_t* K;          // k0-7
    uint64_t* G[16];      // GPRs
    uint64_t rsp;
    uint64_t* rflags;
    int vl;               // vector length in bytes (16/32/64)
    bool mem;             // rm is memory
    uint64_t ea;          // effective address (mem)
    IC(const x86::Insn& i) : in(i) {}

    int reg() const { return x86::reg_of(in); }
    int vvvv() const { return in.vvvv; }
    int rmreg() const { return x86::rm_reg_of(in); }
    int imm() const { return in.imm8; }
    bool masked() const { return in.aaa != 0; }
    bool mbit(int i) const { return in.aaa == 0 || ((K[in.aaa] >> i) & 1); }
    uint64_t gpr(int r) const { return r == 4 ? rsp : *G[r]; }
    void set_gpr64(int r, uint64_t v) { if (r != 4) *G[r] = v; }
    void set_gpr32(int r, uint32_t v) { if (r != 4) *G[r] = v; }

    // memory
    template <class T> T ld(uint64_t a) const { T v; memcpy(&v, (const void*)a, sizeof v); return v; }
    template <class T> void st(uint64_t a, T v) const { memcpy((void*)a, &v, sizeof v); }
    void ldn(void* d, uint64_t a, int n) const { memcpy(d, (const void*)a, n); }
    void stn(uint64_t a, const void* s, int n) const { memcpy((void*)a, s, n); }

    // rm source as `bytes` bytes (reg or memory), broadcast of element `es` when EVEX.b on memory.
    // With fault suppression: only elements whose mask bit is set are read (masked=true).
    void rm_src(Z& out, int bytes, int es, bool suppress = true) const;
    // register source (whole)
    const Z& R(int r) const { return V[r]; }
    // write dst with merge/zero masking per element of size es, n elements, zero bits above `bytes`.
    void write(int r, const Z& val, int es, int bytes, bool maskable = true);
    // write without masking (and zero above bytes)
    void write_raw(int r, const Z& val, int bytes) { Z t = val; memset(t.b + bytes, 0, 64 - bytes); V[r] = t; }
    // store `bytes` of val to memory at ea, element-masked (only enabled elements written)
    void store_masked(const Z& val, int es, int bytes);
    // flags
    void set_flags(uint64_t f, uint64_t mask) { *rflags = (*rflags & ~mask) | (f & mask); }
};

enum { CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048 };

// Family handlers: return false when the instruction is not theirs.
bool fam_mov(IC& c);
bool fam_fp(IC& c);
bool fam_int(IC& c);
bool fam_shift(IC& c);
bool fam_shuf(IC& c);
bool fam_cvt(IC& c);
bool fam_cmp(IC& c);
bool fam_gather(IC& c);
bool fam_misc(IC& c);
bool fam_kreg(IC& c);

// Element-wise helpers --------------------------------------------------------
// dst=reg, a=vvvv, b=rm (broadcastable); f(a, b, i) -> T
template <class T, class F>
inline void ew3(IC& c, F f) {
    const int es = sizeof(T), n = c.vl / es;
    Z a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r);
    c.rm_src(b, c.vl, es);
    for (int i = 0; i < n; i++) { T x, y; memcpy(&x, a.b + i * es, es); memcpy(&y, b.b + i * es, es); T v = f(x, y, i); memcpy(r.b + i * es, &v, es); }
    c.write(c.reg(), r, es, c.vl);
}
// dst=reg, src=rm; f(b, i)
template <class T, class F>
inline void ew2(IC& c, F f) {
    const int es = sizeof(T), n = c.vl / es;
    Z b, r; memset(&r, 0, sizeof r);
    c.rm_src(b, c.vl, es);
    for (int i = 0; i < n; i++) { T y; memcpy(&y, b.b + i * es, es); T v = f(y, i); memcpy(r.b + i * es, &v, es); }
    c.write(c.reg(), r, es, c.vl);
}
// FMA-like: d = reg (read), a = vvvv, b = rm; f(d, a, b, i)
template <class T, class F>
inline void ew3d(IC& c, F f) {
    const int es = sizeof(T), n = c.vl / es;
    Z d = c.R(c.reg()), a = c.R(c.vvvv()), b, r; memset(&r, 0, sizeof r);
    c.rm_src(b, c.vl, es);
    for (int i = 0; i < n; i++) { T x, y, z; memcpy(&x, d.b + i * es, es); memcpy(&y, a.b + i * es, es); memcpy(&z, b.b + i * es, es); T v = f(x, y, z, i); memcpy(r.b + i * es, &v, es); }
    c.write(c.reg(), r, es, c.vl);
}
// scalar op: dst[0] = f(vvvv[0], rm[0]); dst[1..] = vvvv[1..] up to 128 bits
template <class T, class F>
inline void sc3(IC& c, F f) {
    const int es = sizeof(T);
    Z a = c.R(c.vvvv()), b, r = a;
    if (c.mem) { memset(&b, 0, sizeof b); if (c.mbit(0)) c.ldn(b.b, c.ea, es); } else b = c.R(c.rmreg());
    T x, y; memcpy(&x, a.b, es); memcpy(&y, b.b, es);
    T v = c.mbit(0) ? f(x, y) : T();
    if (c.mbit(0)) memcpy(r.b, &v, es);
    else if (c.in.z) memset(r.b, 0, es);
    else memcpy(r.b, c.R(c.reg()).b, es);
    c.write_raw(c.reg(), r, 16);
}
// scalar FMA: d = reg[0], a = vvvv[0], b = rm[0]; upper from reg
template <class T, class F>
inline void sc3d(IC& c, F f) {
    const int es = sizeof(T);
    Z d = c.R(c.reg()), a = c.R(c.vvvv()), b, r = d;
    if (c.mem) { memset(&b, 0, sizeof b); if (c.mbit(0)) c.ldn(b.b, c.ea, es); } else b = c.R(c.rmreg());
    T x, y, z; memcpy(&x, d.b, es); memcpy(&y, a.b, es); memcpy(&z, b.b, es);
    if (c.mbit(0)) { T v = f(x, y, z); memcpy(r.b, &v, es); }
    else if (c.in.z) memset(r.b, 0, es);
    c.write_raw(c.reg(), r, 16);
}

// rounding control from EVEX.b on a register form (returns -1 when none)
inline int evex_rc(const x86::Insn& in) { return (in.b && in.mod == 3) ? in.L : -1; }
struct RcScope { // sets MXCSR rounding for the scope when rc >= 0
    uint32_t old; bool on;
    explicit RcScope(int rc);
    ~RcScope();
};

} // namespace emu
