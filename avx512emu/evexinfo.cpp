#include "evexinfo.h"
namespace x86 {
struct TupleN { uint8_t map, pp, opc, W; int8_t ext; uint8_t n[3][2]; };
static const TupleN tab[] = {
#include "tuple_n.inc"
};
int evex_disp8_n(const Insn& in) {
    bool grp = (in.map == 1 && in.opc >= 0x71 && in.opc <= 0x73) || (in.map == 2 && (in.opc == 0xC6 || in.opc == 0xC7));
    int ext = grp ? in.reg3 : -1;
    int lo = 0, hi = sizeof tab / sizeof tab[0] - 1;
    uint32_t key = in.map << 24 | in.pp << 20 | in.opc << 8 | in.W << 4 | (ext + 1);
    while (lo <= hi) {
        int mid = (lo + hi) / 2; const TupleN& t = tab[mid];
        uint32_t k = t.map << 24 | t.pp << 20 | t.opc << 8 | t.W << 4 | (t.ext + 1);
        if (k == key) { int n = t.n[in.L > 2 ? 2 : in.L][in.b]; return n ? n : 1; }
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 1;
}
}
