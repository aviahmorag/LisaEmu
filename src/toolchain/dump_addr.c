/*
 * dump_addr - Dump bytes at a given address from the linked kernel in a disk image
 *
 * Usage: dump_addr <image_path> <hex_address>
 * Example: dump_addr build/lisa_profile.image 3015C
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define BLOCK_SIZE  532
#define TAG_SIZE    20
#define DATA_SIZE   512
#define BOOT_TRACK_BLOCKS 24

/* 68000 instruction decoder (basic) */
static void decode_instruction(const uint8_t *mem, uint32_t pc) {
    uint16_t opcode = (mem[0] << 8) | mem[1];
    printf("  Opcode: $%04X\n", opcode);

    if (opcode == 0x4E72) {
        uint16_t imm = (mem[2] << 8) | mem[3];
        printf("  STOP #$%04X (wait for interrupt, IPL mask=%d)\n", imm, (imm >> 8) & 7);
    } else if ((opcode & 0xFFC0) == 0x0800) {
        uint16_t bit = (mem[2] << 8) | mem[3];
        int mode = (opcode >> 3) & 7;
        int reg = opcode & 7;
        printf("  BTST #%d,", bit & 31);
        if (mode == 0) printf("D%d\n", reg);
        else if (mode == 7 && reg == 1) {
            uint32_t addr = (mem[4] << 24) | (mem[5] << 16) | (mem[6] << 8) | mem[7];
            printf("$%08X\n", addr);
        } else if (mode == 7 && reg == 0) {
            uint16_t addr = (mem[4] << 8) | mem[5];
            printf("$%04X\n", addr);
        } else printf("(mode=%d reg=%d)\n", mode, reg);
    } else if ((opcode & 0xF1C0) == 0x0100) {
        int dreg = (opcode >> 9) & 7;
        int mode = (opcode >> 3) & 7;
        int reg = opcode & 7;
        printf("  BTST D%d,", dreg);
        if (mode == 0) printf("D%d\n", reg);
        else if (mode == 2) printf("(A%d)\n", reg);
        else if (mode == 5) {
            int16_t disp = (int16_t)((mem[2] << 8) | mem[3]);
            printf("%d(A%d)\n", disp, reg);
        } else printf("(mode=%d reg=%d)\n", mode, reg);
    } else if ((opcode & 0xFF00) == 0x4A00) {
        int size = (opcode >> 6) & 3;
        int mode = (opcode >> 3) & 7;
        int reg = opcode & 7;
        const char *sz[] = {"B","W","L"};
        printf("  TST.%s ", sz[size]);
        if (mode == 0) printf("D%d\n", reg);
        else if (mode == 2) printf("(A%d)\n", reg);
        else if (mode == 5) {
            int16_t disp = (int16_t)((mem[2] << 8) | mem[3]);
            printf("%d(A%d)\n", disp, reg);
        } else if (mode == 7 && reg == 0) {
            uint16_t addr = (mem[2] << 8) | mem[3];
            printf("$%04X.W\n", addr);
        } else if (mode == 7 && reg == 1) {
            uint32_t addr = (mem[2] << 24) | (mem[3] << 16) | (mem[4] << 8) | mem[5];
            printf("$%08X.L\n", addr);
        } else printf("(mode=%d reg=%d)\n", mode, reg);
    } else if ((opcode & 0xF000) == 0x6000) {
        int cond = (opcode >> 8) & 0xF;
        const char *cc[] = {"BRA","BSR","BHI","BLS","BCC","BCS","BNE","BEQ",
                            "BVC","BVS","BPL","BMI","BGE","BLT","BGT","BLE"};
        int8_t disp8 = opcode & 0xFF;
        if (disp8 == 0) {
            int16_t disp16 = (int16_t)((mem[2] << 8) | mem[3]);
            printf("  %s.W $%06X (disp=%d)\n", cc[cond], pc + 2 + disp16, disp16);
        } else {
            printf("  %s.S $%06X (disp=%d)\n", cc[cond], pc + 2 + disp8, disp8);
        }
    } else if (opcode == 0x4E75) {
        printf("  RTS\n");
    } else if (opcode == 0x4E73) {
        printf("  RTE\n");
    } else if ((opcode & 0xFFF8) == 0x4E50) {
        int16_t disp = (int16_t)((mem[2] << 8) | mem[3]);
        printf("  LINK A%d,#%d\n", opcode & 7, disp);
    } else if ((opcode & 0xF100) == 0x7000) {
        int reg = (opcode >> 9) & 7;
        int8_t val = opcode & 0xFF;
        printf("  MOVEQ #%d,D%d\n", val, reg);
    } else if ((opcode & 0xC000) == 0x0000 && (opcode & 0xF000) != 0x0000) {
        printf("  (MOVE or other - check 68000 manual)\n");
    } else {
        printf("  (decode manually - check 68000 manual)\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image_path> <hex_address>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    uint32_t target_addr = (uint32_t)strtoul(argv[2], NULL, 16);

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *image = malloc(fsize);
    fread(image, 1, fsize, f);
    fclose(f);

    /* Read catalog at block 25 to find system.os */
    uint32_t cat_off = 25 * BLOCK_SIZE + TAG_SIZE;
    uint32_t file_size = (image[cat_off+34] << 24) | (image[cat_off+35] << 16) |
                         (image[cat_off+36] << 8) | image[cat_off+37];
    uint32_t start_block = (image[cat_off+38] << 8) | image[cat_off+39];
    uint32_t num_blocks = (image[cat_off+40] << 8) | image[cat_off+41];

    printf("system.os: size=%u start_block=%u num_blocks=%u\n", file_size, start_block, num_blocks);

    /* Function to read a byte at a given OS address from the image */
    #define READ_BYTE(addr) ({ \
        uint32_t _a = (addr); \
        uint32_t _blk = _a / DATA_SIZE; \
        uint32_t _byte = _a % DATA_SIZE; \
        uint32_t _img_off = (start_block + _blk) * BLOCK_SIZE + TAG_SIZE + _byte; \
        (_img_off < (uint32_t)fsize) ? image[_img_off] : 0; \
    })

    printf("\nBytes at $%06X-16 .. $%06X+32:\n", target_addr, target_addr);
    uint32_t start = (target_addr > 32) ? target_addr - 32 : 0;
    uint32_t end = target_addr + 48;

    /* Print as disassembly-style hex dump */
    for (uint32_t a = start; a < end; a += 2) {
        uint8_t hi = READ_BYTE(a);
        uint8_t lo = READ_BYTE(a+1);
        const char *marker = (a == target_addr) ? " <<< PC" : "";
        printf("  $%06X: %02X%02X%s\n", a, hi, lo, marker);
    }

    /* Decode instruction at target address */
    printf("\nInstruction decode at $%06X:\n", target_addr);
    uint8_t instr_bytes[16];
    for (int i = 0; i < 16; i++)
        instr_bytes[i] = READ_BYTE(target_addr + i);
    decode_instruction(instr_bytes, target_addr);

    /* Also try decoding a few instructions before and after */
    printf("\nSequential decode starting at $%06X:\n", target_addr - 8);
    for (uint32_t a = target_addr - 8; a < target_addr + 16; ) {
        uint8_t ib[16];
        for (int i = 0; i < 16; i++)
            ib[i] = READ_BYTE(a + i);
        uint16_t op = (ib[0] << 8) | ib[1];
        printf("$%06X: $%04X  ", a, op);
        decode_instruction(ib, a);
        /* Advance by instruction size (approximate) */
        if (op == 0x4E72 || op == 0x4E50 || (op & 0xFFF8) == 0x4E50)
            a += 4;
        else if ((op & 0xF000) == 0x6000 && (op & 0xFF) == 0)
            a += 4;
        else if ((op & 0xFFC0) == 0x0800)
            a += 6; /* BTST #n has bit number + ea extension */
        else
            a += 2;
    }

    /* Identify which module contains this address */
    printf("\nNote: Run the emulator with these diagnostics to see the module map.\n");

    free(image);
    return 0;
}
