#include "khvr/settings_panel.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace khvr {

namespace {

constexpr int kTabTop = 64, kTabH = 44, kTabGap = 6;
constexpr int kContentTop = kTabTop + kTabH + 10;
constexpr int kProfileH = 64, kProfileGap = 8;
constexpr int kRowH = 42;
constexpr int kMargin = 24;
constexpr int kArrowW = 56;
constexpr int kValueW = 300;
constexpr int kScrollW = 44;
constexpr int kBottomH = 56;

const uint32_t kBg = Rgba(16, 18, 34, 235);
const uint32_t kPanelEdge = Rgba(214, 178, 82);   // the series' gold
const uint32_t kText = Rgba(236, 236, 244);
const uint32_t kDim = Rgba(150, 154, 180);
const uint32_t kTabIdle = Rgba(38, 42, 70);
const uint32_t kTabActive = Rgba(70, 60, 130);
const uint32_t kHover = Rgba(96, 110, 190);
const uint32_t kRowAlt = Rgba(255, 255, 255, 10);

// The pages and the setting tabs (INI sections) each shows, in order, as its sections.
struct Page {
    const char *name;
    std::vector<const char *> sections;
};
const std::vector<Page> &Pages() {
    static const std::vector<Page> pages = {
        {"Quick setup", {}},
        {"Screens", {"Screen", "Picture", "HUD"}},
        {"Hands & DS", {"Hand screen", "Handheld"}},
        {"View", {"View", "Camera", "First person", "Comfort"}},
        {"Combat", {"Combat", "Controls"}},
        {"Game", {"Game"}},
    };
    return pages;
}

// The options people change most, under the profiles.
const SettingId kQuickOptions[] = {
    SettingId::ViewMode,         SettingId::ViewStandInWorld,    SettingId::Passthrough,         SettingId::ScreenPicture,
    SettingId::ScreenWidth,      SettingId::ScreenDistance,      SettingId::HandScreenSide,
    SettingId::HandScreenDsSize, SettingId::HandScreenDsHinge,   SettingId::HandScreenDsTwoHands,
};

// Quick setup shows options from several pages together: their names there say what they are of.
const char *QuickLabel(SettingId id) {
    switch (id) {
    case SettingId::HandScreenSide: return "DS / touch screen hand";
    case SettingId::HandScreenDsSize: return "DS size";
    case SettingId::HandScreenDsHinge: return "DS hinge angle";
    case SettingId::HandScreenDsTwoHands: return "Hold DS with both hands";
    default: return DefOf(id).label;
    }
}

struct ProfileText {
    const char *title, *about;
};
const ProfileText kProfiles[] = {
    {"Giant screen", "The top screen big in front of you, the touch screen above your hand."},
    {"Both screens", "Top and touch screens one above the other in front of you; point to touch."},
    {"DS in your hands", "Only a DS you hold, in your real room (passthrough). No giant screen."},
    {"DS + giant screen", "A DS in your hand showing both screens, and the giant screen too."},
};

// The help text: two lines above the bottom buttons.
int HelpY() { return SettingsPanel::kHeight - kBottomH - 12 - 2 * (Canvas::LineHeight() + 4) - 10; }
int ListBottom() { return HelpY() - 8; }

}  // namespace

void ApplyProfile(SettingsProfile p, Settings &s) {
    s.SetBool(SettingId::ScreenBoth, p == SettingsProfile::BothScreens);
    s.SetBool(SettingId::Passthrough, p == SettingsProfile::DsInHands);
    s.SetEnum(SettingId::HandScreenPlacement,
              p == SettingsProfile::BothScreens ? BottomScreenPlacement::Hidden : BottomScreenPlacement::InHand);
    s.SetEnum(SettingId::HandScreenDs, p == SettingsProfile::DsInHands     ? HandheldDs::Only
                                       : p == SettingsProfile::DsAndScreen ? HandheldDs::Always
                                                                           : HandheldDs::Off);
    if (p == SettingsProfile::DsAndScreen) s.SetBool(SettingId::HandScreenDsGiant, true);
    if (p == SettingsProfile::DsInHands) s.SetBool(SettingId::HandScreenDsTwoHands, true);
}

