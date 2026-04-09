/*
 * LisaEm - Apple Lisa Emulator
 * Memory Management Unit and Memory Controller
 *
 * The Lisa MMU uses a segmented address translation scheme:
 *   - 5 contexts (0=start mode, 1-4=normal)
 *   - 128 segments per context, each up to 128KB
 *   - Address split: bits[23:17]=segment, bits[16:0]=offset
 *   - Physical = (SOR << 9) + offset
 *
 * During start mode, the OS configures MMU segment registers by
 * writing to special I/O addresses. When start mode is exited
 * ($FCE012 write), MMU translation becomes active.
 *
 * Context selection:
 *   context = start ? 0 : (1 + (segment1 | segment2))
 *   segment1 set/cleared by writes to $FCE008/$FCE00A
 *   segment2 set/cleared by writes to $FCE00C/$FCE00E
 */

#include "lisa_mmu.h"
#include <string.h>
#include <stdio.h>

int mmu_write_count_global = 0;

void lisa_mem_init(lisa_mem_t *mem) {
    memset(mem, 0, sizeof(lisa_mem_t));

    /* Initial state: setup mode, ROM visible at low addresses */
    mem->setup_mode = true;
    mem->mmu_enabled = false;
    mem->current_context = 0;
    mem->segment1 = 0;
    mem->segment2 = 0;

    /* Default video address (primary screen) */
    mem->video_addr = 2 * 1024 * 1024 - 0x8000;  /* Top of reported 2MB ($1F8000) */
    mem->contrast = 0xFF;
    mem->vretrace_enabled = true;
}

void lisa_mem_load_rom(lisa_mem_t *mem, const uint8_t *data, size_t size) {
    if (size > LISA_ROM_SIZE) size = LISA_ROM_SIZE;
    memcpy(mem->rom, data, size);
}

/* Compute current MMU context from segment1/segment2/start flags */
static int compute_context(lisa_mem_t *mem) {
    if (mem->setup_mode) return 0;  /* Start mode = context 0 */
    return 1 + (mem->segment1 | mem->segment2);
}

/* Handle MMU register write (SOR/SLR).
 * During start mode, the OS writes to addresses that decode to MMU registers.
 * In LisaEm, these are in the SIO space when certain address bits are set.
 * For our simplified model, we detect MMU register writes by address pattern:
 *   - The segment index comes from bits [23:17] (or bits [20:14] in start mode)
 *   - Bit 3 selects SOR (1) vs SLR (0)
 */
static void mmu_reg_write(lisa_mem_t *mem, uint32_t addr, uint16_t data) {
    /* MMU register writes always target the "real" context (CXASEL),
     * even during start mode. CXASEL ignores the start flag:
     *   context = 1 + (segment1 | segment2)
     * This way the OS can configure context 1 segments while executing
     * in start mode (context 0). */
    int seg = (addr >> 17) & 0x7F;
    int context = 1 + (mem->segment1 | mem->segment2);  /* CXASEL */

    /* Clamp to valid range */
    if (context >= MMU_NUM_CONTEXTS) context = MMU_NUM_CONTEXTS - 1;

    data &= 0x0FFF;  /* 12-bit effective data */

    /* Bit 3 of the low address selects SOR (8) vs SLR (0).
     * SLIM addresses end in $8000, SORG addresses end in $8008. */
    if (addr & 8) {
        /* SORG (Segment Origin Register) write */
        mem->segments[context][seg].sor = data;
        mem->segments[context][seg].changed |= 2;
    } else {
        /* SLIM (Segment Limit Register) write */
        mem->segments[context][seg].slr = data;
        mem->segments[context][seg].changed |= 1;
    }

    /* Mirror critical segments to ALL contexts.
     * DO_AN_MMU switches context bits to program different domains.
     * Between setupoff/setupon, the CPU briefly runs in the target domain's
     * context. The mmucodemmu segment (84) and I/O segment (126) must be
     * accessible in ALL contexts, or DO_AN_MMU crashes when it switches
     * to an empty context.
     * Also mirror context 1 to context 0 for setup mode access. */
    if (context == 1) {
        mem->segments[0][seg] = mem->segments[1][seg];
    }
    /* Mirror mmucodemmu (84) and I/O segments (126,127) to all contexts */
    if (seg == 84 || seg == 126 || seg == 127) {
        for (int c = 0; c < MMU_NUM_CONTEXTS; c++) {
            if (c != context) {
                mem->segments[c][seg] = mem->segments[context][seg];
            }
        }
    }

    static int mmu_write_count = 0;
    mmu_write_count++;
    extern int mmu_write_count_global;
    mmu_write_count_global = mmu_write_count;
    if (mmu_write_count <= 5 || (mmu_write_count >= 39 && mmu_write_count <= 70) || mmu_write_count == 500 || mmu_write_count == 1000) {
        fprintf(stderr, "MMU REG[%d]: ctx=%d seg=%d %s=$%03X (addr=$%06X)\n",
                mmu_write_count, context, seg, (addr & 8) ? "SOR" : "SLR", data, addr);
    }
}

