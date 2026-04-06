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

    /* I/O board type register ($FCC031) — determines Lisa model.
     * For Lisa 2/10 (Pepsi with ProFile): return < -96 (signed) */
    if (offset == 0xC031) {
        return 0x80;  /* -128 signed → enters Pepsi branch */
    }

    /* Internal disk type register ($FCC015) — 0=twiggy, nonzero=Sony/ProFile */
    if (offset == 0xC015) {
        return 0x01;  /* Non-zero → iob_pepsi (not iob_twiggy) */
    }

    /* Disk controller shared memory */
    if (offset < 0x2000) {
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
        lisa->mem.video_addr = lisa->mem.video_alt
            ? (LISA_RAM_SIZE - 0x8000 + LISA_SCREEN_BYTES)
            : (LISA_RAM_SIZE - 0x8000);
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
static uint8_t last_via1_orb = 0;
static bool last_via1_bsy = false;
static void via1_portb_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    (void)ddr;
    bool bsy_before = profile_bsy(&lisa->prof);
    profile_orb_write(&lisa->prof, val, last_via1_orb);
    bool bsy_after = profile_bsy(&lisa->prof);
    /* Detect BSY transitions → CA1 interrupt flag */
    if (bsy_after && !bsy_before) {
        /* BSY asserted (falling edge) → set CA1 flag */
        lisa->via1.ifr |= 0x02;
    }
    if (!bsy_after && bsy_before) {
        /* BSY deasserted (rising edge) → also set CA1 flag */
        lisa->via1.ifr |= 0x02;
    }
    last_via1_bsy = bsy_after;
    last_via1_orb = val;
}

static uint8_t via1_portb_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    uint8_t val = 0;

    if (lisa->prof.mounted) {
        val |= 0x01; /* OCD - connected */
        if (!profile_bsy(&lisa->prof))
            val |= 0x02; /* BSY - not busy (active low) */
    }

    return val;
}

static void via1_porta_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    (void)ddr;
    bool bsy_before = profile_bsy(&lisa->prof);
    profile_porta_write(&lisa->prof, val);
    bool bsy_after = profile_bsy(&lisa->prof);
    if (bsy_after != bsy_before) {
        lisa->via1.ifr |= 0x02;  /* CA1 transition → set flag */
    }
    last_via1_bsy = bsy_after;
}

