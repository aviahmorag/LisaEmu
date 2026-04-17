/*
 * LisaEm - C to Swift Bridge Header
 * This is the public API that Swift code calls.
 */

#ifndef LISA_BRIDGE_H
#define LISA_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the emulator. Call once at startup. */
void emu_init(void);

/* Destroy the emulator. Call at shutdown. */
void emu_destroy(void);

/* Reset the emulator (like pressing the reset button). */
void emu_reset(void);

/* Load ROM image from file path. Returns true on success. */
bool emu_load_rom(const char *path);

/* True iff a real boot ROM has been loaded (non-zero reset vector). */
bool emu_has_rom(void);

/* Mount a ProFile hard disk image. Returns true on success. */
bool emu_mount_profile(const char *path);

/* Parse hle_addrs.txt produced by the build and wire HLE intercepts.
 * Must be called after emu_load_rom + emu_mount_profile and before
 * emu_reset, or the kernel's syscall/loader intercepts are dead. */
bool emu_load_hle_addrs(const char *path);

/* Load the linker symbol map (linked.map from the build). Needed both
 * for boot-progress instrumentation AND for dynamic HLE lookups used
 * by several kernel intercepts. Must be called before emu_reset. */
bool emu_load_symbol_map(const char *path);

/* Mount a floppy disk image. Returns true on success. */
bool emu_mount_floppy(const char *path);

/* Run one frame (1/60th second) of emulation. Returns cycles executed. */
int emu_run_frame(void);

/* Get pointer to the ARGB framebuffer (720 * 364 * 4 bytes).
 * Valid until next emu_run_frame() call. */
const uint32_t *emu_get_framebuffer(void);

/* Screen dimensions */
int emu_screen_width(void);
int emu_screen_height(void);

/* Keyboard input */
void emu_key_down(int keycode);
void emu_key_up(int keycode);

/* Mouse input */
void emu_mouse_move(int dx, int dy);
void emu_mouse_button(bool pressed);

/* State queries */
bool emu_is_running(void);
void emu_set_running(bool running);

/* True once the kernel has executed SYSTEM_ERROR (a fatal non-returnable
 * fault). Sticky — survives interrupt wake-up; cleared only by emu_init. */
bool emu_is_halted(void);

/* Fill `buf` with a human-readable boot-progress report (milestones
 * reached + per-milestone status). Returns bytes written (incl NUL). */
int emu_get_boot_progress_report(char *buf, int bufsize);

/* Debug: get CPU state as formatted string */
void emu_get_cpu_state(char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* LISA_BRIDGE_H */
