// libmaxstatfix.so: Live's Linux Max editor (Resources/Max/Max) on btrfs subvolumes.
//
// Max finds the id of a folder by comparing st_ino alone (Max+0x5c5650, called from the
// parent walk at Max+0x5c1fa0). Every btrfs subvolume root has inode 256 ("/", "/home",
// "/var/tmp" on a default layout), so the parent of "/" can resolve to "/var/tmp", the
// walk goes round in a circle and Max's first database update never ends: the editor
// stays at "Starting Max Editor...". Here a directory with inode 256 gets its device
// number mixed into st_ino, so subvolume roots differ. Nothing else changes.
// libc only; preloaded into the editor by shim/max-editor.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <sys/stat.h>

#define BTRFS_SUBVOLUME_ROOT 256

static void unique(struct stat *st)
{
    if (S_ISDIR(st->st_mode) && st->st_ino == BTRFS_SUBVOLUME_ROOT)
        st->st_ino ^= (uint64_t)st->st_dev << 32;
}

int stat(const char *path, struct stat *st)
{
    static int (*real)(const char *, struct stat *);
    if (!real)
        real = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "stat");
    const int r = real(path, st);
    if (r == 0)
        unique(st);
    return r;
}

int stat64(const char *path, struct stat64 *st)
{
    static int (*real)(const char *, struct stat64 *);
    if (!real)
        real = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "stat64");
    const int r = real(path, st);
    if (r == 0)
        unique((struct stat *)st);
    return r;
}
