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

// A captured model as the headset draws it, and back (the saved place, the Keyblade kept from
// your game between launches).
void ToTextured(const PlatformWorld &w, TexturedWorld *out) {
    TexturedWorld &t = *out;
    t = TexturedWorld();
    t.vertices.reserve(w.vertices.size());
    for (const PlatformWorldVertex &v : w.vertices) t.vertices.push_back({{v.x, v.y, v.z}, v.u, v.v, v.color});
    t.triangleTexture = w.triangleTexture;
    t.triangleFlags = w.triangleFlags;
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
    t.revision = 1;
}

void ToPlatform(const TexturedWorld &t, PlatformWorld *out) {
    PlatformWorld &w = *out;
    w = PlatformWorld();
    w.vertices.reserve(t.vertices.size());
    for (const TexturedVertex &v : t.vertices) {
        PlatformWorldVertex p{};
        p.x = v.position.x;
        p.y = v.position.y;
        p.z = v.position.z;
        p.u = v.u;
        p.v = v.v;
        p.color = v.color;
        w.vertices.push_back(p);
    }
    w.triangleTexture = t.triangleTexture;
    w.triangleFlags = t.triangleFlags;
    w.textures.resize(t.textures.size());
    for (size_t i = 0; i < t.textures.size(); ++i) {
        const WorldTexture &a = t.textures[i];
        PlatformWorldTexture &b = w.textures[i];
        b.key = a.key;
        b.width = a.width;
        b.height = a.height;
        b.repeatS = a.repeatS;
        b.repeatT = a.repeatT;
        b.flipS = a.flipS;
        b.flipT = a.flipT;
        b.rgba = a.rgba;
    }
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
    dataDir_ = dataDir;
    {
        // The Keyblade last taken from your game (khvr_keyblade.bin), until the game draws one.
        PlatformWorld kept;
        if (PlatformWorldRead(dataDir + "/khvr_keyblade.bin", &kept) && !kept.vertices.empty() &&
            kept.vertices.size() == kept.triangleFlags.size() * 3)
            ToTextured(kept, &heldWeapon_);
    }
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
    {
        // Room-scale walking: the steps asked since the last game frame come again with every
        // call until that frame shows (then they start over); the game gets each part once.
        if (frameShown_) stepSent_ = {};
        frameShown_ = false;
        const Vec3 step = requests.hasRoomScaleStep ? requests.roomScaleStep : Vec3{};
        const Vec3 more = step - stepSent_;
        if (std::fabs(more.x) > 1e-6f || std::fabs(more.z) > 1e-6f) {
            Platform::Get().AddPlayerStep(more.x, more.z);
            stepAsked_ += Vec3{more.x, 0.0f, more.z};
        }
        stepSent_ = step;
    }

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
        frameShown_ = true;
    }
    // Without a player in the field (title, menus, mission review), with a menu or event scene
    // over it, or while a choice waits (an NPC's question: up and down), the stick moves a cursor.
    frame_.inTitle = latestPlayer_.title;
    frame_.menuInput = !latestPlayer_.valid || latestPlayer_.menu || latestPlayer_.event || latestPlayer_.choice;
    if (!worldCapture_.load()) {
        frame_.inGameplay = false;
        frame_.inMenu = true;
        if (!texturedWorld_.vertices.empty()) {
            texturedWorld_ = TexturedWorld();
            ++texturedWorld_.revision;
        }
    } else if (latestWorldRevision_ != shownWorldRevision_ && !KeepPlacedScene()) {
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
            const uint8_t model = tagged ? w.triangleModel[tri] : 0;
            const bool his2 = model != 0 && model != kWorldModelCharacter;  // his, not another's
            if (hideHead && his2 && model >= kWorldModelAttached) continue;
            if (hide && his2) continue;
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
        KeepEquippedWeapon();
        TakeHeldWeapon(w);
        TakeHands(w);
        t.lockOn = w.lockOn;
        t.lockOnAt = {w.lockOnAt[0], w.lockOnAt[1], w.lockOnAt[2]};
        t.manualSort = w.manualSort;
        t.lockOnKind = w.lockOnKind;
        t.lockOnLocked = w.lockOnLocked;
        std::copy(w.viewport, w.viewport + 4, t.viewport);
        // The game's fades, but none that comes for a frame or two only (a flash over a zoom, Day
        // 52's Tailbunker): on the DS a blink, in the headset your whole view going black. A fade
        // shows once it has held three captures (a tenth of a second), at the least of them.
        for (int i = 0; i < 2; ++i) {
            const int seen = w.fadeMode[i] ? w.fadeFactor[i] : 0;
            for (int k = 7; k > 0; --k) fadeSeen_[i][k] = fadeSeen_[i][k - 1];
            fadeSeen_[i][0] = w.fadeMode[i] ? seen : 0;
            if (w.fadeMode[i]) fadeModeSeen_[i] = w.fadeMode[i];
            // In a scene drawing 3D on both screens, eight (the screen it blacks out goes dark
            // every other frame, and a skipped frame let two dark ones through in a row: the
            // whole view flashed black, the Coliseum's cutscene).
            const int window = w.alternatingScreens ? 8 : 3;
            int held = fadeSeen_[i][0];
            for (int k = 1; k < window; ++k) held = std::min(held, fadeSeen_[i][k]);
            if (seen > held + 4) {
                char line[96];
                std::snprintf(line, sizeof line, "view: a brief fade left out (screen %d, mode %d, %d/16)", i,
                              w.fadeMode[i], w.fadeFactor[i]);
                Platform::Get().LogLine(line);
            }
            t.fadeMode[i] = held ? fadeModeSeen_[i] : 0;
            t.fadeFactor[i] = held;
        }
        ++t.revision;
        // Gameplay, for the view rigs: the player in the field with the world captured in world
        // space, and no menu or movie. An event scene (an in-engine cutscene) too when View >
        // Cutscenes is Same as gameplay and its camera placed it in the world (its player may be
        // gone: the view stays where it was). Otherwise everything stays on the giant screen.
        const PlatformPlayer &pl = latestPlayer_;
        const bool event = pl.event || w.eventScene;
        // A cutscene drawn as a picture on a few polygons (the scene captured and shown through a
        // flat card, with static over it: Agrabah, Sora through the static) is no world to stand
        // in: it goes to the giant screen as the DS draws it, not a speck in an empty sky.
        // (A cutscene drawing 3D on both screens in turn stays in the world: its main scene around
        // you, its frames of the other screen left out of the world already.)
        const bool flatPicture = (latestPlayer_.event || w.eventScene) && !w.alternatingScreens &&
                                 (w.triangleFlags.size() < 16 || w.capturedPicture);
        const bool placed = standInWorld_.load() && w.worldSpace && !w.vertices.empty() && !screens_.movie && !flatPicture;
        const bool playing = placed && !pl.menu && (event ? cutscenesInWorld_.load() : pl.valid);
        frame_.inGameplay = playing;
        frame_.inMenu = !playing;
        frame_.inCutscene = event;
        frame_.inBattle = pl.battle;
        frame_.inScript = pl.event || pl.choice;
        frame_.twoScreens = w.alternatingScreens;
        frame_.actionsTaken = pl.actionsTaken;
        frame_.roomStepsReported = true;
        frame_.roomStepTaken = {pl.stepTaken[0], 0.0f, pl.stepTaken[1]};
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
    // Your room-scale steps the player's position does not show yet (asked since it was read).
    frame_.roomStepUnapplied = stepAsked_ - frame_.roomStepTaken;
}

