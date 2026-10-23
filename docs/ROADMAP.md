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

## Iteration 2: the game boots

- [ ] Guest build: source pre-pass for the link issues, wasm32 -> wasm2c -> ARM64, linked into the APK
- [ ] Memory map, VBlank-driven frame loop, overlays, the ROM file system from the user's ROM
- [ ] 2D engines: title screen, menus and the touch screen on the giant screen
- [ ] Pad and touch input through `Pad_Sample` and the touch hook
- [ ] `tools/push_rom.py` to copy your ROM to the headset

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