SettingsProfile CurrentProfile(const Settings &s) {
    const bool both = s.Bool(SettingId::ScreenBoth), pass = s.Bool(SettingId::Passthrough);
    const auto place = s.Enum<BottomScreenPlacement>(SettingId::HandScreenPlacement);
    const auto ds = s.Enum<HandheldDs>(SettingId::HandScreenDs);
    if (ds == HandheldDs::Only && !both && place == BottomScreenPlacement::InHand && pass)
        return SettingsProfile::DsInHands;
    if (pass) return SettingsProfile::Count;
    if (ds == HandheldDs::Always && s.Bool(SettingId::HandScreenDsGiant) && !both && place == BottomScreenPlacement::InHand)
        return SettingsProfile::DsAndScreen;
    if (ds != HandheldDs::Off) return SettingsProfile::Count;
    if (both && place == BottomScreenPlacement::Hidden) return SettingsProfile::BothScreens;
    if (!both && place == BottomScreenPlacement::InHand) return SettingsProfile::GiantScreen;
    return SettingsProfile::Count;
}

SettingsPanel::SettingsPanel() : canvas_(kWidth, kHeight) {}

int SettingsPanel::PageCount() { return static_cast<int>(Pages().size()); }
const char *SettingsPanel::PageName(int page) { return Pages()[page].name; }

void SettingsPanel::SetTab(int t) {
    guide_ = false;
    int n = PageCount();
    tab_ = ((t % n) + n) % n;
    scroll_ = 0;
    hover_ = -1;
    hoverOption_ = -1;
    hoverProfile_ = -1;
    dirty_ = true;
}

std::vector<SettingsPanel::Line> SettingsPanel::PageLines(int page) const {
    std::vector<Line> lines;
    if (page == 0) {
        for (SettingId id : kQuickOptions) lines.push_back({nullptr, id});
        return lines;
    }
    for (const char *section : Pages()[page].sections) {
        lines.push_back({section, SettingId::Count});
        for (const SettingDef &d : SettingDefs())
            if (std::strcmp(d.tab, section) == 0) lines.push_back({nullptr, d.id});
    }
    return lines;
}

int SettingsPanel::ListTop() const {
    return tab_ == 0 ? kContentTop + 2 * (kProfileH + kProfileGap) + 4 : kContentTop;
}

int SettingsPanel::VisibleLines() const { return (ListBottom() - ListTop()) / kRowH; }

void SettingsPanel::Scroll(int rows) {
    if (guide_ || rows == 0) return;
    int most = std::max(0, static_cast<int>(PageLines(tab_).size()) - VisibleLines());
    int to = std::clamp(scroll_ + rows, 0, most);
    if (to != scroll_) {
        scroll_ = to;
        hover_ = -1;
        dirty_ = true;
    }
}

