/*
 * LisaEm - C to Swift Bridge Implementation
 */

#include "lisa_bridge.h"
#include "lisa.h"
#include "boot_progress.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lisa_t lisa;
static bool initialized = false;
/* Set to true by a successful emu_load_rom(), cleared by emu_init()
 * (which runs on every Power On teardown+re-init cycle). Can't probe
 * the ROM bytes for "is it loaded" — a valid Lisa boot ROM has reset
 * vectors like SSP=$000FFFFE / PC=$00FE0400 whose FIRST bytes ARE zero
 * (24-bit address space ⇒ top byte of every pointer is $00). */
static bool rom_loaded = false;

void emu_init(void) {
    if (initialized) return;
    lisa_init(&lisa);
    rom_loaded = false;
    initialized = true;
}

void emu_destroy(void) {
    if (!initialized) return;
    lisa_destroy(&lisa);
    rom_loaded = false;
    initialized = false;
}

bool emu_has_rom(void) {
    return initialized && rom_loaded;
}

void emu_reset(void) {
    if (!initialized) return;
    /* No auto-generated stub fallback. If the caller didn't load a real
     * boot ROM, that's a caller bug — fail loudly instead of silently
     * booting against a bare stub (which bypasses the symbol-pinning /
     * patch wiring that toolchain_bridge applies around bootrom_generate
     * during a real build). */
    if (!emu_has_rom()) {
        fprintf(stderr, "emu_reset: refused — no ROM loaded. Load a ROM before Power On.\n");
        return;
    }
    lisa_reset(&lisa);
}

bool emu_load_rom(const char *path) {
    if (!initialized) return false;
    bool ok = lisa_load_rom(&lisa, path);
    if (ok) rom_loaded = true;
    return ok;
}

bool emu_mount_profile(const char *path) {
    if (!initialized) return false;
    return lisa_mount_profile(&lisa, path);
}

/* Parse hle_addrs.txt (one "LABEL 0xHEX" per line) and wire the values
 * into lisa->hle. Without this, the kernel's CALLDRIVER / SYSTEM_ERROR /
 * loader-trap intercepts are dead and the CPU falls off during boot.
 * Returns true on success. */
bool emu_load_hle_addrs(const char *path) {
    if (!initialized) return false;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "emu_load_hle_addrs: cannot open %s\n", path);
        return false;
    }
    uint32_t calldriver = 0, call_hdisk = 0, hdiskio = 0, prodriver = 0;
    uint32_t system_error = 0, badcall = 0, parallel = 0, use_hdisk = 0;
    char label[64];
    uint32_t value;
    while (fscanf(f, "%63s 0x%X", label, &value) == 2) {
        if      (strcmp(label, "CALLDRIVER")   == 0) calldriver   = value;
        else if (strcmp(label, "CALL_HDISK")   == 0) call_hdisk   = value;
        else if (strcmp(label, "HDISKIO")      == 0) hdiskio      = value;
        else if (strcmp(label, "PRODRIVER")    == 0) prodriver    = value;
        else if (strcmp(label, "SYSTEM_ERROR") == 0) system_error = value;
        else if (strcmp(label, "BADCALL")      == 0) badcall      = value;
        else if (strcmp(label, "PARALLEL")     == 0) parallel     = value;
        else if (strcmp(label, "USE_HDISK")    == 0) use_hdisk    = value;
    }
    fclose(f);
    lisa_hle_set_addresses(&lisa, calldriver, call_hdisk, hdiskio, prodriver,
                           system_error, badcall, parallel, use_hdisk);
    fprintf(stderr, "HLE wired from %s: CALLDRIVER=$%X SYSTEM_ERROR=$%X\n",
            path, calldriver, system_error);
    return (calldriver != 0 || system_error != 0);
}

/* Load the linker symbol map so boot_progress_record_pc() can resolve
 * entry points, AND so the dynamic HLE lookups that query
 * boot_progress_lookup() (e.g. CreateProcess, Make_SProcess) find their
 * target addresses. The SDL harness calls this too — without it, several
 * emulated kernel code paths fall back to hardcoded addresses or fail
 * silently, and the boot crashes mid-sequence. */
