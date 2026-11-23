// Launcher platform glue (tools/launcher/platform.h): spawned children inherit only the stdio they are given, the opener
// program is the platform's own, and the running binary's folder is found without /proc.
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <signal.h>

#include <cerrno>
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

// Spawns `sh -c "sleep 60 & echo $!; wait"` the way the Play button does (own process group when `own_group`), returns its pid and
// the pid of the sleep it started (read from its stdout).
static pid_t spawn_with_descendant(bool own_group, pid_t* grandchild) {
    int p[2];
    if (pipe(p) != 0) return 0;
    const char* argv[] = {"/bin/sh", "-c", "sleep 60 & echo $!; wait", nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    dcp::launcher::keep_only_stdio(&fa, &attr, {1, 2});
    dcp::launcher::own_process_group(&attr, own_group);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, "/bin/sh", &fa, &attr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(p[1]);
    char buf[32] = {};
    ssize_t n = rc == 0 ? read(p[0], buf, sizeof buf - 1) : 0;
    close(p[0]);
    *grandchild = n > 0 ? pid_t(std::atoi(buf)) : 0;
    return rc == 0 ? pid : 0;
}

static bool gone(pid_t pid) {  // true once the process no longer exists (or is a zombie we do not wait for)
    for (int i = 0; i < 50; ++i) {
        if (kill(pid, 0) != 0 && errno == ESRCH) return true;
        usleep(40000);
    }
    return false;
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
    // macOS: the script gets the folder and every option, --renderer included (it reaches dcp_run, which is how Settings picks OpenGL).
    CHECK((dcp::launcher::game_command(true, "/app", "/src", "/d/CUSA/eboot.bin", flags) ==
           V{"/src/play-mac.sh", "/d/CUSA", "--render", "--window", "--renderer", "auto", "--size", "1280x720"}));
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
    // Progress: Ninja's "[done/total]" line and the step of the three the script is on.
    {
        using dcp::launcher::ninja_progress;
        using dcp::launcher::play_progress;
        int d = 0, t = 0;
        CHECK(ninja_progress("[249/536] Building C object a.o", &d, &t) && d == 249 && t == 536);
        CHECK(ninja_progress("[1/1] Linking CXX executable x", &d, &t) && d == 1 && t == 1);
        CHECK(!ninja_progress("+ cmake --build x", &d, &t) && !ninja_progress("[ 98%] Built target x", &d, &t));  // Make's lines are not Ninja's
        CHECK(!ninja_progress("[0/0] odd", &d, &t) && !ninja_progress("[5/3] odd", &d, &t) && !ninja_progress("", &d, &t));
        const std::vector<std::string> prep = {"+ cmake --build /w/tools --target dcp_recompile -j10"};
        const std::vector<std::string> recomp = {"+ cmake --build /w/tools --target dcp_recompile -j10", "play-mac: recompiling your dump (about 2 minutes)"};
        const std::vector<std::string> build = {"play-mac: generated code is up to date", "+ cmake --build /w/run --target dcp_run -j10"};
        auto p = play_progress(prep, "[3/30] Building CXX object x");
        CHECK(p.step == 1 && p.steps == 3 && p.fraction > 0.099f && p.fraction < 0.101f);
        CHECK(p.text == "Step 1 of 3: preparing the recompiler (3/30)");
        p = play_progress(prep, "");  // no Ninja line yet: step known, progress not
        CHECK(p.step == 1 && p.fraction < 0 && p.text == "Step 1 of 3: preparing the recompiler");
        p = play_progress(recomp, "[30/30] Linking CXX executable dcp_recompile");  // the recompiler prints no progress: never a stale bar
        CHECK(p.step == 2 && p.fraction < 0 && p.text == "Step 2 of 3: recompiling your game (first run only)");
        p = play_progress(build, "[268/536] Building CXX object y");
        CHECK(p.step == 3 && p.fraction > 0.499f && p.fraction < 0.501f && p.text == "Step 3 of 3: building the game for this Mac (268/536)");
        // The chunks of the game's own generated code (gen/eboot_NNN.cpp, gen/libc_NNN.cpp) are the slow part: say so.
        p = play_progress(build, "[520/536] Building CXX object CMakeFiles/dcp_game.dir/w/build/gen/eboot_015.cpp.o");
        CHECK(p.step == 3 && p.text == "Step 3 of 3: building your game's code, the slowest part (520/536)");
        p = play_progress(build, "[530/536] Building CXX object CMakeFiles/dcp_game.dir/w/build/gen/libc_002.cpp.o");
        CHECK(p.text == "Step 3 of 3: building your game's code, the slowest part (530/536)");
        p = play_progress(build, "[100/536] Building CXX object CMakeFiles/x.dir/src/eboot_loader.cpp.o");  // a source file that only looks alike
        CHECK(p.text == "Step 3 of 3: building the game for this Mac (100/536)");
        CHECK(play_progress({}, "[1/2] x").step == 0 && play_progress({}, "[1/2] x").text.empty());  // plain dcp_run: nothing to show
        CHECK(play_progress({"+ /w/run/dcp_run /d/eboot.bin --window"}, "").step == 0);               // the game is running
    }
    // The self-updater follows the upstream Linux releases (an AppImage): it exists only off macOS.
    CHECK(dcp::launcher::updater_supported(false));
    CHECK(!dcp::launcher::updater_supported(true));
    // The source tree play-mac.sh lives in: inside an app bundle (Contents/MacOS/<exe>, tree in Contents/Resources/linux-port) when it is
    // there, else the one the launcher was built from.
    {
        auto has = [](const std::string& p) { return p == "/Apps/OpenCrown.app/Contents/MacOS/../Resources/linux-port/play-mac.sh"; };
        auto none = [](const std::string&) { return false; };
        CHECK(dcp::launcher::mac_source_dir("/Apps/OpenCrown.app/Contents/MacOS", "/src", has) ==
              "/Apps/OpenCrown.app/Contents/MacOS/../Resources/linux-port");
        CHECK(dcp::launcher::mac_source_dir("/Apps/OpenCrown.app/Contents/MacOS", "/src", none) == "/src");
        CHECK(dcp::launcher::mac_source_dir("/build", "/src", has) == "/src");  // a plain build folder: as before
    }
    // The SDL GPU renderer only loads SPIR-V shaders, so on macOS the game always draws with OpenGL: the options that need SDL GPU
    // (renderer choice, FSR 1, sharpening) are left out there. The Steam Deck / touch-screen settings are Linux only too.
    CHECK(dcp::launcher::deck_touch_options(false) && !dcp::launcher::deck_touch_options(true));
    // Reports go to the lobby server, upstream's by default, and the fork has no public place for them yet: no Report a Bug page on macOS.
    CHECK(dcp::launcher::bug_report_page(false) && !dcp::launcher::bug_report_page(true));
    // A long build floods the log with ninja progress lines: only the script's own marker lines say which step it is on.
    {
        std::vector<std::string> log = {"play-mac: generated code is up to date", "+ cmake --build /w/run --target dcp_run -j10"};
        for (int i = 1; i <= 400; ++i) log.push_back("[" + std::to_string(i) + "/400] Building CXX object x" + std::to_string(i) + ".o");
        CHECK(play_phase(dcp::launcher::phase_markers(log)) == "Building the game for this Mac...");
        CHECK(dcp::launcher::phase_markers(log).size() == 2);
        std::vector<std::string> many;
        for (int i = 0; i < 200; ++i) many.push_back("+ cmake --build /w/tools --target dcp_recompile -j10");
        CHECK(dcp::launcher::phase_markers(many).size() <= 40);  // bounded
    }
    // Stop: on macOS the Play button's child (play-mac.sh) leads its own process group and the whole group is stopped, so the builds and
    // the game it started do not outlive it; elsewhere the child alone is signalled.
    CHECK(dcp::launcher::stop_target(123, true) == -123 && dcp::launcher::stop_target(123, false) == 123);
    {
        pid_t sleeper = 0;
        pid_t sh = spawn_with_descendant(true, &sleeper);
        CHECK(sh > 0 && sleeper > 0 && getpgid(sh) == sh && getpgid(sleeper) == sh);
        if (sh > 0) {
            kill(dcp::launcher::stop_target(sh, true), SIGTERM);
            int st = 0;
            waitpid(sh, &st, 0);
            CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
            CHECK(sleeper > 0 && gone(sleeper));  // the descendant went with it
        }
        if (sleeper > 0) kill(sleeper, SIGKILL);
    }
    {
        pid_t sleeper = 0;
        pid_t sh = spawn_with_descendant(false, &sleeper);
        CHECK(sh > 0 && getpgid(sh) == getpgid(0));  // not asked: stays in the launcher's group
        if (sh > 0) kill(sh, SIGKILL), waitpid(sh, nullptr, 0);
        if (sleeper > 0) kill(sleeper, SIGKILL);
    }
    if (!g_fail) std::puts("launcher platform: all tests passed");
    return g_fail ? 1 : 0;
}
