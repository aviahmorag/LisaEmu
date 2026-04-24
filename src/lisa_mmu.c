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

/* Active memory pointer for diagnostic hooks. lisa_init() registers. */
static lisa_mem_t *g_active_mem = NULL;

void lisa_mmu_register(lisa_mem_t *mem) {
    g_active_mem = mem;
}

uint8_t lisa_mem_read_phys8(uint32_t phys) {
    lisa_mem_t *mem = g_active_mem;
    if (!mem) return 0;
    phys &= 0xFFFFFF;
    if (phys >= LISA_RAM_SIZE) return 0;
    return mem->ram[phys];
}
uint16_t lisa_mem_read_phys16(uint32_t phys) {
    return ((uint16_t)lisa_mem_read_phys8(phys) << 8) | lisa_mem_read_phys8(phys + 1);
}
uint32_t lisa_mem_read_phys32(uint32_t phys) {
    return ((uint32_t)lisa_mem_read_phys16(phys) << 16) | lisa_mem_read_phys16(phys + 2);
}

bool lisa_mmu_seg_info(int ctx, int seg,
                       uint16_t *out_sor, uint16_t *out_slr,
                       uint8_t *out_changed) {
    lisa_mem_t *mem = g_active_mem;
    if (!mem) return false;
    if (ctx < 0 || ctx >= MMU_NUM_CONTEXTS) return false;
    if (seg < 0 || seg >= MMU_NUM_SEGMENTS) return false;
    mmu_segment_t *s = &mem->segments[ctx][seg];
    if (out_sor) *out_sor = s->sor;
    if (out_slr) *out_slr = s->slr;
    if (out_changed) *out_changed = s->changed;
    return s->changed != 0;
}

uint32_t lisa_mmu_xlate_info(uint32_t logical,
                             int *out_seg, uint16_t *out_sor,
                             uint16_t *out_slr, uint8_t *out_changed) {
    lisa_mem_t *mem = g_active_mem;
    int seg = (logical >> 17) & 0x7F;
    if (out_seg) *out_seg = seg;
    if (!mem) {
        if (out_sor) *out_sor = 0;
        if (out_slr) *out_slr = 0;
        if (out_changed) *out_changed = 0;
        return logical & 0xFFFFFF;
    }
    int ctx = mem->current_context;
    if (ctx >= MMU_NUM_CONTEXTS) ctx = 0;
    mmu_segment_t *s = &mem->segments[ctx][seg];
    if (out_sor) *out_sor = s->sor;
    if (out_slr) *out_slr = s->slr;
    if (out_changed) *out_changed = s->changed;
    if (!mem->mmu_enabled) return logical & 0xFFFFFF;
    if (s->changed == 0) return logical & 0xFFFFFF;
    uint32_t offset = logical & 0x1FFFF;
    uint32_t phys = ((uint32_t)s->sor << 9) + offset;
    uint16_t slr_type = s->slr & SLR_MASK;
    if (slr_type == SLR_IO_SPACE || slr_type == SLR_SIO_SPACE)
        return 0xFC0000 | offset;
    return phys & 0xFFFFFF;
}

