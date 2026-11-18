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

    char tmpl[] = "/tmp/dcp_e2e_XXXXXX";
    const char* t = getenv("TMPDIR");
    std::string dir_t = std::string(t ? t : "/tmp") + "/dcp_e2e_XXXXXX";
    std::vector<char> dv(dir_t.begin(), dir_t.end());
    dv.push_back(0);
    (void)tmpl;
    if (!mkdtemp(dv.data())) return std::puts("e2e: skipped (no temp dir)"), 0;
    const std::string dir = dv.data();
    { std::ofstream(dir + "/gen.cpp") << code; }
    const std::string cmd = std::string(DCP_TEST_CXX) + " " + DCP_TEST_ARCH + " -std=c++20 -O1 -shared -fPIC -I" DCP_TEST_RT_DIR " -o " +
                            dir + "/gen.so " + dir + "/gen.cpp " DCP_TEST_RT_DIR "/dcp_dispatch.cpp > " + dir + "/cc.log 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc) {
        std::fprintf(stderr, "e2e: compiling failed:\n");
        std::system(("head -c 3000 " + dir + "/cc.log >&2").c_str());
        std::system(("rm -rf '" + dir + "'").c_str());
        return 1;
    }
    void* lib = dlopen((dir + "/gen.so").c_str(), RTLD_NOW);
    std::system(("rm -rf '" + dir + "'").c_str());
    if (!lib) return std::fprintf(stderr, "e2e: dlopen: %s\n", dlerror()), 1;
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
    if (fail) return 1;
    std::puts("dispatch e2e: ok");
    return 0;
}
