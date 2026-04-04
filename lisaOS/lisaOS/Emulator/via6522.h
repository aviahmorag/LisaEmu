/*
 * LisaEm - Apple Lisa Emulator
 * MOS 6522 VIA (Versatile Interface Adapter) Emulation
 *
 * Lisa has two VIAs:
 *   VIA1: Parallel port / ProFile hard disk interface
 *   VIA2: Keyboard / COPS (mouse, clock, power)
 */

#ifndef VIA6522_H
#define VIA6522_H

#include <stdint.h>
#include <stdbool.h>

/* VIA register offsets */
#define VIA_ORB    0x00  /* Output Register B */
#define VIA_ORA    0x01  /* Output Register A */
#define VIA_DDRB   0x02  /* Data Direction Register B */
#define VIA_DDRA   0x03  /* Data Direction Register A */
#define VIA_T1CL   0x04  /* Timer 1 Counter Low */
#define VIA_T1CH   0x05  /* Timer 1 Counter High */
#define VIA_T1LL   0x06  /* Timer 1 Latch Low */
#define VIA_T1LH   0x07  /* Timer 1 Latch High */
#define VIA_T2CL   0x08  /* Timer 2 Counter Low */
#define VIA_T2CH   0x09  /* Timer 2 Counter High */
#define VIA_SR     0x0A  /* Shift Register */
#define VIA_ACR    0x0B  /* Auxiliary Control Register */
#define VIA_PCR    0x0C  /* Peripheral Control Register */
#define VIA_IFR    0x0D  /* Interrupt Flag Register */
#define VIA_IER    0x0E  /* Interrupt Enable Register */
#define VIA_ORA_NH 0x0F  /* Output Register A (no handshake) */

/* IFR/IER bits */
#define VIA_IRQ_CA2    0x01
#define VIA_IRQ_CA1    0x02
#define VIA_IRQ_SR     0x04
#define VIA_IRQ_CB2    0x08
#define VIA_IRQ_CB1    0x10
#define VIA_IRQ_T2     0x20
#define VIA_IRQ_T1     0x40
#define VIA_IRQ_ANY    0x80

typedef struct {
    /* Registers */
    uint8_t orb, ora;
    uint8_t irb, ira;      /* Input register values */
    uint8_t ddrb, ddra;
    uint8_t sr;
    uint8_t acr, pcr;
    uint8_t ifr, ier;

    /* Timers */
    uint16_t t1_counter;
    uint16_t t1_latch;
    uint16_t t2_counter;
    uint8_t  t2_latch_low;
    bool     t1_running;
    bool     t2_running;
    bool     t1_oneshot_fired;

    /* Port callbacks */
    void (*port_b_write)(uint8_t val, uint8_t ddr, void *ctx);
    void (*port_a_write)(uint8_t val, uint8_t ddr, void *ctx);
    uint8_t (*port_b_read)(void *ctx);
    uint8_t (*port_a_read)(void *ctx);
    void *callback_ctx;

    /* IRQ output callback */
    void (*irq_callback)(bool state, void *ctx);
    void *irq_ctx;
} via6522_t;

void via_init(via6522_t *via);
void via_reset(via6522_t *via);

uint8_t via_read(via6522_t *via, uint8_t reg);
void    via_write(via6522_t *via, uint8_t reg, uint8_t val);

/* Call periodically to tick timers. cycles = number of CPU cycles elapsed */
void via_tick(via6522_t *via, int cycles);

/* Set input register values (from external hardware) */
void via_set_ira(via6522_t *via, uint8_t val);
void via_set_irb(via6522_t *via, uint8_t val);

/* Trigger CA1/CA2/CB1/CB2 edges */
void via_trigger_ca1(via6522_t *via);
void via_trigger_ca2(via6522_t *via);
void via_trigger_cb1(via6522_t *via);
void via_trigger_cb2(via6522_t *via);

#endif /* VIA6522_H */
