# Roadmap

Each iteration ends with something you can try on the headset and give feedback on.

## Iteration 1 (this one): the VR shell

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
- [x] Settings: 66 options on 10 tabs, in-headset panel, `khvr.ini`, generated reference
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
- [ ] First boot with a real ROM on the headset (needs you: send `files/khvr_boot.log`)
- [ ] Sound (the ARM7's sound driver is acknowledged, not played)
- [ ] MobiClip videos (the decoder's assembly parts are stubbed)

## Iteration 3: the world in VR

- [ ] GX geometry capture into world-space draw lists (textures, polygon attributes, toon/edge
      marking approximations)
- [ ] Game hooks: camera read/override, player state, analog movement, room-scale step, culling
- [ ] Calibrate **Game > World units per meter** from Roxas's model height
- [ ] Panoramic and first person in Twilight Town

## Iteration 4 and later

- [ ] Physical hits: Keyblade-vs-hitbox tests against the enemy framework's collision spheres
- [ ] Swing kinds driving combo animations, aim assist toward `GameFrame::nearestEnemy`
- [ ] First-person body: the character model drawn without its head
- [ ] Wall fade in panoramic mode, cutscenes from where you stand
- [ ] Sound (SDAT sequences), MobiClip videos
- [ ] Frame interpolation above the game's 30 Hz
