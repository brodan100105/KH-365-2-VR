# KH 358/2 Days VR

A standalone **Meta Quest 3** VR port of *Kingdom Hearts 358/2 Days*, built on the
[khdays-decomp](https://github.com/Yokimitsuro/khdays-decomp) decompilation, in the spirit of
[GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) for Super Mario Galaxy: the game's own code
runs natively on the headset (no emulator) and is drawn in stereo from wherever your head is.

> **Status: iteration 2, the game boots.** The whole decomp builds into the APK and runs on the
> platform layer: OS, threads, interrupts, the ARM7's answers, file system and overlays come up
> and the title scene loads. The game is shown on the giant screen (2D engines; the 3D world in VR
> is iteration 3). It has not run with a real ROM yet: your first boot log decides what gets
> fixed next. Without a ROM the app runs the demo arena from iteration 1 ([roadmap](docs/ROADMAP.md)).

## View modes

| Mode | What it is |
|---|---|
| **Panoramic (3rd person)** | You stand in the level behind the character, like the game's camera but in 6DoF: walk around, lean in, look anywhere. **World scale** goes from life size down to a tabletop diorama. The view follows with a dead zone (or locked, or teleporting), with snap or smooth turning around the character. |
| **First person** | You are the character. Your head is the character's head (the world is scaled so your eye height matches theirs), walking in your room moves them and walls push you back, and the Keyblade is in your hand: swing it to attack, hold it across your body to block, point your other hand and pull the trigger to cast. |
| **Giant screen** | The game's camera on a big curved 4:3 screen, re-rendered at headset resolution, optionally stereoscopic. Used for menus and cutscenes, or for the whole game if you like. |

Switch between panoramic and first person at any time by holding the right thumbstick click.

## Settings

Hold the left **Menu** button for the VR settings panel: ten tabs (View, Screen, Camera, First
person, Combat, Controls, Comfort, Picture, HUD, Game) with every option, each tab with its own
**Defaults** button. The same options live in
`/sdcard/Android/data/com.khdays.vr/files/khvr.ini`. See [docs/SETTINGS.md](docs/SETTINGS.md) for
the full list and [docs/CONTROLS.md](docs/CONTROLS.md) for the controls.

## Installing

Every push builds an APK on GitHub Actions (**Actions > build > khdays-vr-apk**). With developer
mode on the headset:

```sh
adb install -r app-release.apk
```

To play the game, copy your own dump of the **European** cartridge (game code `YKGP`; the
decompilation only matches that release) to the headset, with SideQuest's file browser or adb:

```sh
adb push "Kingdom Hearts 358-2 Days (EU).nds" /sdcard/Android/data/com.khdays.vr/files/game/
```

The app creates `files/game/` on its first launch; any file name ending in `.nds` works. Without a
ROM it shows the demo arena and says why. The game writes its save to `files/khdays.sav` and a boot
log to `files/khvr_boot.log`: if something goes wrong, pull that log and send it over.

The ROM is never part of the repository or the APK. Note that the APK does contain the game's code
compiled from the decomp, so keep its download private (a private repository keeps CI artifacts
private).

## Building

The core (settings, view rigs, combat, input, demo game) builds and tests on any desktop:

```sh
cmake -S core -B build/core && cmake --build build/core && ctest --test-dir build/core
```

The APK needs JDK 17 and the Android SDK with NDK r27 (Gradle downloads the rest, including the
Khronos OpenXR loader from Maven Central):

```sh
cd android && ./gradlew assembleRelease
```

The game itself is built as a 32-bit "guest" from the decomp (`git submodule update --init decomp`
first; needs clang/lld 17+ and wabt 1.0.34):

```sh
python3 guest/build_guest.py          # decomp -> wasm32 -> C in build/guest/wasm2c
cmake -S platform -B build/platform && cmake --build build/platform
build/platform/khdays_run --rom your.nds --frames 600 --screens out   # headless desktop run
```

The Android build picks the guest up automatically when `build/guest/wasm2c` exists; CI always
builds it. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
explains the design: why the game code is compiled 32-bit, how the DS hardware is replaced, and how
the game's 3D scene is re-projected for each eye.

## Legal

This repository contains no game code, ROM, assets or extracted data. The decomp submodule is
C source only and requires your own copy of the game to produce anything playable. Kingdom Hearts
is a trademark of Disney and Square Enix; this is an unofficial fan project, not affiliated with
either.