/* Translate address through MMU */
static uint32_t mmu_translate(lisa_mem_t *mem, uint32_t addr) {
    if (!mem->mmu_enabled)
        return addr;

    /* Segment number from high bits of address */
    int seg = (addr >> 17) & 0x7F;
    int ctx = mem->current_context;
    if (ctx >= MMU_NUM_CONTEXTS) ctx = 0;

    mmu_segment_t *s = &mem->segments[ctx][seg];

    /* Check if segment has been configured (written via MMU registers) */
    if (s->changed == 0) {
        /* Unconfigured segment — pass through */
        return addr;
    }

    /* Apply segment origin: physical = (SOR << 9) + offset_within_segment */
    uint32_t offset = addr & 0x1FFFF;  /* 17-bit offset (128KB segments) */
    uint32_t phys = ((uint32_t)s->sor << 9) + offset;

    /* Check SLR for I/O space mapping.
     * On real Lisa hardware, I/O segments ignore SOR — the 17-bit offset
     * within the segment maps directly to the I/O address bus at $FC0000+.
     * The OS sets SOR to arbitrary values for I/O segments (e.g. $0E00)
     * which must NOT be used for address calculation. */
    uint16_t slr_type = s->slr & SLR_MASK;
    if (slr_type == SLR_IO_SPACE || slr_type == SLR_SIO_SPACE) {
        return 0xFC0000 | offset;  /* Direct offset into I/O space */
    }

    return phys & 0xFFFFFF;  /* 24-bit physical address */
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
            /* Translation may redirect to I/O */
            if (phys >= LISA_IO_BASE && phys < LISA_ROM_BASE) {
                if (mem->io_read)
                    return mem->io_read(phys - LISA_IO_BASE);
                return 0xFF;
            }
            /* Lisa hardware wraps RAM addresses at the physical memory boundary */
            phys %= LISA_RAM_SIZE;
            return mem->ram[phys];
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

/* Update MMU context after context control register change */
static void update_mmu_context(lisa_mem_t *mem) {
    int new_ctx = compute_context(mem);
    mem->current_context = new_ctx;

    /* MMU translation is ALWAYS active — even during setup mode.
     * Setup mode only affects: (1) ROM overlay at $0-$7FFF, and
     * (2) which context is used (context 0 during setup mode).
     * The MMU must remain enabled because DO_AN_MMU code runs from
     * MMU-mapped segments while toggling setup mode. */
    mem->mmu_enabled = true;
}

void lisa_mem_write8(lisa_mem_t *mem, uint32_t addr, uint8_t val) {
    addr &= 0xFFFFFF;

    switch (addr_region(addr)) {
        case 0: { /* RAM */
            uint32_t phys = mmu_translate(mem, addr);
            /* Translation may redirect to I/O */
            if (phys >= LISA_IO_BASE && phys < LISA_ROM_BASE) {
                if (mem->io_write)
                    mem->io_write(phys - LISA_IO_BASE, val);
                break;
            }
            /* Lisa hardware wraps RAM addresses at the physical memory boundary */
            phys %= LISA_RAM_SIZE;
            mem->ram[phys] = val;
            break;
        }
        case 1: { /* I/O */
            uint32_t offset = addr - LISA_IO_BASE;

            /* Handle I/O registers */
            switch (offset) {
                case IO_CONTRAST:
                    mem->contrast = val;
                    break;

                case IO_VIDEO_LATCH:
                    mem->video_alt = (val & 1) != 0;
                    if (mem->video_alt)
                        mem->video_addr = LISA_RAM_SIZE - 0x8000 + LISA_SCREEN_BYTES;
                    else
                        mem->video_addr = 2 * 1024 * 1024 - 0x8000;  /* Top of reported 2MB ($1F8000) */
                    break;

                case IO_SETUP_SET:   /* $FCE010 — enter start/setup mode */
                    mem->setup_mode = true;
                    update_mmu_context(mem);
                    break;

                case IO_SETUP_RESET: /* $FCE012 — exit start mode, enable MMU */
                    if (mem->setup_mode) {
                        static bool first_exit = true;
                        if (first_exit) {
                            fprintf(stderr, "MMU: exiting start mode, context=%d, enabling translation\n",
                                    compute_context(mem));
                            first_exit = false;
                        }
                    }
                    mem->setup_mode = false;
                    update_mmu_context(mem);
                    break;

                case IO_SEG1_CLEAR:  /* $FCE008 — segment1 = 0 */
                    mem->segment1 = 0;
                    update_mmu_context(mem);
                    break;

                case IO_SEG1_SET:    /* $FCE00A — segment1 = 1 */
                    mem->segment1 = 1;
                    update_mmu_context(mem);
                    break;

                case IO_SEG2_CLEAR:  /* $FCE00C — segment2 = 0 */
                    mem->segment2 = 0;
                    update_mmu_context(mem);
                    break;

                case IO_SEG2_SET:    /* $FCE00E — segment2 = 2 */
                    mem->segment2 = 2;
                    update_mmu_context(mem);
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
    addr &= 0xFFFFFF;

    /* Check for MMU register writes during start mode.
     * The OS programs MMU via TRAP #6 handler (DO_AN_MMU) which:
     *   1. Turns setup on ($FCE010)
     *   2. Writes SLIM at address = $8000 + seg_index * $20000
     *   3. Writes SORG at address = $8008 + seg_index * $20000
     *   4. Turns setup off ($FCE012)
     *
     * During setup mode, writes to addresses $8000-$FFFFFF with
     * the pattern (addr & 0x7FF0) == 0x0 or 0x8 are MMU registers.
     * The segment index is in bits [23:17]. */
    {
        uint32_t low17 = addr & 0x1FFFF;
        if (mem->setup_mode && (low17 == 0x8000 || low17 == 0x8008)) {
            /* MMU register write: SLIM at $8000, SORG at $8008 + seg*$20000 */
            mmu_reg_write(mem, addr, val);
            return;
        }
    }

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
