// Busy-wait throttle for scePthreadYield / sched_yield.
//
// The game's main thread waits for other work by calling yield in a tight loop (about 60% of its samples in a profile on an
// M2 Pro): the loop burns a whole core to do nothing. A thread that keeps yielding is slowed down with short sleeps; a thread
// that yields now and then, or does real work between yields, is never touched.
#pragma once
#include <cstdint>

namespace dcp::hle {

class YieldThrottle {
public:
    enum class Action { Yield, Sleep };
    static constexpr int kSpinYields = 64;                  // plain yields before the first sleep: a quick handoff stays quick
    static constexpr uint64_t kQuietNs = 500 * 1000;        // a gap this long between two calls is real work: start over
    static constexpr uint64_t kSleepNs = 50 * 1000;         // each throttled call sleeps this long

    // Called at every yield with the current monotonic time; says what the caller should do.
    Action next(uint64_t now_ns) {
        if (now_ns - last_ > kQuietNs) run_ = 0;
        last_ = now_ns;
        return run_++ < kSpinYields ? Action::Yield : Action::Sleep;
    }

private:
    uint64_t last_ = 0;
    int run_ = 0;
};

}  // namespace dcp::hle
