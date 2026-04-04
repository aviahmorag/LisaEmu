/*
 * LisaEm - Apple Lisa Emulator
 * Main machine integration
 */

#include "lisa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations for internal callbacks */
static uint8_t  mem_read8_cb(uint32_t addr);
static uint16_t mem_read16_cb(uint32_t addr);
static uint32_t mem_read32_cb(uint32_t addr);
static void     mem_write8_cb(uint32_t addr, uint8_t val);
static void     mem_write16_cb(uint32_t addr, uint16_t val);
static void     mem_write32_cb(uint32_t addr, uint32_t val);
static uint8_t  io_read_cb(uint32_t addr);
static void     io_write_cb(uint32_t addr, uint8_t val);

/* Global pointer for CPU memory callbacks (68000 uses function pointers) */
static lisa_t *g_lisa = NULL;

/* ========================================================================
 * COPS queue management
 * ======================================================================== */

static void cops_queue_init(cops_queue_t *q) {
    q->head = q->tail = q->count = 0;
}

static void cops_enqueue(cops_queue_t *q, uint8_t byte) {
    if (q->count >= COPS_QUEUE_SIZE) return;
    q->queue[q->tail] = byte;
    q->tail = (q->tail + 1) % COPS_QUEUE_SIZE;
    q->count++;

    /* Trigger VIA2 CA1 interrupt when COPS has data */
    if (g_lisa) {
        via_trigger_ca1(&g_lisa->via2);
    }
}

static uint8_t cops_dequeue(cops_queue_t *q) {
    if (q->count == 0) return 0;
    uint8_t val = q->queue[q->head];
    q->head = (q->head + 1) % COPS_QUEUE_SIZE;
    q->count--;
    return val;
}

/* ========================================================================
 * Memory callback bridge (CPU -> Memory system)
 * ======================================================================== */

static uint8_t mem_read8_cb(uint32_t addr) {
    return lisa_mem_read8(&g_lisa->mem, addr);
}

static uint16_t mem_read16_cb(uint32_t addr) {
    return lisa_mem_read16(&g_lisa->mem, addr);
}

static uint32_t mem_read32_cb(uint32_t addr) {
    return lisa_mem_read32(&g_lisa->mem, addr);
}

static void mem_write8_cb(uint32_t addr, uint8_t val) {
    lisa_mem_write8(&g_lisa->mem, addr, val);
}

static void mem_write16_cb(uint32_t addr, uint16_t val) {
    lisa_mem_write16(&g_lisa->mem, addr, val);
}

static void mem_write32_cb(uint32_t addr, uint32_t val) {
    lisa_mem_write32(&g_lisa->mem, addr, val);
}

/* ========================================================================
 * I/O space handler
 * ======================================================================== */

static uint8_t io_read_cb(uint32_t offset) {
    lisa_t *lisa = g_lisa;

    /* VIA1 - parallel/ProFile - registers at odd bytes */
    if (offset >= 0xD801 && offset < 0xD81F) {
        uint8_t reg = (offset - 0xD801) / 2;
        return via_read(&lisa->via1, reg);
    }

    /* VIA2 - keyboard/COPS */
    if (offset >= 0xDD81 && offset < 0xDD9F) {
        uint8_t reg = (offset - 0xDD81) / 2;
        return via_read(&lisa->via2, reg);
    }

    /* Vertical retrace acknowledge — reading clears the IRQ */
    if (offset >= 0xE018 && offset <= 0xE019) {
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        /* Recalculate IRQ level */
        int level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;
        m68k_set_irq(&lisa->cpu, level);
        return lisa->mem.vretrace_irq ? 0x80 : 0x00;
    }

    /* Status register */
    if (offset == 0xF800 || (offset >= 0xF800 && offset < 0xF900)) {
        return lisa->mem.status_reg;
    }

    /* Contrast */
    if (offset == 0xD01C) {
        return lisa->mem.contrast;
    }

    /* Disk controller shared memory */
    if (offset < 0x2000) {
        /* Disk shared memory area - return data from floppy controller */
        return 0;
    }

    return 0xFF;
}