static uint8_t via1_porta_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    return profile_porta_read(&lisa->prof);
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
    uint32_t os_loaded_bytes = 0;
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
                os_loaded_bytes = loaded * PROFILE_DATA_SIZE;
                printf("Pre-loaded system.os: %u blocks (%u bytes) at RAM $0, start_block=%u\n",
                       loaded, os_loaded_bytes, start_block);
            }
        }

        printf("Pre-loaded %d boot blocks at RAM $20000\n", boot_blocks);

        /* Dump PASCALINIT code (first 80 bytes) */
        fprintf(stderr, "PASCALINIT @$DFD38:\n");
        for (int di = 0; di < 40; di += 2) {
            uint32_t a = 0xDFD38 + di;
            fprintf(stderr, " $%04X", (lisa->mem.ram[a]<<8)|lisa->mem.ram[a+1]);
            if ((di % 20) == 18) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");

        /* Vector table ($0-$3FF) is pre-installed by the linker in system.os.
         * Only set Vector 0 (SSP) and Vector 1 (PC) for boot. */
        /* Vector 0: Initial SSP */
        lisa->mem.ram[0] = 0x00; lisa->mem.ram[1] = 0x07;
        lisa->mem.ram[2] = 0x90; lisa->mem.ram[3] = 0x00;
        /* Vector 1: Initial PC (points to first code at $400) */
        lisa->mem.ram[4] = 0x00; lisa->mem.ram[5] = 0x00;
        lisa->mem.ram[6] = 0x04; lisa->mem.ram[7] = 0x00;

        /* Log vector table state */
        printf("Vector table from linker: TRAP1[$84]=$%02X%02X%02X%02X TRAP7[$9C]=$%02X%02X%02X%02X\n",
               lisa->mem.ram[0x84], lisa->mem.ram[0x85], lisa->mem.ram[0x86], lisa->mem.ram[0x87],
               lisa->mem.ram[0x9C], lisa->mem.ram[0x9D], lisa->mem.ram[0x9E], lisa->mem.ram[0x9F]);

        /* Set up loader parameter block in low memory.
         * The real Lisa boot loader fills these before calling the OS.
         * From source/ldequ.text: */
        #define WRITE32(addr, val) do { \
            lisa->mem.ram[(addr)]   = ((val) >> 24) & 0xFF; \
            lisa->mem.ram[(addr)+1] = ((val) >> 16) & 0xFF; \
            lisa->mem.ram[(addr)+2] = ((val) >> 8)  & 0xFF; \
            lisa->mem.ram[(addr)+3] = (val) & 0xFF; \
        } while(0)
        #define WRITE16(addr, val) do { \
            lisa->mem.ram[(addr)]   = ((val) >> 8) & 0xFF; \
            lisa->mem.ram[(addr)+1] = (val) & 0xFF; \
        } while(0)

        /* Install minimal TRAP handlers for traps used before INIT_TRAPV.
         * TRAP #5 = TRAPTOHW (hardware interface) — used by %initstdio
         * TRAP #8 = mapiospace — used for I/O space mapping
         * Both need to be functional before the OS installs real handlers. */
        WRITE32(0x94, 0x00FE0330);  /* TRAP #5 → ROM TRAPTOHW handler */
        WRITE32(0xA0, 0x00FE0300);  /* TRAP #8 → ROM RTE handler */

        /* Override Line-A/Line-F vectors with safe ROM skip handlers.
         * The OS's LINE1111_TRAP handler calls system_error which isn't
         * initialized yet. INIT_TRAPV will install real handlers when ready. */
        WRITE32(0x28, 0x00FE0320);  /* Line-A → ROM skip handler */
        WRITE32(0x2C, 0x00FE0310);  /* Line-F → ROM skip handler */

        /* Fill ALL zero TRAP vectors ($80-$BC) with the ROM RTE handler.
         * TRAP #0-#15 map to vectors $80,$84,...$BC. The OS installs real
         * handlers via INIT_TRAPV during boot, but if any TRAP is called
         * before that (e.g. TRAP #6 for MMU, TRAP #7 for SR management),
         * a zero vector would jump to address 0 and crash the CPU. */
        for (uint32_t vec = 0x80; vec <= 0xBC; vec += 4) {
            uint32_t val = (lisa->mem.ram[vec] << 24) |
                           (lisa->mem.ram[vec+1] << 16) |
                           (lisa->mem.ram[vec+2] << 8) |
                            lisa->mem.ram[vec+3];
            if (val == 0) {
                WRITE32(vec, 0x00FE0300);
                printf("  [TRAP safety] Vector $%03X (TRAP #%u) was zero → installed ROM RTE handler\n",
                       vec, (vec - 0x80) / 4);
            }
        }

        /* %initstdio: no bypass — properly fixed via MOVEM register list
         * parsing and @-label local scoping in the assembler. */

        /* Debug: dump initio's BEQ.S @3 to verify @-label scoping */
        {
            uint16_t bra_disp = (lisa->mem.ram[0x402] << 8) | lisa->mem.ram[0x403];
            uint32_t body = 0x402 + (int16_t)bra_disp;
            uint32_t pi_addr = ((uint32_t)lisa->mem.ram[body+4] << 24) |
                               ((uint32_t)lisa->mem.ram[body+5] << 16) |
                               ((uint32_t)lisa->mem.ram[body+6] << 8) |
                               (uint32_t)lisa->mem.ram[body+7];
            /* JSR initstdio at PI+$36 */
            uint32_t istdio_jsr = pi_addr + 0x36;
            uint32_t istdio = ((uint32_t)lisa->mem.ram[istdio_jsr+2] << 24) |
                              ((uint32_t)lisa->mem.ram[istdio_jsr+3] << 16) |
                              ((uint32_t)lisa->mem.ram[istdio_jsr+4] << 8) |
                              (uint32_t)lisa->mem.ram[istdio_jsr+5];
            /* JSR initio at initstdio+4 (after MOVEM) */
            uint32_t initio_jsr = istdio + 4;
            uint32_t initio_addr = ((uint32_t)lisa->mem.ram[initio_jsr+2] << 24) |
                                   ((uint32_t)lisa->mem.ram[initio_jsr+3] << 16) |
                                   ((uint32_t)lisa->mem.ram[initio_jsr+4] << 8) |
                                   (uint32_t)lisa->mem.ram[initio_jsr+5];
            fprintf(stderr, "%%initstdio at $%06X, first 12 bytes:", istdio);
            for (int i = 0; i < 12; i++)
                fprintf(stderr, " %02X", lisa->mem.ram[istdio+i]);
            /* Check 4 bytes BEFORE initio addr for the missing move.l */
            fprintf(stderr, "\ninitio-4 bytes: %02X%02X %02X%02X",
                    lisa->mem.ram[initio_addr-4], lisa->mem.ram[initio_addr-3],
                    lisa->mem.ram[initio_addr-2], lisa->mem.ram[initio_addr-1]);
            fprintf(stderr, "\ninitio at $%06X, code:", initio_addr);
            for (int i = 0; i < 40; i += 2)
                fprintf(stderr, " %02X%02X", lisa->mem.ram[initio_addr+i], lisa->mem.ram[initio_addr+i+1]);
            fprintf(stderr, "\n");
            /* Find BEQ.S: should be at initio+$18 (24 bytes in) */
            for (int i = 0; i < 40; i += 2) {
                uint8_t hi = lisa->mem.ram[initio_addr+i];
                if (hi == 0x67) { /* BEQ.S */
                    uint8_t disp = lisa->mem.ram[initio_addr+i+1];
                    uint32_t target = initio_addr + i + 2 + (int8_t)disp;
                    fprintf(stderr, "  BEQ.S at +$%02X: disp=$%02X → target=$%06X\n",
                            i, disp, target);
                }
            }
        }

        /* Boot device and low-memory parameters */
        lisa->mem.ram[0x1B3] = 2;      /* adr_bootdev: 2 = parallel ProFile */

        /* Verify chan_select at $1F0 — must be 0 for screen console.
         * The linker output may have data at this offset from code/relocs. */
        {
            uint32_t cs = (lisa->mem.ram[0x1F0] << 24) | (lisa->mem.ram[0x1F1] << 16) |
                          (lisa->mem.ram[0x1F2] << 8) | lisa->mem.ram[0x1F3];
            if (cs != 0) {
                fprintf(stderr, "WARNING: chan_select at $1F0 = $%08X (expected 0), clearing\n", cs);
                WRITE32(0x1F0, 0);  /* Force screen console */
            }
        }
        WRITE32(0x2A4, 0);             /* adr_lowcore: physical byte 0 = 0 */

        /* Screen pointers (from LDEQU) */
        {
            uint32_t scr = LISA_RAM_SIZE - 0x8000;  /* $1F8000 */
            uint32_t alt = scr - 0x8000;            /* $1F0000 */
            WRITE32(0x110, scr);    /* prom_screen: main screen base */
            WRITE32(0x160, scr);    /* realscreenptr: mapped screen */
            WRITE32(0x170, alt);    /* altscreenptr: alternate screen */
            WRITE32(0x174, scr);    /* mainscreenptr: main screen */
        }

        /* Memory info */
        WRITE32(0x294, LISA_RAM_SIZE); /* prom_memsize: last byte + 1 */
        WRITE32(0x2A4, 0x00000000);   /* prom_byte0: physical byte 0 */
        WRITE32(0x2A8, LISA_RAM_SIZE); /* prom_realsize: amount of memory */

        /* Loader parameter block — OS reads this via PASCALINIT/GETLDMAP.
         * Layout from source-parms.text: version, then base/length pairs
         * for each memory region, then miscellaneous loader state.
         * GETLDMAP copies these into INITSYS local variables. */
        uint32_t os_end = os_loaded_bytes + 0x400; /* End of OS code */
        if (os_end & 0xFFF) os_end = (os_end + 0xFFF) & ~0xFFF; /* Page-align */

        /* Memory layout for 1MB Lisa:
         * $000000-$0003FF: Vector table
         * $000400-os_end:  OS code (system.os)
         * os_end-$054000:  System jump table + sysglobal
         * $054000-$064000: Sysglobal heap (64KB)
         * $064000-$068000: Supervisor stack (16KB)
         * $068000-$070000: Syslocal (32KB)
         * $070000-$078000: User stack for outer process
         * $078000-$07A000: Screen data area
         * $07A000-$0FF800: Screen buffer + free memory
         * $0FF800-$100000: Top of RAM */
        uint32_t b_sysjt     = os_end;
        uint32_t l_sysjt     = 0x1000;    /* 4KB jump table */
        uint32_t b_sysglobal = b_sysjt + l_sysjt;
        uint32_t l_sysglobal = 0x6000;    /* 24KB sysglobal */
        uint32_t b_superstack = b_sysglobal + l_sysglobal;
        uint32_t l_superstack = 0x4000;   /* 16KB supervisor stack */
        uint32_t b_sgheap    = b_superstack + l_superstack;
        uint32_t l_sgheap    = 0x8000;    /* 32KB sysglobal heap */
        /* Screen at top of 2MB RAM, like real Lisa */
        uint32_t l_screen    = 0x8000;    /* 32KB screen buffer */
        uint32_t l_dbscreen  = 0x8000;
        uint32_t b_screen    = LISA_RAM_SIZE - l_screen;  /* $1F8000 */
        uint32_t b_dbscreen  = b_screen - l_dbscreen;     /* $1F0000 */
        uint32_t b_syslocal  = b_sgheap + l_sgheap;
        uint32_t l_syslocal  = 0x4000;    /* 16KB syslocal */
        uint32_t b_opustack  = b_syslocal + l_syslocal;
        uint32_t l_opustack  = 0x4000;    /* 16KB user stack */
        uint32_t b_scrdata   = b_opustack + l_opustack;
        uint32_t l_scrdata   = 0x2000;    /* 8KB screen data */
        uint32_t b_vmbuffer  = b_scrdata + l_scrdata;
        uint32_t l_vmbuffer  = 0x4000;    /* 16KB VM buffer */
        uint32_t b_drivers   = b_vmbuffer + l_vmbuffer;
        uint32_t l_drivers   = 0x2000;    /* 8KB driver data */
        uint32_t lomem       = b_drivers + l_drivers;
        uint32_t himem       = 0x0FF800;

        /* Build parameter block — Lisa Pascal lays out variables DOWNWARD.
         * GETLDMAP copies word-by-word, decrementing both pointers.
         * adrparamptr points to `version` (highest address in block).
         * Subsequent fields are at DECREASING addresses below version.
         *
         * We place version at $A00 and write fields downward from there. */
        /* Place param block ABOVE OS code — the binary at $0 would overwrite
         * anything below os_end. Use os_end + $100 for the top of the block. */
        uint32_t version_addr = os_end + 0x100;
        if (version_addr & 1) version_addr++;  /* word-align */
        WRITE16(version_addr, 22);  /* version = 22 */

        /* Write fields downward: p starts just below version, decreases */
        uint32_t p = version_addr;
        #define W32D(val) do { p -= 4; WRITE32(p, (val)); } while(0)
        #define W16D(val) do { p -= 2; WRITE16(p, (val)); } while(0)

        W32D(b_sysjt);       /* b_sysjt */
        W32D(l_sysjt);       /* l_sysjt */
        W32D(b_sysglobal);   /* b_sys_global */
        W32D(l_sysglobal);   /* l_sys_global */
        W32D(b_superstack);  /* b_superstack */
        W32D(l_superstack);  /* l_superstack */
        W32D(b_sysglobal + l_sysglobal); /* b_intrin_ptrs */
        W32D(0x1000);        /* l_intrin_ptrs */
        W32D(b_sgheap);      /* b_sgheap */
        W32D(l_sgheap);      /* l_sgheap */
        W32D(b_screen);      /* b_screen */
        W32D(l_screen);      /* l_screen */
        W32D(b_dbscreen);    /* b_db_screen */
        W32D(l_dbscreen);    /* l_db_screen */
        W32D(b_syslocal);    /* b_opsyslocal */
        W32D(l_syslocal);    /* l_opsyslocal */
        W32D(b_opustack);    /* b_opustack */
        W32D(l_opustack);    /* l_opustack */
        W32D(b_scrdata);     /* b_scrdata */
        W32D(l_scrdata);     /* l_scrdata */
        W32D(b_vmbuffer);    /* b_vmbuffer */
        W32D(l_vmbuffer);    /* l_vmbuffer */
        W32D(b_drivers);     /* b_drivers */
        W32D(l_drivers);     /* l_drivers */
        W32D(himem);         /* himem */
        W32D(lomem);         /* lomem */
        W32D(LISA_RAM_SIZE); /* l_physicalmem */
        W16D(24);            /* fs_block0 */
        W16D(0);             /* debugmode = false */
        W32D(0x280);         /* smt_base */
        W16D(1);             /* os_segs = 1 */
        W32D(0);             /* ld_sernum */
        /* b_oscode[1..32] */
        for (int seg = 1; seg <= 48; seg++)
            W32D((seg == 1) ? 0x400 : 0);
        /* l_oscode[1..32] */
        for (int seg = 1; seg <= 48; seg++)
            W32D((seg == 1) ? (os_end - 0x400) : 0);
        /* swappedin[1..32] */
        for (int seg = 1; seg <= 48; seg++)
            W16D((seg == 1) ? 1 : 0);
        W32D(0);             /* b_debugseg */
        W32D(0);             /* l_debugseg */
        W32D(0);             /* unpktaddr */
        W16D(0);             /* have_lisabug = false */
        W16D(0);             /* two_screens = false */
        W32D(b_sysglobal + 32); /* ldr_A5 */
        W32D(lomem);         /* toplomem */
        W32D(himem);         /* bothimem */
        W16D(0xFFFF);        /* parmend sentinel */

        #undef W32D
        #undef W16D

        /* Low-memory system pointers (PASCALDEFS.TEXT EQU definitions) */
        WRITE32(0x200, b_sysglobal);   /* SGLOBAL/B_SYSGLOBAL: ptr to sysglobal base */
        WRITE32(0x204, 0);             /* loader_link: ptr to loader boot drivers */
        WRITE32(0x208, 0);             /* C_DOMAIN_PTR: ptr to current domain cell */

        /* Low-memory pointers for PASCALINIT */
        WRITE32(0x218, version_addr);  /* adrparamptr → version */
        WRITE32(0x21C, 0x00020000); /* ldbaseptr: loader base */
        /* DRIVRJT ($210) — Driver Jump Table pointer.
         * The OS reads this 32-bit pointer and calls through it.
         * Point to a block of RTS instructions in unused vector table area.
         * Each entry is 4 bytes (just RTS, returns cleanly). */
        {
            uint32_t djt_base = 0x300;  /* Put driver JT at $300 in vector table */
            for (int j = 0; j < 32; j++) {
                /* Each entry: RTS ($4E75) padded to 4 bytes */
                lisa->mem.ram[djt_base + j * 4]     = 0x4E;
                lisa->mem.ram[djt_base + j * 4 + 1] = 0x75;
                lisa->mem.ram[djt_base + j * 4 + 2] = 0x4E;
                lisa->mem.ram[djt_base + j * 4 + 3] = 0x71; /* NOP */
            }
            WRITE32(0x210, djt_base);
        }
        WRITE16(0x22E, 1);         /* dev_type: profile */

        /* esysgloboff (offset 28 from param_block) points to end of sysglobal.
         * PASCALINIT uses this to set up A5 relocation. */

        printf("Loader params: param_block=$%X-%X, os_end=$%X, sysglobal=$%X-%X\n",
               p, version_addr, os_end, b_sysglobal, b_sysglobal + l_sysglobal);

        /* Pre-program OS-critical MMU segments in context 1.
         * The OS uses logical addresses computed as seg_num * $20000.
         * These segments need to map to the physical data areas.
         * From MMPRIM.TEXT: kernelmmu=17, realmemmmu=85, sysglobmmu=102 */
        {
            /* Helper: program an MMU segment directly in our data structures */
            #define SET_MMU_SEG(ctx, seg, slr_val, sor_val) do { \
                lisa->mem.segments[ctx][seg].slr = (slr_val); \
                lisa->mem.segments[ctx][seg].sor = (sor_val); \
                lisa->mem.segments[ctx][seg].changed = 3; \
            } while(0)

            int ctx = 1;  /* Normal context */

            /* sysglobmmu (102): maps $CC0000 → physical sysglobal */
            SET_MMU_SEG(ctx, 102, 0x0700, (uint16_t)(b_sysglobal >> 9));

            /* syslocmmu (103): maps $CE0000 → physical syslocal */
            uint32_t b_syslocal = b_sgheap + l_sgheap;
            uint32_t l_syslocal = 0x4000;
            SET_MMU_SEG(ctx, 103, 0x0700, (uint16_t)(b_syslocal >> 9));

            /* superstkmmu (101): maps $CA0000 → physical supervisor stack */
            SET_MMU_SEG(ctx, 101, 0x0600, (uint16_t)(b_superstack >> 9));

            /* stackmmu (123): maps $F60000 → physical user stack + jump table */
            uint32_t b_stack = b_syslocal + l_syslocal;
            uint32_t b_opustack = b_stack;
            SET_MMU_SEG(ctx, 123, 0x0600, (uint16_t)(b_opustack >> 9));

            /* Also map segment 104 for legacy compatibility */
            SET_MMU_SEG(ctx, 104, 0x0600, (uint16_t)(b_stack >> 9));

            /* screenmmu (105): maps $D20000 → physical screen */
            SET_MMU_SEG(ctx, 105, 0x0700, (uint16_t)(b_screen >> 9));

            /* realmemmmu (85-100): identity map first 2MB as real memory */
            for (int s = 85; s <= 100; s++) {
                SET_MMU_SEG(ctx, s, 0x0700, (uint16_t)((s - 85) * 256));
            }

            /* kernelmmu (17-48): map OS code segments to physical code */
            /* OS code is at physical $400-$6A000. Each segment is 128KB.
             * Map segment 17 → physical $0, segment 18 → $20000, etc. */
            for (int s = 17; s <= 20; s++) {
                SET_MMU_SEG(ctx, s, 0x0500, (uint16_t)((s - 17) * 256)); /* read-only */
            }

            /* Also map segments in context 0 (start mode) for safety */
            for (int s = 0; s < 128; s++) {
                if (lisa->mem.segments[1][s].changed) {
                    lisa->mem.segments[0][s] = lisa->mem.segments[1][s];
                }
            }

            fprintf(stderr, "MMU: pre-programmed sysglobmmu(102)=$%03X, realmemmmu(85-100), kernelmmu(17-20)\n",
                    lisa->mem.segments[1][102].sor);
            /* Note: MMU translation is not active yet (setup mode).
             * After the boot ROM exits setup mode, segment 102 will
             * map $CC0000 → $E2000 for POOL_INIT and GETSPACE. */

            #undef SET_MMU_SEG
        }
        /* Verify RAM at $4EC matches linker output */
        printf("RAM at $4E8-$4FF: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
               lisa->mem.ram[0x4E8], lisa->mem.ram[0x4E9], lisa->mem.ram[0x4EA], lisa->mem.ram[0x4EB],
               lisa->mem.ram[0x4EC], lisa->mem.ram[0x4ED], lisa->mem.ram[0x4EE], lisa->mem.ram[0x4EF],
               lisa->mem.ram[0x4F0], lisa->mem.ram[0x4F1], lisa->mem.ram[0x4F2], lisa->mem.ram[0x4F3]);

        #undef WRITE32
        #undef WRITE16
    }

    /* Debug: verify ROM is loaded before reset */
    printf("Reset: setup_mode=%d, rom[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
           lisa->mem.setup_mode,
           lisa->mem.rom[0], lisa->mem.rom[1], lisa->mem.rom[2], lisa->mem.rom[3],
           lisa->mem.rom[4], lisa->mem.rom[5], lisa->mem.rom[6], lisa->mem.rom[7]);

    m68k_reset(&lisa->cpu);

    /* Set A5 to initial global data pointer (loader normally does this).
     * PASCALINIT in starasm1 expects A5 to point to the end of the
     * user-stack global area, which it copies into sysglobal. */
    lisa->cpu.a[5] = 0x14000;  /* Point into sysglobal area */

    printf("After reset: PC=$%08X SSP=$%08X A5=$%08X\n",
           lisa->cpu.pc, lisa->cpu.ssp, lisa->cpu.a[5]);

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

    /* Also mount on the new protocol-accurate ProFile */
    profile_init(&lisa->prof);
    profile_mount(&lisa->prof, lisa->profile.data, size);

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

    /* Debug: log CPU/VIA/vector state once after significant execution */
    static int frame_count = 0;
    frame_count++;
    /* Don't force-unmask interrupts or generate vretrace during init.
     * The OS must complete INITSYS before interrupt handlers are ready.
     * INTSON(0) at the end of BOOT_IO_INIT enables interrupts naturally. */
    if (frame_count == 10 || frame_count == 60 || frame_count == 120 || frame_count == 300 ||
        frame_count == 500 || frame_count == 800) {
        fprintf(stderr, "DIAG frame %d: PC=$%06X SR=$%04X stopped=%d pending_irq=%d setup=%d\n",
                frame_count, lisa->cpu.pc, lisa->cpu.sr, lisa->cpu.stopped,
                lisa->cpu.pending_irq, lisa->mem.setup_mode);
        fprintf(stderr, "  VIA1: t1_run=%d t1_cnt=%d t1_latch=%d ier=$%02X ifr=$%02X\n",
                lisa->via1.t1_running, lisa->via1.t1_counter, lisa->via1.t1_latch,
                lisa->via1.ier, lisa->via1.ifr);
        fprintf(stderr, "  VIA2: t1_run=%d t1_cnt=%d t1_latch=%d ier=$%02X ifr=$%02X\n",
                lisa->via2.t1_running, lisa->via2.t1_counter, lisa->via2.t1_latch,
                lisa->via2.ier, lisa->via2.ifr);
        /* Test MMU write at runtime */
        if (frame_count == 10) {
            /* Write $AB to logical $CC0000, check physical $E2000 */
            lisa_mem_write8(&lisa->mem, 0xCC0000, 0xAB);
            uint8_t got = lisa->mem.ram[0xE2000];
            fprintf(stderr, "  MMU WRITE TEST: wrote $AB to $CC0000, phys $E2000 = $%02X (%s)\n",
                    got, got == 0xAB ? "PASS" : "FAIL");
            lisa_mem_write8(&lisa->mem, 0xCC0000, 0);  /* clean up */
        }
        if (frame_count == 60) {
            /* Main body is at $400 + body_offset. BRA.W at $400 jumps there. */
            uint16_t bra_disp = (lisa->mem.ram[0x402] << 8) | lisa->mem.ram[0x403];
            uint32_t body = 0x402 + (int16_t)bra_disp;
            fprintf(stderr, "Main body at $%06X:", body);
            for (uint32_t a = body; a < body + 24; a += 2)
                fprintf(stderr, " %02X%02X", lisa->mem.ram[a], lisa->mem.ram[a+1]);
            fprintf(stderr, "\n");
        }
        if (frame_count == 120) {
            fprintf(stderr, "  A5=$%08X A6=$%08X SP=$%08X\n",
                    lisa->cpu.a[5], lisa->cpu.a[6], lisa->cpu.a[7]);
            /* Check b_sysglobal_ptr at $200 */
            uint32_t bsg = lisa_mem_read32(&lisa->mem, 0x200);
            fprintf(stderr, "  b_sysglobal_ptr(@$200)=$%08X\n", bsg);
            /* Check if sysglobal has been written (first 16 bytes) */
            fprintf(stderr, "  sysglobal@$CC0000:");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %02X", lisa_mem_read8(&lisa->mem, 0xCC0000 + i));
            fprintf(stderr, "\n  physical@$E2000:");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %02X", lisa->mem.ram[0xE2000 + i]);
            fprintf(stderr, "\n");
        }
        if (frame_count == 120) {
            uint32_t test_addr = 0xCC0000;
            uint32_t phys = 0;
            int seg = (test_addr >> 17) & 0x7F;
            int ctx = lisa->mem.current_context;
            fprintf(stderr, "  MMU state: enabled=%d ctx=%d seg102: sor=$%03X slr=$%03X changed=%d\n",
                    lisa->mem.mmu_enabled, ctx,
                    lisa->mem.segments[ctx][102].sor,
                    lisa->mem.segments[ctx][102].slr,
                    lisa->mem.segments[ctx][102].changed);
            uint8_t val = lisa_mem_read8(&lisa->mem, test_addr);
            fprintf(stderr, "  MMU read $CC0000 = $%02X (phys $E2000 = $%02X)\n",
                    val, lisa->mem.ram[0xE2000]);
        }
        /* Check screen content */
        {
            uint32_t saddr = lisa->mem.video_addr;
            int nz = 0;
            for (int i = 0; i < LISA_SCREEN_BYTES && saddr + i < LISA_RAM_SIZE; i++)
                if (lisa->mem.ram[saddr + i] != 0x00) nz++;
            fprintf(stderr, "  Screen @$%06X: %d/%d non-zero\n", saddr, nz, LISA_SCREEN_BYTES);
        }
        /* Check key exception vectors in RAM */
        uint32_t trap1_vec = ((uint32_t)lisa->mem.ram[0x84] << 24) |
                             ((uint32_t)lisa->mem.ram[0x85] << 16) |
                             ((uint32_t)lisa->mem.ram[0x86] << 8) |
                             lisa->mem.ram[0x87];
        uint32_t trap2_vec = ((uint32_t)lisa->mem.ram[0x88] << 24) |
                             ((uint32_t)lisa->mem.ram[0x89] << 16) |
                             ((uint32_t)lisa->mem.ram[0x8A] << 8) |
                             lisa->mem.ram[0x8B];
        uint32_t int1_vec = ((uint32_t)lisa->mem.ram[0x64] << 24) |
                            ((uint32_t)lisa->mem.ram[0x65] << 16) |
                            ((uint32_t)lisa->mem.ram[0x66] << 8) |
                            lisa->mem.ram[0x67];
        fprintf(stderr, "  Vectors (RAM): TRAP1=$%08X TRAP2=$%08X INT1=$%08X\n",
                trap1_vec, trap2_vec, int1_vec);
        /* Also read via CPU path to compare */
        uint32_t trap1_cpu = lisa_mem_read32(&lisa->mem, 0x84);
        fprintf(stderr, "  Vectors (CPU): TRAP1=$%08X  SGLOBAL@$200=$%08X\n",
                trap1_cpu,
                lisa_mem_read32(&lisa->mem, 0x200));
    }

    /* Vertical retrace: pulse the IRQ for one instruction only.
     * Only enable after the OS has initialized interrupt handlers
     * (frame_count > 200 gives INITSYS time to complete). */
    if (lisa->mem.vretrace_enabled && frame_count > 200) {
        lisa->mem.vretrace_irq = true;
        lisa->irq_vretrace = 1;
        int level = 3;  /* vretrace at level 3 to break through mask level 2 */
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
