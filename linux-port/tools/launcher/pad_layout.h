// Controls page, controller view: where each input.ini action sits on the drawn controller (the front view and the top view, the PNGs
// in assets/). Pure data and text helpers (no ImGui), so a test keeps them in step with the actions of input.ini.
#pragma once
#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

namespace dcp::padlayout {

enum class View { Front, Top };

struct Zone {
    const char* action;  // input.ini binding name
    const char* label;   // name of the button, printed next to its key
    View view;
    float cx, cy;        // the point on the button the label's leader line ends at, in image pixels of its view
    float hx, hy, rx, ry, rd;  // the button's area, coloured while its label is hovered: centre, half sizes, corner radius
};

// The two views are line drawings (assets/pad_front.png, pad_top.png). Zones are placed in image pixels; the frames are the image sizes.
constexpr float kFrontW = 1145, kFrontH = 717, kTopW = 1195, kTopH = 487;

inline const std::vector<Zone>& zones() {
    static const std::vector<Zone> z = {
        // front view
        {"dpad_up", "D-pad up", View::Front, 233, 147, 233, 147, 30, 38, 14},
        {"dpad_left", "D-pad left", View::Front, 175, 210, 175, 210, 38, 30, 14},
        {"dpad_right", "D-pad right", View::Front, 290, 210, 290, 210, 38, 30, 14},
        {"dpad_down", "D-pad down", View::Front, 233, 272, 233, 272, 30, 38, 14},
        {"triangle", "Triangle", View::Front, 912, 131, 912, 131, 35, 35, 35},
        {"square", "Square", View::Front, 832, 211, 832, 211, 35, 35, 35},
        {"circle", "Circle", View::Front, 994, 211, 994, 211, 35, 35, 35},
        {"cross", "Cross", View::Front, 912, 291, 912, 291, 35, 35, 35},
        {"touchpad", "Touchpad", View::Front, 500, 152, 573, 153, 187, 93, 20},
        {"options", "Options", View::Front, 796, 105, 796, 105, 17, 32, 17},
        {"move_up", "Move up", View::Front, 396, 299, 396, 299, 20, 20, 20},
        {"move_left", "Move left", View::Front, 336, 359, 336, 359, 20, 20, 20},
        {"l3", "L3 (press)", View::Front, 396, 359, 396, 359, 42, 42, 42},
        {"move_right", "Move right", View::Front, 456, 359, 456, 359, 20, 20, 20},
        {"move_down", "Move down", View::Front, 396, 419, 396, 419, 20, 20, 20},
        {"look_up", "Look up", View::Front, 749, 299, 749, 299, 20, 20, 20},
        {"look_left", "Look left", View::Front, 689, 359, 689, 359, 20, 20, 20},
        {"r3", "R3 (press)", View::Front, 749, 359, 749, 359, 42, 42, 42},
        {"look_right", "Look right", View::Front, 809, 359, 809, 359, 20, 20, 20},
        {"look_down", "Look down", View::Front, 749, 419, 749, 419, 20, 20, 20},
        // top view: the shoulder buttons (L on the left, as seen from the player's side)
        {"l2", "L2", View::Top, 214, 110, 218, 93, 68, 62, 40},   {"l1", "L1", View::Top, 211, 221, 209, 233, 85, 42, 30},
        {"r2", "R2", View::Top, 979, 110, 977, 93, 68, 62, 40},   {"r1", "R1", View::Top, 979, 221, 986, 233, 85, 42, 30},
    };
    return z;
}

// Which column a zone's label goes in: left of its image, or right of it.
inline bool label_on_left(const Zone& z) { return z.cx < (z.view == View::Front ? kFrontW : kTopW) * 0.5f; }


// Vertical positions for a column of labels: `want` are the ideal ones (the buttons' heights), the result (same order) keeps their
// order, leaves at least `gap` between neighbours and stays within [lo, hi], moving each label as little as it can. When the column
// is too full for `gap`, the labels are spread evenly instead.
inline std::vector<float> spread(const std::vector<float>& want, float gap, float lo, float hi) {
    const size_t n = want.size();
    std::vector<float> out(n);
    if (n == 0) return out;
    std::vector<size_t> ord(n);
    std::iota(ord.begin(), ord.end(), size_t(0));
    std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return want[a] < want[b]; });
    if (n > 1 && gap * float(n - 1) > hi - lo) gap = (hi - lo) / float(n - 1);
    std::vector<float> p(n);
    for (size_t i = 0; i < n; ++i) p[i] = std::max(want[ord[i]], i ? p[i - 1] + gap : lo);
    p[n - 1] = std::min(p[n - 1], hi);
    for (size_t i = n - 1; i-- > 0;) p[i] = std::min(p[i], p[i + 1] - gap);
    for (size_t i = 0; i < n; ++i) out[ord[i]] = p[i];
    return out;
}

// Controller setup (pad_mapping.h): the button of the drawing each SDL mapping step asks the user to press; nullptr when unknown.
// The stick steps point at the direction they ask for.
inline const char* action_for_step(const std::string& target) {
    static const std::vector<std::pair<const char*, const char*>> m = {
        {"a", "cross"},         {"b", "circle"},       {"x", "square"},       {"y", "triangle"},
        {"leftshoulder", "l1"}, {"rightshoulder", "r1"}, {"lefttrigger", "l2"}, {"righttrigger", "r2"},
        {"back", "touchpad"},   {"start", "options"},  {"leftstick", "l3"},  {"rightstick", "r3"},
        {"dpup", "dpad_up"},    {"dpdown", "dpad_down"}, {"dpleft", "dpad_left"}, {"dpright", "dpad_right"},
        {"leftx", "move_right"}, {"lefty", "move_down"}, {"rightx", "look_right"}, {"righty", "look_down"},
    };
    for (const auto& [step, action] : m)
        if (target == step) return action;
    return nullptr;
}

// input.ini bindings that are not a button of the controller: they stay in the list view.
inline const std::vector<const char*>& not_on_controller() {
    static const std::vector<const char*> n = {"town_map", "fairy", "fairy_toggle", "fairy_tap", "pointer_tap", "chat"};
    return n;
}

// A binding value (as in input.ini) shortened to fit on a button: "Left Shift" -> "L-Shift", "Mouse1" -> "LMB". Several inputs
// ("a, b", from a hand-edited file) show the first one and a "+".
inline std::string short_key(const std::string& value) {
    if (size_t comma = value.find(','); comma != std::string::npos) {
        std::string first = value.substr(0, comma);
        while (!first.empty() && first.back() == ' ') first.pop_back();
        return short_key(first) + "+";
    }
    static const std::vector<std::pair<std::string, std::string>> k = {
        {"Mouse1", "LMB"},      {"Mouse2", "RMB"},      {"Mouse3", "MMB"},       {"Left Shift", "L-Shift"}, {"Right Shift", "R-Shift"},
        {"Left Ctrl", "L-Ctrl"}, {"Right Ctrl", "R-Ctrl"}, {"Left Alt", "L-Alt"}, {"Right Alt", "R-Alt"},   {"Escape", "Esc"},
        {"Backspace", "Bksp"},  {"Return", "Enter"},    {"Left GUI", "L-Cmd"},   {"Right GUI", "R-Cmd"},
    };
    for (const auto& [from, to] : k)
        if (value == from) return to;
    return value;
}

}  // namespace dcp::padlayout
