#include "decomp_game.h"

#include "khvr/held_weapon.h"

#include <dirent.h>

#include <aaudio/AAudio.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace khvr {

namespace {

constexpr double kDsFrameSeconds = 1.0 / 59.8261;

// The game code at 0x0c of an NDS header.
std::string GameCode(const std::string &path) {
    char code[5] = {0};
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        if (std::fseek(f, 0x0c, SEEK_SET) != 0 || std::fread(code, 1, 4, f) != 4) code[0] = 0;
        std::fclose(f);
    }
    return code;
}

bool EndsWithNds(const char *name) {
    size_t n = std::strlen(name);
    return n > 4 && (std::strcmp(name + n - 4, ".nds") == 0 || std::strcmp(name + n - 4, ".NDS") == 0);
}

}  // namespace

DecompGame::~DecompGame() {
    stop_ = true;
    if (driver_.joinable()) driver_.join();
    CloseAudio();
    Platform::Get().Stop();
}

namespace {
// AAudio's data callback: whatever the platform has mixed (silence while it has nothing).
aaudio_data_callback_result_t AudioCallback(AAudioStream *, void *, void *data, int32_t frames) {
    Platform::Get().ReadAudio(static_cast<int16_t *>(data), static_cast<size_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
}  // namespace

void DecompGame::OpenAudio() {
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, Platform::kAudioRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, AudioCallback, nullptr);
    AAudioStream *stream = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream) {
        LOGI("audio: cannot open an output stream (%s)", AAudio_convertResultToText(r));
        return;
    }
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        return;
    }
    audio_ = reinterpret_cast<AAudioStreamStruct *>(stream);
    LOGI("audio: playing at %d Hz", AAudioStream_getSampleRate(stream));
}

void DecompGame::CloseAudio() {
    if (!audio_) return;
    AAudioStream *stream = reinterpret_cast<AAudioStream *>(audio_);
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    audio_ = nullptr;
}

bool DecompGame::Start(const std::vector<std::string> &romDirs, const std::string &dataDir,
                       int language, std::string *message) {
    std::string rom;
    std::vector<std::string> others;
    for (const std::string &dir : romDirs) {
        DIR *d = opendir(dir.c_str());
        if (!d) {
            LOGI("rom search: cannot open %s", dir.c_str());
            continue;
        }
        while (dirent *e = readdir(d)) {
            if (!EndsWithNds(e->d_name)) continue;
            std::string path = dir + "/" + e->d_name;
            std::string code = GameCode(path);
            if (code == "YKGP" && rom.empty()) rom = path;
            else if (code != "YKGP") others.push_back(std::string(e->d_name) + " (" + (code.empty() ? "unreadable" : code) + ")");
        }
        closedir(d);
    }
    if (rom.empty()) {
        *message = "No European ROM (game code YKGP) found in the KHDaysVR or Download folder.";
        if (!others.empty()) {
            *message += " Found:";
            for (const std::string &o : others) *message += " " + o;
            *message += ". Only the EU release works: the decompilation matches that version.";
        }
        return false;
    }
    PlatformConfig cfg;
    cfg.romPath = rom;
    cfg.savePath = dataDir + "/khdays.sav";
    cfg.logPath = dataDir + "/khvr_boot.log";
    cfg.fileListPath = dataDir + "/khvr_files.txt";
    cfg.language = language;
    if (!Platform::Get().Start(cfg, message)) return false;
    *message = "Booting " + rom.substr(rom.find_last_of('/') + 1);
    LOGI("%s", message->c_str());

    frame_.inGameplay = false;
    frame_.inMenu = true;  // everything on the giant screen until the 3D capture exists
    frame_.camera.eye = {0, 0, 1};
    frame_.camera.target = {0, 0, 0};
    screens_.top.assign(DsScreens::kWidth * DsScreens::kHeight, 0xff000000u);
    screens_.topHud.assign(DsScreens::kWidth * DsScreens::kHeight, 0);
    screens_.bottom.assign(DsScreens::kWidth * DsScreens::kHeight, 0xff000000u);
    latest_ = std::make_unique<PlatformScreens>();
    latestWorld_ = std::make_unique<PlatformWorld>();
    OpenAudio();
    driver_ = std::thread([this] { DriverLoop(); });
    return true;
}

