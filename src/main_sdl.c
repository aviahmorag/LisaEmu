/*
 * LisaEm - Standalone SDL2 frontend
 * For testing outside Xcode
 *
 * Usage: lisaemu [rom_path] [profile_image] [floppy_image]
 */

#include "lisa.h"
#include "toolchain/toolchain_bridge.h"
#include "toolchain/bootrom.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define WINDOW_SCALE 2

static lisa_t lisa;

/* Map SDL scancodes to Lisa keycodes */
static int sdl_to_lisa_key(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_A: return 0x42;
        case SDL_SCANCODE_B: return 0x5A;
        case SDL_SCANCODE_C: return 0x6A;
        case SDL_SCANCODE_D: return 0x57;
        case SDL_SCANCODE_E: return 0x32;
        case SDL_SCANCODE_F: return 0x58;
        case SDL_SCANCODE_G: return 0x59;
        case SDL_SCANCODE_H: return 0x44;
        case SDL_SCANCODE_I: return 0x45;
        case SDL_SCANCODE_J: return 0x5C;
        case SDL_SCANCODE_K: return 0x5D;
        case SDL_SCANCODE_L: return 0x5B;
        case SDL_SCANCODE_M: return 0x5F;
        case SDL_SCANCODE_N: return 0x5E;
        case SDL_SCANCODE_O: return 0x36;
        case SDL_SCANCODE_P: return 0x46;
        case SDL_SCANCODE_Q: return 0x30;
        case SDL_SCANCODE_R: return 0x33;
        case SDL_SCANCODE_S: return 0x56;
        case SDL_SCANCODE_T: return 0x34;
        case SDL_SCANCODE_U: return 0x43;
        case SDL_SCANCODE_V: return 0x6B;
        case SDL_SCANCODE_W: return 0x31;
        case SDL_SCANCODE_X: return 0x69;
        case SDL_SCANCODE_Y: return 0x35;
        case SDL_SCANCODE_Z: return 0x68;

        case SDL_SCANCODE_1: return 0x20;
        case SDL_SCANCODE_2: return 0x21;
        case SDL_SCANCODE_3: return 0x22;
        case SDL_SCANCODE_4: return 0x23;
        case SDL_SCANCODE_5: return 0x24;
        case SDL_SCANCODE_6: return 0x25;
        case SDL_SCANCODE_7: return 0x27;
        case SDL_SCANCODE_8: return 0x2A;
        case SDL_SCANCODE_9: return 0x28;
        case SDL_SCANCODE_0: return 0x2B;

        case SDL_SCANCODE_RETURN:    return 0x48;
        case SDL_SCANCODE_ESCAPE:    return 0x00;
        case SDL_SCANCODE_BACKSPACE: return 0x2D;
        case SDL_SCANCODE_TAB:       return 0x30;
        case SDL_SCANCODE_SPACE:     return 0x6E;

        case SDL_SCANCODE_LSHIFT: return 0x70;
        case SDL_SCANCODE_RSHIFT: return 0x71;
        case SDL_SCANCODE_LALT:   return 0x72;
        case SDL_SCANCODE_LGUI:   return 0x74;

        case SDL_SCANCODE_LEFT:  return 0x01;
        case SDL_SCANCODE_RIGHT: return 0x02;
        case SDL_SCANCODE_DOWN:  return 0x03;
        case SDL_SCANCODE_UP:    return 0x04;

        default: return -1;
    }
}

