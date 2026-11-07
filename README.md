# KH 358/2 Days VR

**KH 358/2 Days VR** is an unofficial fan port of *Kingdom Hearts 358/2 Days* (Nintendo DS) to the
**Meta Quest 3**. It is built on the [khdays-decomp](https://github.com/Yokimitsuro/khdays-decomp)
decompilation, in the spirit of [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) for Super
Mario Galaxy: the game's own code runs natively on the headset (no emulator). Play it on a giant
floating screen, or step into its world around you in third or first person, with
motion-controlled Keyblade combat and comfort options such as snap turning and a vignette.

**You must supply your own copy of the game.** This project contains no game data: no ROM, art,
music or other copyrighted material. At first launch the app looks for your own legally obtained
dump of the European cartridge (game code YKGP) on the headset and reads everything from it.
Nothing from the game is ever included in the APK. Please don't ask for or share ROMs.

**Early development:** expect bugs, crashes, missing features and settings that change between
builds. Much of the VR side is experimental and still being tuned on real hardware.

**100% vibecoded:** every line of code was written with [Claude Code](https://claude.com/claude-code),
Anthropic's AI coding agent, guided by playtesting feedback on a Quest 3.

*Kingdom Hearts is the property of Square Enix and Disney. This project is not affiliated with or
endorsed by Square Enix, Disney, Nintendo or Meta.*

> **Status: iteration 4, the world in VR.** The whole decomp builds into the APK and runs natively
> on the headset, with its movies, music and sound, and its 3D drawn by the port's own DS 3D
> engine. You can stand in the game's world in third or first person, cutscenes play around you,
> and first person has motion combat with Roxas's hands and the game's own Keyblade in yours
> ([roadmap](docs/ROADMAP.md), [iteration 4 plan](docs/ITERATION4_PLAN.md)). Story tested in the
> headset up to Day 97.

## VR progress

**Overall: about 56%** of the plan, from the first boot to a PC VR build. Percentages are estimates
from the project plan and playtesting on a Quest 3 (as of November 7, 2026).

| Area | Done | What it covers |
|---|---|---|
| **Native VR compatibility** | 86% | The game runs natively from your own cartridge dump (3D, 2D, saves, movies, music and sound); its world drawn around you with sprites that face you, the HUD and cutscenes; third and first person movement with comfort options; the loading area and menus. |
| **Completed story** | Day 97 of 358 (27%) | How far the story has been played through in the headset. Later days may still have scenes that need fixes. |
| **VR combat** | 42% | Roxas's hands and the game's Keyblade in yours, motion swings, guard and combos. Still to come: Keyblade effects, magic from the Keyblade, and combat where your own swing is the attack. |
| **Additional VR features** | 5% | Extras planned on top of the game: haptics, a wrist quickbar, positional sound, speech bubbles, photo mode. |
| **PC VR** | 0% | The same app for SteamVR and Meta Link, once the Quest version is stable. |

**Coming up:** making first-person combat feel VR native, then the extra VR features. Fixes from
playtesting land along the way.

## Download

**[Experimental build](https://github.com/brodan100105/KH-365-2-VR/releases/tag/experimental)**: the most recent build that has been tested on a headset,
`KH-Days-VR-experimental.apk`. Expect bugs: it is an early, experimental build, and newer work is
still being tested before it is shared. See [Installing](#installing-with-sidequest) below; you
need your own dump of the European cartridge.

## View modes

| Mode | What it is |
|---|---|
| **Panoramic (3rd person)** | You stand in the level behind the character, like the game's camera but in 6DoF: walk around, lean in, look anywhere. **World scale** goes from life size down to a tabletop diorama. The view follows with a dead zone (or locked, or teleporting), with snap or smooth turning around the character. |
| **First person** | You are the character. Your head is the character's head (the world is scaled so your eye height matches theirs), walking in your room moves them and walls push you back, and the Keyblade is in your hand: swing it to attack, hold it across your body to block, point your other hand and pull the trigger to cast. |
| **Giant screen** | The game's camera on a big curved 4:3 screen, re-rendered at headset resolution, optionally stereoscopic. Used for menus and cutscenes, or for the whole game if you like. |

Switch between panoramic and first person at any time by holding the right thumbstick click.

## Controls (Quest controllers)

| Button | Does |
|---|---|
| Left thumbstick | Move |
| A / B / X / Y | DS A / B / X / Y |
| Left Menu, tap / hold | START / VR settings panel |
| Left thumbstick click, hold | Show or hide the wrist (touch) screen |
| Left thumbstick click, tap | SELECT (giant screen and menus) |
| Right thumbstick left/right | Turn (snap or smooth) |
| Right thumbstick click, tap / hold | Recenter / switch third and first person |
| Left / right trigger | DS L / R (third person and giant screen) |
| Other hand's trigger at the touch screen | Touch |

**First person:** short-hold the weapon-hand grip to bring the Keyblade out, swing to attack (keep
swinging for combos), hold it across your face to guard, off-hand grip to lock on, off-hand trigger
to cast, walk in your room to move. The full guide, with every gesture and its setting, is in
[docs/CONTROLS.md](docs/CONTROLS.md); in the headset, **Button guide** on the settings panel shows
it on a drawing of both controllers.

## Settings

Hold the left **Menu** button for the VR settings panel: twelve tabs (View, Screen, Camera, First
person, Combat, Controls, Comfort, Picture, HUD, Hand screen, Handheld, Game) with every option,
each tab with its own **Defaults** button, and a **Button guide** that shows what every button
does on a drawing of both controllers. The same options live in
`/sdcard/Android/data/com.khdays.vr/files/khvr.ini`. See [docs/SETTINGS.md](docs/SETTINGS.md) for
the full list and [docs/CONTROLS.md](docs/CONTROLS.md) for the controls.

## Installing (with SideQuest)

You need: a Quest 3 (or 3S / Pro / 2) in developer mode, a PC, a USB-C cable, and **your own dump
of the European cartridge** of *Kingdom Hearts 358/2 Days* (game code `YKGP`; the decompilation
only matches that release). The ROM is never part of this project or its APK.

### 1. Get the APK

1. Download `KH-Days-VR-experimental.apk` from the
   **[Experimental build](https://github.com/brodan100105/KH-365-2-VR/releases/tag/experimental)** page (under Assets).

### 2. One-time setup

1. **Developer mode**: in the Meta Horizon app on your phone, go to **Menu > Devices > (your
   headset) > Headset settings > Developer mode** and turn it on. Restart the headset.
2. Install **SideQuest** (the "Advanced Installer") on your PC from
   [sidequestvr.com](https://sidequestvr.com/setup-howto).
3. Plug the headset into the PC. In the headset, accept **Allow USB debugging** (tick "Always
   allow from this computer"). SideQuest's top-left dot turns green when it sees the headset.

### 3. Install the app

In SideQuest, click **Install APK file from folder on computer** (the box with a down arrow at
the top) and pick `KH-Days-VR-experimental.apk`, or drag the APK onto the SideQuest window. Wait for
"Success" in SideQuest's task list.

To update later, install the new APK the same way, right over the old one: your save and
settings stay.

### 4. Put your ROM on the headset

1. In SideQuest, click **Manage files on the headset** (the folder icon at the top).
2. You start in the headset's main storage (`/sdcard`). Open the **KHDaysVR** folder, or create it
   if it isn't there yet (the app also creates it once you allow it file access).
3. Drag your `.nds` file into **KHDaysVR**. Any file name ending in `.nds` works.

It should look like this:

```
Quest storage (/sdcard)
└── KHDaysVR/
    ├── your-rom.nds        <- you put this here (EU release, YKGP)
    ├── khdays.sav          <- the game's save, created when you first save
    ├── khvr_boot.log       <- what happened while the game ran: send this with bug reports
    ├── khvr_files.txt      <- the ROM's file list (for development)
    ├── khvr_snap_N.bin     <- screen snapshots (for development)
    └── khvr_world_N.bin    <- 3D captures, with Screen > Game picture on Sharp or 3D depth (for development)
```

(The `Download` folder works for the ROM too.)

### 5. Start it

1. In the headset, open your **Library**, set the filter to **Unknown Sources**, and start
   **KH Days VR**.
2. The first time, the app opens a settings page asking for **All files access** (it needs it to
   read the ROM from the KHDaysVR folder). Turn it on for KH Days VR and go back to the app.
3. You wait in a stained-glass loading area that says what it still needs; the game starts by
   itself as soon as it has the access and finds the ROM. No restart needed.

### If something goes wrong

- **"No European ROM found"**: the ROM is missing from KHDaysVR, or it is another region's
  cartridge (the panel names what it found). Only the EU release (`YKGP`) works.
- **It keeps asking for access**: open the headset's **Settings > Apps > KH Days VR >
  Permissions** (or **Special app access > All files access**) and allow it.
- **Anything else**: copy `KHDaysVR/khvr_boot.log` off the headset with SideQuest's file manager
  and send it along with what you saw.

### With adb instead

```sh
adb install -r KH-Days-VR-experimental.apk
adb push your-rom.nds /sdcard/KHDaysVR/
adb shell appops set --uid com.khdays.vr MANAGE_EXTERNAL_STORAGE allow
```

The settings live in `/sdcard/Android/data/com.khdays.vr/files/khvr.ini`.

## Building

The core (settings, view rigs, combat, input, and the demo arena the tests use) builds and tests on
any desktop:

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
