# KH 358/2 Days VR

A standalone **Meta Quest 3** VR port of *Kingdom Hearts 358/2 Days*, built on the
[khdays-decomp](https://github.com/Yokimitsuro/khdays-decomp) decompilation, in the spirit of
[GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) for Super Mario Galaxy: the game's own code
runs natively on the headset (no emulator) and is drawn in stereo from wherever your head is.

> **Status: iteration 1, the VR shell.** The OpenXR app, both VR modes, motion combat, the
> settings panel and comfort options are in place, running against a **demo arena** that stands
> in for the game. Booting the real game from the decomp is the next iteration
> ([roadmap](docs/ROADMAP.md)). None of this has run on a headset yet.

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

The game itself is not included and never will be: once the decomp boots (iteration 2) you will
copy your own legally-obtained ROM to the headset.

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

`tools/guest/probe_decomp.sh` measures how much of the decomp builds as the port's 32-bit guest
module (`git submodule update --init decomp` first). [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
explains the design: why the game code is compiled 32-bit, how the DS hardware is replaced, and how
the game's 3D scene is re-projected for each eye.

## Legal

This repository contains no game code, ROM, assets or extracted data. The decomp submodule is
C source only and requires your own copy of the game to produce anything playable. Kingdom Hearts
is a trademark of Disney and Square Enix; this is an unofficial fan project, not affiliated with
either.
