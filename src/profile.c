/*
 * LisaEm — ProFile Hard Disk Emulation
 *
 * Implements the VIA1 handshake protocol matching SOURCE-PROFILEASM.TEXT.
 *
 * Protocol for READ:
 *   1. Host asserts CMD (ORB bit 4 clear)
 *   2. Device goes BSY (CA1 falling edge)
 *   3. Host reads PORTA → gets response byte (1 = ready)
 *   4. Host sends $55 on PORTA (acknowledge)
 *   5. Host deasserts CMD (ORB bit 4 set)
 *   6. Device goes not-BSY (CA1 rising edge)
 *   7. Host asserts CMD again, sends 6 command bytes on PORTA
 *   8. Device goes BSY, processes command
 *   9. When ready: device goes not-BSY
 *  10. Host reads 4 status bytes from PORTA
 *  11. Host reads 20 tag + 512 data bytes from PORTA
 */

#include "profile.h"
#include <string.h>
#include <stdio.h>

void profile_init(profile_t *p) {
    memset(p, 0, sizeof(profile_t));
    p->state = PSTATE_IDLE;
    p->busy = false;
}

void profile_mount(profile_t *p, uint8_t *data, size_t size) {
    p->data = data;
    p->data_size = size;
    p->mounted = true;
    p->state = PSTATE_IDLE;
    p->busy = false;
}

static void profile_read_block(profile_t *p, uint32_t block) {
    memset(p->sector_buf, 0, PROFILE_SECTOR_SIZE);

    /* Tag bytes (20 bytes) — minimal valid tag */
    /* Bytes 0-1: file ID (0 for free, AAAA for boot) */
    /* Bytes 2-3: absolute page number */
    p->sector_buf[0] = 0;
    p->sector_buf[1] = 0;
    p->sector_buf[2] = (block >> 8) & 0xFF;
    p->sector_buf[3] = block & 0xFF;

    /* Data (512 bytes) at offset 20 */
    size_t offset = (size_t)block * (PROFILE_TAG_SIZE + PROFILE_DATA_SIZE);
    if (offset + PROFILE_SECTOR_SIZE <= p->data_size) {
        memcpy(p->sector_buf, p->data + offset, PROFILE_SECTOR_SIZE);
    }
}

static void profile_write_block(profile_t *p, uint32_t block) {
    size_t offset = (size_t)block * (PROFILE_TAG_SIZE + PROFILE_DATA_SIZE);
    if (offset + PROFILE_SECTOR_SIZE <= p->data_size) {
        memcpy(p->data + offset, p->sector_buf, PROFILE_SECTOR_SIZE);
    }
}

/* Called when host writes to VIA1 ORB.
 * Key bits: bit 3 = DIR (1=host reading from device), bit 4 = CMD (0=asserted) */
void profile_orb_write(profile_t *p, uint8_t orb, uint8_t old_orb) {
    if (!p->mounted) return;

    bool cmd_now = !(orb & 0x10);   /* CMD active low */
    bool cmd_was = !(old_orb & 0x10);

    /* CMD falling edge (host asserting CMD) */
    if (cmd_now && !cmd_was) {
        if (p->state == PSTATE_IDLE) {
            /* First handshake: device goes busy, puts response on PORTA */
            p->state = PSTATE_GOT_CMD;
            p->busy = true;
            p->response_byte = 0x01;  /* Response: ready */
        } else if (p->state == PSTATE_WAIT_55) {
            /* After $55 ack, host re-asserts CMD for command bytes */
            /* Actually this transition is handled differently —
             * after host sends $55, host sets CMD=false, device goes not-busy,
             * then host asserts CMD again for command phase */
        }
    }

    /* CMD rising edge (host deasserting CMD) */
    if (!cmd_now && cmd_was) {
        if (p->state == PSTATE_WAIT_55) {
            /* Host sent $55 and now deasserts CMD.
             * Device goes not-busy to signal ready for command bytes. */
            p->busy = false;
            p->state = PSTATE_RECV_CMD;
            p->cmd_index = 0;
        }
    }

    /* Second CMD assertion for command bytes */
    if (cmd_now && !cmd_was && p->state == PSTATE_RECV_CMD) {
        /* Host is about to send command bytes */
    }
}

