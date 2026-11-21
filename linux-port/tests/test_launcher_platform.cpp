// Launcher platform glue (tools/launcher/platform.h): spawned children inherit only the stdio they are given, the opener
// program is the platform's own, and the running binary's folder is found without /proc.
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../tools/launcher/platform.h"

extern char** environ;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; } while (0)

// Runs `sh -c "test -e /dev/fd/<fd>"` as a child spawned the way the launcher spawns programs: exit 0 = the child has that fd.
static bool child_sees_fd(int fd) {
    const std::string cmd = "test -e /dev/fd/" + std::to_string(fd);
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    dcp::launcher::keep_only_stdio(&fa, &attr, {1, 2});
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) return true;  // cannot tell: fail the checks below
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main() {
    int extra = open("/dev/null", O_RDONLY);  // inheritable (no O_CLOEXEC), like a socket or a log the launcher holds open
    CHECK(extra > 2);
    CHECK(!child_sees_fd(extra));  // not inherited
    CHECK(child_sees_fd(1));       // stdout and stderr still are
    CHECK(child_sees_fd(2));
    close(extra);

#ifdef __APPLE__
    CHECK(std::strcmp(dcp::launcher::opener_program(), "open") == 0);
#else
    CHECK(std::strcmp(dcp::launcher::opener_program(), "xdg-open") == 0);
#endif
    const std::string self = dcp::launcher::self_exe_path();
    CHECK(!self.empty() && self[0] == '/' && self.find("test_launcher_platform") != std::string::npos);
    CHECK(dcp::launcher::dir_of("/a/b/c") == "/a/b" && dcp::launcher::dir_of("/c") == "/" && dcp::launcher::dir_of("c") == ".");
    // The command the Play button runs: dcp_run next to the launcher, or on macOS the play-mac.sh pipeline for the dump folder.
    using V = std::vector<std::string>;
    const V flags = {"--render", "--window", "--renderer", "auto", "--size", "1280x720"};
    CHECK((dcp::launcher::game_command(false, "/app", "/src", "/d/CUSA/eboot.bin", flags) ==
           V{"/app/dcp_run", "/d/CUSA/eboot.bin", "--render", "--window", "--renderer", "auto", "--size", "1280x720"}));
    // macOS: the script gets the folder; the launcher's own --renderer is dropped (the script picks OpenGL, the only backend there).
    CHECK((dcp::launcher::game_command(true, "/app", "/src", "/d/CUSA/eboot.bin", flags) ==
           V{"/src/play-mac.sh", "/d/CUSA", "--render", "--window", "--size", "1280x720"}));
    CHECK((dcp::launcher::game_command(true, "/app", "/src", "eboot.bin", {"--4k"}) == V{"/src/play-mac.sh", ".", "--4k"}));

    // What the status line says while the script runs, from the end of the log (the latest phase marker wins).
    using dcp::launcher::play_phase;
    CHECK(play_phase({}).empty());                                              // no markers: nothing to say (plain dcp_run)
    CHECK(play_phase({"eboot: base 0x1", "game output"}).empty());
    CHECK(play_phase({"+ cmake -S /s -B /w/tools -G Ninja", "+ cmake --build /w/tools --target dcp_recompile -j10"}) ==
          "Preparing the recompiler...");
    CHECK(play_phase({"+ cmake --build /w/tools --target dcp_recompile -j10", "[3/9] Building CXX object x", "play-mac: recompiling your dump (about 2 minutes)",
                      "+ /w/tools/dcp_recompile /d/eboot.bin /w/gen --name eboot"}) == "Recompiling your game (first run only, a few minutes)...");
    CHECK(play_phase({"play-mac: generated code is up to date", "+ cmake --build /w/run --target dcp_run -j10", "[12/338] Building CXX object y"}) ==
          "Building the game for this Mac...");
    CHECK(play_phase({"+ cmake --build /w/run --target dcp_run -j10", "+ /w/run/dcp_run /d/eboot.bin --window --renderer gl", "eboot: base 0x1"}).empty());  // running
    // The self-updater follows the upstream Linux releases (an AppImage): it exists only off macOS.
    CHECK(dcp::launcher::updater_supported(false));
    CHECK(!dcp::launcher::updater_supported(true));
    if (!g_fail) std::puts("launcher platform: all tests passed");
    return g_fail ? 1 : 0;
}
