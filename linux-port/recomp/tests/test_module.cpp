// Loading a module for recompilation: image layout and function boundaries, on a synthetic ELF. No game data.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>

#include <sys/stat.h>

#include "dcp_rt.h"

#include "emit.h"
#include "module.h"

using namespace dcp::recomp;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

template <class T> static void put(std::vector<uint8_t>& d, size_t off, T v) {
    if (d.size() < off + sizeof(T)) d.resize(off + sizeof(T));
    std::memcpy(d.data() + off, &v, sizeof(T));
}

// An ELF with one PT_LOAD over the whole file (vaddr 0) and a PT_GNU_EH_FRAME. Code at 0x100: two functions, 0x20 and 0x10 bytes.
static std::vector<uint8_t> synthetic_elf(bool with_eh_frame_header) {
    std::vector<uint8_t> d(0x1000, 0);
    std::memcpy(d.data(), "\x7f" "ELF", 4);
    put<uint16_t>(d, 0x10, 0xFE10);
    put<uint64_t>(d, 0x18, 0x100);  // entry
    put<uint64_t>(d, 0x20, 0x40);   // phoff
    put<uint16_t>(d, 0x36, 56);
    put<uint16_t>(d, 0x38, 2);
    auto ph = [&](int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t fs, uint64_t ms) {
        const size_t p = 0x40 + size_t(i) * 56;
        put(d, p, type), put(d, p + 4, flags), put(d, p + 8, off), put(d, p + 16, va), put(d, p + 32, fs), put(d, p + 40, ms);
    };
    ph(0, 1, 5, 0, 0, 0x1000, 0x1000);
    ph(1, with_eh_frame_header ? 0x6474e550u : 0x6474e551u, 4, 0x800, 0x800, 0x10, 0x10);  // the second is PT_GNU_STACK: no eh_frame
    const uint8_t f1[] = {0xb8, 0x05, 0x00, 0x00, 0x00, 0xc3};  // function 1: mov eax, 5; ret
    std::memcpy(d.data() + 0x100, f1, sizeof f1);
    d[0x120] = 0xc3;                                            // function 2: ret
    // .eh_frame_hdr at 0x800: version 1, eh_frame_ptr pcrel sdata4 -> .eh_frame at 0x900
    d[0x800] = 1, d[0x801] = 0x1b, d[0x802] = 0x03, d[0x803] = 0x3b;
    put<int32_t>(d, 0x804, int32_t(0x900 - 0x804));
    // .eh_frame at 0x900: a CIE and two FDEs (pc_begin pcrel sdata4), then the terminator
    size_t at = 0x900;
    auto u32 = [&](uint32_t v) { put(d, at, v), at += 4; };
    const uint8_t cie[] = {0, 0, 0, 0, 1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1b, 0x0c, 7, 8, 0x90, 1, 0, 0};
    u32(uint32_t(sizeof cie));
    std::memcpy(d.data() + at, cie, sizeof cie), at += sizeof cie;
    auto fde = [&](uint32_t begin, uint32_t range) {
        const size_t start = at;
        u32(0x14);
        u32(uint32_t(start + 4 - 0x900));  // back to the CIE at 0x900
        put<int32_t>(d, at, int32_t(begin - (at))), at += 4;
        u32(range);
        d[at++] = 0;
        while ((at - start) % 4) ++at;
        put<uint32_t>(d, start, uint32_t(at - start - 4));
    };
    fde(0x100, 0x20);
    fde(0x120, 0x10);
    u32(0);
    return d;
}

static void test_layout_and_functions() {
    LoadedModule m = load_module_bytes(synthetic_elf(true));
    CHECK(m.image.size() >= 0x1000 && m.image[0x100] == 0xb8 && m.image[0x120] == 0xc3);
    CHECK(m.entry == 0x100);
    CHECK(m.functions.size() == 2);
    if (m.functions.size() == 2) {
        CHECK(m.functions[0].start == 0x100 && m.functions[0].size == 0x20);
        CHECK(m.functions[1].start == 0x120 && m.functions[1].size == 0x10);
    }
}

