// End to end: two hand-assembled functions are discovered, lifted, written out with their function table, compiled into a
// shared library together with the dispatch runtime, and run. The first calls the second through a register, so the call
// goes through the generated table. Plain C++ at run time: works natively on arm64 as well. No game data.
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "dcp_rt.h"
#include "lift.h"

using namespace dcp::recomp;
using dcp::rt::Ctx;

// Compiles `code` together with the dispatch runtime into a shared library and loads it. Returns null on failure.
static void* build_and_load(const std::string& code) {
    const char* t = getenv("TMPDIR");
    std::string dir_t = std::string(t ? t : "/tmp") + "/dcp_e2e_XXXXXX";
    std::vector<char> dv(dir_t.begin(), dir_t.end());
    dv.push_back(0);
    if (!mkdtemp(dv.data())) return nullptr;
    const std::string dir = dv.data();
    { std::ofstream(dir + "/gen.cpp") << code; }
    const std::string cmd = std::string(DCP_TEST_CXX) + " " + DCP_TEST_ARCH + " -std=c++20 -O1 -shared -fPIC -I" DCP_TEST_RT_DIR " -o " +
                            dir + "/gen.so " + dir + "/gen.cpp " DCP_TEST_RT_DIR "/dcp_dispatch.cpp > " + dir + "/cc.log 2>&1";
    void* lib = nullptr;
    if (std::system(cmd.c_str())) {
        std::fprintf(stderr, "e2e: compiling failed:\n");
        std::system(("head -c 3000 " + dir + "/cc.log >&2").c_str());
    } else {
        lib = dlopen((dir + "/gen.so").c_str(), RTLD_NOW);
        if (!lib) std::fprintf(stderr, "e2e: dlopen: %s\n", dlerror());
    }
    std::system(("rm -rf '" + dir + "'").c_str());
    return lib;
}

// Two modules with their own images and overlapping offsets; a function of one calls a function of the other through a register.
static int two_modules() {
    std::vector<uint8_t> imga(0x2000, 0xcc), imgb(0x2000, 0xcc);
    const uint8_t fa[] = {0xff, 0xd7, 0x83, 0xc0, 0x01, 0xc3};  // 0x1000: call rdi; add eax,1; ret
    const uint8_t fb[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};  // 0x1000: mov eax,7; ret (same offset, other module)
    std::memcpy(imga.data() + 0x1000, fa, sizeof fa);
    std::memcpy(imgb.data() + 0x1000, fb, sizeof fb);
    const std::set<uint64_t> fns{0x1000};
    std::string code = lift_prelude() + lift_declarations(fns, "a") + lift_declarations(fns, "b");
    code += lift_function(discover_function(imga.data(), imga.size(), 0, 0x1000, sizeof fa), fns, "a").code;
    code += lift_function(discover_function(imgb.data(), imgb.size(), 0, 0x1000, sizeof fb), fns, "b").code;
    code += lift_function_table(fns, "a") + lift_function_table(fns, "b");
    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto rega = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register_a"));
    auto regb = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register_b"));
    auto run = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_a_1000"));
    if (!rega || !regb || !run) return std::fprintf(stderr, "e2e: missing symbols for the two-module program\n"), 1;
    rega(reinterpret_cast<unsigned long long>(imga.data()), imga.size());
    regb(reinterpret_cast<unsigned long long>(imgb.data()), imgb.size());
    alignas(16) static uint8_t stack[256];
    Ctx c{};
    const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
    c.r[dcp::rt::RSP] = sp - 8;
    c.r[dcp::rt::RDI] = reinterpret_cast<uint64_t>(imgb.data()) + 0x1000;  // module b's function
    run(&c);
    if (uint32_t(c.r[dcp::rt::RAX]) != 8 || c.r[dcp::rt::RSP] != sp) return std::fprintf(stderr, "e2e: two-module call returned %llu\n", (unsigned long long)c.r[dcp::rt::RAX]), 1;
    return 0;
}

