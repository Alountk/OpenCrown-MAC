// Gamepad quit combo (macOS): holding Select + Start for a while ends the game, for players who have no mouse to close the window.
#pragma once
#include <cstdint>

namespace dcp::input {

// Fires once per hold: update(held, now_ms) is true on the call that finds the combo held for hold_ms; a release starts over.
class HoldCombo {
public:
    explicit HoldCombo(uint64_t hold_ms) : hold_ms_(hold_ms) {}
    bool update(bool held, uint64_t now_ms) {
        if (!held) return since_ = kIdle, fired_ = false;
        if (since_ == kIdle) since_ = now_ms;
        if (fired_ || now_ms - since_ < hold_ms_) return false;
        return fired_ = true;
    }

private:
    static constexpr uint64_t kIdle = ~0ull;
    uint64_t hold_ms_, since_ = kIdle;
    bool fired_ = false;
};

}  // namespace dcp::input
