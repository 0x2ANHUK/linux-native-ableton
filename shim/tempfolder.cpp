// Export's temporary files on disk instead of in the RAM-backed runtime folder.
//
// Live (Push 3 build) asks one virtual function for the folder of temporary render
// files (vtable slot 0x9433de8 -> 0x1b43550, through 0x1b424b0 and 0x4be6e30):
// $XDG_RUNTIME_DIR, else /run/user/<uid>. On Push that is the right place; on a
// desktop it is a small tmpfs (by default 10 % of RAM, 1.4 GB on a 14 GB laptop).
// Export Audio renders every track into a "<name>.aad" file there first, so
// "All Individual Tracks" of a 5-minute, 18-track Set (18 x 52 MB) stops with
// "There was an error when seeking the file /run/user/1000/<name>.aad"
// (Log.txt: "No space left on device"), and an interrupted export leaves the files there.
//
// The slot is pointed at tempFolder(), which returns $LIVE_TEMP_DIR or
// <cache>/Ableton/Temp ($XDG_CACHE_HOME or ~/.cache, read at the first export, after
// standalone.cpp chose the home folder), created when missing: next to Live's own
// Decoding Cache (<cache>/Ableton/Cache), and inside live-home/ for a portable
// folder. Files an interrupted export left there are removed at the next export
// (removeLeftovers). Traced in a session
// (gdb on the slot): only Export Audio calls it (0x455e137, 0x4564afc); Freeze and
// recording use the Set's project or Live's Temporary Folder setting. The neighbouring
// slot (0x1b43560, same runtime folder) is left alone: Live and the file database's
// helper (Ableton Index, which runs without this library) meet through a file there.
// LIVE_TEMP_DIR=runtime keeps Live's behaviour.
#include "livebuild.h"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <link.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

// Live 12.4.7: TLinuxAppEnv's second vtable, slot 14 (0x9433de8) -> 0x1b43550: push rbx; mov rbx,rdi;
// call 0x1b424b0. Live 11.3.43: the same slot (0x7b72558) -> 0x164f7b0: push rbx; mov rbx,rdi;
// add rsi,-0x18; call 0x164eb70 (-> 0x3e601a0 -> 0x3e5c840, its XDG_RUNTIME_DIR lookup).
uintptr_t kSlot, kTempFolder; // vtable entry (RELRO: read-only after relocation), its function
constexpr unsigned char kTempFolderPrologue12[] = {0x53, 0x48, 0x89, 0xfb, 0xe8, 0x57, 0xef, 0xff, 0xff};
constexpr unsigned char kTempFolderPrologue11[] = {0x53, 0x48, 0x89, 0xfb, 0x48, 0x83, 0xc6, 0xe8, 0xe8, 0xb3, 0xf3, 0xff, 0xff};

using TempFolderFn = std::string *(*)(std::string *, void *);
TempFolderFn original;

bool makeFolders(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/')
            if (mkdir(path.substr(0, i).c_str(), 0700) != 0 && errno != EEXIST)
                return false;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(path.c_str(), W_OK) == 0;
}

// An export that was interrupted (Live killed, crash, power cut) leaves its
// files behind. In the runtime folder they went at logout; on disk they would
// stay, so the first export of a session removes the earlier ones. Only Live's
// own render files (*.aad) in our folder: nothing else is put there.
void removeLeftovers(const std::string &folder)
{
    DIR *dir = opendir(folder.c_str());
    if (!dir)
        return;
    int removed = 0;
    while (dirent *entry = readdir(dir)) {
        const size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".aad"))
            continue;
        struct stat st;
        if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode)
            && unlinkat(dirfd(dir), entry->d_name, 0) == 0)
            ++removed;
    }
    closedir(dir);
    if (removed)
        menufixTrace("tempfolder: removed %d files left by an interrupted export\n", removed);
}

std::string *tempFolder(std::string *out, void *self)
{
    original(out, self);
    static const std::string folder = [] {
        if (const char *own = getenv("LIVE_TEMP_DIR"); own && *own == '/')
            return std::string(own);
        if (const char *cache = getenv("XDG_CACHE_HOME"); cache && *cache == '/')
            return std::string(cache) + "/Ableton/Temp";
        const char *home = getenv("HOME");
        return home && *home == '/' ? std::string(home) + "/.cache/Ableton/Temp" : std::string();
    }();
    static const bool usable = !folder.empty() && makeFolders(folder) && (removeLeftovers(folder), true);
    if (usable)
        *out = folder;
    return out;
}

} // namespace

__attribute__((constructor)) void installTempFolder()
{
    kSlot = liveAddress(0x9433de8, 0x7b72558);
    kTempFolder = liveAddress(0x1b43550, 0x164f7b0);
    if (!kSlot)
        return;
    const bool live11 = liveBuild() == LiveBuild::Live11_3_43;
    const unsigned char *kTempFolderPrologue = live11 ? kTempFolderPrologue11 : kTempFolderPrologue12;
    const size_t prologueLen = live11 ? sizeof kTempFolderPrologue11 : sizeof kTempFolderPrologue12;
    const char *setting = getenv("LIVE_TEMP_DIR");
    if (setting && !strcmp(setting, "runtime"))
        return;
    uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info *info, size_t, void *out) { *static_cast<uintptr_t *>(out) = info->dlpi_addr; return 1; }, &base);
    auto *slot = reinterpret_cast<uintptr_t *>(base + kSlot);
    if (memcmp(reinterpret_cast<void *>(base + kTempFolder), kTempFolderPrologue, prologueLen)
        || *slot != base + kTempFolder) {
        menufixTrace("tempfolder: not this Live build (or already done), off\n");
        return;
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(slot) & ~uintptr_t(pageSize - 1));
    if (mprotect(page, pageSize, PROT_READ | PROT_WRITE) != 0)
        return;
    original = reinterpret_cast<TempFolderFn>(*slot);
    *slot = reinterpret_cast<uintptr_t>(&tempFolder);
    mprotect(page, pageSize, PROT_READ); // back to read-only (RELRO)
    menufixTrace("tempfolder: export temporaries on disk\n");
}
