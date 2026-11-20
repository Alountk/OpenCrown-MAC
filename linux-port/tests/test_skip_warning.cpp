// The no-skip warning state machine (src/hle/skip_warning.h): the first "no" to the story skip asks again with a warning
// text, the second answer is final, a "yes" starts over.
#include <cstdio>
#include <cstring>

#include "../src/hle/skip_warning.h"

static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

using dcp::skip_warning::Warning;
constexpr uint32_t kWarningId = 0x3cc;  // the message the story skip question shows

int main() {
    Warning w;
    // Nothing asked yet: the lookup of the question text is the game's own.
    CHECK(w.on_lookup(kWarningId) == nullptr);

    // First "no" (0 = cancel, 3 = no): ask again; the next lookup of the question text returns the warning, once.
    CHECK(w.on_no(0));
    CHECK(w.on_lookup(0x111) == nullptr);  // another message is not ours and does not use the warning up
    const char* t = w.on_lookup(kWarningId);
    CHECK(t && std::strstr(t, "online") && std::strstr(t, "talisman"));
    CHECK(w.on_lookup(kWarningId) == nullptr);  // the second question after that lookup is built from the game's text again

    // Second "no": final, the game goes on to state 8; the next "no" (a new question) is a first one again.
    CHECK(!w.on_no(3));
    CHECK(w.on_no(3));

    // Answers that are not a "no" change nothing.
    Warning v;
    CHECK(!v.on_no(1) && !v.on_no(2) && !v.on_no(4));
    CHECK(v.on_lookup(kWarningId) == nullptr);

    // A "yes" after the warning starts over: the next "no" is a first one.
    Warning y;
    CHECK(y.on_no(0));
    y.on_yes();
    CHECK(y.on_lookup(kWarningId) == nullptr);
    CHECK(y.on_no(0));

    // The text goes through the game's vsnprintf: no conversion character, ASCII and newlines only.
    for (const char* p = Warning::text(); *p; ++p) CHECK(*p != '%' && (*p == '\n' || (*p >= 0x20 && *p < 0x7f)));
    if (g_fail) return 1;
    std::puts("skip_warning: all tests passed");
    return 0;
}
