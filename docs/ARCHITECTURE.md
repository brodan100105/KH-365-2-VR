# Architecture

The goal is a native Quest 3 port of *Kingdom Hearts 358/2 Days*, built from the
[khdays-decomp](https://github.com/Yokimitsuro/khdays-decomp) matching decompilation (the
`decomp/` submodule), the way [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) ports Super
Mario Galaxy from the Petari decompilation. There is no emulator: the game's own C code runs on
the headset, on a platform layer that stands in for the DS hardware, and the VR layer draws
the game's 3D scene for each eye from wherever your head is.

```
 ┌───────────────────────────── APK (android/) ─────────────────────────────┐
 │                                                                          │
 │  main.cpp ── XrPlatform (OpenXR, swapchains, controllers, Meta exts)     │
 │      │                                                                   │
 │      └── VrApp ── Settings / SettingsPanel        (core/, unit tested)   │
 │             │     ThirdPersonRig / FirstPersonRig                        │
 │             │     CombatRecognizer / InputMapper                         │
 │             │     Renderer (GLES 3)                                      │
 │             │                                                            │
 │             └── GameSource ─┬─ DemoGame        (today: stand-in arena)    │
 │                             └─ DecompGame      (next: the real game)     │
 │                                   │                                      │
 │                                   ├─ guest: decomp/src compiled 32-bit   │
 │                                   └─ platform: DS hardware stand-ins     │
 └──────────────────────────────────────────────────────────────────────────┘
```

## Why a "guest" module, and why 32-bit

The decomp is written to reproduce the original ARM9 machine code byte for byte, so it assumes
the DS's ILP32 data model everywhere: structures hold pointers in 4-byte fields that other code
reads at fixed offsets (`*(int *)(p + 0x14)`), handles are passed around as `int`, function
tables are declared as data. Compiled as ordinary 64-bit Android code, every structure with a
pointer in it changes size and all of that offset arithmetic breaks.

Quest apps must be 64-bit, so the game code is compiled for **wasm32** (a 32-bit target clang
supports out of the box) and then translated to native ARM64 C with `wasm2c` and linked into the
APK. The result runs at native speed, keeps 32-bit pointers and `int`s exactly as on the DS, and
gives the game one flat 4 GB address space in which the platform layer can put the DS's memory
map where the code expects it (main RAM at `0x02000000`, the ARM7-shared words at
`0x027fff..`, I/O registers at `0x04000000`).

`tools/guest/probe_decomp.sh` measures how close the decomp is. At decomp commit `261a89ea`:

| | |
|---|---|
| Game C files (`src/`) that compile for wasm32 | **23,314 / 23,325** |
| Library C files (`libs/`: NitroSDK, NitroSystem, MSL, MobiClip) | 1,165 / 1,285 |
| Link: uninitialized globals defined in several files | 57 symbols |
| Link: a function declared as data in another file | 115 symbols |

The 11 game files that do not compile are inline ARM assembly (`MsgQueue_*`,
`PartyState_ReleaseNodes`, two sort routines), one array assignment and the DS Protect
anti-tamper overlay (ov028), which the port does not run. The link problems are mechanical and
will be handled by a source pre-pass in the guest build (`extern` for the duplicate
definitions, function declarations for the data views), the same kind of `TARGET_PC` patch set
GalaxyQuest keeps against Petari.

### What it took (iteration 2)

`guest/build_guest.py` builds the guest; the notes in its docstring and in `guest/*.txt` say why
each step exists. The ones that matter for anyone touching it:

* **mwcc's ABI.** mwcc aligns 64-bit types to 4 bytes; clang's wasm32 to 8. The guest is compiled
  with `-fpack-struct=4`, or structures such as `OSContext` grow and code reading their fields by
  offset misses them (the scheduler hung on exactly that).
* **Original data layout.** The `.bss` of every module is generated from the decomp's config as one
  block per module, symbols at their original offsets, because code reaches across symbols. The
  first 128 KB of guest data are DTCM (0x027e0000) and the ARM7-shared page (0x027ff000) at their
  real addresses, since the SDK uses literal addresses there.
