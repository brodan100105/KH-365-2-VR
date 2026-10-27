# Roadmap

Each iteration ends with something you can try on the headset and give feedback on.

## Iteration 1: the VR shell

Implemented; the core logic is unit-tested, but none of it has run on a headset yet. Your
feedback from the first install decides what changes next.

- [x] Quest 3 OpenXR app (GLES 3, stereo swapchains, MSAA, refresh rate, passthrough, fixed
      foveation, Touch controllers, haptics)
- [x] Panoramic third-person rig: life size to diorama, follow modes, dead zone, snap/smooth turn
- [x] First-person rig: head on the character, height matching, room-scale walking with
      push-back, body turning
- [x] Giant screen: the game camera re-rendered per eye on a curved 4:3 screen, HUD on it
- [x] Motion combat: swings (with kind), guard pose, point-and-trigger casting, stick-flick dodge,
      physical jump; all mapped to DS buttons through configurable bindings
- [x] Settings: 76 options on 11 tabs, in-headset panel, `khvr.ini`, generated reference
- [x] Comfort: vignette on artificial motion, blink on cuts and snap turns, seated mode
- [x] HUD panels: top-screen HUD floating or head-locked, touch screen on the left wrist with
      laser touch
- [x] Demo arena standing in for the game, so all of the above can be tested now
- [x] Measured how much of the decomp builds as a 32-bit guest (`tools/guest/probe_decomp.sh`)

## Iteration 2 (this one): the game boots

- [x] Guest build: all 24,497 C files of the decomp compile and link (one sound streaming
      function is stubbed); wasm32 -> wasm2c -> ARM64, in the APK
- [x] Memory map with DTCM and the shared area at their DS addresses, mwcc's struct layout
- [x] Threads, interrupts, VBlank frame loop, timers, DMA, divider, IPC
- [x] The ARM7's answers: PXI handshake, power, touch, RTC, sound driver, card backup (saves)
- [x] ROM file system from your ROM, overlays, save file
- [x] 2D engines rendered to the giant screen and the wrist screen; pad and touch input
- [x] Boot log with backtraces (`tools/symbolize.py`), desktop runner, fake-ROM boot test in CI:
      the title scene loads
- [x] First boot with a real ROM on the headset: title screen, new game, the opening (movie
      timed without a picture, START skips), "Day 1"
- [ ] Into the field: the action-script stall after "Day 1"
- [x] Loading area, hand screen (in hand / on wrist, follows your view), Sharp upscaling

## Iteration 3: movies and sound

Planned in detail in [ITERATION3_PLAN.md](ITERATION3_PLAN.md).

- [x] Movies with picture: the game's own MobiClip player (its C++ parts and the decomp's
      portable frame decoder compiled into the port, the row blitter written in C)
- [x] Audio output (AAudio), the DS's 16-channel mixer, the ARM7 sound driver's commands
- [x] The sequence player (SSEQ/SBNK/SWAR from `sound_data.sdat`), streams, alarms
- [ ] Sound settings (master, music, effects, movie volume)

## Iteration 4: the world in VR

- [x] The DS's 3D engine: geometry engine (matrix stacks, lighting, texture coordinate modes,
      strips, clipping, box/position/vector tests) and a software rasterizer (all texture
      formats, modulate/decal/toon/highlight, alpha test and blending, depth/W buffer, fog),
      drawn as the top screen's BG0 (`platform/src/hle_gx.cpp`, `khdays_run --gx-test`)
- [ ] Shadow volumes, edge marking, anti-aliasing, wireframe polygons, the rear-plane bitmap
- [ ] GX geometry capture into world-space draw lists (textures, polygon attributes, toon/edge
      marking approximations)
- [ ] Game hooks: camera read/override, player state, analog movement, room-scale step, culling
- [ ] Calibrate **Game > World units per meter** from Roxas's model height
- [ ] Panoramic and first person in Twilight Town
- [ ] Loading area on the Twilight Town clock tower: the first time you reach it, the world-space
      capture saves that scene (geometry and textures, from your own ROM as the game draws it) to
      the KHDaysVR folder; later boots load you onto it instead of the Heart's Station, which stays
      the fallback until then. Nothing from the game goes into the APK.

## Iteration 5 and later

- [ ] Physical hits: Keyblade-vs-hitbox tests against the enemy framework's collision spheres
- [ ] Swing kinds driving combo animations, aim assist toward `GameFrame::nearestEnemy`
- [ ] First-person body: the character model drawn without its head
- [ ] Wall fade in panoramic mode, cutscenes from where you stand
- [ ] Positional (VR) sound for the game's 3D sound sources
- [ ] Frame interpolation above the game's 30 Hz

## Last step: the public release

Until the game is done, nothing changes: the repository stays private, CI builds the APK on every
push, and testers download it from Actions. Before the repository goes public:

- [ ] **No prebuilt APK anywhere public.** The APK holds the game's code compiled from the decomp,
      so the public workflow only builds and runs the tests; the APK upload stays off.
- [ ] **Players build their own APK, in a few clicks.** The README's install guide starts with:
      import this repository into a *private* repository of your own on GitHub (Import repository,
      or Use this template, set to private), open its Actions tab, run **build**, and download
      khdays-vr-apk, then the SideQuest steps as now. No tools on the player's PC. For those who
      prefer their own machine, one command builds it in Docker (`tools/build_apk.sh`).
- [ ] **Neutral name and branding**: an app name and icon without the game's title or logos, the
      unofficial fan project notice in the app and the README.
- [ ] **Legal section** in the README: own cartridge only (dump it yourself, never download a ROM),
      no ROM links, no game data in the repository or its history (checked before publishing), no
      money involved, the decomp is its own project.
- [ ] **History check**: no ROM, save, snapshot, world dump or log in any commit.
- [ ] Later, the strongest option: build the game's code on the player's side from their own ROM
      (a static recompiler for the ARM9 code, as Zelda64Recomp does for N64 games), so neither the
      repository nor any APK carries the game's code at all.
