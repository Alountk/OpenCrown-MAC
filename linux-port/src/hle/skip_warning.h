// The no-skip warning of the story skip question (skip_offers.h, story_progress.md 9) as a plain state machine, so the
// recompiled build can drive it from site hooks (hooks/eboot_patches.txt) and a test can run it without the game.
//
// The story skip question is state 9 of FUN_003adbf0. The first "no" (answer 0 = cancel, 3 = no) asks again with a warning
// text in place of the question text, because online needs the story counter the skip sets; the second answer is final.
//   on_no(answer)   at the answer test: true = ask again (the caller redirects to SetState(kind, 9)), false = as the game
//   on_yes()        a "yes": start over
//   on_lookup(id)   the message lookup: the warning text once after a first "no" when the game asks for the question text
#pragma once
#include <atomic>
#include <cstdint>

namespace dcp::skip_warning {

class Warning {
public:
    // ASCII and '\n' only, no '%': the game runs the text through vsnprintf as a format.
    static const char* text() {
        return "Without the skip, this character can't play online\n"
               "until the first half of the story is finished.\n"
               "Skip to the talisman collecting? (Player starts at LV15)";
    }

    static constexpr uint32_t kQuestionId = 0x3cc;  // SystemMsg of the story skip question

    bool on_no(uint32_t answer) {
        if (answer != 0 && answer != 3) return false;
        if (in_second_) {  // the second answer is final: the next question starts over
            in_second_ = false;
            return false;
        }
        text_due_ = in_second_ = true;
        return true;
    }

    void on_yes() { text_due_ = in_second_ = false; }

    const char* on_lookup(uint32_t id) {
        if (id != kQuestionId || !text_due_.exchange(false)) return nullptr;
        return text();
    }

private:
    std::atomic<bool> text_due_{false}, in_second_{false};
};

}  // namespace dcp::skip_warning