// The triangles of one of Roxas's models (kWorldModel...) into `to`, each vertex where `place`
// puts its point in the model's own space, with the textures they use.
template <typename Place>
void CopyTriangles(const PlatformWorld &w, uint8_t model, Place place, TexturedWorld *to) {
    // Into a new one first: when nothing changed (a rigid model, as it mostly is), the old stays,
    // its revision too, so the renderer keeps what it built (and the textures are not copied).
    TexturedWorld k;
    std::vector<int32_t> source;  // per texture of k: its index in w
    const size_t triangles = w.triangleFlags.size();
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
                remap[tex] = static_cast<int32_t>(k.textures.size());
                k.textures.push_back(std::move(b));
                source.push_back(tex);
            }
            tex = remap[tex];
        } else {
            tex = -1;
        }
        k.triangleTexture.push_back(tex);
        // Its sprite-ness is the world's business: in the hand it is plain triangles.
        k.triangleFlags.push_back(w.triangleFlags[t] & static_cast<uint8_t>(~kTriBillboard));
    }
    TexturedWorld &old = *to;
    bool same = old.vertices.size() == k.vertices.size() && old.triangleTexture == k.triangleTexture &&
                old.triangleFlags == k.triangleFlags && old.textures.size() == k.textures.size() &&
                std::memcmp(old.vertices.data(), k.vertices.data(), k.vertices.size() * sizeof(TexturedVertex)) == 0;
    for (size_t i = 0; same && i < k.textures.size(); ++i) same = old.textures[i].key == k.textures[i].key;
    if (same) return;
    for (size_t i = 0; i < k.textures.size(); ++i) k.textures[i].rgba = w.textures[source[i]].rgba;
    k.revision = old.revision + 1;
    old = std::move(k);
}