// A computed jump through a table of absolute addresses (memcpy's size dispatch): both labels sit inside the function.
static int computed_jump() {
    std::vector<uint8_t> img(0x2000, 0xcc);
    const uint8_t f[] = {0x48, 0x8d, 0x0d, 0x1e, 0x00, 0x00, 0x00,      // 0x1000 lea rcx, [rip+0x1e]   (table at 0x1025)
                         0xff, 0x24, 0xf9,                              // 0x1007 jmp qword ptr [rcx+rdi*8]
                         0xb8, 0x0a, 0x00, 0x00, 0x00, 0xc3,            // 0x100a case 0: mov eax, 10; ret
                         0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,            // 0x1010..0x1015
                         0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};           // 0x1016 case 1: mov eax, 20; ret
    std::memcpy(img.data() + 0x1000, f, sizeof f);
    // the table at 0x1025.. holds absolute addresses, as the loader's relocation leaves them
    const uint64_t t0 = reinterpret_cast<uint64_t>(img.data()) + 0x100a, t1 = reinterpret_cast<uint64_t>(img.data()) + 0x1016;
    std::memcpy(img.data() + 0x1025, &t0, 8);
    std::memcpy(img.data() + 0x102d, &t1, 8);
    const std::set<uint64_t> fns{0x1000};
    std::string code = lift_prelude() + lift_declarations(fns);
    code += lift_function(discover_function(img.data(), img.size(), 0, 0x1000, 0x25), fns).code + lift_function_table(fns);
    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register"));
    auto run = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1000"));
    if (!reg || !run) return std::fprintf(stderr, "e2e: missing symbols for the computed jump\n"), 1;
    reg(reinterpret_cast<unsigned long long>(img.data()), img.size());
    alignas(16) static uint8_t stack[256];
    for (int which = 0; which < 2; ++which) {
        Ctx c{};
        const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
        c.r[dcp::rt::RSP] = sp - 8;
        c.r[dcp::rt::RDI] = uint64_t(which);
        run(&c);
        if (uint32_t(c.r[dcp::rt::RAX]) != (which ? 20u : 10u) || c.r[dcp::rt::RSP] != sp)
            return std::fprintf(stderr, "e2e: computed jump %d returned %llu\n", which, (unsigned long long)c.r[dcp::rt::RAX]), 1;
    }
    return 0;
}

// An entry hook runs with the entry state of the call, before the body: it sees the arguments and can change them.
static int entry_hook() {
    std::vector<uint8_t> img(0x2000, 0xcc);
    const uint8_t f[] = {0x8d, 0x47, 0x01, 0xc3};  // lea eax,[rdi+1]; ret
    std::memcpy(img.data() + 0x1000, f, sizeof f);
    std::memcpy(img.data() + 0x1010, f, sizeof f);
    const std::set<uint64_t> fns{0x1000, 0x1010}, hooks{0x1000};
    std::string code = lift_prelude() + lift_declarations(fns, "", hooks);
    for (uint64_t a : {0x1000, 0x1010})
        code += lift_function(discover_function(img.data(), img.size(), 0, a, sizeof f), fns, "", true, hooks).code;
    code += lift_function_table(fns, "", hooks);
    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register"));
    auto hooked = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1000")), plain = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1010"));
    auto slot = reinterpret_cast<void (**)(Ctx*)>(dlsym(lib, "dcp_hook_1000"));
    if (!reg || !hooked || !plain || !slot) return std::fprintf(stderr, "e2e: missing symbols for the entry hook\n"), 1;
    reg(reinterpret_cast<unsigned long long>(img.data()), img.size());
    alignas(16) static uint8_t stack[256];
    const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
    static uint64_t seen_rdi, seen_rsp;
    auto run = [&](void (*fn)(Ctx*), uint64_t rdi) {
        Ctx c{};
        c.r[dcp::rt::RSP] = sp - 8, c.r[dcp::rt::RDI] = rdi;
        fn(&c);
        return c.r[dcp::rt::RAX];
    };
    if (run(hooked, 5) != 6) return std::fprintf(stderr, "e2e: unset hook changed the result\n"), 1;
    *slot = [](Ctx* c) { seen_rdi = c->r[dcp::rt::RDI], seen_rsp = c->r[dcp::rt::RSP], c->r[dcp::rt::RDI] += 100; };
    if (run(hooked, 5) != 106 || seen_rdi != 5 || seen_rsp != sp - 8)
        return std::fprintf(stderr, "e2e: hook saw rdi=%llu or did not run before the body\n", (unsigned long long)seen_rdi), 1;
    seen_rdi = 0;
    if (run(plain, 5) != 6 || seen_rdi != 0) return std::fprintf(stderr, "e2e: an unhooked function ran the hook\n"), 1;
    return 0;
}