static void io_write_cb(uint32_t offset, uint8_t val) {
    lisa_t *lisa = g_lisa;

    /* VIA1 */
    if (offset >= 0xD801 && offset < 0xD81F) {
        uint8_t reg = (offset - 0xD801) / 2;
        via_write(&lisa->via1, reg, val);
        return;
    }

    /* VIA2 */
    if (offset >= 0xDD81 && offset < 0xDD9F) {
        uint8_t reg = (offset - 0xDD81) / 2;
        via_write(&lisa->via2, reg, val);
        return;
    }

    /* Vertical retrace acknowledge — writing also clears */
    if (offset >= 0xE018 && offset <= 0xE019) {
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        int level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;
        m68k_set_irq(&lisa->cpu, level);
        return;
    }

    /* Contrast latch */
    if (offset == 0xD01C) {
        lisa->mem.contrast = val;
        return;
    }

    /* Video page latch */
    if (offset >= 0xE800 && offset < 0xE900) {
        lisa->mem.video_alt = (val & 1) != 0;
        lisa->mem.video_addr = lisa->mem.video_alt ? 0x7A000 : 0x78000;
        return;
    }

    /* MMU setup mode set/reset */
    if (offset == 0xE010) {
        lisa->mem.setup_mode = true;
        return;
    }
    if (offset == 0xE012) {
        lisa->mem.setup_mode = false;
        return;
    }

    /* Disk shared memory */
    if (offset < 0x2000) {
        return;
    }
}

/* Forward declarations */
static void profile_read_block(lisa_t *lisa, uint32_t block);
static void profile_write_block(lisa_t *lisa, uint32_t block);

/* ========================================================================
 * VIA callbacks
 * ======================================================================== */

/* ProFile state machine states */
#define PROF_IDLE       0   /* Waiting for command */
#define PROF_CMD        1   /* Receiving 6-byte command */
#define PROF_READING    2   /* Sending data to host */
#define PROF_WRITING    3   /* Receiving data from host */
#define PROF_STATUS     4   /* Sending status bytes */

/* VIA1 port B: ProFile interface control signals
 *   Bit 0: OCD (device connected)
 *   Bit 1: BSY (device not busy when high)
 *   Bit 2: CMD strobe from host
 *   Bit 3: host direction (0=host writing, 1=host reading)
 */
static void via1_portb_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    if (!lisa->profile.mounted) return;

    bool cmd_strobe = (val & 0x04) != 0;

    /* CMD strobe rising edge triggers command processing */
    if (cmd_strobe && lisa->profile.state == PROF_IDLE) {
        /* Host is starting a command — expect 6 bytes on Port A */
        lisa->profile.state = PROF_CMD;
        lisa->profile.cmd_index = 0;
        lisa->profile.busy = true;
    }
}

static uint8_t via1_portb_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    uint8_t val = 0;

    if (lisa->profile.mounted) {
        val |= 0x01; /* OCD - connected */
        if (!lisa->profile.busy)
            val |= 0x02; /* BSY - not busy */
    }

    return val;
}

static void via1_porta_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    if (!lisa->profile.mounted) return;

    if (lisa->profile.state == PROF_CMD) {
        /* Accumulate command bytes */
        if (lisa->profile.cmd_index < 6) {
            lisa->profile.command[lisa->profile.cmd_index++] = val;
        }
        if (lisa->profile.cmd_index >= 6) {
            /* Command complete. Byte 0 = command, bytes 1-3 = block number */
            uint8_t cmd = lisa->profile.command[0];
            uint32_t block = ((uint32_t)lisa->profile.command[1] << 16) |
                             ((uint32_t)lisa->profile.command[2] << 8) |
                             (uint32_t)lisa->profile.command[3];

            if (cmd == 0x00) {
                /* READ command */
                profile_read_block(lisa, block);
                lisa->profile.state = PROF_READING;
                lisa->profile.buf_index = 0;
                lisa->profile.busy = false;
            } else if (cmd == 0x01) {
                /* WRITE command */
                lisa->profile.block_num = block;
                lisa->profile.state = PROF_WRITING;
                lisa->profile.buf_index = 0;
                lisa->profile.busy = false;
            } else {
                /* Unknown command — just go idle */
                lisa->profile.state = PROF_IDLE;
                lisa->profile.busy = false;
            }
        }
    } else if (lisa->profile.state == PROF_WRITING) {
        /* Accumulate write data */
        if (lisa->profile.buf_index < PROFILE_BLOCK_SIZE) {
            lisa->profile.sector_buf[lisa->profile.buf_index++] = val;
        }
        if (lisa->profile.buf_index >= PROFILE_BLOCK_SIZE) {
            profile_write_block(lisa, lisa->profile.block_num);
            lisa->profile.state = PROF_IDLE;
        }
    }
}

