/*
 * LisaEm - Apple Lisa Emulator
 * Memory Management Unit and Memory Controller
 *
 * Lisa memory map (from source analysis):
 *   $000000-$0FFFFF  RAM (1MB max)
 *   $FE0000-$FEFFFF  ROM (Boot ROM, 16KB)
 *   $FC0000-$FCFFFF  I/O Space
 *   Video RAM is within main RAM, address set by video latch
 */

#ifndef LISA_MMU_H
#define LISA_MMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Lisa hardware constants */
#define LISA_RAM_SIZE       (1024 * 1024)     /* 1 MB RAM */
#define LISA_ROM_SIZE       (16 * 1024)       /* 16 KB Boot ROM */
#define LISA_IO_BASE        0xFC0000
#define LISA_IO_SIZE        0x010000
#define LISA_ROM_BASE       0xFE0000
#define LISA_SCREEN_WIDTH   720
#define LISA_SCREEN_HEIGHT  364
#define LISA_SCREEN_BYTES   (LISA_SCREEN_WIDTH / 8 * LISA_SCREEN_HEIGHT)  /* 32,760 bytes */

/* I/O register offsets from LISA_IO_BASE */
#define IO_DISK_SHARED      0x0000   /* Disk controller shared memory */
#define IO_VIA1_BASE        0xD801   /* VIA1 - Parallel port / HD */
#define IO_VIA2_BASE        0xDD81   /* VIA2 - Keyboard / COPS */
#define IO_CONTRAST         0xD01C   /* Contrast latch */
#define IO_VIDEO_LATCH      0xE800   /* Video page latch */
#define IO_SETUP_SET        0xE010   /* Setup bit set */
#define IO_SETUP_RESET      0xE012   /* Setup bit reset */
#define IO_VRETRACE         0xE018   /* Vertical retrace interrupt */
#define IO_STATUS_REG       0xF800   /* Hardware status register */

/* MMU segment descriptor */
typedef struct {
    uint16_t slr;    /* Segment Limit Register */
    uint16_t sor;    /* Segment Origin Register */
    bool valid;
} mmu_segment_t;

#define MMU_NUM_SEGMENTS  128
#define MMU_NUM_CONTEXTS  4

/* Memory controller state */
typedef struct {
    uint8_t  ram[LISA_RAM_SIZE];
    uint8_t  rom[LISA_ROM_SIZE];

    /* MMU */
    mmu_segment_t segments[MMU_NUM_CONTEXTS][MMU_NUM_SEGMENTS];
    int current_context;
    bool mmu_enabled;
    bool setup_mode;        /* When true, ROM is mapped to low memory */

    /* Video */
    uint32_t video_addr;    /* Base address of video RAM */
    bool     video_alt;     /* Using alternate video page */
    uint8_t  contrast;      /* Display contrast 0-255 */

    /* Vertical retrace */
    bool     vretrace_irq;
    bool     vretrace_enabled;
    int      vretrace_counter;

    /* Status register */
    uint8_t  status_reg;

    /* I/O read/write callbacks */
    uint8_t  (*io_read)(uint32_t addr);
    void     (*io_write)(uint32_t addr, uint8_t val);
} lisa_mem_t;

/* Public API */
void lisa_mem_init(lisa_mem_t *mem);
void lisa_mem_load_rom(lisa_mem_t *mem, const uint8_t *data, size_t size);

uint8_t  lisa_mem_read8(lisa_mem_t *mem, uint32_t addr);
uint16_t lisa_mem_read16(lisa_mem_t *mem, uint32_t addr);
uint32_t lisa_mem_read32(lisa_mem_t *mem, uint32_t addr);

void lisa_mem_write8(lisa_mem_t *mem, uint32_t addr, uint8_t val);
void lisa_mem_write16(lisa_mem_t *mem, uint32_t addr, uint16_t val);
void lisa_mem_write32(lisa_mem_t *mem, uint32_t addr, uint32_t val);

/* Get pointer to video RAM for display rendering */
const uint8_t *lisa_mem_get_video(lisa_mem_t *mem);

#endif /* LISA_MMU_H */