void DecompGame::TakeHeldWeapon(const PlatformWorld &w) {
    const size_t triangles = w.triangleFlags.size();
    if (w.triangleModel.size() != triangles || w.local.size() != w.vertices.size() * 3) return;
    // His hands as the game draws them this frame (world space): the part of a weapon nearest
    // one is where he holds it (a Struggle bat's guard by its handle was taken for its tip, the
    // bat drawn upside down in your hand).
    // Each hand's middle: where along a weapon it is held is where the middle of his hand is
    // (the point of the weapon nearest his hand was a Keyblade's wide guard, not its handle).
    Vec3 handMid[2];
    int handCount[2] = {0, 0};
    for (size_t t = 0; t < triangles; ++t)
        for (int hnd = 0; hnd < 2; ++hnd)
            if (w.triangleModel[t] == kWorldModelHand + hnd)
                for (int k = 0; k < 3; ++k) {
                    const PlatformWorldVertex &v = w.vertices[t * 3 + k];
                    handMid[hnd] += Vec3{v.x, v.y, v.z};
                    ++handCount[hnd];
                }
    for (int hnd = 0; hnd < 2; ++hnd)
        if (handCount[hnd]) handMid[hnd] = handMid[hnd] * (1.0f / static_cast<float>(handCount[hnd]));
    // Of the models attached to Roxas, the longest is his Keyblade.
    int best = -1;
    WeaponFrame frame;
    for (int a = 0; a < kWorldAttachedMax; ++a) {
        std::vector<Vec3> points, world;
        for (size_t t = 0; t < triangles; ++t)
            if (w.triangleModel[t] == kWorldModelAttached + a)
                for (int k = 0; k < 3; ++k) {
                    const float *l = &w.local[(t * 3 + k) * 3];
                    points.push_back({l[0], l[1], l[2]});
                    const PlatformWorldVertex &v = w.vertices[t * 3 + k];
                    world.push_back({v.x, v.y, v.z});
                }
        // The capture keeps only solid models on him (his Keyblade has 7 to 12 polygons).
        if (points.size() < 4 * 3) continue;
        // Its length in the world (the two vertices farthest apart), and the hand nearest that
        // line: how far along it the hand's middle is, the same in its own model's space (it is
        // only moved, turned and scaled).
        Vec3 held;
        bool hint = false;
        {
            size_t ia = 0, ib = 0;
            for (size_t i = 0; i < world.size(); ++i)
                if (Length(world[i] - world[0]) > Length(world[ia] - world[0])) ia = i;
            for (size_t i = 0; i < world.size(); ++i)
                if (Length(world[i] - world[ia]) > Length(world[ib] - world[ia])) ib = i;
            const Vec3 along = world[ib] - world[ia];
            const float len2 = Dot(along, along);
            float nearest = 0.25f;  // within a hand's length of the blade's line
            for (int hnd = 0; hnd < 2 && len2 > 1e-8f; ++hnd) {
                if (!handCount[hnd]) continue;
                const float s = Clamp(Dot(handMid[hnd] - world[ia], along) / len2, 0.0f, 1.0f);
                const float d = Length(world[ia] + along * s - handMid[hnd]);
                if (d < nearest) {
                    nearest = d;
                    held = points[ia] + (points[ib] - points[ia]) * s;
                    hint = true;
                }
            }
        }
        WeaponFrame f = FindWeaponFrame(points, hint ? &held : nullptr);
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
            char line[256];
            std::snprintf(line, sizeof line,
                          "keyblade: taken for your hand, %zu triangles, %.2f long (its model units), %zu textures; "
                          "in its model, blade %+.2f %+.2f %+.2f, teeth %+.2f %+.2f %+.2f",
                          n, frame.length, w.textures.size(), frame.blade.x, frame.blade.y, frame.blade.z,
                          -frame.side.x, -frame.side.y, -frame.side.z);
            Platform::Get().LogLine(line);
        }
    }
    const size_t before = k.vertices.size();
    CopyTriangles(w, kWorldModelAttached + best, [&](const Vec3 &l) { return frame.ToHand(l); }, &k);
    // Kept on the headset (from your own game) so it is in your hand from the start next time,
    // before Roxas has drawn it in a scene: one file per weapon (the row the scene loaded for
    // him), and the last one taken; saved again when another is taken.
    const int row = Platform::Get().WeaponRow(0);
    const bool changed = k.vertices.size() != before || row != heldRow_;
    heldRow_ = row;
    if (changed && !dataDir_.empty()) {
        PlatformWorld keep;
        ToPlatform(k, &keep);
        PlatformWorldWrite(dataDir_ + "/khvr_keyblade.bin", keep);
        if (row >= 0) PlatformWorldWrite(dataDir_ + "/khvr_keyblade_" + std::to_string(row) + ".bin", keep);
    }
}

