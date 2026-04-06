/*
 * LisaEm — ProFile Hard Disk Emulation
 *
 * Implements the VIA1 handshake protocol matching SOURCE-PROFILEASM.TEXT.
 *
 * Protocol for READ (matching OS driver states S1-S7):
 *   S1: Host asserts CMD (ORB bit 4 low) → device goes BSY
 *   S2: Host reads PORTA → gets 0x01 (ready). Host writes $55 (ack).
 *       Host deasserts CMD → device goes not-BSY.
 *   S3: Host asserts CMD, sends 6 command bytes on PORTA, deasserts CMD.
 *   S1A: Second handshake — device goes BSY.
 *   S200: Host reads PORTA → gets 0x02 (read ready). Host writes $55.
 *         Host deasserts CMD → device goes not-BSY.
 *   S6: Host reads 4 status bytes from PORTA.
 *   S7: Host reads 20 tag + 512 data bytes from PORTA.
 *
 * Protocol for WRITE is similar but S200 response is 0x03 and data
 * flows from host to device.
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

    if (block == 0xFFFFFF) {
        /* Spare table / device ID block — PROF_INIT reads this.
         * Return a minimal valid spare table identifying a 5MB ProFile. */
        p->sector_buf[20] = 0x00;  /* Device type: ProFile */
        p->sector_buf[21] = 0x00;
        p->sector_buf[22] = 0x00;
        p->sector_buf[23] = 0x00;
        /* Number of blocks (3 bytes, big-endian): 9728 = $2600 */
        p->sector_buf[24] = 0x00;
        p->sector_buf[25] = 0x26;
        p->sector_buf[26] = 0x00;
        /* Bytes per block: 532 (20 tag + 512 data) */
        p->sector_buf[27] = 0x02;
        p->sector_buf[28] = 0x14;
        /* Number of spares allocated: 0 */
        p->sector_buf[29] = 0x00;
        p->sector_buf[30] = 0x00;
        /* Number of spares used: 0 */
        p->sector_buf[31] = 0x00;
        p->sector_buf[32] = 0x00;
        return;
    }

    /* Normal block: read from disk image (tag + data = 532 bytes per block) */
    size_t offset = (size_t)block * PROFILE_SECTOR_SIZE;
    if (offset + PROFILE_SECTOR_SIZE <= p->data_size) {
        memcpy(p->sector_buf, p->data + offset, PROFILE_SECTOR_SIZE);
    }
}

static void profile_write_block(profile_t *p, uint32_t block) {
    size_t offset = (size_t)block * PROFILE_SECTOR_SIZE;
    if (offset + PROFILE_SECTOR_SIZE <= p->data_size) {
        memcpy(p->data + offset, p->sector_buf, PROFILE_SECTOR_SIZE);
    }
}

/* Called when host writes to VIA1 ORB.
 * Bit 3 = DIR (1=host reading from device)
 * Bit 4 = CMD (0=asserted, active low) */
