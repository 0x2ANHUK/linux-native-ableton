// What AVX/AVX-512 instructions write and read, for the zmm upper-half
// bookkeeping: AVX2 code (VEX) zeroes bits 256..511 of the registers it
// writes, which the emulator keeps in memory and cannot see change.
#include <cpuid.h>
#include <stdlib.h>
#include "rt.h"

namespace emu {
using namespace x86;

// CPUID.(7,0).ECX: GFNI bit 8, VAES bit 9, VPCLMULQDQ bit 10. Live (built for a CPU with them) uses VEX forms without
// checking; on CPUs without them these VEX instructions are emulated too (interpreter, i_misc.cpp).
static unsigned cpu_ecx7() {
    static int v = -1;
    if (v < 0) { unsigned a, b, c = 0, d; v = __get_cpuid_count(7, 0, &a, &b, &c, &d) ? int(c & 0x700) : 0; if (getenv("AVX512EMU_NO_GFNI")) v &= ~0x100; }
    return unsigned(v);
}
bool is_special(const Insn& in) {
    if (in.enc == EVEX) return true;
    if (in.enc != VEX) return false;
    if (in.pp == 1) {
        if (((in.map == 3 && (in.opc == 0xce || in.opc == 0xcf) && in.W) || (in.map == 2 && in.opc == 0xcf && !in.W)) && !(cpu_ecx7() & 0x100)) return true;  // GFNI
        if (in.map == 2 && in.opc >= 0xdc && in.opc <= 0xdf && in.L == 1 && !(cpu_ecx7() & 0x200)) return true;   // VAES 256-bit
        if (in.map == 3 && in.opc == 0x44 && in.L == 1 && !(cpu_ecx7() & 0x400)) return true;                    // VPCLMULQDQ 256-bit
    }
    if (in.map == 1) {
        switch (in.opc) {
        case 0x41: case 0x42: case 0x45: case 0x46: case 0x47: case 0x4a: case 0x4b: // two-source ops are VEX.L1
            return in.L == 1;
        case 0x44: case 0x90: case 0x91: case 0x92: case 0x93: case 0x98: case 0x99:
            return in.L == 0;
        }
        return false;
    }
    if (in.map == 3 && in.pp == 1 && in.opc >= 0x30 && in.opc <= 0x33) return true;
    return false;
}

static bool evex_no_vec_dst(const Insn& in) {
    if (in.map == 1) {
        switch (in.opc) {
        case 0xc2: case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76: // compares -> k
        case 0x2e: case 0x2f:                                                       // ucomis
            return true;
        case 0x2a: return false;
        case 0x2c: case 0x2d: case 0x78: case 0x79: return in.pp >= 2;              // cvt*2si / 2usi scalar -> GPR
        case 0xc5: return true;                                                     // pextrw r32
        case 0x7e: return in.pp == 1;                                               // vmovd/vmovq r/m <- xmm (GPR or memory)
        }
        return false;
    }
    if (in.map == 2) {
        if (in.pp == 1 && (in.opc == 0x26 || in.opc == 0x27 || in.opc == 0x29 || in.opc == 0x37)) return true; // testm, pcmpeqq/gtq
        if (in.pp == 2 && (in.opc == 0x26 || in.opc == 0x27 || in.opc == 0x29 || in.opc == 0x39)) return true; // testnm, pmov*2m
        if (in.pp == 1 && in.opc >= 0xa0 && in.opc <= 0xa3) return true;          // scatters
        return false;
    }
    if (in.map == 3) {
        switch (in.opc) {
        case 0x1e: case 0x1f: case 0x3e: case 0x3f: case 0x66: case 0x67: return true; // vpcmp, fpclass
        case 0x14: case 0x15: case 0x16: case 0x17: return true;                      // pextr*
        }
    }
    return false;
}
// Whether the instruction writes an opmask register (conservative: true when unsure).
bool writes_k(const Insn& in) {
    if (in.enc == VEX) {
        if (in.map == 1) switch (in.opc) { case 0x41: case 0x42: case 0x44: case 0x45: case 0x46: case 0x47: case 0x4a: case 0x4b: case 0x90: case 0x92: return true; }
        if (in.map == 3 && in.opc >= 0x30 && in.opc <= 0x33) return true;
        return false;
    }
    if (in.enc != EVEX) return false;
    if (in.map == 2 && in.pp == 1 && ((in.opc >= 0x90 && in.opc <= 0x93) || (in.opc >= 0xa0 && in.opc <= 0xa3))) return true; // gathers/scatters clear mask bits
    if (!evex_no_vec_dst(in)) return false;
    if (in.map == 1 && (in.opc == 0x2e || in.opc == 0x2f || in.opc == 0xc5 || in.opc == 0x7e ||
                        ((in.opc == 0x2c || in.opc == 0x2d || in.opc == 0x78 || in.opc == 0x79) && in.pp >= 2))) return false;
    if (in.map == 3 && in.opc >= 0x14 && in.opc <= 0x17) return false;
    return true;
}
static bool evex_rm_dst(const Insn& in) {
    if (in.map == 1) {
        switch (in.opc) {
        case 0x11: case 0x13: case 0x17: case 0x29: case 0x2b: case 0x7f: case 0xd6: case 0xe7: return true;
        }
        return false;
    }
    if (in.map == 2) {
        if (in.pp == 2 && ((in.opc >= 0x10 && in.opc <= 0x15) || (in.opc >= 0x20 && in.opc <= 0x25) || (in.opc >= 0x30 && in.opc <= 0x35))) return true; // vpmov narrow
        if (in.pp == 1 && (in.opc == 0x8a || in.opc == 0x8b || in.opc == 0x63)) return true; // compress
        return false;
    }
    if (in.map == 3) {
        switch (in.opc) { case 0x19: case 0x1b: case 0x39: case 0x3b: case 0x1d: return true; }
    }
    return false;
}
static bool vex_no_vec_dst(const Insn& in) {
    if (in.map == 1) {
        switch (in.opc) {
        case 0x2e: case 0x2f: case 0x50: case 0xae: case 0xc5: case 0xd7: case 0xe7: case 0xf7: case 0x2b:
        case 0x2c: case 0x2d:
        case 0x41: case 0x42: case 0x44: case 0x45: case 0x46: case 0x47: case 0x4a: case 0x4b:
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x98: case 0x99:
            return true;
        case 0x7e: return in.pp == 1;
        }
        return false;
    }
    if (in.map == 2) {
        switch (in.opc) {
        case 0x0e: case 0x0f: case 0x17: case 0x2e: case 0x2f: case 0x8e: return true;
        }
        if (in.opc >= 0xf0) return true; // BMI and friends: GPR results
        return false;
    }
    if (in.map == 3) {
        switch (in.opc) {
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x61: case 0x63: case 0xf0: return true;
        case 0x30: case 0x31: case 0x32: case 0x33: return true; // kshift
        }
    }
    return false;
}
static bool vex_rm_dst(const Insn& in) {
    if (in.map == 1) {
        switch (in.opc) { case 0x11: case 0x13: case 0x17: case 0x29: case 0x7f: case 0xd6: return true; }
        return false;
    }
    if (in.map == 3) { switch (in.opc) { case 0x19: case 0x39: case 0x1d: return true; } }
    return false;
}

// Bit mask of vector registers 0..31 written; bit 31|... ALL = 0xffff for vzeroupper/vzeroall.
static bool shift_imm_group(const Insn& in) { return in.map == 1 && in.pp == 1 && in.opc >= 0x71 && in.opc <= 0x73; }

int vec_dst(const Insn& in) {
    if (in.enc == LEGACY) return -1;
    if (shift_imm_group(in)) return in.vvvv; // vpsrld $n, src, dst: dst in vvvv
    if (in.enc == VEX) {
        if (in.map == 1 && in.opc == 0x77) return -2;
        if (vex_no_vec_dst(in)) return -1;
        if (in.map == 3 && (in.opc == 0x60 || in.opc == 0x62)) return 0; // pcmp?strm -> xmm0
        if (vex_rm_dst(in)) return in.mod == 3 ? rm_reg_of(in) : -1;
        return reg_of(in);
    }
    if (is_special(in) && in.enc != EVEX) return -1;
    if (evex_no_vec_dst(in)) return -1;
    if (evex_rm_dst(in)) return in.mod == 3 ? rm_reg_of(in) : -1;
    return reg_of(in);
}

static bool evex_reads_dst(const Insn& in) {
    if (in.map == 2 && in.pp == 1) {
        int o = in.opc;
        if ((o >= 0x96 && o <= 0xbf) || (o >= 0x50 && o <= 0x53) || o == 0xb4 || o == 0xb5 ||
            (o >= 0x75 && o <= 0x77) || (o >= 0x7d && o <= 0x7f) || (o >= 0x70 && o <= 0x73)) return true;
    }
    if (in.map == 3 && in.pp == 1 && in.opc == 0x25) return true; // vpternlog
    return false;
}

// EVEX instruction whose vector destination (reg field) is written whole without reading its old
// value: no merge masking, not read-modify-write, not an rm destination. Conservative.
bool evex_pure_write(const Insn& in) {
    if (in.enc != EVEX || (in.aaa != 0 && !in.z)) return false;
    if (evex_no_vec_dst(in) || evex_rm_dst(in) || evex_reads_dst(in) || shift_imm_group(in)) return false;
    return true;
}

// Scalar EVEX instructions ignore L'L (LIG) and never touch bits above 127.
static bool evex_scalar(const Insn& in) {
    if (in.map == 1) {
        if (in.opc == 0x2e || in.opc == 0x2f) return true;
        if (in.pp >= 2) switch (in.opc) {
            case 0x10: case 0x11: case 0x2a: case 0x2c: case 0x2d: case 0x51: case 0x58: case 0x59: case 0x5a:
            case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0xc2: case 0x78: case 0x79: case 0x7b: return true; }
        return false;
    }
    if (in.map == 2 && in.pp == 1) switch (in.opc) {
        case 0x99: case 0x9b: case 0x9d: case 0x9f: case 0xa9: case 0xab: case 0xad: case 0xaf:
        case 0xb9: case 0xbb: case 0xbd: case 0xbf: case 0x2d: case 0x43: case 0x4d: case 0x4f: case 0xcb: case 0xcd: return true; }
    if (in.map == 3 && in.pp == 1) switch (in.opc) {
        case 0x0a: case 0x0b: case 0x27: case 0x51: case 0x57: case 0x67: return true; }
    return false;
}

// EVEX instructions without a vvvv source (encoded as 1111, the same as zmm0).
static bool evex_uses_vvvv(const Insn& in) {
    if (in.map == 1) {
        if (in.pp >= 2) switch (in.opc) { case 0x10: case 0x11: case 0x2a: case 0x51: case 0x58: case 0x59: case 0x5a: case 0x5c:
                                          case 0x5d: case 0x5e: case 0x5f: case 0xc2: case 0x7b: return true; }
        switch (in.opc) {
        case 0x10: case 0x11: case 0x28: case 0x29: case 0x2b: case 0x6f: case 0x7f: case 0x5a: case 0x5b: case 0xe6: case 0x78:
        case 0x79: case 0x7a: case 0x7b: case 0x2c: case 0x2d: case 0x51: case 0x70: case 0x71: case 0x72: case 0x73: case 0xd6:
        case 0x7e: case 0x6e: case 0xe7: case 0x2e: case 0x2f: return false;
        case 0x12: case 0x16: return in.pp < 2;
        }
        return true;
    }
    if (in.map == 2) {
        if (in.pp == 2) return !((in.opc >= 0x10 && in.opc <= 0x15) || (in.opc >= 0x20 && in.opc <= 0x25) || (in.opc >= 0x30 && in.opc <= 0x35) ||
                                 in.opc == 0x28 || in.opc == 0x29 || in.opc == 0x38 || in.opc == 0x39);
        switch (in.opc) {
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x78: case 0x79: case 0x7a:
        case 0x7b: case 0x7c: case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
        case 0x25: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x42: case 0x4c: case 0x4e: case 0x44:
        case 0x54: case 0x55: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x62: case 0x63: case 0xc4: case 0x13:
        case 0x90: case 0x91: case 0x92: case 0x93: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xc8: case 0xca: return false;
        }
        return true;
    }
    if (in.map == 3) switch (in.opc) {
        case 0x00: case 0x01: case 0x04: case 0x05: case 0x08: case 0x09: case 0x19: case 0x1b: case 0x39: case 0x3b: case 0x1d:
        case 0x26: case 0x56: case 0x66: case 0x14: case 0x15: case 0x16: case 0x17: return false; }
    return true;
}

// Registers (0..31) an EVEX instruction may read with at least `minbytes` (32 or 64) bytes.
uint32_t reads_wide(const Insn& in, int minbytes) {
    if (in.enc != EVEX || evex_scalar(in)) return 0;
    int vl = (in.b && in.mod == 3) ? 64 : (16 << (in.L > 2 ? 2 : in.L));
    if (vl < minbytes) return 0;
    uint32_t m = 0;
    auto add = [&m](int r) { if (r >= 0 && r < 32) m |= 1u << r; };
    if (shift_imm_group(in)) { // dst in vvvv; source in rm; merge reads the old dst
        if (in.mod == 3) add(rm_reg_of(in));
        if (in.aaa != 0 && !in.z) add(in.vvvv);
        return m;
    }
    if (evex_uses_vvvv(in)) add(in.vvvv);
    if (in.mod == 3) add(rm_reg_of(in));
    bool reg_is_k_or_gpr = (in.map == 1 && (in.opc == 0xc2 || (in.opc >= 0x64 && in.opc <= 0x66) || (in.opc >= 0x74 && in.opc <= 0x76) || in.opc == 0xc5)) ||
                           (in.map == 2 && (in.opc == 0x26 || in.opc == 0x27 || in.opc == 0x29 || in.opc == 0x37 || in.opc == 0x39)) ||
                           (in.map == 3 && (in.opc == 0x1e || in.opc == 0x1f || in.opc == 0x3e || in.opc == 0x3f || in.opc == 0x66 || in.opc == 0x67));
    if (reg_is_k_or_gpr) {}
    else if (evex_no_vec_dst(in) || evex_rm_dst(in)) add(reg_of(in));      // reg is a source
    else if (evex_reads_dst(in) || (in.aaa != 0 && !in.z)) add(reg_of(in)); // read-modify-write or merge
    if (evex_rm_dst(in) && in.mod == 3 && in.aaa != 0 && !in.z) add(rm_reg_of(in));
    // VSIB index register
    if (in.map == 2 && in.pp == 1 && ((in.opc >= 0x90 && in.opc <= 0x93) || (in.opc >= 0xa0 && in.opc <= 0xa3)) && in.has_sib)
        add(((in.sib >> 3) & 7) | (in.X << 3) | (in.Vp << 4));
    return m;
}
uint32_t reads512(const Insn& in) { return reads_wide(in, 64) & 0xffff; }

} // namespace emu
