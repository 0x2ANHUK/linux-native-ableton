// Driver types in Live's Settings > Audio: PipeWire, ALSA, JACK.
//
// Live's audio engine keeps a list of "audio architectures" (the Driver Type
// menu). APlatformAudioInOut's constructor (0x2d68890) makes one
// AAlsaAudioArchitecture (new 0x110, constructor 0x2d60000) and adds it with
// 0x2d506e0 (this+0x90 list, 0x1daa280); on Linux that is the only entry. Its
// name comes from vtable slot 56 (+0x1c0, 0x2d60b50: TString "ALSA"). The
// architecture lists devices when it is made (0x2d60150 from the constructor)
// and again in its rescan (0x2d60d30, Live's /dev/snd hotplug check).
//
// The shim adds two more ALSA architectures and gives each architecture its
// own copy of the vtable with another name: Live's original one becomes
// "PipeWire" (first: the default), then "ALSA" and "JACK".
// All three run Live's own ALSA engine; they differ only in the devices their
// scans see (pwdevices.cpp, by scanBackend()) and in where an open of those
// devices goes (alsaaudio.cpp, by card number):
//   PipeWire  cards 100..199  PipeWire's sinks and sources (pipewire-alsa)
//   ALSA      cards 0..99     the sound cards themselves, exclusively
//   JACK      card 200        the JACK server (jackaudio.cpp)
// Every hook first checks the bytes it replaces; another Live build keeps its
// single ALSA entry. LIVE_AUDIO_DRIVERS=0 turns this off.
//
// Live saves the chosen type by name (Preferences.cfg, LastUsedArchitectureName).
// Settings from before the shim had these types say "ALSA" for what is now
// PipeWire; once per settings folder that value is changed to "PipeWire"
// before Live reads it, and a marker file (LinuxDriverTypes.txt) keeps a later
// choice of ALSA from being changed back.
#include "livebuild.h"
#include "audiodrivers.h"
#include <dirent.h>
#include <link.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target); // hook.cpp
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement);

namespace {

// Addresses per Live build (file addresses = load offsets).
struct Addresses {
    uintptr_t addArchitecture;
    unsigned char addArchitecturePrologue[15];
    size_t addArchitecturePrologueLen;
    uintptr_t addToList, fromPlatformCtor, newFn;
    size_t architectureSize;
    uintptr_t architectureCtor;
    unsigned char architectureCtorPrologue[14];
    size_t architectureCtorPrologueLen;
    uintptr_t afterCreate, rescan; // rescan 0: this build has none
    uintptr_t vtable, stringFromC, classCtor, architectureClass, classFactory, classMaker, classGetters[2];
    uintptr_t deviceCount; // 0: adds every architecture
};
// Live 12.4.7 (Push 3 2.4.7). Add: mov rdi,[rdi+0x90]; mov rsi,[rsi]; jmp 0x1daa280.
constexpr Addresses k12 = {0x2d506e0, {0x48, 0x8b, 0xbf, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x36, 0xe9, 0x91, 0x9b, 0x05, 0xff}, 15,
                           0x1daa280, 0x2d688fd, 0x1c27bd0, 0x110,
                           0x2d60000, {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20}, 13,
                           0x1dc29b0, 0x2d60d30,
                           0x95503a8, 0x1d708c0, 0x1dbe790, 0x9ceb988, 0x2d68b10, 0x2d5ff90, {0x2d5fe60, 0x2d5fe80},
                           0};
// Live 11.3.43 (Push 3 1.2.23): APlatformAudioInOut's constructor 0x25e1d10 makes the architecture
// (new 0x168, constructor 0x25db600, 0x1988160) and adds it with 0x25cbc30, which adds it to the list
// (0x1a42f50) only when it has devices (0x25ac460 > 0). Its class records take a C string name and no
// length (0x1983740, registered at 0xf09765). No /dev/snd rescan: devices are listed when an
// architecture is made, also by its clone (slot 48, 0x25db460 runs the constructor).
constexpr Addresses k11 = {0x25cbc30, {0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe, 0x48, 0x8b, 0x3e}, 13,
                           0x1a42f50, 0x25e1d76, 0x175c730, 0x168,
                           0x25db600, {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28}, 14,
                           0x1988160, 0,
                           0x7c80d40, 0x1949480, 0x1983740, 0x82686c0, 0x25e1f60, 0x25db580, {0x25db410, 0x25db430},
                           0x25ac460};
const Addresses *A;
constexpr int kVtableSlots = 80;
constexpr int kNameSlot = 56;
constexpr int kCloneSlot = 48;
constexpr int kClassSlots[] = {45, 46};
constexpr unsigned char kRescanPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
                                             0x48, 0x83, 0xec, 0x68};