void profile_orb_write(profile_t *p, uint8_t orb, uint8_t old_orb) {
    if (!p->mounted) return;

    bool cmd_now = !(orb & 0x10);   /* CMD active low */
    bool cmd_was = !(old_orb & 0x10);

    /* CMD falling edge (host asserting CMD) */
    if (cmd_now && !cmd_was) {
        switch (p->state) {
            case PSTATE_IDLE:
                /* First handshake: device goes busy */
                p->state = PSTATE_GOT_CMD;
                p->busy = true;
                p->response_byte = 0x01;  /* Response: ready for command */
                break;

            case PSTATE_RECV_CMD:
                /* Host re-asserts CMD for command byte phase — expected */
                break;

            case PSTATE_HANDSHAKE2:
                /* Second handshake after command: device goes busy */
                p->busy = true;
                /* Response depends on command type:
                 * 0x02 = read data ready, 0x03 = write data ready */
                p->response_byte = (p->command[0] == 0x00) ? 0x02 : 0x03;
                p->state = PSTATE_GOT_CMD2;
                break;

            default:
                break;
        }
    }

    /* CMD rising edge (host deasserting CMD) */
    if (!cmd_now && cmd_was) {
        switch (p->state) {
            case PSTATE_WAIT_CMD_DEASSERT:
                /* After host sent $55 ack, CMD deasserted.
                 * Device goes not-busy, ready for command bytes. */
                p->busy = false;
                p->state = PSTATE_RECV_CMD;
                p->cmd_index = 0;
                break;

            case PSTATE_RECV_CMD:
                /* Host deasserts CMD after sending command bytes.
                 * Transition to second handshake. */
                if (p->cmd_index >= 6) {
                    p->state = PSTATE_HANDSHAKE2;
                    p->busy = true;
                    /* Process command now */
                    uint8_t cmd = p->command[0];
                    p->block_num = ((uint32_t)p->command[1] << 16) |
                                   ((uint32_t)p->command[2] << 8) |
                                   (uint32_t)p->command[3];

                    if (cmd == 0x00) {
                        /* READ — read block into buffer */
                        static int read_count = 0;
                        if (read_count++ < 20)
                            fprintf(stderr, "ProFile READ block $%06X\n", p->block_num);
                        profile_read_block(p, p->block_num);
                        p->status[0] = 0;
                        p->status[1] = 0;
                        p->status[2] = 0;
                        p->status[3] = 0;
                    } else if (cmd == 0x01) {
                        /* WRITE — will receive data after second handshake */
                        p->status[0] = 0;
                        p->status[1] = 0;
                        p->status[2] = 0;
                        p->status[3] = 0;
                    } else {
                        /* Unknown command */
                        static int unk_count = 0;
                        if (unk_count++ < 5)
                            fprintf(stderr, "ProFile UNKNOWN cmd $%02X block $%06X\n",
                                    cmd, p->block_num);
                        p->status[0] = 0xFF;
                        p->status[1] = 0;
                        p->status[2] = 0;
                        p->status[3] = 0;
                    }
                }
                break;

            case PSTATE_WAIT_CMD_DEASSERT2:
                /* After second $55 ack, CMD deasserted.
                 * Device goes not-busy, ready for status/data transfer. */
                p->busy = false;
                if (p->command[0] == 0x00) {
                    /* READ: send status then data */
                    p->state = PSTATE_SEND_STATUS;
                } else if (p->command[0] == 0x01) {
                    /* WRITE: send status first, then receive data */
                    p->state = PSTATE_SEND_STATUS;
                } else {
                    p->state = PSTATE_SEND_STATUS;
                }
                p->byte_index = 0;
                break;

            default:
                break;
        }
    }
}

/* Called when host writes to VIA1 PORTA/ORA */
void profile_porta_write(profile_t *p, uint8_t val) {
    if (!p->mounted) return;

    switch (p->state) {
        case PSTATE_GOT_CMD:
            /* Host sends $55 acknowledge after reading response byte */
            if (val == 0x55) {
                p->state = PSTATE_WAIT_CMD_DEASSERT;
                /* Stay busy until CMD deasserts */
            }
            break;

        case PSTATE_GOT_CMD2:
            /* Second handshake: host sends $55 after reading response */
            if (val == 0x55) {
                p->state = PSTATE_WAIT_CMD_DEASSERT2;
            }
            break;

        case PSTATE_RECV_CMD:
            /* Accumulate command bytes */
            if (p->cmd_index < 6) {
                p->command[p->cmd_index++] = val;
            }
            break;

        case PSTATE_RECV_DATA:
            /* Accumulate write data (20 tag + 512 data) */
            if (p->byte_index < PROFILE_SECTOR_SIZE) {
                p->sector_buf[p->byte_index++] = val;
            }
            if (p->byte_index >= PROFILE_SECTOR_SIZE) {
                static int write_count = 0;
                if (write_count++ < 10)
                    fprintf(stderr, "ProFile WRITE block $%06X\n", p->block_num);
                profile_write_block(p, p->block_num);
                p->state = PSTATE_IDLE;
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
        case PSTATE_GOT_CMD2:
            /* Host reading response byte */
            return p->response_byte;

        case PSTATE_SEND_STATUS:
            if (p->byte_index < 4) {
                return p->status[p->byte_index++];
            }
            /* Status done — switch to data phase */
            if (p->command[0] == 0x00) {
                /* READ: send data */
                p->state = PSTATE_SEND_DATA;
                p->byte_index = 0;
                return p->sector_buf[p->byte_index++];
            } else if (p->command[0] == 0x01) {
                /* WRITE: receive data */
                p->state = PSTATE_RECV_DATA;
                p->byte_index = 0;
                return 0;
            }
            p->state = PSTATE_IDLE;
            return 0xFF;

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