void lisa_mmu_dump_segments(void) {
    lisa_mem_t *mem = g_active_mem;
    if (!mem) {
        fprintf(stderr, "  [MMU-DUMP] (no active mem)\n");
        return;
    }
    int ctx = mem->current_context;
    if (ctx >= MMU_NUM_CONTEXTS) ctx = 0;
    fprintf(stderr, "  [MMU-DUMP] ctx=%d setup=%d configured segs:\n",
            ctx, mem->setup_mode);
    for (int si = 0; si < MMU_NUM_SEGMENTS; si++) {
        mmu_segment_t *s = &mem->segments[ctx][si];
        if (!s->changed) continue;
        uint32_t seg_base = (uint32_t)si << 17;
        uint32_t phys_base = ((uint32_t)s->sor << 9);
        fprintf(stderr, "    seg%-3d virt=$%06X SOR=$%03X SLR=$%03X chg=$%X → phys=$%06X\n",
                si, seg_base, s->sor, s->slr, s->changed, phys_base);
    }
}

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

    /* SYSTEM_ERROR(10201) diagnosis: track every unique (ctx,seg) SOR/SLR
     * first-set + every seg-85 write + every SOR=0 write (seg<126). */
    {
        extern uint32_t g_last_cpu_pc;
        static uint8_t seen_sor[MMU_NUM_CONTEXTS][128];
        static uint8_t seen_slr[MMU_NUM_CONTEXTS][128];
        static int s85_gen = -1;
        extern int g_emu_generation;
        if (s85_gen != g_emu_generation) {
            memset(seen_sor, 0, sizeof(seen_sor));
            memset(seen_slr, 0, sizeof(seen_slr));
            s85_gen = g_emu_generation;
        }
        int is_sor = (addr & 8) != 0;
        uint8_t *seen = is_sor ? &seen_sor[context][seg] : &seen_slr[context][seg];
        int first = (*seen == 0);
        *seen = 1;
        int interesting = (seg == 85) ||
                          (is_sor && data == 0 && seg > 2 && seg < 126) ||
                          first;
        if (interesting) {
            fprintf(stderr, "MMU-W: PC=$%06X ctx=%d seg=%-3d %s=$%03X%s\n",
                    g_last_cpu_pc, context, seg,
                    is_sor ? "SOR" : "SLR", data, first ? " [first]" : "");
        }
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

extern bool g_p128f_watch_armed;
extern uint32_t g_p128f_last_pc;
extern uint32_t g_p128f_last_a6;
extern uint32_t g_p128f_last_a0;
extern uint32_t g_p128f_last_sp;
extern bool g_p128l_mmrb_watch_armed;
extern uint32_t g_p128l_mmrb_addr;

void lisa_mem_write8(lisa_mem_t *mem, uint32_t addr, uint8_t val) {
    /* P128f watchpoint — log writes that touch the OPEN_SFILE devnum
     * slot at $CBFF2C..$CBFF2D so we can pinpoint the corrupting site.
     * Only active when WATCH is armed (= we're inside OPEN_SFILE). */
    extern int g_emu_generation;
    static int p128f_w8_gen = -1;
    static int p128f_w8_count = 0;
    if (p128f_w8_gen != g_emu_generation) {
        p128f_w8_gen = g_emu_generation;
        p128f_w8_count = 0;
    }
    uint32_t a = addr & 0xFFFFFF;
    if (g_p128f_watch_armed && (a == 0xCBFF2C || a == 0xCBFF2D) &&
        p128f_w8_count < 8) {
        fprintf(stderr,
          "[P128f-W8] write8 to $%06X val=$%02X "
          "(PC=$%06X A6=$%06X A0=$%06X SP=$%06X)\n",
          a, val, g_p128f_last_pc,
          g_p128f_last_a6, g_p128f_last_a0, g_p128f_last_sp);
        p128f_w8_count++;
    }

    /* P128l MMRB+0..+3 watchpoint: any byte write that touches
     * hd_qioreq_list.fwd_link/bkwd_link after MM_INIT is suspect.
     * Fires at MOST 16 times per generation. Capture PC + regs so we
     * can trace who clobbered the self-pointer. */
    static int p128l_w8_gen = -1;
    static int p128l_w8_count = 0;
    if (p128l_w8_gen != g_emu_generation) {
        p128l_w8_gen = g_emu_generation;
        p128l_w8_count = 0;
    }
    if (g_p128l_mmrb_watch_armed &&
        g_p128l_mmrb_addr != 0 &&
        a >= g_p128l_mmrb_addr && a <= g_p128l_mmrb_addr + 3 &&
        p128l_w8_count < 16) {
        fprintf(stderr,
          "[P128l-W8] MMRB+%u write val=$%02X (PC=$%06X A6=$%06X A0=$%06X SP=$%06X)\n",
          (unsigned)(a - g_p128l_mmrb_addr), val,
          g_p128f_last_pc, g_p128f_last_a6, g_p128f_last_a0, g_p128f_last_sp);
        p128l_w8_count++;
    }

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
            /* Unmapped-segment safety (write path only): if the logical
             * address was above physical RAM AND the resulting phys is
             * also out of range, the segment is unconfigured. Wrapping
             * with `%= LISA_RAM_SIZE` would alias onto code (caught
             * pre-P23 as seg-85 → phys $002600, now post-P26 as seg-112
             * → phys $08658A). Drop the write. */
            if (addr >= LISA_RAM_SIZE && phys >= LISA_RAM_SIZE) {
                extern uint32_t g_last_cpu_pc;
                static int unmap_count = 0;
                static int unmap_gen = -1;
                extern int g_emu_generation;
                if (unmap_gen != g_emu_generation) { unmap_count = 0; unmap_gen = g_emu_generation; }
                if (unmap_count++ < 16) {
                    int seg = (addr >> 17) & 0x7F;
                    fprintf(stderr,
                        "UNMAPPED-WRITE[%d]: PC=$%06X log=$%06X phys=$%06X val=$%02X (seg=%d) dropped\n",
                        unmap_count, g_last_cpu_pc, addr, phys, val, seg);
                }
                break;
            }
            /* Lisa hardware wraps RAM addresses at the physical memory boundary */
            phys %= LISA_RAM_SIZE;
            /* FB-WRITE watchpoint: log writes into both framebuffer pages
             * (alt at $1F0000, main at $1F8000). Separate counters so we
             * don't drown alt-only logging in ROM's main fill. */
            if (phys >= 0x1F0000 && phys < 0x200000) {
                extern uint32_t g_last_cpu_pc;
                int is_alt = phys < 0x1F8000;
                static int fb_logged_main = 0;
                static int fb_logged_alt = 0;
                int *cnt = is_alt ? &fb_logged_alt : &fb_logged_main;
                if (*cnt < 16) {
                    fprintf(stderr, "FB-WRITE[%s %d] PC=$%06X log=$%06X phys=$%06X val=$%02X\n",
                            is_alt ? "ALT" : "MAIN", *cnt,
                            g_last_cpu_pc, addr, phys, val);
                    (*cnt)++;
                }
            }
            /* Vector-table watchpoint: catch writes into $00-$FF after
             * boot-ROM init completes. The OS re-writes many vectors
             * during INIT_TRAPV; we only care about writes where the
             * resulting byte has a non-zero high bit in a context that
             * would form a corrupt PC on read (tracked: byte values that
             * end up as the MSB of a longword in range [0x8..0xFC]). */
            {
                extern uint32_t g_last_cpu_pc;
            /* P78d: PROTECT the vector table from post-init writes.
             * Many FS/segment functions compute wrong pointers due to
             * codegen bugs, aliasing into the $000-$0FF vector table.
             * Guard: drop ALL writes to $000-$3FF from code above $1000.
             * The vector table is set up during early STARTUP and should
             * NOT be modified by OS kernel code during process creation. */
            /* Only active after SYS_PROC_INIT milestone fires —
             * earlier writes (INIT_TRAPV etc.) are legitimate. */
            extern int g_vec_guard_active;  /* set in m68k.c after SYS_PROC_INIT */
            if (phys < 0x400 && g_last_cpu_pc > 0x1000 && g_vec_guard_active) {
                static int vtg_count = 0;
                static int vtg_gen = -1;
                extern int g_emu_generation;
                if (vtg_gen != g_emu_generation) { vtg_count = 0; vtg_gen = g_emu_generation; }
                if (vtg_count++ < 16 && (phys & 3) == 0) {
                    fprintf(stderr, "VEC-GUARD[%d]: PC=$%06X dropped write to $%06X val=$%02X\n",
                            vtg_count, g_last_cpu_pc, phys, val);
                }
                return;  /* DROP the write — protect vector table */
            }
            /* P122: segment-aliasing guard. Writes whose logical address is
             * outside seg-0 but whose MMU-translated physical lands inside
             * the vector table ($000-$3FF) are always bogus — no legit OS
             * path uses a non-seg-0 logical path to touch vectors/SGLOBAL.
             * This happens when a segment is configured with SOR=0 and
             * RW_MEM (unallocated demand-page state in Apple's scheme)
             * and compiled code writes through a buffer in that segment.
             * Without this guard, BitMap_IO's 1208-byte read into a
             * temp_addr in seg-61 (SOR=0) wipes vectors + SGLOBAL, then
             * SCHDTRAP reads SGLOBAL and dereferences NULL. */
            if (phys < 0x400 && (addr & 0xFFFFFF) >= 0x20000) {
                static int sag_count = 0;
                static int sag_gen = -1;
                extern int g_emu_generation;
                if (sag_gen != g_emu_generation) { sag_count = 0; sag_gen = g_emu_generation; }
                if (sag_count++ < 16) {
                    int seg = (addr >> 17) & 0x7F;
                    fprintf(stderr, "SEG-ALIAS-GUARD[%d]: PC=$%06X dropped write log=$%06X (seg%d) → phys=$%06X val=$%02X\n",
                            sag_count, g_last_cpu_pc, addr, seg, phys, val);
                }
                return;
            }
            }
            /* Watchpoint: $002600..$00260F code region is overwritten during
             * boot — causing SYSTEM_ERROR(10201). Log every write. */
            if (phys >= 0x2600 && phys < 0x2610) {
                extern uint32_t g_last_cpu_pc;
                static int w2600_count = 0;
                static int w2600_gen = -1;
                extern int g_emu_generation;
                if (w2600_gen != g_emu_generation) {
                    w2600_count = 0; w2600_gen = g_emu_generation;
                }
                if (w2600_count++ < 32) {
                    fprintf(stderr, "WATCH-$2600 [%d]: PC=$%06X log=$%06X phys=$%06X val=$%02X\n",
                            w2600_count, g_last_cpu_pc, addr, phys, val);
                    if (w2600_count == 1) {
                        for (int c = 0; c < MMU_NUM_CONTEXTS; c++) {
                            mmu_segment_t *s85 = &mem->segments[c][85];
                            fprintf(stderr, "  SEG85 ctx=%d sor=$%03X slr=$%03X changed=$%X\n",
                                    c, s85->sor, s85->slr, s85->changed);
                        }
                        fprintf(stderr, "  current_context=%d mmu_enabled=%d setup_mode=%d\n",
                                mem->current_context, mem->mmu_enabled, mem->setup_mode);
                    }
                }
            }
            /* b_syslocal_ptr watchpoint: 4-byte slot at A5-24785 sits at
             * logical $00CC0F2B..$00CC0F2E. POOL_INIT writes the correct
             * value there (verified via INIT_FREEPOOL #2 firing with the
             * right sl_free_pool_addr), but by SYS_PROC_INIT entry the
             * slot reads $FFFFFFFF. Something in between is clobbering
             * it. Log every byte write that lands in that range so we
             * can identify the offending instruction + caller. */
            if (addr >= 0xCC0F2B && addr <= 0xCC0F2E) {
                extern uint32_t g_last_cpu_pc;
                static int wbsl_count = 0;
                static int wbsl_gen = -1;
                extern int g_emu_generation;
                if (wbsl_gen != g_emu_generation) {
                    wbsl_count = 0; wbsl_gen = g_emu_generation;
                }
                if (wbsl_count++ < 64) {
                    /* Read the opcode at PC via the same MMU translation path
                     * the CPU uses, so we see what's ACTUALLY there (the code
                     * may have been overwritten at runtime). */
                    uint8_t op0 = lisa_mem_read8(mem, g_last_cpu_pc);
                    uint8_t op1 = lisa_mem_read8(mem, g_last_cpu_pc + 1);
                    uint8_t op2 = lisa_mem_read8(mem, g_last_cpu_pc + 2);
                    uint8_t op3 = lisa_mem_read8(mem, g_last_cpu_pc + 3);
                    fprintf(stderr, "WATCH-B_SLOC [%d]: PC=$%06X op=%02X%02X%02X%02X log=$%06X phys=$%06X val=$%02X\n",
                            wbsl_count, g_last_cpu_pc, op0, op1, op2, op3, addr, phys, val);
                }
            }
            /* $08658A watchpoint: post-P26 illegal-instr at this PC
             * with opcode $00CC, while linked binary has $3400 — code
             * overwrite, same class as the pre-P23 $2600 one. Log
             * writes to this range to find the aliasing caller. */
            if (phys >= 0x86580 && phys < 0x86590) {
                extern uint32_t g_last_cpu_pc;
                static int w86_count = 0;
                static int w86_gen = -1;
                extern int g_emu_generation;
                if (w86_gen != g_emu_generation) { w86_count = 0; w86_gen = g_emu_generation; }
                if (w86_count++ < 32) {
                    int seg = (addr >> 17) & 0x7F;
                    int ctx = mem->current_context;
                    if (ctx >= MMU_NUM_CONTEXTS) ctx = 0;
                    mmu_segment_t *s = &mem->segments[ctx][seg];
                    fprintf(stderr, "WATCH-$8658A [%d]: PC=$%06X log=$%06X phys=$%06X val=$%02X "
                            "(seg=%d ctx=%d sor=$%03X slr=$%03X)\n",
                            w86_count, g_last_cpu_pc, addr, phys, val,
                            seg, ctx, s->sor, s->slr);
                }
            }
            /* SMT-region watchpoint (P88-followup): the realmemmmu segs
             * (85-100) end up with SOR=0 at the moment PROG_MMU's TRAP6
             * HLE reads the SMT entry. Either Pascal SETMMU writes to a
             * different address, writes at a different time, or never
             * writes at all. Log every byte-write into domain-0's SMT
             * range [smt_base .. smt_base+$400) with PC, logical addr,
             * decoded (seg, field) and the new value so we can correlate
             * with PROG_MMU TRAP6 firings. */
            {
                extern uint32_t g_hle_smt_base;
                uint32_t smt_lo = g_hle_smt_base;
                uint32_t smt_hi = g_hle_smt_base + 0x400;
                if (smt_lo != 0 && phys >= smt_lo && phys < smt_hi) {
                    extern uint32_t g_last_cpu_pc;
                    static int wsmt_count = 0;
                    static int wsmt_gen = -1;
                    extern int g_emu_generation;
                    if (wsmt_gen != g_emu_generation) {
                        wsmt_count = 0; wsmt_gen = g_emu_generation;
                    }
                    uint32_t off  = phys - smt_lo;
                    uint32_t seg  = off / 4;
                    uint32_t fld  = off % 4;   /* 0/1=origin hi/lo, 2=access, 3=limit */
                    const char *fn = (fld < 2) ? "origin" : (fld == 2 ? "access" : "limit");
                    /* Always log writes to seg 102 (the MMRB-aliasing seg);
                     * otherwise obey the 128 cap so early-boot bulk writes
                     * stay readable. */
                    bool is_seg102 = (seg == 102);
                    if (is_seg102 || wsmt_count < 128) {
                        if (!is_seg102) wsmt_count++;
                        fprintf(stderr,
                            "WATCH-SMT[%d]: PC=$%06X log=$%06X phys=$%06X val=$%02X (dom=0 seg=%u %s byte=%u)\n",
                            wsmt_count, g_last_cpu_pc, addr, phys, val, seg, fn, fld);
                    }
                }
            }
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
                    /* Per OS globals ScrnPhys=$1F8000 / AltScrnPhys=$1F0000:
                     * main at top of 2MB - $8000, alt one page below (top - $10000). */
                    mem->video_addr = mem->video_alt
                        ? (2 * 1024 * 1024 - 0x10000)   /* $1F0000 */
                        : (2 * 1024 * 1024 - 0x8000);   /* $1F8000 */
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

    /* P128f one-shot watchpoint: log writes to OPEN_SFILE devnum slot
     * ($CBFF2C) so we can find what miscompile is clobbering it. */
    extern int g_emu_generation;
    static int p128f_w16_gen = -1;
    static int p128f_w16_count = 0;
    if (p128f_w16_gen != g_emu_generation) {
        p128f_w16_gen = g_emu_generation;
        p128f_w16_count = 0;
    }
    if (g_p128f_watch_armed && (addr & 0xFFFFFF) == 0xCBFF2C &&
        p128f_w16_count < 8) {
        fprintf(stderr,
          "[P128f-W16] write16 to $CBFF2C val=$%04X (PC≈$%06X)\n",
          val, g_p128f_last_pc);
        p128f_w16_count++;
    }


    /* HLE: Bypass Lisabug auto-entry on dev-disk boots.
     *
     * SCAFFOLDING FOR PREBUILT-IMAGE SMOKE TESTS ONLY. Does not belong
     * in the source-compile path — that flow will not link SYSTEM.DEBUG
     * at all. Narrowly gated on dev-disk boots of los_compilation_base.
     *
     * Lisa OS installs `JMP macsbug` at low-memory address $234 via
     * INIT_NMI_TRAPV (see source-NMIHANDLER.TEXT line 239:
     *     move.w  #$4ef9,enter_macsbug  ; where enter_macsbug .equ $234
     * ).
     *
     * The real Lisabug entry path is NOT Pascal `MACSBUG;` → `JSR $234`
     * (there is no such direct JSR). Pascal `MACSBUG;` compiles to
     * `JSR macsbug` where macsbug is the assembly routine in
     * source-NMIHANDLER.TEXT:262. That routine sets up a synthetic
     * level-7 exception frame on the supervisor stack (via `trap #7`
     * from the userstate path, or explicit `move.w sr,-(sp)` from the
     * supstate path) and ends with `jmp enter_macsbug` — i.e., `jmp $234`
     * — from the `its_enabled` path in `lisabugentry`. The comment
     * above `lisabugentry` in the OS source is literal:
     *     ";IF Lisabug exists, emulate a level 7 interrupt to get there"
     *
     * So by the time CPU executes whatever is at $234, the supervisor
     * stack has (top) SR word, (next) PC longword — a real exception
     * frame. The correct bypass is RTE ($4E73), which pops SR+PC (6
     * bytes) and returns to the Pascal caller cleanly.
     *
     * A previous iteration of this bypass used RTS ($4E75), which only
     * pops 4 bytes as PC — that popped SR + first-half-of-PC as garbage
     * and broke the return. That bug led to the mistaken conclusion
     * that Lisabug was being entered via some other path (like
     * `hard_excep`/`pmacsbug` directly); in reality no exception ever
     * fires on the dev-disk boot — per-vector first-fire instrumentation
     * in src/m68k.c:take_exception confirms only v37/v38 hit.
     *
     * Root cause of the on-screen "Level 7 Interrupt" banner: it is
     * Lisabug's own header text, drawn after being entered through this
     * synthetic-exception path — not from any real CPU interrupt.
     * The actual entry trigger is SOURCE-STARTUP.TEXT:302 (DB_INIT),
     * the deliberate Workshop developer breakpoint. */
    /* NOTE: previously rewrote $4EF9 at $234 → $4E73 (RTE) here to
     * bypass Lisabug entry. That was too aggressive: both DB_INIT
     * (synthetic level-7 frame — safe to RTE) AND hard_excep (real
     * exception frame — must NOT RTE) route through $234. Patching
     * memory defeats the runtime IPL-gated bypass in m68k.c. Leave
     * memory alone; the fetch-time gate at src/m68k.c decides per
     * entry based on stacked SR IPL. */
    if (addr == 0x234 && val == 0x4EF9) {
        static int noted = 0;
        if (!noted++) {
            fprintf(stderr, "[HLE] $234 JMP install observed "
                    "(handled by runtime IPL gate, not patched)\n");
        }
    }

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
    /* Early boot: the Lisa OS draws its startup/service console to the
     * alternate framebuffer at phys $1F0000 and only flips VideoLatch
     * much later. To actually show something during the first ~second
     * of boot, pick whichever of the two Lisa framebuffers has more
     * non-zero content. Once VideoLatch has been explicitly set to an
     * unambiguous page, trust it. */
    uint32_t main_addr = 2 * 1024 * 1024 - 0x8000;
    uint32_t alt_addr  = 2 * 1024 * 1024 - 0x10000;
    if (main_addr + LISA_SCREEN_BYTES > LISA_RAM_SIZE ||
        alt_addr  + LISA_SCREEN_BYTES > LISA_RAM_SIZE)
        return &mem->ram[mem->video_addr];

    int nz_main = 0, nz_alt = 0;
    for (int i = 0; i < LISA_SCREEN_BYTES; i += 64) {
        if (mem->ram[main_addr + i]) nz_main++;
        if (mem->ram[alt_addr  + i]) nz_alt++;
    }
    if (nz_alt > nz_main * 2) return &mem->ram[alt_addr];
    return &mem->ram[main_addr];
}