int main(int argc, char *argv[]) {
    printf("LisaEm - Apple Lisa Emulator\n");
    printf("720x364 monochrome display, Motorola 68000 @ 5MHz\n\n");

    lisa_init(&lisa);

    if (argc < 2) {
        printf("Usage: %s <Lisa_Source_dir>    (build from source)\n", argv[0]);
        printf("       %s --image <profile_image>  (boot pre-built disk image)\n", argv[0]);
        printf("       %s <rom_file> [profile_image] [floppy_image]\n", argv[0]);
        return 1;
    }

    /* Check for --image flag: boot directly from a pre-built ProFile image */
    if (argc >= 3 && strcmp(argv[1], "--image") == 0) {
        printf("Booting from pre-built disk image: %s\n", argv[2]);

        /* Generate boot ROM in memory and load it directly */
        {
            uint8_t *rom = bootrom_generate();
            if (rom) {
                lisa_mem_load_rom(&lisa.mem, rom, ROM_SIZE);
                free(rom);
                printf("Boot ROM generated (%d bytes)\n", ROM_SIZE);
            } else {
                fprintf(stderr, "Failed to generate boot ROM\n");
                return 1;
            }
        }

        /* Mount the pre-built disk image */
        lisa_mount_profile(&lisa, argv[2]);

        /* No HLE addresses — the pre-built OS has unknown symbol locations.
         * We'll detect SYSTEM_ERROR by exception vector pattern later. */
        fprintf(stderr, "HLE: disabled for pre-built image (no symbol table)\n");
    } else if (toolchain_validate_source(argv[1])) {
        /* Build from source */
        printf("Building from source: %s\n", argv[1]);
        build_result_t br = toolchain_build(argv[1], "build", NULL);
        if (!br.success || br.files_compiled == 0) {
            fprintf(stderr, "Toolchain build failed: %s\n", br.error_message);
            return 1;
        }
        printf("Build OK: %d compiled, %d assembled, %d linked\n",
               br.files_compiled, br.files_assembled, br.files_linked);

        /* Load generated ROM */
        if (!lisa_load_rom(&lisa, "build/lisa_boot.rom")) {
            fprintf(stderr, "Failed to load generated ROM\n");
            return 1;
        }
        /* Mount generated disk image */
        lisa_mount_profile(&lisa, "build/lisa_profile.image");

        /* Load HLE addresses from toolchain globals (set during linking) */
        if (hle_addr_calldriver || hle_addr_system_error) {
            lisa_hle_set_addresses(&lisa, hle_addr_calldriver, 0,
                                   0, 0,
                                   hle_addr_system_error, 0, 0, 0);
            fprintf(stderr, "HLE: CALLDRIVER=$%X SYSTEM_ERROR=$%X\n",
                    hle_addr_calldriver, hle_addr_system_error);
        } else {
            fprintf(stderr, "HLE: no addresses found, HLE disabled\n");
        }
    } else {
        /* Legacy: treat as ROM file */
        if (!lisa_load_rom(&lisa, argv[1])) {
            fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
            return 1;
        }
        if (argc > 2) lisa_mount_profile(&lisa, argv[2]);
        if (argc > 3) lisa_mount_floppy(&lisa, argv[3]);
    }

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int win_w = LISA_SCREEN_WIDTH * WINDOW_SCALE;
    int win_h = LISA_SCREEN_HEIGHT * WINDOW_SCALE;

    SDL_Window *window = SDL_CreateWindow(
        "LisaEm - Apple Lisa",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        LISA_SCREEN_WIDTH, LISA_SCREEN_HEIGHT);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Set logical size to maintain aspect ratio */
    SDL_RenderSetLogicalSize(renderer, LISA_SCREEN_WIDTH, LISA_SCREEN_HEIGHT);

    /* Reset and start */
    lisa_reset(&lisa);

    bool running = true;
    bool mouse_captured = false;
    uint32_t frame_start;

    while (running) {
        frame_start = SDL_GetTicks();

        /* Handle events */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE && mouse_captured) {
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                        mouse_captured = false;
                        break;
                    }
                    {
                        int lk = sdl_to_lisa_key(event.key.keysym.scancode);
                        if (lk >= 0) lisa_key_down(&lisa, lk);
                    }
                    break;

                case SDL_KEYUP: {
                    int lk = sdl_to_lisa_key(event.key.keysym.scancode);
                    if (lk >= 0) lisa_key_up(&lisa, lk);
                    break;
                }

                case SDL_MOUSEBUTTONDOWN:
                    if (!mouse_captured) {
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                        mouse_captured = true;
                    }
                    if (event.button.button == SDL_BUTTON_LEFT)
                        lisa_mouse_button(&lisa, true);
                    break;

                case SDL_MOUSEBUTTONUP:
                    if (event.button.button == SDL_BUTTON_LEFT)
                        lisa_mouse_button(&lisa, false);
                    break;

                case SDL_MOUSEMOTION:
                    if (mouse_captured)
                        lisa_mouse_move(&lisa, event.motion.xrel, event.motion.yrel);
                    break;
            }
        }

        /* Run one frame of emulation */
        lisa_run_frame(&lisa);

        /* Update display */
        const uint32_t *fb = lisa_get_framebuffer(&lisa);
        SDL_UpdateTexture(texture, NULL, fb, LISA_SCREEN_WIDTH * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Frame timing (~60fps) */
        uint32_t elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < 16) {
            SDL_Delay(16 - elapsed);
        }
    }

    /* Cleanup */
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    lisa_destroy(&lisa);
    printf("Goodbye.\n");
    return 0;
}
