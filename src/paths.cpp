#include "paths.h"

#include <windows.h>

namespace paths {

std::string game_dir() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path, n);
    return dir.substr(0, dir.find_last_of("\\/"));
}

std::string mod_dir() {
    static const std::string dir = [] {
        std::string d = game_dir() + "\\libfpv";
        CreateDirectoryA(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::string mod_file(const char *name) { return mod_dir() + "\\" + name; }

}  // namespace paths