using AddToListFn = void (*)(void *list, void *architecture);
using NewFn = void *(*)(size_t);
using CtorFn = void (*)(void *);
using StringFn = void *(*)(void *sret, const char *text);
using RescanFn = void *(*)(void *, void *, void *, void *, void *, void *);
using CloneFn = void **(*)(void **sret, void *self);

uintptr_t base;
bool active;
RescanFn realRescan;
CloneFn realClone;
thread_local AudioBackend currentScan = AudioBackend::PipeWire;

std::mutex mapMutex;
std::map<void *, AudioBackend> backends; // architecture -> backend

// One vtable copy per backend: [offset-to-top, typeinfo, slots...]
uintptr_t vtables[3][kVtableSlots + 2];

const char *const kNames[3] = {"PipeWire", "ALSA", "JACK"};

using ClassCtorFn = void (*)(void *, size_t, const char16_t *, void *, void *, void *, int, int, int);
using ClassCtor11Fn = void (*)(void *, const char *, void *, void *, void *, int, int, int);
using CountFn = long (*)(void *);
alignas(16) unsigned char classes[3][0x100]; // [0] unused: PipeWire keeps Live's class (and its saved settings)
bool classesMade, ownClasses;

template <int B> void *architectureClass(void *)
{
    return classes[B];
}

void makeClasses()
{
    if (classesMade)
        return;
    classesMade = true;
    if (!ownClasses)
        return;
    static const char16_t *const names[3] = {nullptr, u"AAlsaAudioArchitectureALSA", u"AAlsaAudioArchitectureJACK"};
    static const char *const names8[3] = {nullptr, "AAlsaAudioArchitectureALSA", "AAlsaAudioArchitectureJACK"};
    for (int b = 1; b < 3; ++b)
        if (A == &k11)
            reinterpret_cast<ClassCtor11Fn>(base + A->classCtor)(classes[b], names8[b],
                                                                 reinterpret_cast<void *>(base + A->architectureClass),
                                                                 reinterpret_cast<void *>(base + A->classFactory),
                                                                 reinterpret_cast<void *>(base + A->classMaker), 0, 0, 0);
        else
            reinterpret_cast<ClassCtorFn>(base + A->classCtor)(classes[b], std::char_traits<char16_t>::length(names[b]), names[b],
                                                               reinterpret_cast<void *>(base + A->architectureClass),
                                                               reinterpret_cast<void *>(base + A->classFactory),
                                                               reinterpret_cast<void *>(base + A->classMaker), 0, 0, 0);
    menufixTrace("audiodrivers: classes for ALSA and JACK\n");
}

template <int B> void *name(void *sret, void *)
{
    return reinterpret_cast<StringFn>(base + A->stringFromC)(sret, kNames[B]);
}

void setBackend(void *architecture, AudioBackend backend)
{
    *static_cast<uintptr_t **>(architecture) = &vtables[int(backend)][2];
    std::lock_guard<std::mutex> lock(mapMutex);
    backends[architecture] = backend;
}