void DecompGame::CaptureFrame(std::unique_ptr<PlatformScreens> &scratch, std::unique_ptr<PlatformWorld> &scratchWorld) {
    using clock = std::chrono::steady_clock;
    Platform &p = Platform::Get();
    const auto captureStart = clock::now();
    p.CaptureScreens(scratch.get());
    const bool world = worldCapture_.load();
    if (world) p.CaptureWorld(scratchWorld.get());
    p.NoteDisplay(false, std::chrono::duration<float, std::milli>(clock::now() - captureStart).count());
    PlatformPlayer player;
    p.ReadPlayer(&player);  // the snapshot taken when the game finished the frame: cheap
    {
        std::lock_guard<std::mutex> lock(screensMu_);
        latestPlayer_ = player;
        if (world) {
            std::swap(scratchWorld, latestWorld_);
            ++latestWorldRevision_;
        }
    }
    // The loading area stays for 5 seconds at least, and until either screen shows an
    // image: the game starts on flat white or black (a brightness fade), which is not one.
    if (!hasPicture_ && clock::now() - started_ > std::chrono::seconds(5)) {
        auto detailed = [](const uint32_t *px) {
            const size_t n = PlatformScreens::kWidth * PlatformScreens::kHeight;
            size_t differ = 0;
            for (size_t i = 0; i < n; ++i) differ += (px[i] & 0x00ffffffu) != (px[0] & 0x00ffffffu);
            return differ > n / 100;
        };
        if (detailed(scratch->top) || detailed(scratch->bottom)) hasPicture_ = true;
    }
    std::lock_guard<std::mutex> lock(screensMu_);
    std::swap(scratch, latest_);
    ++latestRevision_;
}

void DecompGame::CaptureLoop() {
    auto scratch = std::make_unique<PlatformScreens>();
    auto scratchWorld = std::make_unique<PlatformWorld>();
    std::unique_lock<std::mutex> lock(captureMu_);
    for (;;) {
        captureCv_.wait(lock, [&] { return captureWanted_ || captureStop_; });
        if (captureStop_) return;
        captureWanted_ = false;
        captureBusy_ = true;
        lock.unlock();
        CaptureFrame(scratch, scratchWorld);
        lock.lock();
        captureBusy_ = false;
        captureCv_.notify_all();
    }
}

void DecompGame::DriverLoop() {
    using clock = std::chrono::steady_clock;
    auto frame = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kDsFrameSeconds));
    auto next = clock::now();
    int skippedCaptures = 0;
    auto scratch = std::make_unique<PlatformScreens>();
    auto scratchWorld = std::make_unique<PlatformWorld>();
    Platform &p = Platform::Get();
    auto nextBeat = clock::now() + std::chrono::seconds(2);
    started_ = clock::now();
    capture_ = std::thread([this] { CaptureLoop(); });
    // Waits until the drawing thread has nothing to do (before the guest restarts or stops).
    auto captureIdle = [&] {
        std::unique_lock<std::mutex> lock(captureMu_);
        captureCv_.wait(lock, [&] { return !captureBusy_ && !captureWanted_; });
    };
    while (!stop_ && (p.state() == PlatformState::Running || p.state() == PlatformState::Resetting)) {
        // "Return to title" resets the DS: boot the game again (logos, then the title screen).
        if (p.state() == PlatformState::Resetting) {
            captureIdle();
            p.Restart();
        }
        if (clock::now() >= nextBeat) {
            p.Heartbeat();
            nextBeat = clock::now() + std::chrono::seconds(5);
        }
        std::this_thread::sleep_until(next);
        next += frame;
        if (clock::now() > next + 10 * frame) next = clock::now();  // fell behind (loading): don't catch up
        uint32_t t = touch_.load();
        p.SetInput(static_cast<uint16_t>(buttons_.load()), (t >> 31) & 1, t & 0xff, (t >> 8) & 0xff);
        if (!p.VBlank(100)) continue;  // still busy with the previous frame
        if (parallel_.load()) {
            // The drawing thread draws this frame while the game goes on with the next one; if it
            // is still drawing the last one, this frame's screens are skipped.
            std::lock_guard<std::mutex> lock(captureMu_);
            if (captureBusy_ || captureWanted_) {
                p.NoteDisplay(true, 0.0f);
            } else {
                captureWanted_ = true;
                captureCv_.notify_all();
            }
            continue;
        }
        captureIdle();
        // Behind schedule (a heavy frame): keep the game, and so its sound, at full speed and
        // skip drawing the screens this time, at most twice in a row.
        if (clock::now() > next && skippedCaptures < 2) {
            ++skippedCaptures;
            p.NoteDisplay(true, 0.0f);
            continue;
        }
        skippedCaptures = 0;
        CaptureFrame(scratch, scratchWorld);
    }
    {
        std::lock_guard<std::mutex> lock(captureMu_);
        captureStop_ = true;
        captureCv_.notify_all();
    }
    capture_.join();
}

