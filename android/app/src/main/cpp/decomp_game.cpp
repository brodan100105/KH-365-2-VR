#include "decomp_game.h"

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
    if (world) {
        PlatformPlayer player;
        p.ReadPlayer(&player);
        std::lock_guard<std::mutex> lock(screensMu_);
        std::swap(scratchWorld, latestWorld_);
        latestPlayer_ = player;
        ++latestWorldRevision_;
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
        const bool hide = hidePlayer_ && me.valid;
        auto onPlayer = [&](const PlatformWorldVertex &v) {
            const float dx = v.x - me.position[0], dz = v.z - me.position[2], dy = v.y - me.position[1];
            return dx * dx + dz * dz < 0.55f * 0.55f && dy > -0.2f && dy < 2.2f;
        };
        t.vertices.clear();
        t.triangleTexture.clear();
        t.triangleFlags.clear();
        t.vertices.reserve(w.vertices.size());
        for (size_t tri = 0; tri < w.triangleFlags.size(); ++tri) {
            const PlatformWorldVertex *v = &w.vertices[tri * 3];
            if (hide && onPlayer(v[0]) && onPlayer(v[1]) && onPlayer(v[2])) continue;
            for (int k = 0; k < 3; ++k) t.vertices.push_back({{v[k].x, v[k].y, v[k].z}, v[k].u, v[k].v, v[k].color});
            t.triangleTexture.push_back(w.triangleTexture[tri]);
            t.triangleFlags.push_back(w.triangleFlags[tri]);  // the same bits (kTriFront ...)
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
        std::copy(w.viewport, w.viewport + 4, t.viewport);
        std::copy(w.fadeMode, w.fadeMode + 2, t.fadeMode);
        std::copy(w.fadeFactor, w.fadeFactor + 2, t.fadeFactor);
        ++t.revision;
        // Gameplay, for the view rigs: the player in the field with the world captured, and no
        // menu, event scene or movie. Otherwise everything stays on the giant screen.
        const PlatformPlayer &pl = latestPlayer_;
        const bool playing = standInWorld_.load() && pl.valid && !pl.menu && !pl.event && !screens_.movie &&
                             w.hasCamera && !w.vertices.empty();
        frame_.inGameplay = playing;
        frame_.inMenu = !playing;
        frame_.inCutscene = false;
        if (pl.valid) {
            frame_.playerPos = {pl.position[0], pl.position[1], pl.position[2]};
            frame_.playerYaw = YawOf(Vec3{std::sin(pl.heading), 0.0f, std::cos(pl.heading)});
        }
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
