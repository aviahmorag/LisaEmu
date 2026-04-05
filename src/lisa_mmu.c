/*
 * LisaEm - Apple Lisa Emulator
 * Memory Management Unit and Memory Controller
 */

#include "lisa_mmu.h"
#include <string.h>
#include <stdio.h>

void lisa_mem_init(lisa_mem_t *mem) {
    memset(mem, 0, sizeof(lisa_mem_t));

    /* Initial state: setup mode, ROM visible at low addresses */
    mem->setup_mode = true;
    mem->mmu_enabled = false;
    mem->current_context = 0;

    /* Default video address (primary screen) */
    mem->video_addr = 0x7A000;  /* Common Lisa video base */
    mem->contrast = 0xFF;
    mem->vretrace_enabled = true;
}

void lisa_mem_load_rom(lisa_mem_t *mem, const uint8_t *data, size_t size) {
    if (size > LISA_ROM_SIZE) size = LISA_ROM_SIZE;
    memcpy(mem->rom, data, size);
}

/* Translate address through MMU if enabled */
static uint32_t mmu_translate(lisa_mem_t *mem, uint32_t addr) {
    if (!mem->mmu_enabled)
        return addr;

    /* Segment number from high bits of address */
    int seg = (addr >> 17) & 0x7F;
    mmu_segment_t *s = &mem->segments[mem->current_context][seg];

    if (!s->valid) {
        /* Segment fault - for now, pass through */
        return addr;
    }

    /* Apply segment origin */
    uint32_t offset = addr & 0x1FFFF; /* 128KB segment size */
    uint32_t phys = ((uint32_t)s->sor << 9) + offset;
    return phys & 0xFFFFFF;
}

/* Determine which region an address falls in */
static inline int addr_region(uint32_t addr) {
    if (addr >= LISA_ROM_BASE) return 2;      /* ROM */
    if (addr >= LISA_IO_BASE)  return 1;      /* I/O */
    return 0;                                  /* RAM */
}

uint8_t lisa_mem_read8(lisa_mem_t *mem, uint32_t addr) {
    addr &= 0xFFFFFF;

    /* In setup mode, ROM appears at address 0 */
    if (mem->setup_mode && addr < LISA_ROM_SIZE) {
        return mem->rom[addr];
    }

    switch (addr_region(addr)) {
        case 0: { /* RAM */
            uint32_t phys = mmu_translate(mem, addr);
            if (phys < LISA_RAM_SIZE)
                return mem->ram[phys];
            return 0xFF;
        }
        case 1: /* I/O */
            if (mem->io_read)
                return mem->io_read(addr - LISA_IO_BASE);
            return 0xFF;
        case 2: /* ROM */
            return mem->rom[(addr - LISA_ROM_BASE) % LISA_ROM_SIZE];
    }
    return 0xFF;
}

uint16_t lisa_mem_read16(lisa_mem_t *mem, uint32_t addr) {
    return ((uint16_t)lisa_mem_read8(mem, addr) << 8) |
           lisa_mem_read8(mem, addr + 1);
}

uint32_t lisa_mem_read32(lisa_mem_t *mem, uint32_t addr) {
    return ((uint32_t)lisa_mem_read16(mem, addr) << 16) |
           lisa_mem_read16(mem, addr + 2);
}

void lisa_mem_write8(lisa_mem_t *mem, uint32_t addr, uint8_t val) {
    addr &= 0xFFFFFF;

    switch (addr_region(addr)) {
        case 0: { /* RAM */
            uint32_t phys = mmu_translate(mem, addr);
            /* Watchpoint: detect writes to code at $4EE (BRA displacement) */
            if (phys == 0x4EE || phys == 0x4EF) {
                static int wp_count = 0;
                if (wp_count < 3) {
                    fprintf(stderr, ">>> WATCHPOINT: write $%02X to phys $%X (addr=$%X)\n",
                            val, phys, addr);
                    wp_count++;
                }
            }
            if (phys < LISA_RAM_SIZE)
                mem->ram[phys] = val;
            break;
        }
        case 1: { /* I/O */
            uint32_t offset = addr - LISA_IO_BASE;

            /* Handle some I/O registers directly */
            switch (offset) {
                case IO_CONTRAST:
                    mem->contrast = val;
                    break;

                case IO_VIDEO_LATCH:
                    mem->video_alt = (val & 1) != 0;
                    if (mem->video_alt)
                        mem->video_addr = 0x7A000 + LISA_SCREEN_BYTES;
                    else
                        mem->video_addr = 0x7A000;
                    break;

                case IO_SETUP_SET:
                    mem->setup_mode = true;
                    break;

                case IO_SETUP_RESET:
                    mem->setup_mode = false;
                    break;

                case IO_VRETRACE:
                    mem->vretrace_irq = false;
                    break;

                default:
                    if (mem->io_write)
                        mem->io_write(offset, val);
                    break;
            }
            break;
        }
        case 2: /* ROM - read only */
            break;
    }
}

void lisa_mem_write16(lisa_mem_t *mem, uint32_t addr, uint16_t val) {
    lisa_mem_write8(mem, addr, (val >> 8) & 0xFF);
    lisa_mem_write8(mem, addr + 1, val & 0xFF);
}

void lisa_mem_write32(lisa_mem_t *mem, uint32_t addr, uint32_t val) {
    lisa_mem_write16(mem, addr, (val >> 16) & 0xFFFF);
    lisa_mem_write16(mem, addr + 2, val & 0xFFFF);
}

const uint8_t *lisa_mem_get_video(lisa_mem_t *mem) {
    if (mem->video_addr + LISA_SCREEN_BYTES <= LISA_RAM_SIZE)
        return &mem->ram[mem->video_addr];
    return mem->ram; /* fallback */
}