bool emu_load_symbol_map(const char *path) {
    if (!initialized) return false;
    bool ok = boot_progress_init(path);
    if (!ok) fprintf(stderr, "emu_load_symbol_map: boot_progress_init(%s) failed\n", path);
    return ok;
}

bool emu_mount_floppy(const char *path) {
    if (!initialized) return false;
    return lisa_mount_floppy(&lisa, path);
}

int emu_run_frame(void) {
    if (!initialized) return 0;
    if (lisa.halted) return 0;
    return lisa_run_frame(&lisa);
}

bool emu_is_halted(void) {
    return initialized && lisa.halted;
}

/* Copy the boot-progress report into a caller-provided buffer. Returns
 * the number of bytes written (including NUL). Uses open_memstream so
 * boot_progress_report's FILE-based API works against memory directly. */
int emu_get_boot_progress_report(char *buf, int bufsize) {
    if (!buf || bufsize <= 0) return 0;
    char *mem = NULL;
    size_t mem_size = 0;
    FILE *f = open_memstream(&mem, &mem_size);
    if (!f) { buf[0] = '\0'; return 1; }
    boot_progress_report(f);
    fclose(f);  /* flushes into mem/mem_size */
    int to_copy = (int)mem_size < bufsize - 1 ? (int)mem_size : bufsize - 1;
    if (mem) {
        memcpy(buf, mem, to_copy);
        free(mem);
    }
    buf[to_copy] = '\0';
    return to_copy + 1;
}

const uint32_t *emu_get_framebuffer(void) {
    if (!initialized) return NULL;
    return lisa_get_framebuffer(&lisa);
}

int emu_screen_width(void) {
    return lisa_get_screen_width();
}

int emu_screen_height(void) {
    return lisa_get_screen_height();
}

void emu_key_down(int keycode) {
    if (!initialized) return;
    lisa_key_down(&lisa, keycode);
}

void emu_key_up(int keycode) {
    if (!initialized) return;
    lisa_key_up(&lisa, keycode);
}

void emu_mouse_move(int dx, int dy) {
    if (!initialized) return;
    lisa_mouse_move(&lisa, dx, dy);
}

void emu_mouse_button(bool pressed) {
    if (!initialized) return;
    lisa_mouse_button(&lisa, pressed);
}

bool emu_is_running(void) {
    if (!initialized) return false;
    return lisa.running;
}

void emu_set_running(bool running) {
    if (!initialized) return;
    lisa.running = running;
}

void emu_get_cpu_state(char *buf, int bufsize) {
    if (!initialized || !buf || bufsize < 1) return;

    snprintf(buf, bufsize,
        "PC=%06X  SR=%04X  %s\n"
        "D0=%08X D1=%08X D2=%08X D3=%08X\n"
        "D4=%08X D5=%08X D6=%08X D7=%08X\n"
        "A0=%08X A1=%08X A2=%08X A3=%08X\n"
        "A4=%08X A5=%08X A6=%08X A7=%08X\n"
        "USP=%08X SSP=%08X  Cycles=%d",
        lisa.cpu.pc, lisa.cpu.sr,
        (lisa.cpu.sr & 0x2000) ? "S" : "U",
        lisa.cpu.d[0], lisa.cpu.d[1], lisa.cpu.d[2], lisa.cpu.d[3],
        lisa.cpu.d[4], lisa.cpu.d[5], lisa.cpu.d[6], lisa.cpu.d[7],
        lisa.cpu.a[0], lisa.cpu.a[1], lisa.cpu.a[2], lisa.cpu.a[3],
        lisa.cpu.a[4], lisa.cpu.a[5], lisa.cpu.a[6], lisa.cpu.a[7],
        lisa.cpu.usp, lisa.cpu.ssp, lisa.cpu.total_cycles);
}