std::vector<SettingsPanel::Button> SettingsPanel::Layout() const {
    std::vector<Button> b;
    const int npages = PageCount();
    const int tabW = (kWidth - 2 * kMargin - (npages - 1) * kTabGap) / npages;
    for (int i = 0; i < npages; ++i)
        b.push_back({kMargin + i * (tabW + kTabGap), kTabTop, tabW, kTabH, Button::Tab, i});
    if (!guide_) {
        if (tab_ == 0) {
            const int w = (kWidth - 2 * kMargin - kProfileGap) / 2;
            for (int i = 0; i < static_cast<int>(SettingsProfile::Count); ++i)
                b.push_back({kMargin + (i % 2) * (w + kProfileGap), kContentTop + (i / 2) * (kProfileH + kProfileGap), w,
                             kProfileH, Button::Profile, i});
        }
        const std::vector<Line> lines = PageLines(tab_);
        const int top = ListTop(), shown = VisibleLines();
        const int valueRight = kWidth - kMargin - kScrollW - 8;
        for (int r = 0; r < shown && scroll_ + r < static_cast<int>(lines.size()); ++r) {
            const Line &l = lines[scroll_ + r];
            if (l.heading) continue;
            int y = top + r * kRowH;
            int id = static_cast<int>(l.id);
            b.push_back({kMargin, y, valueRight - kMargin, kRowH, Button::Row, id});
            b.push_back({valueRight - kArrowW, y + 4, kArrowW, kRowH - 8, Button::Increase, id});
            b.push_back({valueRight - 2 * kArrowW - kValueW, y + 4, kArrowW, kRowH - 8, Button::Decrease, id});
        }
        if (static_cast<int>(lines.size()) > shown) {
            const int x = kWidth - kMargin - kScrollW, h = 2 * kRowH;
            b.push_back({x, top, kScrollW, h, Button::ScrollUp, 0});
            b.push_back({x, top + shown * kRowH - h, kScrollW, h, Button::ScrollDown, 0});
        }
    }
    int by = kHeight - kBottomH - 12;
    if (!guide_ && tab_ != 0) b.push_back({kMargin, by, 220, kBottomH, Button::Defaults, 0});
    b.push_back({(kWidth - 280) / 2, by, 280, kBottomH, Button::Guide, 0});
    b.push_back({kWidth - kMargin - 220, by, 220, kBottomH, Button::Close, 0});
    return b;
}

bool SettingsPanel::Find(Button::Kind kind, int index, float *x, float *y) const {
    for (const Button &b : Layout())
        if (b.kind == kind && b.index == index) {
            *x = b.x + b.w * 0.5f;
            *y = b.y + b.h * 0.5f;
            return true;
        }
    return false;
}

void SettingsPanel::Pointer(bool hit, float x, float y, bool click, Settings &s) {
    if (!open_) return;
    std::vector<Button> buttons = Layout();
    int hover = -1, hoverOption = hoverOption_, hoverProfile = -1;
    if (hit) {
        // Later buttons (arrows) sit on top of their row, so search from the back.
        for (int i = static_cast<int>(buttons.size()) - 1; i >= 0; --i)
            if (buttons[i].Contains(x, y)) {
                hover = i;
                break;
            }
    }
    if (hover >= 0) {
        const Button &btn = buttons[hover];
        if (btn.kind == Button::Row || btn.kind == Button::Increase || btn.kind == Button::Decrease)
            hoverOption = btn.index;
        if (btn.kind == Button::Profile) {
            hoverProfile = btn.index;
            hoverOption = -1;
        }
    }
    if (hover != hover_ || hoverOption != hoverOption_ || hoverProfile != hoverProfile_) dirty_ = true;
    hover_ = hover;
    hoverOption_ = hoverOption;
    hoverProfile_ = hoverProfile;

    if (!click || hover < 0) return;
    const Button &btn = buttons[hover];
    switch (btn.kind) {
    case Button::Tab: SetTab(btn.index); break;
    case Button::Increase: s.Step(static_cast<SettingId>(btn.index), 1); break;
    case Button::Decrease: s.Step(static_cast<SettingId>(btn.index), -1); break;
    case Button::Row:
        // Clicking the row itself toggles a switch or cycles a choice.
        if (DefOf(static_cast<SettingId>(btn.index)).type != SettingType::Float)
            s.Step(static_cast<SettingId>(btn.index), 1);
        break;
    case Button::Profile: ApplyProfile(static_cast<SettingsProfile>(btn.index), s); break;
    case Button::ScrollUp: Scroll(-(VisibleLines() - 1)); break;
    case Button::ScrollDown: Scroll(VisibleLines() - 1); break;
    case Button::Defaults:
        for (const char *section : Pages()[tab_].sections) s.ResetTab(section);
        break;
    case Button::Guide:
        guide_ = !guide_;
        hoverOption_ = -1;
        break;
    case Button::Close: Close(); break;
    }
    dirty_ = true;
}

