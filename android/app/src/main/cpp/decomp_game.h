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
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "khvr/game_source.h"
#include "khvr/held_weapon.h"
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
    const TexturedWorld &heldWeapon() const override { return heldWeapon_; }
    const TexturedWorld &hand(int side) const override { return hands_[side & 1]; }
    // View > Stand in the world: report gameplay (the player in the field, no menu or event
    // scene) so the app leaves the giant screen for the captured world around you.
    void SetStandInWorld(bool on) { standInWorld_ = on; }
    // A place for the loading area and the menu's backdrop (Game > Use this place as the
    // loading area): the world captured where the player stands now, Roxas left out, saved next
    // to the save (khvr_place.bin, with where he stood and faced in khvr_place.txt). Made from
    // the user's own game as it runs; never part of the app.
    bool SavePlace();
    // Loads it: the world, where he stood (feet) and the way he faced (yaw, radians).
    bool LoadPlace(TexturedWorld *world, Vec3 *feet, float *yaw);
    // Picture > Draw while the game runs: the screens of a frame are drawn on a thread of their
    // own while the game runs the next frame, as the DS does, instead of before it.
    void SetParallelCapture(bool on) { parallel_ = on; }
    // First person: the game's own Roxas (his model and what he holds, the polygons right around
    // his feet) is left out of the captured world, or only his head and hair: what lies above
    // `headCut` of his height.
    enum class HidePlayer { None, All, Head };
    void SetHidePlayer(HidePlayer what, float headCut) {
        hidePlayer_ = what;
        headCut_ = headCut;
    }
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
    void SetCapturedCards(bool on) { Platform::Get().SetCapturedCards(on); }
    void TakeEnemyHits(std::vector<PlatformEnemyHit> *out) { Platform::Get().TakeEnemyHits(out); }
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
    TexturedWorld heldWeapon_;
    double heldSeen_ = 0.0;  // steady-clock seconds the held Keyblade was last drawn by the game
    // From a capture with Roxas's models: his Keyblade, in the hand's frame, into heldWeapon_.
    void TakeHeldWeapon(const PlatformWorld &w);
    // The scene loaded another weapon for Roxas: the one kept for it into heldWeapon_.
    void KeepEquippedWeapon();
    // The game's own Keyblade summoning effect, recorded when Roxas draws it (RecordSummon, each
    // frame his Keyblade is or is not drawn), played round the Keyblade in your hand.
    void RecordSummon(const PlatformWorld &w, const WeaponFrame *keyblade, double now);
    void FinishSummon();
    void LoadSummon();
    bool summonRecording_ = false;
    std::atomic<bool> summonWanted_{false};
    std::deque<std::pair<double, TexturedWorld>> summonBefore_;  // the effects of the last moment, world
    WeaponFrame summonFrame_;   // the Keyblade's frame in the world, last seen
    double summonLastFx_ = 0.0;  // when the glow last showed while recording
    double summonStart_ = 0.0, heldDrawnAt_ = -10.0;
    std::vector<TexturedWorld> summonTake_, summon_;
    std::vector<float> summonTimes_, summonAt_;
    unsigned summonRevision_ = 0;

    mutable std::mutex summonMu_;

public:
    // The game's summoning effect, frame by frame in the hand's frame (as heldWeapon), and each
    // frame's time from its start (seconds), copied out when it changed since `*revision`;
    // empty until Roxas has drawn his Keyblade once.
    // Record the summoning effect again at the next summoning (it is kept once recorded); false
    // again once recorded.
    void RecordSummonAgain() { summonWanted_ = true; }
    bool RecordingSummonAgain() const { return summonWanted_.load(); }
    bool TakeSummonEffect(unsigned *revision, std::vector<TexturedWorld> *frames, std::vector<float> *times) const {
        std::lock_guard<std::mutex> lock(summonMu_);
        if (*revision == summonRevision_) return false;
        *revision = summonRevision_;
        *frames = summon_;
        *times = summonAt_;
        return true;
    }

private:
    int heldRow_ = -1;  // the weapon (row) heldWeapon_ is, -1 not known
    // The Keyblade's frame in its model, kept after its first frames (kHeldFrameTakes).
    static constexpr int kHeldFrameTakes = 20;
    WeaponFrame heldFrame_;
    size_t heldFrameTris_ = 0;
    int heldFrameRow_ = -2, heldFrameTakes_ = 0;
    // ... and his hands, into hands_.
    TexturedWorld hands_[2];
    // Room-scale steps already given to the game this game frame, and whether a frame was shown.
    Vec3 stepSent_;
    Vec3 stepAsked_;  // all room-scale steps given to the game (world space)
    bool frameShown_ = false;
    void TakeHands(const PlatformWorld &w);
    // A cutscene's capture without its camera, within a second of one with it: kept out.
    bool KeepPlacedScene();
    double placedSceneAt_ = -1e9;  // steady-clock seconds a cutscene was last placed in the world
    size_t placedTriangles_ = 0;   // and its triangles then
    int fadeSeen_[2][8] = {};      // each screen's fade (0..16) in the last eight captures, newest first
    int fadeModeSeen_[2] = {0, 0};
    std::atomic<bool> worldCapture_{false};
    std::unique_ptr<PlatformWorld> latestWorld_;
    uint32_t latestWorldRevision_ = 0, shownWorldRevision_ = 0;
    PlatformPlayer latestPlayer_;  // read with each world capture (screensMu_)
    std::atomic<bool> standInWorld_{false};
    std::string dataDir_;  // the KHDaysVR folder (the save, the boot log, the saved place)
    std::atomic<bool> cutscenesInWorld_{false};
    bool eventAnchored_ = false;  // a cutscene without the player has its view's place
    HidePlayer hidePlayer_ = HidePlayer::None;
    float headCut_ = 0.8f;
    float lastFeet_[3] = {};
    bool hasLastFeet_ = false;
    double movingUntil_ = 0.0;  // steady-clock seconds until which he counts as moving
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
