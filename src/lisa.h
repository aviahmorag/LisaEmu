/*
 * LisaEm - Apple Lisa Emulator
 * Main machine state and integration
 */

#ifndef LISA_H
#define LISA_H

#include "m68k.h"
#include "lisa_mmu.h"
#include "via6522.h"
#include "profile.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Lisa CPU clock: ~5 MHz (actually 7.8336 / 2 = 3.9168 MHz for early Lisa,
   5 MHz for Lisa 2) */
#define LISA_CPU_CLOCK      5000000
#define LISA_FRAME_RATE     60
#define LISA_CYCLES_PER_FRAME (LISA_CPU_CLOCK / LISA_FRAME_RATE)

/* COPS keyboard/mouse controller */
#define COPS_QUEUE_SIZE  64

typedef struct {
    uint8_t queue[COPS_QUEUE_SIZE];
    int     head, tail, count;
} cops_queue_t;

/* Old ProFile constants (now in profile.h, kept for profile_disk_t) */
#ifndef PROFILE_BLOCK_SIZE
#define PROFILE_BLOCK_SIZE   532
#endif
#define PROFILE_MAX_BLOCKS   19456  /* ~10MB ProFile */
/* Keep in sync with diskimage.h — bumped 24 -> 64 in phase2 step3c
 * to carry SYSTEM.BT_PROFILE's linked blob on the boot track. The
 * is_real_image heuristic in lisa.c depends on this matching the
 * disk's fs_block0 for cross-compiled images. */
#define BOOT_TRACK_BLOCKS    64

typedef struct {
    uint8_t *data;          /* Disk image data */
    size_t   data_size;
    bool     mounted;

    /* ProFile state machine */
    int      state;
    uint8_t  command[6];
    int      cmd_index;
    uint32_t block_num;
    uint8_t  sector_buf[PROFILE_BLOCK_SIZE];
    int      buf_index;
    bool     busy;
} profile_disk_t;

/* Floppy disk (Sony 3.5") */
#define FLOPPY_MAX_SIZE  (800 * 1024)  /* 800KB */

typedef struct {
    uint8_t *data;
    size_t   data_size;
    bool     mounted;
    bool     write_protect;
    int      track;
    int      sector;
} floppy_disk_t;

/* Main Lisa machine state */
typedef struct {
    m68k_t       cpu;
    lisa_mem_t   mem;
    via6522_t    via1;    /* Parallel / ProFile */
    via6522_t    via2;    /* Keyboard / COPS */

    /* COPS (keyboard/mouse controller) */
    cops_queue_t cops_rx;  /* Data from COPS to CPU */
    int          cops_crdy_count; /* Port B reads since last command (for CRDY toggle) */
    int          mouse_x, mouse_y;
    bool         mouse_button;

    /* Keyboard state */
    bool         keys_down[128];

    /* Storage */
    profile_disk_t profile;     /* Old simplified ProFile (TODO: remove) */
    profile_t      prof;        /* New protocol-accurate ProFile */
    floppy_disk_t  floppy;

    /* IRQ management */
    int          irq_via1;
    int          irq_via2;
    int          irq_vretrace;

    /* Display state - pixel buffer for rendering */
    uint32_t     framebuffer[LISA_SCREEN_WIDTH * LISA_SCREEN_HEIGHT];
    bool         display_dirty;

    /* Timing */
    int          frame_cycles;  /* Cycles accumulated in current frame */
    uint64_t     total_frames;

    /* Machine state */
    bool         running;
    bool         power_on;
    /* Sticky "permanent halt" flag — set when the kernel's SYSTEM_ERROR
     * handler fires (a fatal non-returnable kernel fault). Survives the
     * CPU stopping/waking via interrupts, unlike cpu->stopped. Cleared
     * only on lisa_init. Swift watches this to stop the frame timer. */
    bool         halted;

    /* HLE (High-Level Emulation) disk I/O — bypasses ProFile driver */
    struct {
        uint32_t calldriver;       /* CALLDRIVER entry in DRIVERASM */
        uint32_t call_hdisk;       /* CALL_HDISK entry in DRIVERASM */
        uint32_t hdiskio;          /* HDISKIO function in HDISK unit */
        uint32_t prodriver;        /* PRODRIVER in PROFILE unit */
        uint32_t system_error;     /* SYSTEM_ERROR — intercept boot failures */
        uint32_t badcall;          /* BADCALL — default driver entry */
        uint32_t parallel;         /* PARALLEL — ProFile interrupt handler */
        uint32_t use_hdisk;        /* USE_HDISK — sets up HDISK entry point */
        bool     active;           /* HLE intercepts enabled */
        bool     boot_config_done; /* Have we injected boot device config? */
        int      reads;            /* Stats: blocks read */
        int      writes;           /* Stats: blocks written */
        /* P115: post-driver SP fixup — kernel CALLDRIVER expects callee-
         * clean for driver's 4-byte parameters push, but our Pascal
         * codegen is caller-clean. Set when dinit passthrough fires;
         * the main loop adjusts SP += 4 on first PC return to kernel
         * (after PRODRIVER's RTS) so kernel's MOVE.W (SP)+,D0 reads
         * the right slot. Cleared after fix-up. */
        bool     p115_sp_fixup_pending;
        uint32_t p115_post_jsr_pc; /* kernel PC right after JSR (A0) */
    } hle;
} lisa_t;

