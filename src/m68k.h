/*
 * LisaEm - Apple Lisa Emulator
 * Motorola 68000 CPU Emulator - Header
 *
 * Targets: Apple Silicon (arm64-apple-darwin)
 */

#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <stdbool.h>

/* Status Register bits */
#define SR_CARRY     0x0001
#define SR_OVERFLOW  0x0002
#define SR_ZERO      0x0004
#define SR_NEGATIVE  0x0008
#define SR_EXTEND    0x0010
#define SR_INT_MASK  0x0700
#define SR_SUPERVISOR 0x2000
#define SR_TRACE     0x8000

/* Exception vectors */
#define VEC_RESET_SSP       0
#define VEC_RESET_PC        1
#define VEC_BUS_ERROR       2
#define VEC_ADDRESS_ERROR   3
#define VEC_ILLEGAL_INST    4
#define VEC_ZERO_DIVIDE     5
#define VEC_CHK             6
#define VEC_TRAPV           7
#define VEC_PRIVILEGE       8
#define VEC_TRACE           9
#define VEC_LINE_A          10
#define VEC_LINE_F          11
#define VEC_SPURIOUS        24
#define VEC_AUTOVECTOR_BASE 25  /* 25-31 for levels 1-7 */
#define VEC_TRAP_BASE       32  /* 32-47 for TRAP #0 - #15 */

/* Addressing mode encoding (from instruction bits) */
#define AM_DATA_REG    0  /* Dn */
#define AM_ADDR_REG    1  /* An */
#define AM_ADDR_IND    2  /* (An) */
#define AM_POST_INC    3  /* (An)+ */
#define AM_PRE_DEC     4  /* -(An) */
#define AM_DISP        5  /* d16(An) */
#define AM_INDEX       6  /* d8(An,Xn) */
#define AM_OTHER       7  /* abs.W, abs.L, d16(PC), d8(PC,Xn), #imm */

/* Size encodings */
#define SIZE_BYTE  0
#define SIZE_WORD  1
#define SIZE_LONG  2

/* CPU state */
typedef struct {
    uint32_t d[8];       /* Data registers D0-D7 */
    uint32_t a[8];       /* Address registers A0-A7 (A7 = active SP) */
    uint32_t pc;         /* Program counter */
    uint16_t sr;         /* Status register */
    uint32_t usp;        /* User stack pointer */
    uint32_t ssp;        /* Supervisor stack pointer */

    /* Internal state */
    int cycles;          /* Cycles consumed by last instruction */
    int total_cycles;    /* Total cycles elapsed */
    bool stopped;        /* STOP instruction active */
    bool halted;         /* Double bus fault */
    int pending_irq;     /* Pending interrupt level (0=none) */

    /* Prefetch */
    uint16_t ir;         /* Current instruction register */

    /* Callbacks for memory access */
    uint8_t  (*read8)(uint32_t addr);
    uint16_t (*read16)(uint32_t addr);
    uint32_t (*read32)(uint32_t addr);
    void     (*write8)(uint32_t addr, uint8_t val);
    void     (*write16)(uint32_t addr, uint16_t val);
    void     (*write32)(uint32_t addr, uint32_t val);

    /* HLE callback — if set, called before each instruction.
     * Returns true if the instruction was handled (skip normal execution).
     * Both args are void* to avoid circular header dependency. */
    bool     (*hle_check)(void *hle_ctx, void *cpu);
    void     *hle_ctx;
} m68k_t;

/* Public API */
void m68k_init(m68k_t *cpu);
void m68k_reset(m68k_t *cpu);
int  m68k_execute(m68k_t *cpu, int target_cycles);
void m68k_set_irq(m68k_t *cpu, int level);
void m68k_pulse_reset(m68k_t *cpu);

/* For debugger */
uint32_t m68k_get_pc(m68k_t *cpu);
uint32_t m68k_get_reg(m68k_t *cpu, int reg);
uint16_t m68k_get_sr(m68k_t *cpu);

/* Exception vector histogram — visible for diag dumps. Reset by consumer. */
extern int m68k_exception_histogram[256];
/* TRAP #5 selector histogram (D7 & 0xFF at entry). Reset by consumer. */
extern int m68k_trap5_selector_histogram[256];

#endif /* M68K_H */
