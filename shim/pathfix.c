// libpathfix.so: Live sees its program folder through a link without hidden folders.
//
// Live and its indexer skip every content folder whose path has a component
// starting with "." (Live+0x1be1220, Ableton Index+0x2c1110). The default
// install folder is ~/.local/share/ableton-live-12, so the built-in devices
// (program folder .../Builtin: Instruments, Audio Effects, MIDI Effects,
// Modulators, Max for Live) were never indexed and those browser categories
// stayed empty.
//
// When the install path has such a component, the launcher (tools/launcher.c)
// makes a symlink without one in $XDG_RUNTIME_DIR, starts Live through it,
// and preloads this library with LIVE_REAL_ROOT (the install folder) and
// LIVE_LINK_ROOT (the link). Live finds its folder from readlink of
// /proc/<pid>/exe and canonicalises paths with realpath, getcwd and lstat;
// the kernel answers those with the real path. Here, results under the real
// folder are rewritten to the link, and the link itself is reported as a
// folder (not a symlink), so path walks do not resolve it back.
//
// Only paths under LIVE_REAL_ROOT change; without both variables nothing does.
// libc only (no Qt), so it can stay preloaded in Live's helper programs
// (shim/menufix.cpp keeps it in LD_PRELOAD for child processes).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *realRoot, *linkRoot;
static size_t realLen, linkLen;

// Also run on first use: the shim's constructor (libmenufix.so, later in
// LD_PRELOAD) runs before this one and already canonicalises the program
// folder (shim/standalone.cpp: realpath, readlink), from which it derives the
// portable build's data folder; without the roots set then, a portable build
// in a hidden folder kept its data folder (settings, Core Library, User
// Library; Live 11's Core Library also holds its own instruments and effects)
// at the hidden path, and Live did not index that content.
static int initDone;

__attribute__((constructor)) static void init(void)
{
    if (initDone)
        return;
    initDone = 1;
    const char *r = getenv("LIVE_REAL_ROOT"), *l = getenv("LIVE_LINK_ROOT");
    if (!r || !l || r[0] != '/' || l[0] != '/' || !strcmp(r, l))
        return;
    realRoot = r;
    linkRoot = l;
    realLen = strlen(r);
    linkLen = strlen(l);
}

// buf (len bytes, room for cap) under the real folder: rewritten in place to the link.
// Returns the new length, or -1 when it is not under the real folder or does not fit.
static long rewrite(char *buf, size_t len, size_t cap)
{
    init();
    if (!realRoot || len < realLen || memcmp(buf, realRoot, realLen) != 0 || (len > realLen && buf[realLen] != '/'))
        return -1;
    const size_t out = len - realLen + linkLen;
    if (out >= cap)
        return -1;
    memmove(buf + linkLen, buf + realLen, len - realLen);
    memcpy(buf, linkRoot, linkLen);
    return (long)out;
}

// Same for a NUL-terminated string.
static void rewriteString(char *s, size_t cap)
{
    const long n = rewrite(s, strlen(s), cap);
    if (n >= 0)
        s[n] = '\0';
}

static int isExeLink(const char *path)
{
    if (!path || strncmp(path, "/proc/", 6) != 0)
        return 0;
    const size_t n = strlen(path);
    return n > 10 && !strcmp(path + n - 4, "/exe");
}

// The link itself (with or without trailing slashes).
static int isLinkRoot(const char *path)
{
    init();
    if (!linkRoot || !path)
        return 0;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        --n;
    return n == linkLen && !memcmp(path, linkRoot, linkLen);
}

static ssize_t fixLinkTarget(ssize_t n, char *buf, size_t size)
{
    char tmp[PATH_MAX];
    if (n <= 0 || (size_t)n >= sizeof tmp)
        return n;
    memcpy(tmp, buf, (size_t)n);
    const long m = rewrite(tmp, (size_t)n, sizeof tmp);
    if (m < 0 || (size_t)m > size)
        return n;
    memcpy(buf, tmp, (size_t)m); // readlink does not terminate
    return m;
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    static ssize_t (*next)(const char *, char *, size_t);
    if (!next)
        next = (ssize_t (*)(const char *, char *, size_t))dlsym(RTLD_NEXT, "readlink");
    const ssize_t n = next(path, buf, size);
    return isExeLink(path) ? fixLinkTarget(n, buf, size) : n;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
    static ssize_t (*next)(int, const char *, char *, size_t);
    if (!next)
        next = (ssize_t (*)(int, const char *, char *, size_t))dlsym(RTLD_NEXT, "readlinkat");
    const ssize_t n = next(dirfd, path, buf, size);
    return isExeLink(path) ? fixLinkTarget(n, buf, size) : n;
}

char *realpath(const char *path, char *resolved)
{
    static char *(*next)(const char *, char *);
    if (!next)
        next = (char *(*)(const char *, char *))dlsym(RTLD_NEXT, "realpath");
    char tmp[PATH_MAX];
    char *r = next(path, resolved ? resolved : tmp);
    if (!r)
        return NULL;
    rewriteString(r, PATH_MAX);
    return resolved ? r : strdup(r);
}

