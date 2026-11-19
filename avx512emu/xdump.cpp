// xdump ELF HEXADDR [N]: translate N instructions from HEXADDR (file vaddr) and print the generated code.
#include "xlate.h"
#include "evexinfo.h"
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "rt.h"
namespace emu { bool is_special(const x86::Insn&); }
int main(int argc, char** argv) {
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    uint8_t* m = (uint8_t*)mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr* eh = (Elf64_Ehdr*)m; Elf64_Phdr* ph = (Elf64_Phdr*)(m + eh->e_phoff);
    uint64_t a = strtoull(argv[2], 0, 16); int n = argc > 3 ? atoi(argv[3]) : 8;
    uint64_t off = 0;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && a >= ph[i].p_vaddr && a < ph[i].p_vaddr + ph[i].p_filesz) off = a - ph[i].p_vaddr + ph[i].p_offset;
    static uint8_t buf[65536]; static emu::Desc d[64]; uint64_t slot = 0;
    emu::XlateOpts o; o.thunk_slot = (uint64_t)&slot;
    FILE* out = fopen("/tmp/claude-1000/xdump.bin", "wb");
    for (int i = 0; i < n; i++) {
        x86::Insn in; int l = x86::decode(m + off, 15, in);
        if (!l) break;
        if (emu::is_special(in)) {
            memset(&d[i], 0, sizeof d[i]); d[i].pc = a; memcpy(d[i].bytes, m + off, l); d[i].in = in;
            x86::Asm as(buf, sizeof buf, 0x1000000); const char* why = "";
            bool inl = emu::xlate_inline(as, d[i], o);
            if (!inl) { as.n = 0; emu::emit_interp_call(as, d[i], o); }
            fwrite(buf, 1, as.n, out);
            fprintf(stderr, "%lx: %d bytes %s\n", (unsigned long)a, as.n, inl ? "inline" : "INTERP");
            uint8_t marker[4] = {0x0f, 0x0b, 0x0f, 0x0b}; fwrite(marker, 1, 4, out); // ud2 ud2 separator
        }
        a += l; off += l;
    }
    fclose(out);
}