AudioBackend backendOf(void *architecture)
{
    std::lock_guard<std::mutex> lock(mapMutex);
    auto it = backends.find(architecture);
    return it == backends.end() ? AudioBackend::PipeWire : it->second;
}

// Live clones architectures (slot 48); the clone keeps the backend. Live 11's
// clone runs the constructor, which lists the devices: of this backend. Live
// 12's clone copies the list (no scan), so it is called as it is.
template <int B> void **cloneArchitecture(void **sret, void *self)
{
    const AudioBackend saved = currentScan;
    if (A == &k11)
        currentScan = AudioBackend(B);
    void **result = realClone(sret, self);
    currentScan = saved;
    if (*result)
        setBackend(*result, AudioBackend(B));
    return result;
}

void *rescan(void *self, void *b, void *c, void *d, void *e, void *f)
{
    const AudioBackend saved = currentScan;
    currentScan = backendOf(self);
    void *result = realRescan(self, b, c, d, e, f);
    currentScan = saved;
    return result;
}

void *makeArchitecture(AudioBackend backend)
{
    void *architecture = reinterpret_cast<NewFn>(base + A->newFn)(A->architectureSize);
    const AudioBackend saved = currentScan;
    currentScan = backend; // its constructor lists the devices
    reinterpret_cast<CtorFn>(base + A->architectureCtor)(architecture);
    currentScan = saved;
    ++*reinterpret_cast<int *>(static_cast<char *>(architecture) + 8); // our reference, never released
    reinterpret_cast<CtorFn>(base + A->afterCreate)(architecture);
    setBackend(architecture, backend);
    return architecture;
}

void addArchitecture(void *self, void **architecture)
{
    void *list = *reinterpret_cast<void **>(static_cast<char *>(self) + 0x90);
    const auto addToList = reinterpret_cast<AddToListFn>(base + A->addToList);
    // Live 11 adds an architecture only when it has devices; Live 12 always.
    auto add = [addToList](void *list, void *architecture) {
        if (!A->deviceCount || reinterpret_cast<CountFn>(base + A->deviceCount)(architecture) > 0)
            addToList(list, architecture);
    };
    if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != base + A->fromPlatformCtor) {
        add(list, *architecture);
        return;
    }
    makeClasses();
    setBackend(*architecture, AudioBackend::PipeWire);
    add(list, *architecture);
    add(list, makeArchitecture(AudioBackend::Alsa));
    add(list, makeArchitecture(AudioBackend::Jack));
    menufixTrace("audiodrivers: driver types PipeWire, ALSA, JACK\n");
}

// Preferences.cfg strings are a 32-bit length and UTF-16LE text.
std::string prefString(const char *text)
{
    std::string out;
    const uint32_t n = strlen(text);
    out.append(reinterpret_cast<const char *>(&n), 4);
    for (const char *c = text; *c; ++c)
        out += *c, out += '\0';
    return out;
}

void migrateSettings()
{
    const char *home = getenv("HOME");
    if (!home)
        return;
    const std::string ableton = std::string(home) + "/.config/Ableton";
    DIR *dir = opendir(ableton.c_str());
    if (!dir)
        return;
    while (dirent *e = readdir(dir)) {
        if (strncmp(e->d_name, "Live ", 5) != 0)
            continue;
        const std::string folder = ableton + "/" + e->d_name, marker = folder + "/LinuxDriverTypes.txt";
        if (FILE *m = fopen(marker.c_str(), "r")) {
            fclose(m);
            continue;
        }
        const std::string path = folder + "/Preferences.cfg";
        std::string data;
        if (FILE *f = fopen(path.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0)
                data.append(buf, n);
            fclose(f);
        }
        const std::string from = prefString("ALSA"), to = prefString("PipeWire");
        int changed = 0;
        for (size_t at = data.find(from); at != std::string::npos; at = data.find(from, at + to.size())) {
            data.replace(at, from.size(), to);
            ++changed;
        }
        if (changed) {
            const std::string tmp = path + ".tmp";
            FILE *f = fopen(tmp.c_str(), "wb");
            if (!f || fwrite(data.data(), 1, data.size(), f) != data.size()) {
                if (f)
                    fclose(f);
                continue;
            }
            fclose(f);
            if (rename(tmp.c_str(), path.c_str()) != 0)
                continue;
        }
        if (FILE *m = fopen(marker.c_str(), "w")) {
            fputs("Written by the Ableton Live Linux shim: these settings use the driver types PipeWire, ALSA and JACK.\n"
                  "Before, \"ALSA\" meant what is now PipeWire, and was renamed once (delete this file to do that again).\n",
                  m);
            fclose(m);
        }
        menufixTrace("audiodrivers: settings migrated (%d)\n", changed);
    }
    closedir(dir);
}

