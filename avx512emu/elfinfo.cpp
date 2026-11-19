// Function bounds from .eh_frame_hdr (the binary search table every
// C/C++ object has for unwinding). No allocation; safe in a signal handler.
#include "rt.h"
#include <link.h>
#include <string.h>

namespace emu {

static uint64_t read_enc(const uint8_t*& p, uint8_t enc, uint64_t datarel) {
    if (enc == 0xff) return 0;
    const uint8_t* start = p;
    uint64_t v = 0;
    switch (enc & 0x0f) {
    case 0x00: memcpy(&v, p, 8); p += 8; break;                               // absptr
    case 0x01: { uint64_t r = 0; int s = 0; uint8_t b; do { b = *p++; r |= uint64_t(b & 0x7f) << s; s += 7; } while (b & 0x80); v = r; break; } // uleb128
    case 0x02: { uint16_t x; memcpy(&x, p, 2); p += 2; v = x; break; }
    case 0x03: { uint32_t x; memcpy(&x, p, 4); p += 4; v = x; break; }
    case 0x04: memcpy(&v, p, 8); p += 8; break;
    case 0x09: { int64_t r = 0; int s = 0; uint8_t b; do { b = *p++; r |= int64_t(b & 0x7f) << s; s += 7; } while (b & 0x80); if (s < 64 && (b & 0x40)) r |= -(int64_t(1) << s); v = uint64_t(r); break; }
    case 0x0a: { int16_t x; memcpy(&x, p, 2); p += 2; v = uint64_t(int64_t(x)); break; }
    case 0x0b: { int32_t x; memcpy(&x, p, 4); p += 4; v = uint64_t(int64_t(x)); break; }
    case 0x0c: memcpy(&v, p, 8); p += 8; break;
    default: return 0;
    }
    switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += uint64_t(start); break;  // pcrel
    case 0x30: v += datarel; break;          // datarel
    default: break;
    }
    return v;
}

struct FindCtx { uint64_t pc; ObjInfo* out; bool found; };

static int phdr_cb(struct dl_phdr_info* info, size_t, void* data) {
    FindCtx* f = (FindCtx*)data;
    uint64_t lo = ~0ull, hi = 0, ehf = 0;
    bool hit = false;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD) {
            uint64_t a = info->dlpi_addr + ph.p_vaddr, e = a + ph.p_memsz;
            if (a < lo) lo = a;
            if (e > hi) hi = e;
            if ((ph.p_flags & PF_X) && f->pc >= a && f->pc < e) hit = true;
        } else if (ph.p_type == PT_GNU_EH_FRAME) ehf = info->dlpi_addr + ph.p_vaddr;
    }
    if (!hit) return 0;
    f->out->base = info->dlpi_addr;
    f->out->lo = lo; f->out->hi = hi;
    f->out->eh_frame_hdr = ehf;
    size_t n = info->dlpi_name ? strlen(info->dlpi_name) : 0;
    if (n >= sizeof f->out->name) n = sizeof f->out->name - 1;
    memcpy(f->out->name, info->dlpi_name ? info->dlpi_name : "", n); f->out->name[n] = 0;
    f->found = true;
    return 1;
}

static uint64_t ro_lo[256], ro_hi[256]; static int ro_n;
void ro_add(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < ro_n; i++) if (ro_lo[i] == lo && ro_hi[i] == hi) return;
    if (ro_n < 256) { ro_lo[ro_n] = lo; ro_hi[ro_n] = hi; ro_n++; }
}
struct RoCtx { uint64_t a, n; bool found; uint64_t lo, hi; };
static int ro_cb(struct dl_phdr_info* info, size_t, void* data) {
    RoCtx* r = (RoCtx*)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_W)) continue;
        uint64_t a = info->dlpi_addr + ph.p_vaddr, e = a + ph.p_filesz;   // filesz: bss-like tails are not constant
        if (r->a >= a && r->a + r->n <= e) { r->found = true; r->lo = a; r->hi = e; return 1; }
    }
    return 0;
}
bool ro_mem(uint64_t a, size_t n) {
    for (int i = 0; i < ro_n; i++) if (a >= ro_lo[i] && a + n <= ro_hi[i]) return true;
    RoCtx r{a, n, false, 0, 0};
    dl_iterate_phdr(ro_cb, &r);
    if (r.found) ro_add(r.lo, r.hi);
    return r.found;
}

bool find_object(uint64_t pc, ObjInfo& o) {
    FindCtx f{pc, &o, false};
    dl_iterate_phdr(phdr_cb, &f);
    return f.found;
}

// Function [start, end) containing pc from the object's .eh_frame_hdr table.
bool find_function(const ObjInfo& o, uint64_t pc, uint64_t& start, uint64_t& end) {
    if (!o.eh_frame_hdr) return false;
    const uint8_t* h = (const uint8_t*)o.eh_frame_hdr;
    if (h[0] != 1) return false;
    uint8_t ptr_enc = h[1], cnt_enc = h[2], tab_enc = h[3];
    const uint8_t* p = h + 4;
    read_enc(p, ptr_enc, o.eh_frame_hdr);
    uint64_t count = read_enc(p, cnt_enc, o.eh_frame_hdr);
    if (tab_enc != 0x3b || count == 0) return false; // datarel|sdata4 expected
    const int32_t* tab = (const int32_t*)p;
    int64_t lo = 0, hi = int64_t(count) - 1, best = -1;
    while (lo <= hi) {
        int64_t mid = (lo + hi) / 2;
        uint64_t ip = o.eh_frame_hdr + int64_t(tab[2 * mid]);
        if (ip <= pc) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return false;
    const uint8_t* fde = (const uint8_t*)(o.eh_frame_hdr + int64_t(tab[2 * best + 1]));
    uint32_t len; memcpy(&len, fde, 4);
    if (len == 0 || len == 0xffffffff) return false;
    int32_t cie_off; memcpy(&cie_off, fde + 4, 4);
    const uint8_t* cie = fde + 4 - cie_off;
    // CIE: length, id, version, augmentation string, code align, data align, ra reg, [aug data]
    const uint8_t* c = cie + 8;
    uint8_t version = *c++;
    const char* aug = (const char*)c; c += strlen(aug) + 1;
    { const uint8_t* q = c; read_enc(q, 0x01, 0); read_enc(q, 0x09, 0); if (version == 1) q++; else read_enc(q, 0x01, 0); c = q; }
    uint8_t fde_enc = 0x00;
    if (aug[0] == 'z') {
        read_enc(c, 0x01, 0); // aug length
        for (const char* a = aug + 1; *a; a++) {
            if (*a == 'R') fde_enc = *c++;
            else if (*a == 'P') { uint8_t e = *c++; read_enc(c, e & 0x7f, 0); }
            else if (*a == 'L') c++;
            else if (*a == 'S' || *a == 'B') {}
            else return false;
        }
    }
    const uint8_t* q = fde + 8;
    uint64_t pc_begin = read_enc(q, fde_enc, 0);
    uint64_t pc_range = read_enc(q, fde_enc & 0x0f, 0);
    if (pc < pc_begin || pc >= pc_begin + pc_range) return false;
    start = pc_begin; end = pc_begin + pc_range;
    return true;
}

} // namespace emu