// A byte patch switches at run time: the original code with the flag clear, the replacement with it set. Covers a data change
// (an immediate) and a branch turned into a nop (the jump is then never taken).
static int byte_patches() {
    std::vector<uint8_t> img(0x2000, 0xcc);
    // 0x1000: mov eax,1; ret      0x1010: cmp edi,0; jne +6; mov eax,10; ret; mov eax,20; ret
    const uint8_t a[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
    const uint8_t b[] = {0x83, 0xff, 0x00, 0x75, 0x06, 0xb8, 0x0a, 0x00, 0x00, 0x00, 0xc3, 0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(img.data() + 0x1000, a, sizeof a);
    std::memcpy(img.data() + 0x1010, b, sizeof b);
    PatchMap patches;
    Insn ia, ib;
    const uint8_t ra[] = {0xb8, 0x07, 0x00, 0x00, 0x00}, rb[] = {0x66, 0x90};  // mov eax,7 ; 2-byte nop over the jne
    if (!decode(ra, sizeof ra, 0x1000, ia) || !decode(rb, sizeof rb, 0x1013, ib)) return std::fprintf(stderr, "e2e: patch decode\n"), 1;
    patches[0x1000] = {ia}, patches[0x1013] = {ib};
    const std::set<uint64_t> fns{0x1000, 0x1010};
    std::string code = lift_prelude() + lift_declarations(fns, "", {}, patches);
    code += lift_function(discover_function(img.data(), img.size(), 0, 0x1000, sizeof a), fns, "", true, {}, patches).code;
    code += lift_function(discover_function(img.data(), img.size(), 0, 0x1010, sizeof b), fns, "", true, {}, patches).code;
    code += lift_function_table(fns, "", {}, patches);
    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register"));
    auto f1 = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1000")), f2 = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1010"));
    auto s1 = reinterpret_cast<unsigned char*>(dlsym(lib, "dcp_patch_1000")), s2 = reinterpret_cast<unsigned char*>(dlsym(lib, "dcp_patch_1013"));
    if (!reg || !f1 || !f2 || !s1 || !s2) return std::fprintf(stderr, "e2e: missing symbols for the byte patches\n"), 1;
    reg(reinterpret_cast<unsigned long long>(img.data()), img.size());
    alignas(16) static uint8_t stack[256];
    const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
    auto run = [&](void (*fn)(Ctx*), uint64_t rdi) {
        Ctx c{};
        c.r[dcp::rt::RSP] = sp - 8, c.r[dcp::rt::RDI] = rdi;
        fn(&c);
        return uint32_t(c.r[dcp::rt::RAX]);
    };
    if (run(f1, 0) != 1 || run(f2, 3) != 20 || run(f2, 0) != 10) return std::fprintf(stderr, "e2e: unset patches changed the result\n"), 1;
    *s1 = 1;
    if (run(f1, 0) != 7 || run(f2, 3) != 20) return std::fprintf(stderr, "e2e: immediate patch\n"), 1;
    *s2 = 1;  // the jne is gone: rdi != 0 now falls through to mov eax,10
    if (run(f2, 3) != 10 || run(f2, 0) != 10) return std::fprintf(stderr, "e2e: branch patch\n"), 1;
    *s1 = *s2 = 0;
    if (run(f1, 0) != 1 || run(f2, 3) != 20) return std::fprintf(stderr, "e2e: patches did not switch off\n"), 1;
    return 0;
}

// A site hook runs before its instruction in the generated code: it can change registers, redirect to a basic block of the
// function, or return from it; with no hook set the function is unchanged.
static uint64_t g_site_calls = 0;
static dcp::rt::u64 site_redirect(Ctx*) { ++g_site_calls; return 1; }
static dcp::rt::u64 site_return(Ctx* c) { c->r[dcp::rt::RAX] = 99; return 1; }
static dcp::rt::u64 site_peek(Ctx* c) { c->r[dcp::rt::RAX] = 5; return 0; }  // result 0: the instruction runs

