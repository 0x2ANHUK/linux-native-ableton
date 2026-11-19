#include "state.h"
namespace emu {
// Constants are part of the TLS image, so every thread starts with them.
static constexpr TState make_tstate() {
    TState s{};
    for (int c = 0; c < 2; c++) for (int j = 0; j < 8; j++) s.c_bitd[c][j] = 1u << (8 * c + j);
    for (int c = 0; c < 2; c++) for (int j = 0; j < 4; j++) s.c_bitq[c][j] = 1ull << (4 * c + j);
    for (int j = 0; j < 16; j++) s.c_bitw[j] = uint16_t(1u << j);
    for (int j = 0; j < 32; j++) { s.c_bspread[j] = uint8_t(j / 8); s.c_bbit[j] = uint8_t(1u << (j % 8)); }
    for (int j = 0; j < 8; j++) { s.c_ones[j] = ~0u; s.c_sign32[j] = 0x80000000u; }
    for (int j = 0; j < 4; j++) s.c_sign64[j] = 0x8000000000000000ull;
    s.c_sign0[0] = 0x80000000u; s.c_sign0q[0] = 0x8000000000000000ull;
    s.c_lane0[0] = ~0u; s.c_lane0q[0] = ~0ull;
    for (int j = 0; j < 32; j++) s.c_b0f[j] = 0x0f;
    for (int j = 0; j < 8; j++) s.c_q01[j] = j & 1;
    for (int j = 0; j < 4; j++) s.c_2p84p63p52[j] = 0x1p84 + 0x1p63 + 0x1p52;
    for (int j = 0; j < 32; j++) { s.c_popnib[j] = uint8_t(((j & 15) & 1) + (((j & 15) >> 1) & 1) + (((j & 15) >> 2) & 1) + (((j & 15) >> 3) & 1)); s.c_b01[j] = 1; }
    for (int j = 0; j < 16; j++) s.c_w01[j] = 1;
    for (int kd = 0; kd < 5; kd++) {
        const int es = kd == 0 ? 2 : kd == 1 || kd == 3 ? 4 : 8, ed = kd < 3 ? 1 : 2;
        for (int j = 0; j < 32; j++) { int e = (j & 15) / ed, b = (j & 15) % ed; s.c_trunc[kd][j] = uint8_t(e < 16 / es ? e * es + b : 0x80); }
    }
    s.c_2p63f = 9223372036854775808.0f; s.c_2p63d = 9223372036854775808.0; s.c_2p32d = 4294967296.0;
    for (int j = 0; j < 8; j++) { s.c_m1f[j] = -1.0f; s.c_2p31f[j] = 2147483648.0f; s.c_2p32f[j] = 4294967296.0f; }
    for (int j = 0; j < 4; j++) { s.c_4330[j] = 0x4330000000000000ull; s.c_4530[j] = 0x4530000000000000ull; s.c_2p84p52[j] = 0x1p84 + 0x1p52; }
    for (int j = 0; j < 4; j++) { s.c_m1d[j] = -1.0; s.c_2p64d[j] = 0x1p64; s.c_2pm32d[j] = 0x1p-32; s.c_2p32dv[j] = 0x1p32; }
    for (int j = 0; j < 8; j++) s.c_pack02[j] = j < 4 ? 2 * j : 0;
    s.c_kmask[0] = 0xff; s.c_kmask[1] = 0xffff; s.c_kmask[2] = 0xffffffffull; s.c_kmask[3] = ~0ull;
    return s;
}
__thread TState tst __attribute__((tls_model("initial-exec"))) = make_tstate();
uint64_t g_xl_base = 0;   // load base of the object with an offline segment (Desc pc relative to it)
int32_t g_tls_fixed = 0;   // offline translation (tools/xloff): generate with this TState offset (fixed up at load)
int32_t tls_offset() { if (g_tls_fixed) return g_tls_fixed; uint64_t fs0; __asm__("mov %%fs:0, %0" : "=r"(fs0)); return int32_t((uint64_t)&tst - fs0); }
void emu_init_tables() {}
}
