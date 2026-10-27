// The captured 3D world in a file (khvr_world_N.bin, written next to the snapshots while
// Screen > Game in 3D is on), so the desktop can draw what the headset drew: khdays_run
// --render-world. Little-endian, as both ends are.
#include <cstdio>
#include <cstring>
#include <string>

#include "khvr/platform.h"

namespace khvr {

namespace {

constexpr char kMagic[8] = {'K', 'H', 'V', 'R', 'W', 'L', 'D', '1'};

struct Writer {
    FILE *f;
    bool ok = true;
    void Put(const void *p, size_t n) { ok = ok && std::fwrite(p, 1, n, f) == n; }
    template <typename T> void Pod(const T &v) { Put(&v, sizeof v); }
    template <typename T> void Vec(const std::vector<T> &v) {
        uint32_t n = static_cast<uint32_t>(v.size());
        Pod(n);
        if (n) Put(v.data(), n * sizeof(T));
    }
};

struct Reader {
    FILE *f;
    bool ok = true;
    void Get(void *p, size_t n) { ok = ok && std::fread(p, 1, n, f) == n; }
    template <typename T> void Pod(T &v) { Get(&v, sizeof v); }
    template <typename T> void Vec(std::vector<T> &v) {
        uint32_t n = 0;
        Pod(n);
        if (!ok || n > (64u << 20) / sizeof(T)) {
            ok = false;
            return;
        }
        v.resize(n);
        if (n) Get(v.data(), n * sizeof(T));
    }
};

}  // namespace

bool PlatformWorldWrite(const std::string &path, const PlatformWorld &w) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    Writer o{f};
    o.Put(kMagic, sizeof kMagic);
    o.Vec(w.vertices);
    o.Vec(w.triangleTexture);
    o.Vec(w.triangleFlags);
    o.Pod(w.hasCamera);
    o.Pod(w.eye);
    o.Pod(w.target);
    o.Pod(w.up);
    o.Pod(w.fovY);
    o.Pod(w.nearZ);
    o.Pod(w.farZ);
    o.Pod(w.polygonsLeftOut);
    o.Pod(w.lens);
    o.Pod(w.view);
    o.Pod(w.viewport);
    o.Pod(w.fadeMode);
    o.Pod(w.fadeFactor);
    uint32_t n = static_cast<uint32_t>(w.textures.size());
    o.Pod(n);
    for (const PlatformWorldTexture &t : w.textures) {
        o.Pod(t.key);
        o.Pod(t.width);
        o.Pod(t.height);
        uint8_t flags = (t.repeatS ? 1 : 0) | (t.repeatT ? 2 : 0) | (t.flipS ? 4 : 0) | (t.flipT ? 8 : 0);
        o.Pod(flags);
        o.Vec(t.rgba);
    }
    bool ok = o.ok;
    return std::fclose(f) == 0 && ok;
}

bool PlatformWorldRead(const std::string &path, PlatformWorld *w) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    Reader in{f};
    char magic[8];
    in.Get(magic, sizeof magic);
    in.ok = in.ok && std::memcmp(magic, kMagic, sizeof magic) == 0;
    in.Vec(w->vertices);
    in.Vec(w->triangleTexture);
    in.Vec(w->triangleFlags);
    in.Pod(w->hasCamera);
    in.Pod(w->eye);
    in.Pod(w->target);
    in.Pod(w->up);
    in.Pod(w->fovY);
    in.Pod(w->nearZ);
    in.Pod(w->farZ);
    in.Pod(w->polygonsLeftOut);
    in.Pod(w->lens);
    in.Pod(w->view);
    in.Pod(w->viewport);
    in.Pod(w->fadeMode);
    in.Pod(w->fadeFactor);
    uint32_t n = 0;
    in.Pod(n);
    w->textures.clear();
    for (uint32_t i = 0; in.ok && i < n && i < 4096; ++i) {
        PlatformWorldTexture t;
        uint8_t flags = 0;
        in.Pod(t.key);
        in.Pod(t.width);
        in.Pod(t.height);
        in.Pod(flags);
        in.Vec(t.rgba);
        t.repeatS = flags & 1;
        t.repeatT = flags & 2;
        t.flipS = flags & 4;
        t.flipT = flags & 8;
        w->textures.push_back(std::move(t));
    }
    std::fclose(f);
    return in.ok;
}

}  // namespace khvr
