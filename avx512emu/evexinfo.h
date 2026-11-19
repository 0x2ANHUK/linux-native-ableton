// EVEX opcode facts: compressed displacement scale.
#pragma once
#include "x86dec.h"
namespace x86 {
// disp8 scale for an EVEX instruction (1 if unknown); sets disp to the real displacement.
int evex_disp8_n(const Insn& in);
inline int32_t mem_disp(const Insn& in) { return (in.enc == EVEX && in.disp_size == 1) ? in.disp_raw * evex_disp8_n(in) : in.disp_raw; }
}
