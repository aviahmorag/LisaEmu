/* intrinsic_lib.c — Minimal INTRINSIC.LIB and program OBJ generators for LisaEmu
 *
 * Generates a valid Lisa OBJ-format INTRINSIC.LIB file that
 * Setup_IUInfo can parse. Contains zero intrinsic units initially —
 * the Shell doesn't need any loaded units to BEGIN executing
 * (its `uses` imports resolve at link time, not at load time for
 * regular units). This lets the IU directory data segment be
 * created successfully so that Open_IUDirectory returns a valid
 * refnum during Make_Process.
 *
 * Lisa OBJ block format (from source-OBJIO.TEXT):
 *   4 bytes: [(headerByte + 1) << 24] | blockSize
 *   blockSize includes the 4-byte header itself.
 *   After the header: InvarSize bytes of invariant data,
 *   then (blockSize - 4 - InvarSize) bytes of variant data.
 *
 * Block types (headerByte values, signed):
 *   $80 = ModuleName (-128)
 *   $81 = EndBlock   (-127)
 *   $85 = CodeBlock  (-123)
 *   $99 = VersionCtrl(-103)
 *   $9A = SegmentTable(-102)
 *   $9B = UnitTable  (-101)
 *   $9C = SegLocation(-100)
 *   $9D = UnitLocation(-99)
 *   $9E = StringBlock (-98)
 *   $B2 = OSData     (-78)
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Write a 16-bit big-endian value */
static void put16(uint8_t *buf, int16_t v) {
    buf[0] = (uint8_t)((uint16_t)v >> 8);
    buf[1] = (uint8_t)((uint16_t)v);
}

/* Write a 32-bit big-endian value */
static void put32(uint8_t *buf, int32_t v) {
    buf[0] = (uint8_t)((uint32_t)v >> 24);
    buf[1] = (uint8_t)((uint32_t)v >> 16);
    buf[2] = (uint8_t)((uint32_t)v >> 8);
    buf[3] = (uint8_t)((uint32_t)v);
}

/* Build the 4-byte OBJ block header.
 * headerByte is the SIGNED byte (e.g. -103 for VersionCtrl).
 * blockSize is the TOTAL size of the block including this 4-byte header. */
static void put_block_header(uint8_t *buf, int8_t headerByte, int32_t blockSize) {
    /* The on-disk format packs (headerByte + 1) into the top byte and
     * blockSize into the lower 3 bytes:
     *   word = ((headerByte + 1) << 24) | (blockSize & 0x00FFFFFF)
     * GetObjInvar reverses: HeaderByte = BlockSize DIV $1000000 - 1
     *                       iBlockSize = BlockSize MOD $1000000 */
    uint32_t word = (uint32_t)(uint8_t)(headerByte + 1) << 24;
    word |= (uint32_t)(blockSize & 0x00FFFFFF);
    put32(buf, (int32_t)word);
}

/* Generate a minimal INTRINSIC.LIB.
 * Returns a malloc'd buffer; caller frees. *out_size set to byte count. */
