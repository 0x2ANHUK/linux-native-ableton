// ftime ELF: time analyze_function (and its phases) per function; print the slowest.
#include "func.cpp"
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>
#include <algorithm>
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    uint8_t* m = (uint8_t*)mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr* eh = (Elf64_Ehdr*)m; Elf64_Shdr* sh = (Elf64_Shdr*)(m + eh->e_shoff);
    const char* names = (const char*)m + sh[eh->e_shstrndx].sh_offset;
    emu::ObjInfo o; memset(&o, 0, sizeof o); o.base = (uint64_t)m;
    for (int s = 0; s < eh->e_shnum; s++) if (!strcmp(names + sh[s].sh_name, ".eh_frame_hdr")) o.eh_frame_hdr = (uint64_t)m + sh[s].sh_addr;
    const uint8_t* h = (const uint8_t*)o.eh_frame_hdr; const int32_t* tab = (const int32_t*)(h + 12);
    uint32_t count; memcpy(&count, h + 8, 4);
    std::vector<std::pair<double, uint64_t>> t; double total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t ip = o.eh_frame_hdr + int64_t(tab[2 * i]), fs, fe;
        if (!emu::find_function(o, ip, fs, fe)) continue;
        bool has = false;
        for (uint64_t a = fs; a < fe;) { x86::Insn in; int l = x86::decode((const uint8_t*)a, int(fe - a < 15 ? fe - a : 15), in); if (!l) break; if (emu::is_special(in)) { has = true; break; } a += l; }
        if (!has) continue;
        emu::scratch_reset();
        double t0 = now(); emu::FI* fi; int n = emu::analyze_function(fs, fe, 0, &fi); double t1 = now();
        total += t1 - t0; t.push_back({t1 - t0, (fs - o.base) | (uint64_t(n) << 40)});
    }
    std::sort(t.rbegin(), t.rend());
    printf("total %.0f ms over %zu functions\n", total, t.size());
    for (int i = 0; i < 8 && i < (int)t.size(); i++) printf("%8.2f ms %#lx insns %lu\n", t[i].first, (unsigned long)(t[i].second & 0xffffffffff), (unsigned long)(t[i].second >> 40));
}