static void test_errors_are_readable() {
    bool threw = false;
    try { load_module_bytes(synthetic_elf(false)); } catch (const std::runtime_error& e) { threw = std::strstr(e.what(), "eh_frame") != nullptr; }
    CHECK(threw);  // a module without .eh_frame cannot be split into functions: say so
    threw = false;
    try { load_module_bytes({1, 2, 3}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { load_module("/nonexistent/path/eboot.bin"); } catch (const std::runtime_error& e) { threw = std::strstr(e.what(), "cannot read") != nullptr; }
    CHECK(threw);
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static void test_emit_and_run() {
    const char* t = getenv("TMPDIR");
    std::string dt = std::string(t ? t : "/tmp") + "/dcp_emit_XXXXXX";
    std::vector<char> dv(dt.begin(), dt.end());
    dv.push_back(0);
    if (!mkdtemp(dv.data())) return (void)std::puts("emit: skipped (no temp dir)");
    const std::string dir = dv.data();
    LoadedModule m = load_module_bytes(synthetic_elf(true));

    EmitOptions o;
    o.module = "m";
    o.out_dir = dir;
    o.shards = 2;
    EmitStats st = emit_module(m, o);
    CHECK(st.functions == 2 && st.unsupported == 0 && st.files == 3);  // two shards and the table
    CHECK(access((dir + "/m_000.cpp").c_str(), R_OK) == 0 && access((dir + "/m_001.cpp").c_str(), R_OK) == 0 &&
          access((dir + "/m_table.cpp").c_str(), R_OK) == 0);
    const std::string all = read_file(dir + "/m_000.cpp") + read_file(dir + "/m_001.cpp") + read_file(dir + "/m_table.cpp");
    CHECK(all.find("// 0x") == std::string::npos);                          // comments are off by default
    CHECK(all.find("f_m_100") != std::string::npos && all.find("f_m_120") != std::string::npos && all.find("dcp_register_m") != std::string::npos);
    CHECK(all.find("Derived from the user's own game files") != std::string::npos);  // every file carries the do-not-distribute notice

    o.comments = true;
    o.out_dir = dir + "/c";
    o.hooks = {0x100, 0x999};  // 0x999 is not a function of the module: ignored
    mkdir(o.out_dir.c_str(), 0700);
    emit_module(m, o);
    CHECK(read_file(o.out_dir + "/m_000.cpp").find("// 0x100: mov eax, 0x05") != std::string::npos);
    CHECK(read_file(o.out_dir + "/m_000.cpp").find("if (dcp_hook_m_100) dcp_hook_m_100(c);") != std::string::npos);
    const std::string hooked_table = read_file(o.out_dir + "/m_table.cpp");
    CHECK(hooked_table.find("register_hook_slot(\"m\", 0x100u, &dcp_hook_m_100);") != std::string::npos && hooked_table.find("999") == std::string::npos);
    CHECK(read_file(dir + "/m_000.cpp").find("dcp_hook") == std::string::npos);  // no hooks requested: no hook code

    // Byte patches: the instruction at 0x100 (mov eax,5) is lifted with a switchable replacement (mov eax,7).
    o.out_dir = dir + "/p";
    o.hooks.clear();
    o.patches = {{0x100, {{0xb8, 0x05, 0x00, 0x00, 0x00}, {0xb8, 0x07, 0x00, 0x00, 0x00}}}};
    mkdir(o.out_dir.c_str(), 0700);
    emit_module(m, o);
    const std::string patched = read_file(o.out_dir + "/m_000.cpp");
    CHECK(patched.find("if (dcp_patch_m_100) {") != std::string::npos && patched.find("} else {") != std::string::npos);
    CHECK(read_file(o.out_dir + "/m_table.cpp").find("register_patch_slot(\"m\", 0x100u, &dcp_patch_m_100);") != std::string::npos);
    CHECK(read_file(dir + "/m_000.cpp").find("dcp_patch") == std::string::npos);  // no patches requested: no patch code
    // A patch that does not match the module (another build of the game) or is not one whole instruction is refused.
    bool refused = false;
    o.patches = {{0x100, {{0xb8, 0x06, 0x00, 0x00, 0x00}, {0xb8, 0x07, 0x00, 0x00, 0x00}}}};
    try { emit_module(m, o); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
    refused = false;
    o.patches = {{0x100, {{0xb8, 0x05, 0x00}, {0x90, 0x90, 0x90}}}};  // 3 bytes of a 5-byte instruction
    try { emit_module(m, o); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
    o.patches.clear();

    // Site hooks: a host hook slot before the instruction at 0x100; refused like a patch when it does not belong to this module.
    o.out_dir = dir + "/s";
    o.sites = {{0x100, {{0xb8, 0x05, 0x00, 0x00, 0x00}, kSiteNoTarget}}};
    mkdir(o.out_dir.c_str(), 0700);
    emit_module(m, o);
    CHECK(read_file(o.out_dir + "/m_000.cpp").find("if (dcp_site_m_100) {") != std::string::npos);
    CHECK(read_file(o.out_dir + "/m_table.cpp").find("register_site_slot(\"m\", 0x100u, &dcp_site_m_100);") != std::string::npos);
    CHECK(read_file(dir + "/m_000.cpp").find("dcp_site") == std::string::npos);  // no sites requested: no site code
    for (const SiteHook& bad : {SiteHook{{0xb8, 0x06, 0x00, 0x00, 0x00}, kSiteNoTarget},   // other bytes
                                SiteHook{{0xb8, 0x05, 0x00}, kSiteNoTarget},                 // not a whole instruction
                                SiteHook{{0xb8, 0x05, 0x00, 0x00, 0x00}, 0x777}}) {         // target is no block of the function
        refused = false;
        o.sites = {{0x100, bad}};
        try { emit_module(m, o); } catch (const std::runtime_error&) { refused = true; }
        CHECK(refused);
    }
    refused = false;
    o.sites = {{0x3f0, {{0xcc}, kSiteNoTarget}}};  // inside the module but in no function
    try { emit_module(m, o); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
    o.sites.clear();

    // The shards build and run: compile them with the dispatch runtime, register, call function 1.
    const std::string cmd = std::string(DCP_TEST_CXX) + " " + DCP_TEST_ARCH + " -std=c++20 -O1 -shared -fPIC -I" DCP_TEST_RT_DIR " -o " + dir +
                            "/m.so " + dir + "/m_000.cpp " + dir + "/m_001.cpp " + dir + "/m_table.cpp " DCP_TEST_RT_DIR "/dcp_dispatch.cpp > " + dir + "/cc.log 2>&1";
    if (std::system(cmd.c_str())) {
        std::fprintf(stderr, "emit: compiling the shards failed:\n");
        std::system(("head -c 3000 " + dir + "/cc.log >&2").c_str());
        ++g_fail;
    } else if (void* lib = dlopen((dir + "/m.so").c_str(), RTLD_NOW)) {
        auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register_m"));
        auto fn = reinterpret_cast<void (*)(dcp::rt::Ctx*)>(dlsym(lib, "f_m_100"));
        CHECK(reg && fn);
        if (reg && fn) {
            reg(reinterpret_cast<unsigned long long>(m.image.data()), m.image.size());
            alignas(16) static uint8_t stack[128];
            dcp::rt::Ctx c{};
            c.r[dcp::rt::RSP] = reinterpret_cast<uint64_t>(stack) + sizeof stack - 8;
            fn(&c);
            CHECK(uint32_t(c.r[dcp::rt::RAX]) == 5);
        }
    } else {
        std::fprintf(stderr, "emit: dlopen: %s\n", dlerror());
        ++g_fail;
    }
    std::system(("rm -rf '" + dir + "'").c_str());
}

static void test_complete_functions() {
    // Code without unwind info: the entry point (libc.prx's module_start sits at 0, before its first FDE), PLT stubs that are only
    // ever called, and tail-jump targets all need to be functions of their own.
    LoadedModule m;
    m.image.assign(0x400, 0xcc);
    const uint8_t a[] = {0xe8, 0x7b, 0x00, 0x00, 0x00, 0xc3};        // 0x100: call 0x180; ret
    const uint8_t plt[] = {0xff, 0x25, 0x72, 0x01, 0x00, 0x00};      // 0x180: jmp [rip+0x172]
    const uint8_t entry[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};    // 0x40: mov eax, 1; ret
    const uint8_t b[] = {0xe9, 0xfb, 0x00, 0x00, 0x00};              // 0x200: jmp 0x300 (a tail call to code without an FDE)
    std::memcpy(m.image.data() + 0x100, a, sizeof a);
    std::memcpy(m.image.data() + 0x180, plt, sizeof plt);
    std::memcpy(m.image.data() + 0x40, entry, sizeof entry);
    std::memcpy(m.image.data() + 0x200, b, sizeof b);
    m.image[0x300] = 0xc3;
    m.entry = 0x40;
    m.functions = {{0x100, 0x10}, {0x200, 0x10}};
    const std::vector<FdeRange> all = complete_functions(m);
    std::vector<uint64_t> starts;
    for (const FdeRange& r : all) starts.push_back(r.start);
    CHECK((starts == std::vector<uint64_t>{0x40, 0x100, 0x180, 0x200, 0x300}));
    for (size_t i = 0; i + 1 < all.size(); ++i) CHECK(all[i].start + all[i].size <= all[i + 1].start);  // ranges never overlap
    CHECK(all.front().start == 0x40 && all.front().size == 0xc0);  // up to the next function
    // nothing to add when everything is covered
    LoadedModule c;
    c.image = m.image;
    c.entry = 0x100;
    c.functions = {{0x100, 0x10}};
    c.image[0x100] = 0xc3;
    CHECK(complete_functions(c).size() == 1);
}

static void test_tail_jump_into_a_function() {
    // A jmp into the middle of another function (a shared tail) makes that address a function of its own. The function it
    // lands in keeps its whole declared range, so its own blocks past that address are still lifted.
    LoadedModule m;
    m.image.assign(0x300, 0xcc);
    const uint8_t a[] = {0x90, 0x90, 0x90, 0x90, 0xc3, 0x90, 0x90, 0xc3};  // 0x100: nops, ret at 0x104, more code up to 0x107
    const uint8_t b[] = {0xe9, 0xff, 0xfe, 0xff, 0xff};                     // 0x200: jmp 0x104
    std::memcpy(m.image.data() + 0x100, a, sizeof a);
    std::memcpy(m.image.data() + 0x200, b, sizeof b);
    m.entry = 0x100;
    m.functions = {{0x100, 8}, {0x200, 8}};
    const std::vector<FdeRange> all = complete_functions(m);
    std::vector<uint64_t> starts;
    for (const FdeRange& r : all) starts.push_back(r.start);
    CHECK((starts == std::vector<uint64_t>{0x100, 0x104, 0x200}));
    CHECK(all[0].size == 8);  // not cut at 0x104
    CHECK(all[1].size == 4);  // the new entry ends where the function it lies in ends
}

static void test_parse_hooks() {
    // One hexadecimal image offset per line; # starts a comment.
    const std::set<uint64_t> h = parse_hooks("# render observers\n5650d0\n0x39ad80  # cursor\n\n  7ddb0\n");
    CHECK((h == std::set<uint64_t>{0x5650d0, 0x39ad80, 0x7ddb0}));
    CHECK(parse_hooks("").empty() && parse_hooks("# nothing\n").empty());
    bool threw = false;
    try { parse_hooks("12zz\n"); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

static void test_parse_patches() {
    // One patch per line: image offset, the bytes expected there, the replacement bytes (hex, same length); # starts a comment.
    const auto p = parse_patches("# layout\n2f394b be01000000 be1d000000  # confirm\n\n3aeda9 7507 6690\n");
    CHECK(p.size() == 2 && p.at(0x2f394b).expect == std::vector<uint8_t>({0xbe, 1, 0, 0, 0}) && p.at(0x2f394b).repl[1] == 0x1d);
    CHECK(p.at(0x3aeda9).repl == std::vector<uint8_t>({0x66, 0x90}));
    CHECK(parse_patches("").empty());
    for (const char* bad : {"12zz be be\n", "10 be\n", "10 be01 be\n", "10 b be\n", "10 be be 90\n", "10 be90 be\n"}) {
        bool threw = false;
        try { parse_patches(bad); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
}

static void test_parse_sites() {
    // `site <hex offset> <hex expected bytes> <target>`: a host hook that runs before the instruction there; target is `-` (none),
    // `ret` (leave the function) or the hex offset of an instruction the hook can jump to. Byte patches and sites share one file.
    const std::string file = "# warning\n3aeda9 7507 6690\nsite 3ae3b3 0f84 3aecd2  # no\nsite 591450 31c0 ret\nsite 3ae3cb 4c89fb -\n";
    const auto s = parse_sites(file);
    CHECK(s.size() == 3);
    CHECK(s.at(0x3ae3b3).expect == std::vector<uint8_t>({0x0f, 0x84}) && s.at(0x3ae3b3).target == 0x3aecd2);
    CHECK(s.at(0x591450).target == kSiteRet && s.at(0x3ae3cb).target == kSiteNoTarget);
    CHECK(parse_patches(file).size() == 1);  // the site lines are not patches
    CHECK(parse_sites("3aeda9 7507 6690\n").empty() && parse_sites("").empty());
    for (const char* bad : {"site 12zz be -\n", "site 10 be\n", "site 10 b -\n", "site 10 be - x\n", "site 10 be zz\n", "site 10 be 0\n"}) {
        bool threw = false;
        try { parse_sites(bad); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
}

static void test_repo_guard() {
    // Generated sources must not end up in a git work tree (they are derived from the user's game files).
    const char* t = getenv("TMPDIR");
    std::string dt = std::string(t ? t : "/tmp") + "/dcp_guard_XXXXXX";
    std::vector<char> dv(dt.begin(), dt.end());
    dv.push_back(0);
    if (!mkdtemp(dv.data())) return (void)std::puts("guard: skipped (no temp dir)");
    const std::string dir = dv.data();
    mkdir((dir + "/repo").c_str(), 0700);
    mkdir((dir + "/repo/.git").c_str(), 0700);
    mkdir((dir + "/repo/out").c_str(), 0700);
    mkdir((dir + "/plain").c_str(), 0700);
    CHECK(inside_git_work_tree(dir + "/repo/out"));
    CHECK(inside_git_work_tree(dir + "/repo"));
    CHECK(!inside_git_work_tree(dir + "/plain"));
    std::system(("rm -rf '" + dir + "'").c_str());
}

int main() {
    test_layout_and_functions();
    test_errors_are_readable();
    test_emit_and_run();
    test_parse_hooks();
    test_parse_patches();
    test_parse_sites();
    test_repo_guard();
    test_complete_functions();
    test_tail_jump_into_a_function();
    if (g_fail) return std::fprintf(stderr, "module: %d check(s) failed\n", g_fail), 1;
    std::puts("module: all tests passed");
    return 0;
}