* **Variables the original only kept the code of.** Some sources define globals the ROM's link
  discarded (the real variable is a field of another symbol's block). Labels at the original
  addresses replace them: derived automatically from the decomp's `khdays: shared-bss` blocks, plus
  `guest/aliases.txt`.
* **Calls with mismatched declarations** (1,600 call sites) become adapters that behave like the ARM
  calling convention; indirect calls skip wasm's signature check for the same reason.
* **Hardware access** is two traps in wasm2c's memory accessors: 0x04xxxxxx (I/O) and 0x06xxxxxx
  (VRAM) go to the platform, everything else is plain memory.

## The platform layer (DS hardware stand-ins)

What the game touches, and what replaces it:

| DS hardware / SDK | Port |
|---|---|
| Main RAM, DTCM, ITCM, shared work RAM | Regions of the guest's linear memory at their DS addresses |
| ROM file system (`FS_*`, `CARD_*`) | Reads the user's own ROM image from `/sdcard/Android/data/com.khdays.vr/files/game/` |
| Overlays (`FS_LoadOverlay`) | All overlays are linked in; loading one only runs its static initializers. Overlays that share a load address get their own copies (each player/enemy overlay is already its own directory) |
| 3D engine (GX geometry FIFO, `G3_*`, NNS G3D) | The I/O stores are trapped and decoded into a geometry stream: matrices, vertices, textures, polygon attributes. Instead of rasterizing at 256x192, the stream becomes a `WorldDrawList` in world space (below) |
| 2D engines (BG, OAM, VRAM banks) | A software 2D compositor produces the HUD layers and the touch screen as textures |
| Pad, touch screen, lid | `Pad_Sample` reads `0x04000130` / `0x027fffa8`: the platform writes the VR layer's pad bits there; touch comes from the laser pointer |
| Sound (NNS SND, SDAT) | A software mixer for the SDAT sequences and wave archives (later iteration) |
| MobiClip video (ov012/ov024) | Decoded in software by the decomp's own MobiClip code, shown on the giant screen |
| Interrupts, VBlank, timers, ARM7 | Simulated: the frame loop in `main()` waits for a VBlank the platform raises at the game's frame rate |
| DS Protect (ov028) | Not run |

## Drawing the game for each eye

The DS multiplies every vertex by the current position matrix (game camera view x model) before
projecting it. The platform captures the view matrix the game commits each frame
(`Camera_CommitMatrices` builds it with `MTX_LookAt` into `data_020473e0`), so every captured
vertex can be taken back to world space:

```
world = inverse(gameView) * (gameView * model) * vertex
```

The VR layer then draws that world from each eye through the rig's mapping from tracking space
into the world (`core/include/khvr/view_rig.h`):

```
eyeViewProj = projection(eye) * inverse(eyePose) * inverse(trackingToWorld)
```

* **Panoramic (third person)**: `trackingToWorld` puts the character a few meters in front of
  you and below your floor, at any scale from life size to a tabletop diorama. The view follows
  the character with a dead zone (or locked, or teleporting), and snap turns rotate the world
  around the character.
* **First person**: `trackingToWorld` puts your head on the character's head. Physical walking is
  sent to the game as a movement step through its own collision (`Actor_ResolveMove`), so walls
  stop you; while the game has not applied a step yet the view shows it as applied, and if a
  wall rejected it you are pushed back.
* **Giant screen**: the game's own camera, re-rendered at headset resolution on a 4:3 screen
  (stereoscopic if you want), for menus, cutscenes, or the whole game.

In both world modes the game camera is overridden after the game computes it, with the
camera you actually look through. That keeps the game's frustum culling, 3D sound listener and
camera-relative logic consistent with the headset (**Picture > Draw distance** widens the culling
frustum so objects do not vanish at the edges of your view).

## Game hooks

The port needs a handful of hooks into the game, each a small `#ifdef TARGET_KHVR` change in the
decomp source or a platform-side wrapper. Names are the decomp's:

| Hook | Where | Purpose |
|---|---|---|
| Game camera read / override | `Camera_CommitMatrices`, `Ov002_Camera_UpdateFollow` | `GameFrame::camera`; `GameRequests::camera` |
| Player position, heading, grounded | player overlay actor (`Ov030_StepActor` .. per character), `Actor_ResolveGround` | `GameFrame::playerPos/playerYaw` |
| Analog movement | `Ov030_UpdateController` (the per-frame movement vectors it builds) | `GameRequests::analogMoveWorld` |
| Room-scale step through collision | `Actor_ResolveMove` | `GameRequests::roomScaleStep` |
| Player heading in first person | the character node's heading (`angle80` in the controller's view of the node) | `GameRequests::playerYaw` |
| Pad | `Pad_Sample` | `GameRequests::buttons` |
| Scene / cutscene / menu state | `data_027e0060` (scene id), `CamAnim_IsActive`, `ScriptVm_UpdateActiveCamera`, `gPauseMode` | `GameFrame::inGameplay/inCutscene/inMenu` |
| Nearest enemy, lock-on target | the enemy framework's actor list (ov107) | aim assist, physical hits |
| Culling frustum | `Camera_BuildProjectionMtx` inputs | `GameRequests::cullingScale` |

## Repository layout

```
core/                 platform-independent VR logic (C++17) and its unit tests
  include/khvr/       math, settings, settings panel, view rigs, combat, input, game interface
  src/                implementations; demo_game.cpp is the stand-in arena
  tests/              core_tests.cpp (ctest)
  tools/              settings_doc.cpp (generates docs/SETTINGS.md)
android/              the Quest app: Gradle project, manifest, native code in app/src/main/cpp
decomp/               submodule: Yokimitsuro/khdays-decomp (C sources only, no game data)
guest/                build_guest.py, postprocess.py, the guest runtime (C for the SDK's assembly)
platform/             the DS hardware stand-ins, the 2D renderer, the desktop runner (tools/)
tools/guest/          probe_decomp.sh (the iteration 1 measurement)
tools/symbolize.py    resolves backtraces in a boot log
tools/gen_font.py     regenerates the UI font
docs/                 this file, SETTINGS.md, CONTROLS.md, ROADMAP.md
```