static int site_hooks() {
    std::vector<uint8_t> img(0x2000, 0xcc);
    // 0x1000: cmp edi,0; jne 0x100b; mov eax,10; ret; 0x100b: mov eax,20; ret
    const uint8_t b[] = {0x83, 0xff, 0x00, 0x75, 0x06, 0xb8, 0x0a, 0x00, 0x00, 0x00, 0xc3, 0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(img.data() + 0x1000, b, sizeof b);
    const std::set<uint64_t> fns{0x1000};
    const SiteMap sites{{0x1003, 0x100b}, {0x1005, kSiteRet}, {0x100b, kSiteNoTarget}};
    std::string code = lift_prelude() + lift_declarations(fns, "", {}, {}, sites);
    code += lift_function(discover_function(img.data(), img.size(), 0, 0x1000, sizeof b), fns, "", true, {}, {}, sites).code;
    code += lift_function_table(fns, "", {}, {}, sites);
    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register"));
    auto f = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1000"));
    if (!reg || !f) return std::fprintf(stderr, "e2e: missing symbols for the site hooks\n"), 1;
    reg(reinterpret_cast<unsigned long long>(img.data()), img.size());
    alignas(16) static uint8_t stack[256];
    const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
    auto run = [&](uint64_t rdi) {
        Ctx c{};
        c.r[dcp::rt::RSP] = sp - 8, c.r[dcp::rt::RDI] = rdi;
        f(&c);
        return std::make_pair(uint32_t(c.r[dcp::rt::RAX]), c.r[dcp::rt::RSP]);
    };
    if (run(3).first != 20 || run(0).first != 10) return std::fprintf(stderr, "e2e: unset site hooks changed the result\n"), 1;
    if (!dcp::rt::set_site_hook("", 0x1003, site_redirect)) return std::fprintf(stderr, "e2e: site slot not registered\n"), 1;
    if (run(0).first != 20 || g_site_calls != 1) return std::fprintf(stderr, "e2e: redirect to the block\n"), 1;  // rdi 0 would give 10
    dcp::rt::set_site_hook("", 0x1003, nullptr);
    dcp::rt::set_site_hook("", 0x1005, site_return);
    auto r = run(0);
    if (r.first != 99 || r.second != sp) return std::fprintf(stderr, "e2e: ret target (eax %u, rsp %llx)\n", r.first, (unsigned long long)r.second), 1;
    dcp::rt::set_site_hook("", 0x1005, nullptr);
    dcp::rt::set_site_hook("", 0x100b, site_peek);
    if (run(3).first != 20) return std::fprintf(stderr, "e2e: a zero result must run the instruction\n"), 1;  // mov eax,20 overwrites the 5
    dcp::rt::set_site_hook("", 0x100b, nullptr);
    if (run(3).first != 20 || run(0).first != 10) return std::fprintf(stderr, "e2e: site hooks did not switch off\n"), 1;
    return 0;
}

int main() {
    std::vector<uint8_t> img(0x2000, 0xcc);
    // 0x1000: lea rcx,[rip+0xf9] (-> 0x1100); call rcx; add eax,1; ret
    const uint8_t f1[] = {0x48, 0x8d, 0x0d, 0xf9, 0x00, 0x00, 0x00, 0xff, 0xd1, 0x83, 0xc0, 0x01, 0xc3};
    // 0x1100: mov eax,7; ret
    const uint8_t f2[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(img.data() + 0x1000, f1, sizeof f1);
    std::memcpy(img.data() + 0x1100, f2, sizeof f2);

    const std::set<uint64_t> fns{0x1000, 0x1100};
    std::string code = lift_prelude() + lift_declarations(fns);
    const struct { uint64_t start, size; } ranges[] = {{0x1000, sizeof f1}, {0x1100, sizeof f2}};
    for (auto& r : ranges) {
        Function fn = discover_function(img.data(), img.size(), 0, r.start, r.size);
        LiftResult lr = lift_function(fn, fns);
        if (fn.has_bad_bytes || !lr.unsupported.empty()) return std::fprintf(stderr, "e2e: function 0x%llx did not lift\n", (unsigned long long)r.start), 1;
        code += lr.code;
    }
    code += lift_function_table(fns);

    void* lib = build_and_load(code);
    if (!lib) return 1;
    auto reg = reinterpret_cast<void (*)(unsigned long long, unsigned long long)>(dlsym(lib, "dcp_register"));
    auto run = reinterpret_cast<void (*)(Ctx*)>(dlsym(lib, "f_1000"));
    if (!reg || !run) return std::fprintf(stderr, "e2e: missing dcp_register or f_1000\n"), 1;

    reg(reinterpret_cast<unsigned long long>(img.data()), img.size());
    alignas(16) static uint8_t stack[256];
    Ctx c{};
    const uint64_t sp = reinterpret_cast<uint64_t>(stack) + sizeof stack;
    c.r[dcp::rt::RSP] = sp - 8;  // as if the caller had pushed a return address
    run(&c);
    int fail = 0;
    if (uint32_t(c.r[dcp::rt::RAX]) != 8) std::fprintf(stderr, "e2e: eax = %llu, expected 8\n", (unsigned long long)c.r[dcp::rt::RAX]), ++fail;
    if (c.r[dcp::rt::RSP] != sp) std::fprintf(stderr, "e2e: rsp not restored\n"), ++fail;
    if (fail || two_modules() || computed_jump() || entry_hook() || byte_patches() || site_hooks()) return 1;
    std::puts("dispatch e2e: ok");
    return 0;
}