bool SettingsPanel::Redraw(const Settings &s) {
    if (!dirty_ && drawnRevision_ == s.revision()) return false;
    dirty_ = false;
    drawnRevision_ = s.revision();

    Canvas &c = canvas_;
    c.Clear(kBg);
    c.StrokeRect(0, 0, kWidth, kHeight, kPanelEdge, 3);
    c.DrawText(kMargin, 16, "VR SETTINGS", kPanelEdge, 2);
    c.DrawText(kWidth - kMargin - Canvas::TextWidth("thumbstick scrolls - hold MENU to close"), 28,
               "thumbstick scrolls - hold MENU to close", kDim);

    // Section headings of the list.
    if (!guide_) {
        const std::vector<Line> lines = PageLines(tab_);
        const int top = ListTop(), shown = VisibleLines();
        for (int r = 0; r < shown && scroll_ + r < static_cast<int>(lines.size()); ++r) {
            const Line &l = lines[scroll_ + r];
            if (!l.heading) continue;
            int y = top + r * kRowH;
            c.DrawText(kMargin + 4, y + kRowH - Canvas::LineHeight() - 8, l.heading, kPanelEdge);
            c.FillRect(kMargin, y + kRowH - 4, kWidth - 2 * kMargin - kScrollW - 8, 2, Rgba(214, 178, 82, 90));
        }
        if (static_cast<int>(lines.size()) > shown) {
            // The scroll bar between the two arrows: where the shown lines are in the list.
            const int x = kWidth - kMargin - kScrollW, trackTop = top + 2 * kRowH + 4,
                      trackH = shown * kRowH - 4 * kRowH - 8;
            c.FillRect(x + kScrollW / 2 - 3, trackTop, 6, trackH, kTabIdle);
            const int n = static_cast<int>(lines.size());
            const int thumbH = std::max(20, trackH * shown / n);
            const int thumbY = trackTop + (trackH - thumbH) * scroll_ / std::max(1, n - shown);
            c.FillRect(x + kScrollW / 2 - 5, thumbY, 10, thumbH, kPanelEdge);
        }
    }

    const SettingsProfile current = CurrentProfile(s);
    std::vector<Button> buttons = Layout();
    for (size_t i = 0; i < buttons.size(); ++i) {
        const Button &b = buttons[i];
        bool hot = static_cast<int>(i) == hover_;
        switch (b.kind) {
        case Button::Tab: {
            const char *name = PageName(b.index);
            const bool active = b.index == tab_ && !guide_;
            c.FillRect(b.x, b.y, b.w, b.h, active ? kTabActive : (hot ? kHover : kTabIdle));
            if (active) c.FillRect(b.x, b.y + b.h - 4, b.w, 4, kPanelEdge);
            c.DrawText(b.x + (b.w - Canvas::TextWidth(name)) / 2, b.y + (b.h - Canvas::LineHeight()) / 2,
                       name, kText);
            break;
        }
        case Button::Profile: {
            const bool on = static_cast<int>(current) == b.index;
            c.FillRect(b.x, b.y, b.w, b.h, on ? kTabActive : (hot ? kHover : kTabIdle));
            c.StrokeRect(b.x, b.y, b.w, b.h, on ? kPanelEdge : Rgba(214, 178, 82, 90), on ? 3 : 1);
            const ProfileText &t = kProfiles[b.index];
            c.DrawText(b.x + 14, b.y + 8, t.title, kPanelEdge);
            c.DrawText(b.x + 14, b.y + 8 + Canvas::LineHeight() + 4, on ? "in use" : "click to use", kDim);
            break;
        }
        case Button::Row: {
            SettingId id = static_cast<SettingId>(b.index);
            int rowIndex = (b.y - ListTop()) / kRowH;
            if (rowIndex % 2) c.FillRect(b.x, b.y, b.w, b.h, kRowAlt);
            if (b.index == hoverOption_) c.StrokeRect(b.x, b.y, b.w, b.h, kHover, 2);
            c.DrawText(b.x + 12, b.y + (b.h - Canvas::LineHeight()) / 2, tab_ == 0 ? QuickLabel(id) : DefOf(id).label,
                       kText);
            std::string value = s.FormatValue(id);
            int valueX = b.x + b.w - kArrowW - kValueW;
            c.DrawText(valueX + (kValueW - Canvas::TextWidth(value)) / 2,
                       b.y + (b.h - Canvas::LineHeight()) / 2, value, kPanelEdge);
            break;
        }
        case Button::Decrease:
        case Button::Increase:
        case Button::ScrollUp:
        case Button::ScrollDown: {
            c.FillRect(b.x, b.y, b.w, b.h, hot ? kHover : kTabIdle);
            const char *arrow = b.kind == Button::Increase   ? ">"
                                : b.kind == Button::Decrease ? "<"
                                : b.kind == Button::ScrollUp ? "^"
                                                             : "v";
            c.DrawText(b.x + (b.w - Canvas::TextWidth(arrow, 2)) / 2,
                       b.y + (b.h - Canvas::LineHeight(2)) / 2, arrow, kText, 2);
            break;
        }
        case Button::Defaults:
        case Button::Guide:
        case Button::Close: {
            const char *label = b.kind == Button::Close  ? "Close"
                                : b.kind == Button::Guide ? (guide_ ? "Back to settings" : "Button guide")
                                                          : "Defaults";
            c.FillRect(b.x, b.y, b.w, b.h, hot ? kHover : kTabIdle);
            c.StrokeRect(b.x, b.y, b.w, b.h, kPanelEdge, 2);
            c.DrawText(b.x + (b.w - Canvas::TextWidth(label)) / 2,
                       b.y + (b.h - Canvas::LineHeight()) / 2, label, kText);
            break;
        }
        }
    }
    if (guide_) DrawGuide(s);
    const char *help = hoverProfile_ >= 0  ? kProfiles[hoverProfile_].about
                       : hoverOption_ >= 0 ? DefOf(static_cast<SettingId>(hoverOption_)).help
                                           : nullptr;
    if (help) {
        // Wrapped at spaces over two lines; what does not fit ends in "...".
        const int maxW = kWidth - 2 * kMargin;
        std::string rest = help;
        for (int lineNo = 0; lineNo < 2 && !rest.empty(); ++lineNo) {
            std::string line;
            size_t at = 0;
            while (at < rest.size()) {
                size_t next = rest.find(' ', at + 1);
                if (next == std::string::npos) next = rest.size();
                if (Canvas::TextWidth(rest.substr(0, next)) > maxW) break;
                at = next;
            }
            if (at == 0) at = rest.size();
            line = rest.substr(0, at);
            rest = at < rest.size() ? rest.substr(at + 1) : "";
            if (lineNo == 1 && !rest.empty()) {
                while (!line.empty() && Canvas::TextWidth(line + "...") > maxW) line.pop_back();
                line += "...";
            }
            c.DrawText(kMargin, HelpY() + lineNo * (Canvas::LineHeight() + 4), line, kDim);
        }
    }
    return true;
}