void DecompGame::Advance(double now, const GameRequests &requests, uint16_t (*sampleButtons)(void *),
                         void *ctx) {
    // Sample the pad once per DS frame period, like the game's own Pad_Sample would; pulses from
    // the input mapper (a swing's A press) then last the right number of game frames.
    if (lastSample_ < 0) lastSample_ = now;
    if (now - lastSample_ >= kDsFrameSeconds && sampleButtons) {
        uint16_t bits = 0;
        for (int i = 0; i < 4 && now - lastSample_ >= kDsFrameSeconds; ++i) {
            bits |= sampleButtons(ctx);
            lastSample_ += kDsFrameSeconds;
        }
        if (now - lastSample_ > kDsFrameSeconds) lastSample_ = now;
        buttons_ = bits;
    }
    touch_ = (requests.touching ? 0x80000000u : 0u) | requests.touchX | (static_cast<uint32_t>(requests.touchY) << 8);

    std::lock_guard<std::mutex> lock(screensMu_);
    if (latestRevision_ != shownRevision_) {
        shownRevision_ = latestRevision_;
        const size_t n = DsScreens::kWidth * DsScreens::kHeight;
        screens_.top.assign(latest_->top, latest_->top + n);
        screens_.topHud.assign(latest_->topHud, latest_->topHud + n);
        screens_.bottom.assign(latest_->bottom, latest_->bottom + n);
        screens_.movie = latest_->movie;
        if (latest_->split) {
            screens_.topFront.assign(latest_->topFront, latest_->topFront + n);
            screens_.topBack.assign(latest_->topBack, latest_->topBack + n);
        } else {
            screens_.topFront.clear();
            screens_.topBack.clear();
        }
        screens_.topScale = latest_->topScale;
        if (latest_->topScale > 1) screens_.topHi = latest_->topHi;
        ++screens_.revision;
        ++frame_.frameCounter;
    }
    // Without a player in the field (title, menus, mission review), with a menu or event scene
    // over it, or while a choice waits (an NPC's question: up and down), the stick moves a cursor.
    frame_.menuInput = !latestPlayer_.valid || latestPlayer_.menu || latestPlayer_.event || latestPlayer_.choice;
    if (!worldCapture_.load()) {
        frame_.inGameplay = false;
        frame_.inMenu = true;
        if (!texturedWorld_.vertices.empty()) {
            texturedWorld_ = TexturedWorld();
            ++texturedWorld_.revision;
        }
    } else if (latestWorldRevision_ != shownWorldRevision_) {
        shownWorldRevision_ = latestWorldRevision_;
        const PlatformWorld &w = *latestWorld_;
        TexturedWorld &t = texturedWorld_;
        // Roxas's own polygons: every vertex within a body-sized cylinder on his feet (game units
        // are about a meter; the field's Roxas is some 1.6 tall).
        const PlatformPlayer &me = latestPlayer_;
        // Body without head: while he runs, his running pose and the view's smoothing put his body
        // in front of you, so all of him goes until he has stood still a moment.
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (me.valid && hasLastFeet_) {
            const float dx = me.position[0] - lastFeet_[0], dz = me.position[2] - lastFeet_[2];
            if (dx * dx + dz * dz > 0.012f * 0.012f) movingUntil_ = now + 0.35;
        }
        if (me.valid) {
            std::copy(me.position, me.position + 3, lastFeet_);
            hasLastFeet_ = true;
        }
        const bool running = now < movingUntil_;
        const bool hide = me.valid && (hidePlayer_ == HidePlayer::All || (hidePlayer_ == HidePlayer::Head && running));
        const bool hideHead = hidePlayer_ == HidePlayer::Head && me.valid && !running;
        auto onPlayer = [&](const PlatformWorldVertex &v) {
            const float dx = v.x - me.position[0], dz = v.z - me.position[2], dy = v.y - me.position[1];
            return dx * dx + dz * dz < 0.55f * 0.55f && dy > -0.2f && dy < 2.2f;
        };
        // Running swings arms, hair and the Keyblade out of that cylinder: small triangles whose
        // middle is close to him go too (the ground's are large, and lie below his knees).
        auto nearPlayer = [&](const PlatformWorldVertex *v) {
            const float cx = (v[0].x + v[1].x + v[2].x) / 3 - me.position[0];
            const float cy = (v[0].y + v[1].y + v[2].y) / 3 - me.position[1];
            const float cz = (v[0].z + v[1].z + v[2].z) / 3 - me.position[2];
            if (cx * cx + cz * cz > 0.85f * 0.85f || cy < 0.25f || cy > 2.4f) return false;
            for (int k = 0; k < 3; ++k) {
                const PlatformWorldVertex &a = v[k], &b = v[(k + 1) % 3];
                const float ex = a.x - b.x, ey = a.y - b.y, ez = a.z - b.z;
                if (ex * ex + ey * ey + ez * ez > 0.6f * 0.6f) return false;
            }
            return true;
        };
        t.vertices.clear();
        t.triangleTexture.clear();
        t.triangleFlags.clear();
        t.billboard.clear();
        constexpr size_t kB = TexturedWorld::kBillboardFloats;
        static_assert(TexturedWorld::kBillboardFloats == kWorldBillboardFloats, "one layout");
        const bool sprites = w.billboard.size() == w.vertices.size() * kB;
        t.vertices.reserve(w.vertices.size());
        // The capture knows Roxas's own models (his and the Keyblade attached to him): those go,
        // exactly. Without them (not found), the polygons in and around his body.
        const bool tagged = w.triangleModel.size() == w.triangleFlags.size() &&
                            std::find(w.triangleModel.begin(), w.triangleModel.end(), kWorldModelPlayer) != w.triangleModel.end();
        // His own triangles (by the capture's tag, else in and around his body) and the height of
        // his head's cut: headCut_ of the way from his feet to his top.
        auto his = [&](size_t tri) {
            const PlatformWorldVertex *v = &w.vertices[tri * 3];
            return tagged ? w.triangleModel[tri] == kWorldModelPlayer
                          : (onPlayer(v[0]) && onPlayer(v[1]) && onPlayer(v[2])) || nearPlayer(v);
        };
        float cut = 1e30f, height = 0.0f;
        if (hideHead) {
            float top = me.position[1];
            for (size_t tri = 0; tri < w.triangleFlags.size(); ++tri)
                if (his(tri))
                    for (int k = 0; k < 3; ++k) top = std::max(top, w.vertices[tri * 3 + k].y);
            height = top - me.position[1];
            if (height > 0.5f) cut = me.position[1] + height * headCut_;
        }
        auto inHead = [&](const PlatformWorldVertex *v) {
            const float hi = std::max({v[0].y, v[1].y, v[2].y}), mid = (v[0].y + v[1].y + v[2].y) / 3;
            // Above the cut, or reaching over it from under (hair falling to his neck, his chin's
            // underside at the base of his neck).
            return mid > cut || (hi > cut && mid > cut - 0.16f * height);
        };
        for (size_t tri = 0; tri < w.triangleFlags.size(); ++tri) {
            const PlatformWorldVertex *v = &w.vertices[tri * 3];
            if (hideHead && his(tri) && inHead(v)) continue;
            // His Keyblade is in your hand, and his swings' trails are not drawn.
            if (hideHead && tagged && w.triangleModel[tri] >= kWorldModelAttached) continue;
            if (hide && tagged && w.triangleModel[tri] != 0) continue;
            if (hide && !tagged && ((onPlayer(v[0]) && onPlayer(v[1]) && onPlayer(v[2])) || nearPlayer(v))) continue;
            for (int k = 0; k < 3; ++k) t.vertices.push_back({{v[k].x, v[k].y, v[k].z}, v[k].u, v[k].v, v[k].color});
            t.triangleTexture.push_back(w.triangleTexture[tri]);
            t.triangleFlags.push_back(w.triangleFlags[tri]);  // the same bits (kTriFront ...)
            if (sprites)
                t.billboard.insert(t.billboard.end(), w.billboard.begin() + tri * 3 * kB, w.billboard.begin() + (tri + 1) * 3 * kB);
        }
        t.textures.resize(w.textures.size());
        for (size_t i = 0; i < w.textures.size(); ++i) {
            const PlatformWorldTexture &a = w.textures[i];
            WorldTexture &b = t.textures[i];
            b.key = a.key;
            b.width = a.width;
            b.height = a.height;
            b.repeatS = a.repeatS;
            b.repeatT = a.repeatT;
            b.flipS = a.flipS;
            b.flipT = a.flipT;
            b.rgba = a.rgba;
        }
        TakeHeldWeapon(w);
        TakeHands(w);
        t.lockOn = w.lockOn;
        t.lockOnAt = {w.lockOnAt[0], w.lockOnAt[1], w.lockOnAt[2]};
        t.manualSort = w.manualSort;
        t.lockOnKind = w.lockOnKind;
        t.lockOnLocked = w.lockOnLocked;
        std::copy(w.viewport, w.viewport + 4, t.viewport);
        std::copy(w.fadeMode, w.fadeMode + 2, t.fadeMode);
        std::copy(w.fadeFactor, w.fadeFactor + 2, t.fadeFactor);
        ++t.revision;
        // Gameplay, for the view rigs: the player in the field with the world captured in world
        // space, and no menu or movie. An event scene (an in-engine cutscene) too when View >
        // Cutscenes is Same as gameplay and its camera placed it in the world (its player may be
        // gone: the view stays where it was). Otherwise everything stays on the giant screen.
        const PlatformPlayer &pl = latestPlayer_;
        const bool event = pl.event || w.eventScene;
        const bool placed = standInWorld_.load() && w.worldSpace && !w.vertices.empty() && !screens_.movie;
        const bool playing = placed && !pl.menu && (event ? cutscenesInWorld_.load() : pl.valid);
        frame_.inGameplay = playing;
        frame_.inMenu = !playing;
        frame_.inCutscene = event;
        frame_.inBattle = pl.battle;
        frame_.inScript = pl.event || pl.choice;
        if (pl.valid) {
            frame_.playerPos = {pl.position[0], pl.position[1], pl.position[2]};
            frame_.playerYaw = YawOf(Vec3{std::sin(pl.heading), 0.0f, std::cos(pl.heading)});
        } else if (event && w.worldSpace && !eventAnchored_) {
            // A cutscene without the player (Roxas waking in his room): the view stands where its
            // camera looks as it begins, a unit (about a metre) below that, for the whole scene.
            frame_.playerPos = {w.target[0], w.target[1] - 1.0f, w.target[2]};
            eventAnchored_ = true;
        }
        if (!event) eventAnchored_ = false;
        if (w.hasCamera) {
            frame_.camera.eye = {w.eye[0], w.eye[1], w.eye[2]};
            frame_.camera.target = {w.target[0], w.target[1], w.target[2]};
            frame_.camera.up = {w.up[0], w.up[1], w.up[2]};
            frame_.camera.fovYRad = w.fovY;
            frame_.camera.nearZ = w.nearZ;
            frame_.camera.farZ = w.farZ;
            frame_.camera.hasMatrices = true;
            std::copy(w.lens, w.lens + 16, frame_.camera.proj);
            std::copy(w.view, w.view + 16, frame_.camera.view);
        }
    }
}