uintptr_t mainBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr;
            return 1;
        },
        &b);
    return b;
}

__attribute__((constructor)) void installAudioDrivers()
{
    A = liveBuild() == LiveBuild::Live12_4_7 ? &k12 : liveBuild() == LiveBuild::Live11_3_43 ? &k11 : nullptr;
    if (!A)
        return;
    const char *off = getenv("LIVE_AUDIO_DRIVERS");
    if ((off && !strcmp(off, "0")) || getenv("LIVE_ALSA_EXCLUSIVE") || getenv("LIVE_ALSA_DEVICE"))
        return;
    base = mainBase();
    if (memcmp(reinterpret_cast<void *>(base + A->architectureCtor), A->architectureCtorPrologue,
               A->architectureCtorPrologueLen) != 0)
        return;
    migrateSettings();
    const auto *original = reinterpret_cast<const uintptr_t *>(base + A->vtable) - 2;
    for (int b = 0; b < 3; ++b)
        memcpy(vtables[b], original, sizeof vtables[b]);
    realClone = reinterpret_cast<CloneFn>(original[2 + kCloneSlot]);
    vtables[0][2 + kNameSlot] = reinterpret_cast<uintptr_t>(&name<0>);
    vtables[1][2 + kNameSlot] = reinterpret_cast<uintptr_t>(&name<1>);
    vtables[2][2 + kNameSlot] = reinterpret_cast<uintptr_t>(&name<2>);
    vtables[0][2 + kCloneSlot] = reinterpret_cast<uintptr_t>(&cloneArchitecture<0>);
    vtables[1][2 + kCloneSlot] = reinterpret_cast<uintptr_t>(&cloneArchitecture<1>);
    vtables[2][2 + kCloneSlot] = reinterpret_cast<uintptr_t>(&cloneArchitecture<2>);
    ownClasses = original[2 + kClassSlots[0]] == base + A->classGetters[0] && original[2 + kClassSlots[1]] == base + A->classGetters[1]
                 && !getenv("LIVE_AUDIO_SHARED_CLASS");
    if (ownClasses)
        for (int slot : kClassSlots) {
            vtables[1][2 + slot] = reinterpret_cast<uintptr_t>(&architectureClass<1>);
            vtables[2][2 + slot] = reinterpret_cast<uintptr_t>(&architectureClass<2>);
        }
    if (A->rescan)
        realRescan = reinterpret_cast<RescanFn>(
            hookFunction(base + A->rescan, kRescanPrologue, sizeof kRescanPrologue, reinterpret_cast<void *>(&rescan)));
    const bool added = (realRescan || !A->rescan)
                       && redirectFunction(base + A->addArchitecture, A->addArchitecturePrologue,
                                           A->addArchitecturePrologueLen, reinterpret_cast<void *>(&addArchitecture));
    active = added;
    menufixTrace("audiodrivers: hooks %d\n", added);
}

} // namespace

bool driverTypesActive()
{
    return active;
}

AudioBackend scanBackend()
{
    return currentScan;
}
