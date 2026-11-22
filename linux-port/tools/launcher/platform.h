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

// macOS: the Play button's child (play-mac.sh) starts builds and the game itself, so it leads a process group of its own and Stop
// signals that whole group. Elsewhere the child (dcp_run) is the game and stays in the launcher's group.
inline void own_process_group(posix_spawnattr_t* attr, bool mac) {
    if (!mac) return;
    short flags = 0;
    posix_spawnattr_getflags(attr, &flags);
    posix_spawnattr_setflags(attr, short(flags | POSIX_SPAWN_SETPGROUP));
    posix_spawnattr_setpgroup(attr, 0);
}

// What to pass to kill() to stop the child `pid` started with own_process_group(attr, mac).
inline pid_t stop_target(pid_t pid, bool mac) { return mac ? -pid : pid; }

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

// The self-updater (net/update.h) follows the upstream Linux releases and swaps an AppImage: it has nothing to offer on macOS, so there the
// launcher makes no start-up check, and shows no Update entry.
inline bool updater_supported(bool mac) { return !mac; }

// The SDL GPU renderer only loads SPIR-V shaders, so on macOS the game always draws with OpenGL: the renderer choice, FSR 1 and sharpening
// (SDL GPU only) do nothing there and are not shown.
inline bool gpu_renderer_options(bool mac) { return !mac; }

// Touch screen, Steam Deck trackpads and "mouse buttons press game buttons" (a Steam Deck setting) are for Linux handhelds, not a Mac.
inline bool deck_touch_options(bool mac) { return !mac; }

// "Report a Bug" sends the report to the lobby server (upstream's by default), which is not where a bug of the macOS fork belongs, and the
// fork has no public tracker of its own yet: the page is not shown on macOS.
inline bool bug_report_page(bool mac) { return !mac; }

// The lines of play-mac.sh's own output that mark a step ("+ command" echoes and "play-mac:" messages). A build floods the log with
// compiler progress lines, which would push these out of any fixed-size tail.
inline bool is_phase_marker(const std::string& line) { return line.rfind("+ ", 0) == 0 || line.rfind("play-mac:", 0) == 0; }

// The last (at most 40) marker lines of a log, in order.
inline std::vector<std::string> phase_markers(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    for (const std::string& l : lines)
        if (is_phase_marker(l)) out.push_back(l);
    if (out.size() > 40) out.erase(out.begin(), out.end() - 40);
    return out;
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