// The triangles of one of Roxas's models (kWorldModel...) into `to`, each vertex where `place`
// puts its point in the model's own space, with the textures they use.
template <typename Place>
void CopyTriangles(const PlatformWorld &w, uint8_t model, Place place, TexturedWorld *to) {
    TexturedWorld &k = *to;
    const size_t triangles = w.triangleFlags.size();
    k.vertices.clear();
    k.triangleTexture.clear();
    k.triangleFlags.clear();
    k.textures.clear();
    std::vector<int32_t> remap(w.textures.size(), -1);
    for (size_t t = 0; t < triangles; ++t) {
        if (w.triangleModel[t] != model) continue;
        for (int i = 0; i < 3; ++i) {
            const PlatformWorldVertex &v = w.vertices[t * 3 + i];
            const float *l = &w.local[(t * 3 + i) * 3];
            k.vertices.push_back({place(Vec3{l[0], l[1], l[2]}), v.u, v.v, v.color});
        }
        int32_t tex = w.triangleTexture[t];
        if (tex >= 0 && static_cast<size_t>(tex) < w.textures.size()) {
            if (remap[tex] < 0) {
                const PlatformWorldTexture &a = w.textures[tex];
                WorldTexture b;
                b.key = a.key;
                b.width = a.width;
                b.height = a.height;
                b.repeatS = a.repeatS;
                b.repeatT = a.repeatT;
                b.flipS = a.flipS;
                b.flipT = a.flipT;
                b.rgba = a.rgba;
                remap[tex] = static_cast<int32_t>(k.textures.size());
                k.textures.push_back(std::move(b));
            }
            tex = remap[tex];
        } else {
            tex = -1;
        }
        k.triangleTexture.push_back(tex);
        // Its sprite-ness is the world's business: in the hand it is plain triangles.
        k.triangleFlags.push_back(w.triangleFlags[t] & static_cast<uint8_t>(~kTriBillboard));
    }
    ++k.revision;
}

