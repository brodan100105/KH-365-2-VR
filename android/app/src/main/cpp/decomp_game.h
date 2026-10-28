// The decompiled 358/2 Days as a GameSource: finds the user's ROM, boots it on the platform
// layer (platform/), and steps it at the DS's 59.83 Hz on its own thread, so the headset's frame
// rate never waits for the game. The VR thread only exchanges input and finished screens.
//
// Iteration 2 shows the game on the giant screen (menus, title, the touch screen on the wrist);
// the 3D world comes with the geometry capture of iteration 3, so frame() reports a menu.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "khvr/game_source.h"
#include "khvr/platform.h"

namespace khvr {

class DecompGame : public GameSource {
public:
    ~DecompGame() override;

    // Looks for an EU ROM (*.nds, game code YKGP) in `romDirs`, in order, and boots it; the save
    // and the boot log go in `dataDir`. On failure `message` says what to do; on success it
    // names the ROM.
    bool Start(const std::vector<std::string> &romDirs, const std::string &dataDir, int language,
               std::string *message);

    void Advance(double now, const GameRequests &requests, uint16_t (*sampleButtons)(void *ctx),
                 void *ctx) override;
    const GameFrame &frame() const override { return frame_; }
    const WorldDrawList &world() const override { return world_; }
    const DsScreens &screens() const override { return screens_; }
    const TexturedWorld &texturedWorld() const override { return texturedWorld_; }
    // View > Stand in the world: report gameplay (the player in the field, no menu or event
    // scene) so the app leaves the giant screen for the captured world around you.
    void SetStandInWorld(bool on) { standInWorld_ = on; }
    // Picture > Draw while the game runs: the screens of a frame are drawn on a thread of their
    // own while the game runs the next frame, as the DS does, instead of before it.
    void SetParallelCapture(bool on) { parallel_ = on; }
    // First person without the full body: the game's own Roxas (his model and what he holds,
    // the polygons right around his feet) is left out of the captured world.
    void SetHidePlayer(bool on) { hidePlayer_ = on; }
    // The game draws what lies outside its own camera's view too (Platform::SetDrawAll).
    void SetDrawAll(bool on) { Platform::Get().SetDrawAll(on); }
    // The headset draws the field's cursors at their targets; the game leaves its own out.
    void SetOwnCursors(bool on) { Platform::Get().SetOwnCursors(on); }
    // View > Cutscenes is Same as gameplay: in-engine cutscenes placed in the world count as
    // gameplay for the view rigs.
    void SetCutscenesInWorld(bool on) { cutscenesInWorld_ = on; }
    // Screen > Game picture (Sharp, 3D depth) or standing in the world: capture the game's 3D
    // world each frame.
    void SetWorldCapture(bool on) {
        worldCapture_ = on;
        Platform::Get().SetWorldCapture(on);
    }

    PlatformState state() const { return Platform::Get().state(); }
    // The game has shown an image (not a flat fade) on either screen, after the loading area's
    // few seconds: the loading area can give way.
    bool hasPicture() const { return hasPicture_; }
    // Picture > 3D resolution: 1 to 4 times the DS's.
    void Set3dScale(int scale) { Platform::Get().Set3dScale(scale); }
    uint32_t gameFrames() const { return Platform::Get().frames(); }
    // A few lines for the in-headset status panel: booting, crashed, the latest log.
    std::vector<std::string> Status(size_t lines) const;

private:
    void DriverLoop();
    // Draws the screens (and the world, and reads the player) of the frame the game just
    // finished, and hands them to the app.
    void CaptureFrame(std::unique_ptr<PlatformScreens> &scratch, std::unique_ptr<PlatformWorld> &scratchWorld);
    void CaptureLoop();
    void OpenAudio();
    void CloseAudio();

    GameFrame frame_;
    WorldDrawList world_;
    DsScreens screens_;
    TexturedWorld texturedWorld_;
    std::atomic<bool> worldCapture_{false};
    std::unique_ptr<PlatformWorld> latestWorld_;
    uint32_t latestWorldRevision_ = 0, shownWorldRevision_ = 0;
    PlatformPlayer latestPlayer_;  // read with each world capture (screensMu_)
    std::atomic<bool> standInWorld_{false};
    std::atomic<bool> cutscenesInWorld_{false};
    bool eventAnchored_ = false;  // a cutscene without the player has its view's place
    bool hidePlayer_ = false;
    // The drawing thread (Picture > Draw while the game runs) and its hand-off.
    std::atomic<bool> parallel_{true};
    std::thread capture_;
    std::mutex captureMu_;
    std::condition_variable captureCv_;
    bool captureWanted_ = false, captureBusy_ = false, captureStop_ = false;
    std::chrono::steady_clock::time_point started_;

    std::thread driver_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> buttons_{0};
    std::atomic<uint32_t> touch_{0};  // bit 31 touching, x in bits 0-7, y in bits 8-15
    std::mutex screensMu_;
    std::unique_ptr<PlatformScreens> latest_;
    uint32_t latestRevision_ = 0, shownRevision_ = 0;
    double lastSample_ = -1.0;
    std::atomic<bool> hasPicture_{false};
    struct AAudioStreamStruct *audio_ = nullptr;  // the game's sound out (AAudio), while running
};

}  // namespace khvr