static uint8_t via1_porta_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;

    if (lisa->profile.mounted && lisa->profile.state == PROF_READING) {
        if (lisa->profile.buf_index < PROFILE_BLOCK_SIZE) {
            uint8_t val = lisa->profile.sector_buf[lisa->profile.buf_index++];
            if (lisa->profile.buf_index >= PROFILE_BLOCK_SIZE) {
                lisa->profile.state = PROF_IDLE;
            }
            return val;
        }
    }
    return 0xFF;
}

/* VIA1 IRQ -> CPU IRQ level 1 */
static void via1_irq(bool state, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    lisa->irq_via1 = state ? 1 : 0;
    /* Recalculate CPU IRQ level */
    int level = 0;
    if (lisa->irq_vretrace) level = 1;
    if (lisa->irq_via1) level = 1;
    if (lisa->irq_via2) level = 2;
    m68k_set_irq(&lisa->cpu, level);
}

/* COPS command states */
#define COPS_IDLE       0
#define COPS_CMD_RECV   1

/* VIA2 port B: COPS interface
 *   Bit 0: COPS data available (for interrupt)
 *   Bit 4: CRDY - COPS ready for command (1=ready, 0=busy)
 */
static void via2_portb_write(uint8_t val, uint8_t ddr, void *ctx) {
    /* COPS reset/control — bit 0 can be used to reset COPS */
    lisa_t *lisa = (lisa_t *)ctx;
    if ((ddr & 0x01) && !(val & 0x01)) {
        /* COPS reset — queue a keyboard ID response */
        cops_enqueue(&lisa->cops_rx, 0x80);  /* Reset indicator */
        cops_enqueue(&lisa->cops_rx, 0x2F);  /* Keyboard ID: US layout */
    }
}

static uint8_t via2_portb_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    uint8_t val = 0;

    /* Bit 4: CRDY — COPS always ready for commands */
    val |= 0x10;

    /* Bit 0: data available from COPS */
    if (lisa->cops_rx.count > 0)
        val |= 0x01;

    return val;
}

static void via2_porta_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;

    /* OS is sending a command to COPS */
    /* Commands: $7C = enable mouse, $01 = read clock, etc. */
    /* We just acknowledge and don't do anything special for most */

    switch (val) {
        case 0x7C: /* Enable mouse with 16ms interval */
            /* Acknowledged — mouse will be sent via cops_rx queue when moved */
            break;
        case 0x01: /* Read clock */
            /* Queue clock response: 5 bytes (year, month/day, hour, min, sec) */
            cops_enqueue(&lisa->cops_rx, 0xE0 | 0x06);  /* Clock data, year nibble (1986) */
            cops_enqueue(&lisa->cops_rx, 0x01);  /* Month */
            cops_enqueue(&lisa->cops_rx, 0x01);  /* Day */
            cops_enqueue(&lisa->cops_rx, 0x00);  /* Hour */
            cops_enqueue(&lisa->cops_rx, 0x00);  /* Minute */
            break;
        default:
            /* Unknown command — just ignore */
            break;
    }
}

static uint8_t via2_porta_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;

    /* Read byte from COPS */
    if (lisa->cops_rx.count > 0) {
        return cops_dequeue(&lisa->cops_rx);
    }
    return 0xFF;  /* No data */
}

/* VIA2 IRQ -> CPU IRQ level 2 */
static void via2_irq(bool state, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    lisa->irq_via2 = state ? 1 : 0;
    int level = 0;
    if (lisa->irq_vretrace) level = 1;
    if (lisa->irq_via1) level = 1;
    if (lisa->irq_via2) level = 2;
    m68k_set_irq(&lisa->cpu, level);
}