/* Public API */
void lisa_init(lisa_t *lisa);
void lisa_destroy(lisa_t *lisa);
void lisa_reset(lisa_t *lisa);

/* Load ROM image. Returns true on success. */
bool lisa_load_rom(lisa_t *lisa, const char *path);

/* Mount disk images */
bool lisa_mount_profile(lisa_t *lisa, const char *path);
bool lisa_mount_floppy(lisa_t *lisa, const char *path);

/* Run one frame (~1/60th second). Returns cycles executed. */
int lisa_run_frame(lisa_t *lisa);

/* Input events */
void lisa_key_down(lisa_t *lisa, int keycode);
void lisa_key_up(lisa_t *lisa, int keycode);
void lisa_mouse_move(lisa_t *lisa, int dx, int dy);
void lisa_mouse_button(lisa_t *lisa, bool pressed);

/* Get framebuffer for display (720x364 ARGB pixels) */
const uint32_t *lisa_get_framebuffer(lisa_t *lisa);
int lisa_get_screen_width(void);
int lisa_get_screen_height(void);

/* HLE disk I/O — called from CPU execution loop when PC matches a known function.
 * Returns true if the intercept handled the call (CPU should skip execution).
 * Returns false if normal execution should continue. */
bool lisa_hle_intercept(lisa_t *lisa, m68k_t *cpu);

/* Set HLE addresses from linker symbol table */
void lisa_hle_set_addresses(lisa_t *lisa, uint32_t calldriver, uint32_t call_hdisk,
                            uint32_t hdiskio, uint32_t prodriver,
                            uint32_t system_error, uint32_t badcall,
                            uint32_t parallel, uint32_t use_hdisk);

/* Step 4a HLE: decode the `fake_parms` pointed at by the top-of-stack
 * args of an ENTER_LOADER call and service the requested loader
 * operation (call_open/fill/byte/word/long/move) natively against
 * ldr_fs. Writes error=0 + result/value fields back into the VAR
 * record, pops retaddr+8 bytes of args, and sets cpu->pc to retaddr.
 *
 * Returns true if handled (caller should `continue` the CPU loop).
 * This is a scaffolding HLE pending the BT_PROFILE relink work that
 * will put the real compiled LOADER at a non-conflicting RAM
 * address; it encodes real LD_OPENINPUT/LD_FILLBUF/etc. semantics
 * so LOADEM and downstream code see the same contract the compiled
 * Pascal would eventually honor. */
bool lisa_hle_enter_loader(m68k_t *cpu);

#endif /* LISA_H */
