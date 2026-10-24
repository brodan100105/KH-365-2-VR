// The decompiled 358/2 Days as a GameSource: finds the user's ROM, boots it on the platform
// layer (platform/), and steps it at the DS's 59.83 Hz on its own thread, so the headset's frame
// rate never waits for the game. The VR thread only exchanges input and finished screens.
//
// Iteration 2 shows the game on the giant screen (menus, title, the touch screen on the wrist);
// the 3D world comes with the geometry capture of iteration 3, so frame() reports a menu.
#pragma once

#include <atomic>
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

    PlatformState state() const { return Platform::Get().state(); }
    // The game has shown an image (not a flat fade) on either screen, after the loading area's
    // few seconds: the loading area can give way.
    bool hasPicture() const { return hasPicture_; }
    uint32_t gameFrames() const { return Platform::Get().frames(); }
    // A few lines for the in-headset status panel: booting, crashed, the latest log.
    std::vector<std::string> Status(size_t lines) const;

private:
    void DriverLoop();

    GameFrame frame_;
    WorldDrawList world_;
    DsScreens screens_;

    std::thread driver_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> buttons_{0};
    std::atomic<uint32_t> touch_{0};  // bit 31 touching, x in bits 0-7, y in bits 8-15
    std::mutex screensMu_;
    std::unique_ptr<PlatformScreens> latest_;
    uint32_t latestRevision_ = 0, shownRevision_ = 0;
    double lastSample_ = -1.0;
    std::atomic<bool> hasPicture_{false};
};

}  // namespace khvr