/* ========================================================================
 * Display rendering
 * ======================================================================== */

static void render_framebuffer(lisa_t *lisa) {
    const uint8_t *video = lisa_mem_get_video(&lisa->mem);
    uint8_t contrast = lisa->mem.contrast;

    /* Calculate phosphor colors based on contrast */
    uint32_t fg_color, bg_color;
    if (contrast > 128) {
        /* Lisa has a white phosphor CRT */
        fg_color = 0xFF000000;                /* Black pixels (lit = dark on paper white) */
        bg_color = 0xFFFFFFFF;                /* White background */
    } else {
        fg_color = 0xFF000000;
        bg_color = 0xFF808080;                /* Dimmed background */
    }

    /* Convert 1-bit monochrome bitmap to ARGB */
    int pixel = 0;
    for (int y = 0; y < LISA_SCREEN_HEIGHT; y++) {
        for (int x = 0; x < LISA_SCREEN_WIDTH; x += 8) {
            int byte_idx = y * (LISA_SCREEN_WIDTH / 8) + (x / 8);
            uint8_t byte = video[byte_idx];

            for (int bit = 7; bit >= 0; bit--) {
                /* Lisa: 1 = black pixel, 0 = white pixel */
                lisa->framebuffer[pixel++] = (byte & (1 << bit)) ? fg_color : bg_color;
            }
        }
    }

    lisa->display_dirty = true;
}

/* ========================================================================
 * ProFile disk operations
 * ======================================================================== */

static void profile_read_block(lisa_t *lisa, uint32_t block) {
    if (!lisa->profile.mounted || !lisa->profile.data) {
        memset(lisa->profile.sector_buf, 0xFF, PROFILE_BLOCK_SIZE);
        return;
    }

    size_t offset = (size_t)block * PROFILE_BLOCK_SIZE;
    if (offset + PROFILE_BLOCK_SIZE <= lisa->profile.data_size) {
        memcpy(lisa->profile.sector_buf, lisa->profile.data + offset, PROFILE_BLOCK_SIZE);
    } else {
        memset(lisa->profile.sector_buf, 0, PROFILE_BLOCK_SIZE);
    }
    lisa->profile.buf_index = 0;
}

