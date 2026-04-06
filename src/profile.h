/*
 * LisaEm — ProFile Hard Disk Emulation
 *
 * Implements the VIA1 handshake protocol from SOURCE-PROFILEASM.TEXT.
 * Protocol states match the OS driver's state machine (S1-S7).
 */

#ifndef PROFILE_H
#define PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PROFILE_DATA_SIZE    512
#define PROFILE_TAG_SIZE     20
#define PROFILE_SECTOR_SIZE  (PROFILE_DATA_SIZE + PROFILE_TAG_SIZE)
#define PROFILE_BLOCK_SIZE   (PROFILE_TAG_SIZE + PROFILE_DATA_SIZE)

/* ProFile protocol states (matches OS driver S1-S7) */
typedef enum {
    PSTATE_IDLE,                /* Waiting for host CMD */
    PSTATE_GOT_CMD,             /* S1: went busy, host can read response byte */
    PSTATE_WAIT_CMD_DEASSERT,   /* S2: got $55 ack, waiting for CMD deassert */
    PSTATE_RECV_CMD,            /* S3: receiving 6 command bytes */
    PSTATE_HANDSHAKE2,          /* S1A: second handshake after command */
    PSTATE_GOT_CMD2,            /* S200: second handshake busy, host reads response */
    PSTATE_WAIT_CMD_DEASSERT2,  /* Got $55, waiting for CMD deassert */
    PSTATE_SEND_STATUS,         /* S6: sending 4 status bytes */
    PSTATE_SEND_DATA,           /* S7: sending 532 bytes (20 tag + 512 data) */
    PSTATE_RECV_DATA,           /* S7: receiving 532 bytes for write */
} profile_state_t;

typedef struct {
    /* Disk image */
    uint8_t *data;
    size_t data_size;
    bool mounted;

    /* Protocol state */
    profile_state_t state;
    int byte_index;         /* Current byte in transfer */
    uint8_t command[6];     /* 6-byte command */
    int cmd_index;
    uint32_t block_num;

    /* Transfer buffers */
    uint8_t status[4];      /* 4-byte status response */
    uint8_t sector_buf[PROFILE_SECTOR_SIZE]; /* 20 tag + 512 data */

    /* VIA interface signals (directly mapped to VIA1 ORB/PORTA) */
    bool busy;              /* BSY line (active low on real HW) */
    bool cmd_line;          /* Host CMD signal */
    uint8_t response_byte;  /* Next byte to put on PORTA when host reads */
} profile_t;

void profile_init(profile_t *p);
void profile_mount(profile_t *p, uint8_t *data, size_t size);

/* Called when host writes to VIA1 ORB (port B) */
void profile_orb_write(profile_t *p, uint8_t orb, uint8_t old_orb);

/* Called when host writes to VIA1 PORTA/ORA */
void profile_porta_write(profile_t *p, uint8_t val);

/* Called when host reads from VIA1 PORTA/IRA */
uint8_t profile_porta_read(profile_t *p);

/* Returns BSY state for VIA1 IRB bit 1 */
bool profile_bsy(profile_t *p);

#endif /* PROFILE_H */
