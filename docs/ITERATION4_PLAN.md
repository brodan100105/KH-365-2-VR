# Iteration 4: the world in VR

Iteration 4's first half is done: the DS's 3D engine runs in the port and draws the game's 3D
picture (the Day number, the field, cutscenes) at up to 4x the DS's resolution
(`platform/src/hle_gx.cpp`). The second half takes the same polygons out of the DS's picture
and into VR as real geometry around you.

## The order from here

1. Step 5, Roxas in first person: the game's Keyblade in your hand (done); his arms following
   your controllers (first version built, waiting for CI minutes); a stationary upper body with
   the legs animating under it; chain attacks kept until the game can take them; Keyblade
   effects (summon, trails, damage numbers); the characters half drawn in third person when
   Roxas turns away (the game's own culling, to find).
2. Step 5b, magic: from the Keyblade's tip, its effects facing you, aim to cast.
3. Loose ends of steps 2 and 3: the world's scale from Roxas's height; analog movement and
   room-scale steps through the game's collision; the rest of the game's culling.
4. The chosen ideas, in this order: haptics that tell hits apart; hits felt in the picture; guard
   with the blade across the body; chests by striking or touching; a pointer and a wrist quickbar;
   sound from where it happens; speech bubbles above the speaker.
5. Photo mode: pause, fly around the scene, take pictures with the trigger.
6. Step 6, the clock tower as the loading area.
7. The open ideas, as chosen.
8. A PCVR build (once the Quest version is stable): see the last section.

## Step 1: the world as geometry (capture, textures, a stereo giant screen)

- [x] **Capture in world space.** Every polygon the game submits, before the DS's own culling and
      clipping (a VR viewpoint sees faces the DS camera did not), with its vertices through the
      position matrix. The NitroSystem 3D library puts the camera either into the position matrix
      (`NNS_G3dGlbFlush`) or into the projection matrix (`NNS_G3dGlbFlushP`): removing what the
      projection holds beyond the library's lens (`NNS_G3dGlb.projMtx`) and then the camera
      (`NNS_G3dGlb.cameraMtx`) gives world coordinates either way. Polygons drawn with another
      projection (orthographic HUD pieces such as the Day number) stay in the DS picture.
- [x] **The game camera** from `NNS_G3dGlb`: eye, target, up and the lens's field of view.
- [x] **Textures**: each texture the frame uses, decoded from texture VRAM to RGBA with its wrap,
      flip and colour-0 rules, keyed by its parameters and a hash of its texels and palette so the
      app uploads it to the GPU only when it changes.
- [x] **VR renderer**: textured, vertex-coloured, unlit triangles (the DS already lit them),
      alpha test and blending as the polygon asks, each face drawn as the polygon's front/back
      bits ask. Solid texels of alpha-tested textures (hair, leaves) write depth before the
      translucent pass, so the model's own layers hide each other as on the DS.
- [x] **Screen > Game picture**: *DS picture* (the DS's own pixels), *Sharp* (the default: the
      captured world drawn from the game's camera at headset resolution, flat like the DS
      picture) or *3D depth* (experimental: one camera per eye, so the level has depth behind
      the screen). The DS's 2D behind its 3D layer fills the
      view first; the 2D in front of it, with the orthographic 3D pieces, lies on the screen.
- [x] Desktop test: a textured quad drawn through `NNS_G3dGlb` with a camera, captured back in
      world space with the right camera and texture.

## Step 2: the game's state (hooks)

- [ ] Player position, heading and height, from the active player actor. Found as the field's
      own code finds it (`func_ov022_020881f8`): the battle session's player table
      (`data_ov022_020b2e78`), the player actor's position at +0x48c and its heading, a u16 angle
      at +0x80 of the node at +0x20. `Platform::ReadPlayer` reads them and the boot log's
      heartbeat prints them, to be checked on the headset before the view uses them.
- [ ] Gameplay, cutscene and menu flags (scene ids, event scripts, camera animations).
- [ ] **Game > World units per meter** from Roxas's model height.

## Step 3: standing in the world

- [ ] Panoramic (third person): the captured world around you at the rig's scale, Roxas in front.
      First slice in: **View > Stand in the world (experimental)** reports gameplay while the
      player is in the field and neither the camp menu (ov025, ov008) nor an event script (ov023)
      runs, and the view mode then draws the captured world through the rig. Still to come: the
      scale from Roxas's height, cutscene detection beyond event scripts, and what the game's
      camera does not see (step 3's camera override).
- [x] First person: the view from Roxas's head, his model hidden: the game's own Roxas is left
      out of the captured world unless **Show body** is Full body; **First person > World scale**.
      (His Keyblade and arms in VR: step 5.)
- [x] Smooth motion at the headset's rate: the game runs its logic at 60 frames a second (faster
      would play the game faster), so the captured world is interpolated between two game frames
      for the 90 or 120 Hz display when both have the same polygons (**Picture > In-between
      frames**), and the rigs follow Roxas between game frames (**View > Smooth camera motion**).
      The world is turned back into world space with the camera each projection was drawn with,
      not the one the game had set up for its next frame when it swapped (that turned the world
      whenever the game's camera swung).
- [x] Sprites (munny, orbs, effects) face you: polygons flat across the game camera's view are
      sprites, turned to face the viewer. Flat markers drawn with their own projection are
      placed in the world in front of what they mark.
- [x] The field's cursors: the game's are 2D sprites placed for its own camera (enemies,
      Ov022_DrawSelectionMarkers) or corner marks drawn in front of it (people and things,
      Ov022_UpdateUiSelectionMarker). Ov022_UpdateSelectionMarker reports the target, its kind and
      lock (khvr_host_lock_on) and, standing in the world, leaves the game's own out
      (khvr_host_own_cursors); the headset draws them at the target: yellow arcs turning clockwise
      on an enemy, blue once locked on, green corner marks on people and things.
- [ ] Camera override and wider culling, so the game draws what you look at (it culls to its own
      camera: what lies behind it is not submitted at all). Enemies: their models take the game
      camera's 60-degree view cone (Ov107_IsBehindView) as their level of detail
      (Ov107_AiState_DispatchModelCallbacks); with the setting below they are drawn as in view.
      First part in: **View > Draw what the
      game's camera does not see** answers the game's BOX_TESTs "visible" (the heartbeat counts
      the boxes out of view). The field does not use BOX_TEST (0 in the Day 11 log): its culling
      is in its own code, still to find (the enemy framework hides and shows models through its
      visibility messages, Ov107_AiState_SetVisible; func_ov022_0208ffe8 skips only nodes within
      0.8 of the camera; Ov002_IsPointOnScreen serves the camera).
- [x] HUD pieces the game draws in 3D in front of its camera (the mission's heart counter) stay
      on the HUD: polygons that keep their place in camera space while the camera turns and most
      of the scene moves across the view, away from Roxas and the cursor's target (a camera
      turning round them keeps them in place too), never more than a quarter of the picture.
- [x] Upright sprites (trees) turn to face you about the vertical.
- [ ] Analog movement and room-scale steps through the game's collision.

## Step 4: in-engine cutscenes from where you stand

- [ ] **View > Cutscenes: Same as gameplay** in 6DoF: event scenes (ov023) set their own
      projection (their zoom), not NNS_G3dGlb's lens. Their scripts drive the engine's camera
      objects (Camera_Init on the event block's cameras), so NNS_G3dGlb's camera (+0x4c) is the
      event camera: first slice in, the scene's lens is its most used perspective projection and
      that camera places it in the world (or, folded into the projection, it comes in world space
      already); with Same as gameplay such a scene counts as gameplay for the view rigs, and the
      heartbeat counts the frames placed ("event scene: ... placed in the world"). The first log
      (Roxas waking in his room, ov023) showed the scene drawn with NNS_G3dGlb's own lens, so
      already in world space, but with no player actor: the event was only seen with a player,
      so it went to the giant screen. Menus, events and choices are now read without a player,
      and a cutscene without one has its view where its camera looks as it begins. In the
      headset the scenes stood in the world but the view did not follow the cutscene's camera:
      now a cutscene rig puts your head at the camera's eye, looking its way, moving and turning
      (heading only) with it, smoothed, a cut behind a blink, at View > Cutscene world scale
      (1.75); sprites stay as the game drew them, and no 3D HUD pieces are looked for.

## Step 5: Roxas in VR (first person)

- [ ] The game's own Keyblade in your hand instead of the placeholder: its model from the ROM's
      files (or picked out of the captured player mesh by its bone), drawn at the VR hand.
      First version (to test): NNS_G3dDraw tells the capture which render object each polygon is
      of; Roxas's (the player actor's scene node) and the models attached to his joints
      (SceneNode_AttachToModelJoint: the node's children) are tagged, with their vertices in their
      own model space. First person hides exactly those; the longest attached model is taken as
      the Keyblade, its grip at its origin and its blade to its farthest point, and drawn along
      the controller's blade axis at Keyblade length. The heartbeat names the joints they are
      attached to and how many joints each has (for the arm IK next).
- [ ] Roxas's arms following yours: find his model's skeleton (the player actor's NNS G3D joint
      matrices), solve two-bone IK for the arms from the controllers, and have the game pose him
      with it, so Full body shows your own movements.
- [ ] Full-body VRIK (the goal for first person): his upper body stays under the headset (the
      torso follows your head's position and turns with your body, not the run cycle); his hands
      are at your controllers, the Keyblade in the weapon hand; his elbows and shoulders bend to
      reach them (two-bone IK with the elbow hint from the hand's roll and the shoulder); his
      legs keep the game's animation, used only while he moves (standing still, they stand), a
      blend of animation below the hips and tracking above. Until then, Body without head hides
      him while he moves (his run cycle carried him in front of the view).

### Step 5, next (after the arm-following build is tested)

- [ ] A stationary upper body: his shoulders, torso and hips stay under your headset while he
      moves (a little sway with his steps), never running out from under your head; his legs
      play the game's run and walk, kept to what looks natural under that body.
- [ ] Chain attacks in first person: a swing made while his attack cannot take a new one yet is
      kept and given to the game the moment it can (Ov022_TranslateCommandToAction drops an A
      press while Ov022_IsActionAllowed says no), while the enemy is still there and locked or
      soft-locked, so swinging on plays the chain through.
- [ ] Roxas's hands close into a fist as you press the grip and while the Keyblade is in that
      hand, and open as you let go: a short animation, the fingers of his one-piece hand bent at
      the knuckles (next build).
- [ ] Keyblade effects in first person: the game's own summoning effect (as the game plays it
      when Roxas draws his Keyblade, read from the running game) when it appears in your hand,
      with the grow-in kept; trails on your swings, and damage numbers floating up briefly from
      an enemy you hit (first person in VR only).

## Step 5b: magic from the Keyblade (after full-body IK and first-person Keyblade combat are right)

- [ ] The game's own magic effects (Fire, Blizzard, Thunder, Aero...), unchanged, start at the tip
      of the Keyblade in your hand and travel the way you point it, instead of from Roxas's
      on-screen position.
- [ ] Every effect is seen properly in VR: its sprites and particles face your eyes (not the game
      camera or Roxas), the right way up, at the right size and depth; trails, flashes and
      screen-wide effects look right in the headset as well as on the giant screen.
- [ ] Aim to cast (first person): a spell fires only while the Keyblade points at the target, the
      one soft-locked or locked on, within a set angle (Combat > Magic aim angle); pointing away,
      the cast waits (a gentle buzz and the cursor dimming say why). Without a target it goes
      where the Keyblade points.

## Ideas: quality of life and immersion

Each would be an option in the settings. Chosen by the user (in this order of priority, after
Step 5): haptics that tell hits apart; hits felt in the picture; guard by holding the blade across
the body; chests opened by striking or touching them; a pointer and a wrist quickbar for items
(potions); sound from where it happens; speech bubbles above the speaker. The rest are open.

- [ ] Haptics that tell hits apart (chosen): a light tap for a hit, a heavier one for a finisher or
      critical, a sharp jolt when Roxas is hit, a buzz on a blocked attack; a faint hum while
      the Keyblade is out.
- [ ] Hits felt in the picture (chosen): a few frames of hit-stop on finishers, a red pulse at the edges
      of the view when Roxas takes damage, and an arrow at the edge toward an enemy attacking
      from outside the view.
- [ ] Guard by holding the Keyblade across your body (chosen): attacks and projectiles that meet
      the blade are blocked or deflected (the game's own guard), timed for a parry.
- [ ] Chests opened by striking or touching them (chosen); later hearts, munny and orbs pulled
      in when your hand reaches for them.
- [ ] A wrist quickbar (chosen): turn the off-hand wrist up to see a few quick slots (potions,
      ethers, the deck's shortcuts) and pick one with the pointer or a touch of the other hand;
      a pointer option for menus and targets. Later: a potion by bringing the off hand to your
      mouth; the mission gauge and map on the wrist.
- [ ] Lock-on by looking: the enemy you look at (or point the Keyblade at) for a moment
      becomes the target; flick the stick to change it.
- [ ] Sound from where it happens (chosen): the game's sound effects placed at their source around you
      (the enemy that growls, the chest that opens), not from the middle of your head.
- [ ] People talk where they stand (chosen): the speech text in a bubble above the speaker
      instead of on the HUD, turning to face you.
- [ ] Limit breaks and specials by pose: raise the Keyblade overhead and hold to charge.
- [ ] Physical movement in the game's abilities: lean into a dodge roll; a real jump for the
      High Jump; arms out to glide.
- [ ] The Grey Area and the castle at full scale: the people around you life-sized, a seat on
      the couches, the mission board as a panel you touch.
- [x] Chosen: a photo mode (see "Photo mode" below).

## Photo mode (chosen)

- [ ] A dedicated mode, turned on and off in the settings panel (its own switch, not a view
      mode). While it is on, everything in the scene pauses: the game, its animations and
      effects, its sound.
- [ ] You move apart from Roxas, flying freely in any direction (up and down too) at a slow to
      medium speed with the sticks, through the paused scene; turning as in the comfort settings.
      Turning it off puts you back where you were with Roxas.
- [ ] The trigger takes a photo: what you see (one eye's view, at the headset's resolution) saved
      as a picture in a new folder of its own, KHDaysVR/Screenshots, named by date and time;
      a shutter flash and sound to show it was taken.

## Step 6: the loading area on the clock tower

- [ ] The first time you reach the top of the Twilight Town clock tower, its captured geometry and
      textures are saved to the KHDaysVR folder; later boots stand you there instead of in the
      Heart's Station (which stays the fallback). Nothing from the game goes into the APK.

## Last: a PCVR build (once the Quest version is stable)

The game (the decomp through wasm2c: portable C), the DS hardware stand-ins (`platform/`) and the
VR core (`core/`: settings, rigs, combat, input, panel) already build and run on a desktop for the
tests. Only the Quest app layer (`android/app/src/main/cpp`, about 3,500 lines) is Android's:

- [ ] OpenXR on Windows (SteamVR, Meta Link / Air Link, WMR): the same OpenXR code with a desktop
      window and loop instead of the Android activity, and desktop OpenGL instead of OpenGL ES
      (XR_KHR_opengl_enable), or GLES kept through ANGLE.
- [ ] The renderer's shaders and few ES-only calls for desktop OpenGL.
- [ ] Audio through a small cross-platform library instead of AAudio.
- [ ] The ROM and the saves: a folder beside the program or a file picker instead of the Quest's
      folders. Still nothing from the game in the build.
- [ ] Quest-only features (passthrough, refresh rates, foveation) left out or given PC
      counterparts; controller binding profiles for Index and Vive controllers too.
- [ ] Built for Windows from the Linux CI (cross-compiling) or on your own PC: Windows CI minutes
      cost twice the Linux ones.