static void profile_write_block(lisa_t *lisa, uint32_t block) {
    if (!lisa->profile.mounted || !lisa->profile.data) return;

    size_t offset = (size_t)block * PROFILE_BLOCK_SIZE;
    if (offset + PROFILE_BLOCK_SIZE <= lisa->profile.data_size) {
        memcpy(lisa->profile.data + offset, lisa->profile.sector_buf, PROFILE_BLOCK_SIZE);
    }
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void lisa_init(lisa_t *lisa) {
    memset(lisa, 0, sizeof(lisa_t));
    g_lisa = lisa;

    /* Initialize components */
    m68k_init(&lisa->cpu);
    lisa_mem_init(&lisa->mem);
    via_init(&lisa->via1);
    via_init(&lisa->via2);
    cops_queue_init(&lisa->cops_rx);

    /* Wire CPU to memory */
    lisa->cpu.read8 = mem_read8_cb;
    lisa->cpu.read16 = mem_read16_cb;
    lisa->cpu.read32 = mem_read32_cb;
    lisa->cpu.write8 = mem_write8_cb;
    lisa->cpu.write16 = mem_write16_cb;
    lisa->cpu.write32 = mem_write32_cb;

    /* Wire memory I/O callbacks */
    lisa->mem.io_read = io_read_cb;
    lisa->mem.io_write = io_write_cb;

    /* Wire VIA1 (ProFile) */
    via_reset(&lisa->via1);
    lisa->via1.port_b_write = via1_portb_write;
    lisa->via1.port_b_read = via1_portb_read;
    lisa->via1.port_a_write = via1_porta_write;
    lisa->via1.port_a_read = via1_porta_read;
    lisa->via1.callback_ctx = lisa;
    lisa->via1.irq_callback = via1_irq;
    lisa->via1.irq_ctx = lisa;

    /* Wire VIA2 (Keyboard/COPS) */
    via_reset(&lisa->via2);
    lisa->via2.port_b_write = via2_portb_write;
    lisa->via2.port_b_read = via2_portb_read;
    lisa->via2.port_a_write = via2_porta_write;
    lisa->via2.port_a_read = via2_porta_read;
    lisa->via2.callback_ctx = lisa;
    lisa->via2.irq_callback = via2_irq;
    lisa->via2.irq_ctx = lisa;
}

void lisa_destroy(lisa_t *lisa) {
    if (lisa->profile.data) {
        free(lisa->profile.data);
        lisa->profile.data = NULL;
    }
    if (lisa->floppy.data) {
        free(lisa->floppy.data);
        lisa->floppy.data = NULL;
    }
    g_lisa = NULL;
}

void lisa_reset(lisa_t *lisa) {
    g_lisa = lisa;
    lisa->mem.setup_mode = true;  /* ROM visible at address 0 */
    via_reset(&lisa->via1);
    via_reset(&lisa->via2);

    /* Pre-load OS from ProFile disk image into RAM.
     * Read the disk catalog to find system.os, then load all its blocks.
     * Boot track (blocks 0-23) → RAM at $20000
     * system.os file data → RAM at $0 (where the linker placed it) */
    if (lisa->profile.mounted && lisa->profile.data) {
        /* First, load boot track at $20000 */
        uint32_t dest = 0x20000;
        int boot_blocks = 0;
        for (int blk = 0; blk < BOOT_TRACK_BLOCKS && dest + PROFILE_DATA_SIZE < LISA_RAM_SIZE; blk++) {
            size_t src_offset = (size_t)blk * PROFILE_BLOCK_SIZE;
            if (src_offset + PROFILE_BLOCK_SIZE <= lisa->profile.data_size) {
                memcpy(&lisa->mem.ram[dest], lisa->profile.data + src_offset + PROFILE_TAG_SIZE, PROFILE_DATA_SIZE);
                dest += PROFILE_DATA_SIZE;
                boot_blocks++;
            }
        }

        /* Read catalog to find system.os and load it at $0 */
        uint32_t cat_block = BOOT_TRACK_BLOCKS + 1;  /* Catalog is after MDDF */
        size_t cat_offset = (size_t)cat_block * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
        if (cat_offset + PROFILE_DATA_SIZE <= lisa->profile.data_size) {
            uint8_t *catalog = lisa->profile.data + cat_offset;
            /* First entry (offset 0) should be system.os */
            uint32_t file_size = ((uint32_t)catalog[34] << 24) | ((uint32_t)catalog[35] << 16) |
                                 ((uint32_t)catalog[36] << 8)  | (uint32_t)catalog[37];
            uint32_t start_block = ((uint32_t)catalog[38] << 8) | (uint32_t)catalog[39];
            uint32_t num_blocks = ((uint32_t)catalog[40] << 8) | (uint32_t)catalog[41];

            if (file_size > 0 && start_block > 0) {
                /* Load system.os into RAM at $0. */
                uint32_t ram_dest = 0;
                uint32_t loaded = 0;
                for (uint32_t b = 0; b < num_blocks && ram_dest + PROFILE_DATA_SIZE < LISA_RAM_SIZE; b++) {
                    size_t blk_offset = (size_t)(start_block + b) * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
                    if (blk_offset + PROFILE_DATA_SIZE <= lisa->profile.data_size) {
                        memcpy(&lisa->mem.ram[ram_dest], lisa->profile.data + blk_offset, PROFILE_DATA_SIZE);
                        ram_dest += PROFILE_DATA_SIZE;
                        loaded++;
                    }
                }
                printf("Pre-loaded system.os: %u blocks (%u bytes) at RAM $0, start_block=%u\n",
                       loaded, loaded * PROFILE_DATA_SIZE, start_block);
            }
        }

        printf("Pre-loaded %d boot blocks at RAM $20000\n", boot_blocks);

        /* Set up exception vectors in RAM $0-$3FF.
         * Code starts at $400, so vectors don't overlap.
         * Line-A/Line-F get skip handlers; others get plain RTE. */
        for (int v = 0; v < 256; v++) {
            uint32_t handler;
            if (v == 10)      handler = 0x00FE0320;  /* Line-A skip handler */
            else if (v == 11) handler = 0x00FE0310;  /* Line-F (SANE) skip handler */
            else              handler = 0x00FE0300;  /* Default RTE */
            int off = v * 4;
            if (off + 3 < 0x400) {
                lisa->mem.ram[off + 0] = (handler >> 24) & 0xFF;
                lisa->mem.ram[off + 1] = (handler >> 16) & 0xFF;
                lisa->mem.ram[off + 2] = (handler >> 8) & 0xFF;
                lisa->mem.ram[off + 3] = handler & 0xFF;
            }
        }
        /* Vector 0: Initial SSP */
        lisa->mem.ram[0] = 0x00; lisa->mem.ram[1] = 0x07;
        lisa->mem.ram[2] = 0x90; lisa->mem.ram[3] = 0x00;
        /* Vector 1: Initial PC (points to first code at $400) */
        lisa->mem.ram[4] = 0x00; lisa->mem.ram[5] = 0x00;
        lisa->mem.ram[6] = 0x04; lisa->mem.ram[7] = 0x00;
        printf("Set up 256 exception vectors in RAM $0-$3FF\n");
    }

    /* Debug: verify ROM is loaded before reset */
    printf("Reset: setup_mode=%d, rom[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
           lisa->mem.setup_mode,
           lisa->mem.rom[0], lisa->mem.rom[1], lisa->mem.rom[2], lisa->mem.rom[3],
           lisa->mem.rom[4], lisa->mem.rom[5], lisa->mem.rom[6], lisa->mem.rom[7]);

    m68k_reset(&lisa->cpu);

    printf("After reset: PC=$%08X SSP=$%08X\n", lisa->cpu.pc, lisa->cpu.ssp);

    /* Queue initial COPS data: keyboard ID so OS init can proceed */
    cops_queue_init(&lisa->cops_rx);
    cops_enqueue(&lisa->cops_rx, 0x80);  /* Reset/status indicator */
    cops_enqueue(&lisa->cops_rx, 0x2F);  /* Keyboard ID: US layout */

    lisa->running = true;
    lisa->power_on = true;
    lisa->frame_cycles = 0;
    lisa->total_frames = 0;
}

bool lisa_load_rom(lisa_t *lisa, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open ROM: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > LISA_ROM_SIZE * 2) {
        fprintf(stderr, "Invalid ROM size: %ld bytes\n", size);
        fclose(f);
        return false;
    }

    uint8_t *buf = malloc(size);
    if (!buf) { fclose(f); return false; }

    fread(buf, 1, size, f);
    fclose(f);

    lisa_mem_load_rom(&lisa->mem, buf, size);
    free(buf);

    printf("Loaded ROM: %s (%ld bytes)\n", path, size);
    return true;
}

