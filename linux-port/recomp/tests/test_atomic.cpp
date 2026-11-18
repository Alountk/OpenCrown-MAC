// The runtime's atomic read-modify-write really is atomic: many threads hammer one location. No game data.
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "dcp_rt.h"

using namespace dcp::rt;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    alignas(16) static u64 counter64 = 0;
    alignas(16) static u32 counter32 = 0;
    alignas(16) static u8 bytes[16] = {};
    alignas(16) static u64 pair[2] = {0, 0};
    constexpr int kThreads = 8, kIters = 50000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&] {
            Flags fl;
            for (int i = 0; i < kIters; ++i) {
                atomic_rmw<u64>(reinterpret_cast<u64>(&counter64), [&](u64 o) { return add<u64>(o, 1, fl); });
                atomic_rmw<u32>(reinterpret_cast<u64>(&counter32), [&](u32 o) { return add<u32>(o, 3, fl); });
                atomic_rmw<u8>(reinterpret_cast<u64>(&bytes[5]), [&](u8 o) { return add<u8>(o, 1, fl); });
                // a 16-byte compare-exchange loop incrementing both halves together
                for (;;) {
                    u64 e[2] = {pair[0], pair[1]};  // may be torn: the compare-exchange below catches it
                    u64 d[2] = {e[0] + 1, e[1] + 2};
                    if (atomic_cas128(reinterpret_cast<u64>(pair), e, d)) break;
                }
            }
        });
    for (auto& t : ts) t.join();
    CHECK(counter64 == u64(kThreads) * kIters);
    CHECK(counter32 == u32(kThreads * kIters * 3));
    CHECK(bytes[5] == u8(kThreads * kIters));
    CHECK(pair[0] == u64(kThreads) * kIters && pair[1] == u64(kThreads) * kIters * 2);

    // old value returned; atomic_cas reports success and the actual value on failure
    u64 v = 7;
    CHECK(atomic_rmw<u64>(reinterpret_cast<u64>(&v), [](u64 o) { return o * 2; }) == 7 && v == 14);
    u64 expected = 14;
    CHECK(atomic_cas<u64>(reinterpret_cast<u64>(&v), expected, 99) && v == 99);
    expected = 5;
    CHECK(!atomic_cas<u64>(reinterpret_cast<u64>(&v), expected, 1) && expected == 99 && v == 99);
    if (g_fail) return std::fprintf(stderr, "atomic: %d check(s) failed\n", g_fail), 1;
    std::puts("atomic: all tests passed");
    return 0;
}
