// Gamepad quit combo (tools/dcp_run/quit_combo.h): Select + Start held for a while ends the game; a tap or a release resets it.
#include <cstdio>

#include "../tools/dcp_run/quit_combo.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; } while (0)

int main() {
    using dcp::input::HoldCombo;
    HoldCombo h(2000);
    CHECK(!h.update(false, 0));
    CHECK(!h.update(true, 100));      // pressed: the clock starts here
    CHECK(!h.update(true, 2099));     // 1999 ms held
    CHECK(h.update(true, 2100));      // 2000 ms held: fires
    CHECK(!h.update(true, 5000));     // fires once per hold
    CHECK(!h.update(false, 5100));    // released
    CHECK(!h.update(true, 5200));     // a new hold starts from its own press
    CHECK(h.update(true, 7200));
    // A tap, or a gap in the middle, never adds up.
    HoldCombo t(2000);
    CHECK(!t.update(true, 0));
    CHECK(!t.update(true, 1500));
    CHECK(!t.update(false, 1600));    // released before the time
    CHECK(!t.update(true, 1700));
    CHECK(!t.update(true, 3500));     // 1800 ms since the second press
    CHECK(t.update(true, 3700));
    if (!g_fail) std::puts("quit combo: all tests passed");
    return g_fail ? 1 : 0;
}