void DecompGame::TakeHeldWeapon(const PlatformWorld &w) {
    const size_t triangles = w.triangleFlags.size();
    if (w.triangleModel.size() != triangles || w.local.size() != w.vertices.size() * 3) return;
    // Of the models attached to Roxas, the longest is his Keyblade.
    int best = -1;
    WeaponFrame frame;
    for (int a = 0; a < kWorldAttachedMax; ++a) {
        std::vector<Vec3> points;
        for (size_t t = 0; t < triangles; ++t)
            if (w.triangleModel[t] == kWorldModelAttached + a)
                for (int k = 0; k < 3; ++k) {
                    const float *l = &w.local[(t * 3 + k) * 3];
                    points.push_back({l[0], l[1], l[2]});
                }
        // The capture keeps only solid models on him (his Keyblade has 7 to 12 polygons).
        if (points.size() < 4 * 3) continue;
        WeaponFrame f = FindWeaponFrame(points);
        if (f.valid && (best < 0 || f.length > frame.length)) {
            best = a;
            frame = f;
        }
    }
    if (best < 0) return;  // not drawn this frame: the last one stays
    TexturedWorld &k = heldWeapon_;
    {
        // A much smaller model on him for a moment (a part of an effect) does not replace the
        // Keyblade in your hand: the log showed it switching between 112 and 25 triangles.
        size_t n = 0;
        for (size_t t = 0; t < triangles; ++t) n += w.triangleModel[t] == kWorldModelAttached + best;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const size_t held = k.vertices.size() / 3;
        if (n * 2 < held && now - heldSeen_ < 3.0) return;
        if (n * 2 >= held) heldSeen_ = now;
    }
    // The game's model faces the other way about its blade from the frame found: turned half round.
    frame.side = frame.side * -1.0f;
    {
        // Into the boot log when what is taken changes (to tell a Keyblade from an effect).
        size_t n = 0;
        for (size_t t = 0; t < triangles; ++t) n += w.triangleModel[t] == kWorldModelAttached + best;
        if (n != k.vertices.size() / 3) {
            char line[160];
            std::snprintf(line, sizeof line, "keyblade: taken for your hand, %zu triangles, %.2f long (its model units), %zu textures",
                          n, frame.length, w.textures.size());
            Platform::Get().LogLine(line);
        }
    }
    CopyTriangles(w, kWorldModelAttached + best, [&](const Vec3 &l) { return frame.ToHand(l); }, &k);
}

