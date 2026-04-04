/*
 * LisaEm - C to Swift Bridge Implementation
 */

#include "lisa_bridge.h"
#include "lisa.h"
#include <stdio.h>
#include <string.h>

static lisa_t lisa;
static bool initialized = false;

void emu_init(void) {
    if (initialized) return;
    lisa_init(&lisa);
    initialized = true;
}

void emu_destroy(void) {
    if (!initialized) return;
    lisa_destroy(&lisa);
    initialized = false;
}

void emu_reset(void) {
    if (!initialized) return;
    lisa_reset(&lisa);
}

bool emu_load_rom(const char *path) {
    if (!initialized) return false;
    return lisa_load_rom(&lisa, path);
}

bool emu_mount_profile(const char *path) {
    if (!initialized) return false;
    return lisa_mount_profile(&lisa, path);
}

bool emu_mount_floppy(const char *path) {
    if (!initialized) return false;
    return lisa_mount_floppy(&lisa, path);
}

int emu_run_frame(void) {
    if (!initialized) return 0;
    return lisa_run_frame(&lisa);
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
