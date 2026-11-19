// Runtime of the AVX-512 emulator: finds the function around a faulting
// AVX-512 instruction, translates it, patches the original code.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "x86dec.h"

namespace emu {

struct ObjInfo { uint64_t base, lo, hi, eh_frame_hdr; char name[256]; };
bool find_object(uint64_t pc, ObjInfo& o);
// [a, a+n) lies in a read-only (no PF_W) PT_LOAD segment: constant data. ro_add registers a range (harness, xloff).
bool ro_mem(uint64_t a, size_t n);
void ro_add(uint64_t lo, uint64_t hi);
bool find_function(const ObjInfo& o, uint64_t pc, uint64_t& start, uint64_t& end);

// arena for analysis data (reset per translation, under the global lock)
void* scratch_alloc(size_t n);
void scratch_reset();
// executable memory within +-2 GB of `near`
uint8_t* cache_alloc(uint64_t near, size_t n, size_t align = 16);
void cache_trim(uint8_t* p);
// a code region made elsewhere (offline segment, or the offline tool's output area) used as a cache chunk
void cache_add_region(uint8_t* base, size_t cap, size_t used);
void map_add_sorted(uint64_t base, uint64_t lo, uint64_t hi, const uint32_t* pairs, uint32_t n);
void map_foreach(void (*fn)(uint64_t orig, uint64_t stub, void* ctx), void* ctx);
// offline translation segment of the main program (func.cpp): attach at start
void offline_attach();
extern int32_t g_tls_fixed;
// original instruction address -> address in translated code
void map_put(uint64_t orig, uint64_t stub);
uint64_t map_get(uint64_t orig);
// write code bytes in the running process
bool poke(uint64_t addr, const void* src, size_t n);
void sync_cores();
void elog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern int log_level;
extern bool no_inline;
// sampling profiler (AVX512EMU_PROF=1)
void prof_register_group(uint64_t stub, uint32_t len, uint64_t orig, uint32_t n_specials);
void perfmap_group(uint64_t code, uint32_t len, uint64_t orig);
void prof_report();

// statistics
struct Stats { uint64_t xlate_ns, xlate_max_ns; uint64_t functions, groups, specials, fixups, int3_sites, fallback_sites, sigill, sigtrap, failures, regions;
               uint64_t rt_n, rt_ns, rt_max_ns, wait_max_ns; };   // rt_*: translations on audio threads (SCHED_FIFO/RR or named alsa-audio/AudioCalc)
extern Stats stats;

// Translate the function containing pc (or a fallback region). Returns the
// address to continue at for pc, or 0 on failure.
uint64_t translate_at(uint64_t pc);

// classify instructions
bool is_special(const x86::Insn& in);           // EVEX or VEX opmask instruction
int vec_dst(const x86::Insn& in);               // vector register written (0..31), -1 none, -2 all (vzeroupper/all)
uint32_t reads512(const x86::Insn& in);
uint32_t reads_wide(const x86::Insn& in, int minbytes); // zmm0-31 read with >= minbytes         // zmm0-15 whose bits 256..511 an EVEX instruction may read

} // namespace emu
