#include "khvr/settings_panel.h"

#include <cstring>

namespace khvr {

namespace {

constexpr int kTabsPerRow = 6;
constexpr int kTabTop = 64, kTabH = 44, kTabGap = 6;
constexpr int kRowsTop = kTabTop + 2 * (kTabH + kTabGap) + 10;
constexpr int kRowH = 46;  // ten rows (the longest tab) end above the help line
constexpr int kMargin = 24;
constexpr int kArrowW = 56;
constexpr int kValueW = 300;
constexpr int kBottomH = 56;

const uint32_t kBg = Rgba(16, 18, 34, 235);
const uint32_t kPanelEdge = Rgba(214, 178, 82);   // the series' gold
const uint32_t kText = Rgba(236, 236, 244);
const uint32_t kDim = Rgba(150, 154, 180);
const uint32_t kTabIdle = Rgba(38, 42, 70);
const uint32_t kTabActive = Rgba(70, 60, 130);
const uint32_t kHover = Rgba(96, 110, 190);
const uint32_t kRowAlt = Rgba(255, 255, 255, 10);

}  // namespace

SettingsPanel::SettingsPanel() : canvas_(kWidth, kHeight) {}

void SettingsPanel::SetTab(int t) {
    int n = static_cast<int>(SettingTabs().size());
    tab_ = ((t % n) + n) % n;
    hover_ = -1;
    hoverOption_ = -1;
    dirty_ = true;
}

std::vector<SettingId> SettingsPanel::TabOptions(int tab) const {
    std::vector<SettingId> ids;
    const char *name = SettingTabs()[tab];
    for (const SettingDef &d : SettingDefs())
        if (std::strcmp(d.tab, name) == 0) ids.push_back(d.id);
    return ids;
}

std::vector<SettingsPanel::Button> SettingsPanel::Layout() const {
    std::vector<Button> b;
    int ntabs = static_cast<int>(SettingTabs().size());
    int tabW = (kWidth - 2 * kMargin - (kTabsPerRow - 1) * kTabGap) / kTabsPerRow;
    for (int i = 0; i < ntabs; ++i) {
        int row = i / kTabsPerRow, col = i % kTabsPerRow;
        b.push_back({kMargin + col * (tabW + kTabGap), kTabTop + row * (kTabH + kTabGap), tabW,
                     kTabH, Button::Tab, i});
    }
    std::vector<SettingId> ids = TabOptions(tab_);
    for (size_t r = 0; r < ids.size(); ++r) {
        int y = kRowsTop + static_cast<int>(r) * kRowH;
        int id = static_cast<int>(ids[r]);
        int valueRight = kWidth - kMargin;
        b.push_back({kMargin, y, kWidth - 2 * kMargin, kRowH, Button::Row, id});
        b.push_back({valueRight - kArrowW, y + 4, kArrowW, kRowH - 8, Button::Increase, id});
        b.push_back({valueRight - 2 * kArrowW - kValueW, y + 4, kArrowW, kRowH - 8,
                     Button::Decrease, id});
    }
    int by = kHeight - kBottomH - 12;
    b.push_back({kMargin, by, 220, kBottomH, Button::Defaults, 0});
    b.push_back({kWidth - kMargin - 220, by, 220, kBottomH, Button::Close, 0});
    return b;
}

void SettingsPanel::Pointer(bool hit, float x, float y, bool click, Settings &s) {
    if (!open_) return;
    std::vector<Button> buttons = Layout();
    int hover = -1, hoverOption = hoverOption_;
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
    }
    if (hover != hover_ || hoverOption != hoverOption_) dirty_ = true;
    hover_ = hover;
    hoverOption_ = hoverOption;

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
    case Button::Defaults: s.ResetTab(SettingTabs()[tab_]); break;
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
    c.DrawText(kWidth - kMargin - Canvas::TextWidth("hold MENU to close"), 28,
               "hold MENU to close", kDim);

    std::vector<Button> buttons = Layout();
    for (size_t i = 0; i < buttons.size(); ++i) {
        const Button &b = buttons[i];
        bool hot = static_cast<int>(i) == hover_;
        switch (b.kind) {
        case Button::Tab: {
            const char *name = SettingTabs()[b.index];
            c.FillRect(b.x, b.y, b.w, b.h, b.index == tab_ ? kTabActive : (hot ? kHover : kTabIdle));
            if (b.index == tab_) c.FillRect(b.x, b.y + b.h - 4, b.w, 4, kPanelEdge);
            c.DrawText(b.x + (b.w - Canvas::TextWidth(name)) / 2, b.y + (b.h - Canvas::LineHeight()) / 2,
                       name, kText);
            break;
        }
        case Button::Row: {
            SettingId id = static_cast<SettingId>(b.index);
            int rowIndex = (b.y - kRowsTop) / kRowH;
            if (rowIndex % 2) c.FillRect(b.x, b.y, b.w, b.h, kRowAlt);
            if (b.index == hoverOption_) c.StrokeRect(b.x, b.y, b.w, b.h, kHover, 2);
            c.DrawText(b.x + 12, b.y + (b.h - Canvas::LineHeight()) / 2, DefOf(id).label, kText);
            std::string value = s.FormatValue(id);
            int valueX = kWidth - kMargin - kArrowW - kValueW;
            c.DrawText(valueX + (kValueW - Canvas::TextWidth(value)) / 2,
                       b.y + (b.h - Canvas::LineHeight()) / 2, value, kPanelEdge);
            break;
        }
        case Button::Decrease:
        case Button::Increase: {
            c.FillRect(b.x, b.y, b.w, b.h, hot ? kHover : kTabIdle);
            const char *arrow = b.kind == Button::Increase ? ">" : "<";
            c.DrawText(b.x + (b.w - Canvas::TextWidth(arrow, 2)) / 2,
                       b.y + (b.h - Canvas::LineHeight(2)) / 2, arrow, kText, 2);
            break;
        }
        case Button::Defaults:
        case Button::Close: {
            const char *label = b.kind == Button::Close ? "Close" : "Defaults";
            c.FillRect(b.x, b.y, b.w, b.h, hot ? kHover : kTabIdle);
            c.StrokeRect(b.x, b.y, b.w, b.h, kPanelEdge, 2);
            c.DrawText(b.x + (b.w - Canvas::TextWidth(label)) / 2,
                       b.y + (b.h - Canvas::LineHeight()) / 2, label, kText);
            break;
        }
        }
    }
    if (hoverOption_ >= 0) {
        const char *help = DefOf(static_cast<SettingId>(hoverOption_)).help;
        int y = kHeight - kBottomH - 12 - Canvas::LineHeight() - 14;
        int maxChars = (kWidth - 2 * kMargin) / 10;
        std::string line = help;
        if (static_cast<int>(line.size()) > maxChars) line = line.substr(0, maxChars - 3) + "...";
        c.DrawText(kMargin, y, line, kDim);
    }
    return true;
}

}  // namespace khvr
