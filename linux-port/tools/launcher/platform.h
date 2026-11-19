// Platform glue of the launcher: what differs between Linux and macOS (spawning children, the "open this folder" program, the
// path of the running binary).
#pragma once
#include <limits.h>
#include <spawn.h>
#include <unistd.h>

#include <initializer_list>
#include <string>

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

}  // namespace dcp::launcher
