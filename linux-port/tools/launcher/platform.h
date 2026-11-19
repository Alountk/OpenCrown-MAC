// Platform glue of the launcher: what differs between Linux and macOS (spawning children, the "open this folder" program, the
// path of the running binary).
#pragma once
#include <limits.h>
#include <spawn.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace dcp::launcher {

// The child gets only the descriptors it is explicitly given: fds 0-2 as set up by the caller's file actions, plus `keep` (the
// stdio fds the caller did not redirect, inherited from the launcher). Linux: close everything from fd 3 up. macOS has no
// closefrom file action: POSIX_SPAWN_CLOEXEC_DEFAULT closes everything not listed, so `keep` is listed explicitly.
inline void keep_only_stdio(posix_spawn_file_actions_t* fa, posix_spawnattr_t* attr, std::initializer_list<int> keep) {
#ifdef __APPLE__
    short flags = 0;
    posix_spawnattr_getflags(attr, &flags);
    posix_spawnattr_setflags(attr, short(flags | POSIX_SPAWN_CLOEXEC_DEFAULT));
    for (int fd : keep) posix_spawn_file_actions_addinherit_np(fa, fd);
#else
    (void)attr, (void)keep;
    posix_spawn_file_actions_addclosefrom_np(fa, 3);
#endif
}

// The program that opens a folder, file or URL in the desktop's default application.
inline const char* opener_program() {
#ifdef __APPLE__
    return "open";
#else
    return "xdg-open";
#endif
}

// Absolute path of the running binary.
inline std::string self_exe_path() {
    char buf[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof buf;
    char raw[PATH_MAX];
    if (_NSGetExecutablePath(raw, &n) != 0 || !realpath(raw, buf)) return "";
    return buf;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : "";
#endif
}

// The folder part of a path: "/a/b/c" -> "/a/b", "/c" -> "/", "c" -> ".".
inline std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

// The command the Play button runs, `flags` being the dcp_run options chosen in the launcher. Linux: dcp_run next to the launcher.
// macOS: the game code has to be recompiled from the user's dump first, which play-mac.sh does (and then starts dcp_run with the
// same options); it takes the dump folder, and the launcher's --renderer is dropped because the script picks OpenGL there.
inline std::vector<std::string> game_command(bool mac, const std::string& exe_dir, const std::string& source_dir,
                                             const std::string& eboot, const std::vector<std::string>& flags) {
    std::vector<std::string> cmd;
    if (!mac) {
        cmd = {exe_dir + "/dcp_run", eboot};
        cmd.insert(cmd.end(), flags.begin(), flags.end());
        return cmd;
    }
    cmd = {source_dir + "/play-mac.sh", dir_of(eboot)};
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i] == "--renderer") { ++i; continue; }
        cmd.push_back(flags[i]);
    }
    return cmd;
}

// What the launcher says while play-mac.sh is working, from the last lines of its log: the latest phase marker wins. Empty when the
// log has no marker (plain dcp_run) or the script has already started the game.
inline std::string play_phase(const std::vector<std::string>& lines) {
    auto has = [](const std::string& l, const char* what) { return l.find(what) != std::string::npos; };
    for (size_t i = lines.size(); i-- > 0;) {
        const std::string& l = lines[i];
        if (l.rfind("+ ", 0) == 0 && has(l, "dcp_run ") && !has(l, "--build") && !has(l, "cmake")) return "";
        if (l.rfind("+ ", 0) == 0 && has(l, "--build") && has(l, "--target dcp_run")) return "Building the game for this Mac...";
        if (l.rfind("play-mac: recompiling", 0) == 0) return "Recompiling your game (first run only, a few minutes)...";
        if (l.rfind("+ ", 0) == 0 && has(l, "--build") && has(l, "--target dcp_recompile")) return "Preparing the recompiler...";
    }
    return "";
}

}  // namespace dcp::launcher