void SettingsPanel::DrawGuide(const Settings &s) {
    Canvas &c = canvas_;
    const uint32_t body = Rgba(52, 56, 84), face = Rgba(30, 32, 52), key = Rgba(120, 126, 170);
    const uint32_t line = Rgba(214, 178, 82, 200);
    const bool weaponRight = s.Enum<Hand>(SettingId::FpWeaponHand) == Hand::Right;
    auto bind = [&](SettingId id) { return s.FormatValue(id); };

    // A Touch controller seen from above: its ring, the face with thumbstick and two buttons, and
    // the trigger (front) and grip (side) as tabs. `mirror` draws the right one.
    struct Spot {
        int x, y;
    };
    auto controller = [&](int cx, int cy, bool right, Spot *stick, Spot *upper, Spot *lower, Spot *menu, Spot *trigger,
                          Spot *grip) {
        const int sx = right ? -1 : 1;
        c.FillRect(cx - 34, cy + 40, 68, 120, body);  // the handle
        c.FillCircle(cx, cy + 160, 34, body);
        c.StrokeCircle(cx, cy - 6, 92, body, 14);     // the tracking ring
        c.FillCircle(cx, cy, 70, face);
        c.FillRect(cx - 50, cy - 98, 100, 18, key);   // the trigger, on the front
        c.FillRect(cx + sx * 40 - 9, cy + 60, 18, 64, key);  // the grip, on the inner side
        *stick = {cx - sx * 22, cy - 14};
        *upper = {cx + sx * 34, cy - 24};
        *lower = {cx + sx * 26, cy + 24};
        *menu = {cx - sx * 4, cy + 44};
        *trigger = {cx, cy - 89};
        *grip = {cx + sx * 40, cy + 92};
        c.FillCircle(stick->x, stick->y, 22, key);
        c.FillCircle(stick->x, stick->y, 12, face);
        c.FillCircle(upper->x, upper->y, 13, key);
        c.FillCircle(lower->x, lower->y, 13, key);
        c.FillCircle(menu->x, menu->y, 7, key);
    };
    // A label with a leader line to its control, on the outer side of the controller.
    auto label = [&](const Spot &at, int tx, int ty, const std::string &title, const std::string &what) {
        int w = std::max(Canvas::TextWidth(title), Canvas::TextWidth(what));
        bool leftSide = tx < at.x;
        int anchorX = leftSide ? tx + w + 6 : tx - 6;
        c.DrawLine(at.x, at.y, anchorX, ty + 10, line, 2);
        c.FillCircle(at.x, at.y, 4, kPanelEdge);
        c.DrawText(tx, ty, title, kPanelEdge);
        c.DrawText(tx, ty + 20, what, kText);
    };

    const int top = kContentTop + 4;
    Spot st, up, lo, mn, tr, gr;
    // Left controller.
    controller(400, top + 140, false, &st, &up, &lo, &mn, &tr, &gr);
    const bool fpProfile = s.Enum<FirstPersonButtons>(SettingId::CtrlFirstPersonButtons) == FirstPersonButtons::Profile;
    label(tr, kMargin, top, "Trigger", !fpProfile ? "DS L" : weaponRight ? "DS L / 1st person: cast" : "DS L / 1st person: DS L");
    label(up, kMargin, top + 60, "Y", "DS Y");
    label(st, kMargin, top + 120, "Thumbstick", "move / hold: 2nd screen / tap: SELECT (menus)");
    label(lo, kMargin, top + 180, "X", "DS X");
    label(mn, kMargin, top + 240, "Menu", "tap: START / hold: settings");
    label(gr, kMargin, top + 300, "Grip", fpProfile && weaponRight ? "1st person: " + bind(SettingId::CtrlLockOnButton) + " lock-on" : "-");
    // Right controller.
    controller(624, top + 140, true, &st, &up, &lo, &mn, &tr, &gr);
    const int rx = 772;
    label(tr, rx, top, "Trigger", "DS R / select / touch");
    label(up, rx, top + 60, "B", "DS B");
    label(st, rx, top + 120, "Thumbstick", "turn / click: recenter");
    label(lo, rx, top + 180, "A", "DS A");
    c.DrawText(rx, top + 240, "hold stick click:", kDim);
    c.DrawText(rx, top + 260, "switch view", kDim);
    label(gr, rx, top + 300, "Grip", !fpProfile || weaponRight ? "-" : "1st person: " + bind(SettingId::CtrlLockOnButton) + " lock-on");

    // First person's gestures.
    int y = top + 352;
    c.DrawText(kMargin, y, "First person, with the Keyblade in your " + std::string(weaponRight ? "right" : "left") + " hand:",
               kPanelEdge);
    y += 24;
    c.DrawText(kMargin, y, "Swing: attack (" + bind(SettingId::CtrlAttackButton) + ")   Hold it level before your face: guard (" +
                               bind(SettingId::CtrlGuardButton) + ")", kText);
    y += 22;
    c.DrawText(kMargin, y, (fpProfile ? "Point the other hand and pull its trigger: cast (" + bind(SettingId::CtrlMagicButton) + ")   "
                                      : std::string("Buttons as in the other views.   ")) +
                               "Flick the stick: dodge (" + bind(SettingId::CtrlDodgeButton) + ")", kText);
    y += 22;
    c.DrawText(kMargin, y, "Aim the other hand at the touch screen and pull its trigger to touch it.", kDim);
}

}  // namespace khvr