void DecompGame::KeepEquippedWeapon() {
    // A scene loaded another weapon for Roxas than the one in your hand: the one kept for it, if
    // he has drawn it before (else the last one stays until he draws this one).
    const int row = Platform::Get().WeaponRow(0);
    if (row < 0 || row == heldRow_ || dataDir_.empty()) return;
    heldRow_ = row;
    PlatformWorld kept;
    if (PlatformWorldRead(dataDir_ + "/khvr_keyblade_" + std::to_string(row) + ".bin", &kept) && !kept.vertices.empty() &&
        kept.vertices.size() == kept.triangleFlags.size() * 3) {
        ToTextured(kept, &heldWeapon_);
        Platform::Get().LogLine("keyblade: weapon " + std::to_string(row) + " from the one kept for it");
    }
}

bool DecompGame::SavePlace() {
    std::lock_guard<std::mutex> lock(screensMu_);
    if (dataDir_.empty() || !latestWorld_ || !latestWorld_->worldSpace || !latestPlayer_.valid) return false;
    const PlatformWorld &w = *latestWorld_;
    const size_t triangles = w.triangleFlags.size();
    if (w.vertices.size() != triangles * 3) return false;
    // Without Roxas and what he holds (his tagged models), the other characters (those following
    // him), and the sprites' turning.
    PlatformWorld place;
    place.textures = w.textures;
    place.worldSpace = true;
    const bool tagged = w.triangleModel.size() == triangles;
    for (size_t t = 0; t < triangles; ++t) {
        if (tagged && w.triangleModel[t] != 0) continue;
        for (int k = 0; k < 3; ++k) place.vertices.push_back(w.vertices[t * 3 + k]);
        place.triangleTexture.push_back(w.triangleTexture[t]);
        place.triangleFlags.push_back(w.triangleFlags[t] & static_cast<uint8_t>(~kWorldBillboard));
    }
    if (place.triangleFlags.empty() || !PlatformWorldWrite(dataDir_ + "/khvr_place.bin", place)) return false;
    if (FILE *f = std::fopen((dataDir_ + "/khvr_place.txt").c_str(), "w")) {
        std::fprintf(f, "%f %f %f %f\n", latestPlayer_.position[0], latestPlayer_.position[1], latestPlayer_.position[2],
                     latestPlayer_.heading);
        std::fclose(f);
    }
    Platform::Get().LogLine("place: saved as the loading area (" + std::to_string(place.triangleFlags.size()) + " triangles)");
    return true;
}

bool DecompGame::LoadPlace(TexturedWorld *world, Vec3 *feet, float *yaw) {
    if (dataDir_.empty()) return false;
    PlatformWorld w;
    if (!PlatformWorldRead(dataDir_ + "/khvr_place.bin", &w) || w.vertices.size() != w.triangleFlags.size() * 3) return false;
    float x = 0, y = 0, z = 0, heading = 0;
    FILE *f = std::fopen((dataDir_ + "/khvr_place.txt").c_str(), "r");
    if (!f) return false;
    const int n = std::fscanf(f, "%f %f %f %f", &x, &y, &z, &heading);
    std::fclose(f);
    if (n != 4) return false;
    ToTextured(w, world);
    *feet = {x, y, z};
    *yaw = YawOf(Vec3{std::sin(heading), 0.0f, std::cos(heading)});
    return true;
}

bool DecompGame::KeepPlacedScene() {
    // A cutscene placed in the world whose camera is lost for a moment (a quick zoom, Day 52's
    // Tailbunker over the archway) keeps its last placed picture rather than dropping to the
    // giant screen and back (a black flicker), for up to a second.
    const PlatformWorld &w = *latestWorld_;
    const bool event = latestPlayer_.event || w.eventScene;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!event) {
        placedTriangles_ = 0;
        return false;
    }
    if (w.worldSpace && !w.vertices.empty()) {
        // A capture with most of the scene gone at once (a tenth of what it had) for a moment:
        // kept out too, for up to a quarter of a second.
        const size_t n = w.triangleFlags.size();
        if (placedTriangles_ > 200 && n * 10 < placedTriangles_ && now - placedSceneAt_ < 0.25) {
            Platform::Get().LogLine("view: a cutscene frame with most of the scene missing left out");
            return true;
        }
        placedSceneAt_ = now;
        placedTriangles_ = n;
        return false;
    }
    if (now - placedSceneAt_ < 1.0) {
        static double logged = 0;
        if (now - logged > 1.0) {
            logged = now;
            Platform::Get().LogLine("view: a cutscene frame without its camera left out");
        }
        return true;
    }
    return false;
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
