// The VR settings panel: six pages across the top, each a scrolling list of options under
// section headings, with < and > to change each option, the hovered option's help line, and
// "Defaults" (the page's options), "Button guide" and "Close" at the bottom. The first page,
// Quick setup, starts with profiles that set every option a way of playing needs in one click
// (a giant screen, both screens on the wall, a DS in your hands...) and then the options people
// change most. The button guide is a drawing of both controllers with what each control does.
// It is drawn into a Canvas; the app shows that canvas on a quad and feeds laser-pointer hits
// in canvas pixels, and the thumbstick scrolls.
#pragma once

#include <string>
#include <vector>

#include "khvr/canvas.h"
#include "khvr/settings.h"

namespace khvr {

// A way of playing the Quick setup page sets up in one click.
enum class SettingsProfile : int { GiantScreen = 0, BothScreens = 1, DsInHands = 2, DsAndScreen = 3, Count = 4 };
void ApplyProfile(SettingsProfile p, Settings &s);
// The profile the settings match now, or Count when none does.
SettingsProfile CurrentProfile(const Settings &s);

class SettingsPanel {
public:
    static constexpr int kWidth = 1024, kHeight = 768;

    struct Button {
        int x, y, w, h;
        enum Kind { Tab, Decrease, Increase, Row, Defaults, Close, Guide, Profile, ScrollUp, ScrollDown } kind;
        int index;  // page, option id (Decrease/Increase/Row) or profile
        bool Contains(float px, float py) const {
            return px >= x && py >= y && px < x + w && py < y + h;
        }
    };

    SettingsPanel();

    void Open() { open_ = true; dirty_ = true; }
    void Close() { open_ = false; }
    bool isOpen() const { return open_; }

    // Pointer position in canvas pixels (hit = false when the laser misses the panel) and
    // whether the select button was pressed this frame. Changes go straight into `s`.
    void Pointer(bool hit, float x, float y, bool click, Settings &s);
    // Scrolls the page's list by whole rows (positive: down).
    void Scroll(int rows);

    // Redraws if anything changed since the last call; returns true when it did.
    bool Redraw(const Settings &s);
    const Canvas &canvas() const { return canvas_; }

    static int PageCount();
    static const char *PageName(int page);
    int tab() const { return tab_; }
    void SetTab(int t);
    bool guideOpen() const { return guide_; }
    int scroll() const { return scroll_; }
    // Where a button is now (its centre), for tests: false when it is not on the panel.
    bool Find(Button::Kind kind, int index, float *x, float *y) const;

private:
    // A line of a page's list: a section heading, or an option.
    struct Line {
        const char *heading;  // non-null: a heading
        SettingId id;
    };
    std::vector<Line> PageLines(int page) const;
    int ListTop() const;
    int VisibleLines() const;
    std::vector<Button> Layout() const;
    void DrawGuide(const Settings &s);

    Canvas canvas_;
    bool open_ = false;
    bool dirty_ = true;
    int tab_ = 0;
    int scroll_ = 0;          // first line of the list shown
    bool guide_ = false;      // the button guide instead of the page's options
    int hover_ = -1;          // index into Layout()
    int hoverOption_ = -1;    // option whose help line is shown
    int hoverProfile_ = -1;   // profile whose description is shown
    unsigned drawnRevision_ = ~0u;
};

}  // namespace khvr
