#!/bin/sh
# Builds libavx512emu.so (AVX-512 -> AVX2 translator, preloaded by the launcher on CPUs
# without AVX-512) into ../shim. Our own code must not use AVX-512: -march=x86-64-v3.
cd "$(dirname "$0")"
OUT=${1:-../shim/libavx512emu.so}
g++ -O2 -std=c++17 -fPIC -shared -march=x86-64-v3 -mno-avx512f -mfma -maes -mpclmul -fno-math-errno -ffp-contract=off \
    -Wall -Wno-unused-function -fvisibility=hidden \
    x86dec.cpp evexinfo.cpp tables.cpp interp_core.cpp i_mov.cpp i_fp.cpp i_int.cpp i_shift.cpp i_shuf.cpp i_cvt.cpp \
    i_cmp.cpp i_gather.cpp i_misc.cpp i_kreg.cpp xlate.cpp xinline.cpp classify.cpp elfinfo.cpp func.cpp runtime.cpp thunk.S \
    ../shim/glibccompat.cpp \
    -o "$OUT" -ldl -Wl,-z,nodelete -Wl,--exclude-libs,ALL -Wl,-soname,libavx512emu.so -Wl,-z,defs