char *getcwd(char *buf, size_t size)
{
    static char *(*next)(char *, size_t);
    if (!next)
        next = (char *(*)(char *, size_t))dlsym(RTLD_NEXT, "getcwd");
    char tmp[PATH_MAX];
    if (!next(tmp, sizeof tmp))
        return next(buf, size); // too long for us: unchanged, with libc's own errors
    rewriteString(tmp, sizeof tmp);
    const size_t len = strlen(tmp) + 1;
    if (!buf) {
        if (size && size < len) {
            errno = ERANGE;
            return NULL;
        }
        char *out = malloc(size > len ? size : len);
        return out ? memcpy(out, tmp, len) : NULL;
    }
    if (size < len) {
        errno = size ? ERANGE : EINVAL;
        return NULL;
    }
    return memcpy(buf, tmp, len);
}

int lstat(const char *path, struct stat *st)
{
    static int (*next)(const char *, struct stat *);
    if (!next)
        next = (int (*)(const char *, struct stat *))dlsym(RTLD_NEXT, "lstat");
    return isLinkRoot(path) ? stat(path, st) : next(path, st);
}

int lstat64(const char *path, struct stat64 *st)
{
    static int (*next)(const char *, struct stat64 *);
    if (!next)
        next = (int (*)(const char *, struct stat64 *))dlsym(RTLD_NEXT, "lstat64");
    return isLinkRoot(path) ? stat64(path, st) : next(path, st);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    static int (*next)(int, const char *, struct stat *, int);
    if (!next)
        next = (int (*)(int, const char *, struct stat *, int))dlsym(RTLD_NEXT, "fstatat");
    if ((flags & AT_SYMLINK_NOFOLLOW) && isLinkRoot(path))
        flags &= ~AT_SYMLINK_NOFOLLOW;
    return next(dirfd, path, st, flags);
}

int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags)
{
    static int (*next)(int, const char *, struct stat64 *, int);
    if (!next)
        next = (int (*)(int, const char *, struct stat64 *, int))dlsym(RTLD_NEXT, "fstatat64");
    if ((flags & AT_SYMLINK_NOFOLLOW) && isLinkRoot(path))
        flags &= ~AT_SYMLINK_NOFOLLOW;
    return next(dirfd, path, st, flags);
}

// Live 11.3.43 and its indexer were built against an older glibc: they call the
// stat wrappers __lxstat, __lxstat64 (GLIBC_2.2.5) and __fxstatat64
// (GLIBC_2.4) instead of lstat and fstatat, so the hooks above never saw their
// calls, the link read as a symlink, path walks resolved it back to the hidden
// folder, and the built-in categories stayed empty. Since glibc 2.33 these are
// compatibility symbols only, found with dlvsym. Only Live 11's launcher turns
// these on (LIVE_PATHFIX_OLDSTAT=1); elsewhere they pass every call through.
static int oldStat(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LIVE_PATHFIX_OLDSTAT");
        on = v && !strcmp(v, "1");
    }
    return on;
}

int __lxstat(int ver, const char *path, struct stat *st)
{
    static int (*next)(int, const char *, struct stat *), (*follow)(int, const char *, struct stat *);
    if (!next) {
        next = (int (*)(int, const char *, struct stat *))dlvsym(RTLD_NEXT, "__lxstat", "GLIBC_2.2.5");
        follow = (int (*)(int, const char *, struct stat *))dlvsym(RTLD_NEXT, "__xstat", "GLIBC_2.2.5");
    }
    return oldStat() && isLinkRoot(path) && follow ? follow(ver, path, st) : next(ver, path, st);
}

int __lxstat64(int ver, const char *path, struct stat64 *st)
{
    static int (*next)(int, const char *, struct stat64 *), (*follow)(int, const char *, struct stat64 *);
    if (!next) {
        next = (int (*)(int, const char *, struct stat64 *))dlvsym(RTLD_NEXT, "__lxstat64", "GLIBC_2.2.5");
        follow = (int (*)(int, const char *, struct stat64 *))dlvsym(RTLD_NEXT, "__xstat64", "GLIBC_2.2.5");
    }
    return oldStat() && isLinkRoot(path) && follow ? follow(ver, path, st) : next(ver, path, st);
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *st, int flags)
{
    static int (*next)(int, int, const char *, struct stat64 *, int);
    if (!next)
        next = (int (*)(int, int, const char *, struct stat64 *, int))dlvsym(RTLD_NEXT, "__fxstatat64", "GLIBC_2.4");
    if (oldStat() && (flags & AT_SYMLINK_NOFOLLOW) && isLinkRoot(path))
        flags &= ~AT_SYMLINK_NOFOLLOW;
    return next(ver, dirfd, path, st, flags);
}