void DecompGame::TakeHands(const PlatformWorld &w) {
    const size_t triangles = w.triangleFlags.size();
    if (w.triangleModel.size() != triangles || w.local.size() != w.vertices.size() * 3) return;
    for (int a = 0; a < 2; ++a) {
        const uint8_t model = static_cast<uint8_t>(kWorldModelHand + a);
        std::vector<Vec3> points;
        for (size_t t = 0; t < triangles; ++t)
            if (w.triangleModel[t] == model)
                for (int k = 0; k < 3; ++k) {
                    const float *l = &w.local[(t * 3 + k) * 3];
                    points.push_back({l[0], l[1], l[2]});
                }
        if (points.size() < 2 * 3) continue;  // not drawn this frame: the last one stays
        const HandFrame frame = FindHandFrame(points);
        if (!frame.valid) continue;
        if (points.size() != hands_[a].vertices.size()) {
            char line[160];
            std::snprintf(line, sizeof line, "hands: his %s hand taken for yours, %zu triangles, %.2f long (its model units)",
                          a ? "right" : "left", points.size() / 3, frame.length);
            Platform::Get().LogLine(line);
        }
        CopyTriangles(w, model, [&](const Vec3 &l) { return frame.ToHand(l); }, &hands_[a]);
    }
}

std::vector<std::string> DecompGame::Status(size_t lines) const {
    std::vector<std::string> out;
    Platform &p = Platform::Get();
    char head[96];
    switch (p.state()) {
    case PlatformState::Running: std::snprintf(head, sizeof head, "Running - %u frames", p.frames()); break;
    case PlatformState::Crashed: std::snprintf(head, sizeof head, "Stopped: %s", p.crashReason().c_str()); break;
    default: std::snprintf(head, sizeof head, "Not running"); break;
    }
    out.push_back(head);
    for (const std::string &l : p.RecentLog(lines)) out.push_back(l);
    return out;
}

}  // namespace khvr
