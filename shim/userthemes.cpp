// Themes of the user's own.
//
// Live's theme chooser (Preferences > Theme & Colors) lists the .ask files in
// its program folder, products/live/Live/AppLive/Resources/Themes, and looks
// nowhere else. At every start the shim links each .ask file found in
// <data>/Ableton/Themes/Live 12 (Live 11: .../Live 11; <data> = $XDG_DATA_HOME, else
// ~/.local/share; also one level of subfolders, so a theme can keep its
// licence beside it) into that folder. The links are made again at each start:
// a removed theme disappears, a Live update that replaces the program folder
// gets them back, and Ableton's own files are never replaced (a theme with the
// name of one of them is skipped). The folder is created when missing, so it
// is easy to find.
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "livebuild.h"

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

const char kProgramThemes[] = "/products/live/Live/AppLive/Resources/Themes";

bool isTheme(const char *name)
{
    const size_t n = strlen(name);
    return n > 4 && name[0] != '.' && !strcasecmp(name + n - 4, ".ask");
}

void mkdirs(const std::string &path)
{
    for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at)
        mkdir(path.substr(0, at).c_str(), 0755);
    mkdir(path.c_str(), 0755);
}

// Links the themes in folder (and, at depth 0, its subfolders) into program.
void linkFrom(const std::string &folder, const std::string &program, int depth, int &linked, int &skipped)
{
    DIR *dir = opendir(folder.c_str());
    if (!dir)
        return;
    while (dirent *e = readdir(dir)) {
        if (e->d_name[0] == '.')
            continue;
        const std::string path = folder + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth == 0)
                linkFrom(path, program, 1, linked, skipped);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !isTheme(e->d_name))
            continue;
        if (symlink(path.c_str(), (program + "/" + e->d_name).c_str()) == 0)
            ++linked;
        else
            ++skipped; // a file of that name is there already (Ableton's, or a theme of the same name)
    }
    closedir(dir);
}

} // namespace

void linkUserThemes(const std::string &app)
{
    const std::string program = app + kProgramThemes;
    struct stat st;
    if (stat(program.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    const char *dataHome = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    std::string user;
    if (dataHome && dataHome[0] == '/')
        user = dataHome;
    else if (home && home[0] == '/')
        user = std::string(home) + "/.local/share";
    else
        return;
    switch (liveBuild()) {
    case LiveBuild::Live11_3_43: user += "/Ableton/Themes/Live 11"; break; // Live 11 and 12 themes differ
    case LiveBuild::Live12_4_7: user += "/Ableton/Themes/Live 12"; break;
    default: return;
    }
    mkdirs(user);

    // Links of an earlier start into the user's folder go first.
    int removed = 0;
    if (DIR *dir = opendir(program.c_str())) {
        while (dirent *e = readdir(dir)) {
            char target[4096];
            const ssize_t n = readlinkat(dirfd(dir), e->d_name, target, sizeof target - 1);
            if (n <= 0)
                continue;
            target[n] = '\0';
            if (!strncmp(target, user.c_str(), user.size()) && target[user.size()] == '/'
                && unlinkat(dirfd(dir), e->d_name, 0) == 0)
                ++removed;
        }
        closedir(dir);
    }
    int linked = 0, skipped = 0;
    linkFrom(user, program, 0, linked, skipped);
    if (linked || skipped || removed)
        menufixTrace("userthemes: %d linked, %d skipped (name taken), %d old links removed\n", linked, skipped,
                     removed);
}