/* Called when host writes to VIA1 PORTA/ORA */
void profile_porta_write(profile_t *p, uint8_t val) {
    if (!p->mounted) return;

    switch (p->state) {
        case PSTATE_GOT_CMD:
            /* Host is sending response byte — should be $55 */
            if (val == 0x55 || val == 0x69) {
                p->state = PSTATE_WAIT_55;
                /* Device acknowledges — will go not-busy on CMD deassert */
                p->busy = false;
                p->state = PSTATE_RECV_CMD;
                p->cmd_index = 0;
            }
            break;

        case PSTATE_RECV_CMD:
            /* Accumulate command bytes */
            if (p->cmd_index < 6) {
                p->command[p->cmd_index++] = val;
            }
            if (p->cmd_index >= 6) {
                /* Command complete */
                uint8_t cmd = p->command[0];
                p->block_num = ((uint32_t)p->command[1] << 16) |
                               ((uint32_t)p->command[2] << 8) |
                               (uint32_t)p->command[3];

                /* Go busy while processing */
                p->busy = true;

                if (cmd == 0x00) {
                    /* READ */
                    static int read_count = 0;
                    if (read_count++ < 10)
                        fprintf(stderr, "ProFile READ block %u\n", p->block_num);
                    profile_read_block(p, p->block_num);
                    /* Set up status + data for reading */
                    p->status[0] = 0;  /* No error */
                    p->status[1] = 0;
                    p->status[2] = 0;
                    p->status[3] = 0;
                    p->state = PSTATE_SEND_STATUS;
                    p->byte_index = 0;
                    /* Device goes not-busy to signal data ready */
                    p->busy = false;
                } else if (cmd == 0x01) {
                    /* WRITE — prepare to receive data */
                    p->state = PSTATE_RECV_DATA;
                    p->byte_index = 0;
                    p->busy = false;
                } else {
                    /* Unknown command — return error status */
                    p->status[0] = 0xFF;
                    p->status[1] = 0;
                    p->status[2] = 0;
                    p->status[3] = 0;
                    p->state = PSTATE_SEND_STATUS;
                    p->byte_index = 0;
                    p->busy = false;
                }
            }
            break;

        case PSTATE_RECV_DATA:
            /* Accumulate write data (20 tag + 512 data) */
            if (p->byte_index < PROFILE_SECTOR_SIZE) {
                p->sector_buf[p->byte_index++] = val;
            }
            if (p->byte_index >= PROFILE_SECTOR_SIZE) {
                profile_write_block(p, p->block_num);
                p->status[0] = 0;
                p->status[1] = 0;
                p->status[2] = 0;
                p->status[3] = 0;
                p->state = PSTATE_SEND_STATUS;
                p->byte_index = 0;
                p->busy = false;
            }
            break;

        default:
            break;
    }
}

/* Called when host reads from VIA1 PORTA/IRA */
uint8_t profile_porta_read(profile_t *p) {
    if (!p->mounted) return 0xFF;

    switch (p->state) {
        case PSTATE_GOT_CMD:
            /* Host reading our response byte */
            return p->response_byte;

        case PSTATE_SEND_STATUS:
            if (p->byte_index < 4) {
                return p->status[p->byte_index++];
            }
            /* Status done — switch to data */
            p->state = PSTATE_SEND_DATA;
            p->byte_index = 0;
            /* fall through */

        case PSTATE_SEND_DATA:
            if (p->byte_index < PROFILE_SECTOR_SIZE) {
                uint8_t val = p->sector_buf[p->byte_index++];
                if (p->byte_index >= PROFILE_SECTOR_SIZE) {
                    p->state = PSTATE_IDLE;
                    p->busy = false;
                }
                return val;
            }
            p->state = PSTATE_IDLE;
            return 0xFF;

        default:
            return 0xFF;
    }
}

bool profile_bsy(profile_t *p) {
    return p->busy;
}
