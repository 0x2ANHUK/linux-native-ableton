// xloff: offline translation of one ELF object (see offline.h).
//   xloff IN OUT [-a HEXADDR]   translates every function with AVX-512 / opmask code of IN and writes OUT =
//   IN with the original code patched (jmp into the new segment) plus one PT_LOAD (the former PT_NOTE) holding
//   the translated code and the fixup tables. OUT needs libavx512emu.so (same ABI) at run time.
// Built like gharness (#includes func.cpp): build-xloff.sh
#include "func.cpp"
namespace emu { extern bool g_xl_pic; extern uint64_t g_xl_thunk_slot; extern void (*xl_group_hook)(uint8_t*, int, int, uint64_t, int, uint64_t); }
#include "offline.h"
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

using namespace emu;

static uint8_t* B;          // image base in this process
static uint8_t* SEG;        // segment start in this process
static uint64_t SEGV;       // segment vaddr
static std::vector<uint32_t> v_reloc, v_thunk, v_desc, v_group, v_map, v_fn, v_tls;

static int32_t T_BUILD = XL_TLS_BUILD;   // -T: the TState offset Live's process will have (no fixups needed)
static uint32_t soff(uint64_t p) { return uint32_t(p - (uint64_t)SEG); }
static void on_group(uint8_t* mem, int nspecial, int ncall, uint64_t code, int len, uint64_t orig) {
    Desc* d = (Desc*)mem;
    for (int i = 0; i < nspecial; i++) { d[i].pc -= (uint64_t)B; d[i].flags |= DF_XLREL; v_desc.push_back(soff((uint64_t)&d[i])); }
    uint64_t* slots = (uint64_t*)(mem + sizeof(Desc) * nspecial);
    for (int i = 0; i <= ncall; i++) slots[i] = 0;   // unused: calls are position-independent, one shared thunk slot
    v_group.push_back(uint32_t(code - (uint64_t)B)); v_group.push_back(uint32_t(len)); v_group.push_back(uint32_t(orig - (uint64_t)B)); v_group.push_back(uint32_t(nspecial));
    // TLS fixups: every fs:disp32 (no base, no index) whose value is one of our TState offsets
    for (uint64_t a = code; a < code + len;) {
        x86::Insn in; int l = x86::decode((const uint8_t*)a, int(code + len - a < 15 ? code + len - a : 15), in);
        if (!l) { fprintf(stderr, "xloff: cannot decode generated code at %#lx (group %#lx)\n", (unsigned long)(a - (uint64_t)B), (unsigned long)(orig - (uint64_t)B)); exit(2); }
        if (T_BUILD == XL_TLS_BUILD && in.seg == 0x64 && in.has_modrm && in.mod == 0 && in.rm3 == 4 && in.has_sib && (in.sib & 7) == 5 && ((in.sib >> 3) & 7) == 4 && !in.X && in.disp_size == 4) {
            int32_t v; memcpy(&v, (const void*)(a + in.disp_off), 4);
            if (v >= T_BUILD && v < T_BUILD + int32_t(sizeof(TState))) v_tls.push_back(soff(a + in.disp_off));
        }
        a += l;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: xloff IN OUT [-a HEXADDR] [-T TLSOFFSET]\n"); return 1; }
    const char* path = argv[1]; const char* outp = argv[2]; uint64_t only_addr = 0;
    for (int i = 3; i < argc; i++) { if (!strcmp(argv[i], "-a")) only_addr = strtoull(argv[++i], 0, 16); else if (!strcmp(argv[i], "-T")) T_BUILD = atoi(argv[++i]); }
    int fd = open(path, O_RDONLY); if (fd < 0) { perror(path); return 1; }
    struct stat st; fstat(fd, &st);
    uint8_t* file = (uint8_t*)mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    Elf64_Ehdr* eh = (Elf64_Ehdr*)file; Elf64_Phdr* ph = (Elf64_Phdr*)(file + eh->e_phoff);
    if (eh->e_type != ET_DYN) { fprintf(stderr, "xloff: only position-independent objects (ET_DYN)\n"); return 1; }
    uint64_t span = 0; int note = -1;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > span) span = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_type == PT_NOTE && note < 0) note = i;
        if (ph[i].p_type == PT_LOAD && !memcmp(file + ph[i].p_offset, XL_MAGIC, 8)) { fprintf(stderr, "xloff: %s is already translated\n", path); return 1; }
    }
    if (note < 0) { fprintf(stderr, "xloff: no PT_NOTE to turn into the new segment\n"); return 1; }
    SEGV = (span + 0x1fffff) & ~0x1fffffull;
    const size_t RESERVE = 1ull << 30;
    B = (uint8_t*)mmap((void*)0x200000000ull, SEGV + RESERVE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (B == MAP_FAILED) { perror("mmap"); return 1; }
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD) memcpy(B + ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && !(ph[i].p_flags & PF_W)) ro_add((uint64_t)B + ph[i].p_vaddr, (uint64_t)B + ph[i].p_vaddr + ph[i].p_filesz);
    SEG = B + SEGV;
    ObjInfo obj; memset(&obj, 0, sizeof obj); obj.base = (uint64_t)B; snprintf(obj.name, sizeof obj.name, "%s", path);
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_GNU_EH_FRAME) obj.eh_frame_hdr = (uint64_t)B + ph[i].p_vaddr;
    if (!obj.eh_frame_hdr) { fprintf(stderr, "xloff: no .eh_frame_hdr\n"); return 1; }
    // generated code goes to the segment (header page first); TState offsets get a recognisable value
    const size_t HDR = 4096;
    cache_add_region(SEG, RESERVE, HDR);
    g_tls_fixed = T_BUILD;
    xl_group_hook = on_group;
    g_xl_pic = true; g_xl_thunk_slot = (uint64_t)SEG + 2048; v_thunk.push_back(2048);
    const uint8_t* h = (const uint8_t*)obj.eh_frame_hdr; const int32_t* tab = (const int32_t*)(h + 12);
    uint32_t count; memcpy(&count, h + 8, 4);
    int funcs = 0, failed = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t ip = obj.eh_frame_hdr + int64_t(tab[2 * i]), fs, fe;
        if (!find_function(obj, ip, fs, fe)) continue;
        if (only_addr && !(fs - obj.base <= only_addr && fe - obj.base > only_addr)) continue;
        bool has = false;
        for (uint64_t a = fs; a < fe;) { x86::Insn in; int l = x86::decode((const uint8_t*)a, int(fe - a < 15 ? fe - a : 15), in); if (!l) break; if (is_special(in)) { has = true; break; } a += l; }
        if (!has) continue;
        scratch_reset();
        if (!translate_function(fs, fe, 0)) failed++;
        funcs++;
        if (funcs % 2000 == 0) fprintf(stderr, "xloff: %d functions\n", funcs);
    }
    // what the runtime gets: translated functions, address map, patched bytes
    { static uint64_t fnl[1 << 17]; uint32_t n = translated_functions(fnl, 1 << 17); for (uint32_t k = 0; k < n; k++) v_fn.push_back(uint32_t(fnl[k] - (uint64_t)B)); }
    { std::vector<std::pair<uint32_t, uint32_t>> mp;
      map_foreach([](uint64_t o, uint64_t s, void* c) { ((std::vector<std::pair<uint32_t, uint32_t>>*)c)->push_back({uint32_t(o - (uint64_t)B), uint32_t(s - (uint64_t)B)}); }, &mp);
      std::sort(mp.begin(), mp.end());
      for (auto& e : mp) { v_map.push_back(e.first); v_map.push_back(e.second); } }
    std::vector<uint32_t> v_patch; std::vector<uint8_t> pbytes;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        const uint8_t* o = file + ph[i].p_offset; const uint8_t* n = B + ph[i].p_vaddr;
        for (uint64_t k = 0; k < ph[i].p_filesz;) {
            if (o[k] == n[k]) { k++; continue; }
            uint64_t s = k; while (k < ph[i].p_filesz && (o[k] != n[k] || (k + 1 < ph[i].p_filesz && o[k + 1] != n[k + 1]))) k++;
            v_patch.push_back(uint32_t(ph[i].p_vaddr + s)); v_patch.push_back(uint32_t(k - s)); v_patch.push_back(uint32_t(pbytes.size()));
            pbytes.insert(pbytes.end(), o + s, o + k);
        }
    }
    // tables after the code
    size_t used = 0;
    { uint8_t* p = cache_alloc((uint64_t)SEG, 16, 16); used = size_t(p - SEG); }
    auto put = [&](const void* src, size_t n) -> uint64_t { used = (used + 15) & ~size_t(15); uint64_t at = used; memcpy(SEG + used, src, n); used += n; return at; };
    XlHeader hd; memset(&hd, 0, sizeof hd);
    memcpy(hd.magic, XL_MAGIC, 8); hd.version = XL_VERSION; hd.abi = xl_abi(); hd.seg_vaddr = SEGV; hd.tls_build = T_BUILD;
    hd.n_tls = v_tls.size(); hd.off_tls = put(v_tls.data(), v_tls.size() * 4);
    hd.n_reloc = v_reloc.size(); hd.off_reloc = put(v_reloc.data(), v_reloc.size() * 4);
    hd.n_thunk = v_thunk.size(); hd.off_thunk = put(v_thunk.data(), v_thunk.size() * 4);
    hd.n_desc = v_desc.size(); hd.off_desc = put(v_desc.data(), v_desc.size() * 4);
    hd.n_map = v_map.size() / 2; hd.off_map = put(v_map.data(), v_map.size() * 4);
    hd.n_fn = v_fn.size(); hd.off_fn = put(v_fn.data(), v_fn.size() * 4);
    hd.n_patch = v_patch.size() / 3; hd.off_patch = put(v_patch.data(), v_patch.size() * 4);
    hd.off_patchbytes = put(pbytes.data(), pbytes.size());
    hd.n_group = v_group.size() / 4; hd.off_group = put(v_group.data(), v_group.size() * 4);
    used = (used + 0xfff) & ~size_t(0xfff);
    hd.seg_size = used;
    memcpy(SEG, &hd, sizeof hd);
    // output: original file with patched code, the segment appended, PT_NOTE -> PT_LOAD
    std::vector<uint8_t> out(file, file + st.st_size);
    for (size_t k = 0; k < v_patch.size(); k += 3) {
        uint64_t va = v_patch[k];
        for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_LOAD && va >= ph[i].p_vaddr && va < ph[i].p_vaddr + ph[i].p_filesz)
            memcpy(out.data() + ph[i].p_offset + (va - ph[i].p_vaddr), B + va, v_patch[k + 1]);
    }
    size_t off = (out.size() + 0xfff) & ~size_t(0xfff);
    out.resize(off + used, 0);
    memcpy(out.data() + off, SEG, used);
    Elf64_Phdr* oph = (Elf64_Phdr*)(out.data() + eh->e_phoff);
    Elf64_Phdr& np = oph[note];
    np.p_type = PT_LOAD; np.p_flags = PF_R | PF_W | PF_X; np.p_offset = off; np.p_vaddr = np.p_paddr = SEGV;
    np.p_filesz = np.p_memsz = used; np.p_align = 0x1000;
    int ofd = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (ofd < 0 || write(ofd, out.data(), out.size()) != (ssize_t)out.size()) { perror(outp); return 1; }
    close(ofd);
    printf("xloff: %d functions (%d failed), %u groups, %u map entries, %u tls fixups, %u relocs, %u patches; segment %#lx, %.1f MB\n",
           funcs, failed, hd.n_group, hd.n_map, hd.n_tls, hd.n_reloc, hd.n_patch, (unsigned long)SEGV, used / 1048576.0);
    return 0;
}
