/* Functions the guest calls on the platform layer (host). wasm-ld turns every function the guest
 * does not define into an import from module "env"; these are the ones the platform implements on
 * purpose (platform/src/hle_*.cpp). Everything else left undefined gets a logging stub. */
#ifndef KHVR_HOST_H
#define KHVR_HOST_H

/* Threads: the host runs each OS thread on its own host thread and passes a baton between them,
 * so only one guest thread ever runs at a time, as on the DS's single ARM9. */
int khvr_host_save_context(void *context);   /* 0 now; the thread later resumes inside load */
void khvr_host_load_context(void *context);  /* switch to the thread that owns `context` */
/* Wait for the next interrupt (VBlank, timers): the idle thread's OS_Halt. */
void khvr_host_halt(void);
/* Interrupts were just re-enabled: deliver any that are pending. */
void khvr_host_irq_window(void);
/* One turn of a busy-wait loop (guest/build_guest.py adds it to the SDK's): pending interrupts
 * are delivered if the guest has them enabled, as the hardware would mid-loop. */
void khvr_host_spin(void);
void khvr_host_reset(void);
/* The game stopped on a fatal error (OS_Terminate). */
void khvr_host_terminate(void);
/* ROM and overlays (guest/runtime/card_fs.c). */
void khvr_host_read_rom(unsigned long offset, void *dst, unsigned long len);
void khvr_host_start_overlay(unsigned long id);
/* Debug output (OS_Printf and the port's own messages). */
void khvr_host_print(const char *text);
/* A traced event for the boot log (guest/runtime/trace.c): `b` is shown by function name when it
 * is a guest function. */
void khvr_host_trace(const char *what, unsigned long a, unsigned long b);
/* Watch `size` bytes at `addr`: the boot log names the first stores into them (platform/src/hle_io.cpp). */
void khvr_host_watch(const void *addr, unsigned long size);
/* Movie pictures (platform/src/movie.cpp): the stream opened by its NitroFS path, the frame due
 * at each vertical blank, and its end. */
void khvr_host_movie_open(const char *path);
void khvr_host_movie_frame(unsigned long frame);
void khvr_host_movie_close(void);
/* The field's cursor is on a target at this world position (VecFx32) this frame: `kind` 1 or 3 an
 * enemy, 2 one of the field's entries (a person to talk to, a thing to use); `locked` when locked on. */
void khvr_host_lock_on(const void *position, long kind, long locked);
/* Nonzero when the headset draws the field's cursors itself (standing in the world). */
int khvr_host_own_cursors(void);
/* Nonzero when what the game's camera does not see is drawn too (Platform::SetDrawAll). */
int khvr_host_draw_all(void);
/* Analog movement: the heading the player actor's held D-pad stands for (0..0xffff, the D-pad's
 * own convention), or -1 to use the D-pad's eight (Ov022_HeadingFromHeldDpad). */
long khvr_host_move_heading(const void *actor);
/* A model's joint `node` is the current position matrix (NNSi_G3dFuncSbc_NODEDESC), about to be
 * stored: the port may turn it (Roxas's arms following yours). */
void khvr_host_joint(const void *renderObj, unsigned long node);
/* How much further (fx32) the attacker's swings reach: Roxas's, in first person; 0 otherwise. */
long khvr_host_reach_bonus(const void *actor);
/* The player's room-scale step (fx32, x and z) into xz for this frame, taken once; 0 0 for any
 * other actor (Ov022_ApplyExternalPush adds it to his step, which meets the walls). */
void khvr_host_room_step(const void *actor, long *xz);
/* An enemy takes `damage` from a hit landing at `point` (fx32 x, y, z, world space): for the
 * damage numbers floating up in first person (Ov107_CalcHitDamage, guest/overrides). */
void khvr_host_enemy_hit(const void *actor, long damage, const long *point);
/* A scene loads character `slot`'s weapon, row `row` of its weapon table (Ov002_LoadCharacterWeapon):
 * which Keyblade Roxas (slot 0) has equipped. */
void khvr_host_weapon_row(long slot, long row);
/* NNS_G3dDraw draws this render object (NNSG3dRenderObj) from begin to end. */
void khvr_host_model_begin(const void *obj);
void khvr_host_model_end(void);

#endif
