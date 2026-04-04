/*
 * LisaEm - Apple Lisa Emulator
 * Main machine state and integration
 */

#ifndef LISA_H
#define LISA_H

#include "m68k.h"
#include "lisa_mmu.h"
#include "via6522.h"
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

/* ProFile hard disk */
#define PROFILE_BLOCK_SIZE   532  /* 512 data + 20 tag bytes */
#define PROFILE_MAX_BLOCKS   19456  /* ~10MB ProFile */

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
    int          mouse_x, mouse_y;
    bool         mouse_button;

    /* Keyboard state */
    bool         keys_down[128];

    /* Storage */
    profile_disk_t profile;
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

#endif /* LISA_H */
