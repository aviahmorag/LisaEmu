/*
 * LisaEm - Apple Lisa Emulator
 * Motorola 68000 CPU Emulator
 *
 * Full implementation of the Motorola 68000 instruction set.
 * Reference: M68000 Programmer's Reference Manual (Motorola, 1992)
 */

#include "m68k.h"
#include "lisa_mmu.h"
#include "boot_progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

static inline uint32_t mask_24(uint32_t addr) {
    return addr & 0x00FFFFFF;  /* 68000 has 24-bit address bus */
}

static inline uint8_t cpu_read8(m68k_t *cpu, uint32_t addr) {
    return cpu->read8(mask_24(addr));
}

static inline uint16_t cpu_read16(m68k_t *cpu, uint32_t addr) {
    return cpu->read16(mask_24(addr));
}

static inline uint32_t cpu_read32(m68k_t *cpu, uint32_t addr) {
    return cpu->read32(mask_24(addr));
}

/* P84 debug: trace writes to the MAKE_MRDATA sdb region to diagnose
 * why BLD_SEG's field writes don't land in the right place. */
extern int g_p84_trace;
int g_p84_trace = 0;
static inline void p84_log_write(m68k_t *cpu, uint32_t addr, uint32_t val, int width) {
    if (!g_p84_trace) return;
    uint32_t a = mask_24(addr);
    if (a < 0x00CCBA40 || a > 0x00CCBA90) return;
    static int budget = 80;
    if (budget-- <= 0) return;
    fprintf(stderr, "[P84W] pc=$%06X write%d @$%06X <= $%0*X\n",
            cpu->pc & 0xFFFFFF, width, a, width * 2, val);
}

static inline void cpu_write8(m68k_t *cpu, uint32_t addr, uint8_t val) {
    p84_log_write(cpu, addr, val, 1);
    cpu->write8(mask_24(addr), val);
}

static inline void cpu_write16(m68k_t *cpu, uint32_t addr, uint16_t val) {
    p84_log_write(cpu, addr, val, 2);
    cpu->write16(mask_24(addr), val);
}

static inline void cpu_write32(m68k_t *cpu, uint32_t addr, uint32_t val) {
    p84_log_write(cpu, addr, val, 4);
    cpu->write32(mask_24(addr), val);
}

static inline uint16_t fetch16(m68k_t *cpu) {
    uint16_t val = cpu_read16(cpu, cpu->pc);
    cpu->pc += 2;
    return val;
}

static inline uint32_t fetch32(m68k_t *cpu) {
    uint32_t val = cpu_read32(cpu, cpu->pc);
    cpu->pc += 4;
    return val;
}

/* Sign extension */


static inline int32_t sign_extend_16(uint16_t val) {
    return (int32_t)(int16_t)val;
}

/* Stack operations - use A7 */
static inline void push16(m68k_t *cpu, uint16_t val) {
    cpu->a[7] -= 2;
    cpu_write16(cpu, cpu->a[7], val);
}

static inline void push32(m68k_t *cpu, uint32_t val) {
    cpu->a[7] -= 4;
    cpu_write32(cpu, cpu->a[7], val);
    /* Probe: catch any 32-bit push with non-zero high byte onto the
     * supervisor stack. Such values become bogus PCs on the Lisa's
     * 24-bit bus when popped by RTS/RTE. */
    if ((val & 0xFF000000) && cpu->a[7] >= 0xCB0000 && cpu->a[7] < 0xD00000) {
        extern int g_emu_generation;
        static int hip_count = 0;
        static int hip_gen = -1;
        if (hip_gen != g_emu_generation) { hip_count = 0; hip_gen = g_emu_generation; }
        if (hip_count++ < 16) {
            fprintf(stderr, "PUSH-HIPC[%d]: PC=$%08X pushed $%08X to SSP=$%08X\n",
                    hip_count, cpu->pc, val, cpu->a[7]);
        }
    }
}

static inline uint16_t pop16(m68k_t *cpu) {
    uint16_t val = cpu_read16(cpu, cpu->a[7]);
    cpu->a[7] += 2;
    return val;
}

static inline uint32_t pop32(m68k_t *cpu) {
    uint32_t val = cpu_read32(cpu, cpu->a[7]);
    cpu->a[7] += 4;
    if ((val & 0xFF000000)) {
        extern int g_emu_generation;
        static int pop_count = 0;
        static int pop_gen = -1;
        if (pop_gen != g_emu_generation) { pop_count = 0; pop_gen = g_emu_generation; }
        if (pop_count++ < 16) {
            fprintf(stderr, "POP-HIPC[%d]: PC=$%08X popped $%08X from SSP=$%08X\n",
                    pop_count, cpu->pc, val, cpu->a[7] - 4);
        }
    }
    return val;
}

/* Supervisor mode helpers */
static inline bool is_supervisor(m68k_t *cpu) {
    return (cpu->sr & SR_SUPERVISOR) != 0;
}

static void set_supervisor(m68k_t *cpu, bool super) {
    if (super && !is_supervisor(cpu)) {
        cpu->usp = cpu->a[7];
        cpu->a[7] = cpu->ssp;
        cpu->sr |= SR_SUPERVISOR;
    } else if (!super && is_supervisor(cpu)) {
        cpu->ssp = cpu->a[7];
        cpu->a[7] = cpu->usp;
        cpu->sr &= ~SR_SUPERVISOR;
    }
}

/* ========================================================================
 * Condition code helpers
 * ======================================================================== */

static inline void set_flag(m68k_t *cpu, uint16_t flag, bool val) {
    if (val) cpu->sr |= flag; else cpu->sr &= ~flag;
}

static inline bool get_flag(m68k_t *cpu, uint16_t flag) {
    return (cpu->sr & flag) != 0;
}

/* Set NZ flags for result */
static void set_nz_8(m68k_t *cpu, uint8_t result) {
    set_flag(cpu, SR_NEGATIVE, (result & 0x80) != 0);
    set_flag(cpu, SR_ZERO, result == 0);
}

static void set_nz_16(m68k_t *cpu, uint16_t result) {
    set_flag(cpu, SR_NEGATIVE, (result & 0x8000) != 0);
    set_flag(cpu, SR_ZERO, result == 0);
}

static void set_nz_32(m68k_t *cpu, uint32_t result) {
    set_flag(cpu, SR_NEGATIVE, (result & 0x80000000) != 0);
    set_flag(cpu, SR_ZERO, result == 0);
}

/* Set flags for logical operations (V=0, C=0) */
static void set_logic_flags_8(m68k_t *cpu, uint8_t result) {
    set_nz_8(cpu, result);
    set_flag(cpu, SR_OVERFLOW, false);
    set_flag(cpu, SR_CARRY, false);
}

static void set_logic_flags_16(m68k_t *cpu, uint16_t result) {
    set_nz_16(cpu, result);
    set_flag(cpu, SR_OVERFLOW, false);
    set_flag(cpu, SR_CARRY, false);
}

static void set_logic_flags_32(m68k_t *cpu, uint32_t result) {
    set_nz_32(cpu, result);
    set_flag(cpu, SR_OVERFLOW, false);
    set_flag(cpu, SR_CARRY, false);
}

/* ADD/SUB flag computation */
static void set_add_flags_8(m68k_t *cpu, uint8_t src, uint8_t dst, uint16_t result) {
    uint8_t r = (uint8_t)result;
    set_nz_8(cpu, r);
    bool sm = (src & 0x80) != 0;
    bool dm = (dst & 0x80) != 0;
    bool rm = (r & 0x80) != 0;
    set_flag(cpu, SR_OVERFLOW, (sm && dm && !rm) || (!sm && !dm && rm));
    set_flag(cpu, SR_CARRY, result > 0xFF);
    set_flag(cpu, SR_EXTEND, result > 0xFF);
}

static void set_add_flags_16(m68k_t *cpu, uint16_t src, uint16_t dst, uint32_t result) {
    uint16_t r = (uint16_t)result;
    set_nz_16(cpu, r);
    bool sm = (src & 0x8000) != 0;
    bool dm = (dst & 0x8000) != 0;
    bool rm = (r & 0x8000) != 0;
    set_flag(cpu, SR_OVERFLOW, (sm && dm && !rm) || (!sm && !dm && rm));
    set_flag(cpu, SR_CARRY, result > 0xFFFF);
    set_flag(cpu, SR_EXTEND, result > 0xFFFF);
}

static void set_add_flags_32(m68k_t *cpu, uint32_t src, uint32_t dst, uint64_t result) {
    uint32_t r = (uint32_t)result;
    set_nz_32(cpu, r);
    bool sm = (src & 0x80000000) != 0;
    bool dm = (dst & 0x80000000) != 0;
    bool rm = (r & 0x80000000) != 0;
    set_flag(cpu, SR_OVERFLOW, (sm && dm && !rm) || (!sm && !dm && rm));
    set_flag(cpu, SR_CARRY, result > 0xFFFFFFFF);
    set_flag(cpu, SR_EXTEND, result > 0xFFFFFFFF);
}

static void set_sub_flags_8(m68k_t *cpu, uint8_t src, uint8_t dst, uint16_t result) {
    uint8_t r = (uint8_t)result;
    set_nz_8(cpu, r);
    bool sm = (src & 0x80) != 0;
    bool dm = (dst & 0x80) != 0;
    bool rm = (r & 0x80) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    bool borrow = ((uint16_t)src > (uint16_t)dst);
    set_flag(cpu, SR_CARRY, borrow);
    set_flag(cpu, SR_EXTEND, borrow);
}

static void set_sub_flags_16(m68k_t *cpu, uint16_t src, uint16_t dst, uint32_t result) {
    uint16_t r = (uint16_t)result;
    set_nz_16(cpu, r);
    bool sm = (src & 0x8000) != 0;
    bool dm = (dst & 0x8000) != 0;
    bool rm = (r & 0x8000) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    bool borrow = ((uint32_t)src > (uint32_t)dst);
    set_flag(cpu, SR_CARRY, borrow);
    set_flag(cpu, SR_EXTEND, borrow);
}

static void set_sub_flags_32(m68k_t *cpu, uint32_t src, uint32_t dst, uint64_t result) {
    uint32_t r = (uint32_t)result;
    set_nz_32(cpu, r);
    bool sm = (src & 0x80000000) != 0;
    bool dm = (dst & 0x80000000) != 0;
    bool rm = (r & 0x80000000) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    bool borrow = ((uint64_t)src > (uint64_t)dst);
    set_flag(cpu, SR_CARRY, borrow);
    set_flag(cpu, SR_EXTEND, borrow);
}

static void set_cmp_flags_8(m68k_t *cpu, uint8_t src, uint8_t dst) {
    uint16_t result = (uint16_t)dst - (uint16_t)src;
    uint8_t r = (uint8_t)result;
    set_nz_8(cpu, r);
    bool sm = (src & 0x80) != 0;
    bool dm = (dst & 0x80) != 0;
    bool rm = (r & 0x80) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    set_flag(cpu, SR_CARRY, (uint16_t)src > (uint16_t)dst);
}

static void set_cmp_flags_16(m68k_t *cpu, uint16_t src, uint16_t dst) {
    uint32_t result = (uint32_t)dst - (uint32_t)src;
    uint16_t r = (uint16_t)result;
    set_nz_16(cpu, r);
    bool sm = (src & 0x8000) != 0;
    bool dm = (dst & 0x8000) != 0;
    bool rm = (r & 0x8000) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    set_flag(cpu, SR_CARRY, (uint32_t)src > (uint32_t)dst);
}

static void set_cmp_flags_32(m68k_t *cpu, uint32_t src, uint32_t dst) {
    uint64_t result = (uint64_t)dst - (uint64_t)src;
    uint32_t r = (uint32_t)result;
    set_nz_32(cpu, r);
    bool sm = (src & 0x80000000) != 0;
    bool dm = (dst & 0x80000000) != 0;
    bool rm = (r & 0x80000000) != 0;
    set_flag(cpu, SR_OVERFLOW, (!sm && dm && !rm) || (sm && !dm && rm));
    set_flag(cpu, SR_CARRY, (uint64_t)src > (uint64_t)dst);
}

/* ========================================================================
 * Condition code testing (for Bcc, Scc, DBcc)
 * ======================================================================== */

static bool test_condition(m68k_t *cpu, int cond) {
    bool c = get_flag(cpu, SR_CARRY);
    bool v = get_flag(cpu, SR_OVERFLOW);
    bool z = get_flag(cpu, SR_ZERO);
    bool n = get_flag(cpu, SR_NEGATIVE);

    switch (cond) {
        case 0x0: return true;            /* T */
        case 0x1: return false;           /* F */
        case 0x2: return !c && !z;        /* HI */
        case 0x3: return c || z;          /* LS */
        case 0x4: return !c;             /* CC (HI) */
        case 0x5: return c;              /* CS (LO) */
        case 0x6: return !z;             /* NE */
        case 0x7: return z;              /* EQ */
        case 0x8: return !v;             /* VC */
        case 0x9: return v;              /* VS */
        case 0xA: return !n;             /* PL */
        case 0xB: return n;              /* MI */
        case 0xC: return (n && v) || (!n && !v);    /* GE */
        case 0xD: return (n && !v) || (!n && v);    /* LT */
        case 0xE: return (n && v && !z) || (!n && !v && !z); /* GT */
        case 0xF: return z || (n && !v) || (!n && v);        /* LE */
    }
    return false;
}

/* ========================================================================
 * Effective Address calculation
 * ======================================================================== */

/* Calculate effective address. Returns the address for memory modes.
 * For register modes, returns a sentinel and sets reg_ptr. */
typedef struct {
    uint32_t addr;
    bool is_register;
    uint32_t *reg_ptr;
} ea_result_t;

static ea_result_t calc_ea(m68k_t *cpu, int mode, int reg, int size) {
    ea_result_t ea = {0, false, NULL};

    switch (mode) {
        case AM_DATA_REG:
            ea.is_register = true;
            ea.reg_ptr = &cpu->d[reg];
            break;

        case AM_ADDR_REG:
            ea.is_register = true;
            ea.reg_ptr = &cpu->a[reg];
            break;

        case AM_ADDR_IND:
            ea.addr = cpu->a[reg];
            break;

        case AM_POST_INC:
            ea.addr = cpu->a[reg];
            /* Byte access to A7 rounds up to keep stack aligned */
            if (size == SIZE_BYTE && reg == 7)
                cpu->a[reg] += 2;
            else
                cpu->a[reg] += (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            break;

        case AM_PRE_DEC:
            if (size == SIZE_BYTE && reg == 7)
                cpu->a[reg] -= 2;
            else
                cpu->a[reg] -= (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            ea.addr = cpu->a[reg];
            break;

        case AM_DISP: {
            int16_t disp = (int16_t)fetch16(cpu);
            ea.addr = cpu->a[reg] + disp;
            break;
        }

        case AM_INDEX: {
            uint16_t ext = fetch16(cpu);
            int xreg = (ext >> 12) & 7;
            bool is_addr = (ext & 0x8000) != 0;
            bool is_long = (ext & 0x0800) != 0;
            int8_t disp = (int8_t)(ext & 0xFF);
            int32_t xval;
            if (is_addr)
                xval = is_long ? (int32_t)cpu->a[xreg] : sign_extend_16((uint16_t)cpu->a[xreg]);
            else
                xval = is_long ? (int32_t)cpu->d[xreg] : sign_extend_16((uint16_t)cpu->d[xreg]);
            ea.addr = cpu->a[reg] + disp + xval;
            break;
        }

        case AM_OTHER:
            switch (reg) {
                case 0: /* Absolute short */
                    ea.addr = (uint32_t)sign_extend_16(fetch16(cpu));
                    break;
                case 1: /* Absolute long */
                    ea.addr = fetch32(cpu);
                    break;
                case 2: { /* d16(PC) */
                    uint32_t base = cpu->pc;
                    int16_t disp = (int16_t)fetch16(cpu);
                    ea.addr = base + disp;
                    break;
                }
                case 3: { /* d8(PC,Xn) */
                    uint32_t base = cpu->pc;
                    uint16_t ext = fetch16(cpu);
                    int xreg = (ext >> 12) & 7;
                    bool is_addr = (ext & 0x8000) != 0;
                    bool is_long = (ext & 0x0800) != 0;
                    int8_t disp = (int8_t)(ext & 0xFF);
                    int32_t xval;
                    if (is_addr)
                        xval = is_long ? (int32_t)cpu->a[xreg] : sign_extend_16((uint16_t)cpu->a[xreg]);
                    else
                        xval = is_long ? (int32_t)cpu->d[xreg] : sign_extend_16((uint16_t)cpu->d[xreg]);
                    ea.addr = base + disp + xval;
                    break;
                }
                case 4: /* Immediate */
                    ea.is_register = false;
                    if (size == SIZE_BYTE) {
                        ea.addr = cpu->pc + 1; /* low byte of word */
                        cpu->pc += 2;
                    } else if (size == SIZE_WORD) {
                        ea.addr = cpu->pc;
                        cpu->pc += 2;
                    } else {
                        ea.addr = cpu->pc;
                        cpu->pc += 4;
                    }
                    break;
            }
            break;
    }

    return ea;
}

/* Read value from effective address */
static uint32_t read_ea(m68k_t *cpu, ea_result_t *ea, int size) {
    if (ea->is_register) {
        switch (size) {
            case SIZE_BYTE: return *ea->reg_ptr & 0xFF;
            case SIZE_WORD: return *ea->reg_ptr & 0xFFFF;
            case SIZE_LONG: return *ea->reg_ptr;
        }
    }
    switch (size) {
        case SIZE_BYTE: return cpu_read8(cpu, ea->addr);
        case SIZE_WORD: return cpu_read16(cpu, ea->addr);
        case SIZE_LONG: return cpu_read32(cpu, ea->addr);
    }
    return 0;
}

/* Write value to effective address */
static void write_ea(m68k_t *cpu, ea_result_t *ea, int size, uint64_t val64) {
    uint32_t val = (uint32_t)val64;
    if (ea->is_register) {
        switch (size) {
            case SIZE_BYTE:
                *ea->reg_ptr = (*ea->reg_ptr & 0xFFFFFF00) | (val & 0xFF);
                break;
            case SIZE_WORD:
                *ea->reg_ptr = (*ea->reg_ptr & 0xFFFF0000) | (val & 0xFFFF);
                break;
            case SIZE_LONG:
                *ea->reg_ptr = val;
                break;
        }
        return;
    }
    switch (size) {
        case SIZE_BYTE: cpu_write8(cpu, ea->addr, val & 0xFF); break;
        case SIZE_WORD: cpu_write16(cpu, ea->addr, val & 0xFFFF); break;
        case SIZE_LONG: cpu_write32(cpu, ea->addr, val); break;
    }
}

/* Quick EA read helpers */
static uint32_t read_ea_mode(m68k_t *cpu, int mode, int reg, int size) {
    ea_result_t ea = calc_ea(cpu, mode, reg, size);
    return read_ea(cpu, &ea, size);
}

/* ========================================================================
 * Exception processing
 * ======================================================================== */

static int exception_count = 0;
static int exception_count_gen = 0;
static uint32_t pascalinit_addr = 0;
static int pascalinit_addr_gen = 0;  /* gen vars for file-scope statics — reset in take_exception */
static int illegal_opcode_histogram[16] = {0};
uint32_t g_last_cpu_pc = 0;  /* Visible to lisa_mmu.c for write watchpoints */

int g_emu_generation = 0;
int g_vec_guard_active = 0;  /* P78d: set after SYS_PROC_INIT to protect vector table */
int m68k_exception_histogram[256] = {0};
#define exception_histogram m68k_exception_histogram

/* TRAP #5 selector histogram: D7 value at trap entry == routine number.
 * Lisa driver Trap5 reads TrapTable(D7.W) so only the low word matters; we
 * bucket by (D7 & 0xFF) since routines are indexed 0..~128 in word stride. */
int m68k_trap5_selector_histogram[256] = {0};

static void take_exception(m68k_t *cpu, int vector) {
    /* Reset file-scope debug statics on power cycle */
    if (exception_count_gen != g_emu_generation) {
        exception_count = 0; exception_count_gen = g_emu_generation;
        pascalinit_addr = 0; pascalinit_addr_gen = g_emu_generation;
        memset(m68k_exception_histogram, 0, sizeof(m68k_exception_histogram));
        memset(m68k_trap5_selector_histogram, 0, sizeof(m68k_trap5_selector_histogram));
        memset(illegal_opcode_histogram, 0, sizeof(illegal_opcode_histogram));
    }

    /* Per-vector first-fire trace: fires exactly once per vector number, so
     * we can see in order which exceptions are hit during boot. The first
     * non-trivial vector to fire on native-vs-sandbox divergence is the
     * signal we're hunting. */
    {
        static bool ff_seen[256];
        static int ff_gen = 0;
        if (ff_gen != g_emu_generation) { memset(ff_seen, 0, sizeof(ff_seen)); ff_gen = g_emu_generation; }
        if (vector >= 0 && vector < 256 && !ff_seen[vector]) {
            ff_seen[vector] = true;
            uint32_t handler = cpu_read32(cpu, (uint32_t)vector * 4);
            fprintf(stderr,
                "[VEC-FIRST] v=%d PC=$%06X SR=$%04X SSP=$%08X USP=$%08X handler=$%08X\n",
                vector, cpu->pc, cpu->sr, cpu->a[7], cpu->usp, handler);
        }
    }

    /* Count all exceptions by type */
    if (vector < 256) exception_histogram[vector]++;
    if (vector == 37) {
        uint32_t sel = cpu->d[7] & 0xFF;
        m68k_trap5_selector_histogram[sel]++;
    }

    /* Detect stack overflow — allow mapped stack addresses ($CA0000+ and $F60000+) */
    if (cpu->a[7] < 0x1000 || (cpu->a[7] > 0x1FF000 && cpu->a[7] < 0xCA0000)) {
        DBGSTATIC(int, overflow_reported, 0);
        if (!overflow_reported) {
            overflow_reported = 1;
            fprintf(stderr, "STACK OVERFLOW: A7=$%08X at PC=$%06X, vector=%d\n",
                    cpu->a[7], cpu->pc, vector);
            fprintf(stderr, "Exception counts: ");
            for (int i = 0; i < 64; i++) {
                if (exception_histogram[i] > 0)
                    fprintf(stderr, "v%d=%d ", i, exception_histogram[i]);
            }
            fprintf(stderr, "\n");
        }
    }

    /* Detect crash at PC=$2A — dump last PCs to understand the jump */
    if (vector == 4 && (cpu->pc & 0xFFFFFF) < 0x100) {
        DBGSTATIC(int, crash_traced, 0);
        if (crash_traced++ < 1) {
            /* Print total trap counts */
            extern int g_trap6_total;
            fprintf(stderr, "=== CRASH at PC=$%06X (vector %d) TRAP6_TOTAL=%d ===\n",
                    cpu->pc, vector, g_trap6_total);
            fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                    cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
            fprintf(stderr, "  A0=$%08X A1=$%08X A5=$%08X A6=$%08X SP=$%08X SR=$%04X\n",
                    cpu->a[0], cpu->a[1], cpu->a[5], cpu->a[6], cpu->a[7], cpu->sr);
            /* pc_ring is in the main loop scope — can't access from here.
             * Use the stack trace instead. */
            fprintf(stderr, "  Stack: ");
            for (int j = 0; j < 32; j += 4)
                fprintf(stderr, "$%08X ", cpu_read32(cpu, (cpu->a[7] + j) & 0xFFFFFF));
            fprintf(stderr, "\n");
        }
    }

    /* Count TRAP #6 calls */
    if (vector == 38) {
        DBGSTATIC(int, trap6_count, 0);
        trap6_count++;
        if (trap6_count <= 5) {
            fprintf(stderr, "TRAP6[%d] at PC=$%06X: D0-7=$%08X $%08X $%08X $%08X $%08X $%08X $%08X $%08X\n",
                    trap6_count, cpu->pc,
                    cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3],
                    cpu->d[4], cpu->d[5], cpu->d[6], cpu->d[7]);
        }
    }

    /* Trace exceptions for debugging */
    if (exception_count < 10) {
        static const char *vec_names[] = {
            [2] = "Bus Error", [3] = "Address Error", [4] = "Illegal Instruction",
            [5] = "Zero Divide", [6] = "CHK", [7] = "TRAPV",
            [8] = "Privilege Violation", [9] = "Trace",
            [10] = "Line-A", [11] = "Line-F"
        };
        const char *name = (vector < 12 && vec_names[vector]) ? vec_names[vector] :
                           (vector >= 32 && vector < 48) ? "TRAP" : "IRQ";
        printf("Exception: vector %d (%s) at PC=$%06X, new PC=$%08X\n",
               vector, name, cpu->pc, cpu_read32(cpu, vector * 4));
        /* Extra detail for Zero Divide */
        if (vector == 5) {
            fprintf(stderr, "  ZERO DIVIDE at PC=$%06X: opcode=$%04X D0=$%08X D1=$%08X D2=$%08X\n",
                    cpu->pc, cpu_read16(cpu, cpu->pc),
                    cpu->d[0], cpu->d[1], cpu->d[2]);
            fprintf(stderr, "    A6=$%08X SP=$%08X A5=$%08X\n",
                    cpu->a[6], cpu->a[7], cpu->a[5]);
            /* Dump code around PC */
            fprintf(stderr, "    Code at PC-16..+8: ");
            for (uint32_t a = (cpu->pc > 16 ? cpu->pc - 16 : 0); a < cpu->pc + 8; a += 2)
                fprintf(stderr, "%04X ", cpu_read16(cpu, a));
            fprintf(stderr, "\n");
        }

        exception_count++;
    }

    /* Save old SR, enter supervisor mode */
    uint16_t old_sr = cpu->sr;
    set_supervisor(cpu, true);
    cpu->sr &= ~SR_TRACE;

    /* Trap-frame probe: log the PC we're pushing for vectors 37/39
     * (post-SYSTEM_ERROR(10201) crash hunt — a bogus $615262DC return
     * address ends up on the stack; want to see whether a trap entry
     * is the culprit). Also log when the pushed PC has a non-zero
     * high byte — those are always wrong on the Lisa (24-bit bus). */
    if (vector == 37 || vector == 39 || (cpu->pc & 0xFF000000)) {
        DBGSTATIC(int, tf_count, 0);
        if (tf_count++ < 32) {
            fprintf(stderr,
                "TRAP-FRAME[v%d #%d]: push PC=$%08X SR=$%04X  SSP_before=$%08X  A6=$%08X\n",
                vector, tf_count, cpu->pc, old_sr, cpu->a[7], cpu->a[6]);
        }
    }

    /* Push PC and SR */
    push32(cpu, cpu->pc);
    push16(cpu, old_sr);

    /* Read new PC from vector table */
    uint32_t handler = cpu_read32(cpu, vector * 4);

    /* Guard against recursive Line-F/Line-A exceptions: if the handler
     * itself starts with a Line-F/Line-A opcode, it would loop forever.
     * This happens when INIT_NMI_TRAPV installs an OS handler with a
     * bad address (unresolved symbol). Use the ROM skip handler instead. */
    if ((vector == 10 || vector == 11) && handler > 0 && handler < 0xFE0000) {
        uint16_t handler_op = cpu_read16(cpu, handler & 0xFFFFFF);
        if ((handler_op & 0xF000) == 0xA000 || (handler_op & 0xF000) == 0xF000) {
            /* Handler starts with Line-A/F opcode — would recurse. Skip. */
            handler = (vector == 10) ? 0x00FE0320 : 0x00FE0310;
            DBGSTATIC(int, recurse_warned, 0);
            if (recurse_warned++ < 3)
                fprintf(stderr, "WARNING: Line-%c handler at $%06X starts with $%04X (would recurse), using ROM handler\n",
                        vector == 10 ? 'A' : 'F',
                        cpu_read32(cpu, vector * 4) & 0xFFFFFF, handler_op);
        }
    }

    cpu->pc = handler;
    cpu->cycles += 34;
}

static void privilege_violation(m68k_t *cpu) {
    take_exception(cpu, VEC_PRIVILEGE);
}

static void illegal_instruction(m68k_t *cpu) {
    /* Track which opcode groups trigger illegal instruction */
    int group = (cpu->ir >> 12) & 0xF;
    if (illegal_opcode_histogram[group]++ < 5) {
        uint32_t pc = cpu->pc - 2;
        fprintf(stderr, "ILLEGAL: opcode=$%04X group=%X at PC=$%06X\n",
                cpu->ir, group, pc);
        /* P113 diag: dump 16 bytes around PC and 32 bytes at SP.
         * Classifies stack-return-to-garbage vs. bad-relocation. */
        fprintf(stderr, "  bytes @PC-8..PC+8:");
        for (int i = -8; i < 8; i++)
            fprintf(stderr, " %02X", cpu->read8((pc + i) & 0xFFFFFF));
        fprintf(stderr, "\n  stack @SP..SP+32:");
        for (int i = 0; i < 32; i++)
            fprintf(stderr, " %02X", cpu->read8((cpu->a[7] + i) & 0xFFFFFF));
        fprintf(stderr, "\n  D0=$%08X D1=$%08X A0=$%08X A1=$%08X A6=$%08X A7=$%08X\n",
                cpu->d[0], cpu->d[1], cpu->a[0], cpu->a[1], cpu->a[6], cpu->a[7]);
    }
    /* Per M68000 PRM: illegal-instruction exceptions push the PC of the
     * faulting instruction itself, not PC+2. Rewind so the handler sees
     * the opcode address on its stack frame. Lisa OS uses $4FBC as a
     * custom exception-driven opcode — its handler reads the instruction
     * at the stacked PC to decode the operation, so stacking PC+2 would
     * make it read the operand word instead of the opcode. */
    cpu->pc -= 2;
    take_exception(cpu, VEC_ILLEGAL_INST);
}

/* ========================================================================
 * Instruction implementations
 * ======================================================================== */

/* --- MOVE (and MOVEA) --- */
static void op_move(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size_enc = (op >> 12) & 3;
    int size;
    switch (size_enc) {
        case 1: size = SIZE_BYTE; break;
        case 3: size = SIZE_WORD; break;
        case 2: size = SIZE_LONG; break;
        default: illegal_instruction(cpu); return;
    }

    int src_mode = (op >> 3) & 7;
    int src_reg = op & 7;
    int dst_reg = (op >> 9) & 7;
    int dst_mode = (op >> 6) & 7;

    /* Read source */
    ea_result_t src_ea = calc_ea(cpu, src_mode, src_reg, size);
    uint32_t val = read_ea(cpu, &src_ea, size);

    /* MOVEA doesn't set condition codes and always writes full register */
    if (dst_mode == AM_ADDR_REG) {
        if (size == SIZE_WORD)
            cpu->a[dst_reg] = sign_extend_16(val & 0xFFFF);
        else
            cpu->a[dst_reg] = val;
        cpu->cycles += 4;
        return;
    }

    /* Write destination */
    ea_result_t dst_ea = calc_ea(cpu, dst_mode, dst_reg, size);
    write_ea(cpu, &dst_ea, size, val);

    /* Set condition codes */
    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, val); break;
        case SIZE_WORD: set_logic_flags_16(cpu, val); break;
        case SIZE_LONG: set_logic_flags_32(cpu, val); break;
    }
    cpu->cycles += 4;
}

