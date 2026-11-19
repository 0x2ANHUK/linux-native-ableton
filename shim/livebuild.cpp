// Which Live binary runs (livebuild.h): the main program's GNU build ID.
//   Live 12.4.7  (Push 3 2.4.7):  0e5b75d81930b23fbeb5febaecc718498ad2c8c9
//   Live 11.3.43 (Push 3 1.2.23): c4330b0c200e70e2fb20ff1f76ec812544e5b39d
#include "livebuild.h"
#include <elf.h>
#include <link.h>
#include <cstring>

namespace {

const unsigned char kId12[20] = {0x0e, 0x5b, 0x75, 0xd8, 0x19, 0x30, 0xb2, 0x3f, 0xbe, 0xb5,
                                 0xfe, 0xba, 0xec, 0xc7, 0x18, 0x49, 0x8a, 0xd2, 0xc8, 0xc9};
const unsigned char kId11[20] = {0xc4, 0x33, 0x0b, 0x0c, 0x20, 0x0e, 0x70, 0xe2, 0xfb, 0x20,
                                 0xff, 0x1f, 0x76, 0xec, 0x81, 0x25, 0x44, 0xe5, 0xb3, 0x9d};

LiveBuild detect()
{
    LiveBuild build = LiveBuild::Unknown;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr) &ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE)
                    continue;
                auto *p = reinterpret_cast<const unsigned char *>(info->dlpi_addr + ph.p_vaddr);
                const unsigned char *end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    auto *note = reinterpret_cast<const ElfW(Nhdr) *>(p);
                    const unsigned char *name = p + sizeof *note;
                    const unsigned char *desc = name + ((note->n_namesz + 3) & ~3u);
                    if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && !memcmp(name, "GNU", 4) &&
                        note->n_descsz == 20) {
                        auto *result = static_cast<LiveBuild *>(out);
                        if (!memcmp(desc, kId12, 20))
                            *result = LiveBuild::Live12_4_7;
                        else if (!memcmp(desc, kId11, 20))
                            *result = LiveBuild::Live11_3_43;
                    }
                    p = desc + ((note->n_descsz + 3) & ~3u);
                }
            }
            return 1; // first entry = main program
        },
        &build);
    return build;
}

} // namespace

LiveBuild liveBuild()
{
    static const LiveBuild build = detect();
    return build;
}
