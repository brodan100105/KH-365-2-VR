// The VR settings panel: tabs across the top, one row per option with < and > to change it,
// the hovered option's help line, and "Defaults" (current tab) / "Close" at the bottom.
// It is drawn into a Canvas; the app shows that canvas on a quad and feeds laser-pointer hits
// in canvas pixels.
#pragma once

#include <string>
#include <vector>

#include "khvr/canvas.h"
#include "khvr/settings.h"

namespace khvr {

class SettingsPanel {
public:
    static constexpr int kWidth = 1024, kHeight = 768;

    SettingsPanel();

    void Open() { open_ = true; dirty_ = true; }
    void Close() { open_ = false; }
    bool isOpen() const { return open_; }

    // Pointer position in canvas pixels (hit = false when the laser misses the panel) and
    // whether the select button was pressed this frame. Changes go straight into `s`.
    void Pointer(bool hit, float x, float y, bool click, Settings &s);

    // Redraws if anything changed since the last call; returns true when it did.
    bool Redraw(const Settings &s);
    const Canvas &canvas() const { return canvas_; }

    int tab() const { return tab_; }
    void SetTab(int t);

private:
    struct Button {
        int x, y, w, h;
        enum Kind { Tab, Decrease, Increase, Row, Defaults, Close } kind;
        int index;  // tab index, or option id for Decrease/Increase/Row
        bool Contains(float px, float py) const {
            return px >= x && py >= y && px < x + w && py < y + h;
        }
    };
    std::vector<SettingId> TabOptions(int tab) const;
    std::vector<Button> Layout() const;

    Canvas canvas_;
    bool open_ = false;
    bool dirty_ = true;
    int tab_ = 0;
    int hover_ = -1;          // index into Layout()
    int hoverOption_ = -1;    // option whose help line is shown
    unsigned drawnRevision_ = ~0u;
};

}  // namespace khvr
