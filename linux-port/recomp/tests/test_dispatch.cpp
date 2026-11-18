// Runtime dispatch of recompiled code: guest address -> function table, indirect calls and jumps, foreign targets.
// Plain C++ with no x86 involved: runs natively on arm64 as well. No game data.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dcp_dispatch.h"

using namespace dcp::rt;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void fn_a(Ctx* c) { c->r[RAX] += 1; }
static void fn_b(Ctx* c) { c->r[RAX] += 10; }
static void fn_c(Ctx* c) { c->r[RAX] += 100; }

static u64 g_foreign_target = 0;
static int g_foreign_calls = 0;
static void foreign(Ctx* c, u64 target) { g_foreign_target = target, ++g_foreign_calls, c->r[RAX] = 0xf0; }

alignas(16) static unsigned char g_image[0x1000];

static void test_lookup_and_dispatch() {
    const FnEntry table[] = {{0x10, fn_a}, {0x40, fn_b}, {0x800, fn_c}};
    set_image(reinterpret_cast<u64>(g_image), sizeof g_image);
    set_function_table(table, 3);
    set_foreign_handler(foreign);
    const u64 base = reinterpret_cast<u64>(g_image);

    CHECK(find_function(base + 0x10) == fn_a && find_function(base + 0x40) == fn_b && find_function(base + 0x800) == fn_c);
    CHECK(find_function(base + 0x11) == nullptr);   // inside the image but not a function entry
    CHECK(find_function(base - 1) == nullptr);      // before the image
    CHECK(find_function(base + sizeof g_image) == nullptr);  // one past the end
    CHECK(find_function(0) == nullptr);

    Ctx c{};
    call_ind(&c, base + 0x40);
    CHECK(c.r[RAX] == 10 && g_foreign_calls == 0);
    call_va(&c, 0x800);  // a guest virtual address: the image base is added
    CHECK(c.r[RAX] == 110 && g_foreign_calls == 0);
    jmp_ind(&c, base + 0x10);
    CHECK(c.r[RAX] == 111 && g_foreign_calls == 0);

    // A pointer that is not a recompiled function goes to the foreign handler with the host pointer unchanged.
    call_ind(&c, base + 0x11);
    CHECK(g_foreign_calls == 1 && g_foreign_target == base + 0x11 && c.r[RAX] == 0xf0);
    const u64 outside = 0x1234560;
    jmp_ind(&c, outside);
    CHECK(g_foreign_calls == 2 && g_foreign_target == outside);
    call_va(&c, 0x7ff0);  // a guest address no function starts at
    CHECK(g_foreign_calls == 3 && g_foreign_target == base + 0x7ff0);
}

static void test_large_table() {
    // The binary search must find every entry and reject every neighbour, whatever the spacing.
    std::vector<FnEntry> table;
    for (u32 off = 0x20; table.size() < 5000; off += 7 + (off * 2654435761u >> 28)) table.push_back({off, off % 2 ? fn_a : fn_b});
    set_image(0x100000000ull, 0x10000000);
    set_function_table(table.data(), table.size());
    size_t found = 0, spurious = 0;
    for (const FnEntry& e : table) {
        found += find_function(0x100000000ull + e.off) == e.fn;
        spurious += find_function(0x100000000ull + e.off + 1) != nullptr && (e.off + 1 != (&e + 1 < table.data() + table.size() ? (&e + 1)->off : 0));
    }
    CHECK(found == table.size() && spurious == 0);
}

int main() {
    test_lookup_and_dispatch();
    test_large_table();
    if (g_fail) return std::fprintf(stderr, "dispatch: %d check(s) failed\n", g_fail), 1;
    std::puts("dispatch: all tests passed");
    return 0;
}
