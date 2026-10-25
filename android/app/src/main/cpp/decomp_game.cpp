#include "decomp_game.h"

#include <dirent.h>

#include <aaudio/AAudio.h>
#include <chrono>
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
    OpenAudio();
    driver_ = std::thread([this] { DriverLoop(); });
    return true;
}

void DecompGame::DriverLoop() {
    using clock = std::chrono::steady_clock;
    auto frame = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kDsFrameSeconds));
    auto next = clock::now();
    int skippedCaptures = 0;
    auto scratch = std::make_unique<PlatformScreens>();
    Platform &p = Platform::Get();
    auto nextBeat = clock::now() + std::chrono::seconds(2);
    const auto started = clock::now();
    while (!stop_ && (p.state() == PlatformState::Running || p.state() == PlatformState::Resetting)) {
        // "Return to title" resets the DS: boot the game again (logos, then the title screen).
        if (p.state() == PlatformState::Resetting) p.Restart();
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
        // Behind schedule (a heavy frame): keep the game, and so its sound, at full speed and
        // skip drawing the screens this time, at most twice in a row.
        if (clock::now() > next && skippedCaptures < 2) {
            ++skippedCaptures;
            p.NoteDisplay(true, 0.0f);
            continue;
        }
        skippedCaptures = 0;
        const auto captureStart = clock::now();
        p.CaptureScreens(scratch.get());
        p.NoteDisplay(false, std::chrono::duration<float, std::milli>(clock::now() - captureStart).count());
        // The loading area stays for 5 seconds at least, and until either screen shows an
        // image: the game starts on flat white or black (a brightness fade), which is not one.
        if (!hasPicture_ && clock::now() - started > std::chrono::seconds(5)) {
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
        screens_.topScale = latest_->topScale;
        if (latest_->topScale > 1) screens_.topHi = latest_->topHi;
        ++screens_.revision;
        ++frame_.frameCounter;
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
