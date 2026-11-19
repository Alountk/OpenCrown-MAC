// The yield throttle (src/hle/kernel/yield_throttle.h): a thread that keeps yielding in a tight loop (a busy wait) is slowed
// down with short sleeps; a thread that yields now and then is not touched.
#include <cstdint>
#include <cstdio>

#include "../src/hle/kernel/yield_throttle.h"

using dcp::hle::YieldThrottle;
using Action = YieldThrottle::Action;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; } while (0)

int main() {
    constexpr uint64_t us = 1000;
    {  // A tight loop: the first calls just yield (a quick handoff stays quick), then it sleeps.
        YieldThrottle t;
        uint64_t now = 1000 * us;
        int yields = 0;
        Action a;
        do { a = t.next(now); now += 2 * us; yields += a == Action::Yield; } while (a == Action::Yield && yields < 1000);
        CHECK(yields == YieldThrottle::kSpinYields);
        CHECK(t.next(now) == Action::Sleep);  // and keeps sleeping while the loop goes on
        now += 60 * us;
        CHECK(t.next(now) == Action::Sleep);
    }
    {  // Real work between the yields (more than kQuietNs apart) restarts the count: never throttled.
        YieldThrottle t;
        uint64_t now = 0;
        bool slept = false;
        for (int i = 0; i < 10000; ++i) {
            now += YieldThrottle::kQuietNs + 1;
            slept |= t.next(now) == Action::Sleep;
        }
        CHECK(!slept);
    }
    {  // After a quiet gap a throttled loop starts over with plain yields.
        YieldThrottle t;
        uint64_t now = 0;
        for (int i = 0; i < YieldThrottle::kSpinYields + 5; ++i) t.next(now += 2 * us);
        CHECK(t.next(now += 2 * us) == Action::Sleep);
        now += YieldThrottle::kQuietNs + 1;
        CHECK(t.next(now) == Action::Yield);
    }
    if (!g_fail) std::puts("yield throttle: all tests passed");
    return g_fail ? 1 : 0;
}
