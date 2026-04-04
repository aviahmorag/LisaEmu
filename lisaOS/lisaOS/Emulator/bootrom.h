/*
 * LisaEm Toolchain — Boot ROM Synthesizer
 *
 * Generates a minimal 16KB boot ROM that:
 *   1. Sets up the 68000 stack pointer and exception vectors
 *   2. Initializes the Lisa MMU for setup mode
 *   3. Initializes the display (720x364 monochrome)
 *   4. Reads the boot track from ProFile disk
 *   5. Jumps to the loaded boot code
 *
 * This replaces the need for a ROM dump — it's synthesized from
 * the hardware specifications in the Lisa source code.
 */

#ifndef BOOTROM_H
#define BOOTROM_H

#include <stdint.h>
#include <stdbool.h>

#define ROM_SIZE    (16 * 1024)     /* 16KB */
#define ROM_BASE    0x00FE0000      /* ROM base address */

/* Generate a boot ROM image.
 * Returns a malloc'd buffer of ROM_SIZE bytes, or NULL on failure. */
uint8_t *bootrom_generate(void);

#endif /* BOOTROM_H */