/* --- ADD/ADDA/ADDX --- */
static void op_add(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int dir = (op >> 8) & 1;  /* 0 = Dn := Dn + EA, 1 = EA := EA + Dn */
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) {
        /* ADDA */
        int asize = (op & 0x0100) ? SIZE_LONG : SIZE_WORD;
        uint32_t src = read_ea_mode(cpu, ea_mode, ea_reg, asize);
        if (asize == SIZE_WORD)
            src = (uint32_t)sign_extend_16(src);
        cpu->a[reg] += src;
        cpu->cycles += (asize == SIZE_LONG) ? 8 : 8;
        return;
    }

    /* Check for ADDX (RM bit = 1, ea_mode = 0 or 1) */
    if (dir && (ea_mode == 0 || ea_mode == 1)) {
        /* ADDX */
        uint32_t src, dst;
        int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;
        if (ea_mode == 0) {
            /* Data register */
            src = cpu->d[ea_reg];
            dst = cpu->d[reg];
        } else {
            /* Memory: -(Ay), -(Ax) */
            int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            if (size == SIZE_BYTE && ea_reg == 7) inc = 2;
            if (size == SIZE_BYTE && reg == 7) inc = 2;
            cpu->a[ea_reg] -= inc;
            src = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[ea_reg]) :
                  (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[ea_reg]) :
                  cpu_read32(cpu, cpu->a[ea_reg]);
            cpu->a[reg] -= inc;
            dst = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[reg]) :
                  (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[reg]) :
                  cpu_read32(cpu, cpu->a[reg]);
        }

        uint64_t result = (uint64_t)dst + (uint64_t)src + x;
        uint32_t r;
        switch (size) {
            case SIZE_BYTE:
                r = result & 0xFF;
                set_add_flags_8(cpu, src, dst, (uint16_t)result);
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | r;
                else cpu_write8(cpu, cpu->a[reg], r);
                break;
            case SIZE_WORD:
                r = result & 0xFFFF;
                set_add_flags_16(cpu, src, dst, (uint32_t)result);
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | r;
                else cpu_write16(cpu, cpu->a[reg], r);
                break;
            case SIZE_LONG:
                r = (uint32_t)result;
                set_add_flags_32(cpu, src, dst, result);
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = r;
                else cpu_write32(cpu, cpu->a[reg], r);
                break;
        }
        cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
        return;
    }

    /* Regular ADD */
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t ea_val = read_ea(cpu, &ea, size);
    uint32_t dn_val;
    switch (size) {
        case SIZE_BYTE: dn_val = cpu->d[reg] & 0xFF; break;
        case SIZE_WORD: dn_val = cpu->d[reg] & 0xFFFF; break;
        default: dn_val = cpu->d[reg]; break;
    }

    if (dir == 0) {
        /* Dn := Dn + <ea> */
        uint64_t result = (uint64_t)dn_val + (uint64_t)ea_val;
        uint32_t r = (uint32_t)result;
        switch (size) {
            case SIZE_BYTE:
                set_add_flags_8(cpu, ea_val, dn_val, (uint16_t)result);
                cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (r & 0xFF);
                break;
            case SIZE_WORD:
                set_add_flags_16(cpu, ea_val, dn_val, (uint32_t)result);
                cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (r & 0xFFFF);
                break;
            case SIZE_LONG:
                set_add_flags_32(cpu, ea_val, dn_val, result);
                cpu->d[reg] = r;
                break;
        }
    } else {
        /* <ea> := <ea> + Dn */
        uint64_t result = (uint64_t)ea_val + (uint64_t)dn_val;
        uint32_t r = (uint32_t)result;
        switch (size) {
            case SIZE_BYTE:
                set_add_flags_8(cpu, dn_val, ea_val, (uint16_t)result);
                write_ea(cpu, &ea, size, r);
                break;
            case SIZE_WORD:
                set_add_flags_16(cpu, dn_val, ea_val, (uint32_t)result);
                write_ea(cpu, &ea, size, r);
                break;
            case SIZE_LONG:
                set_add_flags_32(cpu, dn_val, ea_val, result);
                write_ea(cpu, &ea, size, r);
                break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- SUB/SUBA/SUBX --- */
static void op_sub(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int dir = (op >> 8) & 1;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) {
        /* SUBA */
        int asize = (op & 0x0100) ? SIZE_LONG : SIZE_WORD;
        uint32_t src = read_ea_mode(cpu, ea_mode, ea_reg, asize);
        if (asize == SIZE_WORD)
            src = (uint32_t)sign_extend_16(src);
        cpu->a[reg] -= src;
        cpu->cycles += 8;
        return;
    }

    /* SUBX */
    if (dir && (ea_mode == 0 || ea_mode == 1)) {
        uint32_t src, dst;
        int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;
        if (ea_mode == 0) {
            src = cpu->d[ea_reg];
            dst = cpu->d[reg];
        } else {
            int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
            if (size == SIZE_BYTE && ea_reg == 7) inc = 2;
            if (size == SIZE_BYTE && reg == 7) inc = 2;
            cpu->a[ea_reg] -= inc;
            src = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[ea_reg]) :
                  (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[ea_reg]) :
                  cpu_read32(cpu, cpu->a[ea_reg]);
            cpu->a[reg] -= inc;
            dst = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[reg]) :
                  (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[reg]) :
                  cpu_read32(cpu, cpu->a[reg]);
        }

        uint64_t result = (uint64_t)dst - (uint64_t)src - x;
        uint32_t r;
        switch (size) {
            case SIZE_BYTE:
                r = result & 0xFF;
                set_sub_flags_8(cpu, src + x, dst, (uint16_t)(dst - src - x));
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | r;
                else cpu_write8(cpu, cpu->a[reg], r);
                break;
            case SIZE_WORD:
                r = result & 0xFFFF;
                set_sub_flags_16(cpu, src + x, dst, (uint32_t)(dst - src - x));
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | r;
                else cpu_write16(cpu, cpu->a[reg], r);
                break;
            case SIZE_LONG:
                r = (uint32_t)result;
                set_sub_flags_32(cpu, src + x, dst, result);
                if (r != 0) set_flag(cpu, SR_ZERO, false);
                if (ea_mode == 0) cpu->d[reg] = r;
                else cpu_write32(cpu, cpu->a[reg], r);
                break;
        }
        cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
        return;
    }

    /* Regular SUB */
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t ea_val = read_ea(cpu, &ea, size);
    uint32_t dn_val;
    switch (size) {
        case SIZE_BYTE: dn_val = cpu->d[reg] & 0xFF; break;
        case SIZE_WORD: dn_val = cpu->d[reg] & 0xFFFF; break;
        default: dn_val = cpu->d[reg]; break;
    }

    if (dir == 0) {
        /* Dn := Dn - <ea> */
        switch (size) {
            case SIZE_BYTE: {
                uint16_t result = (uint16_t)dn_val - (uint16_t)ea_val;
                set_sub_flags_8(cpu, ea_val, dn_val, result);
                cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (result & 0xFF);
                break;
            }
            case SIZE_WORD: {
                uint32_t result = (uint32_t)dn_val - (uint32_t)ea_val;
                set_sub_flags_16(cpu, ea_val, dn_val, result);
                cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (result & 0xFFFF);
                break;
            }
            case SIZE_LONG: {
                uint64_t result = (uint64_t)dn_val - (uint64_t)ea_val;
                set_sub_flags_32(cpu, ea_val, dn_val, result);
                cpu->d[reg] = (uint32_t)result;
                break;
            }
        }
    } else {
        /* <ea> := <ea> - Dn */
        switch (size) {
            case SIZE_BYTE: {
                uint16_t result = (uint16_t)ea_val - (uint16_t)dn_val;
                set_sub_flags_8(cpu, dn_val, ea_val, result);
                write_ea(cpu, &ea, size, result);
                break;
            }
            case SIZE_WORD: {
                uint32_t result = (uint32_t)ea_val - (uint32_t)dn_val;
                set_sub_flags_16(cpu, dn_val, ea_val, result);
                write_ea(cpu, &ea, size, result);
                break;
            }
            case SIZE_LONG: {
                uint64_t result = (uint64_t)ea_val - (uint64_t)dn_val;
                set_sub_flags_32(cpu, dn_val, ea_val, result);
                write_ea(cpu, &ea, size, result);
                break;
            }
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- CMP/CMPA/CMPM --- */
static void op_cmp(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) {
        /* CMPA */
        int asize = (op & 0x0100) ? SIZE_LONG : SIZE_WORD;
        uint32_t src = read_ea_mode(cpu, ea_mode, ea_reg, asize);
        if (asize == SIZE_WORD)
            src = (uint32_t)sign_extend_16(src);
        set_cmp_flags_32(cpu, src, cpu->a[reg]);
        cpu->cycles += 6;
        return;
    }

    /* CMPM */
    if ((op & 0x0138) == 0x0108) {
        uint32_t src, dst;
        int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
        if (size == SIZE_BYTE && ea_reg == 7) inc = 2;
        if (size == SIZE_BYTE && reg == 7) inc = 2;

        src = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[ea_reg]) :
              (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[ea_reg]) :
              cpu_read32(cpu, cpu->a[ea_reg]);
        cpu->a[ea_reg] += inc;

        dst = (size == SIZE_BYTE) ? cpu_read8(cpu, cpu->a[reg]) :
              (size == SIZE_WORD) ? cpu_read16(cpu, cpu->a[reg]) :
              cpu_read32(cpu, cpu->a[reg]);
        cpu->a[reg] += inc;

        switch (size) {
            case SIZE_BYTE: set_cmp_flags_8(cpu, src, dst); break;
            case SIZE_WORD: set_cmp_flags_16(cpu, src, dst); break;
            case SIZE_LONG: set_cmp_flags_32(cpu, src, dst); break;
        }
        cpu->cycles += (size == SIZE_LONG) ? 20 : 12;
        return;
    }

    /* Regular CMP: Dn - <ea> */
    uint32_t src = read_ea_mode(cpu, ea_mode, ea_reg, size);
    switch (size) {
        case SIZE_BYTE: set_cmp_flags_8(cpu, src, cpu->d[reg] & 0xFF); break;
        case SIZE_WORD: set_cmp_flags_16(cpu, src, cpu->d[reg] & 0xFFFF); break;
        case SIZE_LONG: set_cmp_flags_32(cpu, src, cpu->d[reg]); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 6 : 4;
}

/* --- AND --- */
static void op_and(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int dir = (op >> 8) & 1;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) {
        /* MULU/MULS handled elsewhere */
        return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t ea_val = read_ea(cpu, &ea, size);
    uint32_t dn_val = cpu->d[reg];

    uint32_t result = dn_val & ea_val;

    if (dir == 0) {
        /* Dn := Dn AND <ea> */
        switch (size) {
            case SIZE_BYTE:
                cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (result & 0xFF);
                set_logic_flags_8(cpu, result);
                break;
            case SIZE_WORD:
                cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (result & 0xFFFF);
                set_logic_flags_16(cpu, result);
                break;
            case SIZE_LONG:
                cpu->d[reg] = result;
                set_logic_flags_32(cpu, result);
                break;
        }
    } else {
        /* <ea> := <ea> AND Dn */
        write_ea(cpu, &ea, size, result);
        switch (size) {
            case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
            case SIZE_WORD: set_logic_flags_16(cpu, result); break;
            case SIZE_LONG: set_logic_flags_32(cpu, result); break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- OR --- */
static void op_or(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int dir = (op >> 8) & 1;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) {
        /* DIVU/DIVS handled elsewhere */
        return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t ea_val = read_ea(cpu, &ea, size);
    uint32_t dn_val = cpu->d[reg];

    uint32_t result = dn_val | ea_val;

    if (dir == 0) {
        switch (size) {
            case SIZE_BYTE:
                cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (result & 0xFF);
                set_logic_flags_8(cpu, result);
                break;
            case SIZE_WORD:
                cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (result & 0xFFFF);
                set_logic_flags_16(cpu, result);
                break;
            case SIZE_LONG:
                cpu->d[reg] = result;
                set_logic_flags_32(cpu, result);
                break;
        }
    } else {
        write_ea(cpu, &ea, size, result);
        switch (size) {
            case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
            case SIZE_WORD: set_logic_flags_16(cpu, result); break;
            case SIZE_LONG: set_logic_flags_32(cpu, result); break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- EOR --- */
static void op_eor(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (size == 3) return; /* CMPA handled in op_cmp */

    /* Check for CMPM */
    if ((op & 0x0138) == 0x0108) {
        op_cmp(cpu);
        return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t ea_val = read_ea(cpu, &ea, size);
    uint32_t result = cpu->d[reg] ^ ea_val;

    write_ea(cpu, &ea, size, result);
    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
        case SIZE_WORD: set_logic_flags_16(cpu, result); break;
        case SIZE_LONG: set_logic_flags_32(cpu, result); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- ADDI --- */
static void op_addi(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (val & 0xFF) + (imm & 0xFF);
            set_add_flags_8(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (val & 0xFFFF) + (imm & 0xFFFF);
            set_add_flags_16(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)val + (uint64_t)imm;
            set_add_flags_32(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 16 : 8;
}

/* --- SUBI --- */
static void op_subi(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (val & 0xFF) - (imm & 0xFF);
            set_sub_flags_8(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (val & 0xFFFF) - (imm & 0xFFFF);
            set_sub_flags_16(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)val - (uint64_t)imm;
            set_sub_flags_32(cpu, imm, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 16 : 8;
}

/* --- CMPI --- */
static void op_cmpi(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    uint32_t val = read_ea_mode(cpu, ea_mode, ea_reg, size);

    switch (size) {
        case SIZE_BYTE: set_cmp_flags_8(cpu, imm, val); break;
        case SIZE_WORD: set_cmp_flags_16(cpu, imm, val); break;
        case SIZE_LONG: set_cmp_flags_32(cpu, imm, val); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 14 : 8;
}

/* --- ANDI --- */
static void op_andi(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    /* ANDI to CCR */
    if (op == 0x023C) {
        uint16_t imm = fetch16(cpu);
        cpu->sr = (cpu->sr & 0xFF00) | ((cpu->sr & 0xFF) & (imm & 0xFF));
        cpu->cycles += 20;
        return;
    }
    /* ANDI to SR */
    if (op == 0x027C) {
        if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
        uint16_t imm = fetch16(cpu);
        uint16_t new_sr = cpu->sr & imm;
        if ((cpu->sr & SR_SUPERVISOR) && !(new_sr & SR_SUPERVISOR)) {
            cpu->ssp = cpu->a[7];
            cpu->a[7] = cpu->usp;
        }
        cpu->sr = new_sr;
        cpu->cycles += 20;
        return;
    }

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);
    uint32_t result = val & imm;
    write_ea(cpu, &ea, size, result);

    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
        case SIZE_WORD: set_logic_flags_16(cpu, result); break;
        case SIZE_LONG: set_logic_flags_32(cpu, result); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 16 : 8;
}

/* --- ORI --- */
static void op_ori(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    /* ORI to CCR */
    if (op == 0x003C) {
        uint16_t imm = fetch16(cpu);
        cpu->sr |= (imm & 0x1F);
        cpu->cycles += 20;
        return;
    }
    /* ORI to SR */
    if (op == 0x007C) {
        if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
        uint16_t imm = fetch16(cpu);
        uint16_t new_sr = cpu->sr | imm;
        if (!(cpu->sr & SR_SUPERVISOR) && (new_sr & SR_SUPERVISOR)) {
            cpu->usp = cpu->a[7];
            cpu->a[7] = cpu->ssp;
        }
        cpu->sr = new_sr;
        cpu->cycles += 20;
        return;
    }

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);
    uint32_t result = val | imm;
    write_ea(cpu, &ea, size, result);

    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
        case SIZE_WORD: set_logic_flags_16(cpu, result); break;
        case SIZE_LONG: set_logic_flags_32(cpu, result); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 16 : 8;
}

/* --- EORI --- */
static void op_eori(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    /* EORI to CCR */
    if (op == 0x0A3C) {
        uint16_t imm = fetch16(cpu);
        cpu->sr ^= (imm & 0x1F);
        cpu->cycles += 20;
        return;
    }
    /* EORI to SR */
    if (op == 0x0A7C) {
        if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
        uint16_t imm = fetch16(cpu);
        uint16_t old_sr = cpu->sr;
        cpu->sr ^= imm;
        if ((old_sr & SR_SUPERVISOR) && !(cpu->sr & SR_SUPERVISOR)) {
            cpu->ssp = cpu->a[7];
            cpu->a[7] = cpu->usp;
        } else if (!(old_sr & SR_SUPERVISOR) && (cpu->sr & SR_SUPERVISOR)) {
            cpu->usp = cpu->a[7];
            cpu->a[7] = cpu->ssp;
        }
        cpu->cycles += 20;
        return;
    }

    uint32_t imm;
    switch (size) {
        case SIZE_BYTE: imm = fetch16(cpu) & 0xFF; break;
        case SIZE_WORD: imm = fetch16(cpu); break;
        case SIZE_LONG: imm = fetch32(cpu); break;
        default: illegal_instruction(cpu); return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);
    uint32_t result = val ^ imm;
    write_ea(cpu, &ea, size, result);

    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
        case SIZE_WORD: set_logic_flags_16(cpu, result); break;
        case SIZE_LONG: set_logic_flags_32(cpu, result); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 16 : 8;
}

/* --- ADDQ --- */
static void op_addq(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int data = (op >> 9) & 7;
    if (data == 0) data = 8;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (ea_mode == AM_ADDR_REG) {
        /* ADDQ to An - no flags affected, always long */
        cpu->a[ea_reg] += data;
        cpu->cycles += 8;
        return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (val & 0xFF) + data;
            set_add_flags_8(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (val & 0xFFFF) + data;
            set_add_flags_16(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)val + data;
            set_add_flags_32(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- SUBQ --- */
static void op_subq(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int data = (op >> 9) & 7;
    if (data == 0) data = 8;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    if (ea_mode == AM_ADDR_REG) {
        cpu->a[ea_reg] -= data;
        cpu->cycles += 8;
        return;
    }

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (val & 0xFF) - data;
            set_sub_flags_8(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (val & 0xFFFF) - data;
            set_sub_flags_16(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)val - data;
            set_sub_flags_32(cpu, data, val, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 : 4;
}

/* --- MOVEQ --- */
static void op_moveq(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int8_t data = (int8_t)(op & 0xFF);
    cpu->d[reg] = (uint32_t)(int32_t)data;
    set_logic_flags_32(cpu, cpu->d[reg]);
    cpu->cycles += 4;
}

/* --- CLR --- */
static void op_clr(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    write_ea(cpu, &ea, size, 0);

    set_flag(cpu, SR_NEGATIVE, false);
    set_flag(cpu, SR_ZERO, true);
    set_flag(cpu, SR_OVERFLOW, false);
    set_flag(cpu, SR_CARRY, false);
    cpu->cycles += (size == SIZE_LONG) ? 6 : 4;
}

/* --- NEG --- */
static void op_neg(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (uint16_t)(0 - (val & 0xFF));
            set_sub_flags_8(cpu, val, 0, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (uint32_t)(0 - (val & 0xFFFF));
            set_sub_flags_16(cpu, val, 0, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)(0 - (uint64_t)val);
            set_sub_flags_32(cpu, val, 0, result);
            write_ea(cpu, &ea, size, result);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 6 : 4;
}

/* --- NEGX --- */
static void op_negx(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);
    int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;

    switch (size) {
        case SIZE_BYTE: {
            uint16_t result = (uint16_t)(0 - (val & 0xFF) - x);
            uint8_t r = result & 0xFF;
            set_sub_flags_8(cpu, val + x, 0, result);
            if (r != 0) set_flag(cpu, SR_ZERO, false);
            write_ea(cpu, &ea, size, r);
            break;
        }
        case SIZE_WORD: {
            uint32_t result = (uint32_t)(0 - (val & 0xFFFF) - x);
            uint16_t r = result & 0xFFFF;
            set_sub_flags_16(cpu, val + x, 0, result);
            if (r != 0) set_flag(cpu, SR_ZERO, false);
            write_ea(cpu, &ea, size, r);
            break;
        }
        case SIZE_LONG: {
            uint64_t result = (uint64_t)(0 - (uint64_t)val - x);
            uint32_t r = (uint32_t)result;
            set_sub_flags_32(cpu, val + x, 0, result);
            if (r != 0) set_flag(cpu, SR_ZERO, false);
            write_ea(cpu, &ea, size, r);
            break;
        }
    }
    cpu->cycles += (size == SIZE_LONG) ? 6 : 4;
}

/* --- NOT --- */
static void op_not(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
    uint32_t val = read_ea(cpu, &ea, size);
    uint32_t result = ~val;
    write_ea(cpu, &ea, size, result);

    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
        case SIZE_WORD: set_logic_flags_16(cpu, result); break;
        case SIZE_LONG: set_logic_flags_32(cpu, result); break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 6 : 4;
}

/* --- TST --- */
static void op_tst(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int size = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint32_t val = read_ea_mode(cpu, ea_mode, ea_reg, size);
    switch (size) {
        case SIZE_BYTE: set_logic_flags_8(cpu, val); break;
        case SIZE_WORD: set_logic_flags_16(cpu, val); break;
        case SIZE_LONG: set_logic_flags_32(cpu, val); break;
    }
    cpu->cycles += 4;
}

/* --- EXT --- */
static void op_ext(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = op & 7;

    if (op & 0x0040) {
        /* EXT.L - word to long */
        cpu->d[reg] = (uint32_t)sign_extend_16(cpu->d[reg] & 0xFFFF);
        set_logic_flags_32(cpu, cpu->d[reg]);
    } else {
        /* EXT.W - byte to word */
        uint16_t result = (uint16_t)(int16_t)(int8_t)(cpu->d[reg] & 0xFF);
        cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | result;
        set_logic_flags_16(cpu, result);
    }
    cpu->cycles += 4;
}

/* --- SWAP --- */
static void op_swap(m68k_t *cpu) {
    int reg = cpu->ir & 7;
    cpu->d[reg] = (cpu->d[reg] >> 16) | (cpu->d[reg] << 16);
    set_logic_flags_32(cpu, cpu->d[reg]);
    cpu->cycles += 4;
}

/* --- Bcc/BRA/BSR --- */
static void op_bcc(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int cond = (op >> 8) & 0xF;
    int8_t disp8 = (int8_t)(op & 0xFF);
    uint32_t base = cpu->pc;
    int32_t disp;

    if (disp8 == 0) {
        disp = sign_extend_16(fetch16(cpu));
    } else {
        disp = disp8;
    }

    if (cond == 0) {
        /* BRA */
        uint32_t target = base + disp;
        /* Debug: trace when BRA produces a bad target */
        if ((target & 0xFFFFFF) >= 0x200000 && (target & 0xFFFFFF) < 0xFE0000) {
            DBGSTATIC(int, bra_bad, 0);
            if (bra_bad++ < 3)
                fprintf(stderr, "!!! BAD BRA: from PC=$%06X base=$%06X disp=%d($%04X) → target=$%08X op=$%04X\n",
                        cpu->pc - (disp8 == 0 ? 4 : 2), base, disp,
                        (uint16_t)(disp & 0xFFFF), target, op);
        }
        cpu->pc = target;
        cpu->cycles += 10;
    } else if (cond == 1) {
        /* BSR */
        push32(cpu, cpu->pc);
        cpu->pc = base + disp;
        cpu->cycles += 18;
    } else {
        if (test_condition(cpu, cond)) {
            cpu->pc = base + disp;
            cpu->cycles += 10;
        } else {
            cpu->cycles += (disp8 == 0) ? 12 : 8;
        }
    }
}

/* --- DBcc --- */
static void op_dbcc(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int cond = (op >> 8) & 0xF;
    int reg = op & 7;
    int16_t disp = (int16_t)fetch16(cpu);
    uint32_t base = cpu->pc - 2; /* PC points after displacement */

    if (!test_condition(cpu, cond)) {
        uint16_t counter = (cpu->d[reg] & 0xFFFF) - 1;
        cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | counter;
        if (counter != 0xFFFF) {
            cpu->pc = base + disp;
            cpu->cycles += 10;
        } else {
            cpu->cycles += 14;
        }
    } else {
        cpu->cycles += 12;
    }
}

/* --- Scc --- */
static void op_scc(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int cond = (op >> 8) & 0xF;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
    write_ea(cpu, &ea, SIZE_BYTE, test_condition(cpu, cond) ? 0xFF : 0x00);
    cpu->cycles += 4;
}

/* --- JMP --- */
static void op_jmp(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->pc = ea.addr;
    cpu->cycles += 8;
}

/* --- JSR --- */
static void op_jsr(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    push32(cpu, cpu->pc);
    cpu->pc = ea.addr;
    cpu->cycles += 16;
}

/* --- RTS --- */
static void op_rts(m68k_t *cpu) {
    cpu->pc = pop32(cpu);
    cpu->cycles += 16;
}

/* --- RTE --- */
static void op_rte(m68k_t *cpu) {
    if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
    uint16_t new_sr = pop16(cpu);
    uint32_t new_pc = pop32(cpu);
    /* Trace RTE from DO_AN_MMU */
    {
        DBGSTATIC(int, rte_trace, 0);
        uint32_t old_pc = cpu->pc & 0xFFFFFF;
        if (old_pc >= 0xA84000 && old_pc < 0xA84200 && rte_trace < 5) {
            rte_trace++;
            fprintf(stderr, "RTE[%d] from $%06X: SR=$%04X→$%04X PC→$%08X D1=$%08X D3=$%08X\n",
                    rte_trace, old_pc, cpu->sr, new_sr, new_pc, cpu->d[1], cpu->d[3]);
        }
    }
    cpu->pc = new_pc;
    bool was_super = is_supervisor(cpu);
    cpu->sr = new_sr;
    if (was_super && !(new_sr & SR_SUPERVISOR)) {
        cpu->ssp = cpu->a[7];
        cpu->a[7] = cpu->usp;
    }
    cpu->cycles += 20;
}

/* --- RTR --- */
static void op_rtr(m68k_t *cpu) {
    uint16_t ccr = pop16(cpu);
    cpu->sr = (cpu->sr & 0xFF00) | (ccr & 0xFF);
    cpu->pc = pop32(cpu);
    cpu->cycles += 20;
}

/* --- LINK --- */
static void op_link(m68k_t *cpu) {
    int reg = cpu->ir & 7;
    int16_t disp = (int16_t)fetch16(cpu);
    push32(cpu, cpu->a[reg]);
    cpu->a[reg] = cpu->a[7];
    cpu->a[7] += disp;
    cpu->cycles += 16;
}

/* --- UNLK --- */
static void op_unlk(m68k_t *cpu) {
    int reg = cpu->ir & 7;
    cpu->a[7] = cpu->a[reg];
    cpu->a[reg] = pop32(cpu);
    cpu->cycles += 12;
}

/* --- LEA --- */
static void op_lea(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    cpu->a[reg] = ea.addr;
    cpu->cycles += 4;
}

/* --- PEA --- */
static void op_pea(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_LONG);
    push32(cpu, ea.addr);
    cpu->cycles += 12;
}

/* --- MOVEM --- */
static void op_movem(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int dir = (op >> 10) & 1;  /* 0 = reg-to-mem, 1 = mem-to-reg */
    int size = (op & 0x0040) ? SIZE_LONG : SIZE_WORD;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    uint16_t mask = fetch16(cpu);
    int inc = (size == SIZE_LONG) ? 4 : 2;

    if (dir == 0) {
        /* Register to memory */
        if (ea_mode == AM_PRE_DEC) {
            /* Pre-decrement: registers stored in reverse order A7..A0, D7..D0 */
            for (int i = 15; i >= 0; i--) {
                if (mask & (1 << (15 - i))) {
                    cpu->a[ea_reg] -= inc;
                    uint32_t val = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
                    if (size == SIZE_LONG)
                        cpu_write32(cpu, cpu->a[ea_reg], val);
                    else
                        cpu_write16(cpu, cpu->a[ea_reg], val & 0xFFFF);
                }
            }
        } else {
            ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
            uint32_t addr = ea.addr;
            for (int i = 0; i < 16; i++) {
                if (mask & (1 << i)) {
                    uint32_t val = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
                    if (size == SIZE_LONG)
                        cpu_write32(cpu, addr, val);
                    else
                        cpu_write16(cpu, addr, val & 0xFFFF);
                    addr += inc;
                }
            }
        }
    } else {
        /* Memory to register */
        ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, size);
        uint32_t addr = ea.addr;
        for (int i = 0; i < 16; i++) {
            if (mask & (1 << i)) {
                if (i < 8) {
                    if (size == SIZE_LONG)
                        cpu->d[i] = cpu_read32(cpu, addr);
                    else
                        cpu->d[i] = (uint32_t)sign_extend_16(cpu_read16(cpu, addr));
                } else {
                    if (size == SIZE_LONG)
                        cpu->a[i - 8] = cpu_read32(cpu, addr);
                    else
                        cpu->a[i - 8] = (uint32_t)sign_extend_16(cpu_read16(cpu, addr));
                }
                addr += inc;
            }
        }
        if (ea_mode == AM_POST_INC) {
            cpu->a[ea_reg] = addr;
        }
    }
    cpu->cycles += 12;
}

/* --- MULU/MULS --- */
static void op_mulu(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint16_t src = read_ea_mode(cpu, ea_mode, ea_reg, SIZE_WORD);
    uint16_t dst = cpu->d[reg] & 0xFFFF;

    if (op & 0x0100) {
        /* MULS - signed */
        int32_t result = (int16_t)dst * (int16_t)src;
        cpu->d[reg] = (uint32_t)result;
        set_logic_flags_32(cpu, cpu->d[reg]);
    } else {
        /* MULU - unsigned */
        uint32_t result = (uint32_t)dst * (uint32_t)src;
        cpu->d[reg] = result;
        set_logic_flags_32(cpu, cpu->d[reg]);
    }
    cpu->cycles += 70; /* worst case */
}

/* --- DIVU/DIVS --- */
static void op_divu(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    uint16_t src = read_ea_mode(cpu, ea_mode, ea_reg, SIZE_WORD);

    if (src == 0) {
        take_exception(cpu, VEC_ZERO_DIVIDE);
        return;
    }

    if (op & 0x0100) {
        /* DIVS - signed */
        int32_t dst = (int32_t)cpu->d[reg];
        int32_t quot = dst / (int16_t)src;
        int16_t rem = dst % (int16_t)src;

        if (quot > 32767 || quot < -32768) {
            set_flag(cpu, SR_OVERFLOW, true);
        } else {
            cpu->d[reg] = ((uint16_t)rem << 16) | ((uint16_t)quot & 0xFFFF);
            set_flag(cpu, SR_OVERFLOW, false);
            set_flag(cpu, SR_ZERO, (quot & 0xFFFF) == 0);
            set_flag(cpu, SR_NEGATIVE, (quot & 0x8000) != 0);
        }
        set_flag(cpu, SR_CARRY, false);
    } else {
        /* DIVU - unsigned */
        uint32_t dst = cpu->d[reg];
        uint32_t quot = dst / src;
        uint16_t rem = dst % src;

        if (quot > 0xFFFF) {
            set_flag(cpu, SR_OVERFLOW, true);
        } else {
            cpu->d[reg] = (rem << 16) | (quot & 0xFFFF);
            set_flag(cpu, SR_OVERFLOW, false);
            set_flag(cpu, SR_ZERO, (quot & 0xFFFF) == 0);
            set_flag(cpu, SR_NEGATIVE, (quot & 0x8000) != 0);
        }
        set_flag(cpu, SR_CARRY, false);
    }
    cpu->cycles += 140; /* worst case */
}

/* --- Shift/Rotate operations --- */
static void op_shift(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int type = (op >> 3) & 3;     /* 00=AS, 01=LS, 10=ROX, 11=RO */
    int dir = (op >> 8) & 1;      /* 0=right, 1=left */
    int size = (op >> 6) & 3;

    if (size == 3) {
        /* Memory shift - always word, shift by 1 */
        int ea_mode = (op >> 3) & 7;
        int ea_reg = op & 7;
        type = (op >> 9) & 3;
        dir = (op >> 8) & 1;

        ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
        uint16_t val = read_ea(cpu, &ea, SIZE_WORD);
        uint16_t result;
        bool msb = (val & 0x8000) != 0;
        bool lsb = (val & 1) != 0;

        switch (type) {
            case 0: /* ASd */
                if (dir) {
                    result = val << 1;
                    set_flag(cpu, SR_CARRY, msb);
                    set_flag(cpu, SR_EXTEND, msb);
                    set_flag(cpu, SR_OVERFLOW, (result & 0x8000) != (val & 0x8000));
                } else {
                    result = (int16_t)val >> 1;
                    set_flag(cpu, SR_CARRY, lsb);
                    set_flag(cpu, SR_EXTEND, lsb);
                    set_flag(cpu, SR_OVERFLOW, false);
                }
                break;
            case 1: /* LSd */
                if (dir) {
                    result = val << 1;
                    set_flag(cpu, SR_CARRY, msb);
                    set_flag(cpu, SR_EXTEND, msb);
                } else {
                    result = val >> 1;
                    set_flag(cpu, SR_CARRY, lsb);
                    set_flag(cpu, SR_EXTEND, lsb);
                }
                set_flag(cpu, SR_OVERFLOW, false);
                break;
            case 2: /* ROXd */
                if (dir) {
                    result = (val << 1) | (get_flag(cpu, SR_EXTEND) ? 1 : 0);
                    set_flag(cpu, SR_CARRY, msb);
                    set_flag(cpu, SR_EXTEND, msb);
                } else {
                    result = (val >> 1) | (get_flag(cpu, SR_EXTEND) ? 0x8000 : 0);
                    set_flag(cpu, SR_CARRY, lsb);
                    set_flag(cpu, SR_EXTEND, lsb);
                }
                set_flag(cpu, SR_OVERFLOW, false);
                break;
            case 3: /* ROd */
                if (dir) {
                    result = (val << 1) | (msb ? 1 : 0);
                    set_flag(cpu, SR_CARRY, msb);
                } else {
                    result = (val >> 1) | (lsb ? 0x8000 : 0);
                    set_flag(cpu, SR_CARRY, lsb);
                }
                set_flag(cpu, SR_OVERFLOW, false);
                break;
            default:
                result = val;
                break;
        }
        set_nz_16(cpu, result);
        write_ea(cpu, &ea, SIZE_WORD, result);
        cpu->cycles += 8;
        return;
    }

    /* Register shift */
    int reg = op & 7;
    int count_or_reg = (op >> 9) & 7;
    int ir_bit = (op >> 5) & 1; /* 0=immediate count, 1=register count */

    int count;
    if (ir_bit)
        count = cpu->d[count_or_reg] & 63;
    else {
        count = count_or_reg;
        if (count == 0) count = 8;
    }

    uint32_t val;
    uint32_t mask;
    uint32_t msb_mask;
    switch (size) {
        case SIZE_BYTE: val = cpu->d[reg] & 0xFF; mask = 0xFF; msb_mask = 0x80; break;
        case SIZE_WORD: val = cpu->d[reg] & 0xFFFF; mask = 0xFFFF; msb_mask = 0x8000; break;
        default: val = cpu->d[reg]; mask = 0xFFFFFFFF; msb_mask = 0x80000000; break;
    }

    uint32_t result = val;
    type = (op >> 3) & 3;

    if (count == 0) {
        set_flag(cpu, SR_CARRY, false);
        switch (size) {
            case SIZE_BYTE: set_logic_flags_8(cpu, result); break;
            case SIZE_WORD: set_logic_flags_16(cpu, result); break;
            case SIZE_LONG: set_logic_flags_32(cpu, result); break;
        }
        cpu->cycles += (size == SIZE_LONG) ? 8 : 6;
        return;
    }

    switch (type) {
        case 0: /* ASd */
            if (dir) {
                bool overflow = false;
                for (int i = 0; i < count; i++) {
                    bool old_msb = (result & msb_mask) != 0;
                    result = (result << 1) & mask;
                    bool new_msb = (result & msb_mask) != 0;
                    if (old_msb != new_msb) overflow = true;
                    set_flag(cpu, SR_CARRY, old_msb);
                    set_flag(cpu, SR_EXTEND, old_msb);
                }
                set_flag(cpu, SR_OVERFLOW, overflow);
            } else {
                for (int i = 0; i < count; i++) {
                    bool lsb_bit = (result & 1) != 0;
                    bool sign = (result & msb_mask) != 0;
                    result = (result >> 1) | (sign ? msb_mask : 0);
                    result &= mask;
                    set_flag(cpu, SR_CARRY, lsb_bit);
                    set_flag(cpu, SR_EXTEND, lsb_bit);
                }
                set_flag(cpu, SR_OVERFLOW, false);
            }
            break;

        case 1: /* LSd */
            if (dir) {
                for (int i = 0; i < count; i++) {
                    bool old_msb = (result & msb_mask) != 0;
                    result = (result << 1) & mask;
                    set_flag(cpu, SR_CARRY, old_msb);
                    set_flag(cpu, SR_EXTEND, old_msb);
                }
            } else {
                for (int i = 0; i < count; i++) {
                    bool lsb_bit = (result & 1) != 0;
                    result = (result >> 1) & mask;
                    set_flag(cpu, SR_CARRY, lsb_bit);
                    set_flag(cpu, SR_EXTEND, lsb_bit);
                }
            }
            set_flag(cpu, SR_OVERFLOW, false);
            break;

        case 2: /* ROXd */
            if (dir) {
                for (int i = 0; i < count; i++) {
                    bool old_msb = (result & msb_mask) != 0;
                    bool old_x = get_flag(cpu, SR_EXTEND);
                    result = ((result << 1) | (old_x ? 1 : 0)) & mask;
                    set_flag(cpu, SR_CARRY, old_msb);
                    set_flag(cpu, SR_EXTEND, old_msb);
                }
            } else {
                for (int i = 0; i < count; i++) {
                    bool lsb_bit = (result & 1) != 0;
                    bool old_x = get_flag(cpu, SR_EXTEND);
                    result = ((result >> 1) | (old_x ? msb_mask : 0)) & mask;
                    set_flag(cpu, SR_CARRY, lsb_bit);
                    set_flag(cpu, SR_EXTEND, lsb_bit);
                }
            }
            set_flag(cpu, SR_OVERFLOW, false);
            break;

        case 3: /* ROd */
            if (dir) {
                for (int i = 0; i < count; i++) {
                    bool old_msb = (result & msb_mask) != 0;
                    result = ((result << 1) | (old_msb ? 1 : 0)) & mask;
                    set_flag(cpu, SR_CARRY, old_msb);
                }
            } else {
                for (int i = 0; i < count; i++) {
                    bool lsb_bit = (result & 1) != 0;
                    result = ((result >> 1) | (lsb_bit ? msb_mask : 0)) & mask;
                    set_flag(cpu, SR_CARRY, lsb_bit);
                }
            }
            set_flag(cpu, SR_OVERFLOW, false);
            break;
    }

    switch (size) {
        case SIZE_BYTE:
            cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (result & 0xFF);
            set_nz_8(cpu, result);
            break;
        case SIZE_WORD:
            cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (result & 0xFFFF);
            set_nz_16(cpu, result);
            break;
        case SIZE_LONG:
            cpu->d[reg] = result;
            set_nz_32(cpu, result);
            break;
    }
    cpu->cycles += (size == SIZE_LONG) ? 8 + 2 * count : 6 + 2 * count;
}

/* --- Bit operations (BTST, BCHG, BCLR, BSET) --- */
static void op_bit_dynamic(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int bit_reg = (op >> 9) & 7;
    int type = (op >> 6) & 3;  /* 0=BTST, 1=BCHG, 2=BCLR, 3=BSET */
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int bit = cpu->d[bit_reg];

    if (ea_mode == AM_DATA_REG) {
        /* Long operation on data register */
        bit &= 31;
        uint32_t mask_bit = 1u << bit;
        set_flag(cpu, SR_ZERO, (cpu->d[ea_reg] & mask_bit) == 0);
        switch (type) {
            case 1: cpu->d[ea_reg] ^= mask_bit; break;
            case 2: cpu->d[ea_reg] &= ~mask_bit; break;
            case 3: cpu->d[ea_reg] |= mask_bit; break;
        }
    } else {
        /* Byte operation on memory */
        bit &= 7;
        ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
        uint8_t val = read_ea(cpu, &ea, SIZE_BYTE);
        set_flag(cpu, SR_ZERO, (val & (1 << bit)) == 0);
        switch (type) {
            case 1: val ^= (1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
            case 2: val &= ~(1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
            case 3: val |= (1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
        }
    }
    cpu->cycles += (type == 0) ? 4 : 8;
}

static void op_bit_static(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int type = (op >> 6) & 3;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    int bit = fetch16(cpu) & 0xFF;

    if (ea_mode == AM_DATA_REG) {
        bit &= 31;
        uint32_t mask_bit = 1u << bit;
        set_flag(cpu, SR_ZERO, (cpu->d[ea_reg] & mask_bit) == 0);
        switch (type) {
            case 1: cpu->d[ea_reg] ^= mask_bit; break;
            case 2: cpu->d[ea_reg] &= ~mask_bit; break;
            case 3: cpu->d[ea_reg] |= mask_bit; break;
        }
    } else {
        bit &= 7;
        ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
        uint8_t val = read_ea(cpu, &ea, SIZE_BYTE);
        set_flag(cpu, SR_ZERO, (val & (1 << bit)) == 0);
        switch (type) {
            case 1: val ^= (1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
            case 2: val &= ~(1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
            case 3: val |= (1 << bit); write_ea(cpu, &ea, SIZE_BYTE, val); break;
        }
    }
    cpu->cycles += (type == 0) ? 4 : 8;
}

/* --- TRAP --- */

/* HLE: TRAP #6 — MMU programming (DO_AN_MMU).
 * Emulates the DO_AN_MMU handler from LDASM which programs MMU
 * segment registers during boot. The real handler toggles setup mode,
 * reads SMT entries, and writes to MMU hardware registers.
 * We emulate this by directly programming our MMU segment tables.
 *
 * Registers on entry (from PROG_MMU caller):
 *   A0 = return address
 *   D0 = return domain
 *   D1 = mmu count
 *   D2 = target domain
 *   D3 = mmu index
 *
 * The handler reads from smt_base (at the end of do_an_mmu code)
 * which contains: origin(16) | access(8) | length(8) per segment.
 */
static bool hle_trap6_do_an_mmu(m68k_t *cpu) {
    uint32_t handler = cpu_read32(cpu, 0x98);
    DBGSTATIC(int, t6_dbg, 0);
    if (t6_dbg++ < 3)
        fprintf(stderr, "[HLE-TRAP6] check: handler@$98=$%08X\n", handler);
    if (handler != 0x3F8) return false;

    uint32_t d0 = cpu->d[0] & 0xFFFF;
    uint32_t d1 = cpu->d[1] & 0xFFFF;
    uint32_t d2 = cpu->d[2] & 0xFFFF;
    uint32_t d3 = cpu->d[3] & 0xFFFF;

    /* smt_base: physical address where the SMT (segment mapping table)
     * data lives at runtime. Two candidates:
     *   1. The LDASM local label `smt_base` (line 448 of source-LDASM)
     *      — NOT exported to the linker, so not in the map.
     *   2. The Pascal VAR `smt_base: absptr` in parms.text — declared as
     *      a global, so the linker map DOES contain "smt_base" but as
     *      the VAR's A5-relative offset (negative), not the SMT data's
     *      physical location. Pre-P86 linker bug: base_addr was added
     *      to this negative offset, turning it into a coincidental
     *      positive value in RAM that happened to collide with
     *      g_hle_smt_base (= os_end from bootrom init) — so the HLE
     *      "worked" by luck.
     *   3. g_hle_smt_base: set by bootrom_build to os_end when the
     *      synthetic bootrom primes the SMT region. Deterministically
     *      points to the real SMT data location.
     * Post-P86: use g_hle_smt_base directly. If that's zero (shouldn't
     * happen in normal boot), fall back to the map lookup but only if
     * the value looks like a valid physical address. */
    extern uint32_t g_hle_smt_base;
    extern uint32_t boot_progress_lookup(const char *name);
    static uint32_t smt_ptr_cached = 0;
    if (smt_ptr_cached == 0) {
        if (g_hle_smt_base != 0 && g_hle_smt_base < 0xFFFFFF) {
            smt_ptr_cached = g_hle_smt_base;
        } else {
            uint32_t map_val = boot_progress_lookup("smt_base");
            /* Reject A5-relative offsets (high bits set = signed-negative
             * in 32-bit). Only use if it looks like a real RAM address. */
            if (map_val != 0 && map_val < 0x01000000)
                smt_ptr_cached = map_val;
        }
    }
    uint32_t smt_ptr = smt_ptr_cached;
    if (smt_ptr == 0 || smt_ptr > 0xFFFFFF) return false;

    DBGSTATIC(int, hle_t6_count, 0);
    if (hle_t6_count < 5)
        fprintf(stderr, "[HLE-TRAP6] #%d: d2=%d d3=%d count=%d SMT@$%06X\n",
                hle_t6_count + 1, d2, d3, d1, smt_ptr);
    hle_t6_count++;

    extern void lisa_hle_prog_mmu(uint32_t domain, uint32_t index,
                                  uint32_t count, uint32_t smt_base);
    lisa_hle_prog_mmu(d2, d3, d1, smt_ptr);

    (void)d0;
    cpu->cycles += 34;
    return true;
}

static void op_trap(m68k_t *cpu) {
    int vector = cpu->ir & 0xF;

    {
        static int trap_count[16] = {0};
        static int trap_count_gen = 0;
        if (trap_count_gen != g_emu_generation) { memset(trap_count, 0, sizeof(trap_count)); trap_count_gen = g_emu_generation; }
        trap_count[vector]++;
        if (vector == 6) { extern int g_trap6_total; g_trap6_total++; }
        if (vector == 6 && trap_count[6] >= 211 && trap_count[6] <= 220) {
            uint32_t smt_ptr = cpu_read32(cpu, (cpu->a[5] - 4) & 0xFFFFFF);
            uint32_t d2v = cpu->d[2] & 0xFFFF;
            uint32_t d3v = cpu->d[3] & 0xFFFF;
            uint32_t smt_entry = smt_ptr + d2v * 512 + d3v * 4;
            uint16_t origin = cpu_read16(cpu, smt_entry);
            uint8_t access = cpu_read8(cpu, smt_entry + 2);
            uint8_t limit = cpu_read8(cpu, smt_entry + 3);
            fprintf(stderr, "TRAP6[%d]: d2=%d d3=%d SMT@$%06X: origin=$%04X access=$%02X limit=$%02X\n",
                    trap_count[6], d2v, d3v, smt_entry, origin, access, limit);
        }
    }

    if (vector == 6 && hle_trap6_do_an_mmu(cpu))
        return;

    take_exception(cpu, VEC_TRAP_BASE + vector);
    cpu->cycles += 34;
}

/* --- MOVE USP --- */
static void op_move_usp(m68k_t *cpu) {
    if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
    int reg = cpu->ir & 7;
    if (cpu->ir & 8) {
        /* USP to An */
        cpu->a[reg] = cpu->usp;
    } else {
        /* An to USP */
        cpu->usp = cpu->a[reg];
    }
    cpu->cycles += 4;
}

/* --- NOP --- */
static void op_nop(m68k_t *cpu) {
    cpu->cycles += 4;
}

/* --- STOP --- */
static void op_stop(m68k_t *cpu) {
    if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
    uint16_t new_sr = fetch16(cpu);
    cpu->sr = new_sr;
    cpu->stopped = true;
    cpu->cycles += 4;
}

/* --- RESET --- */
static void op_reset(m68k_t *cpu) {
    if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
    /* Assert RESET line - hardware callback would go here */
    cpu->cycles += 132;
}

/* --- TRAPV --- */
static void op_trapv(m68k_t *cpu) {
    if (get_flag(cpu, SR_OVERFLOW)) {
        take_exception(cpu, VEC_TRAPV);
    }
    cpu->cycles += 4;
}

/* --- CHK --- */
static void op_chk(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int reg = (op >> 9) & 7;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    int16_t src = (int16_t)read_ea_mode(cpu, ea_mode, ea_reg, SIZE_WORD);
    int16_t dn = (int16_t)(cpu->d[reg] & 0xFFFF);

    if (dn < 0 || dn > src) {
        set_flag(cpu, SR_NEGATIVE, dn < 0);
        take_exception(cpu, VEC_CHK);
    }
    cpu->cycles += 10;
}

/* --- MOVE to/from SR, CCR --- */
static void op_move_to_sr(m68k_t *cpu) {
    if (!is_supervisor(cpu)) { privilege_violation(cpu); return; }
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    uint16_t val = read_ea_mode(cpu, ea_mode, ea_reg, SIZE_WORD);
    bool was_super = is_supervisor(cpu);
    cpu->sr = val;
    if (was_super && !(val & SR_SUPERVISOR)) {
        cpu->ssp = cpu->a[7];
        cpu->a[7] = cpu->usp;
    } else if (!was_super && (val & SR_SUPERVISOR)) {
        cpu->usp = cpu->a[7];
        cpu->a[7] = cpu->ssp;
    }
    cpu->cycles += 12;
}

static void op_move_from_sr(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_WORD);
    write_ea(cpu, &ea, SIZE_WORD, cpu->sr);
    cpu->cycles += 6;
}

static void op_move_to_ccr(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;
    uint16_t val = read_ea_mode(cpu, ea_mode, ea_reg, SIZE_WORD);
    cpu->sr = (cpu->sr & 0xFF00) | (val & 0xFF);
    cpu->cycles += 12;
}

/* --- EXG --- */
static void op_exg(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int rx = (op >> 9) & 7;
    int ry = op & 7;
    int mode = (op >> 3) & 0x1F;
    uint32_t tmp;

    switch (mode) {
        case 0x08: /* Data registers */
            tmp = cpu->d[rx]; cpu->d[rx] = cpu->d[ry]; cpu->d[ry] = tmp;
            break;
        case 0x09: /* Address registers */
            tmp = cpu->a[rx]; cpu->a[rx] = cpu->a[ry]; cpu->a[ry] = tmp;
            break;
        case 0x11: /* Data and address */
            tmp = cpu->d[rx]; cpu->d[rx] = cpu->a[ry]; cpu->a[ry] = tmp;
            break;
    }
    cpu->cycles += 6;
}

/* --- MOVEP --- */
static void op_movep(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int data_reg = (op >> 9) & 7;
    int addr_reg = op & 7;
    int16_t disp = (int16_t)fetch16(cpu);
    uint32_t addr = cpu->a[addr_reg] + disp;
    int mode = (op >> 6) & 7;

    switch (mode) {
        case 4: /* Word, memory to register */
            cpu->d[data_reg] = (cpu->d[data_reg] & 0xFFFF0000) |
                               ((uint16_t)cpu_read8(cpu, addr) << 8) |
                               cpu_read8(cpu, addr + 2);
            break;
        case 5: /* Long, memory to register */
            cpu->d[data_reg] = ((uint32_t)cpu_read8(cpu, addr) << 24) |
                               ((uint32_t)cpu_read8(cpu, addr + 2) << 16) |
                               ((uint32_t)cpu_read8(cpu, addr + 4) << 8) |
                               cpu_read8(cpu, addr + 6);
            break;
        case 6: /* Word, register to memory */
            cpu_write8(cpu, addr, (cpu->d[data_reg] >> 8) & 0xFF);
            cpu_write8(cpu, addr + 2, cpu->d[data_reg] & 0xFF);
            break;
        case 7: /* Long, register to memory */
            cpu_write8(cpu, addr, (cpu->d[data_reg] >> 24) & 0xFF);
            cpu_write8(cpu, addr + 2, (cpu->d[data_reg] >> 16) & 0xFF);
            cpu_write8(cpu, addr + 4, (cpu->d[data_reg] >> 8) & 0xFF);
            cpu_write8(cpu, addr + 6, cpu->d[data_reg] & 0xFF);
            break;
    }
    cpu->cycles += 16;
}

/* --- NBCD --- */
static void op_nbcd(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
    uint8_t val = read_ea(cpu, &ea, SIZE_BYTE);
    int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;

    uint16_t result = (uint16_t)(0 - val - x);

    /* BCD correction */
    uint8_t lo = (0 - (val & 0x0F) - x) & 0x1F;
    if (lo > 9) lo -= 6;
    uint8_t hi_src = (val >> 4) & 0x0F;
    int borrow = (lo & 0x10) ? 1 : 0;
    uint8_t hi = (0 - hi_src - borrow) & 0x1F;
    if (hi > 9) hi -= 6;

    uint8_t bcd_result = ((hi & 0x0F) << 4) | (lo & 0x0F);
    bool carry = (result > 0xFF) || (hi > 9);

    if (bcd_result != 0) set_flag(cpu, SR_ZERO, false);
    set_flag(cpu, SR_CARRY, carry);
    set_flag(cpu, SR_EXTEND, carry);

    write_ea(cpu, &ea, SIZE_BYTE, bcd_result);
    cpu->cycles += 6;
}

/* --- ABCD --- */
static void op_abcd(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int rx = (op >> 9) & 7;
    int ry = op & 7;
    int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;

    uint8_t src, dst;
    if (op & 8) {
        /* Memory: -(Ay), -(Ax) */
        cpu->a[ry] -= (ry == 7) ? 2 : 1;
        src = cpu_read8(cpu, cpu->a[ry]);
        cpu->a[rx] -= (rx == 7) ? 2 : 1;
        dst = cpu_read8(cpu, cpu->a[rx]);
    } else {
        src = cpu->d[ry] & 0xFF;
        dst = cpu->d[rx] & 0xFF;
    }

    uint16_t lo = (dst & 0x0F) + (src & 0x0F) + x;
    if (lo > 9) lo += 6;
    uint16_t hi = ((dst >> 4) & 0x0F) + ((src >> 4) & 0x0F) + (lo > 0x1F ? 1 : (lo > 9 ? 1 : 0));
    if (hi > 9) hi += 6;

    uint8_t result = ((hi & 0x0F) << 4) | (lo & 0x0F);
    bool carry = hi > 0x0F;

    if (result != 0) set_flag(cpu, SR_ZERO, false);
    set_flag(cpu, SR_CARRY, carry);
    set_flag(cpu, SR_EXTEND, carry);

    if (op & 8) {
        cpu_write8(cpu, cpu->a[rx], result);
    } else {
        cpu->d[rx] = (cpu->d[rx] & 0xFFFFFF00) | result;
    }
    cpu->cycles += 6;
}

/* --- SBCD --- */
static void op_sbcd(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int rx = (op >> 9) & 7;
    int ry = op & 7;
    int x = get_flag(cpu, SR_EXTEND) ? 1 : 0;

    uint8_t src, dst;
    if (op & 8) {
        cpu->a[ry] -= (ry == 7) ? 2 : 1;
        src = cpu_read8(cpu, cpu->a[ry]);
        cpu->a[rx] -= (rx == 7) ? 2 : 1;
        dst = cpu_read8(cpu, cpu->a[rx]);
    } else {
        src = cpu->d[ry] & 0xFF;
        dst = cpu->d[rx] & 0xFF;
    }

    int lo = (dst & 0x0F) - (src & 0x0F) - x;
    int borrow_lo = 0;
    if (lo < 0) { lo += 10; borrow_lo = 1; }
    int hi = ((dst >> 4) & 0x0F) - ((src >> 4) & 0x0F) - borrow_lo;
    bool carry = false;
    if (hi < 0) { hi += 10; carry = true; }

    uint8_t result = ((hi & 0x0F) << 4) | (lo & 0x0F);

    if (result != 0) set_flag(cpu, SR_ZERO, false);
    set_flag(cpu, SR_CARRY, carry);
    set_flag(cpu, SR_EXTEND, carry);

    if (op & 8) {
        cpu_write8(cpu, cpu->a[rx], result);
    } else {
        cpu->d[rx] = (cpu->d[rx] & 0xFFFFFF00) | result;
    }
    cpu->cycles += 6;
}

/* --- TAS --- */
static void op_tas(m68k_t *cpu) {
    uint16_t op = cpu->ir;
    int ea_mode = (op >> 3) & 7;
    int ea_reg = op & 7;

    ea_result_t ea = calc_ea(cpu, ea_mode, ea_reg, SIZE_BYTE);
    uint8_t val = read_ea(cpu, &ea, SIZE_BYTE);

    set_logic_flags_8(cpu, val);
    val |= 0x80;
    write_ea(cpu, &ea, SIZE_BYTE, val);
    cpu->cycles += 4;
}

/* --- MOVE to/from A7 (USP) already handled --- */

/* ========================================================================
 * Main instruction decoder
 * ======================================================================== */

static void execute_one(m68k_t *cpu) {
    cpu->ir = fetch16(cpu);
    uint16_t op = cpu->ir;

    /* Decode by top 4 bits */
    switch ((op >> 12) & 0xF) {
        case 0x0:
            /* Bit manipulation, MOVEP, immediate */
            if (op & 0x0100) {
                /* Dynamic bit ops (BTST/BCHG/BCLR/BSET Dn,<ea>) or MOVEP.
                 * MOVEP has mode field = 001 (An indirect with disp). */
                if ((op & 0x0038) == 0x0008) {
                    op_movep(cpu);
                } else {
                    op_bit_dynamic(cpu);
                }
            } else if ((op & 0x0F00) == 0x0800) {
                /* Static bit ops */
                op_bit_static(cpu);
            } else {
                switch ((op >> 8) & 0xF) {
                    case 0x0: op_ori(cpu); break;
                    case 0x2: op_andi(cpu); break;
                    case 0x4: op_subi(cpu); break;
                    case 0x6: op_addi(cpu); break;
                    case 0xA: op_eori(cpu); break;
                    case 0xC: op_cmpi(cpu); break;
                    default: illegal_instruction(cpu); break;
                }
            }
            break;

        case 0x1: /* MOVE.B */
        case 0x2: /* MOVE.L */
        case 0x3: /* MOVE.W */
            op_move(cpu);
            break;

        case 0x4:
            /* Miscellaneous */
            if ((op & 0xFFC0) == 0x40C0) { op_move_from_sr(cpu); break; }
            if ((op & 0xFFC0) == 0x44C0) { op_move_to_ccr(cpu); break; }
            if ((op & 0xFFC0) == 0x46C0) { op_move_to_sr(cpu); break; }

            if ((op & 0xFF00) == 0x4000) { op_negx(cpu); break; }
            if ((op & 0xFF00) == 0x4200) { op_clr(cpu); break; }
            if ((op & 0xFF00) == 0x4400) { op_neg(cpu); break; }
            if ((op & 0xFF00) == 0x4600) { op_not(cpu); break; }
            if ((op & 0xFF00) == 0x4A00) { op_tst(cpu); break; }

            if ((op & 0xFFC0) == 0x4AC0) { op_tas(cpu); break; }

            if ((op & 0xFFF8) == 0x4840) { op_swap(cpu); break; }
            if ((op & 0xFFC0) == 0x4840) { op_pea(cpu); break; }

            if ((op & 0xFFF8) == 0x4880) { op_ext(cpu); break; }  /* EXT.W */
            if ((op & 0xFFF8) == 0x48C0) { op_ext(cpu); break; }  /* EXT.L */

            if ((op & 0xFB80) == 0x4880) { op_movem(cpu); break; }

            if ((op & 0xFFF0) == 0x4E40) { op_trap(cpu); break; }
            if ((op & 0xFFF8) == 0x4E50) { op_link(cpu); break; }
            if ((op & 0xFFF8) == 0x4E58) { op_unlk(cpu); break; }
            if ((op & 0xFFF0) == 0x4E60) { op_move_usp(cpu); break; }

            if (op == 0x4E70) { op_reset(cpu); break; }
            if (op == 0x4E71) { op_nop(cpu); break; }
            if (op == 0x4E72) { op_stop(cpu); break; }
            if (op == 0x4E73) { op_rte(cpu); break; }
            if (op == 0x4E75) { op_rts(cpu); break; }
            if (op == 0x4E76) { op_trapv(cpu); break; }
            if (op == 0x4E77) { op_rtr(cpu); break; }

            if ((op & 0xFFC0) == 0x4EC0) { op_jmp(cpu); break; }
            if ((op & 0xFFC0) == 0x4E80) { op_jsr(cpu); break; }

            if ((op & 0xFFC0) == 0x4180) { op_chk(cpu); break; }

            if ((op & 0xF1C0) == 0x41C0) { op_lea(cpu); break; }

            if ((op & 0xFFC0) == 0x4800) { op_nbcd(cpu); break; }

            /* HLE: $4FBC is a Lisa Pascal INLINE custom opcode used in
             * Workshop init code. It's illegal on plain 68000. The operand
             * word follows immediately (e.g. $4FBC $000C). Skip as NOP so
             * the init loop completes and boot continues past Lisabug. */
            if (op == 0x4FBC) {
                DBGSTATIC(int, hle_4fbc_count, 0);
                if (hle_4fbc_count++ < 5)
                    fprintf(stderr, "[HLE] $%04X NOP-skip at PC=$%06X (operand=$%04X)\n",
                            op, cpu->pc - 2, cpu_read16(cpu, cpu->pc));
                cpu->pc += 2;
                break;
            }

            illegal_instruction(cpu);
            break;

        case 0x5:
            if ((op & 0x00C0) == 0x00C0) {
                if ((op & 0x0038) == 0x0008)
                    op_dbcc(cpu);
                else
                    op_scc(cpu);
            } else if (op & 0x0100) {
                op_subq(cpu);
            } else {
                op_addq(cpu);
            }
            break;

        case 0x6:
            if ((op & 0x0F00) == 0) {
                /* BRA */
                op_bcc(cpu);
            } else if ((op & 0x0F00) == 0x0100) {
                /* BSR */
                op_bcc(cpu);
            } else {
                op_bcc(cpu);
            }
            break;

        case 0x7:
            op_moveq(cpu);
            break;

        case 0x8:
            if ((op & 0x01C0) == 0x0100) {
                op_sbcd(cpu);
            } else if ((op & 0x00C0) == 0x00C0) {
                op_divu(cpu);
            } else {
                op_or(cpu);
            }
            break;

        case 0x9:
            op_sub(cpu);
            break;

        case 0xA:
            /* Line-A emulation — 68000 pushes PC of the Line-A opcode itself */
            cpu->pc -= 2;
            take_exception(cpu, VEC_LINE_A);
            break;

        case 0xB:
            if ((op & 0x00C0) == 0x00C0 || ((op & 0x0100) == 0)) {
                op_cmp(cpu);
            } else {
                op_eor(cpu);
            }
            break;

        case 0xC:
            if ((op & 0x01C0) == 0x0100) {
                op_abcd(cpu);
            } else if ((op & 0x00C0) == 0x00C0) {
                op_mulu(cpu);
            } else if ((op & 0x01F0) == 0x0100 || (op & 0x01F0) == 0x0140 || (op & 0x01F0) == 0x0180) {
                /* Could be EXG */
                int opmode = (op >> 3) & 0x1F;
                if (opmode == 0x08 || opmode == 0x09 || opmode == 0x11) {
                    op_exg(cpu);
                } else {
                    op_and(cpu);
                }
            } else {
                op_and(cpu);
            }
            break;

        case 0xD:
            op_add(cpu);
            break;

        case 0xE:
            op_shift(cpu);
            break;

        case 0xF:
            /* Line-F: 68000 pushes PC of the Line-F opcode itself.
             * Skip past it and continue. */
            cpu->pc -= 2;
            take_exception(cpu, VEC_LINE_F);
            break;
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void m68k_init(m68k_t *cpu) {
    memset(cpu, 0, sizeof(m68k_t));
}

void m68k_reset(m68k_t *cpu) {
    /* Read initial SSP and PC from vector table */
    cpu->sr = SR_SUPERVISOR | 0x0700; /* Supervisor mode, all interrupts masked */
    cpu->a[7] = cpu_read32(cpu, 0);    /* SSP */
    cpu->ssp = cpu->a[7];
    cpu->pc = cpu_read32(cpu, 4);      /* PC */
    cpu->stopped = false;
    cpu->halted = false;
    cpu->pending_irq = 0;
    cpu->cycles = 0;
    cpu->total_cycles = 0;
}

int m68k_execute(m68k_t *cpu, int target_cycles) {
    int start_cycles = cpu->total_cycles;

    while (cpu->total_cycles - start_cycles < target_cycles) {
        if (cpu->halted) {
            cpu->total_cycles = start_cycles + target_cycles;
            break;
        }

        /* Check for pending interrupts */
        if (cpu->pending_irq > 0) {
            int mask = (cpu->sr & SR_INT_MASK) >> 8;
            if (cpu->pending_irq > mask || cpu->pending_irq == 7) {
                int lvl = cpu->pending_irq;
                /* Accept interrupt */
                cpu->stopped = false;
                uint16_t old_sr = cpu->sr;
                set_supervisor(cpu, true);
                cpu->sr = (cpu->sr & ~SR_INT_MASK) | (lvl << 8);
                cpu->sr &= ~SR_TRACE;
                push32(cpu, cpu->pc);
                push16(cpu, old_sr);
                cpu->pc = cpu_read32(cpu, (VEC_AUTOVECTOR_BASE + lvl - 1) * 4);
                cpu->cycles = 44;
                cpu->total_cycles += cpu->cycles;
                cpu->pending_irq = 0;
                continue;
            }
        }

        if (cpu->stopped) {
            cpu->total_cycles = start_cycles + target_cycles;
            break;
        }

        /* PC ring buffer — trace crashes and escapes */
        static uint32_t pc_ring[256];
        static int pc_ring_idx = 0;
        static int pc_ring_gen = 0;
        /* P80h3: track (PCB, syslocal) pairs created via HLE-CreateProcess
         * so HLE-SelectProcess can update b_syslocal_ptr when the
         * Scheduler dispatches a process. Map_Syslocal (the normal
         * Pascal routine that does this) is never called under our
         * bypasses. */
        static struct { uint32_t pcb, sloc; } pcb_sloc_map[8];
        static int pcb_sloc_count = 0;
        if (pc_ring_gen != g_emu_generation) { memset(pc_ring, 0, sizeof(pc_ring)); pc_ring_idx = 0; pc_ring_gen = g_emu_generation; }
        /* High-byte PC trip: fire once when PC first leaves the 24-bit
         * range. Dump the PC ring (previous instruction(s)) to identify
         * the transition point. */
        {
            static int hip_fired = 0;
            static int hip_fired_gen = -1;
            if (hip_fired_gen != g_emu_generation) { hip_fired = 0; hip_fired_gen = g_emu_generation; }
            if (!hip_fired && (cpu->pc & 0xFF000000)) {
                hip_fired = 1;
                fprintf(stderr, "HIPC-TRIP: PC=$%08X (prev-ring, newest first):\n", cpu->pc);
                for (int k = 0; k < 16; k++) {
                    int idx = (pc_ring_idx - 1 - k + 256) & 0xFF;
                    fprintf(stderr, "  [-%d] PC=$%08X\n", k+1, pc_ring[idx]);
                }
                fprintf(stderr, "  A0=$%08X A1=$%08X A5=$%08X A6=$%08X A7=$%08X SR=$%04X\n",
                        cpu->a[0], cpu->a[1], cpu->a[5], cpu->a[6], cpu->a[7], cpu->sr);
            }
        }
        pc_ring[pc_ring_idx++ & 255] = cpu->pc;
        g_last_cpu_pc = cpu->pc;

        /* P113: detect PC entering stack region ($CBF___-$CBFFFF) — the
         * classic "returned to garbage retaddr" signature for our P112
         * driver-dispatch debug. Dump PC ring once so we can see what
         * path led to the stack jump. */
        {
            static int stack_trip_fired = 0;
            static int stack_trip_gen = -1;
            if (stack_trip_gen != g_emu_generation) {
                stack_trip_fired = 0;
                stack_trip_gen = g_emu_generation;
            }
            /* P113/P114: log driver-range PC transitions + dump kernel
             * dispatch stub bytes at first DRV-EXIT. */
            {
                static uint32_t prev_drv_pc = 0;
                static bool dumped_ccb = false;
                bool cur_drv = (cpu->pc >= 0x200000 && cpu->pc < 0x210000);
                bool prev_drv = (prev_drv_pc >= 0x200000 && prev_drv_pc < 0x210000);
                if (cur_drv != prev_drv) {
                    DBGSTATIC(int, drv_log, 0);
                    if (drv_log++ < 20)
                        fprintf(stderr, "P113 DRV-%s: PC=$%08X (prev=$%08X) A6=$%08X A7=$%08X\n",
                                cur_drv ? "ENTER" : "EXIT", cpu->pc, prev_drv_pc,
                                cpu->a[6], cpu->a[7]);
                    /* First driver-exit to kernel jumptable area: dump
                     * 80 bytes at destination so we can see the real
                     * dispatch stubs. */
                    if (!cur_drv && prev_drv && !dumped_ccb &&
                        cpu->pc >= 0x00C00000 && cpu->pc < 0x00D00000) {
                        dumped_ccb = true;
                        uint32_t base = cpu->pc;
                        fprintf(stderr, "P114 JUMPTABLE @ $%06X:\n", base);
                        for (int row = 0; row < 5; row++) {
                            fprintf(stderr, "  $%06X:", base + row * 16);
                            for (int b = 0; b < 16; b++)
                                fprintf(stderr, " %02X",
                                        cpu->read8((base + row * 16 + b) & 0xFFFFFF));
                            fprintf(stderr, "\n");
                        }
                        /* Also dump $210 (DRIVRJT value) and the
                         * longword it points to. */
                        uint32_t drvrjt_val = cpu->read8(0x210) << 24 |
                                              cpu->read8(0x211) << 16 |
                                              cpu->read8(0x212) << 8  |
                                              cpu->read8(0x213);
                        fprintf(stderr, "P114 DRIVRJT ($210) = $%08X\n", drvrjt_val);
                    }
                }
                prev_drv_pc = cpu->pc;
            }
            if (!stack_trip_fired && cpu->pc >= 0x00CBE000 && cpu->pc < 0x00CC0000) {
                stack_trip_fired = 1;
                fprintf(stderr, "P113 STACK-JUMP: PC=$%08X A6=$%08X A7=$%08X SR=$%04X\n",
                        cpu->pc, cpu->a[6], cpu->a[7], cpu->sr);
                fprintf(stderr, "  PC ring (newest first, 256 entries, driver PCs marked *):\n");
                for (int k = 0; k < 256; k++) {
                    int idx = (pc_ring_idx - 1 - k + 256) & 0xFF;
                    uint32_t rp = pc_ring[idx];
                    const char *mark = (rp >= 0x200000 && rp < 0x210000) ? " *DRV*" : "";
                    fprintf(stderr, "    [-%3d] PC=$%08X%s\n", k+1, rp, mark);
                }
            }
        }




        /* P77 diag probes removed (session cleanup). Key findings:
         * - Build_Syslocal writes to wrong syslocal address (MMU_BASE
         *   returns $9C0000 instead of $CE0000)
         * - Semaphore wait_queue contains code addresses (e.g. BUS_ERR)
         *   instead of valid PCB pointers
         * - UNLOCKSEGS walks corrupt seglock sentinel → RELSPACE spin
         * Both are downstream of c_syslocal_ptr miscalculation. */

        /* P78b diag: trace VEC-WRITE source PCs via A6 frame chain.
         * The FS code around $02Bxxx writes to vector table during
         * SYS_PROC_INIT. Catch the first such write and dump the call stack. */
        {
            DBGSTATIC(int, p78b_armed, 0);
            DBGSTATIC(int, p78b_traced, 0);
            uint32_t spi_addr = boot_progress_lookup("SYS_PROC_INIT");
            if (spi_addr && cpu->pc == spi_addr) p78b_armed = 1;
            /* After SYS_PROC_INIT, watch for PCs in the FS code area that
             * writes to vectors. From prior probes: $02BB0E, $02BC8C,
             * $02BC9C, $02BE48, $02C6B2. */
            if (p78b_armed && !p78b_traced &&
                cpu->pc >= 0x02B000 && cpu->pc <= 0x02D000) {
                p78b_traced = 1;
                fprintf(stderr, "[P78b] FS code entered from SYS_PROC_INIT context, PC=$%06X\n",
                        cpu->pc);
                fprintf(stderr, "  D0=$%08X D1=$%08X A0=$%08X A1=$%08X A5=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->a[0], cpu->a[1], cpu->a[5]);
                fprintf(stderr, "  A6 chain:");
                uint32_t fp = cpu->a[6] & 0xFFFFFF;
                for (int i = 0; i < 12 && fp > 0x100 && fp < 0xF00000; i++) {
                    uint32_t ret = cpu_read32(cpu, (fp + 4) & 0xFFFFFF);
                    fprintf(stderr, " ret=$%06X", ret);
                    uint32_t next_fp = cpu_read32(cpu, fp & 0xFFFFFF) & 0xFFFFFF;
                    if (next_fp <= fp || next_fp == 0) break;
                    fp = next_fp;
                }
                fprintf(stderr, "\n");
            }
        }
        /* P78 probes removed — LDSN_TO_MMU and MMU_BASE work correctly
         * after the P77 EXT.L fix. Build_Syslocal's syslocal sentinel
         * stores are still not landing (separate issue in the refdb LDSN
         * lookup or WITH-block address-of computation). */

        /* P41: Cache all HLE bypass addresses by symbol name rather
         * than hardcoding post-link offsets. This makes every HLE
         * robust against codegen changes that shift the linker map
         * (e.g. adding ANDI.W instructions widens code regions). */
        static uint32_t pc_FS_CLEANUP, pc_PR_CLEANUP, pc_MEM_CLEANUP;
        static uint32_t pc_SYS_PROC_INIT, pc_excep_setup, pc_REG_OPEN_LIST;
        static uint32_t pc_QUEUE_PR, pc_GETSPACE, pc_Wait_sem, pc_MM_Setup;
        static uint32_t pc_unitio, pc_UNLOCKSEGS, pc_Signal_sem, pc_Make_File;
        static uint32_t pc_SplitPathname, pc_MAKE_SYSDATASEG;
        static uint32_t pc_MERGE_FREE;
        static int hle_pc_gen = -1;
        if (hle_pc_gen != g_emu_generation) {
            pc_FS_CLEANUP    = boot_progress_lookup("FS_CLEANUP");
            pc_PR_CLEANUP    = boot_progress_lookup("PR_CLEANUP");
            pc_MEM_CLEANUP   = boot_progress_lookup("MEM_CLEANUP");
            pc_SYS_PROC_INIT = boot_progress_lookup("SYS_PROC_INIT");
            pc_excep_setup   = boot_progress_lookup("excep_setup");
            pc_REG_OPEN_LIST = boot_progress_lookup("REG_OPEN_LIST");
            pc_QUEUE_PR      = boot_progress_lookup("QUEUE_PR");
            pc_GETSPACE      = boot_progress_lookup("GETSPACE");
            pc_Wait_sem      = boot_progress_lookup("Wait_sem");
            pc_MM_Setup      = boot_progress_lookup("MM_Setup");
            pc_unitio        = boot_progress_lookup("unitio");
            pc_UNLOCKSEGS    = boot_progress_lookup("UNLOCKSEGS");
            pc_Signal_sem    = boot_progress_lookup("Signal_sem");
            pc_Make_File     = boot_progress_lookup("Make_File");
            pc_SplitPathname = boot_progress_lookup("SplitPathname");
            pc_MAKE_SYSDATASEG = boot_progress_lookup("MAKE_SYSDATASEG");
            pc_MERGE_FREE    = boot_progress_lookup("MERGE_FREE");
            hle_pc_gen = g_emu_generation;
        }



        /* P89c — prime LOADER's `smt_adr` VAR on first SETMMU entry.
         *
         * LOADER.TEXT and STARTUP.TEXT both export `SETMMU`. Our linker's
         * "first ENTRY wins" policy picks LOADER's 4-param version because
         * LOADER compiles before STARTUP. LOADER's SETMMU reads `smt_adr`
         * (A5-6112) to find where to write SMT entries — but `smt_adr` is
         * initialized in LOADER's BOOTINIT, which we skip. So before P89c,
         * SETMMU writes origin/limit/access to `$0 + index*4`, stomping
         * the vector table; after boot progresses a little, a crashed
         * RTE/RTS loads SSP from the stomped vector 0 and the CPU dies.
         *
         * P89b (codegen) made SETMMU actually emit those writes. Without
         * this prime, the writes go to low memory. With this prime,
         * SETMMU writes to `g_hle_smt_base + index*4` (the real SMT),
         * and the TRAP6 PROG_MMU HLE reads real SOR/SLR values from
         * those entries.
         *
         * We do this lazily at SETMMU entry (not at bootrom time) because
         * PASCALINIT runs between reset and SETMMU and may clear the
         * A5-relative area. Lazy priming sidesteps the timing question. */
        {
            static uint32_t pc_SETMMU = 0;
            static int smt_prime_gen = -1;
            extern uint32_t g_hle_smt_base;
            if (smt_prime_gen != g_emu_generation) {
                pc_SETMMU = boot_progress_lookup("SETMMU");
                smt_prime_gen = g_emu_generation;
            }
            if (pc_SETMMU && cpu->pc == pc_SETMMU && g_hle_smt_base != 0) {
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                /* Only fire once A5 has been moved to the sysglobal area
                 * by PASCALINIT (A5=$CC6FFC for our memory layout). Writing
                 * at the early-reset A5=$14000 would stomp random RAM. */
                if (a5 >= 0x00CC0000 && a5 < 0x00CE0000) {
                    uint32_t slot = (a5 - 6112) & 0xFFFFFF;
                    uint32_t cur = cpu_read32(cpu, slot);
                    if (cur == 0) {
                        cpu_write32(cpu, slot, g_hle_smt_base);
                        DBGSTATIC(int, prime_count, 0);
                        if (prime_count++ < 2) {
                            fprintf(stderr, "[P89c] primed smt_adr @A5-6112 "
                                    "(log=$%06X) <= $%08X on first SETMMU entry (A5=$%06X)\n",
                                    slot, g_hle_smt_base, a5);
                        }
                    }
                }
            }
        }

        /* P83a HLE guard — MERGE_FREE must only merge when c_sdb_ptr is
         * itself a free region. Apple's source comment says "merge two
         * adjacent free regions" but the code has no guard; when
         * INSERTSDB calls MERGE_FREE(left_sdb) after the first MAKE_FREE
         * (free chain empty → the walk lands on head_sdb), the loop
         * condition evaluates TRUE because head.freechain.fwd_link was
         * just written by P_ENQUEUE to point at the new free sdb. The
         * body then does `memsize := memsize + right_sdb.memsize` inside
         * `with head_sdb^ do`, scribbling head.memsize and then TAKE_FREE
         * removes the just-inserted free sdb. Result: free chain empty,
         * head.memsize non-zero → downstream GetFree returns head as
         * "last free sdb", TAKE_FREE assertion fires SYSTEM_ERROR(10598).
         * Skip MERGE_FREE body whenever c_sdb_ptr.sdbtype != free.
         * With this guard the SYSTEM_ERROR is averted, but boot then
         * stalls in a CHECK_DS/INIT_SWAPIN loop — the next blocker. */
        if (pc_MERGE_FREE && cpu->pc == pc_MERGE_FREE) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t c_sdb = cpu_read32(cpu, sp + 4);
            if (c_sdb >= 0x00CC0000 && c_sdb < 0x00E00000) {
                uint8_t sdbtype = cpu_read8(cpu, c_sdb + 13);
                if (sdbtype != 0 /* Tsdbtype.free */) {
                    DBGSTATIC(int, mf_guard_count, 0);
                    if (mf_guard_count < 4) {
                        mf_guard_count++;
                        fprintf(stderr, "[P83a] MERGE_FREE skipped (c_sdb_ptr=$%08X sdbtype=$%02X != free)\n",
                                c_sdb, sdbtype);
                    }
                    uint32_t ret = cpu_read32(cpu, sp);
                    cpu->a[7] = (cpu->a[7] + 4) & 0xFFFFFF;
                    cpu->pc = ret;
                    continue;
                }
            }
        }

        /* P89i — Defer first INTSON until SYS_PROC_INIT has reached.
         *
         * BOOT_IO_INIT's `INTSON(0)` at source-STARTUP:1977 enables
         * interrupts before SYS_PROC_INIT has created the real system
         * processes. The timer-tick IRQ would then fire on POP (the
         * pseudo-outer-process running INITSYS) whose `env_save_area`
         * at `b_syslocal_ptr+6` was never populated — Apple doesn't
         * call CreateProcess for POP. Scheduler → Launch → RTE would
         * pop a garbage PC → hard_excep → SYSTEM_ERROR(10204).
         *
         * Part of the multilayer boot-up story: stays until (a) real
         * SYS_PROC_INIT creates proper processes with populated
         * env_save_areas AND (b) IRQ-driven driver completion is
         * wired, at which point timer IRQs before SYS_PROC_INIT no
         * longer threaten POP and this gate is naturally obsolete.
         *
         * Calling convention: our codegen's call site pushes the
         * 2-byte arg (MOVE.W D0,-(SP) = $3F00) and emits no post-JSR
         * cleanup, so INTSON is callee-clean — body would end with a
         * sequence equivalent to RTS #2. Skip path mirrors that: pop
         * 4-byte return PC, then skip the 2-byte arg (SP += 6). */
        {
            static uint32_t pc_INTSON = 0;
            static int intson_gen = -1;
            if (intson_gen != g_emu_generation) {
                pc_INTSON = boot_progress_lookup("INTSON");
                intson_gen = g_emu_generation;
            }
            if (pc_INTSON && cpu->pc == pc_INTSON &&
                !boot_progress_reached("SYS_PROC_INIT")) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] = (cpu->a[7] + 6) & 0xFFFFFF;  /* pop 4-byte ret + 2-byte arg */
                cpu->pc = ret;
                DBGSTATIC(int, intson_skip_count, 0);
                if (intson_skip_count++ < 4) {
                    fprintf(stderr, "[P89i] INTSON deferred (SYS_PROC_INIT not reached) "
                            "ret=$%06X SP=$%06X\n", ret, sp);
                }
                continue;
            }
        }

        /* P83b probe — CHECK_DS loop investigation. With the P83a guard
         * in place boot gets past FS_INIT then enters an infinite loop
         * between CHECK_DS ($043CAE) and INIT_SWAPIN ($045DFE). Log the
         * first few CHECK_DS entries to see which sdb is being swapped
         * in and why sdbstate.memoryF isn't becoming true.
         *
         * P83c extension — also probe MAKE_MRDATA entry and BLD_SEG
         * entry/exit so we can see whether BLD_SEG actually populated
         * the sdb the MAKE_MRDATA loop later calls CHECK_DS with. */
        {
            static uint32_t pc_CHECK_DS = 0, pc_MAKE_MRDATA = 0, pc_BLD_SEG = 0;
            static uint32_t pc_ALLOC_MEM = 0, pc_SWAP_SEG = 0, pc_INIT_SWAPIN = 0, pc_GET_SEG = 0;
            static int pc_gen = -1;
            if (pc_gen != g_emu_generation) {
                pc_CHECK_DS    = boot_progress_lookup("CHECK_DS");
                pc_MAKE_MRDATA = boot_progress_lookup("MAKE_MRDATA");
                pc_BLD_SEG     = boot_progress_lookup("BLD_SEG");
                pc_ALLOC_MEM   = boot_progress_lookup("ALLOC_MEM");
                pc_SWAP_SEG    = boot_progress_lookup("SWAP_SEG");
                pc_INIT_SWAPIN = boot_progress_lookup("INIT_SWAPIN");
                pc_GET_SEG     = boot_progress_lookup("GET_SEG");
                pc_gen = g_emu_generation;
            }
            DBGSTATIC(int, alloc_count, 0);
            DBGSTATIC(int, swap_count, 0);
            DBGSTATIC(int, initswap_count, 0);
            DBGSTATIC(int, getseg_count, 0);
            if (pc_ALLOC_MEM && cpu->pc == pc_ALLOC_MEM && alloc_count < 3) {
                alloc_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t sdb = cpu_read32(cpu, sp + 8); /* sig: var err; aSeg_sdb_ptr (by-val 4 bytes after err var ref) */
                uint32_t err_ref = cpu_read32(cpu, sp + 4);
                fprintf(stderr, "[P84b] ALLOC_MEM#%d ret=$%06X err_ref=$%08X sdb=$%08X\n",
                        alloc_count, cpu_read32(cpu, sp), err_ref, sdb);
            }
            if (pc_SWAP_SEG && cpu->pc == pc_SWAP_SEG && swap_count < 3) {
                swap_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                /* sig: var error; aSeg_sdb_ptr; func (readop|writeop) */
                uint32_t err_ref = cpu_read32(cpu, sp + 4);
                uint32_t sdb = cpu_read32(cpu, sp + 8);
                uint32_t func_word = cpu_read16(cpu, sp + 12);
                fprintf(stderr, "[P84b] SWAP_SEG#%d ret=$%06X err_ref=$%08X sdb=$%08X func=$%04X\n",
                        swap_count, cpu_read32(cpu, sp), err_ref, sdb, func_word);
            }
            if (pc_INIT_SWAPIN && cpu->pc == pc_INIT_SWAPIN && initswap_count < 3) {
                initswap_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t sdb = cpu_read32(cpu, sp + 4);
                fprintf(stderr, "[P84b] INIT_SWAPIN#%d ret=$%06X sdb=$%08X sdbtype=$%02X memoryF_byte=$%02X newlength=$%04X\n",
                        initswap_count, cpu_read32(cpu, sp), sdb,
                        cpu_read8(cpu, sdb + 13), cpu_read8(cpu, sdb + 14),
                        cpu_read16(cpu, sdb + 46));
                /* g_p84_trace = 1; */  /* uncomment to dump sdb-region writes */
            }
            if (pc_GET_SEG && cpu->pc == pc_GET_SEG && getseg_count < 3) {
                getseg_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t err_ref = cpu_read32(cpu, sp + 4);
                uint32_t sdb = cpu_read32(cpu, sp + 8);
                fprintf(stderr, "[P84b] GET_SEG#%d ret=$%06X err_ref=$%08X sdb=$%08X\n",
                        getseg_count, cpu_read32(cpu, sp), err_ref, sdb);
            }
            DBGSTATIC(int, check_ds_count, 0);
            DBGSTATIC(int, make_mrd_count, 0);
            DBGSTATIC(int, bld_seg_after_mrd, 0);
            DBGSTATIC(uint32_t, last_mrd_ret_sp, 0);

            if (pc_MAKE_MRDATA && cpu->pc == pc_MAKE_MRDATA && make_mrd_count < 3) {
                make_mrd_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc   = cpu_read32(cpu, sp);
                uint32_t errnum_p = cpu_read32(cpu, sp + 4);
                uint32_t size     = cpu_read32(cpu, sp + 8);
                uint32_t addr_p   = cpu_read32(cpu, sp + 12);
                fprintf(stderr, "[P83c] MAKE_MRDATA#%d entry ret=$%06X errnum=$%08X size=$%08X addr=$%08X\n",
                        make_mrd_count, ret_pc, errnum_p, size, addr_p);
                last_mrd_ret_sp = ret_pc;
                bld_seg_after_mrd = 1;
                /* g_p84_trace = 1;  // enable write trace for the sdb region (noisy) */
            }

            if (pc_BLD_SEG && cpu->pc == pc_BLD_SEG && bld_seg_after_mrd) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc = cpu_read32(cpu, sp);
                /* BLD_SEG(kind, csize, newsize, disca, discspace, pkedLen, var sdb) */
                uint8_t  kind     = cpu_read8(cpu, sp + 7);   /* Tsdbtype on stack, low byte of word */
                uint32_t csize    = cpu_read32(cpu, sp + 8);
                uint16_t newsize  = cpu_read16(cpu, sp + 12);
                uint32_t sdb_ref  = cpu_read32(cpu, sp + 14 + 6 + 4 + 4); /* var param — last */
                /* Back off: we don't know exact stack layout for BLD_SEG
                 * params with addrdisc (6 bytes) in the middle. Instead,
                 * just log the top-of-stack for manual decoding. */
                fprintf(stderr, "[P83c] BLD_SEG entry (post-MAKE_MRDATA) ret=$%06X kind=$%02X csize=$%08X newsize=$%04X\n",
                        ret_pc, kind, csize, newsize);
                fprintf(stderr, "       stack@+0..+32: ");
                for (int i = 0; i <= 32; i += 4) {
                    fprintf(stderr, "%08X ", cpu_read32(cpu, sp + i));
                }
                fprintf(stderr, "\n");
                (void)sdb_ref;
                bld_seg_after_mrd = 0; /* only log first one after each MAKE_MRDATA */
            }

            if (pc_CHECK_DS && cpu->pc == pc_CHECK_DS && check_ds_count < 3) {
                check_ds_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc = cpu_read32(cpu, sp);
                uint32_t c_sdb = cpu_read32(cpu, sp + 4);
                fprintf(stderr, "[P83b] CHECK_DS#%d entry ret=$%06X c_sdb_ptr=$%08X\n",
                        check_ds_count, ret_pc, c_sdb);
                if (c_sdb >= 0x00AA0000 && c_sdb < 0x00E00000) {
                    /* sdbstate is variant-header offset 14. memoryF is
                     * bit 7 of the first byte (per MMASM: MEMORYF=7). */
                    uint8_t sdbstate0 = cpu_read8(cpu, c_sdb + 14);
                    fprintf(stderr, "      c_sdb: memaddr=$%04X memsize=$%04X sdbtype=$%02X sdbstate[0]=$%02X (memoryF=bit7) newlength=$%04X\n",
                            cpu_read16(cpu, c_sdb + 8),
                            cpu_read16(cpu, c_sdb + 10),
                            cpu_read8(cpu,  c_sdb + 13),
                            sdbstate0,
                            cpu_read16(cpu, c_sdb + 32));
                    /* Dump full 48 bytes of sdb so we can look for
                     * offset-shift bugs (maybe data is there but at
                     * different offsets than our probe assumes). */
                    fprintf(stderr, "      c_sdb bytes: ");
                    for (int i = 0; i < 48; i++) {
                        fprintf(stderr, "%02X", cpu_read8(cpu, c_sdb + i));
                        if ((i & 3) == 3) fprintf(stderr, " ");
                    }
                    fprintf(stderr, "\n");
                    /* Dump bytes around caller return PC so we can see
                     * which offset MAKE_MRDATA's while-condition reads
                     * for sdbstate.memoryF. ret_pc is the insn AFTER
                     * the JSR, so the JSR + loop test is just before. */
                    if (ret_pc >= 0x010800 && ret_pc < 0x010A00) {
                        uint32_t dump_start = ret_pc - 32;
                        fprintf(stderr, "      caller insn bytes @$%06X..$%06X: ",
                                dump_start, ret_pc + 16);
                        for (uint32_t i = 0; i < 48; i++) {
                            fprintf(stderr, "%02X", cpu_read8(cpu, dump_start + i));
                            if ((i & 1) == 1) fprintf(stderr, " ");
                            if ((i & 15) == 15) fprintf(stderr, " ");
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
        }

        /* P85 probe — FlushNodes buffer-pool spin investigation.
         * After the P84 CHECK_DS fix, FS_INIT reaches and then spins
         * in FlushNodes (VMSTUFF:1369) for tens of thousands of iters.
         * The `until ptrS = ptrHot` loop never hits ptrHot. Either
         * InitBufPool didn't run, or the circular doubly-linked list
         * is broken, or link.b is compiled at the wrong offset.
         *
         * ptrHot's A5 offset = -$2066 (from disasm of $064EC2:
         * `202D DF9A` = MOVE.L -$2066(A5),D0). Buffer layout:
         *   link.f @0 (ptr,4), link.b @4 (ptr,4),
         *   dirty @8 (1), lock @9 (1), device @10 (int,2),
         *   page @12 (long,4), sem @16 (record), Data (variable). */
        {
            static uint32_t pc_FlushNodes = 0, pc_InitBufPool = 0, pc_InitBuf = 0;
            static uint32_t pc_MAP_SEGMENT = 0, pc_PROG_MMU = 0;
            static uint32_t pc_QUEUE_PR = 0;
            static uint32_t pc_RELSPACE = 0, pc_GETSPACE_p85 = 0;
            static int pc_gen_p85 = -1;
            if (pc_gen_p85 != g_emu_generation) {
                pc_FlushNodes = boot_progress_lookup("FlushNodes");
                pc_InitBufPool = boot_progress_lookup("InitBufPool");
                pc_InitBuf = boot_progress_lookup("InitBuf");
                pc_MAP_SEGMENT = boot_progress_lookup("MAP_SEGMENT");
                pc_PROG_MMU = boot_progress_lookup("PROG_MMU");
                pc_QUEUE_PR = boot_progress_lookup("QUEUE_PR");
                pc_RELSPACE = boot_progress_lookup("RELSPACE");
                pc_GETSPACE_p85 = boot_progress_lookup("GETSPACE");
                pc_gen_p85 = g_emu_generation;
            }
            DBGSTATIC(int, p85_initbuf_count, 0);
            DBGSTATIC(int, p85_flush_count, 0);
            DBGSTATIC(int, p85_ib_count, 0);
            DBGSTATIC(int, p85_ms_count, 0);
            DBGSTATIC(int, p85_pm_count, 0);
            DBGSTATIC(int, p85_qp_count, 0);
            DBGSTATIC(int, p85_rel_count, 0);
            DBGSTATIC(int, p85_gs_count, 0);

            /* RELSPACE(ordaddr: absptr; b_area: absptr). Probe fires at LINK
             * entry before A6 is set up, so read args from SP: sp+0=ret,
             * sp+4=ordaddr (pushed last by caller), sp+8=b_area.
             *
             * P85d HLE GUARD: if ordaddr = badptr1 ($414231), the caller
             * passed the "no device mounted / not initialized" sentinel —
             * Apple's def_unmount doesn't guard MDDFdata against badptr1
             * before calling relspace, which is fine on real HW where mount
             * always completes before unmount, but not in our init flow
             * where HLE'd boot-device handling (10738) short-circuits mount.
             * Skip the call and return cleanly with the 8 bytes of args
             * still to be popped by the caller's ADDQ #8,A7. */
            if (pc_RELSPACE && cpu->pc == pc_RELSPACE) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t ordaddr = cpu_read32(cpu, sp + 4);
                uint32_t b_area  = cpu_read32(cpu, sp + 8);
                if (p85_rel_count < 12) {
                    p85_rel_count++;
                    uint32_t c_pool_ptr = 0;
                    if (b_area >= 0x000400 && b_area < 0x00F00000)
                        c_pool_ptr = cpu_read32(cpu, b_area);
                    fprintf(stderr, "[P85] RELSPACE#%d ret=$%06X ordaddr=$%08X b_area=$%08X *b_area=$%08X A5=$%06X\n",
                            p85_rel_count, ret, ordaddr, b_area, c_pool_ptr,
                            cpu->a[5] & 0xFFFFFF);
                }
                if (ordaddr == 0x00414231u) {
                    static int p85d_skip_count = 0;
                    if (p85d_skip_count++ < 4)
                        fprintf(stderr, "[P85d] RELSPACE guard: skipping ordaddr=badptr1 ret=$%06X\n", ret);
                    /* Pop ret + restore PC + advance. Leave args on stack for
                     * caller's ADDQ #8,A7 (both call sites do this). */
                    cpu->pc = ret;
                    cpu->a[7] = (cpu->a[7] + 4) & 0xFFFFFF;
                    continue;
                }
            }

            /* GETSPACE(amount: int2; b_area: absptr; VAR ordaddr): boolean.
             * Pascal convention: caller pushes right-to-left, so amount (2 bytes)
             * is on top: sp+0=ret, sp+4=amount (int2 word), sp+6=b_area (4),
             * sp+10=@ordaddr (4). */
            if (pc_GETSPACE_p85 && cpu->pc == pc_GETSPACE_p85 && p85_gs_count < 12) {
                p85_gs_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint16_t amount = cpu_read16(cpu, sp + 4);
                uint32_t b_area = cpu_read32(cpu, sp + 6);
                uint32_t pordaddr = cpu_read32(cpu, sp + 10);
                uint32_t c_pool_ptr = 0;
                if (b_area >= 0x000400 && b_area < 0x00F00000)
                    c_pool_ptr = cpu_read32(cpu, b_area);
                fprintf(stderr, "[P85] GETSPACE#%d ret=$%06X amount=%u b_area=$%08X *b_area=$%08X &ordaddr=$%08X A5=$%06X\n",
                        p85_gs_count, ret, amount, b_area, c_pool_ptr, pordaddr,
                        cpu->a[5] & 0xFFFFFF);
            }

            /* QUEUE_PR is an asm proc (source-PROCASM.TEXT) that pops
             * D0=retaddr, D1.B=queue, A1=pcb_ptr from the stack before
             * running. At PC=entry A7 still holds all three. PCB layout:
             *   @0: next_schedPtr (4), @4: prev_schedPtr (4),
             *   @8: semwait_queue (4), @12: priority (1), @13: norm_pri (1). */
            if (pc_QUEUE_PR && cpu->pc == pc_QUEUE_PR && p85_qp_count < 8) {
                p85_qp_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint16_t queue_w = cpu_read16(cpu, sp + 4);  /* byte in low half */
                uint32_t pcb = cpu_read32(cpu, sp + 6);
                fprintf(stderr, "[P85] QUEUE_PR#%d ret=$%06X queue=$%04X pcb=$%08X",
                        p85_qp_count, ret, queue_w, pcb);
                if (pcb >= 0x000400 && pcb < 0x00F00000) {
                    uint32_t next_sch = cpu_read32(cpu, pcb + 0);
                    uint32_t prev_sch = cpu_read32(cpu, pcb + 4);
                    uint16_t w12      = cpu_read16(cpu, pcb + 12);  /* asm reads this */
                    uint8_t  b12      = cpu_read8 (cpu, pcb + 12);
                    uint8_t  b13      = cpu_read8 (cpu, pcb + 13);
                    uint8_t  b14      = cpu_read8 (cpu, pcb + 14);
                    uint8_t  b15      = cpu_read8 (cpu, pcb + 15);
                    fprintf(stderr, " next=$%08X prev=$%08X W@12=$%04X b[12..15]=%02X %02X %02X %02X\n",
                            next_sch, prev_sch, w12, b12, b13, b14, b15);
                } else {
                    fprintf(stderr, " (pcb out of range)\n");
                }
            }

            if (pc_MAP_SEGMENT && cpu->pc == pc_MAP_SEGMENT && p85_ms_count < 5) {
                p85_ms_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t c_sdb = cpu_read32(cpu, sp + 4);
                uint16_t c_mmu = cpu_read16(cpu, sp + 8);
                uint16_t domain = cpu_read16(cpu, sp + 10);
                uint16_t access = cpu_read16(cpu, sp + 12);
                uint16_t memsize = c_sdb ? cpu_read16(cpu, c_sdb + 10) : 0;
                fprintf(stderr, "[P85] MAP_SEGMENT#%d entry ret=$%06X c_sdb=$%08X c_mmu=%d domain=%d access=$%04X sdb.memsize=%d\n",
                        p85_ms_count, ret, c_sdb, c_mmu, domain, access, memsize);
            }
            if (pc_PROG_MMU && cpu->pc == pc_PROG_MMU && p85_pm_count < 40) {
                p85_pm_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                /* PROG_MMU is external asm — args pushed left-to-right
                 * (index first/deepest, ret_domain last/top). */
                uint16_t rdom  = cpu_read16(cpu, sp + 4);
                uint16_t cnt   = cpu_read16(cpu, sp + 6);
                uint16_t tdom  = cpu_read16(cpu, sp + 8);
                uint16_t index = cpu_read16(cpu, sp + 10);
                fprintf(stderr, "[P85] PROG_MMU#%d entry ret=$%06X index=%d tdom=%d count=%d rdom=%d\n",
                        p85_pm_count, ret, index, tdom, cnt, rdom);
            }

            if (pc_InitBuf && cpu->pc == pc_InitBuf && p85_ib_count < 5) {
                p85_ib_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret   = cpu_read32(cpu, sp);
                uint32_t ptrB  = cpu_read32(cpu, sp + 4);
                uint32_t predB = cpu_read32(cpu, sp + 8);
                uint32_t succB = cpu_read32(cpu, sp + 12);
                fprintf(stderr, "[P85] InitBuf#%d entry ret=$%06X ptrB=$%08X predB=$%08X succB=$%08X\n",
                        p85_ib_count, ret, ptrB, predB, succB);
            }

            if (pc_InitBufPool && cpu->pc == pc_InitBufPool && p85_initbuf_count < 2) {
                p85_initbuf_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                /* Post-P86 linker fix: ptrCold at -8296, ptrHot at -8292. */
                uint32_t ptrHot_addr  = (a5 - 8292) & 0xFFFFFF;
                uint32_t ptrCold_addr = (a5 - 8296) & 0xFFFFFF;
                uint32_t ptrHot_val  = cpu_read32(cpu, ptrHot_addr);
                uint32_t ptrCold_val = cpu_read32(cpu, ptrCold_addr);
                fprintf(stderr, "[P85] InitBufPool#%d entry ret=$%06X A5=$%06X ptrHot@$%06X=$%08X ptrCold@$%06X=$%08X\n",
                        p85_initbuf_count, ret, a5,
                        ptrHot_addr, ptrHot_val, ptrCold_addr, ptrCold_val);
            }

            if (pc_FlushNodes && cpu->pc == pc_FlushNodes && p85_flush_count < 2) {
                p85_flush_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint16_t device = cpu_read16(cpu, sp + 4);
                uint16_t clear  = cpu_read16(cpu, sp + 6);
                uint32_t ecode_ref = cpu_read32(cpu, sp + 8);
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                /* Post-P86 linker fix: ptrCold at -8296, ptrHot at -8292. */
                uint32_t ptrHot_addr  = (a5 - 8292) & 0xFFFFFF;
                uint32_t ptrCold_addr = (a5 - 8296) & 0xFFFFFF;
                uint32_t ptrHot_val  = cpu_read32(cpu, ptrHot_addr);
                uint32_t ptrCold_val = cpu_read32(cpu, ptrCold_addr);
                fprintf(stderr, "[P85] FlushNodes#%d entry ret=$%06X device=$%04X clear=$%04X ecode_ref=$%08X A5=$%06X ptrHot=$%08X ptrCold=$%08X\n",
                        p85_flush_count, ret, device, clear, ecode_ref, a5, ptrHot_val, ptrCold_val);
                if (ptrHot_val >= 0x000400 && ptrHot_val < 0x00F00000) {
                    uint32_t cur = ptrHot_val;
                    for (int hop = 0; hop < 20; hop++) {
                        if (hop > 0 && cur == ptrHot_val) {
                            fprintf(stderr, "       *** looped back to ptrHot after %d hops ***\n", hop);
                            break;
                        }
                        uint32_t link_f = cpu_read32(cpu, cur + 0);
                        uint32_t link_b = cpu_read32(cpu, cur + 4);
                        uint8_t  dirty  = cpu_read8 (cpu, cur + 8);
                        uint8_t  lock_  = cpu_read8 (cpu, cur + 9);
                        uint16_t dev_f  = cpu_read16(cpu, cur + 10);
                        uint32_t page_f = cpu_read32(cpu, cur + 12);
                        fprintf(stderr, "       hop %2d @$%06X: f=$%08X b=$%08X dirty=$%02X lock=$%02X dev=$%04X page=$%08X\n",
                                hop, cur, link_f, link_b, dirty, lock_, dev_f, page_f);
                        if (link_b < 0x000400 || link_b >= 0x00F00000) {
                            fprintf(stderr, "       *** link.b out of range, stopping walk ***\n");
                            break;
                        }
                        cur = link_b;
                    }
                } else {
                    fprintf(stderr, "       ptrHot looks unset/invalid — pool probably not initialized\n");
                }
            }
        }

        /* P86 — probes for post-FS_INIT flow. The boot now runs 300 frames
         * cleanly but never reaches SYS_PROC_INIT. Execution ends up at
         * user-space PCs ($0118xxxx) spinning on F-line traps. Three
         * possibilities:
         *   (a) BOOT_IO_INIT never returns to INITSYS (for-loop hangs)
         *   (b) INITSYS returns but doesn't call SYS_PROC_INIT (codegen
         *       skipped the call, or took a different path)
         *   (c) Scheduler dispatches a garbage process whose env_save_area
         *       is corrupt (c_pcb_ptr or b_syslocal_ptr points wrong)
         * Probe at INITSYS-body-post-BOOT_IO_INIT, Sys_Proc_Init entry,
         * Scheduler entry, and Launch entry to localize. */
        {
            static uint32_t pc_Scheduler = 0, pc_Launch = 0, pc_Sys_Proc_Init = 0;
            static uint32_t pc_INITSYS = 0;
            static int pc_gen_p86 = -1;
            if (pc_gen_p86 != g_emu_generation) {
                pc_Scheduler     = boot_progress_lookup("Scheduler");
                pc_Launch        = boot_progress_lookup("Launch");
                pc_Sys_Proc_Init = boot_progress_lookup("Sys_Proc_Init");
                pc_INITSYS       = boot_progress_lookup("INITSYS");
                pc_gen_p86 = g_emu_generation;
            }
            DBGSTATIC(int, p86_sched_count, 0);
            DBGSTATIC(int, p86_launch_count, 0);
            DBGSTATIC(int, p86_spi_count, 0);

            /* Scheduler entry — log c_pcb_ptr, b_syslocal_ptr, invoke_sched.
             * Scheduler is at $05DAE8 per map. It saves caller state, calls
             * SelectProcess, then Launch. */
            if (pc_Scheduler && cpu->pc == pc_Scheduler && p86_sched_count < 6) {
                p86_sched_count++;
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                uint32_t cpcb = cpu_read32(cpu, (a5 - 24617) & 0xFFFFFF);
                uint32_t bsl  = cpu_read32(cpu, (a5 - 24785) & 0xFFFFFF);
                uint8_t  isch = cpu_read8 (cpu, (a5 - 24786) & 0xFFFFFF);
                fprintf(stderr, "[P86] Scheduler#%d entry A5=$%06X A6=$%06X A7=$%06X c_pcb=$%08X b_syslocal=$%08X inv_sched=$%02X\n",
                        p86_sched_count, a5, cpu->a[6]&0xFFFFFF, cpu->a[7]&0xFFFFFF,
                        cpcb, bsl, isch);
            }

            /* Launch entry — $06EAB0. Dump everything that SETREGS reads,
             * especially b_syslocal_ptr and env_save_area. */
            if (pc_Launch && cpu->pc == pc_Launch && p86_launch_count < 4) {
                p86_launch_count++;
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                uint32_t sglobal = cpu_read32(cpu, 0x200);
                /* b_syslocal_ptr is at A5-24785, but Launch actually reads it
                 * via SGLOBAL+B_SYSLOC offset. SGLOBAL = b_sysglobal_ptr. */
                uint32_t bsl = cpu_read32(cpu, (a5 - 24785) & 0xFFFFFF);
                uint32_t cpcb = cpu_read32(cpu, (a5 - 24617) & 0xFFFFFF);
                fprintf(stderr, "[P86] Launch#%d entry A6=$%06X A5=$%06X SGLOBAL=$%08X b_syslocal=$%08X c_pcb=$%08X\n",
                        p86_launch_count, cpu->a[6]&0xFFFFFF, a5, sglobal, bsl, cpcb);
                /* If b_syslocal is valid, dump env_save_area. ENV_SAVE offset
                 * in syslocal record depends on PASCALDEFS. Dump first 64 bytes
                 * of syslocal so we can see what's there. */
                if (bsl >= 0x400 && bsl < 0x00F00000) {
                    fprintf(stderr, "       syslocal@%06X first 64 bytes:\n       ", bsl);
                    for (int i = 0; i < 64; i++) {
                        fprintf(stderr, "%02X", cpu_read8(cpu, (bsl + i) & 0xFFFFFF));
                        if ((i & 3) == 3) fprintf(stderr, " ");
                    }
                    fprintf(stderr, "\n");
                }
                /* Dump PCB to see if c_pcb_ptr is valid and what its
                 * slocal_sdbRP / domain is. */
                if (cpcb >= 0x400 && cpcb < 0x00F00000) {
                    fprintf(stderr, "       c_pcb@%06X first 32 bytes:\n       ", cpcb);
                    for (int i = 0; i < 32; i++) {
                        fprintf(stderr, "%02X", cpu_read8(cpu, (cpcb + i) & 0xFFFFFF));
                        if ((i & 3) == 3) fprintf(stderr, " ");
                    }
                    fprintf(stderr, "\n");
                }
            }

            /* Sys_Proc_Init entry — $0057CC. If this fires, INITSYS IS
             * reaching it, so the issue is in its body. If not, we never
             * return from BOOT_IO_INIT. */
            if (pc_Sys_Proc_Init && cpu->pc == pc_Sys_Proc_Init && p86_spi_count < 2) {
                p86_spi_count++;
                fprintf(stderr, "[P86] Sys_Proc_Init#%d ENTERED from ret=$%08X A6=$%06X A5=$%06X\n",
                        p86_spi_count,
                        cpu_read32(cpu, cpu->a[7] & 0xFFFFFF),
                        cpu->a[6]&0xFFFFFF, cpu->a[5]&0xFFFFFF);
            }

        }

        /* P86e HLE guard: DEL_MMLIST spin when SRB list is empty.
         *
         * MEM_CLEANUP calls Del_SRB twice to remove the pseudo-outer
         * process from SRB lists of shrsegmmu and IUDsdb. Del_SRB computes
         * c_mmlist = c_sdb_ptr->srbRP + b_sysglobal_ptr and calls
         * DEL_MMLIST(c_mmlist, c_pcb).
         *
         * If srbRP is 0 (no SRBs on this SDB — which is our state during
         * boot because no one ever added to these SRB lists), c_mmlist =
         * b_sysglobal_ptr = $CC6FFC. DEL_MMLIST's repeat-until reads
         * chain.fwd_link at b_sysglobal_ptr+offset, which is arbitrary
         * globals data. f_mmlist never comes back to c_mmlist and the
         * loop spins forever.
         *
         * Apple's source doesn't guard against this either — on real
         * hardware srbRP is always populated before Del_SRB runs, because
         * ADDTO_SRB is called during process setup paths we haven't fully
         * wired up. The right long-term fix is to make sure srbRP is
         * initialized; the short-term is to detect the degenerate case
         * and return early.
         *
         * Heuristic: if c_mmlist (arg 1) equals b_sysglobal_ptr, the list
         * was empty. Return immediately (no-op, matches "nothing to
         * delete"). */
        {
            static uint32_t pc_DEL_MMLIST = 0;
            static int pc_gen_p86e = -1;
            if (pc_gen_p86e != g_emu_generation) {
                pc_DEL_MMLIST = boot_progress_lookup("DEL_MMLIST");
                pc_gen_p86e = g_emu_generation;
            }
            if (pc_DEL_MMLIST && cpu->pc == pc_DEL_MMLIST) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t c_mmlist = cpu_read32(cpu, sp + 4);
                uint32_t entry    = cpu_read32(cpu, sp + 8);
                uint32_t sgbase = cpu_read32(cpu, 0x200);  /* b_sysglobal_ptr */
                /* mmlist.chain.fwd_link is a 2-byte offset at record
                 * offset 0. If 0, list has no next entry — treat as
                 * empty and return. Also catch the c_mmlist==sgbase
                 * case (srbRP was 0). */
                uint16_t fwd_link = 0;
                bool mmlist_valid = (c_mmlist >= 0x000400 && c_mmlist < 0x00F00000);
                if (mmlist_valid)
                    fwd_link = cpu_read16(cpu, c_mmlist);
                bool empty = (c_mmlist == sgbase) || (mmlist_valid && fwd_link == 0);
                if (empty) {
                    DBGSTATIC(int, p86e_skip, 0);
                    if (p86e_skip++ < 4)
                        fprintf(stderr, "[P86e] DEL_MMLIST guard: empty SRB list "
                                "(c_mmlist=$%06X fwd_link=$%04X sgbase=$%06X), ret=$%06X\n",
                                c_mmlist, fwd_link, sgbase, ret);
                    cpu->a[7] = (sp + 4) & 0xFFFFFF;
                    cpu->pc = ret;
                    (void)entry;
                    continue;
                }
            }
        }

        /* P37 HLE bypass: FS_CLEANUP (fsinit.text:136). Fires the
         * milestone on entry then bypasses — its body crashes into
         * $F8xxxx wild-PC space because downstream calls hit a
         * code-corruption / miscompile region that P31 didn't
         * catch. No args. */
        if (pc_FS_CLEANUP && cpu->pc == pc_FS_CLEANUP) {
            boot_progress_record_pc(cpu->pc);
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret = cpu_read32(cpu, sp);
            cpu->a[7] += 4;
            cpu->pc = ret;
            continue;
        }
        /* P38 HLE bypass: PR_CLEANUP (STARTUP:2082). No args.
         * Real proc unlinks c_pcb_ptr from the Ready queue (which crashes
         * since c_pcb_ptr is nil during boot) then enters the scheduler.
         * Skip the PCB-cleanup preamble but DO enter the scheduler — that's
         * the whole point of getting this far: let it dispatch MemMgr. */
        if (0 && pc_PR_CLEANUP && cpu->pc == pc_PR_CLEANUP) {
            /* P81c: PR_CLEANUP HLE bypass DISABLED. With CreateProcess
             * native, c_pcb_ptr is a valid PCB; the natural PR_CLEANUP
             * body unlinks it correctly and enters the scheduler. */
            boot_progress_record_pc(cpu->pc);
            static uint32_t pc_enter_sched = 0;
            if (!pc_enter_sched) {
                /* Jump straight to the Pascal Scheduler body, skipping
                 * ENTER_SC's TRAP #2 dance and SCHDTRAP's save-state (we
                 * nil-ed c_pcb_ptr below so there's nothing to save
                 * anyway). The stack and A5 are already correct for the
                 * jump. */
                /* Jump straight to the Pascal Scheduler body, skipping
                 * ENTER_SC's TRAP #2 dance. Note: the map's Scheduler
                 * symbol at $05B832 is the segment's reentry RTS trampoline
                 * — a few bytes further (past the BRA) is the actual
                 * prologue. Either works since Scheduler's prologue only
                 * needs the current A5/A6 to be sane. */
                pc_enter_sched = boot_progress_lookup("Scheduler");
                if (!pc_enter_sched) pc_enter_sched = 0x05B832;
                fprintf(stderr, "  DEBUG: Scheduler lookup → $%06X\n", pc_enter_sched);
            }
            /* Unlink c_pcb (STARTUP's pseudo-process) from the ready queue so
             * the scheduler doesn't try to Launch it — it has no valid env
             * save area, and the real PR_CLEANUP body normally does this
             * before entering the scheduler. */
            uint32_t a5 = cpu->a[5] & 0xFFFFFF;
            uint32_t ready_head = (a5 - 1116) & 0xFFFFFF;  /* @fwd_ReadyQ */
            /* Walk the ready queue and unlink STARTUP's pseudo-PCB (priority
             * 255 with need_mem=false), plus any orphan PCBs whose env_save
             * area wasn't set up — we only want MemMgr and Root. Our Pascal
             * compiler's global layout isn't byte-for-byte compatible with
             * PASCALDEFS so locating c_pcb_ptr by fixed A5 offset was off by
             * a couple of bytes; scanning is more robust. */
            uint32_t cur = cpu_read32(cpu, ready_head) & 0xFFFFFF;
            int guard = 0;
            while (cur && cur != ready_head && guard++ < 16) {
                uint32_t next = cpu_read32(cpu, cur + 0) & 0xFFFFFF;
                int pri = cpu_read8(cpu, cur + 12);  /* 1-byte subrange 0..255 */
                /* Remove STARTUP's pseudo c_pcb (priority 255) so the
                 * scheduler doesn't try to Launch it — its env_save_area
                 * was never populated. Keep MemMgr (250), Root (230), and
                 * the sentinel (0). */
                if (pri == 255) {
                    uint32_t prev = cpu_read32(cpu, cur + 4) & 0xFFFFFF;
                    if (!prev) prev = ready_head;
                    cpu_write32(cpu, prev + 0, next);
                    cpu_write32(cpu, next + 4, prev);
                    fprintf(stderr, "  [PR_CLEANUP HLE] unlinked pri-255 PCB "
                            "$%06X (prev=$%06X next=$%06X)\n", cur, prev, next);
                }
                cur = next;
            }
            /* Pop PR_CLEANUP's return address — scheduler never returns. */
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            cpu_read32(cpu, sp);
            cpu->a[7] += 4;
            cpu->pc = pc_enter_sched;
            continue;
        }
        /* P80h3: HLE SelectProcess. Our Pascal compiler's codegen for
         * exit(SelectProcess) inside SelectProcess emits
         * `MOVEA.L (A6),A6; UNLK A6; RTS`, which walks the static link
         * one level up and then UNLK's the WRONG frame — A7 jumps into
         * garbage and RTS pops 0 → PC=0 crash. Rather than rewrite the
         * codegen (turned out to have subtle interactions with
         * depth-1/2 procs that the OS relies on), emulate SelectProcess
         * directly: pick the highest-priority ready PCB, write it to
         * Scheduler's `candidate` local at -6(A6), then RTS.
         *
         * At entry: cpu->a[6] = Scheduler's A6 (SelectProcess's LINK
         * hasn't executed yet since we intercept on its first byte).
         * [A7] = return PC pushed by JSR. */
        {
            static uint32_t pc_select = 0;
            static int select_probed = 0;
            if (!select_probed) {
                select_probed = 1;
                pc_select = boot_progress_lookup("SelectProcess");
                if (pc_select)
                    fprintf(stderr, "  DEBUG: SelectProcess → $%06X\n", pc_select);
            }
            if (0 && pc_select && cpu->pc == pc_select) {
                /* P81c: HLE-SelectProcess DISABLED. The exit(SelectProcess)
                 * codegen issue that motivated it turns out to be masked
                 * by the call chain at boot: natural SelectProcess body
                 * runs correctly now with CreateProcess native and all
                 * upstream HLEs off. */
                uint32_t a6 = cpu->a[6] & 0xFFFFFF;
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                uint32_t head = (a5 - 1116) & 0xFFFFFF;   /* @fwd_ReadyQ */
                /* Walk the ready queue, pick first PCB with priority > 0
                 * and (priority < 255) — skip c_pcb (255) if it's still
                 * lingering, and skip the sentinel (0). */
                uint32_t candidate = 0;
                uint32_t cur = cpu_read32(cpu, head) & 0xFFFFFF;
                int safety = 0;
                while (cur && cur != head && safety++ < 32) {
                    int pri = cpu_read8(cpu, cur + 12);
                    if (pri > 0 && pri < 255) { candidate = cur; break; }
                    cur = cpu_read32(cpu, cur) & 0xFFFFFF;
                }
                /* Write candidate to Scheduler's local at -6(A6). */
                cpu_write32(cpu, (a6 - 6) & 0xFFFFFF, candidate);
                /* Also update b_syslocal_ptr so Launch's SETREGS reads
                 * env_save_area from the right syslocal. b_syslocal_ptr
                 * is at A5-1389 per the linker map. */
                if (candidate) {
                    uint32_t sloc = 0;
                    for (int i = 0; i < pcb_sloc_count; i++) {
                        if (pcb_sloc_map[i].pcb == candidate) {
                            sloc = pcb_sloc_map[i].sloc;
                            break;
                        }
                    }
                    if (sloc) {
                        /* Launch reads b_syslocal_ptr via B_SYSLOCAL_PTR
                         * (PASCALDEFS -24785) from the sysglobal base
                         * register. Our runtime diagnostics confirm the
                         * slot is at A5-24785. */
                        cpu_write32(cpu, (a5 - 24785) & 0xFFFFFF, sloc);
                        fprintf(stderr, "  [HLE-SelectProcess] "
                                "b_syslocal_ptr ← $%06X\n", sloc);
                    }
                }
                DBGSTATIC(int, sp_count, 0);
                if (sp_count++ < 5)
                    fprintf(stderr, "[HLE-SelectProcess #%d] "
                            "candidate=$%06X (pri=%d) → [A6-6]=$%06X\n",
                            sp_count, candidate,
                            candidate ? cpu_read8(cpu, candidate + 12) : 0,
                            (a6 - 6) & 0xFFFFFF);
                /* RTS: pop return PC from [A7]. */
                uint32_t sp_ret = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp_ret);
                cpu->a[7] += 4;
                cpu->pc = ret;
                continue;
            }
        }
        /* P81c: MEM_CLEANUP HLE bypass DISABLED. With SYS_PROC_INIT
         * running natively, its stksdb_ptr+slsdb_ptr args are now
         * properly initialized, so the real MEM_CLEANUP body works. */
        /* P35 HLE bypass: SYS_PROC_INIT (STARTUP:2042). Creates the
         * MemMgr and Root system processes. Each Make_SProcess call
         * cascades through MM_Setup → init_proc_syslocal → crea_ecb,
         * all of which fail because per-process syslocal areas
         * aren't being initialized correctly (Pascal-vs-asm field
         * offset class). Bypass entirely for now — boot may reach
         * INIT_DRIVER_SPACE / FS_CLEANUP without functional system
         * processes. No args (parameterless). */
        /* P43 HLE bypass: Wait_sem (procprims:721).
         * Signature: procedure Wait_sem(var this_sem: semaphore; control: sem_control)
         * Stack: retPC(4) + this_sem_ptr(4) + control(2 — set-of-2 enum).
         * Real body decrements sem_count, may block, may raise priority.
         * Our Pascal codegen's byte-subrange MOVE.B leaves stale D0 upper
         * bits → `if priority < semPriority` incorrectly takes true branch,
         * which writes $E2 into priority byte, which combined with norm_pri
         * byte $FF at offset+1 gives RQSCAN a word-read of $E2FF and a
         * never-terminating signed BLE loop.
         *
         * Tactical bypass: at boot-init, semaphores aren't contended — skip
         * the entire body. Decrement sem_count in place so paired Signal_sem
         * operations don't drift over time. */
        /* P44 HLE bypass: MM_Setup (load2.text:637). Per-process syslocal
         * setup during Make_SProcess. Body touches slocal_sdbRP chains that
         * aren't set up correctly in our source-compiled OS. For init phase
         * we just want Make_SProcess to return a "working enough" PCB so
         * STARTUP main can proceed to INIT_DRIVER_SPACE etc. Signature:
         *   (stk_refnum, sloc_refnum : int2;    { 2 + 2 = 4 }
         *    jt_ptr : ptr_JumpTable;            { 4 }
         *    plcbRP : relptr;                   { 2 }
         *    var sl_sdbRP : relptr);            { 4 ptr }
         * Total args = 16 bytes. Callee-clean. */
        /* P81c: MM_Setup HLE bypass DISABLED. "Body touches slocal_sdbRP
         * chains that aren't set up correctly" was static-link related.
         * Natural body works with the ABI fix in place. */
        if (0 && pc_MM_Setup && cpu->pc == pc_MM_Setup) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret = cpu_read32(cpu, sp);
            uint32_t varptr = cpu_read32(cpu, sp + 4);
            if (varptr >= 0x400 && varptr < 0xFE0000)
                cpu_write16(cpu, varptr, 0);
            cpu->a[7] += 4 + 16;
            cpu->pc = ret;
            continue;
        }
        /* P81a: Wait_sem HLE bypass DISABLED. The "byte-subrange MOVE.B
         * leaves stale D0 upper bits" was fixed in P80h2 (zero-extend).
         * The semaphore's priority/norm_pri layout is now consistent
         * (byte-packed priority in both CreateProcess HLE and RQSCAN).
         * Let the real body run. */
        if (0 && pc_Wait_sem && cpu->pc == pc_Wait_sem) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret = cpu_read32(cpu, sp);
            uint32_t this_sem = cpu_read32(cpu, sp + 4);
            if (this_sem >= 0x400 && this_sem < 0xFE0000) {
                int16_t cnt = (int16_t)cpu_read16(cpu, this_sem);
                cpu_write16(cpu, this_sem, (uint16_t)(cnt - 1));
            }
            cpu->a[7] += 4 + 4 + 2;
            cpu->pc = ret;
            continue;
        }
        /* P50 HLE bypass: unitio (OSUNITIO.TEXT:32). With P35 disabled so
         * SYS_PROC_INIT runs naturally, control reaches a tight self-
         * referential loop inside unitio around $0B0000 — symptom of
         * unitio's frame being set up with LINK that saves a stale A6
         * value equal to current A6, causing RTS to return to its own
         * entry point. Downstream of the subrange-word codegen change,
         * this may resolve with further codegen work, but for now the
         * real body is a thin wrapper around lisaio and our disk I/O is
         * HLE'd elsewhere, so unitio as a no-op is safe.
         * Signature: (var errnum:int; devnum:int; bufadr:absptr;
         *             numblocks:int4; blocknum:int4; var actual:int4;
         *             mode:disk_io_type; op:ioop);
         * Arg total: 4+2+4+4+4+4+2+2 = 26 bytes. */
        if (pc_unitio && cpu->pc == pc_unitio) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret      = cpu_read32(cpu, sp);
            uint32_t errnum_p = cpu_read32(cpu, sp + 4);
            uint32_t actual_p = cpu_read32(cpu, sp + 4 + 2 + 4 + 4 + 4);  /* offset to var actual */
            if (errnum_p >= 0x400 && errnum_p < 0xFE0000)
                cpu_write16(cpu, errnum_p, 0);
            if (actual_p >= 0x400 && actual_p < 0xFE0000)
                cpu_write32(cpu, actual_p, 0);
            cpu->a[7] += 4 + 26;
            cpu->pc = ret;
            continue;
        }
        /* P69 was a MAKE_DATASEG HLE attempt (fake seg_ptr carved from
         * $180000+) that unlocked SYS_PROC_INIT past FS_Setup but then
         * crashed deeper in Make_SProcess. Removed — the real fix is
         * P71's unary-minus CONST evaluation which made LDSN_TO_MMU
         * receive the correct ldsn value so DS_OPEN computes the right
         * seg_ptr via real MMU_Base. */
        /* P80c: trace INIT_FREEPOOL and fix broken pool headers.
         * Due to a record field offset corruption bug, INIT_FREEPOOL's
         * compiled code writes firstfree (int4) at offset 0 instead of
         * offset 2, overwriting pool_size with $00000008. Fix: after
         * INIT_FREEPOOL returns, check if pool_size=0 and repair. */
        {
            static uint32_t pc_ifp = 0;
            static int ifp_probed = 0;
            static uint32_t pending_fp_ptr = 0;
            static int16_t pending_fp_size = 0;
            static uint32_t pending_ret = 0;
            if (!ifp_probed) { ifp_probed = 1; pc_ifp = boot_progress_lookup("INIT_FREEPOOL"); }
            /* On INIT_FREEPOOL entry: save parameters */
            if (pc_ifp && cpu->pc == pc_ifp) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                pending_fp_ptr = cpu_read32(cpu, (sp + 4) & 0xFFFFFF);
                pending_fp_size = (int16_t)cpu_read16(cpu, (sp + 8) & 0xFFFFFF);
                pending_ret = cpu_read32(cpu, sp) & 0xFFFFFF;
                DBGSTATIC(int, ifp_count, 0);
                if (ifp_count++ < 5)
                    fprintf(stderr, "[INIT_FREEPOOL #%d] fp_ptr=$%08X fp_size=%d ($%04X) ret=$%06X\n",
                            ifp_count, pending_fp_ptr, pending_fp_size,
                            (uint16_t)pending_fp_size, pending_ret);
            }
            /* After return: check and fix pool header */
            if (pending_ret && cpu->pc == pending_ret && pending_fp_ptr) {
                uint32_t pool = pending_fp_ptr & 0xFFFFFF;
                int16_t pool_size = (int16_t)cpu_read16(cpu, pool);
                int expected_pool_size = (pending_fp_size - 8) / 2;
                if (pool_size != expected_pool_size && expected_pool_size > 0) {
                    fprintf(stderr, "[P80c-HLE] INIT_FREEPOOL pool header corrupt at $%06X: "
                            "pool_size=%d (expected %d). Repairing.\n",
                            pool, pool_size, expected_pool_size);
                    /* hdr_freepool layout: pool_size(2) + firstfree(4) + freecount(2) = 8 bytes.
                     * The bug writes firstfree at offset 0, producing:
                     * +0: $0000 $0008 instead of +0: $xxxx +2: $0000 $0008.
                     * Fix: write correct values at correct offsets. */
                    cpu_write16(cpu, pool, (uint16_t)expected_pool_size);     /* pool_size at +0 */
                    cpu_write32(cpu, pool + 2, 8);                            /* firstfree at +2 */
                    cpu_write16(cpu, pool + 6, (uint16_t)expected_pool_size); /* freecount at +6 */
                    /* Also fix the first free entry (at +8): size=pool_size, next=0(stopper) */
                    cpu_write16(cpu, pool + 8, (uint16_t)expected_pool_size); /* ent.size */
                    cpu_write32(cpu, pool + 10, 0);                           /* ent.next = stopper */
                }
                pending_ret = 0;
                pending_fp_ptr = 0;
            }
        }
        /* P80b: trace GETSPACE calls to diagnose pool exhaustion.
         * Codegen pushes: ord_ptr (4), b_area (4), size (2) — left-to-right.
         * After JSR: SP+0=ret(4), SP+4=size(2), SP+6=b_area(4), SP+10=ord_ptr(4) */
        if (pc_GETSPACE && cpu->pc == pc_GETSPACE) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            int16_t size = (int16_t)cpu_read16(cpu, (sp + 4) & 0xFFFFFF);
            uint32_t b_area = cpu_read32(cpu, (sp + 6) & 0xFFFFFF);
            DBGSTATIC(int, gs_count, 0);
            if (gs_count++ < 60 || (cpu->a[6] & 0xFFFFFF) >= 0xD00000) {
                fprintf(stderr, "[GETSPACE #%d] size=%d b_area=$%08X ret=$%06X A6=$%06X\n",
                        gs_count, size, b_area, cpu_read32(cpu, sp) & 0xFFFFFF,
                        cpu->a[6] & 0xFFFFFF);
                if ((cpu->a[6] & 0xFFFFFF) >= 0xD00000) {
                    /* Dump caller chain for the corrupt A6 call */
                    uint32_t fp = cpu->a[6] & 0xFFFFFF;
                    for (int i = 0; i < 6 && fp > 0x400 && fp < 0xFFFFFF; i++) {
                        uint32_t saved = cpu_read32(cpu, fp) & 0xFFFFFF;
                        uint32_t ret = cpu_read32(cpu, fp + 4) & 0xFFFFFF;
                        fprintf(stderr, "  frame[%d]: A6=$%06X saved=$%06X ret=$%06X\n",
                                i, fp, saved, ret);
                        fp = saved;
                    }
                }
            }
        }
        /* P78d: activate vector table guard when SYS_PROC_INIT entry is reached */
        if (pc_SYS_PROC_INIT && cpu->pc == pc_SYS_PROC_INIT && !g_vec_guard_active) {
            g_vec_guard_active = 1;
            /* P80: dump key globals at SYS_PROC_INIT entry using PASCALDEFS offsets */
            uint32_t a5 = cpu->a[5] & 0xFFFFFF;
            fprintf(stderr, "[P80-DIAG] SYS_PROC_INIT entered. A5=$%06X A6=$%06X A7=$%06X\n",
                    a5, cpu->a[6]&0xFFFFFF, cpu->a[7]&0xFFFFFF);
            fprintf(stderr, "  SGLOBAL @$200:                 $%08X\n", cpu_read32(cpu, 0x200));
            fprintf(stderr, "  sg_free_pool_addr (A5-24575):  $%08X\n", cpu_read32(cpu, (a5 - 24575) & 0xFFFFFF));
            fprintf(stderr, "  size_sglobal      (A5-24577):  $%04X\n", cpu_read16(cpu, (a5 - 24577) & 0xFFFFFF));
            fprintf(stderr, "  c_pcb_ptr         (A5-24617):  $%08X\n", cpu_read32(cpu, (a5 - 24617) & 0xFFFFFF));
            fprintf(stderr, "  sct_ptr           (A5-24781):  $%08X\n", cpu_read32(cpu, (a5 - 24781) & 0xFFFFFF));
            fprintf(stderr, "  b_syslocal_ptr    (A5-24785):  $%08X\n", cpu_read32(cpu, (a5 - 24785) & 0xFFFFFF));
            fprintf(stderr, "  mmrb_addr         (A5-25691):  $%08X\n", cpu_read32(cpu, (a5 - 25691) & 0xFFFFFF));
            fprintf(stderr, "  sctab             (A5-25661):  $%08X\n", cpu_read32(cpu, (a5 - 25661) & 0xFFFFFF));
            fprintf(stderr, "  invoke_sched      (A5-24786):  $%02X\n", cpu_read8(cpu, (a5 - 24786) & 0xFFFFFF));
            /* P80b: dump MMRB structure to diagnose sds_sem corruption */
            uint32_t mmrb = cpu_read32(cpu, (a5 - 25691) & 0xFFFFFF);
            if (mmrb >= 0xCC0000 && mmrb < 0xCE0000) {
                fprintf(stderr, "  MMRB @$%06X hex dump (80 bytes):\n", mmrb);
                for (int row = 0; row < 80; row += 16) {
                    fprintf(stderr, "    +$%02X:", row);
                    for (int col = 0; col < 16 && row + col < 80; col += 2)
                        fprintf(stderr, " %04X", cpu_read16(cpu, (mmrb + row + col) & 0xFFFFFF));
                    fprintf(stderr, "\n");
                }
                /* Decode expected MMRB fields:
                 * +0:  hd_qioreq_list (linkage: fwd:int2 + bkwd:int2 = 4)
                 * +4:  seg_wait_sem (semaphore: count:int2 + owner:int2 + wait_q:4 = 8)
                 * +12: memmgr_sem (semaphore: 8)
                 * +20: memmgr_busyF (boolean: 1-2)
                 * +22: clr_mmbusy (boolean: 1-2)
                 * +24: numbRelSegs (int2: 2)
                 * +26: req_pcb_ptr (ptr_pcb: 4)
                 * +30: hd_sdscb_list (linkage: 4)
                 * +34: sds_sem (semaphore: count:int2 + owner:int2 + wait_q:4 = 8)
                 */
                fprintf(stderr, "  Decoded (assuming Apple layout):\n");
                fprintf(stderr, "    hd_qioreq_list:  fwd=$%04X bkwd=$%04X\n",
                        cpu_read16(cpu, mmrb), cpu_read16(cpu, mmrb + 2));
                fprintf(stderr, "    seg_wait_sem:    count=%d owner=$%04X wait_q=$%08X\n",
                        (int16_t)cpu_read16(cpu, mmrb + 4),
                        cpu_read16(cpu, mmrb + 6), cpu_read32(cpu, mmrb + 8));
                fprintf(stderr, "    memmgr_sem:      count=%d owner=$%04X wait_q=$%08X\n",
                        (int16_t)cpu_read16(cpu, mmrb + 12),
                        cpu_read16(cpu, mmrb + 14), cpu_read32(cpu, mmrb + 16));
                fprintf(stderr, "    memmgr_busyF:    $%02X  clr_mmbusy: $%02X\n",
                        cpu_read8(cpu, mmrb + 20), cpu_read8(cpu, mmrb + 21));
                fprintf(stderr, "    numbRelSegs:     %d  req_pcb_ptr: $%08X\n",
                        (int16_t)cpu_read16(cpu, mmrb + 22), cpu_read32(cpu, mmrb + 24));
                fprintf(stderr, "    hd_sdscb_list:   fwd=$%04X bkwd=$%04X\n",
                        cpu_read16(cpu, mmrb + 28), cpu_read16(cpu, mmrb + 30));
                fprintf(stderr, "    sds_sem@+34:     count=%d owner=$%04X wait_q=$%08X\n",
                        (int16_t)cpu_read16(cpu, mmrb + 34),
                        cpu_read16(cpu, mmrb + 36), cpu_read32(cpu, mmrb + 38));
            }
        }
        /* P80: VEC-GUARD PC trace removed — binary layout shifts on recompile.
         * Use generic VEC-GUARD dump in lisa_mmu.c instead. */
        /* (P79f probes removed — b_syslocal_ptr corruption traced to SCTAB2 overflow, fixed) */
        /* P35: SYS_PROC_INIT bypass — DISABLED (P80).
         * P79 fixes (record layouts, push direction, enum constants,
         * byte-subrange sizing) resolved the NULL pointer crashes in
         * BLD_SEG/Signal_sem/ENQUEUE. Let the body run for real. */
#if 0
        if (pc_SYS_PROC_INIT && cpu->pc == pc_SYS_PROC_INIT) {
            boot_progress_record_pc(cpu->pc);
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret = cpu_read32(cpu, sp);
            cpu->a[7] += 4;
            cpu->pc = ret;
            continue;
        }
#endif
        /* P81c: excep_setup HLE bypass DISABLED. The "wild b_sloc_ptr"
         * was CreateProcess passing a garbage syslocal pointer — a
         * static-link symptom. With CreateProcess native, b_sloc_ptr
         * is now always valid. */
        /* P81c: REG_OPEN_LIST HLE bypass DISABLED. The "same sentinel-init
         * class as QUEUE_PR" was the static-link bug; natural body works. */
        /* P77 HLE bypass: UNLOCKSEGS (MM0.TEXT:486). The seglock list
         * sentinel (hd_seglock_list.fwd_link) isn't initialized by
         * Build_Syslocal's WITH-block sentinel stores (same P13 class).
         * Result: UNLOCKSEGS walks a corrupt chain starting at syslocal
         * base, calling RELSPACE($CE0000, $CE0000) in an infinite loop.
         * Since newly created processes have no locked segments, bypass
         * is safe: write errnum=0 and return. Signature:
         * procedure UNLOCKSEGS(var errnum: integer) — Pascal callee-clean,
         * stack: retPC(4) + errnum_ptr(4). */
        /* P78c HLE bypass: SplitPathname (fsui1.text:247). Called from
         * MAKE_SYSDATASEG during process creation. DecompPath (called
         * internally) writes to bogus addresses that alias into the
         * vector table because our filesystem isn't functional.
         * HLE: write ecode=0, device=0 (bootdev), volPath='' (empty),
         * return. Signature: procedure SplitPathname(var ecode: integer;
         * var path: pathname; var device: integer; var volPath: pathname)
         * — 4 VAR params, Pascal callee-clean.
         * Stack: retPC(4) + volPath_ptr(4) + device_ptr(4) + path_ptr(4) + ecode_ptr(4) = 20. */
        /* P78c HLE bypass: DecompPath (fsui1.text). Called from
         * SPLITPATHNAME and directly from Make_Object during FS operations.
         * The call to SPLITPATHNAME in MAKE_SYSDATASEG resolves to
         * DecompPath's entry (SplitPathname is a thin wrapper).
         * DecompPath → parse_pathname writes to bogus addresses aliasing
         * into the vector table. HLE: write ecode=0, device=0, return.
         * Signature: procedure DecompPath(var ecode: error; var path:
         * pathname; var device: integer; var parID: NodeIdent;
         * var volPath: pathname) — 5 VAR params, 20 bytes of args. */
        {
            static uint32_t pc_DecompPath = 0;
            static int dp_gen = -1;
            if (dp_gen != g_emu_generation) {
                pc_DecompPath = boot_progress_lookup("DecompPath");
                dp_gen = g_emu_generation;
            }
            /* Also bypass parse_pathname — it's the actual function writing
             * to the vector table via bogus pathname pointers */
            static uint32_t pc_parse_pathname = 0;
            if (dp_gen == g_emu_generation && !pc_parse_pathname)
                pc_parse_pathname = boot_progress_lookup("parse_pathname");
            /* P80a: DecompPath and parse_pathname bypasses DISABLED.
             * The 8-char identifier fix (P80) resolved the codegen bugs
             * that caused bogus pointer writes from these FS functions.
             * Let them run natively so they set device numbers correctly. */
#if 0
            /* parse_pathname bypass (was P79) */
            if (pc_parse_pathname && cpu->pc == pc_parse_pathname) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t ecode_ptr = cpu_read32(cpu, sp + 24) & 0xFFFFFF;
                if (ecode_ptr >= 0x1000 && ecode_ptr < 0x240000)
                    cpu_write16(cpu, ecode_ptr, 0);
                cpu->a[7] += 4 + 24;
                cpu->pc = ret;
                DBGSTATIC(int, pp_count, 0);
                if (pp_count++ < 3)
                    fprintf(stderr, "[HLE-parse_pathname #%d] bypassed\n", pp_count);
                continue;
            }
            /* DecompPath bypass (was P78c) */
            if (pc_DecompPath && cpu->pc == pc_DecompPath) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                uint32_t ecode_ptr = cpu_read32(cpu, sp + 20) & 0xFFFFFF;
                uint32_t device_ptr = cpu_read32(cpu, sp + 12) & 0xFFFFFF;
                if (ecode_ptr >= 0x1000 && ecode_ptr < 0x240000)
                    cpu_write16(cpu, ecode_ptr, 0);
                if (device_ptr >= 0x1000 && device_ptr < 0x240000)
                    cpu_write16(cpu, device_ptr, 0);
                cpu->a[7] += 4 + 20;
                cpu->pc = ret;
                DBGSTATIC(int, dp_count, 0);
                if (dp_count++ < 5)
                    fprintf(stderr, "[HLE-DecompPath #%d] bypassed\n", dp_count);
                continue;
            }
#endif
        }
        /* P79 HLE bypass: MAKE_SYSDATASEG — when discsize > 0 (non-resident),
         * the body calls SPLITPATHNAME, OPEN_TEMP, ALLOCPAGES which all need
         * a functional filesystem. Return error 134 so STARTUP retries with
         * resident=true (discsize=0, memory-only path that works).
         * Signature: procedure MAKE_SYSDATASEG(var errnum: int2;
         *   var progname: pathname; memsize, discsize: int4;
         *   var refnum: int2; var segptr: absptr; ldsn: int2)
         * Stack: retPC(4) + ldsn(2) + segptr_ptr(4) + refnum_ptr(4) +
         *   discsize(4) + memsize(4) + progname_ptr(4) + errnum_ptr(4) = 30 */
        /* P80e: memory watchpoint at saved A6 location.
         * The saved A6 at $CBFD48 gets overwritten with $E000D0 during
         * SEG_IO execution. Watch for the exact instruction that writes. */
        if (g_vec_guard_active && cpu->a[6] == 0xCBFD48) {
            /* A6 = $CBFD48 means we're in SEG_IO's frame.
             * Check if saved A6 at [A6] = [$CBFD48] changed. */
            static uint32_t watch_val = 0;
            static int watch_gen = -1;
            if (watch_gen != g_emu_generation) { watch_val = 0; watch_gen = g_emu_generation; }
            uint32_t cur = cpu_read32(cpu, 0xCBFD48);
            if (watch_val != 0 && cur != watch_val) {
                fprintf(stderr, "[P80e-MEMWATCH] [$CBFD48] changed from $%08X to $%08X at PC=$%06X op=$%04X\n",
                        watch_val, cur, cpu->pc, cpu_read16(cpu, cpu->pc));
                watch_val = cur;
            } else if (watch_val == 0) {
                watch_val = cur;
            }
        }
        /* P80e: A6 watchpoint — detect when A6 enters the $Dxxxxx-$Exxxxx
         * range (syslocal/new-process area) during SYS_PROC_INIT. */
        if (g_vec_guard_active) {
            static uint32_t wp_prev_a6 = 0;
            static int wp_a6_gen = -1;
            static int wp_a6_trips = 0;
            if (wp_a6_gen != g_emu_generation) { wp_prev_a6 = 0; wp_a6_trips = 0; wp_a6_gen = g_emu_generation; }
            uint32_t a6 = cpu->a[6] & 0xFFFFFF;
            /* Trigger when A6 moves INTO $D0xxxx-$F0xxxx (syslocal/process area)
             * from outside that range */
            if (a6 >= 0xD00000 && a6 < 0xF00000 &&
                (wp_prev_a6 < 0xD00000 || wp_prev_a6 >= 0xF00000) &&
                wp_a6_trips++ < 3) {
                fprintf(stderr, "[P80e-A6TRAP] A6 $%06X→$%06X at PC=$%06X op=$%04X\n",
                        wp_prev_a6, a6, cpu->pc, cpu_read16(cpu, cpu->pc));
                uint32_t fp = a6;
                for (int i = 0; i < 5 && fp > 0x400 && fp < 0xFFFFFF; i++) {
                    uint32_t saved = cpu_read32(cpu, fp) & 0xFFFFFF;
                    uint32_t ret = cpu_read32(cpu, fp + 4) & 0xFFFFFF;
                    fprintf(stderr, "  frame[%d]: A6=$%06X saved=$%06X ret=$%06X\n",
                            i, fp, saved, ret);
                    fp = saved;
                }
            }
            wp_prev_a6 = a6;
        }
        /* P81a: CHK_LDSN_FREE HLE bypass DISABLED after the static-link ABI
         * fix. The "errnum compared against 3, not 1" behavior that
         * motivated the bypass was a symptom of the static-link bug —
         * CHK_LDSN_FREE reads an enclosing proc's e_ldsnfree constant,
         * and the dynamic-link walk returned the wrong frame so the
         * comparison used a neighboring local's value. With the proper
         * static-link ABI, let the natural body run. */
        /* P80e: trace MAKE_DATASEG RECOVER to find failure cause */
        {
            static uint32_t pc_recover = 0;
            static int rec_probed = 0;
            if (!rec_probed) { rec_probed = 1; pc_recover = boot_progress_lookup("RECOVER"); }
            /* RECOVER is at $01AF16. Its first param is error: int2 at A6+8 (after LINK) */
            if (pc_recover && cpu->pc == pc_recover) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                int16_t error = (int16_t)cpu_read16(cpu, sp + 4); /* error param before LINK */
                /* Also read the actual errnum from MAKE_DATASEG's VAR param */
                uint32_t a6 = cpu->a[6] & 0xFFFFFF; /* RECOVER's A6 = MAKE_DATASEG's frame */
                uint32_t mds_a6 = cpu_read32(cpu, a6) & 0xFFFFFF; /* follow static link to parent */
                uint32_t errnum_ptr = cpu_read32(cpu, mds_a6 + 8) & 0xFFFFFF; /* MAKE_DATASEG param at A6+8 */
                int16_t real_errnum = (errnum_ptr >= 0x1000) ? (int16_t)cpu_read16(cpu, errnum_ptr) : -9999;
                DBGSTATIC(int, rec_count, 0);
                if (rec_count++ < 5) {
                    fprintf(stderr, "[MAKE_DATASEG-RECOVER #%d] error=%d errnum=%d ret=$%06X\n",
                            rec_count, error, real_errnum, cpu_read32(cpu, sp) & 0xFFFFFF);
                    /* Walk A6 chain for context */
                    uint32_t fp = cpu->a[6] & 0xFFFFFF;
                    for (int i = 0; i < 4 && fp > 0x400 && fp < 0xFFFFFF; i++) {
                        uint32_t r = cpu_read32(cpu, fp + 4) & 0xFFFFFF;
                        fprintf(stderr, "  frame[%d]: A6=$%06X ret=$%06X\n", i, fp, r);
                        fp = cpu_read32(cpu, fp) & 0xFFFFFF;
                    }
                }
            }
        }
        /* P80g: trace Make_SProcess calls */
        {
            static uint32_t pc_msp = 0;
            static int msp_probed = 0;
            if (!msp_probed) {
                msp_probed = 1;
                pc_msp = boot_progress_lookup("Make_SPr");
                if (!pc_msp) pc_msp = boot_progress_lookup("Make_SProcess");
            }
            if (cpu->pc == pc_msp) {
                DBGSTATIC(int, msp_count, 0);
                fprintf(stderr, "[Make_SProcess #%d] A6=$%06X A7=$%06X\n",
                        ++msp_count, cpu->a[6] & 0xFFFFFF, cpu->a[7] & 0xFFFFFF);
            }
        }
        /* P80g: bypass CreateProcess. The process creation code has deep
         * record field offset corruption that causes A6 to become $FD800000
         * during Build_Syslocal/Build_Stack. For now, skip the entire
         * initialization — processes won't run but boot can continue.
         * CreateProcess(pcb_ptr: ptr_PCB; sloc: segHandle; stk: segHandle;
         *   stk_info: stkInfo_rec; jt: ptr_JumpTable; units: IUuse_list;
         *   start_PC: absptr; evt_chn: int2) — many value params */
        {
            static uint32_t pc_cp = 0;
            static int cp_probed = 0;
            if (!cp_probed) {
                cp_probed = 1;
                pc_cp = boot_progress_lookup("CreateProcess");
                if (!pc_cp) pc_cp = boot_progress_lookup("CreatePr");
                if (pc_cp) fprintf(stderr, "  DEBUG: CreateProcess at $%06X\n", pc_cp);
                else fprintf(stderr, "  DEBUG: CreateProcess NOT FOUND, using hardcoded\n");
                if (!pc_cp) pc_cp = 0x04E348;  /* from linker map */
            }
            /* Also bypass ModifyProcess and FinishCreate — they access
             * uninitialized process structures that crash. */
            static uint32_t pc_mp = 0, pc_fc = 0;
            static int mp_probed = 0;
            if (!mp_probed) {
                mp_probed = 1;
                pc_mp = boot_progress_lookup("ModifyProcess");
                pc_fc = boot_progress_lookup("FinishCreate");
                if (!pc_mp) pc_mp = 0x04EF12;  /* from map */
                if (!pc_fc) pc_fc = 0x04E3CA;  /* from map */
            }
            if (0 && pc_cp && cpu->pc == pc_cp) {
                /* P81a: CreateProcess HLE DISABLED. With static-link ABI and
                 * natural MAKE_SYSDATASEG, the native CreateProcess body
                 * should populate PCB/syslocal/env_save_area correctly. */
                /* P80h: HLE CreateProcess — initialize minimum PCB fields.
                 * Stack: ret(4), pcb_ptr(4), sloc_addr(4), stk_addr(4),
                 * stk_info_addr(4), jt_ptr(4), unit_list_addr(4),
                 * start_PC(4), evnt_chn(2) — 30 bytes of args */
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t pcb = cpu_read32(cpu, (sp + 4) & 0xFFFFFF) & 0xFFFFFF;
                uint32_t sloc_addr = cpu_read32(cpu, (sp + 8) & 0xFFFFFF) & 0xFFFFFF;
                uint32_t stk_addr = cpu_read32(cpu, (sp + 12) & 0xFFFFFF) & 0xFFFFFF;
                uint32_t start_pc_val = cpu_read32(cpu, (sp + 28) & 0xFFFFFF);
                /* Read sloc_handle.seg_ptr and stk_handle.seg_ptr/seg_size.
                 * segHandle: seg_size(4), disk_size(4), seg_refnum(2), seg_ptr(4) */
                uint32_t sloc_seg_ptr = cpu_read32(cpu, (sloc_addr + 10) & 0xFFFFFF);
                uint32_t stk_seg_ptr = cpu_read32(cpu, (stk_addr + 10) & 0xFFFFFF);
                uint32_t stk_seg_size = cpu_read32(cpu, (stk_addr) & 0xFFFFFF);
                /* Read sysA5 from SGLOBAL @$200 */
                uint32_t sys_a5 = cpu_read32(cpu, 0x200);
                DBGSTATIC(int, cp_count, 0);
                cp_count++;
                fprintf(stderr, "[HLE-CreateProcess #%d] pcb=$%06X sloc=$%06X stk=$%06X "
                        "stk_size=%d start_PC=$%06X\n",
                        cp_count, pcb, sloc_seg_ptr, stk_seg_ptr,
                        stk_seg_size, start_pc_val & 0xFFFFFF);
                /* Dump stack for debugging */
                fprintf(stderr, "  Stack:");
                for (int j = 4; j <= 34; j += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, (sp + j) & 0xFFFFFF));
                fprintf(stderr, "\n");
                /* Initialize PCB fields at estimated offsets.
                 * PCB layout: next_schedPtr(4), prev_schedPtr(4), semwait_queue(4),
                 * priority(2), norm_pri(2), blk_state(2), domain(2), sems_owned(2) */
                if (pcb >= 0xCC0000 && pcb < 0xCE0000) {
                    /* priority from Make_SProcess (250 MemMgr, 230 Root).
                     * P80h2: our Pascal compiler packs subrange 0..255 as a
                     * 1-byte field (Scheduler compiles priority read as
                     * MOVE.B $0C(A0),D0). Writing a 16-bit word put $00 in
                     * the high byte at offset 12, so the scheduler read
                     * priority as 0 and SelectProcess returned nil. Write
                     * priority/norm_pri as bytes; blk_state/domain as words. */
                    static int pri_vals[] = {250, 230, 200, 200};
                    int pri = pri_vals[(cp_count-1) < 4 ? (cp_count-1) : 3];
                    cpu_write8(cpu, pcb + 12, (uint8_t)pri);     /* priority */
                    cpu_write8(cpu, pcb + 13, (uint8_t)pri);     /* norm_pri */
                    cpu_write16(cpu, pcb + 14, 0);               /* blk_state = empty */
                    cpu_write16(cpu, pcb + 16, 0);               /* domain = 0 */
                }
                /* P80h2: set up env_save_area in the syslocal so Launch's
                 * SETREGS has real register state to RTE into. Pascal's
                 * Build_Stack normally does this; we bypassed it. Without
                 * this, every dispatched process crashes immediately when
                 * SETREGS pops $00000000 as PC and jumps to vector 0.
                 *
                 * syslocal layout (source-SYSGLOBAL.TEXT):
                 *   +0  sl_free_pool_addr  (4)
                 *   +4  size_slocal        (2)
                 *   +6  env_save_area      (env_area, 70 bytes)
                 *         +0  PC (4)
                 *         +4  SR (2)
                 *         +6..+37  D0..D7 (8*4)
                 *         +38..+65 A0..A5 (6*4)
                 *         +62 A6 (4) — NB: record says A6 after A5, so +58+4
                 *         +66 A7 (4)
                 *   +76 SCB { A5value(4), PCvalue(4), domvalue(2) }
                 *
                 * Recompute env_area offsets: PC@+0, SR@+4, D0@+6, ..., A0@+38,
                 *   A1@+42, A2@+46, A3@+50, A4@+54, A5@+58, A6@+62, A7@+66.
                 * So absolute syslocal offsets: PC=6, SR=10, D0=12, A5=64,
                 *   A6=68, A7=72, SCB_A5value=76, SCB_PCvalue=80.
                 *
                 * We skip the Initiate/init_stack dance and RTE directly into
                 * start_PC. The process body will LINK/UNLK normally with
                 * A6=A7=stack top. sl_free_pool_addr is set so MemMgr's
                 * GETSPACE calls don't fault on an empty free pool. */
                if (sloc_seg_ptr >= 0xCC0000 && sloc_seg_ptr < 0xCE0000 &&
                    stk_seg_ptr  >= 0xCC0000 && stk_seg_ptr  < 0xCE0000 &&
                    (start_pc_val & 0xFFFFFF) >= 0x000400 &&
                    (start_pc_val & 0xFFFFFF) <  0x800000) {
                    uint32_t stk_top = (stk_seg_ptr + stk_seg_size) & 0xFFFFFF;
                    uint32_t env = (sloc_seg_ptr + 6) & 0xFFFFFF;
                    /* PC, SR */
                    cpu_write32(cpu, env + 0,  start_pc_val & 0xFFFFFF);
                    cpu_write16(cpu, env + 4,  0x0000);          /* SR = user, IPL=0 */
                    /* D0..D7, A0..A4 = 0 */
                    for (uint32_t off = 6; off <= 54; off += 4)
                        cpu_write32(cpu, env + off, 0);
                    cpu_write32(cpu, env + 58, sys_a5);           /* A5 = sysA5 */
                    cpu_write32(cpu, env + 62, stk_top);          /* A6 = stack top */
                    cpu_write32(cpu, env + 66, stk_top);          /* A7 = stack top */
                    /* SCB: used for syscall PC/A5 save, but pre-populate so
                     * trap handlers that read it find sane values. */
                    cpu_write32(cpu, sloc_seg_ptr + 76, sys_a5);           /* SCB.A5value */
                    cpu_write32(cpu, sloc_seg_ptr + 80, start_pc_val & 0xFFFFFF); /* SCB.PCvalue */
                    cpu_write16(cpu, sloc_seg_ptr + 84, 1);                /* SCB.domvalue */
                    /* sl_free_pool_addr → just past the syslocal record.
                     * size_slocal is already implicit (slot stays 0, but
                     * GETSPACE reads sl_free_pool_addr first). */
                    cpu_write32(cpu, sloc_seg_ptr + 0, (sloc_seg_ptr + 256) & 0xFFFFFF);
                    fprintf(stderr, "  [HLE-CP env] PC=$%06X A5=$%06X A6=A7=$%06X\n",
                            start_pc_val & 0xFFFFFF, sys_a5, stk_top);
                    if (pcb_sloc_count < 8) {
                        pcb_sloc_map[pcb_sloc_count].pcb = pcb;
                        pcb_sloc_map[pcb_sloc_count].sloc = sloc_seg_ptr & 0xFFFFFF;
                        pcb_sloc_count++;
                    }
                }
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] += 4;
                cpu->pc = ret;
                continue;
            }
            if (0 && pc_cp && cpu->pc == pc_mp) {
                /* P81a: ModifyProcess HLE DISABLED alongside CreateProcess. */
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] += 4;
                cpu->pc = ret;
                continue;
            }
            if (0 && pc_cp && cpu->pc == pc_fc) {
                /* P81a: FinishCreate HLE DISABLED. The priority-sorted queue
                 * insert is Pascal Queue_Process (PROCASM.TEXT:174); its
                 * original body accesses an outer-scope state via a sibling
                 * call, which was the static-link bug. Let it run natively. */
                /* (original HLE kept below for reference, unreachable) */
                /* P80h2: HLE FinishCreate — priority-sorted insert into ready
                 * queue. Mirrors Queue_Process (PROCASM.TEXT:174): walk from
                 * @fwd_ReadyQ following next_schedPtr until we find a node
                 * with priority < newpcb.priority, insert before that node.
                 *
                 * Queue is a doubly-linked list of absolute 4-byte pointers:
                 *   PCB+0 = next_schedPtr, PCB+4 = prev_schedPtr
                 * fwd_ReadyQ lives at A5-1116 (PFWD_REA); the sentinel PCB
                 * has priority 0 and wraps back to @fwd_ReadyQ, so the walk
                 * always terminates. */
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t pcb = cpu_read32(cpu, (sp + 4) & 0xFFFFFF) & 0xFFFFFF;
                DBGSTATIC(int, fc_count, 0);
                fc_count++;
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                uint32_t head = (a5 - 1116) & 0xFFFFFF;  /* @fwd_ReadyQ */
                int new_pri = cpu_read8(cpu, pcb + 12);  /* 1-byte priority */
                /* Walk the queue: cur starts at fwd_ReadyQ's first entry. */
                uint32_t cur = cpu_read32(cpu, head) & 0xFFFFFF;
                int safety = 0;
                while (cur && cur != head && safety++ < 64) {
                    int cur_pri = cpu_read8(cpu, cur + 12);
                    if (new_pri > cur_pri) break;
                    cur = cpu_read32(cpu, cur) & 0xFFFFFF;
                }
                if (!cur || safety >= 64) {
                    fprintf(stderr, "[HLE-FinishCreate #%d] queue walk failed "
                            "(cur=$%06X safety=%d) — forcing head insert\n",
                            fc_count, cur, safety);
                    cur = head;
                }
                uint32_t prev = cpu_read32(cpu, cur + 4) & 0xFFFFFF;
                if (!prev) prev = head;
                /* Insert pcb between prev and cur. */
                cpu_write32(cpu, pcb + 0, cur);   /* pcb.next = cur */
                cpu_write32(cpu, pcb + 4, prev);  /* pcb.prev = prev */
                cpu_write32(cpu, prev + 0, pcb);  /* prev.next = pcb */
                cpu_write32(cpu, cur + 4, pcb);   /* cur.prev  = pcb */
                fprintf(stderr, "[HLE-FinishCreate #%d] pcb=$%06X pri=%d "
                        "inserted between prev=$%06X and cur=$%06X\n",
                        fc_count, pcb, new_pri, prev, cur);
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] += 4;
                cpu->pc = ret;
                continue;
            }
        }
        /* P81a: Move_MemMgr HLE bypass DISABLED. With natural MAKE_SYSDATASEG
         * running, the Pascal MM's free-space bookkeeping reflects real
         * allocations, so GetFree finds room for MemMgr's stack/syslocal
         * move. */
        /* P81a: MAKE_SYSDATASEG HLE bypass DISABLED. Its motivating issues
         * ("exit(Make_SProcess) from nested Recover doesn't write errnum",
         * "corrupt WITH block reading segHandle at wrong offsets") are
         * classic symptoms of the static-link bug — Make_SProcess and the
         * Recover it calls are nested procs accessing outer-scope locals
         * via sibling calls. With the static-link ABI fix, let the
         * natural DS_OPEN / GetFree / MMU-program path run. */
        if (0 && pc_MAKE_SYSDATASEG && cpu->pc == pc_MAKE_SYSDATASEG) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            /* Stack layout verified from compiled code:
             * errnum_ptr(4), progname_ptr(4), memsize(4), discsize(4),
             * refnum_ptr(4), segptr_ptr(4), ldsn(2) — left to right push.
             * After JSR: SP+4=errnum_ptr, SP+8=progname_ptr, SP+12=memsize,
             * SP+16=discsize, SP+20=refnum_ptr, SP+24=segptr_ptr, SP+28=ldsn(2) */
            int32_t discsize = (int32_t)cpu_read32(cpu, (sp + 16) & 0xFFFFFF);
            DBGSTATIC(int, msd_count, 0);
            if (msd_count++ < 5) {
                fprintf(stderr, "[MAKE_SYSDATASEG #%d] discsize=%d memsize=%d ldsn=%d\n",
                        msd_count, discsize,
                        (int32_t)cpu_read32(cpu, (sp + 12) & 0xFFFFFF),
                        (int16_t)cpu_read16(cpu, (sp + 28) & 0xFFFFFF));
                /* Dump stack for debugging */
                fprintf(stderr, "  Stack: ");
                for (int j = 4; j <= 30; j += 2)
                    fprintf(stderr, "%04X ", cpu_read16(cpu, (sp + j) & 0xFFFFFF));
                fprintf(stderr, "\n");
            }
            /* P80g: treat ALL MAKE_SYSDATASEG calls as resident (discsize=0).
             * Previously, discsize>0 returned error 134 to trigger a resident
             * retry. But the retry fails because exit(Make_SProcess) from
             * the nested Recover doesn't properly write errnum to the VAR
             * param. Just allocate as resident regardless of discsize. */
            /* P80f: bypass disc_size=0 (resident) calls too.
             * DS_OPEN fails because the FS isn't functional.
             * For memory-resident segments, allocate via GETSPACE and
             * program the MMU directly. Set errnum=0, refnum=0,
             * segptr=allocated address. */
            {   /* P80g: handle ALL discsize values as resident */
                int32_t memsize = (int32_t)cpu_read32(cpu, (sp + 12) & 0xFFFFFF);
                uint32_t errnum_ptr = cpu_read32(cpu, (sp + 4) & 0xFFFFFF) & 0xFFFFFF;
                uint32_t segptr_ptr = cpu_read32(cpu, (sp + 24) & 0xFFFFFF) & 0xFFFFFF;
                uint32_t refnum_ptr = cpu_read32(cpu, (sp + 20) & 0xFFFFFF) & 0xFFFFFF;
                int16_t ldsn = (int16_t)cpu_read16(cpu, (sp + 28) & 0xFFFFFF);
                /* P80g: validate pointers — if errnum_ptr is garbage (e.g., $000009
                 * from corrupt WITH block reading segHandle at wrong offsets),
                 * still return success but skip pointer writes */
                bool ptrs_valid = (errnum_ptr >= 0x10000 && segptr_ptr >= 0x10000);
                /* P80f: for resident segments, allocate from the sgheap area
                 * which is already MMU-mapped. The sgheap starts after the
                 * sysglobal pool. Use a bump allocator within the mapped range.
                 * The sgheap is mapped in segment 102 ($CC0000-$CDFFFF). */
                static uint32_t hle_seg_bump = 0xCCC000;  /* well above pool at $CCB000 */
                static int hle_seg_gen = -1;
                extern int g_emu_generation;
                if (hle_seg_gen != g_emu_generation) {
                    hle_seg_bump = 0xCCC000;
                    hle_seg_gen = g_emu_generation;
                }
                uint32_t seg_ptr = hle_seg_bump;
                int alloc_size = (memsize > 0) ? memsize : 4096;  /* min 4KB */
                hle_seg_bump += (alloc_size + 0x1FF) & ~0x1FF;
                /* Zero-fill the allocated area */
                for (int j = 0; j < alloc_size; j++)
                    cpu_write8(cpu, (seg_ptr + j) & 0xFFFFFF, 0);
                /* Write results only if pointers are valid */
                if (ptrs_valid) {
                    cpu_write16(cpu, errnum_ptr, 0);
                    cpu_write32(cpu, segptr_ptr, seg_ptr);
                    if (refnum_ptr >= 0x10000) cpu_write16(cpu, refnum_ptr, 100 + ldsn);
                }
                fprintf(stderr, "[HLE-MAKE_SYSDATASEG] resident: memsize=%d ldsn=%d → seg_ptr=$%06X ptrs_valid=%d\n",
                        alloc_size, ldsn, seg_ptr, ptrs_valid);
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] += 4;  /* pop ret only — caller does ADDA.W #$1A,SP for args */
                cpu->pc = ret;
                continue;
            }
        }
        /* P78b HLE bypass: Make_File (fsui1.text:593). During process
         * creation, MAKE_SYSDATASEG → MAKE_IT → Make_File tries to create
         * a backing file for the data segment. Our filesystem isn't
         * functional (no mounttable, no real disk catalog), so Make_File →
         * Make_Object → DecompPath → SplitPathname writes to garbage
         * addresses that alias into the CPU vector table, causing F-line
         * trap. HLE: write ecode=0, pop args, return. The physical memory
         * for the data segment is already allocated; the file is only
         * needed for disk swapping (which we don't do).
         * Signature: procedure Make_File(var ecode: error; var path:
         * pathname; label_size: integer) — Pascal callee-clean.
         * Stack: retPC(4) + label_size(2) + path_ptr(4) + ecode_ptr(4). */
        /* P81c: Make_File HLE bypass DISABLED. The DecompPath/SplitPathname
         * garbage-write path that motivated it was a static-link symptom;
         * natural body works now. */
        /* P81a: Signal_sem HLE bypass DISABLED. The "garbage wait_queue
         * (code addresses like BUS_ERR)" was likely a downstream symptom
         * of the static-link bug corrupting semaphore initialization.
         * With the ABI fix and MAKE_SYSDATASEG running natively, let
         * the real Signal_sem body run. */
        if (0 && pc_Signal_sem && cpu->pc == pc_Signal_sem) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t sem_ptr = cpu_read32(cpu, sp + 4);
            /* P79e: validate sem_ptr — bogus values like $000012 (in vector
             * table area) mean the caller passed garbage. Skip entirely. */
            if (sem_ptr && sem_ptr >= 0x1000 && sem_ptr < 0x240000) {
                int16_t sem_count = (int16_t)cpu_read16(cpu, sem_ptr & 0xFFFFFF);
                if (sem_count >= 0) {
                    /* No waiters — just increment and return */
                    cpu_write16(cpu, sem_ptr & 0xFFFFFF, (uint16_t)(sem_count + 1));
                    /* Clear owner (offset 2, relptr = int2) */
                    cpu_write16(cpu, (sem_ptr + 2) & 0xFFFFFF, 0);
                    uint32_t ret = cpu_read32(cpu, sp);
                    cpu->a[7] += 8;  /* pop retPC + sem_ptr */
                    cpu->pc = ret;
                    continue;
                }
                /* sem_count < 0: there's supposedly a waiter.
                 * Check wait_queue (offset 4) — if it's outside sysglobal/
                 * syslocal range, it's garbage. Skip the waiter path. */
                uint32_t wait_q = cpu_read32(cpu, (sem_ptr + 4) & 0xFFFFFF);
                /* P79: also treat wait_q==0 (NULL) as bogus — sem_count<0
                 * with NULL wait_queue means corrupt semaphore state */
                if (!wait_q || wait_q < 0xCA0000 || wait_q > 0xD40000) {
                    DBGSTATIC(int, ss_skip, 0);
                    if (ss_skip++ < 5) {
                        uint32_t ret = cpu_read32(cpu, sp);
                        fprintf(stderr, "[P78-SS] Signal_sem: sem_ptr=$%08X sem_count=%d wait_queue=$%08X (bogus) ret=$%06X\n",
                                sem_ptr, sem_count, wait_q, ret & 0xFFFFFF);
                        /* Dump A6 chain to identify caller */
                        uint32_t fp = cpu->a[6] & 0xFFFFFF;
                        for (int i = 0; i < 6 && fp > 0x400 && fp < 0xFFFFFF; i++) {
                            uint32_t r = cpu_read32(cpu, fp + 4);
                            fprintf(stderr, "  frame[%d]: A6=$%06X ret=$%06X\n", i, fp, r & 0xFFFFFF);
                            fp = cpu_read32(cpu, fp) & 0xFFFFFF;
                        }
                    }
                    cpu_write16(cpu, sem_ptr & 0xFFFFFF, (uint16_t)(sem_count + 1));
                    cpu_write16(cpu, (sem_ptr + 2) & 0xFFFFFF, 0);
                    uint32_t ret = cpu_read32(cpu, sp);
                    cpu->a[7] += 8;
                    cpu->pc = ret;
                    continue;
                }
            } else if (sem_ptr) {
                /* sem_ptr is in vector table area — bogus, just return */
                uint32_t ret = cpu_read32(cpu, sp);
                cpu->a[7] += 8;
                cpu->pc = ret;
                continue;
            }
        }
        /* P77 HLE: guard RELSPACE against freeing pool base itself.
         * When ordaddr == b_area, the caller is trying to free the pool
         * header (uninitialized seglock sentinel). Skip the call. */
        {
            static uint32_t pc_RELSPACE_cached = 0;
            static int rs_gen = -1;
            if (rs_gen != g_emu_generation) {
                pc_RELSPACE_cached = boot_progress_lookup("RELSPACE");
                rs_gen = g_emu_generation;
            }
            if (pc_RELSPACE_cached && cpu->pc == pc_RELSPACE_cached) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t retpc = cpu_read32(cpu, sp);
                /* RELSPACE(ordaddr: absptr; b_area: absptr) - Pascal callee-clean.
                 * After LINK, params are at 8(A6) and 12(A6). At entry (before LINK):
                 * SP+4 = b_area (pushed last), SP+8 = ordaddr (pushed first). */
                uint32_t b_area = cpu_read32(cpu, sp + 4);
                uint32_t ordaddr = cpu_read32(cpu, sp + 8);
                if (ordaddr == b_area) {
                    DBGSTATIC(int, rs_skip, 0);
                    if (rs_skip++ < 3)
                        fprintf(stderr, "[P77-RSSKIP] RELSPACE(ordaddr=$%06X, b_area=$%06X) — skipping pool-base free\n",
                                ordaddr, b_area);
                    cpu->a[7] += 4 + 8;  /* pop retPC + 2 args */
                    cpu->pc = retpc;
                    continue;
                }
            }
        }
        /* P32 HLE bypass: QUEUE_PR (PROCASM.TEXT). Pascal vs asm
         * record-offset mismatch — fwd_ReadyQ at A5-1116 (PASCALDEFS
         * hardcoded) is uninitialized because Pascal puts it at a
         * different offset, and PCB priority field offset (PRIORITY=12
         * per PASCALDEFS) doesn't match Pascal's PCB layout either.
         * RQSCAN spins forever walking a bogus queue. Skip the queue
         * manipulation entirely — pop args + RTS.
         * Stack: retPC(4) + queue_byte(2 with A7 align) + pcb_ptr(4). */
        /* P32 disabled post-P39 — pin makes RQSCAN head-init correct.
         * Kept as if(0) for easy re-enable if downstream scheduler
         * is exercised and PCB priority-compare still spins. */
        if (0 && pc_QUEUE_PR && cpu->pc == pc_QUEUE_PR) {
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            uint32_t ret = cpu_read32(cpu, sp);
            cpu->a[7] += 10;
            cpu->pc = ret;
            continue;
        }
        /* P40: zero-fill GETSPACE returns. Pascal code (CreateProcess →
         * enqueue before ModifyProcess sets priority) reads the new
         * PCB's priority field before it's initialized; garbage bytes
         * from GETSPACE leftovers cause RQSCAN's BLE compare to spin.
         * GETSPACE sig: function GETSPACE(amount: int2; b_area: absptr;
         * var ordaddr: absptr): boolean. Stack at entry: retPC(4) +
         * amount(2) + b_area(4) + ordaddr-ptr(4). We record the args
         * at entry and, when control returns to retPC, zero `amount`
         * bytes at *ordaddr (provided allocation succeeded).
         * Single-slot state — GETSPACE is not re-entrant. */
        {
            static uint32_t gs_pending_ret = 0;
            static uint32_t gs_pending_varptr = 0;
            static uint32_t gs_pending_amount = 0;
            static int gs_pending_gen = 0;
            if (gs_pending_gen != g_emu_generation) {
                gs_pending_ret = 0; gs_pending_varptr = 0;
                gs_pending_amount = 0; gs_pending_gen = g_emu_generation;
            }
            if (pc_GETSPACE && cpu->pc == pc_GETSPACE) {
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                gs_pending_ret    = cpu_read32(cpu, sp) & 0xFFFFFF;
                gs_pending_amount = cpu_read16(cpu, sp + 4);
                gs_pending_varptr = cpu_read32(cpu, sp + 10) & 0xFFFFFF;
            } else if (gs_pending_ret && cpu->pc == gs_pending_ret) {
                uint32_t allocated = cpu_read32(cpu, gs_pending_varptr) & 0xFFFFFF;
                /* Only zero when allocation landed in RAM-like range. */
                if (allocated >= 0x000400 && allocated + gs_pending_amount <= 0xFE0000
                        && gs_pending_amount > 0 && gs_pending_amount < 0x10000) {
                    for (uint32_t i = 0; i < gs_pending_amount; i++)
                        cpu_write8(cpu, allocated + i, 0);
                }
                gs_pending_ret = 0;
            }
        }
        /* Track last-JSR/BSR/JMP source so we can identify the caller when
         * execution ends up in a garbage-code region. Captured before the
         * instruction executes, i.e. at the call-site PC. */
        static uint32_t last_jsr_pc[8];  /* ring of last 8 call-site PCs */
        static uint32_t last_jsr_target[8];
        static int last_jsr_idx = 0;
        static int last_jsr_gen = 0;
        if (last_jsr_gen != g_emu_generation) {
            memset(last_jsr_pc, 0, sizeof(last_jsr_pc));
            memset(last_jsr_target, 0, sizeof(last_jsr_target));
            last_jsr_idx = 0; last_jsr_gen = g_emu_generation;
        }
        {
            uint16_t op_peek = cpu_read16(cpu, cpu->pc);
            /* 4EB9 xxxxxxxx = JSR abs.L ; 4EB8 xxxx = JSR abs.W
             * 61xx = BSR.S/W ; 4E90..4E97 = JSR (An) */
            if (op_peek == 0x4EB9) {
                last_jsr_target[last_jsr_idx & 7] = cpu_read32(cpu, cpu->pc + 2);
                last_jsr_pc[last_jsr_idx++ & 7] = cpu->pc;
            } else if ((op_peek & 0xFF00) == 0x6100) {
                int8_t disp8 = (int8_t)(op_peek & 0xFF);
                uint32_t tgt = (disp8 == 0) ? cpu->pc + 2 + (int16_t)cpu_read16(cpu, cpu->pc + 2)
                                             : cpu->pc + 2 + disp8;
                last_jsr_target[last_jsr_idx & 7] = tgt;
                last_jsr_pc[last_jsr_idx++ & 7] = cpu->pc;
            } else if ((op_peek & 0xFFF8) == 0x4E90) {
                last_jsr_target[last_jsr_idx & 7] = cpu->a[op_peek & 7];
                last_jsr_pc[last_jsr_idx++ & 7] = cpu->pc;
            }
        }

        /* Boot-progress instrumentation: record every PC so we can see
         * which procedures have been entered. O(1) lookup, cheap. */
        boot_progress_record_pc(cpu->pc);

        /* HLE: bypass SRCH_SDSCB.Find_it. The nested Find_it walks
         * c_mmrb^.hd_sdscb_list; when the chain's self-sentinel
         * isn't correctly initialized (suspected MM_INIT codegen bug
         * for `hd_sdscb_list.fwd_link := ord(@hd_sdscb_list.fwd_link)
         * - b_sysglobal_ptr` inside WITH c_mmrb^), the walk spins
         * forever following bogus fwd_links. Bypass by just RTSing —
         * c_sdscb_ptr (the enclosing-scope var searchers use) was set
         * to nil at SRCH_SDSCB entry, so "not found" is the outcome.
         * For an empty FS at boot this is the correct answer anyway.
         *
         * Find_it is a nested proc: Pascal calls pass no args via
         * stack (uplink for enclosing locals is implicit), so the
         * bypass is pop-retaddr + RTS. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t find_it_addr = 0;
            static int fi_probed = 0;
            if (!fi_probed) { fi_probed = 1; find_it_addr = boot_progress_lookup("Find_it"); }
            if (find_it_addr && cpu->pc == find_it_addr) {
                /* Nested-proc frame: there's 1 argument (key: integer)
                 * pushed as a word, plus retaddr. Pop both. */
                uint32_t retaddr = cpu_read32(cpu, cpu->a[7]);
                cpu->a[7] += 6;  /* retaddr (4) + key (2) */
                cpu->pc = retaddr;
                cpu->cycles += 20;
                DBGSTATIC(int, fi_count, 0);
                if (fi_count++ < 3)
                    fprintf(stderr, "[HLE-Find_it #%d] bypassed, return to $%06X\n",
                            fi_count, retaddr);
                continue;
            }
        }

        /* HLE: bypass Setup_IUInfo. Our source-compile boot doesn't
         * have INTRINSIC.LIB populated via a real loader, so
         * Build_Unit_Directory's `for i := 1 to nUnits do GetObjVar(..)`
         * spins forever reading stale bytes from an empty/unseeded
         * intrinsic library file. Setup_IUInfo is only needed for
         * dynamic code loading (IU trap handler, shared-segment dir);
         * the boot path to FS_INIT / system processes doesn't strictly
         * require it. Bypassing it is the same strategy as the
         * ENTER_LOADER HLE (P24) and lets boot advance.
         *
         * Setup_IUInfo takes no Pascal args, so HLE = pop retaddr, RTS. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t setup_iuinfo_addr = 0;
            static int si_probed = 0;
            if (!si_probed) { si_probed = 1; setup_iuinfo_addr = boot_progress_lookup("Setup_IUInfo"); }
            if (setup_iuinfo_addr && cpu->pc == setup_iuinfo_addr) {
                uint32_t retaddr = cpu_read32(cpu, cpu->a[7]);
                cpu->a[7] += 4;
                cpu->pc = retaddr;
                cpu->cycles += 20;
                DBGSTATIC(int, si_count, 0);
                if (si_count++ < 3)
                    fprintf(stderr, "[HLE-Setup_IUInfo #%d] bypassed, return to $%06X\n",
                            si_count, retaddr);
                continue;
            }
        }

        /* READ_DATA caller probe: locate the outer proc that keeps
         * re-invoking READ_DATA during FS_INIT, and log its return
         * chain so we can find the retry loop. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t read_data_addr = 0;
            static int rd_probed = 0;
            if (!rd_probed) { rd_probed = 1; read_data_addr = boot_progress_lookup("READ_DATA"); }
            if (read_data_addr && cpu->pc == read_data_addr) {
                DBGSTATIC(int, rd_count, 0);
                rd_count++;
                if (rd_count < 6 || rd_count == 100 || rd_count == 1000) {
                    uint32_t sp = cpu->a[7];
                    uint32_t r0 = cpu_read32(cpu, sp);
                    /* Walk A6 chain a few links to find the deep caller */
                    uint32_t a6 = cpu->a[6];
                    fprintf(stderr, "[READ_DATA #%d] ret=$%06X A6=$%06X", rd_count, r0, a6);
                    for (int lvl = 0; lvl < 6 && a6 > 0x1000 && a6 < 0xFFFFFF; lvl++) {
                        uint32_t caller_ret = cpu_read32(cpu, (a6 + 4) & 0xFFFFFF);
                        uint32_t next_a6 = cpu_read32(cpu, a6);
                        fprintf(stderr, " ← $%06X", caller_ret & 0xFFFFFF);
                        if (next_a6 == a6) break;
                        a6 = next_a6;
                    }
                    fprintf(stderr, "\n");
                }
            }
        }

        /* VALID_AD spin probe: boot stalls in VALID_ADDR walking a
         * bogus parmcheck array. Log entry details for the first few
         * calls so we can identify the caller and why numcheck is huge. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t valid_ad_addr = 0;
            static int va_probed = 0;
            if (!va_probed) { va_probed = 1; valid_ad_addr = boot_progress_lookup("VALID_AD"); }
            if (valid_ad_addr && cpu->pc == valid_ad_addr) {
                DBGSTATIC(int, va_count, 0);
                va_count++;
                if (va_count < 8 || va_count == 100 || va_count == 1000 || va_count == 10000) {
                    /* Stack frame at entry: return addr, then pcheck
                     * (4 bytes), errnum (4 bytes, VAR ptr). RTN offset = 12. */
                    uint32_t sp = cpu->a[7];
                    uint32_t retaddr = cpu_read32(cpu, sp);
                    uint32_t arr = cpu_read32(cpu, sp + 4);
                    uint32_t errnum = cpu_read32(cpu, sp + 8);
                    uint16_t numcheck = cpu_read16(cpu, arr);
                    fprintf(stderr,
                        "[VALID_AD #%d] caller=$%06X arr=$%06X numcheck=%d errnum@$%06X\n",
                        va_count, retaddr, arr, numcheck, errnum);
                }
            }
        }

        /* HLE: smart ENTER_LOADER dispatch. Step 4a replaces the old
         * pop-and-RTS stub with a real loader-call handler in lisa.c
         * (lisa_hle_enter_loader) that decodes fake_parms and services
         * call_open / call_fill / call_byte / call_word / call_long /
         * call_move against our in-memory ldr_fs filesystem. See the
         * comment over lisa_hle_enter_loader for the layered-HLE
         * rationale and the removal plan (BT_PROFILE relink). */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            extern bool lisa_hle_enter_loader(m68k_t *cpu);
            static uint32_t enter_loader_addr = 0;
            static int el_probe_gen = -1;
            if (el_probe_gen != g_emu_generation) {
                el_probe_gen = g_emu_generation;
                enter_loader_addr = boot_progress_lookup("ENTER_LOADER");
            }
            if (enter_loader_addr && cpu->pc == enter_loader_addr) {
                if (lisa_hle_enter_loader(cpu))
                    continue;
            }
        }

        /* Diagnostic: trace entry to UP() to see config_ptr/drvrec state.
         * UP(var errnum, config_ptr, callers_config_ptr) — right-to-left
         * push ⇒ at entry: A7+0=retPC, A7+4=&errnum, A7+8=config_ptr,
         * A7+12=callers_config_ptr. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t up_addr = 0;
            static int up_probe_gen = -1;
            DBGSTATIC(int, up_trace, 0);
            if (up_probe_gen != g_emu_generation) {
                up_probe_gen = g_emu_generation;
                up_addr = boot_progress_lookup("UP");
            }
            if (up_addr && cpu->pc == up_addr && up_trace < 4) {
                up_trace++;
                uint32_t sp = cpu->a[7];
                uint32_t ret_pc = cpu_read32(cpu, sp);
                uint32_t cfg_ptr = cpu_read32(cpu, sp + 8);
                uint32_t caller_cfg = cpu_read32(cpu, sp + 12);
                uint32_t req_drvr = cfg_ptr ? cpu_read32(cpu, cfg_ptr + 60) : 0;
                fprintf(stderr, "HLE UP entry: ret=$%06X cfg=$%06X caller=$%06X req_drvr=$%06X\n",
                        ret_pc, cfg_ptr, caller_cfg, req_drvr);
            }
        }

        /* Diagnostic: trace post-BOOT_IO_INIT checkpoints — see which procs
         * actually get entered. Addresses resolved each generation. */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            struct { const char *name; uint32_t addr; } pcs[] = {
                {"INIT_BOOT_CDS",  0}, {"PARAMEMINIT",    0},
                {"MAKE_BUILTIN",   0}, {"FS_INIT",        0},
                {"Sys_Proc_Init",  0}, {"CONFIG_DOWN",    0},
                {"LD_DISABLE",     0}, {"INITUID",        0},
                {"SET_PREFERENCES",0}, {"DiskSync",       0},
                {"PARAMEM_WRITE",  0}, {"TIMER_UNBLK",    0},
                {"ALRM",           0},
                /* P103 FS_Master_Init subcall trace */
                {"FS_Master_Init", 0}, {"InitQVM",        0},
                {"InitBufPool",    0}, {"InitFS",         0},
                {"fs_mount",       0}, {"real_mount",     0},
                {"def_mount",      0}, {"MDDF_IO",        0},
                {"UltraIO",        0},
            };
            static uint32_t pcs_cache[24];
            static int pci_gen = -1;
            if (pci_gen != g_emu_generation) {
                pci_gen = g_emu_generation;
                for (int i = 0; i < (int)(sizeof(pcs)/sizeof(pcs[0])); i++)
                    pcs_cache[i] = boot_progress_lookup(pcs[i].name);
            }
            DBGSTATIC(int, pci_trace, 0);
            static bool after_parameminit = false;
            DBGSTATIC(int, post_gs_count, 0);
            if (pci_trace < 40) {
                for (int i = 0; i < (int)(sizeof(pcs)/sizeof(pcs[0])); i++) {
                    if (pcs_cache[i] && cpu->pc == pcs_cache[i]) {
                        pci_trace++;
                        fprintf(stderr, "[POST-BOOT] entered %s @$%06X (A7=$%08X A6=$%08X SR=$%04X)\n",
                                pcs[i].name, cpu->pc, cpu->a[7], cpu->a[6], cpu->sr);
                        if (strcmp(pcs[i].name, "PARAMEMINIT") == 0)
                            after_parameminit = true;
                        /* P104-diag: at MDDF_IO entry, full dump of
                         * configinfo[device]^ so we can find the byte offsets
                         * of devt and blockstructured empirically. */
                        if (strcmp(pcs[i].name, "MDDF_IO") == 0) {
                            uint32_t sp = cpu->a[7];
                            uint16_t device = cpu_read16(cpu, sp + 8);
                            extern uint32_t boot_progress_lookup(const char *name);
                            uint32_t cfgi_off = boot_progress_lookup("configinfo");
                            uint32_t a5 = cpu->a[5];
                            uint32_t cfgi_abs = (a5 + cfgi_off) & 0xFFFFFF;
                            uint32_t cfg_ptr = cpu_read32(cpu, cfgi_abs + device * 4);
                            fprintf(stderr, "  cfg[%d] at $%06X pts->$%06X\n",
                                    device, cfgi_abs + device*4, cfg_ptr);
                            for (uint32_t off = 0; off < 96; off += 8) {
                                fprintf(stderr, "    +%02X:", off);
                                for (int b = 0; b < 8; b++) {
                                    uint32_t a = (cfg_ptr + off + b) & 0xFFFFFF;
                                    uint16_t w = cpu_read16(cpu, a & ~1u);
                                    uint8_t byte = (a & 1) ? (w & 0xFF) : (w >> 8);
                                    fprintf(stderr, " %02X", byte);
                                }
                                fprintf(stderr, "\n");
                            }
                        }
                        break;
                    }
                }
            }
            /* After PARAMEMINIT, count GETSPACE calls to see if for-loop runs. */
            /* Trace AlarmAssign internals */
            /* P102b replaced P102a: the nil-A1 TrapTable copy came from a
             * call at BOOT_IO_INIT:$5CF4 that mis-resolved INIT_TWIGGGLOB
             * (3 Gs, 14 chars, from STARTUP.TEXT:1906) to INIT_TWIG_TABLE
             * (asm, same 8-char prefix INIT_TWI). find_proc_sig/linker
             * now pick the longest-common-prefix ENTRY on 8-char collisions,
             * so INIT_TWIGGGLOB resolves to INIT_TWIGGLOB (Pascal, LCP=10)
             * over INIT_TWIG_TABLE (LCP=9). With the right callee, the
             * error VAR param pushes @error, INIT_TWIG_TABLE is never
             * called from $5CF4, and the nil-copy scaffold is dead. */
            if (after_parameminit) {
                static uint32_t getspace_addr = 0;
                static int gs_probe_gen = -1;
                if (gs_probe_gen != g_emu_generation) {
                    gs_probe_gen = g_emu_generation;
                    getspace_addr = boot_progress_lookup("GETSPACE");
                }
                if (getspace_addr && cpu->pc == getspace_addr && post_gs_count < 15) {
                    post_gs_count++;
                    fprintf(stderr, "[POST-PMINIT GETSPACE #%d] A6=$%08X SR=$%04X\n",
                            post_gs_count, cpu->a[6], cpu->sr);
                }
            }
        }

        /* Diagnostic: on entry to FIND_EMPTYSLOT, dump the first few configinfo[]
         * entries and their devnames so we can see why the scan doesn't find a
         * 'BITBKT' slot. configinfo lives at A5-$A0 (signed 32-bit global offset
         * per the map), 40 slots × 4 bytes. devname is at devrec+16 (Pascal
         * string: length byte + chars). */
        {
            extern uint32_t boot_progress_lookup(const char *name);
            static uint32_t fes_addr = 0;
            static int fes_probe_gen = -1;
            DBGSTATIC(int, fes_trace, 0);
            if (fes_probe_gen != g_emu_generation) {
                fes_probe_gen = g_emu_generation;
                fes_addr = boot_progress_lookup("FIND_EMPTYSLOT");
            }
            if (fes_addr && cpu->pc == fes_addr && fes_trace < 3) {
                fes_trace++;
                uint32_t a5 = cpu->a[5];
                uint32_t base = a5 - 0xA0;
                fprintf(stderr, "HLE FIND_EMPTYSLOT entry #%d: A5=$%08X configinfo@$%08X\n",
                        fes_trace, a5, base);
                /* Print slots 0..15 and maxdev-7..maxdev */
                for (int i = 0; i < 40; i++) {
                    if (i >= 4 && i < 32) continue;  /* skip middle to reduce noise */
                    uint32_t slot_addr = base + i * 4;
                    uint32_t dr = cpu_read32(cpu, slot_addr);
                    char name[8] = {0};
                    int nlen = 0;
                    if (dr && dr < 0x01000000) {
                        nlen = cpu_read8(cpu, dr + 16);  /* string length byte */
                        if (nlen > 7) nlen = 7;
                        for (int k = 0; k < nlen; k++) {
                            uint8_t ch = cpu_read8(cpu, dr + 17 + k);
                            name[k] = (ch >= 32 && ch < 127) ? ch : '?';
                        }
                    }
                    fprintf(stderr, "  [%2d] ptr=$%06X devname.len=%d \"%s\"\n",
                            i, dr, nlen, name);
                }
            }
        }

        /* Jump-only ring: records (src_pc, dst_pc) pairs when PC changes
         * discontinuously (JSR, JMP, RTS, BRA, BSR with distance > 8). */
        static struct { uint32_t src, dst; } jmp_ring[128];
        static int jmp_idx = 0;
        static int jmp_ring_gen = 0;
        static uint32_t prev_pc_for_jmp = 0;
        if (jmp_ring_gen != g_emu_generation) { memset(jmp_ring, 0, sizeof(jmp_ring)); jmp_idx = 0; jmp_ring_gen = g_emu_generation; prev_pc_for_jmp = 0; }
        if (prev_pc_for_jmp) {
            int32_t d = (int32_t)cpu->pc - (int32_t)prev_pc_for_jmp;
            if (d > 8 || d < -4) {
                jmp_ring[jmp_idx & 127].src = prev_pc_for_jmp;
                jmp_ring[jmp_idx & 127].dst = cpu->pc;
                jmp_idx++;
            }
        }
        prev_pc_for_jmp = cpu->pc;

        /* Trace each JSR $A17A0 call (from $A1996) — log the pushed
         * longint parameter + caller-frame locals so we can see what
         * pointer is being passed and where it came from. */
        {
            DBGSTATIC(int, jsr_a17a0_count, 0);
            if (cpu->pc == 0xA1996 && jsr_a17a0_count < 6) {
                jsr_a17a0_count++;
                /* At this point SP points at the pushed parameter (MOVE.L D0,-(SP) at $A1994
                 * just executed). The top-of-stack 4 bytes are the param. */
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t param = cpu_read32(cpu, sp);
                uint32_t local_m4 = cpu_read32(cpu, (cpu->a[6] & 0xFFFFFF) - 4);
                uint32_t local_m8 = cpu_read32(cpu, (cpu->a[6] & 0xFFFFFF) - 8);
                uint32_t arg_p8   = cpu_read32(cpu, (cpu->a[6] & 0xFFFFFF) + 8);
                uint32_t arg_p12  = cpu_read32(cpu, (cpu->a[6] & 0xFFFFFF) + 12);
                fprintf(stderr, "=== JSR $A17A0 call #%d at $A1996 ===\n", jsr_a17a0_count);
                fprintf(stderr, "  param-on-stack = $%08X\n", param);
                fprintf(stderr, "  caller frame A6=$%08X: -4=$%08X  -8=$%08X  +8=$%08X  +12=$%08X\n",
                        cpu->a[6], local_m4, local_m8, arg_p8, arg_p12);
                fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X D7=$%08X A0=$%08X A5=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3], cpu->d[7], cpu->a[0], cpu->a[5]);
            }
            /* Log entry/exit of MM_INIT and GETSPACE calls within it,
             * to see whether mmrb_addr gets written. */
            DBGSTATIC(int, mminit_state, 0); /* 0=before, 1=inside */
            if (cpu->pc == 0xA71E0 && mminit_state == 0) {
                mminit_state = 1;
                fprintf(stderr, "=== ENTER MM_INIT @ $A69F2, mmrb_addr sysglobal value = ?\n");
                /* dump the global mmrb_addr location — we need to know where it lives.
                 * A5-relative offset from lisa.c:2451 is -1264. So addr = A5 - 1264. */
                uint32_t ga = (cpu->a[5] & 0xFFFFFF) - 1264;
                uint32_t gv = cpu_read32(cpu, ga);
                fprintf(stderr, "    mmrb_addr at logical $%06X = $%08X (A5=$%08X)\n", ga, gv, cpu->a[5]);
            }
            /* Log each JSR to GETSPACE while in MM_INIT */
            /* Turn off the giant trace — watch head_sdb memory writes instead. */
            #if 0
            /* Dump MM_INIT body around where head_sdb sentinels are written.
             * We want to see the compiled bytes for 'memchain.fwd_link := @memchain.fwd_link'.
             * Once we enter MM_INIT, scan forward and log PC + instruction for
             * the first ~200 instructions, then we can trace by hand. */
            DBGSTATIC(int, mminit_trace_count, 0);
            if (mminit_state == 1 && mminit_trace_count < 180) {
                /* Only log PCs we haven't seen (detect loops) and only log the first
                 * ~180 distinct instructions by incrementing counter. */
                static uint32_t last_logged_pc = 0;
                if (cpu->pc != last_logged_pc) {
                    last_logged_pc = cpu->pc;
                    /* Filter: only log PCs in MM_INIT body range — $A6F1A..$A77FF (rough) */
                    if (cpu->pc >= 0xA6F1A && cpu->pc <= 0xA7800) {
                        fprintf(stderr, "MM_INIT trace[%3d]: PC=$%06X op=$%04X%04X\n",
                                mminit_trace_count++, cpu->pc,
                                cpu_read16(cpu, cpu->pc), cpu_read16(cpu, cpu->pc + 2));
                    }
                }
            }

            #endif

            /* Watch writes into head_sdb region ($CCA020..$CCA03F) during MM_INIT.
             * We can't hook the store directly here, but we can sample the memory
             * region every instruction while inside MM_INIT and log changes. */
            if (mminit_state == 1) {
                static uint32_t sdb_prev[8] = {0,0,0,0,0,0,0,0};
                static int sdb_prev_gen = 0;
                static int sdb_log_count = 0;
                if (sdb_prev_gen != g_emu_generation) {
                    memset(sdb_prev, 0, sizeof(sdb_prev));
                    sdb_log_count = 0;
                    sdb_prev_gen = g_emu_generation;
                }
                for (int i = 0; i < 8 && sdb_log_count < 40; i++) {
                    uint32_t a = 0xCCA020 + i*4;
                    uint32_t v = cpu_read32(cpu, a);
                    if (v != sdb_prev[i]) {
                        fprintf(stderr, "  head_sdb write: $%06X = $%08X  (PC=$%06X op=$%04X)\n",
                                a, v, cpu->pc, cpu_read16(cpu, cpu->pc));
                        sdb_prev[i] = v;
                        sdb_log_count++;
                    }
                }
            }

            /* Log every GETSPACE call with correctly-sized args.
             * GETSPACE signature (SYSG1.TEXT:158):
             *   function GETSPACE(amount: int2; b_area: absptr; var ordaddr: absptr): boolean
             * Stack after JSR: retPC(4) | amount(2) | b_area(4) | ordaddr-ptr(4). */
            DBGSTATIC(int, gs_all_count, 0);
            if (cpu->pc == 0x5FAA && gs_all_count < 40) {
                gs_all_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc = cpu_read32(cpu, sp);
                uint16_t amount = cpu_read16(cpu, sp + 4);
                uint32_t b_area = cpu_read32(cpu, sp + 6);
                uint32_t varptr = cpu_read32(cpu, sp + 10);
                /* GETSPACE internally: if b_area == b_sysglobal_ptr, adj := b_area - 24575.
                 * b_sysglobal_ptr is odd (ends in $EB1 or similar); syslocal b_area is even.
                 * Treat any odd b_area as sysglobal and subtract 24575. */
                uint32_t adj = (b_area & 1) ? (b_area - 24575) : b_area;
                uint32_t pool_hdr_ptr = cpu_read32(cpu, adj);
                uint32_t h0 = cpu_read32(cpu, pool_hdr_ptr);
                uint32_t h4 = cpu_read32(cpu, pool_hdr_ptr + 4);
                uint32_t h8 = cpu_read32(cpu, pool_hdr_ptr + 8);
                uint16_t firstfree = cpu_read16(cpu, pool_hdr_ptr);
                uint16_t poolsize = cpu_read16(cpu, pool_hdr_ptr + 2);
                uint16_t freecount = cpu_read16(cpu, pool_hdr_ptr + 4);
                fprintf(stderr, "GETSPACE #%d ret=$%06X amt=%d b_area=$%08X var=$%08X A5=$%08X A7=$%08X adj=$%06X hdr=$%06X [ff=$%04X sz=$%04X fc=$%04X] %08X %08X %08X\n",
                        gs_all_count, ret_pc, amount, b_area, varptr,
                        cpu->a[5], cpu->a[7], adj & 0xFFFFFF,
                        pool_hdr_ptr & 0xFFFFFF, firstfree, poolsize, freecount, h0, h4, h8);
            }

            DBGSTATIC(int, gs_count, 0);
            if (cpu->pc == 0x5CEA && mminit_state == 1 && gs_count < 4) {
                gs_count++;
                /* GETSPACE signature: function GETSPACE(size: longint; ptrsysg: absptr; var addr: longint): boolean;
                 * params pushed right-to-left: var addr (4 bytes, ADDRESS), ptrsysg (4), size (4), return slot (2 or 4).
                 * After JSR pushes return PC, frame has: [SP]=retPC, [SP+4]=var-addr-ptr, [SP+8]=ptrsysg, [SP+12]=size. */
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc = cpu_read32(cpu, sp);
                /* Probe several word offsets to catch the var parameter. */
                fprintf(stderr, "=== GETSPACE call #%d  ret=$%06X  SP=$%06X\n", gs_count, ret_pc, sp);
                for (int off = 4; off <= 20; off += 2)
                    fprintf(stderr, "    SP+%2d = $%08X\n", off, cpu_read32(cpu, sp + off));
            }
            /* On first SYSTEM_ERROR, dump the syslocal pool header at $CE0000
             * to diagnose whether POOL_INIT initialized the syslocal pool. */
            DBGSTATIC(int, syslocal_pool_dumped, 0);
            if (cpu->pc == 0x5380 && !syslocal_pool_dumped) {
                syslocal_pool_dumped = 1;
                fprintf(stderr, "=== syslocal pool check ($CE0000):\n");
                for (int off = 0; off < 32; off += 4) {
                    uint32_t v = cpu_read32(cpu, 0xCE0000 + off);
                    fprintf(stderr, "    $%06X = $%08X\n", 0xCE0000 + off, v);
                }
                fprintf(stderr, "=== sysglobal pool ($CC0000):\n");
                for (int off = 0; off < 32; off += 4) {
                    uint32_t v = cpu_read32(cpu, 0xCC0000 + off);
                    fprintf(stderr, "    $%06X = $%08X\n", 0xCC0000 + off, v);
                }
            }

            /* Trace first SYSTEM_ERROR call — dump caller chain and
             * instruction bytes around the JSR site. Uses the live
             * hle_addr_system_error (set by toolchain_bridge at link),
             * NOT a hardcoded PC — addresses shift whenever codegen
             * changes. */
            extern uint32_t hle_addr_system_error;
            DBGSTATIC(int, syserr_dumped, 0);
            if (hle_addr_system_error && cpu->pc == hle_addr_system_error && !syserr_dumped) {
                syserr_dumped = 1;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                uint32_t ret_pc = cpu_read32(cpu, sp);
                uint16_t errcode = cpu_read16(cpu, sp + 4);
                fprintf(stderr, "=== SYSTEM_ERROR entered: code=%d ret=$%06X SP=$%06X A6=$%06X\n",
                        errcode, ret_pc, sp, cpu->a[6]);
                fprintf(stderr, "  Bytes around caller (ret-16..ret+4):\n   ");
                for (int b = -16; b < 8; b += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, ret_pc + b));
                fprintf(stderr, "\n");
                /* Grand-parent PC bytes */
                uint32_t gpc = cpu_read32(cpu, (cpu->a[6] & 0xFFFFFF) + 4);
                fprintf(stderr, "  Bytes around grand-parent (ret=$%06X -16..+4):\n   ", gpc);
                for (int b = -16; b < 8; b += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, gpc + b));
                fprintf(stderr, "\n");
                /* D0-D7 + A0-A5 at the moment of SYSTEM_ERROR */
                fprintf(stderr, "  Regs: D0=$%08X D1=$%08X D2=$%08X D7=$%08X A0=$%08X A5=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[7], cpu->a[0], cpu->a[5]);
                fprintf(stderr, "  Stack walk (looking for saved return PCs):\n");
                for (int off = 0; off <= 80; off += 4) {
                    uint32_t v = cpu_read32(cpu, sp + off);
                    if (v >= 0x400 && v < 0xE0000)
                        fprintf(stderr, "    [SP+%2d] = $%06X\n", off, v);
                }
                /* Also walk A6-link chain */
                fprintf(stderr, "  A6-link chain:\n");
                uint32_t a6 = cpu->a[6] & 0xFFFFFF;
                for (int i = 0; i < 10 && a6 > 0x100 && a6 < 0xFFFFFF; i++) {
                    uint32_t prev_a6 = cpu_read32(cpu, a6);
                    uint32_t ret = cpu_read32(cpu, a6 + 4);
                    fprintf(stderr, "    A6=$%06X prev_A6=$%06X ret=$%06X\n", a6, prev_a6, ret);
                    if (prev_a6 == a6) break;
                    a6 = prev_a6 & 0xFFFFFF;
                }
                /* Last 20 PCs — helps pinpoint which JSR called SYSTEM_ERROR. */
                fprintf(stderr, "  Last 20 PCs:");
                for (int ri = 20; ri > 0; ri--)
                    fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                fprintf(stderr, "\n");
            }

            /* Dump P_ENQUEUE ($A1B00) prologue on first call */
            DBGSTATIC(int, penq_dumped, 0);
            if (cpu->pc == 0xA1C82 && !penq_dumped) {
                penq_dumped = 1;
                fprintf(stderr, "=== P_ENQUEUE bytes at $A1B00-$A1B5F:");
                for (int b = 0; b < 96; b += 2) {
                    if (b % 32 == 0) fprintf(stderr, "\n   $%06X: ", 0xA1B00 + b);
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xA1B00 + b));
                }
                fprintf(stderr, "\n");
            }

            /* Dump head_sdb sentinels at $CCA020 on first INSERTSDB entry */
            DBGSTATIC(int, sentinel_dumped, 0);
            if (cpu->pc == 0xA1EE6 && !sentinel_dumped) {
                sentinel_dumped = 1;
                fprintf(stderr, "=== head_sdb sentinels at $CCA020 (should be self-refs):\n");
                for (int off = 0; off < 40; off += 4) {
                    uint32_t a = 0xCCA020 + off;
                    fprintf(stderr, "    $%06X = $%08X\n", a, cpu_read32(cpu, a));
                }
            }

            /* Dump the first 32 bytes of INSERTSDB ($A17A0) on first entry */
            DBGSTATIC(int, insertsdb_dumped, 0);
            if (cpu->pc == 0xA1EE6 && !insertsdb_dumped) {
                insertsdb_dumped = 1;
                fprintf(stderr, "=== INSERTSDB prologue bytes at $A17A0-$A17BF:");
                for (int b = 0; b < 32; b += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xA17A0 + b));
                fprintf(stderr, "\n");
                /* And MAKE_FREE (\$A18E6) prologue */
                fprintf(stderr, "=== MAKE_FREE prologue bytes at $A18E6-$A1905:");
                for (int b = 0; b < 32; b += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xA18E6 + b));
                fprintf(stderr, "\n");
                /* And MM_INIT prologue — to see where @mmrb_addr is computed */
                fprintf(stderr, "=== MM_INIT prologue bytes at $A69F2-$A6A11:");
                for (int b = 0; b < 32; b += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xA69F2 + b));
                fprintf(stderr, "\n");
            }

            /* After GETSPACE call #1 returns, dump memory around expected mmrb_addr location */
            DBGSTATIC(int, gs1_dumped, 0);
            if (cpu->pc == 0x0A6A10 && gs_count >= 1 && !gs1_dumped) {
                gs1_dumped = 1;
                uint32_t a5 = cpu->a[5] & 0xFFFFFF;
                fprintf(stderr, "=== After GETSPACE#1 return  A5=$%06X, D0=$%08X (return value)\n", a5, cpu->d[0]);
                for (int off = -1280; off <= -1240; off += 4) {
                    uint32_t addr = a5 + off;
                    uint32_t v = cpu_read32(cpu, addr);
                    fprintf(stderr, "    A5%+5d ($%06X) = $%08X\n", off, addr, v);
                }
            }
            /* On exit of MM_INIT (RTS, approximately): report final mmrb_addr. Just sample periodically. */
            DBGSTATIC(int, mminit_done, 0);
            if (mminit_state == 1 && !mminit_done && cpu->pc == 0x09A8) {
                /* hack: any PC back in STARTUP $900-$A00 range likely means MM_INIT has returned */
                mminit_done = 1;
                uint32_t ga = (cpu->a[5] & 0xFFFFFF) - 1264;
                uint32_t gv = cpu_read32(cpu, ga);
                fprintf(stderr, "=== MM_INIT RETURNED (heuristic), mmrb_addr = $%08X\n", gv);
            }

            /* Also log ENTRY to $A18E6 — to see what was passed to the outer helper */
            DBGSTATIC(int, jsr_a18e6_count, 0);
            if (cpu->pc == 0xA18E6 && jsr_a18e6_count < 6) {
                jsr_a18e6_count++;
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                /* Stack at entry: [SP]=return PC (4 bytes), [SP+4]=pushed word param. */
                uint32_t ret_pc = cpu_read32(cpu, sp);
                uint16_t arg_w = cpu_read16(cpu, sp + 4);
                fprintf(stderr, "=== ENTER $A18E6 call #%d  return=$%06X  word-arg=$%04X  D0=$%08X D3=$%08X ===\n",
                        jsr_a18e6_count, ret_pc, arg_w, cpu->d[0], cpu->d[3]);
            }
        }

        /* One-shot caller trace for the MODEMA poll hang at $A17BC-$A17F6.
         * Dumps the last 64 PCs so we can see which init routine entered
         * here, plus the enclosing JSR targets. */
        {
            DBGSTATIC(int, modema_fired, 0);
            if (!modema_fired && cpu->pc >= 0xA17BC && cpu->pc <= 0xA17F6) {
                modema_fired = 1;
                fprintf(stderr, "=== MODEMA-POLL CALLER TRACE (first hit at PC=$%06X) ===\n", cpu->pc);
                fprintf(stderr, "  SR=$%04X A0=$%08X A5=$%08X A6=$%08X SP=$%08X\n",
                        cpu->sr, cpu->a[0], cpu->a[5], cpu->a[6], cpu->a[7]);
                fprintf(stderr, "  Last 64 PCs (oldest first):\n   ");
                for (int ri = 64; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    fprintf(stderr, " $%06X", rpc);
                    if ((ri % 8) == 1) fprintf(stderr, "\n   ");
                }
                fprintf(stderr, "\n");
                /* Also walk stack looking for saved return PCs (JSR pushes PC+4-byte) */
                fprintf(stderr, "  Stack walk (A7..A7+64) for saved PCs:\n");
                uint32_t sp = cpu->a[7] & 0xFFFFFF;
                for (int off = 0; off <= 64; off += 4) {
                    uint32_t v = cpu_read32(cpu, sp + off);
                    /* Print anything in text-segment range $400..$E0000 */
                    if (v >= 0x400 && v < 0xE0000)
                        fprintf(stderr, "    [SP+%2d] = $%06X\n", off, v);
                }
                fprintf(stderr, "  Last 128 jumps (src → dst):\n");
                uint32_t last_src=0, last_dst=0; int repeat=0;
                for (int ji = 128; ji > 0; ji--) {
                    int idx = (jmp_idx - ji) & 127;
                    uint32_t s=jmp_ring[idx].src, d=jmp_ring[idx].dst;
                    if (!s && !d) continue;
                    if (s==last_src && d==last_dst) { repeat++; continue; }
                    if (repeat) { fprintf(stderr, "      (× %d more)\n", repeat); repeat=0; }
                    fprintf(stderr, "    $%06X → $%06X\n", s, d);
                    last_src=s; last_dst=d;
                }
                if (repeat) fprintf(stderr, "      (× %d more)\n", repeat);
                /* Dump opcodes at the critical call site and target */
                fprintf(stderr, "  Bytes at $000950..$00097F:");
                for (int b=0; b<48; b+=2) fprintf(stderr, " %04X", cpu_read16(cpu, 0x950 + b));
                fprintf(stderr, "\n  Bytes at $0A18E0..$0A190F:");
                for (int b=0; b<48; b+=2) fprintf(stderr, " %04X", cpu_read16(cpu, 0xA18E0 + b));
                fprintf(stderr, "\n  Bytes at $0A1990..$0A19BF:");
                for (int b=0; b<48; b+=2) fprintf(stderr, " %04X", cpu_read16(cpu, 0xA1990 + b));
                fprintf(stderr, "\n");
                fprintf(stderr, "=== END MODEMA CALLER TRACE ===\n");
            }
        }


        /* Push-into-vector-table guard: fires when A7 drops into the
         * exception-vector region ($0..$1000). Handoff lead is that SP
         * briefly lands at $414 during process creation at PC=$20820C
         * (op=$4FF8 LEA abs.W,A7) and a subsequent push would corrupt
         * vector $414 = line-1010. We catch the LEA itself (not a push
         * but also a write to A7), and any real push while SP<$1000. */
        {
            DBGSTATIC(uint32_t, pg_prev_sp, 0);
            DBGSTATIC(uint32_t, pg_prev_pc, 0);
            DBGSTATIC(uint16_t, pg_prev_ir, 0);
            DBGSTATIC(int, pg_fired, 0);
            uint32_t sp_now = cpu->a[7] & 0xFFFFFF;
            if (pg_prev_sp != 0 && sp_now < pg_prev_sp && sp_now < 0x1000 && pg_fired < 10) {
                pg_fired++;
                fprintf(stderr,
                    "!!! A7 INTO VECTORS[%d]: $%06X -> $%06X (delta=%d)\n"
                    "    caused by PC=$%06X ir=$%04X  now at PC=$%06X op=$%04X\n"
                    "    A6=$%08X SR=$%04X D0=$%08X\n",
                    pg_fired, pg_prev_sp, sp_now, (int)sp_now - (int)pg_prev_sp,
                    pg_prev_pc, pg_prev_ir, cpu->pc, cpu_read16(cpu, cpu->pc),
                    cpu->a[6], cpu->sr, cpu->d[0]);
                fprintf(stderr, "    Last 20 PCs:");
                for (int ri = 20; ri > 0; ri--)
                    fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                fprintf(stderr, "\n");
            }
            pg_prev_sp = sp_now;
            pg_prev_pc = cpu->pc;
            pg_prev_ir = cpu->ir;
        }

        /* SP watermark: catch stack leak in both physical and mapped space */
        {
            DBGSTATIC(uint32_t, lowest_sp, 0xFFFFFF);
            uint32_t sp = cpu->a[7] & 0xFFFFFF;
            if (sp < lowest_sp && sp > 0x100) {
                lowest_sp = sp;
                /* Trigger at various thresholds */
                if ((sp < 0x20000 || (sp > 0xC00000 && sp < 0xCB0000)) && sp != 0) {
                    DBGSTATIC(int, sp_log, 0);
                    if (sp_log++ < 5) {
                        fprintf(stderr, "!!! SP LOW: $%06X at PC=$%06X A6=$%08X\n",
                                sp, cpu->pc, cpu->a[6]);
                        fprintf(stderr, "    Last 10 PCs:");
                        for (int ri = 10; ri > 0; ri--)
                            fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
        /* Monitor A5 for corruption */
        {
            DBGSTATIC(uint32_t, last_a5, 0);
            DBGSTATIC(int, a5_log, 0);
            if (a5_log < 3 && cpu->a[5] != last_a5 && last_a5 != 0) {
                if ((last_a5 < 0x200000 && cpu->a[5] > 0x200000) ||
                    (last_a5 > 0x200000 && cpu->a[5] < 0x200000 && cpu->a[5] != 0)) {
                    fprintf(stderr, "=== A5 CHANGED: $%08X → $%08X at PC=$%06X\n",
                            last_a5, cpu->a[5], cpu->pc);
                    a5_log++;
                }
            }
            last_a5 = cpu->a[5];
        }
        /* Trace PASCALINIT internals: monitor the $DFC00-$DFD00 range */
        {
            DBGSTATIC(int, pi_trace, 0);
            /* Detect PASCALINIT entry dynamically from the main body JSR */
            if (cpu->pc >= 0x4000 && cpu->pc < 0x5000 && pascalinit_addr == 0) {
                /* Read the JSR target from the main body */
                uint16_t op = cpu_read16(cpu, cpu->pc);
                if (op == 0x4EB9) {
                    pascalinit_addr = cpu_read32(cpu, cpu->pc + 2);
                    fprintf(stderr, ">>> PASCALINIT detected at $%06X\n", pascalinit_addr);
                }
            }
            /* Once we know PASCALINIT's address, trace key points */
            if (pascalinit_addr > 0 && pi_trace < 5) {
                /* Trace when PC is within PASCALINIT's code (assume ~100 bytes) */
                if (cpu->pc >= pascalinit_addr && cpu->pc < pascalinit_addr + 120) {
                    uint32_t offset = cpu->pc - pascalinit_addr;
                    /* Only trace at key offsets: entry, after copy loop, after JSR, return */
                    if (offset <= 0x50 && (offset % 2) == 0) {
                        fprintf(stderr, ">>> PI+$%02X: PC=$%06X op=$%04X A0=$%08X A5=$%08X SP=$%08X\n",
                                offset, cpu->pc, cpu_read16(cpu, cpu->pc),
                                cpu->a[0], cpu->a[5], cpu->a[7]);
                        pi_trace++;
                    }
                }
            }
        }
        /* Watch A5: catch when it drops from normal range (>$10000) to small value */
        {
            DBGSTATIC(uint32_t, prev_a5_drop, 0);
            DBGSTATIC(int, a5_drop, 0);
            if (prev_a5_drop > 0x10000 && cpu->a[5] < 0x1000 && a5_drop < 3) {
                a5_drop++;
                fprintf(stderr, "!!! A5 DROP: $%08X → $%08X at PC=$%06X op=$%04X (ir=$%04X)\n",
                        prev_a5_drop, cpu->a[5], cpu->pc, cpu_read16(cpu, cpu->pc), cpu->ir);
                fprintf(stderr, "    Last 30 PCs:");
                for (int ri = 30; ri > 0; ri--)
                    fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                fprintf(stderr, "\n    SR=$%04X SP=$%08X A6=$%08X D0=$%08X\n",
                        cpu->sr, cpu->a[7], cpu->a[6], cpu->d[0]);
                /* Stack top */
                fprintf(stderr, "    Stack:");
                for (int si = 0; si < 8; si++)
                    fprintf(stderr, " $%08X", cpu_read32(cpu, (cpu->a[7] + si*4) & 0xFFFFFF));
                fprintf(stderr, "\n");
            }
            prev_a5_drop = cpu->a[5];
        }
        /* Detect when PC enters non-code I/O space ($FF0000+, excluding ROM $FE0000-$FEFFFF) */
        {
            DBGSTATIC(int, io_exec, 0);
            uint32_t mpc = cpu->pc & 0xFFFFFF;
            if (mpc >= 0xFF0000 && io_exec < 3) {
                io_exec++;
                fprintf(stderr, "!!! PC IN I/O SPACE: PC=$%06X op=$%04X SR=$%04X SP=$%08X\n",
                        cpu->pc, cpu_read16(cpu, cpu->pc), cpu->sr, cpu->a[7]);
                fprintf(stderr, "    Last 120 PCs:");
                for (int ri = 120; ri > 0; ri--)
                    fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                fprintf(stderr, "\n    A6=$%08X D0=$%08X\n", cpu->a[6], cpu->d[0]);
            }
        }
        /* Detect PC entering vector table ($0-$3EF) — indicates stack corruption
         * or null function pointer. $3F0 is the stub, so exclude that. */
        {
            DBGSTATIC(int, vec_exec_count, 0);
            uint32_t masked = cpu->pc & 0xFFFFFF;
            if (masked < 0x3F0 && masked > 0 && vec_exec_count < 3) {
                vec_exec_count++;
                fprintf(stderr, "!!! PC IN VECTOR TABLE: PC=$%06X op=$%04X SP=$%08X SR=$%04X\n",
                        cpu->pc, cpu_read16(cpu, cpu->pc), cpu->a[7], cpu->sr);
                fprintf(stderr, "    A0=$%08X A5=$%08X A6=$%08X D0=$%08X\n",
                        cpu->a[0], cpu->a[5], cpu->a[6], cpu->d[0]);
                fprintf(stderr, "    Last 20 PCs:");
                for (int ri = 20; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    fprintf(stderr, " $%06X", rpc);
                }
                fprintf(stderr, "\n    Stack top:");
                for (int si = 0; si < 8; si++)
                    fprintf(stderr, " $%08X", cpu_read32(cpu, (cpu->a[7] + si*4) & 0xFFFFFF));
                fprintf(stderr, "\n");
            }
        }
        /* Detect PC=$2A specifically — "Illegal Instruction at PC=$2A" debug.
         * $2A is 2 bytes into the Line-A vector entry at $28. Something is
         * jumping/returning here. Dump full ring buffer, regs, and stack. */
        {
            DBGSTATIC(int, pc2a_count, 0);
            uint32_t masked = cpu->pc & 0xFFFFFF;
            if (masked == 0x2A && pc2a_count < 5) {
                pc2a_count++;
                fprintf(stderr, "\n!!! PC=$00002A HIT (#%d) — in Line-A vector entry\n", pc2a_count);
                fprintf(stderr, "    op=$%04X SP=$%08X SR=$%04X\n",
                        cpu_read16(cpu, cpu->pc), cpu->a[7], cpu->sr);
                fprintf(stderr, "    Last 30 PCs (oldest→newest):\n");
                for (int ri = 30; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    uint16_t rop = cpu_read16(cpu, rpc);
                    fprintf(stderr, "      PC=$%06X op=$%04X", rpc, rop);
                    if (rop == 0x4E75) fprintf(stderr, "  RTS");
                    else if (rop == 0x4E73) fprintf(stderr, "  RTE");
                    else if ((rop & 0xFFC0) == 0x4EC0) fprintf(stderr, "  JMP");
                    else if ((rop & 0xFFC0) == 0x4E80) fprintf(stderr, "  JSR");
                    else if ((rop & 0xFF00) == 0x6000) fprintf(stderr, "  BRA");
                    else if ((rop & 0xFF00) == 0x6100) fprintf(stderr, "  BSR");
                    else if ((rop & 0xF0F8) == 0x50C8) fprintf(stderr, "  DBcc");
                    else if ((rop & 0xF000) == 0xA000) fprintf(stderr, "  Line-A ($%04X)", rop);
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "    Registers:\n");
                fprintf(stderr, "      D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
                fprintf(stderr, "      D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                        cpu->d[4], cpu->d[5], cpu->d[6], cpu->d[7]);
                fprintf(stderr, "      A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n",
                        cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3]);
                fprintf(stderr, "      A4=$%08X A5=$%08X A6=$%08X A7=$%08X\n",
                        cpu->a[4], cpu->a[5], cpu->a[6], cpu->a[7]);
                fprintf(stderr, "      SR=$%04X\n", cpu->sr);
                fprintf(stderr, "    Stack (16 words from SP):\n      ");
                for (int si = 0; si < 16; si++) {
                    fprintf(stderr, " $%04X", cpu_read16(cpu, (cpu->a[7] + si*2) & 0xFFFFFF));
                    if (si == 7) fprintf(stderr, "\n      ");
                }
                fprintf(stderr, "\n");
                /* Also show what the Line-A vector ($28) currently points to */
                uint32_t line_a_handler = cpu_read32(cpu, 0x28);
                uint32_t illegal_handler = cpu_read32(cpu, 0x10);
                fprintf(stderr, "    Vector table: Line-A($28)=$%08X  Illegal($10)=$%08X\n",
                        line_a_handler, illegal_handler);
                fprintf(stderr, "    Memory at $28: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        cpu_read8(cpu, 0x28), cpu_read8(cpu, 0x29),
                        cpu_read8(cpu, 0x2A), cpu_read8(cpu, 0x2B),
                        cpu_read8(cpu, 0x2C), cpu_read8(cpu, 0x2D),
                        cpu_read8(cpu, 0x2E), cpu_read8(cpu, 0x2F));
            }
        }
        /* Detect PC in suspicious low RAM ($400..$5000) — above vector table,
         * below the normal Pascal-code start. PC=$002600 fired during boot and
         * funneled into hard_excep → SYSTEM_ERROR(10201). Dump ring, regs,
         * stack to find the JSR/JMP that landed us here. */
        {
            DBGSTATIC(int, lowpc_trace_count, 0);
            uint32_t masked = cpu->pc & 0xFFFFFF;
            if (masked == 0x002600 && lowpc_trace_count < 3) {
                lowpc_trace_count++;
                fprintf(stderr, "\n!!! PC IN LOW RAM (#%d): PC=$%06X op=$%04X SP=$%08X SR=$%04X\n",
                        lowpc_trace_count, cpu->pc, cpu_read16(cpu, cpu->pc),
                        cpu->a[7], cpu->sr);
                fprintf(stderr, "    Last 30 PCs (oldest→newest):\n");
                for (int ri = 30; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    uint16_t rop = cpu_read16(cpu, rpc);
                    fprintf(stderr, "      PC=$%06X op=$%04X", rpc, rop);
                    if (rop == 0x4E75) fprintf(stderr, "  RTS");
                    else if (rop == 0x4E73) fprintf(stderr, "  RTE");
                    else if ((rop & 0xFFC0) == 0x4EC0) fprintf(stderr, "  JMP");
                    else if ((rop & 0xFFC0) == 0x4E80) fprintf(stderr, "  JSR");
                    else if ((rop & 0xFF00) == 0x6000) fprintf(stderr, "  BRA");
                    else if ((rop & 0xFF00) == 0x6100) fprintf(stderr, "  BSR");
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "    Regs: D0=$%08X D1=$%08X A0=$%08X A1=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->a[0], cpu->a[1]);
                fprintf(stderr, "          A5=$%08X A6=$%08X A7=$%08X\n",
                        cpu->a[5], cpu->a[6], cpu->a[7]);
                fprintf(stderr, "    Stack (8 longs from SP):");
                for (int si = 0; si < 8; si++)
                    fprintf(stderr, " $%08X", cpu_read32(cpu, (cpu->a[7] + si*4) & 0xFFFFFF));
                fprintf(stderr, "\n");
                fprintf(stderr, "    Bytes @$25F0..$2620:");
                for (uint32_t a = 0x25F0; a < 0x2620; a += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, a));
                fprintf(stderr, "\n");
                fprintf(stderr, "    Last 8 JSR/BSR/JMP(An) (oldest→newest):\n");
                for (int ji = 0; ji < 8; ji++) {
                    int slot = (last_jsr_idx + ji) & 7;
                    if (last_jsr_pc[slot])
                        fprintf(stderr, "      call@$%06X → target=$%08X\n",
                                last_jsr_pc[slot], last_jsr_target[slot]);
                }
            }
        }
        /* Detect PC in unmapped RAM ($200000-$FBFFFF) — past 2MB, before I/O */
        {
            DBGSTATIC(int, unmapped_trace_count, 0);
            uint32_t masked = cpu->pc & 0xFFFFFF;
            if (masked >= 0x200000 && masked < 0xFC0000 && unmapped_trace_count < 1) {
                unmapped_trace_count++;
                fprintf(stderr, "\n!!! PC IN UNMAPPED RAM: PC=$%06X op=$%04X\n", cpu->pc, cpu_read16(cpu, cpu->pc));
                fprintf(stderr, "    Last 30 PCs (oldest first):\n");
                for (int ri = 30; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    uint16_t rop = cpu_read16(cpu, rpc);
                    fprintf(stderr, "      PC=$%06X op=$%04X", rpc, rop);
                    /* Decode common branch/jump opcodes */
                    if ((rop & 0xFFC0) == 0x4EC0) fprintf(stderr, "  JMP");
                    else if ((rop & 0xFFC0) == 0x4E80) fprintf(stderr, "  JSR");
                    else if (rop == 0x4E75) fprintf(stderr, "  RTS");
                    else if (rop == 0x4E73) fprintf(stderr, "  RTE");
                    else if ((rop & 0xFF00) == 0x6000) fprintf(stderr, "  BRA");
                    else if ((rop & 0xF0F8) == 0x50C8) fprintf(stderr, "  DBcc");
                    fprintf(stderr, "\n");
                }
                fprintf(stderr, "    Registers:\n");
                fprintf(stderr, "      D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
                fprintf(stderr, "      D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                        cpu->d[4], cpu->d[5], cpu->d[6], cpu->d[7]);
                fprintf(stderr, "      A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n",
                        cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3]);
                fprintf(stderr, "      A4=$%08X A5=$%08X A6=$%08X A7=$%08X\n",
                        cpu->a[4], cpu->a[5], cpu->a[6], cpu->a[7]);
                fprintf(stderr, "      SR=$%04X\n", cpu->sr);
                fprintf(stderr, "    Stack top 8 words:\n      ");
                for (int si = 0; si < 8; si++)
                    fprintf(stderr, " $%04X", cpu_read16(cpu, (cpu->a[7] + si*2) & 0xFFFFFF));
                fprintf(stderr, "\n");
            }
        }
        /* Detect when PC returns to $4000-$5000 (INITSYS/main body) after PASCALINIT */
        {
            DBGSTATIC(int, pi_state, 0);  /* 0=before, 1=in PASCALINIT, 2=left */
            DBGSTATIC(int, low_trace, 0);
            if (pi_state == 0 && cpu->pc >= 0xDF000 && cpu->pc < 0xE0000) pi_state = 1;
            if (pi_state == 1 && cpu->pc < 0xDF000) pi_state = 2;
            if (pi_state == 2 && cpu->pc >= 0x4000 && cpu->pc < 0x5000 && low_trace < 5) {
                fprintf(stderr, ">>> BACK LOW: PC=$%06X op=$%04X A5=$%08X SP=$%08X\n",
                        cpu->pc, cpu_read16(cpu, cpu->pc), cpu->a[5], cpu->a[7]);
                low_trace++;
            }
        }
        /* Track large SP changes */
        {
            DBGSTATIC(uint32_t, prev_sp, 0);
            DBGSTATIC(int, sp_trace, 0);
            if (prev_sp > 0 && sp_trace < 3) {
                int32_t delta = (int32_t)cpu->a[7] - (int32_t)prev_sp;
                if (prev_sp > 0x1000 && cpu->a[7] < 0x1000 && cpu->a[7] > 0) {
                    fprintf(stderr, ">>> SP JUMP: $%08X → $%08X (delta=%d) at PC=$%06X\n",
                            prev_sp, cpu->a[7], delta, cpu->pc);
                    sp_trace++;
                }
            }
            prev_sp = cpu->a[7];
        }
        /* Track when SP hits $78FBC (after %initstdio MOVEM push) going UP */
        {
            DBGSTATIC(uint32_t, prev_sp2, 0);
            DBGSTATIC(int, sp_up, 0);
            if (sp_up < 3 && prev_sp2 < 0x78FBC && cpu->a[7] >= 0x78FBC && cpu->a[7] <= 0x78FFC) {
                fprintf(stderr, ">>> SP UP TO $%08X at PC=$%06X (MOVEM restore?)\n",
                        cpu->a[7], cpu->pc);
                sp_up++;
            }
            prev_sp2 = cpu->a[7];
        }
        /* Track A5 corruption */
        {
            DBGSTATIC(uint32_t, prev_a5_corrupt, 0);
            DBGSTATIC(int, a5_trace, 0);
            if (a5_trace < 5 && prev_a5_corrupt != cpu->a[5] && prev_a5_corrupt != 0) {
                if (cpu->a[5] > 0x100000 && prev_a5_corrupt < 0x100000) {
                    fprintf(stderr, ">>> A5 CORRUPT: $%08X → $%08X at PC=$%06X\n",
                            prev_a5_corrupt, cpu->a[5], cpu->pc);
                    a5_trace++;
                }
            }
            prev_a5_corrupt = cpu->a[5];
        }
        /* Trace wait loops */
        if ((cpu->pc & 0xFFFFFF) == 0xCB012) {
            DBGSTATIC(int, wait_trace, 0);
            if (wait_trace < 3) {
                fprintf(stderr, "WAIT@$CB012 #%d: D0=$%08X D7=$%08X SR=$%04X A0=$%08X A4=$%08X\n",
                        ++wait_trace, cpu->d[0], cpu->d[7], cpu->sr, cpu->a[0], cpu->a[4]);
                /* Dump code context */
                fprintf(stderr, "  Code:");
                for (int i = -8; i < 24; i += 2)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xCB012 + i));
                fprintf(stderr, "\n");
            }
        }
        /* Dump FP code at NEWFPSUB when loop is first entered */
        if (cpu->pc == 0xC11CE) {
            DBGSTATIC(int, fp_trace, 0);
            if (fp_trace++ < 1) {
                fprintf(stderr, "=== NEWFPSUB entry at $C11CE ===\n");
                fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
                fprintf(stderr, "  D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                        cpu->d[4], cpu->d[5], cpu->d[6], cpu->d[7]);
                fprintf(stderr, "  A0=$%08X A1=$%08X A2=$%08X A3=$%08X A5=$%08X A6=$%08X SP=$%08X\n",
                        cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3], cpu->a[5], cpu->a[6], cpu->a[7]);
                /* Dump code from $C11CE to $C1240 */
                fprintf(stderr, "  Code $C11C0-$C1240:");
                for (int i = 0; i < 128; i += 2) {
                    if (i % 32 == 0) fprintf(stderr, "\n    $%06X:", 0xC11C0 + i);
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0xC11C0 + i));
                }
                fprintf(stderr, "\n");
            }
        }
        /* Trace CALLDRIVER loop — find who called it */
        if (cpu->pc == 0xC120E) {
            DBGSTATIC(int, calldriver_count, 0);
            if (calldriver_count < 1) {
                calldriver_count++;
                uint32_t a3 = cpu->a[3];
                fprintf(stderr, "=== CALLDRIVER at PC=$%06X A3=$%08X A6=$%08X SP=$%08X\n",
                        cpu->pc, a3, cpu->a[6], cpu->a[7]);
                /* Walk the A6 frame chain to find callers */
                fprintf(stderr, "  Frame chain:\n");
                uint32_t frame = cpu->a[6];
                for (int depth = 0; depth < 10 && frame > 0x1000 && frame < 0x200000; depth++) {
                    uint32_t saved_a6 = cpu_read32(cpu, frame & 0xFFFFFF);
                    uint32_t ret_addr = cpu_read32(cpu, (frame + 4) & 0xFFFFFF);
                    fprintf(stderr, "    [%d] A6=$%08X ret=$%08X\n", depth, frame, ret_addr);
                    frame = saved_a6;
                }
                fprintf(stderr, "  Last 40 PCs:");
                for (int ri = 40; ri > 0; ri--)
                    fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                fprintf(stderr, "\n");
                /* Params record at A3: control(w), completion1(l), fnctn_code(w?), completion2(l) */
                fprintf(stderr, "  Params[0..19]:");
                for (int pi = 0; pi < 20; pi += 2) {
                    fprintf(stderr, " %04X", cpu_read16(cpu, a3 + pi));
                }
                fprintf(stderr, "\n");
                uint16_t control = cpu_read16(cpu, a3 + 0);
                uint32_t comp1 = cpu_read32(cpu, a3 + 2);
                uint16_t fnctn_code = cpu_read16(cpu, a3 + 4);
                uint32_t comp2 = cpu_read32(cpu, a3 + 6);
                fprintf(stderr, "  control=$%04X comp1=$%08X fnctn_code=$%04X comp2=$%08X\n",
                        control, comp1, fnctn_code, comp2);
                /* Registers at entry */
                fprintf(stderr, "  D0=$%08X D4=$%08X D5=$%08X D7=$%08X\n",
                        cpu->d[0], cpu->d[4], cpu->d[5], cpu->d[7]);
                fprintf(stderr, "  A0=$%08X A1=$%08X A2=$%08X A4=$%08X A5=$%08X\n",
                        cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[4], cpu->a[5]);
                /* Device config pointer from stack frame: A6+8 */
                uint32_t config_ptr = cpu_read32(cpu, cpu->a[6] + 8);
                fprintf(stderr, "  config_ptr(A6+8)=$%08X\n", config_ptr);
                /* Read first 16 bytes of config record if accessible */
                if (config_ptr > 0 && config_ptr < 0xFC0000) {
                    fprintf(stderr, "  config[0..15]:");
                    for (int ci = 0; ci < 16; ci += 2) {
                        fprintf(stderr, " %04X", cpu_read16(cpu, config_ptr + ci));
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        /* Detect SYSTEM_ERROR calls */
        if (cpu->pc == 0xD8FAC) {
            DBGSTATIC(int, syserr_count, 0);
            if (syserr_count++ < 5) {
                /* Error number is on the stack as parameter */
                uint16_t errnum = cpu_read16(cpu, cpu->a[7] + 4);
                fprintf(stderr, "=== SYSTEM_ERROR(%d) called from $%06X\n",
                        (int16_t)errnum,
                        cpu_read32(cpu, cpu->a[7]) - 6);
            }
        }
        /* Log calls to stub ($3F0) — shows which functions are missing */
        if (cpu->pc == 0x3F0) {
            DBGSTATIC(int, stub_call_count, 0);
            if (stub_call_count < 20) {
                uint32_t caller = cpu_read32(cpu, cpu->a[7]);
                fprintf(stderr, "STUB CALL from $%06X (call #%d)\n", caller - 6, ++stub_call_count);
            }
        }
        /* Trace first escape past loaded OS code ($6B000) */
        {
            DBGSTATIC(int, code_escape_count, 0);
            uint32_t masked_pc = cpu->pc & 0xFFFFFF;
            if (code_escape_count < 3 &&
                ((masked_pc >= 0x6B000 && masked_pc < 0xFC0000) || cpu->pc > 0xFFFFFF)) {
                code_escape_count++;
                fprintf(stderr, "=== CODE ESCAPE: PC=$%06X SP=$%08X A6=$%08X\n",
                        cpu->pc, cpu->a[7], cpu->a[6]);
                fprintf(stderr, "  A0=$%08X A1=$%08X D0=$%08X D1=$%08X\n",
                        cpu->a[0], cpu->a[1], cpu->d[0], cpu->d[1]);
                fprintf(stderr, "  Last 10 PCs:\n");
                for (int ri = 10; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    uint16_t rop = cpu_read16(cpu, rpc);
                    fprintf(stderr, "    PC=$%06X op=$%04X", rpc, rop);
                    /* Decode JSR/JMP targets */
                    if (rop == 0x4EB9 || rop == 0x4EF9) {
                        uint32_t tgt = cpu_read32(cpu, rpc + 2);
                        fprintf(stderr, " → $%06X", tgt);
                    }
                    /* Decode MOVEA.L (abs),An */
                    if ((rop & 0xF1FF) == 0x2079) {
                        uint32_t addr = cpu_read32(cpu, rpc + 2);
                        uint32_t val = cpu_read32(cpu, addr);
                        fprintf(stderr, " MOVEA.L ($%06X),A%d [=$%08X]", addr, (rop >> 9) & 7, val);
                    }
                    /* Decode JMP d16(A0) */
                    if ((rop & 0xFFF8) == 0x4EE8) {
                        int16_t disp = (int16_t)cpu_read16(cpu, rpc + 2);
                        fprintf(stderr, " JMP %d(A%d) [A%d=$%08X → $%06X]",
                                disp, rop & 7, rop & 7, cpu->a[rop & 7],
                                cpu->a[rop & 7] + disp);
                    }
                    fprintf(stderr, "\n");
                }
                /* keep counting */
            }
        }




        /* Log TRAP exceptions with call context */
        DBGSTATIC(int, trap_detail_count, 0);
        if (trap_detail_count < 10) {
            uint16_t opword2 = cpu_read16(cpu, cpu->pc);
            if ((opword2 & 0xFFF0) == 0x4E40) {
                int vec = opword2 & 0xF;
                uint32_t vec_addr = (VEC_TRAP_BASE + vec) * 4;
                uint32_t handler = cpu_read32(cpu, vec_addr);
                fprintf(stderr, "TRAP #%d at PC=$%06X: handler=$%08X D0=$%08X A0=$%08X A6=$%08X SP=$%08X\n",
                        vec, cpu->pc, handler, cpu->d[0], cpu->a[0], cpu->a[6], cpu->a[7]);
                /* Show last 10 PCs before TRAP */
                if (trap_detail_count < 3) {
                    fprintf(stderr, "  Call context (last 10 PCs):\n");
                    for (int ri = 10; ri > 0; ri--) {
                        uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                        fprintf(stderr, "    PC=$%06X op=$%04X\n", rpc, cpu_read16(cpu, rpc));
                    }
                }
                (void)handler;
                trap_detail_count++;
            }
        }

        /* Trace PASCALINIT entry and critical values */
        DBGSTATIC(int, pascalinit_logged, 0);
        {
            /* Detect PASCALINIT by its first instruction: MOVE.L $218.L,A2 (opcode $2479) */
            uint16_t pi_op = cpu_read16(cpu, cpu->pc);
            if (!pascalinit_logged && pi_op == 0x2479 && cpu->pc >= 0x50000) {
                uint32_t addr = cpu_read32(cpu, cpu->pc + 2);
                if (addr == 0x218) {
                    uint32_t paramptr = cpu_read32(cpu, 0x218);
                    uint32_t version = cpu_read16(cpu, paramptr);
                    uint32_t esysg_addr = paramptr - 28;
                    uint32_t esysg_val = cpu_read32(cpu, esysg_addr);
                    fprintf(stderr, ">>> PASCALINIT: PC=$%06X A5=$%08X SP=$%08X\n", cpu->pc, cpu->a[5], cpu->a[7]);
                    fprintf(stderr, "  adrparamptr($218)=$%08X version=%d\n", paramptr, version);
                    fprintf(stderr, "  esysglobal @$%X = $%08X (end of globals)\n", esysg_addr, esysg_val);
                    fprintf(stderr, "  A5-32 data: ");
                    for (int i = 0; i < 32; i += 4)
                        fprintf(stderr, "$%08X ", cpu_read32(cpu, cpu->a[5] - 32 + i));
                    fprintf(stderr, "\n");
                    pascalinit_logged = 1;
                }
            }
        }

        /* Detect odd PC — should never happen on 68000 */
        DBGSTATIC(int, odd_pc_logged, 0);
        if (!odd_pc_logged && (cpu->pc & 1) && cpu->pc >= 0x400 && cpu->pc < 0x100000) {
            fprintf(stderr, ">>> ODD PC: $%06X! Last 30 PCs:\n", cpu->pc);
            for (int ri = 30; ri > 0; ri--) {
                uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                fprintf(stderr, "    PC=$%06X op=$%04X\n", rpc, cpu_read16(cpu, rpc));
            }
            fprintf(stderr, "  D0=$%08X A0=$%08X A5=$%08X A6=$%08X SP=$%08X SR=$%04X\n",
                    cpu->d[0], cpu->a[0], cpu->a[5], cpu->a[6], cpu->a[7], cpu->sr);
            odd_pc_logged = 1;
        }

        /* Trace entry to STARTUP at $400 */
        {
            DBGSTATIC(int, startup_entry_logged, 0);
            if (!startup_entry_logged && cpu->pc == 0x400) {
                fprintf(stderr, ">>> STARTUP entry: A5=$%08X A6=$%08X A7=$%08X SSP=$%08X USP=$%08X SR=$%04X\n",
                        cpu->a[5], cpu->a[6], cpu->a[7], cpu->ssp, cpu->usp, cpu->sr);
                startup_entry_logged = 1;
            }
        }
        /* Trace GETLDMAP and parameter block */
        DBGSTATIC(int, getldmap_logged, 0);
        if (!getldmap_logged && cpu->pc == 0x45A) {
            /* At entry (before LINK): SP → return addr, SP+4 → ldmapbase */
            uint32_t ldm = cpu_read32(cpu, cpu->a[7] + 4);
            fprintf(stderr, ">>> GETLDMAP: ldmapbase=$%08X version=%d A6=$%08X\n",
                    ldm, cpu_read16(cpu, ldm), cpu->a[6]);
            /* Dump parameter block: 20 longs downward from ldmapbase */
            fprintf(stderr, "  Param block (version then 20 fields):\n");
            fprintf(stderr, "    version=$%04X\n", cpu_read16(cpu, ldm));
            for (int i = 0; i < 20; i++) {
                uint32_t addr = ldm - 4 - i * 4;
                fprintf(stderr, "    [%d] @$%X = $%08X\n", i, addr, cpu_read32(cpu, addr));
            }
            /* INITSYS parent frame: A6 at entry = INITSYS frame */
            uint32_t initsys_a6 = cpu->a[6];
            fprintf(stderr, "  INITSYS locals (A6=$%08X): @-102=$%04X @-104=$%08X @-108=$%08X\n",
                    initsys_a6,
                    cpu_read16(cpu, initsys_a6 - 102),
                    cpu_read32(cpu, initsys_a6 - 104),
                    cpu_read32(cpu, initsys_a6 - 108));
            getldmap_logged = 1;
        }
        /* Trace REG_TO_MAPPED entry — locals should be filled by GETLDMAP */
        DBGSTATIC(int, r2m_logged, 0);
        if (!r2m_logged && cpu->pc == 0xD1B64) {
            r2m_logged = 1;
            /* At REG_TO_MAPPED entry: SP → return addr, SP+4 → e_us, SP+8 → b_sg */
            /* But more importantly, check INITSYS locals via A6 */
            uint32_t a6 = cpu->a[6]; /* INITSYS frame pointer */
            fprintf(stderr, ">>> REG_TO_MAPPED entry: A6=$%08X A5=$%08X SP=$%08X\n",
                    a6, cpu->a[5], cpu->a[7]);
            fprintf(stderr, "  Params: b_sg=$%08X e_us=$%08X\n",
                    cpu_read32(cpu, cpu->a[7] + 4),
                    cpu_read32(cpu, cpu->a[7] + 8));
            /* Check INITSYS locals after GETLDMAP should have filled them */
            /* version at A6-102, b_sysjt at A6-106, l_sysjt at A6-110 */
            /* b_sys_global at A6-114, l_sys_global at A6-118 */
            fprintf(stderr, "  INITSYS locals: version=%d b_sysjt=$%08X l_sysjt=$%08X\n",
                    cpu_read16(cpu, a6 - 102),
                    cpu_read32(cpu, a6 - 106),
                    cpu_read32(cpu, a6 - 110));
            fprintf(stderr, "  b_sys_global=$%08X l_sys_global=$%08X\n",
                    cpu_read32(cpu, a6 - 114),
                    cpu_read32(cpu, a6 - 118));
            fprintf(stderr, "  b_sgheap=$%08X l_sgheap=$%08X\n",
                    cpu_read32(cpu, a6 - 138),
                    cpu_read32(cpu, a6 - 142));
        }

        /* Trace SYSTEM_ERROR call */
        DBGSTATIC(int, syserr_logged, 0);
        if (!syserr_logged && cpu->pc >= 0x4AF00 && cpu->pc <= 0x4AF20) {
            uint16_t op = cpu_read16(cpu, cpu->pc);
            if (op == 0x2079) {
                uint16_t errnum = cpu_read16(cpu, cpu->a[7] + 4);
                fprintf(stderr, ">>> SYSTEM_ERROR(%d) at PC=$%06X SP=$%08X\n",
                        (int16_t)errnum, cpu->pc, cpu->a[7]);
                fprintf(stderr, "  D0=$%08X D3=$%08X A5=$%08X A6=$%08X\n",
                        cpu->d[0], cpu->d[3], cpu->a[5], cpu->a[6]);
                /* Unwind stack to find call chain */
                uint32_t sp = cpu->a[7];
                fprintf(stderr, "  Stack: ");
                for (int i = 0; i < 20; i += 4)
                    fprintf(stderr, "$%08X ", cpu_read32(cpu, sp + i));
                fprintf(stderr, "\n");
                /* Show 30 PCs */
                fprintf(stderr, "  Last 30 PCs:\n");
                for (int ri = 30; ri > 0; ri--) {
                    uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                    fprintf(stderr, "    PC=$%06X op=$%04X\n", rpc, cpu_read16(cpu, rpc));
                }
                syserr_logged = 1;
            }
        }

        /* Trace when A7 goes to 0 or near 0 */
        DBGSTATIC(int, a7_zero_logged, 0);
        if (!a7_zero_logged && cpu->a[7] < 0x100 && cpu->pc >= 0x400) {
            fprintf(stderr, ">>> A7 ZEROED: PC=$%06X A7=$%08X\n", cpu->pc, cpu->a[7]);
            fprintf(stderr, "  Last 30 PCs:\n");
            for (int ri = 30; ri > 0; ri--) {
                uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                fprintf(stderr, "    PC=$%06X op=$%04X\n", rpc, cpu_read16(cpu, rpc));
            }
            fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                    cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
            fprintf(stderr, "  A0=$%08X A1=$%08X A2=$%08X A3=$%08X A4=$%08X A5=$%08X A6=$%08X\n",
                    cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3], cpu->a[4], cpu->a[5], cpu->a[6]);
            a7_zero_logged = 1;
        }

        /* Track highest PC reached in OS code and detect escape from kernel */
        DBGSTATIC(uint32_t, max_os_pc, 0);
        DBGSTATIC(int, escape_logged, 0);
        if (cpu->pc >= 0x400 && cpu->pc < 0x100000 && cpu->pc > max_os_pc)
            max_os_pc = cpu->pc;
        /* Track transition: last PC in valid kernel code, first PC past it */
        DBGSTATIC(uint32_t, last_valid_pc, 0);
        DBGSTATIC(uint16_t, last_valid_op, 0);
        if (cpu->pc >= 0x400 && cpu->pc < 0x53000) {
            last_valid_pc = cpu->pc;
            last_valid_op = cpu_read16(cpu, cpu->pc);
        }
        /* Detect when PC leaves kernel binary (end ~$53000, includes PASCALINIT etc.) */
        if (!escape_logged && cpu->pc >= 0x53000 && cpu->pc < 0x200000) {
            fprintf(stderr, ">>> KERNEL ESCAPE: PC=$%06X (past kernel end ~$52000)\n", cpu->pc);
            fprintf(stderr, "  Last valid PC: $%06X opcode=$%04X\n", last_valid_pc, last_valid_op);
            fprintf(stderr, "  Last 20 PCs:\n");
            for (int ri = 20; ri > 0; ri--) {
                uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 255];
                uint16_t rop = cpu_read16(cpu, rpc);
                fprintf(stderr, "  PC=$%06X opcode=$%04X%s\n", rpc, rop,
                        (rpc >= 0x52000) ? " <<<" : "");
            }
            fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                    cpu->d[0], cpu->d[1], cpu->d[2], cpu->d[3]);
            fprintf(stderr, "  A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n",
                    cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[3]);
            fprintf(stderr, "  A4=$%08X A5=$%08X A6=$%08X SP=$%08X SR=$%04X\n",
                    cpu->a[4], cpu->a[5], cpu->a[6], cpu->a[7], cpu->sr);
            escape_logged = 1;
        }

        /* Log when PC enters vector table after being in OS code */
        DBGSTATIC(uint32_t, prev_pc, 0);
        DBGSTATIC(int, crash_logged, 0);
        if (!crash_logged && cpu->pc < 0x400 && prev_pc >= 0x400 && prev_pc < 0x200000) {
            fprintf(stderr, "CRASH TO VECTORS: prev_pc=$%06X → pc=$%06X opcode=$%04X A7=$%08X\n",
                    prev_pc, cpu->pc, cpu_read16(cpu, prev_pc), cpu->a[7]);
            crash_logged = 1;
        }
        if (cpu->pc >= 0x400) prev_pc = cpu->pc;

        /* HLE intercept check — before executing the instruction,
         * see if this PC should be handled by high-level emulation. */
        if (cpu->hle_check && cpu->hle_check(cpu->hle_ctx, cpu)) {
            /* HLE handler set cpu->pc and cpu->cycles directly */
            if (cpu->cycles == 0) cpu->cycles = 4;
            cpu->total_cycles += cpu->cycles;
            continue;
        }

        /* (Post-INTSON trace removed — enable for debugging) */

        /* Trace crash function: dump opcodes and track A6 */
        {
            DBGSTATIC(int, crash_fn_dumped, 0);
            /* Trace FPMODES function at $022892 */
            if (cpu->pc == 0x022892 && !crash_fn_dumped) {
                fprintf(stderr, "\n>>> ENTERING FPMODES fn at $022892, A6=$%08X SP=$%08X\n",
                        cpu->a[6], cpu->a[7]);
                fprintf(stderr, "    Code at $022892:");
                for (int di = 0; di < 24; di++)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0x022892 + di * 2));
                fprintf(stderr, "\n");
                fprintf(stderr, "    Stack:");
                for (int si = 0; si < 8; si++)
                    fprintf(stderr, " %08X", cpu_read32(cpu, (cpu->a[7] + si * 4) & 0xFFFFFF));
                fprintf(stderr, "\n");
            }
            if (cpu->pc == 0x03D53E && !crash_fn_dumped) {
                crash_fn_dumped = 1;
                fprintf(stderr, "\n>>> ENTERING CRASH FN at $03D53E, A6=$%08X SP=$%08X\n", cpu->a[6], cpu->a[7]);
                fprintf(stderr, "    Code BEFORE $03D53E:");
                for (int di = -16; di < 0; di++)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0x03D53E + di * 2));
                fprintf(stderr, "\n");
                fprintf(stderr, "    Code FROM  $03D53E:");
                for (int di = 0; di < 24; di++)
                    fprintf(stderr, " %04X", cpu_read16(cpu, 0x03D53E + di * 2));
                fprintf(stderr, "\n");
                /* Stack dump */
                fprintf(stderr, "    Return addr on stack: $%08X\n", cpu_read32(cpu, cpu->a[7]));
                fprintf(stderr, "    Stack:");
                for (int si = 0; si < 12; si++)
                    fprintf(stderr, " %08X", cpu_read32(cpu, (cpu->a[7] + si * 4) & 0xFFFFFF));
                fprintf(stderr, "\n");
                fprintf(stderr, "    Regs: D0=%08X D1=%08X D2=%08X A0=%08X A1=%08X A2=%08X\n",
                        cpu->d[0], cpu->d[1], cpu->d[2], cpu->a[0], cpu->a[1], cpu->a[2]);
            }
            /* Trace when A6 becomes $FFFFFFDE (garbage) */
            {
                DBGSTATIC(uint32_t, prev_a6, 0);
                DBGSTATIC(int, a6_corrupt_log, 0);
                if (cpu->a[6] == 0xFFFFFFDE && prev_a6 != 0xFFFFFFDE && a6_corrupt_log < 5) {
                    a6_corrupt_log++;
                    fprintf(stderr, "\n!!! A6 CORRUPTED: $%08X → $FFFFFFDE at PC=$%06X op=$%04X\n",
                            prev_a6, cpu->pc, cpu_read16(cpu, cpu->pc));
                    fprintf(stderr, "    SP=$%08X A5=$%08X D0=$%08X\n",
                            cpu->a[7], cpu->a[5], cpu->d[0]);
                    fprintf(stderr, "    Last 30 PCs:");
                    for (int ri = 30; ri > 0; ri--)
                        fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                    fprintf(stderr, "\n");
                    fprintf(stderr, "    Stack:");
                    for (int si = 0; si < 8; si++)
                        fprintf(stderr, " %08X", cpu_read32(cpu, (cpu->a[7] + si * 4) & 0xFFFFFF));
                    fprintf(stderr, "\n");
                }
                prev_a6 = cpu->a[6];
            }
        }

        cpu->cycles = 0;
        uint32_t sp_before = cpu->a[7];
        uint32_t pc_before = cpu->pc;

        /* HLE: Lisabug auto-entry bypass — execution-time.
         *
         * See src/lisa_mmu.c $234 write-intercept comment for context.
         * The write-time intercept doesn't always catch INIT_NMI_TRAPV
         * (depends on whether it runs through lisa_mem_write16), so we
         * belt-and-braces it here: any time PC reaches $234 (target of
         * `jmp enter_macsbug` from source-NMIHANDLER.TEXT lisabugentry),
         * synthesize an RTE directly. The supervisor stack has SR+PC
         * pushed as a fake level-7 exception frame by lisabugentry
         * ("emulate a level 7 interrupt to get there" — NMIHANDLER:311)
         * so RTE cleanly returns to the Pascal caller of MACSBUG.
         *
         * GATE: only fire if the stacked SR has IPL==7 — that's the
         * signature of the DB_INIT synthetic level-7 frame. The
         * `hard_excep` path also routes through $234 when dropping
         * into Lisabug on a real system-code exception, but its
         * stacked SR carries the faulting code's IPL (typically 0),
         * and its frame is a real exception frame that Lisabug is
         * supposed to parse. Popping it via RTE corrupts the
         * supervisor stack → A7=0 cascade. Let Lisa OS's real
         * Lisabug run for those.
         *
         * Scoped to prebuilt Workshop images; source-compiled boots
         * won't link SYSTEM.DEBUG and won't hit this path. */
        if ((cpu->pc & 0xFFFFFF) == 0x234) {
            uint16_t stacked_sr = cpu_read16(cpu, cpu->a[7] & 0xFFFFFF);
            uint32_t stacked_pc = cpu_read32(cpu, (cpu->a[7] + 2) & 0xFFFFFF);
            int ipl = (stacked_sr >> 8) & 7;
            DBGSTATIC(int, hle_seen, 0);
            if (hle_seen++ < 8) {
                fprintf(stderr, "[HLE] $234 entry #%d: stacked SR=$%04X "
                        "(IPL=%d) PC=$%06X SSP=$%08X → %s\n",
                        hle_seen, stacked_sr, ipl, stacked_pc, cpu->a[7],
                        ipl == 7 ? "RTE (DB_INIT)" : "EXECUTE (hard_excep)");
            }
            if (ipl == 7) {
                op_rte(cpu);
                cpu->cycles = 20;
                cpu->total_cycles += cpu->cycles;
                continue;
            }
            /* else: fall through and let real Lisabug code at $234 run */
        }

        execute_one(cpu);
        if (cpu->cycles == 0) cpu->cycles = 4; /* minimum */
        cpu->total_cycles += cpu->cycles;

        /* SP delta trace: catch single-instruction stack corruption */
        {
            uint32_t sp_after = cpu->a[7];
            int32_t delta = (int32_t)sp_after - (int32_t)sp_before;
            if (delta > 0x1000 || delta < -0x1000) {
                DBGSTATIC(int, sp_delta_log, 0);
                if (sp_delta_log++ < 10) {
                    fprintf(stderr, "\n!!! SP DELTA: $%08X → $%08X (delta=%+d) at PC=$%06X op=$%04X\n",
                            sp_before, sp_after, delta, pc_before, cpu_read16(cpu, pc_before));
                    fprintf(stderr, "    A6=$%08X A5=$%08X A0=$%08X D0=$%08X SR=$%04X\n",
                            cpu->a[6], cpu->a[5], cpu->a[0], cpu->d[0], cpu->sr);
                    /* Decode what the instruction did */
                    uint16_t op_at = cpu_read16(cpu, pc_before);
                    if ((op_at & 0xFFF8) == 0x4E58) {
                        /* UNLK An */
                        int reg = op_at & 7;
                        fprintf(stderr, "    UNLK A%d — A%d was $%08X, loaded from [A%d]=$%08X\n",
                                reg, 7, sp_before, reg, cpu->a[reg]);
                        fprintf(stderr, "    Frame pointer (A%d) was pointing to: $%08X\n",
                                reg, sp_before);
                    } else {
                        fprintf(stderr, "    Instruction at PC=$%06X: $%04X\n",
                                pc_before, op_at);
                    }
                    /* Dump stack around the corruption */
                    fprintf(stderr, "    Stack at SP_before=$%08X:", sp_before);
                    for (int si = 0; si < 8; si++)
                        fprintf(stderr, " %08X", cpu_read32(cpu, (sp_before + si * 4) & 0xFFFFFF));
                    fprintf(stderr, "\n");
                    /* Last 20 PCs */
                    fprintf(stderr, "    Last 20 PCs:");
                    for (int ri = 20; ri > 0; ri--)
                        fprintf(stderr, " $%06X", pc_ring[(pc_ring_idx - ri) & 255]);
                    fprintf(stderr, "\n");
                }
            }
        }

        /* Trace exception */
        if (cpu->sr & SR_TRACE) {
            take_exception(cpu, VEC_TRACE);
            cpu->total_cycles += cpu->cycles;
        }
    }

    return cpu->total_cycles - start_cycles;
}

void m68k_set_irq(m68k_t *cpu, int level) {
    cpu->pending_irq = level & 7;
}

void m68k_pulse_reset(m68k_t *cpu) {
    m68k_reset(cpu);
}

uint32_t m68k_get_pc(m68k_t *cpu) { return cpu->pc; }
uint32_t m68k_get_reg(m68k_t *cpu, int reg) {
    if (reg < 8) return cpu->d[reg];
    if (reg < 16) return cpu->a[reg - 8];
    return 0;
}
uint16_t m68k_get_sr(m68k_t *cpu) { return cpu->sr; }
int g_trap6_total = 0;
