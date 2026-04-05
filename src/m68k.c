/*
 * LisaEm - Apple Lisa Emulator
 * Motorola 68000 CPU Emulator
 *
 * Full implementation of the Motorola 68000 instruction set.
 * Reference: M68000 Programmer's Reference Manual (Motorola, 1992)
 */

#include "m68k.h"
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

static inline void cpu_write8(m68k_t *cpu, uint32_t addr, uint8_t val) {
    cpu->write8(mask_24(addr), val);
}

static inline void cpu_write16(m68k_t *cpu, uint32_t addr, uint16_t val) {
    cpu->write16(mask_24(addr), val);
}

static inline void cpu_write32(m68k_t *cpu, uint32_t addr, uint32_t val) {
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
}

static inline uint16_t pop16(m68k_t *cpu) {
    uint16_t val = cpu_read16(cpu, cpu->a[7]);
    cpu->a[7] += 2;
    return val;
}

static inline uint32_t pop32(m68k_t *cpu) {
    uint32_t val = cpu_read32(cpu, cpu->a[7]);
    cpu->a[7] += 4;
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

static int exception_histogram[256] = {0};

static void take_exception(m68k_t *cpu, int vector) {
    /* Count all exceptions by type */
    if (vector < 256) exception_histogram[vector]++;

    /* Detect stack overflow */
    if (cpu->a[7] < 0x1000 || cpu->a[7] > 0x1FF000) {
        static int overflow_reported = 0;
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

    /* Trace exceptions for debugging */
    if (exception_count < 50) {
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

        exception_count++;
    }

    /* Save old SR, enter supervisor mode */
    uint16_t old_sr = cpu->sr;
    set_supervisor(cpu, true);
    cpu->sr &= ~SR_TRACE;

    /* Push PC and SR */
    push32(cpu, cpu->pc);
    push16(cpu, old_sr);

    /* Read new PC from vector table */
    cpu->pc = cpu_read32(cpu, vector * 4);
    cpu->cycles += 34;
}

static void privilege_violation(m68k_t *cpu) {
    take_exception(cpu, VEC_PRIVILEGE);
}

static int illegal_opcode_histogram[16] = {0};

static void illegal_instruction(m68k_t *cpu) {
    /* Track which opcode groups trigger illegal instruction */
    int group = (cpu->ir >> 12) & 0xF;
    if (illegal_opcode_histogram[group]++ < 5) {
        fprintf(stderr, "ILLEGAL: opcode=$%04X group=%X at PC=$%06X\n",
                cpu->ir, group, cpu->pc - 2);
    }
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
        cpu->pc = base + disp;
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
    cpu->pc = pop32(cpu);
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
static void op_trap(m68k_t *cpu) {
    int vector = cpu->ir & 0xF;
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
            if ((op & 0x0100) && ((op & 0x00C0) != 0x00C0)) {
                /* Dynamic bit ops or MOVEP */
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
                /* Accept interrupt */
                cpu->stopped = false;
                uint16_t old_sr = cpu->sr;
                set_supervisor(cpu, true);
                cpu->sr = (cpu->sr & ~SR_INT_MASK) | (cpu->pending_irq << 8);
                cpu->sr &= ~SR_TRACE;
                push32(cpu, cpu->pc);
                push16(cpu, old_sr);
                cpu->pc = cpu_read32(cpu, (VEC_AUTOVECTOR_BASE + cpu->pending_irq - 1) * 4);
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

        /* PC ring buffer for crash tracing */
        static uint32_t pc_ring[32];
        static int pc_ring_idx = 0;
        static bool line_f_logged = false;
        pc_ring[pc_ring_idx++ & 31] = cpu->pc;
        if (!line_f_logged && cpu->pc == 0x20DB1E) {
            fprintf(stderr, "=== LINE-F TRACE: PC=$%06X reached. Last 20 PCs:\n", cpu->pc);
            for (int ri = 20; ri > 0; ri--) {
                uint32_t rpc = pc_ring[(pc_ring_idx - ri) & 31];
                uint16_t rop = cpu_read16(cpu, rpc);
                fprintf(stderr, "  PC=$%06X opcode=$%04X\n", rpc, rop);
            }
            fprintf(stderr, "  D0=$%08X A0=$%08X A5=$%08X A6=$%08X A7=$%08X\n",
                    cpu->d[0], cpu->a[0], cpu->a[5], cpu->a[6], cpu->a[7]);
            line_f_logged = true;
        }

        /* Breakpoint: log when PC enters INIT_TRAPV */
        static int initrap_logged = 0;
        if (!initrap_logged && cpu->pc >= 0x178000 && cpu->pc <= 0x179000) {
            /* Check if this is near INIT_TRAPV ($17850C) */
            if (cpu->pc >= 0x178500 && cpu->pc <= 0x178510) {
                fprintf(stderr, "INIT_TRAPV HIT: PC=$%06X A0=$%08X A1=$%08X A2=$%08X A5=$%08X SP=$%08X\n",
                        cpu->pc, cpu->a[0], cpu->a[1], cpu->a[2], cpu->a[5], cpu->a[7]);
                fprintf(stderr, "  RAM[$84]=$%02X%02X%02X%02X before INIT_TRAPV\n",
                        cpu_read8(cpu, 0x84), cpu_read8(cpu, 0x85),
                        cpu_read8(cpu, 0x86), cpu_read8(cpu, 0x87));
                initrap_logged = 1;
            }
        }

        /* Log when PC enters vector table after being in OS code */
        static uint32_t prev_pc = 0;
        static int crash_logged = 0;
        if (!crash_logged && cpu->pc < 0x400 && prev_pc >= 0x400 && prev_pc < 0x200000) {
            fprintf(stderr, "CRASH TO VECTORS: prev_pc=$%06X → pc=$%06X opcode=$%04X A7=$%08X\n",
                    prev_pc, cpu->pc, cpu_read16(cpu, prev_pc), cpu->a[7]);
            crash_logged = 1;
        }
        if (cpu->pc >= 0x400) prev_pc = cpu->pc;

        cpu->cycles = 0;
        execute_one(cpu);
        if (cpu->cycles == 0) cpu->cycles = 4; /* minimum */
        cpu->total_cycles += cpu->cycles;

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
