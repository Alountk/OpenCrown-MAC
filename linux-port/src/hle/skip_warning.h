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
#include <cstdlib>

namespace dcp::skip_warning {

class Warning {
public:
    // The question text in the game's language (`lang` = $DCP_LANGUAGE: en / fr / es / de / it; anything else is English).
    // UTF-8 as the game's own sheets are (their accents are in its font) and '\n', no '%': the game runs the text through
    // vsnprintf as a format. The English one is ASCII only. Each fits the 0xf0 bytes of the x86 relay page.
    static const char* text(const char* lang = nullptr) {
        auto is = [&](const char* c) { return lang && lang[0] == c[0] && lang[1] == c[1] && !lang[2]; };
        if (is("es"))
            return "Sin el salto, este personaje no puede jugar en línea\n"
                   "hasta terminar la primera mitad de la historia.\n"
                   "¿Saltar a la recogida de talismanes? (Empiezas en LV15)";
        if (is("fr"))
            return "Sans le saut, ce personnage ne peut pas jouer en ligne\n"
                   "avant la fin de la première moitié de l'histoire.\n"
                   "Passer à la collecte des talismans ? (Début au LV15)";
        if (is("de"))
            return "Ohne das Überspringen kann dieser Charakter erst nach\n"
                   "der ersten Hälfte der Geschichte online spielen.\n"
                   "Zur Talisman-Suche springen? (Start mit LV15)";
        if (is("it"))
            return "Senza il salto, questo personaggio non può giocare online\n"
                   "finché non finisce la prima metà della storia.\n"
                   "Saltare alla raccolta dei talismani? (Inizi al LV15)";
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
        return text(std::getenv("DCP_LANGUAGE"));
    }

private:
    std::atomic<bool> text_due_{false}, in_second_{false};
};

}  // namespace dcp::skip_warning
