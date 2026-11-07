// Renders the settings panel's button guide (the drawing of both controllers the headset shows)
// to a PPM image, for the README: khvr_guide_image out.ppm
#include <cstdio>

#include "khvr/settings.h"
#include "khvr/settings_panel.h"

int main(int argc, char **argv) {
    using namespace khvr;
    const char *out = argc > 1 ? argv[1] : "button_guide.ppm";
    Settings s;
    SettingsPanel panel;
    panel.Open();
    float x = 0, y = 0;
    if (!panel.Find(SettingsPanel::Button::Guide, 0, &x, &y)) return 1;
    panel.Pointer(true, x, y, true, s);
    panel.Pointer(false, 0, 0, false, s);  // no hover highlight
    panel.Redraw(s);
    if (!panel.guideOpen()) return 2;
    const Canvas &c = panel.canvas();
    FILE *f = std::fopen(out, "wb");
    if (!f) return 3;
    std::fprintf(f, "P6\n%d %d\n255\n", c.width(), c.height());
    for (int i = 0; i < c.width() * c.height(); ++i) {
        const uint32_t p = c.data()[i];
        const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                      static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return 0;
}
