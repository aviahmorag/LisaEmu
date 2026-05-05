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

/* Generate a minimal program OBJ file (e.g. SYSTEM.SHELL).
 *
 * The file must satisfy Get_ProgInfo (reads UnitTable + Executable blocks),
 * Build_JumpTable (reads JT variant data after Executable invariant),
 * and Load_Program (reads SegmentTable block + code segments via segDesc).
 *
 * Block sequence:
 *   1. UnitTable — nUnits=0, maxunit=0, one dummy 12-byte variant
 *   2. Executable — 40-byte invariant + JT variant data
 *   3. SegmentTable — nSegments=0 (shared segs), one dummy 20-byte variant
 *   4. CodeBlock — single private code segment containing a halt loop
 *
 * Returns a malloc'd buffer; caller frees. *out_size set to byte count. */
uint8_t *build_program_obj(uint32_t *out_size) {
    uint8_t *buf = calloc(1, 4096);
    if (!buf) return NULL;
    uint32_t pos = 0;

    /* === Block 1: UnitTable (headerByte = -101 = $9B) ===
     * iUnitTable = record nUnits, maxunit: integer; end;
     * InvarSize = 4. Variant = iUnitVariant (12 bytes each).
     * Get_ProgInfo's Do_UnitTable uses repeat...until which executes
     * at least once, so we need one dummy 12-byte variant even with
     * nUnits=0. */
    {
        int32_t invar_size = 4;
        int32_t variant_size = 12; /* one dummy iUnitVariant */
        int32_t block_size = 4 + invar_size + variant_size;
        put_block_header(buf + pos, -101, block_size);
        pos += 4;
        put16(buf + pos, 0); /* nUnits = 0 */
        put16(buf + pos + 2, 0); /* maxunit = 0 */
        pos += invar_size;
        /* dummy variant: ObjName(8) + UnitNumber(2) + UnitType(2) */
        memset(buf + pos, 0, 12);
        pos += 12;
    }

    /* The code segment: a simple infinite loop (BRA.S *-2).
     * We'll place it in the CodeBlock later. First compute its
     * file offset so segDesc.SegmentAddr can point to it. */
    uint8_t code[] = { 0x60, 0xFE }; /* BRA.S $-2 (infinite loop) */
    uint32_t code_size = sizeof(code);

    /* === Block 2: Executable (headerByte = -104 = $98) ===
     * iExecutable: 10 longints = 40 bytes invariant.
     * Variant = the jump table data (read by Build_JumpTable).
     *
     * JumpTable layout:
     *   numSegs: int2 = 1
     *   segDesc[1]: jtSegEntry (12 bytes)
     *     SegmentAddr: FileAddr (4) — byte offset to code in file
     *     SizePacked: int2 = 0 (not packed)
     *     SizeUnpacked: int2 = code_size
     *     MemLoc: MemAddr (4) = 0
     *   numProcs: int2 = 0
     *
     * Total JT size = 2 + 12 + 2 = 16 bytes.
     *
     * After Executable block: SegmentTable + CodeBlock.
     * We need to compute the SegmentAddr (offset to code bytes in file). */

    int32_t jt_size = 16;
    int32_t exec_invar_size = 40;
    int32_t exec_block_size = 4 + exec_invar_size + jt_size;

    /* Compute file layout to determine code byte offset:
     * Block 1 (UnitTable): 4 + 4 + 12 = 20 bytes
     * Block 2 (Executable): 4 + 40 + 16 = 60 bytes
     * Block 3 (SegmentTable): 4 + 2 + 20 = 26 bytes (see below)
     * Block 4 (CodeBlock): 4-byte header + 4-byte Addr + code bytes
     * Code bytes start at: 20 + 60 + 26 + 8 = 114 */
    uint32_t seg_table_block_size = 4 + 2 + 20; /* header + invar + 1 dummy variant */
    uint32_t code_file_offset = 20 + (uint32_t)exec_block_size +
                                seg_table_block_size + 4 + 4; /* past CodeBlock hdr + Addr */

    put_block_header(buf + pos, -104, exec_block_size);
    pos += 4;

    /* iExecutable invariant (40 bytes): */
    put32(buf + pos +  0, 0);           /* JTLaddr — logical addr (0) */
    put32(buf + pos +  4, jt_size);     /* JTSize — total JT bytes */
    put32(buf + pos +  8, 0);           /* DataSize — regular unit globals */
    put32(buf + pos + 12, 32);          /* MainSize — main program globals (min) */
    put32(buf + pos + 16, 0);           /* JTSegDelta */
    put32(buf + pos + 20, 0);           /* StkSegDelta */
    put32(buf + pos + 24, 4096);        /* DynStack — initial dynamic stack */
    put32(buf + pos + 28, 16384);       /* MaxStack — max total stack */
    put32(buf + pos + 32, 512);         /* MinHeap — initial heap */
    put32(buf + pos + 36, 4096);        /* MaxHeap — max heap */
    pos += exec_invar_size;

    /* JT variant data (16 bytes): */
    put16(buf + pos, 1);                /* numSegs = 1 (one private code seg) */
    pos += 2;
    /* segDesc[1]: jtSegEntry (12 bytes) */
    put32(buf + pos, (int32_t)code_file_offset); /* SegmentAddr */
    put16(buf + pos + 4, 0);            /* SizePacked = 0 (not packed) */
    put16(buf + pos + 6, (int16_t)code_size); /* SizeUnpacked = code bytes */
    put32(buf + pos + 8, 0);            /* MemLoc = 0 */
    pos += 12;
    put16(buf + pos, 0);                /* numProcs = 0 */
    pos += 2;

    /* === Block 3: SegmentTable (headerByte = -102 = $9A) ===
     * iSegmentTable = record nSegments: integer; end;
     * InvarSize = 2. Variant = iSegVariant (20 bytes each).
     * Build_SegList's repeat...until executes once with nSegments=0,
     * so we need one dummy 20-byte variant. */
    {
        int32_t invar_size = 2;
        int32_t variant_size = 20;
        int32_t block_size = 4 + invar_size + variant_size;
        put_block_header(buf + pos, -102, block_size);
        pos += 4;
        put16(buf + pos, 0); /* nSegments = 0 (no shared segments) */
        pos += invar_size;
        /* dummy variant: ObjName(8) + SegNumber(2) + Version1(4) + Version2(4) */
        memset(buf + pos, 0, 20);
        pos += 20;
    }

    /* === Block 4: CodeBlock (headerByte = -123 = $85) ===
     * Contains the actual machine code for private segment 1.
     * iCodeBlock = record Addr: SegAddr (4 bytes); end;
     * Variant = ObjectCode bytes. */
    {
        int32_t invar_size = 4;
        int32_t block_size = 4 + invar_size + (int32_t)code_size;
        put_block_header(buf + pos, -123, block_size);
        pos += 4;
        put32(buf + pos, 0); /* Addr = 0 (offset within segment) */
        pos += 4;
        /* Verify our offset calculation */
        if (pos != code_file_offset) {
            fprintf(stderr, "BUG: code_file_offset=%u but pos=%u\n",
                    code_file_offset, pos);
        }
        memcpy(buf + pos, code, code_size);
        pos += code_size;
    }

    *out_size = pos;
    fprintf(stderr, "SYSTEM.SHELL OBJ generated: %u bytes "
            "(1 private seg, %u bytes code at offset %u)\n",
            pos, code_size, code_file_offset);
    return buf;
}
