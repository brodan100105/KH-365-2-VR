# Iteration 4: the world in VR

Iteration 4's first half is done: the DS's 3D engine runs in the port and draws the game's 3D
picture (the Day number, the field, cutscenes) at up to 4x the DS's resolution
(`platform/src/hle_gx.cpp`). The second half takes the same polygons out of the DS's picture
and into VR as real geometry around you.

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
- [x] The lock-on sight: the game's is 2D sprites placed for its own camera
      (Ov022_UpdateSelectionMarker); Ov022_ProjectToScreen reports the target's world position
      (khvr_host_lock_on) and the headset draws a sight of its own around it, over everything.
- [ ] Camera override and wider culling, so the game draws what you look at (it culls to its own
      camera now: what lies behind it is not submitted at all).
- [ ] Analog movement and room-scale steps through the game's collision.

## Step 4: in-engine cutscenes from where you stand

- [ ] **View > Cutscenes: Same as gameplay** in 6DoF: event scenes (ov023) set their own
      projection, so they are captured now only in camera space (sharp on the giant screen). Find
      the event camera (where the event script keeps its eye and target) to place the scene in the
      world, then show it around you like gameplay, with the giant screen as the fallback.

## Step 5: Roxas in VR (first person)

- [ ] The game's own Keyblade in your hand instead of the placeholder: its model from the ROM's
      files (or picked out of the captured player mesh by its bone), drawn at the VR hand.
- [ ] Roxas's arms following yours: find his model's skeleton (the player actor's NNS G3D joint
      matrices), solve two-bone IK for the arms from the controllers, and have the game pose him
      with it, so Full body shows your own movements.

## Step 6: the loading area on the clock tower

- [ ] The first time you reach the top of the Twilight Town clock tower, its captured geometry and
      textures are saved to the KHDaysVR folder; later boots stand you there instead of in the
      Heart's Station (which stays the fallback). Nothing from the game goes into the APK.