bool lisa_mount_profile(lisa_t *lisa, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open ProFile image: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    lisa->profile.data = malloc(size);
    if (!lisa->profile.data) { fclose(f); return false; }

    fread(lisa->profile.data, 1, size, f);
    fclose(f);

    lisa->profile.data_size = size;
    lisa->profile.mounted = true;
    lisa->profile.state = 0;
    lisa->profile.buf_index = 0;
    lisa->profile.busy = false;

    printf("Mounted ProFile: %s (%ld bytes, %ld blocks)\n",
           path, size, size / PROFILE_BLOCK_SIZE);
    return true;
}

bool lisa_mount_floppy(lisa_t *lisa, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open floppy image: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    lisa->floppy.data = malloc(size);
    if (!lisa->floppy.data) { fclose(f); return false; }

    fread(lisa->floppy.data, 1, size, f);
    fclose(f);

    lisa->floppy.data_size = size;
    lisa->floppy.mounted = true;
    lisa->floppy.write_protect = false;

    printf("Mounted floppy: %s (%ld bytes)\n", path, size);
    return true;
}

int lisa_run_frame(lisa_t *lisa) {
    if (!lisa->running || !lisa->power_on)
        return 0;

    g_lisa = lisa;
    int cycles_this_frame = 0;
    int via_tick_accum = 0;

    /* Clear any stale vretrace IRQ from previous frame BEFORE executing */
    lisa->mem.vretrace_irq = false;
    lisa->irq_vretrace = 0;

    while (cycles_this_frame < LISA_CYCLES_PER_FRAME) {
        /* Execute a batch of CPU instructions */
        int batch = 64;
        int executed = m68k_execute(&lisa->cpu, batch);
        cycles_this_frame += executed;
        via_tick_accum += executed;

        /* Tick VIAs periodically */
        if (via_tick_accum >= 10) {
            via_tick(&lisa->via1, via_tick_accum);
            via_tick(&lisa->via2, via_tick_accum);
            via_tick_accum = 0;
        }
    }

    /* Vertical retrace: pulse the IRQ for one instruction only.
     * Set it, let CPU take it, immediately clear. */
    if (lisa->mem.vretrace_enabled) {
        lisa->mem.vretrace_irq = true;
        lisa->irq_vretrace = 1;
        int level = 1;
        if (lisa->irq_via2) level = 2;
        m68k_set_irq(&lisa->cpu, level);

        /* Execute one instruction — enough for the CPU to take the IRQ */
        m68k_execute(&lisa->cpu, 50);

        /* Immediately clear */
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;
        m68k_set_irq(&lisa->cpu, level);
    }

    /* Update display */
    render_framebuffer(lisa);

    lisa->total_frames++;
    return cycles_this_frame;
}