uint8_t *build_intrinsic_lib(uint32_t *out_size) {
    /* We'll build into a temporary buffer. Max size is small. */
    uint8_t *buf = calloc(1, 4096);
    if (!buf) return NULL;
    uint32_t pos = 0;

    /* === Block 1: VersionCtrl (headerByte = $99 = -103) ===
     * iVersionCtrl = record
     *   sysNum, minSys, maxSys, Reserv1, Reserv2, Reserv3: longint;
     * end;
     * OR (via case):
     *   FileType: ObjFileType (byte);  -- IUDirectory = 1
     *   ILibNum: VersNum (byte);       -- 0
     *   Config: VersVal (4 bytes);
     *   ModLevel: ModVal (2 bytes);
     *   IntfTime, ImplTime: longint (4+4);
     *   MinConfig, MaxConfig: VersVal (4+4);
     * Total invariant size = 6 * 4 = 24 bytes.
     * No variants for VersionCtrl. */
    {
        int32_t invar_size = 24; /* 6 longints */
        int32_t block_size = 4 + invar_size;
        put_block_header(buf + pos, -103, block_size);
        pos += 4;

        /* iVersionCtrl fields:
         * FileType = IUDirectory = 1 (first byte of sysNum longint) */
        buf[pos] = 1;  /* FileType = IUDirectory */
        buf[pos + 1] = 0;  /* ILibNum = 0 */
        /* Config (VersVal): System=3, Release=1, Version=0, InterFLevel=0 */
        buf[pos + 2] = 3;  /* System */
        buf[pos + 3] = 1;  /* Release */
        buf[pos + 4] = 0;  /* Version */
        buf[pos + 5] = 0;  /* InterFLevel */
        /* ModLevel (ModVal): ImplLevel=0, FixLevel=0 */
        buf[pos + 6] = 0;
        buf[pos + 7] = 0;
        /* IntfTime: 0 */
        put32(buf + pos + 8, 0);
        /* ImplTime: 0 */
        put32(buf + pos + 12, 0);
        /* MinConfig: all zeros */
        put32(buf + pos + 16, 0);
        /* MaxConfig: all zeros */
        put32(buf + pos + 20, 0);
        pos += invar_size;
    }

    /* === Block 2: UnitLocation (headerByte = -99 = $9D) ===
     * iUnitLocation = record nUnits: integer; end;
     * InvarSize = SizeOf(iUnitLocation) = 2 bytes
     * Each variant is iUnitLVariant (14 bytes):
     *   UnitName: ObjName (8); UnitNumber: int2;
     *   FileNumber: int1; UnitType: int1; DataSize: int4
     *
     * For a minimal lib with 0 units: nUnits=0, no variants. */
    {
        int32_t invar_size = 2; /* nUnits: integer */
        int32_t block_size = 4 + invar_size; /* no variants */
        put_block_header(buf + pos, -99, block_size);
        pos += 4;
        put16(buf + pos, 0); /* nUnits = 0 */
        pos += invar_size;
    }

    /* === Block 3: SegLocation (headerByte = -100 = $9C) ===
     * iSegLocation = record nSegments: integer; end;
     * InvarSize = 2. Each variant is iSegLocVariant (28 bytes).
     * 0 segments. */
    {
        int32_t invar_size = 2;
        int32_t block_size = 4 + invar_size;
        put_block_header(buf + pos, -100, block_size);
        pos += 4;
        put16(buf + pos, 0); /* nSegments = 0 */
        pos += invar_size;
    }

    /* === Block 4: StringBlock (headerByte = -98 = $9E) ===
     * iStringBlock = record nStrings: integer; end;
     * InvarSize = 2. Variant type = UnknownVariant (size=1 byte each).
     * NrVariants = (blockSize - 4 - InvarSize) / 1.
     * Build_StringTable computes:
     *   sTable_size = NrVariants - nStrings * sizeof(iStringVariant)
     * With nStrings=0: sTable_size = NrVariants.
     * We want SBT_size = nStrings*SBT_eSize + sTable_size >= 1
     * so GetSpace doesn't get called with 0. Put 2 bytes of
     * "string table" padding (NrVariants=2, sTable_size=2). */
    {
        int32_t invar_size = 2;
        int32_t padding = 2;
        int32_t block_size = 4 + invar_size + padding;
        put_block_header(buf + pos, -98, block_size);
        pos += 4;
        put16(buf + pos, 0); /* nStrings = 0 */
        pos += invar_size;
        buf[pos++] = 0; /* padding byte 1 */
        buf[pos++] = 0; /* padding byte 2 */
    }

    /* === Block 5: CodeBlock (headerByte = -123 = $85) ===
     * iCodeBlock = record Addr: SegAddr (4 bytes); end;
     * InvarSize = 4. Variant data = the actual code bytes.
     *
     * This is the IU JSR handler. For our minimal stub, we emit
     * a single RTS ($4E75) — 2 bytes of code. This handler is
     * installed at the A-emulator trap vector ($28) by Setup_IUInfo.
     * With 0 units, it should never be called, but having valid
     * code prevents crashes if it is. */
    {
        int32_t invar_size = 4; /* Addr: SegAddr */
        int32_t code_size = 2; /* RTS */
        int32_t block_size = 4 + invar_size + code_size;
        put_block_header(buf + pos, -123, block_size);
        pos += 4;
        put32(buf + pos, 0); /* Addr = 0 (offset within segment) */
        pos += 4;
        /* RTS opcode */
        buf[pos++] = 0x4E;
        buf[pos++] = 0x75;
    }

    /* === Block 6: EndBlock (headerByte = -127 = $81) ===
     * iEndBlock = record CSize: LongInt; end;
     * InvarSize = 4. */
    {
        int32_t invar_size = 4;
        int32_t block_size = 4 + invar_size;
        put_block_header(buf + pos, -127, block_size);
        pos += 4;
        put32(buf + pos, 0); /* CSize = 0 */
        pos += 4;
    }

    *out_size = pos;
    fprintf(stderr, "INTRINSIC.LIB generated: %u bytes (0 units, 0 segments)\n", pos);
    return buf;
}
