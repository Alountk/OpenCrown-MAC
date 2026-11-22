// Controls page, controller view (tools/launcher/pad_layout.h): the zones on the controller images must match input.ini's actions,
// stay apart and inside their image (whose size the layout's frame constants must state); long key names are shortened.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "../tools/common/input_defaults.h"
#include "../tools/common/pad_mapping.h"
#include "../tools/launcher/pad_layout.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; } while (0)

// Width and height of a PNG file (its IHDR chunk), 0 x 0 when it cannot be read.
static void png_size(const std::string& path, uint32_t& w, uint32_t& h) {
    unsigned char hd[24] = {};
    std::ifstream f(path, std::ios::binary);
    f.read(reinterpret_cast<char*>(hd), sizeof hd);
    w = h = 0;
    if (f.gcount() != 24 || hd[1] != 'P' || hd[2] != 'N' || hd[3] != 'G') return;
    for (int i = 0; i < 4; ++i) w = w << 8 | hd[16 + i], h = h << 8 | hd[20 + i];
}

int main() {
    using namespace dcp::padlayout;
    // The binding names of the default input.ini (settings are not bindings).
    std::set<std::string> bindings;
    std::istringstream in(dcp::kInputDefaults);
    std::string line;
    const std::set<std::string> settings = {"fairy_speed", "touch_screen", "deck_trackpads", "touch_as_mouse", "mouse_attacks"};
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (!k.empty() && !settings.count(k)) bindings.insert(k);
    }
    CHECK(bindings.size() > 20);

    for (const Kind kind : {Kind::Ps4, Kind::Xbox}) {
        const auto& zs = zones(kind);
        std::set<std::string> drawn;
        for (const Zone& z : zs) {
            CHECK(drawn.insert(z.action).second);   // one zone per action
            CHECK(bindings.count(z.action) == 1);   // and it is a real binding
            const Frame f = frame(kind, z.view);
            CHECK(z.cx > 0 && z.cy > 0 && z.cx < f.w && z.cy < f.h);
            CHECK(z.label && z.label[0]);
            // The coloured area is a real shape inside the image, and the line ends on it.
            CHECK(z.rx > 5 && z.ry > 5 && z.rd > 0 && z.rd <= std::fmin(z.rx, z.ry) + 1e-3f);
            CHECK(z.hx - z.rx >= 0 && z.hx + z.rx <= f.w && z.hy - z.ry >= 0 && z.hy + z.ry <= f.h);
            CHECK(std::fabs(z.cx - z.hx) <= z.rx + 1e-3f && std::fabs(z.cy - z.hy) <= z.ry + 1e-3f);
        }
        // Every binding is either on the controller or listed as one that is not a controller button.
        for (const std::string& b : bindings) {
            bool listed = false;
            for (const char* n : not_on_controller()) listed = listed || b == n;
            CHECK(drawn.count(b) != (listed ? 1u : 0u));
        }
        // Zones are the ends of leader lines: two anchors in one view are never on the same spot (4.3% of the image width apart).
        for (size_t i = 0; i < zs.size(); ++i)
            for (size_t j = i + 1; j < zs.size(); ++j) {
                if (zs[i].view != zs[j].view) continue;
                const float dx = std::fabs(zs[i].cx - zs[j].cx), dy = std::fabs(zs[i].cy - zs[j].cy);
                const float least = 0.043f * frame(kind, zs[i].view).w;
                const bool apart = dx >= least || dy >= least;
                if (!apart) std::printf("too close: %s / %s\n", zs[i].action, zs[j].action);
                CHECK(apart);
            }

        // The images: the frames are their pixel sizes (the zones are placed in them).
        for (const View v : {View::Front, View::Top}) {
            uint32_t w = 0, h = 0;
            png_size(std::string(DCP_PAD_ASSETS_DIR) + "/" + image_file(kind, v), w, h);
            CHECK(w == frame(kind, v).w && h == frame(kind, v).h);
        }

        // Label columns: left or right by where the button is; each side of each view has room to be told apart.
        int left_front = 0, right_front = 0;
        for (const Zone& z : zs)
            if (z.view == View::Front) (label_on_left(kind, z) ? left_front : right_front)++;
        CHECK(left_front >= 8 && right_front >= 8);
        const Zone *l1 = nullptr, *r1 = nullptr;
        for (const Zone& z : zs) {
            if (std::string(z.action) == "l1") l1 = &z;
            if (std::string(z.action) == "r1") r1 = &z;
        }
        CHECK(l1 && r1 && label_on_left(kind, *l1) && !label_on_left(kind, *r1));  // L on the left of the top view, as the player sees it

        // Controller setup: every step asks for a button that has a zone.
        for (const auto& step : dcp::padmap::kSteps) {
            const char* act = action_for_step(step.target);
            bool found = false;
            for (const Zone& z : zs) found = found || (act && std::string(z.action) == act);
            CHECK(found);
        }
    }
    // The two controllers name their buttons their own way.
    auto label_of = [](Kind k, const char* action) {
        for (const Zone& z : zones(k))
            if (std::string(z.action) == action) return std::string(z.label);
        return std::string();
    };
    CHECK(label_of(Kind::Ps4, "cross") == "Cross" && label_of(Kind::Xbox, "cross") == "A");
    CHECK(label_of(Kind::Ps4, "l2") == "L2" && label_of(Kind::Xbox, "l2") == "LT");
    CHECK(label_of(Kind::Ps4, "touchpad") == "Touchpad" && label_of(Kind::Xbox, "touchpad") == "View");
    CHECK(label_of(Kind::Xbox, "options") == "Menu");

    // spread(): order kept, gaps respected, inside the column, moved as little as possible.
    {
        auto gaps_ok = [](std::vector<float> v, float gap) {
            std::sort(v.begin(), v.end());
            for (size_t i = 1; i < v.size(); ++i) if (v[i] - v[i - 1] < gap - 1e-3f) return false;
            return true;
        };
        std::vector<float> r = spread({100, 105, 110, 400}, 30, 0, 600);  // a crowd of three and a loner
        CHECK(gaps_ok(r, 30) && r[3] == 400 && r[0] < r[1] && r[1] < r[2]);
        CHECK(std::fabs((r[0] + r[1] + r[2]) / 3 - 105) < 1e-3f + 30);  // the crowd stays around its ideal place
        r = spread({5, 6, 7}, 30, 0, 100);  // pushed down by the top edge
        CHECK(r[0] >= 0 && r[2] <= 100 && gaps_ok(r, 30));
        r = spread({95, 96, 97}, 30, 0, 100);  // pushed up by the bottom edge
        CHECK(r[2] == 100 && gaps_ok(r, 30) && r[0] >= 0);
        r = spread({50, 10}, 20, 0, 100);  // given out of order: results follow the input order
        CHECK(r[1] == 10 && r[0] == 50);
        r = spread({1, 2, 3, 4, 5}, 50, 0, 100);  // too full for the gap: spread evenly
        CHECK(r[0] == 0 && r[4] == 100 && gaps_ok(r, 25));
        CHECK(spread({}, 10, 0, 10).empty());
    }

    // Controller setup: the stick steps point at their direction.
    CHECK(std::string(action_for_step("a")) == "cross" && std::string(action_for_step("lefttrigger")) == "l2");
    CHECK(std::string(action_for_step("leftx")) == "move_right" && std::string(action_for_step("righty")) == "look_down");
    CHECK(action_for_step("nonsense") == nullptr);

    // Key names on a button: long ones are shortened, the rest is left alone.
    CHECK(short_key("Left Shift") == "L-Shift" && short_key("Escape") == "Esc" && short_key("Mouse1") == "LMB");
    CHECK(short_key("W") == "W" && short_key("Space") == "Space" && short_key("Up") == "Up" && short_key("") == "");
    CHECK(short_key("Left Shift, Space") == "L-Shift+");  // a hand-edited line with two inputs
    if (!g_fail) std::puts("pad layout: all tests passed");
    return g_fail ? 1 : 0;
}