/* ========================================================================
 * Input handling
 * ======================================================================== */

/*
 * Lisa keyboard keycodes are sent via COPS as:
 *   Key down: keycode with bit 7 clear
 *   Key up:   keycode with bit 7 set
 */
void lisa_key_down(lisa_t *lisa, int keycode) {
    if (keycode < 0 || keycode > 127) return;
    lisa->keys_down[keycode] = true;
    cops_enqueue(&lisa->cops_rx, keycode & 0x7F);
    via_trigger_ca1(&lisa->via2);
}

void lisa_key_up(lisa_t *lisa, int keycode) {
    if (keycode < 0 || keycode > 127) return;
    lisa->keys_down[keycode] = false;
    cops_enqueue(&lisa->cops_rx, keycode | 0x80);
    via_trigger_ca1(&lisa->via2);
}

/*
 * Mouse movement: COPS sends delta packets
 * Format: 2 bytes - dx, dy (signed)
 */
void lisa_mouse_move(lisa_t *lisa, int dx, int dy) {
    if (dx == 0 && dy == 0) return;

    /* Clamp deltas to signed byte range */
    if (dx > 127) dx = 127;
    if (dx < -128) dx = -128;
    if (dy > 127) dy = 127;
    if (dy < -128) dy = -128;

    lisa->mouse_x += dx;
    lisa->mouse_y += dy;

    /* Clamp to screen bounds */
    if (lisa->mouse_x < 0) lisa->mouse_x = 0;
    if (lisa->mouse_x >= LISA_SCREEN_WIDTH) lisa->mouse_x = LISA_SCREEN_WIDTH - 1;
    if (lisa->mouse_y < 0) lisa->mouse_y = 0;
    if (lisa->mouse_y >= LISA_SCREEN_HEIGHT) lisa->mouse_y = LISA_SCREEN_HEIGHT - 1;

    /* Queue mouse delta packet */
    cops_enqueue(&lisa->cops_rx, (uint8_t)(int8_t)dx);
    cops_enqueue(&lisa->cops_rx, (uint8_t)(int8_t)dy);
    via_trigger_ca1(&lisa->via2);
}

void lisa_mouse_button(lisa_t *lisa, bool pressed) {
    lisa->mouse_button = pressed;
    /* Mouse button state is read through VIA2 port */
    if (pressed)
        lisa->via2.irb &= ~0x04;  /* Button down */
    else
        lisa->via2.irb |= 0x04;   /* Button up */
}

const uint32_t *lisa_get_framebuffer(lisa_t *lisa) {
    return lisa->framebuffer;
}

int lisa_get_screen_width(void) {
    return LISA_SCREEN_WIDTH;
}

int lisa_get_screen_height(void) {
    return LISA_SCREEN_HEIGHT;
}
