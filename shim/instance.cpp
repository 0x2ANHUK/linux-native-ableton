// Live's single-instance check.
//
// At start Live connects to the Unix socket /tmp/abl-live-detection. When
// something accepts, Live starts as a *second* instance ("Start as first
// instance: no" in Log.txt): it hands its file over to the first one, shows
// "Can't access file database" and keeps the browser's file operations and
// drop targets off. Otherwise it listens on that socket itself. A socket file
// with nothing listening is harmless: Live replaces it (tested after kill -9).
//
// The trouble is a listener that is not Live. Live does not mark the socket
// close-on-exec, so every program it starts inherits the listening socket
// (seen: Ableton Index). A child that outlives a crashed Live keeps it
// listening, and every following start is a second instance until that
// child ends.
//   - bind() below marks the socket close-on-exec, so children no longer
//     inherit it.
//   - clearStrayInstance() runs at start: when the socket is listening but no
//     Live holds it, it removes the socket file, so Live starts as the first
//     instance. A running Live is left alone.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include "livebuild.h"

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
extern __attribute__((visibility("hidden"))) bool shimActive;

namespace {

const char kInstanceSocket[] = "/tmp/abl-live-detection";

bool isInstanceSocket(const sockaddr *addr, socklen_t len)
{
    if (!addr || addr->sa_family != AF_UNIX || len <= offsetof(sockaddr_un, sun_path))
        return false;
    const size_t n = len - offsetof(sockaddr_un, sun_path); // the path, with or without its NUL
    return n >= sizeof kInstanceSocket - 1
           && strncmp(reinterpret_cast<const sockaddr_un *>(addr)->sun_path, kInstanceSocket, n) == 0;
}

std::string exeOf(const char *pid)
{
    char link[64], exe[4096];
    snprintf(link, sizeof link, "/proc/%s/exe", pid);
    const ssize_t n = readlink(link, exe, sizeof exe - 1);
    return n > 0 ? std::string(exe, n) : std::string();
}

bool isLiveExe(const std::string &exe)
{
    const size_t slash = exe.rfind('/');
    return exe.compare(slash == std::string::npos ? 0 : slash + 1, std::string::npos, "Live") == 0;
}

// Inodes of listening sockets bound to the instance path (/proc/net/unix:
// Num RefCount Protocol Flags Type St Inode Path; Flags 0x10000 = listening).
std::vector<unsigned long> listeningInodes()
{
    std::vector<unsigned long> inodes;
    FILE *f = fopen("/proc/net/unix", "r");
    if (!f)
        return inodes;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long flags = 0, inode = 0;
        char path[256] = "";
        if (sscanf(line, "%*s %*s %*s %lx %*s %*s %lu %255s", &flags, &inode, path) == 3
            && (flags & 0x10000) && !strcmp(path, kInstanceSocket))
            inodes.push_back(inode);
    }
    fclose(f);
    return inodes;
}

} // namespace

extern "C" int bind(int fd, const sockaddr *addr, socklen_t len)
{
    static auto next = reinterpret_cast<int (*)(int, const sockaddr *, socklen_t)>(nextSymbol("bind", "GLIBC_2.2.5"));
    const int result = next(fd, addr, len);
    if (result == 0 && shimActive && isInstanceSocket(addr, len)) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0)
            menufixTrace("instance: detection socket close-on-exec\n");
    }
    return result;
}

void clearStrayInstance()
{
    struct stat st;
    if (lstat(kInstanceSocket, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid())
        return;
    const std::vector<unsigned long> inodes = listeningInodes();
    if (inodes.empty())
        return; // nothing listening: Live replaces the file itself
    DIR *proc = opendir("/proc");
    if (!proc)
        return;
    std::vector<std::string> needles;
    for (unsigned long inode : inodes)
        needles.push_back("socket:[" + std::to_string(inode) + "]");
    const std::string self = std::to_string(getpid());
    std::string stray;
    bool live = false;
    while (dirent *p = readdir(proc)) {
        if (p->d_name[0] < '1' || p->d_name[0] > '9' || self == p->d_name)
            continue;
        const std::string fdDir = std::string("/proc/") + p->d_name + "/fd";
        DIR *fds = opendir(fdDir.c_str());
        if (!fds)
            continue;
        bool holds = false;
        while (dirent *e = readdir(fds)) {
            char target[64];
            const ssize_t n = readlinkat(dirfd(fds), e->d_name, target, sizeof target - 1);
            if (n <= 0)
                continue;
            target[n] = '\0';
            for (const std::string &needle : needles)
                holds |= needle == target;
            if (holds)
                break;
        }
        closedir(fds);
        if (!holds)
            continue;
        const std::string exe = exeOf(p->d_name);
        if (isLiveExe(exe))
            live = true;
        else if (stray.empty())
            stray = std::string(p->d_name) + " " + exe;
    }
    closedir(proc);
    if (live || stray.empty())
        return; // a running Live (it takes the second start), or a holder that cannot be seen
    if (unlink(kInstanceSocket) == 0) {
        fprintf(stderr, "AbletonLive%s: %s was held by process %s, not by Live; removed it\n",
                liveBuild() == LiveBuild::Live11_3_43 ? "11" : "12", kInstanceSocket, stray.c_str());
        menufixTrace("instance: removed a detection socket held by another program\n");
    }
}
