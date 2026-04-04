/*
 * LisaEm - Apple Lisa Emulator
 * MOS 6522 VIA Emulation
 */

#include "via6522.h"
#include <string.h>

static void update_irq(via6522_t *via) {
    bool irq = (via->ifr & via->ier & 0x7F) != 0;
    if (irq)
        via->ifr |= VIA_IRQ_ANY;
    else
        via->ifr &= ~VIA_IRQ_ANY;

    if (via->irq_callback)
        via->irq_callback(irq, via->irq_ctx);
}

void via_init(via6522_t *via) {
    memset(via, 0, sizeof(via6522_t));
}

void via_reset(via6522_t *via) {
    via->orb = via->ora = 0;
    via->irb = via->ira = 0xFF;
    via->ddrb = via->ddra = 0;
    via->sr = 0;
    via->acr = via->pcr = 0;
    via->ifr = via->ier = 0;
    via->t1_counter = 0xFFFF;
    via->t1_latch = 0xFFFF;
    via->t2_counter = 0xFFFF;
    via->t2_latch_low = 0xFF;
    via->t1_running = false;
    via->t2_running = false;
    via->t1_oneshot_fired = false;
}

uint8_t via_read(via6522_t *via, uint8_t reg) {
    reg &= 0x0F;

    switch (reg) {
        case VIA_ORB: {
            /* Read port B: input bits come from IRB, output bits from ORB */
            via->ifr &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
            update_irq(via);
            uint8_t ext = via->port_b_read ? via->port_b_read(via->callback_ctx) : via->irb;
            return (via->orb & via->ddrb) | (ext & ~via->ddrb);
        }
        case VIA_ORA: {
            via->ifr &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
            update_irq(via);
            uint8_t ext = via->port_a_read ? via->port_a_read(via->callback_ctx) : via->ira;
            return (via->ora & via->ddra) | (ext & ~via->ddra);
        }
        case VIA_DDRB: return via->ddrb;
        case VIA_DDRA: return via->ddra;

        case VIA_T1CL:
            via->ifr &= ~VIA_IRQ_T1;
            update_irq(via);
            return via->t1_counter & 0xFF;
        case VIA_T1CH:
            return (via->t1_counter >> 8) & 0xFF;
        case VIA_T1LL:
            return via->t1_latch & 0xFF;
        case VIA_T1LH:
            return (via->t1_latch >> 8) & 0xFF;

        case VIA_T2CL:
            via->ifr &= ~VIA_IRQ_T2;
            update_irq(via);
            return via->t2_counter & 0xFF;
        case VIA_T2CH:
            return (via->t2_counter >> 8) & 0xFF;

        case VIA_SR:
            via->ifr &= ~VIA_IRQ_SR;
            update_irq(via);
            return via->sr;
        case VIA_ACR: return via->acr;
        case VIA_PCR: return via->pcr;
        case VIA_IFR: return via->ifr;
        case VIA_IER: return via->ier | 0x80;

        case VIA_ORA_NH: {
            uint8_t ext = via->port_a_read ? via->port_a_read(via->callback_ctx) : via->ira;
            return (via->ora & via->ddra) | (ext & ~via->ddra);
        }
    }
    return 0xFF;
}

void via_write(via6522_t *via, uint8_t reg, uint8_t val) {
    reg &= 0x0F;

    switch (reg) {
        case VIA_ORB:
            via->orb = val;
            via->ifr &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
            update_irq(via);
            if (via->port_b_write)
                via->port_b_write(val, via->ddrb, via->callback_ctx);
            break;

        case VIA_ORA:
            via->ora = val;
            via->ifr &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
            update_irq(via);
            if (via->port_a_write)
                via->port_a_write(val, via->ddra, via->callback_ctx);
            break;

        case VIA_DDRB: via->ddrb = val; break;
        case VIA_DDRA: via->ddra = val; break;

        case VIA_T1CL:
        case VIA_T1LL:
            via->t1_latch = (via->t1_latch & 0xFF00) | val;
            break;

        case VIA_T1CH:
            via->t1_latch = (via->t1_latch & 0x00FF) | ((uint16_t)val << 8);
            via->t1_counter = via->t1_latch;
            via->t1_running = true;
            via->t1_oneshot_fired = false;
            via->ifr &= ~VIA_IRQ_T1;
            update_irq(via);
            break;

        case VIA_T1LH:
            via->t1_latch = (via->t1_latch & 0x00FF) | ((uint16_t)val << 8);
            via->ifr &= ~VIA_IRQ_T1;
            update_irq(via);
            break;

        case VIA_T2CL:
            via->t2_latch_low = val;
            break;

        case VIA_T2CH:
            via->t2_counter = ((uint16_t)val << 8) | via->t2_latch_low;
            via->t2_running = true;
            via->ifr &= ~VIA_IRQ_T2;
            update_irq(via);
            break;

        case VIA_SR:
            via->sr = val;
            via->ifr &= ~VIA_IRQ_SR;
            update_irq(via);
            break;

        case VIA_ACR: via->acr = val; break;
        case VIA_PCR: via->pcr = val; break;

        case VIA_IFR:
            via->ifr &= ~(val & 0x7F);
            update_irq(via);
            break;

        case VIA_IER:
            if (val & 0x80)
                via->ier |= (val & 0x7F);  /* Set bits */
            else
                via->ier &= ~(val & 0x7F); /* Clear bits */
            update_irq(via);
            break;

        case VIA_ORA_NH:
            via->ora = val;
            if (via->port_a_write)
                via->port_a_write(val, via->ddra, via->callback_ctx);
            break;
    }
}

void via_tick(via6522_t *via, int cycles) {
    /* Timer 1 */
    if (via->t1_running) {
        int old = via->t1_counter;
        via->t1_counter -= cycles;

        if ((int16_t)via->t1_counter < 0 || via->t1_counter > (uint16_t)old) {
            /* Timer 1 underflow */
            if (via->acr & 0x40) {
                /* Free-running mode: reload and keep firing */
                via->t1_counter = via->t1_latch;
                via->ifr |= VIA_IRQ_T1;
                /* Toggle PB7 if enabled */
                if (via->acr & 0x80)
                    via->orb ^= 0x80;
            } else {
                /* One-shot mode */
                if (!via->t1_oneshot_fired) {
                    via->ifr |= VIA_IRQ_T1;
                    via->t1_oneshot_fired = true;
                }
                via->t1_running = false;
            }
            update_irq(via);
        }
    }

    /* Timer 2 (one-shot only in timed mode) */
    if (via->t2_running && !(via->acr & 0x20)) {
        int old = via->t2_counter;
        via->t2_counter -= cycles;

        if ((int16_t)via->t2_counter < 0 || via->t2_counter > (uint16_t)old) {
            via->ifr |= VIA_IRQ_T2;
            via->t2_running = false;
            update_irq(via);
        }
    }
}

void via_set_ira(via6522_t *via, uint8_t val) { via->ira = val; }
void via_set_irb(via6522_t *via, uint8_t val) { via->irb = val; }

void via_trigger_ca1(via6522_t *via) {
    via->ifr |= VIA_IRQ_CA1;
    update_irq(via);
}

void via_trigger_ca2(via6522_t *via) {
    via->ifr |= VIA_IRQ_CA2;
    update_irq(via);
}

void via_trigger_cb1(via6522_t *via) {
    via->ifr |= VIA_IRQ_CB1;
    update_irq(via);
}

void via_trigger_cb2(via6522_t *via) {
    via->ifr |= VIA_IRQ_CB2;
    update_irq(via);
}
