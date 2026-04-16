/*
 * LisaEm - Apple Lisa Emulator
 * Main machine integration
 */

#include "lisa.h"
#include "boot_progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 68000 24-bit address masking */
static inline uint32_t mask_24(uint32_t addr) { return addr & 0x00FFFFFF; }

/* Read helpers for CPU memory (used by HLE) */
static inline uint32_t cpu_read32(m68k_t *cpu, uint32_t addr) {
    return cpu->read32(mask_24(addr));
}
static inline uint16_t cpu_read16(m68k_t *cpu, uint32_t addr) {
    return cpu->read16(mask_24(addr));
}

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
 * Loader Filesystem — C reimplementation of the Lisa OS loader's LDLFS unit.
 * Provides file open/read/fill/move operations for the OS boot process.
 * Reads directly from the ProFile disk image in memory.
 * ======================================================================== */

#define LDR_MAP_MAX 1024

typedef struct {
    int32_t address;    /* Absolute page on disk */
    int16_t cpages;     /* Number of contiguous pages */
} ldr_mapentry_t;

typedef struct {
    bool initialized;
    int32_t fs_block0;      /* Disk block where MDDF is physically located */
    int32_t geo_firstblock; /* geo.firstblock from MDDF — used for page→block mapping */
    int16_t data_size;
    int32_t slist_addr;
    int16_t slist_packing;
    int16_t slist_block_count;
    int16_t map_offset;
    int16_t smallmap_offset;
    int16_t catentries;
    int16_t fs_version;
    int32_t root_page;
    int16_t tree_depth;
    int32_t rootsnum;
    int16_t first_file;
    int16_t empty_file;
    int16_t maxfiles;
    int16_t leader_offset;
    int16_t leader_pages;
    int16_t flabel_offset;
    int16_t hintsize;

    bool file_open;
    ldr_mapentry_t filemap[LDR_MAP_MAX];
    int32_t last_page;
    int32_t curr_page;
    int16_t next_byte;
    uint8_t buf[512];
} ldr_fs_t;

static ldr_fs_t ldr_fs = { .initialized = false };

static bool ldr_read_disk_block(lisa_t *lisa, uint32_t block, uint8_t *dst) {
    if (!lisa->profile.mounted || !lisa->profile.data) return false;
    size_t offset = (size_t)block * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
    if (offset + 512 > lisa->profile.data_size) return false;
    memcpy(dst, lisa->profile.data + offset, 512);
    return true;
}

static bool ldr_read_disk_tag(lisa_t *lisa, uint32_t block, uint8_t *dst) {
    if (!lisa->profile.mounted || !lisa->profile.data) return false;
    size_t offset = (size_t)block * PROFILE_BLOCK_SIZE;
    if (offset + PROFILE_TAG_SIZE > lisa->profile.data_size) return false;
    memcpy(dst, lisa->profile.data + offset, PROFILE_TAG_SIZE);
    return true;
}

static int16_t ldr_get16(const uint8_t *p) {
    return (int16_t)((p[0] << 8) | p[1]);
}
static int32_t ldr_get32(const uint8_t *p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3]);
}

static bool ldr_fs_init(lisa_t *lisa) {
    if (ldr_fs.initialized) return true;
    memset(&ldr_fs, 0, sizeof(ldr_fs));

    /* Scan for MDDF: look for fsversion = 14, 15, or 17 (valid Lisa OS versions).
     * The MDDF can be at various locations depending on disk format:
     * - Block 24 for our cross-compiled images (24-block boot track)
     * - Block 46 for real Lisa OS disk images (larger boot track)
     * - Block 30 for some Twiggy-converted images
     * We also verify the volname is a valid Pascal string. */
    uint8_t mddf_buf[512];
    bool found = false;
    uint32_t max_scan = 100;
    if (lisa->profile.data_size / PROFILE_BLOCK_SIZE < max_scan)
        max_scan = (uint32_t)(lisa->profile.data_size / PROFILE_BLOCK_SIZE);

    for (uint32_t blk = 0; blk < max_scan; blk++) {
        if (!ldr_read_disk_block(lisa, blk, mddf_buf)) continue;
        int16_t fsver = ldr_get16(mddf_buf);
        if (fsver != 14 && fsver != 15 && fsver != 17) continue;

        /* Verify: volname at offset 12 must be a valid Pascal string[32] */
        uint8_t namelen = mddf_buf[12];
        if (namelen == 0 || namelen > 32) continue;
        bool valid_name = true;
        for (int i = 0; i < namelen; i++) {
            uint8_t c = mddf_buf[13 + i];
            if (c < 0x20 || c > 0x7E) { valid_name = false; break; }
        }
        if (!valid_name) continue;

        /* Verify: datasize should be 512.
         * At offset 126 in correct layout (real images with 34-byte strings).
         * At offset 124 in our cross-compiled images (33-byte strings, 2 off). */
        int16_t ds126 = ldr_get16(mddf_buf + 126);
        int16_t ds124 = ldr_get16(mddf_buf + 124);
        if (ds126 != 512 && ds124 != 512) continue;

        ldr_fs.fs_block0 = (int32_t)blk;
        found = true;
        char vname[33] = {0};
        memcpy(vname, mddf_buf + 13, namelen);
        fprintf(stderr, "LDR_FS: MDDF at block %u, fsversion=%d, volume=\"%s\"\n",
                blk, fsver, vname);
        break;
    }
    if (!found) { fprintf(stderr, "LDR_FS: no MDDF found\n"); return false; }

    /* Parse MDDF fields per SOURCE-VMSTUFF.TEXT MDDFdb layout.
     *
     * IMPORTANT: Lisa Pascal string[32] = 34 bytes in records (33 + 1 pad
     * for word alignment). Lisa Pascal boolean = 1 byte in records, with
     * consecutive booleans packed and padded at end to word boundary.
     *
     * Field offsets (verified against real "AOS 3.0" disk image):
     *   0: fsversion (2)        10: volnum (2)
     *   2: volid.a (4)          12: volname (34 = 1 len + 32 chars + 1 pad)
     *   6: volid.b (4)          46: password (34)
     *  80: init_machine_id (4)  84: master_machine_id (4)
     *  88: DT_created (4)       92: DT_copy_created (4)
     *  96: DT_copied (4)       100: DT_scavenged (4)
     * 104: copy_thread (4)
     * 108: geo.firstblock (4)  112: geo.lastblock (4)  116: geo.lastfspage (4)
     * 120: blockcount (4)      124: blocksize (2)      126: datasize (2)
     * 128: cluster_size (2)    130: MDDFaddr (4)       134: MDDFsize (2)
     * 136: bitmap_addr (4)     140: bitmap_size (4)
     * 144: bitmap_bytes (2)    146: bitmap_pages (2)
     * 148: slist_addr (4)      152: slist_packing (2)  154: slist_block_count (2)
     * 156: first_file (2)      158: empty_file (2)     160: maxfiles (2)
     * 162: hintsize (2)        164: leader_offset (2)  166: leader_pages (2)
     * 168: flabel_offset (2)   170: unusedi1 (2)
     * 172: map_offset (2)      174: map_size (2)
     * 176: filecount (2)       178: freestart (4)
     * 182: unusedl1 (4)        186: freecount (4)
     * 190: rootsnum (2)        192: rootmaxentries (2)
     * 194: mountinfo (2)       196: overmount_stamp (8)
     * 204: pmem_id (4)         208: pmem (66)
     * 274: vol_scavenged (1)   275: tbt_copied (1)   [+pad to 276]
     * 276: smallmap_offset (2) 278: hentry_offset (2)
     * 280: backup_volid (8)    288: flabel_size (2)
     * 290: fs_overhead (2)     292: result_scavenge (2)
     * 294: boot_code (2)       296: boot_environ (2)
     * 298: oem_id (4)          302: root_page (4)
     * 306: tree_depth (2)      308: node_id (2)
     * 310: vol_seq_no (2)      312: vol_mounted (2)
     */
    /* Detect layout variant: real images have datasize=512 at offset 126,
     * our cross-compiled images have it at offset 124 (2-byte shift from
     * using 33-byte strings instead of 34-byte word-aligned strings). */
    int adj = 0;
    if (ldr_get16(mddf_buf + 126) == 512) {
        adj = 0;  /* Correct layout (real Lisa OS images) */
        fprintf(stderr, "LDR_FS: using correct MDDF layout (34-byte strings)\n");
    } else if (ldr_get16(mddf_buf + 124) == 512) {
        adj = -2; /* Our cross-compiled layout (33-byte strings, 2 bytes early) */
        fprintf(stderr, "LDR_FS: using cross-compiled MDDF layout (33-byte strings, adj=-2)\n");
    }

    ldr_fs.fs_version = ldr_get16(mddf_buf + 0);
    ldr_fs.geo_firstblock = ldr_get32(mddf_buf + 108 + adj);
    ldr_fs.data_size = ldr_get16(mddf_buf + 126 + adj);
    if (ldr_fs.data_size <= 0 || ldr_fs.data_size > 512) ldr_fs.data_size = 512;
    ldr_fs.slist_addr = ldr_get32(mddf_buf + 148 + adj);
    ldr_fs.slist_packing = ldr_get16(mddf_buf + 152 + adj);
    ldr_fs.slist_block_count = ldr_get16(mddf_buf + 154 + adj);
    ldr_fs.first_file = ldr_get16(mddf_buf + 156 + adj);
    ldr_fs.empty_file = ldr_get16(mddf_buf + 158 + adj);
    ldr_fs.maxfiles = ldr_get16(mddf_buf + 160 + adj);
    ldr_fs.hintsize = ldr_get16(mddf_buf + 162 + adj);
    ldr_fs.leader_offset = ldr_get16(mddf_buf + 164 + adj);
    ldr_fs.leader_pages = ldr_get16(mddf_buf + 166 + adj);
    ldr_fs.flabel_offset = ldr_get16(mddf_buf + 168 + adj);
    ldr_fs.map_offset = ldr_get16(mddf_buf + 172 + adj);
    ldr_fs.catentries = ldr_get16(mddf_buf + 192 + adj);
    ldr_fs.rootsnum = ldr_get16(mddf_buf + 190 + adj);
    /* Fields after booleans at 274-275 shift by 2 vs naive 2-byte-bool layout.
     * For correct layout: smallmap_offset at 276, root_page at 302, tree_depth at 306.
     * For cross-compiled (adj=-2): smallmap_offset at 274, root_page at 300, etc. */
    ldr_fs.smallmap_offset = ldr_get16(mddf_buf + 276 + adj);
    ldr_fs.root_page = ldr_get32(mddf_buf + 302 + adj);
    ldr_fs.tree_depth = ldr_get16(mddf_buf + 306 + adj);

    fprintf(stderr, "LDR_FS: geo.firstblock=%d datasize=%d slist=%d packing=%d slist_blks=%d\n"
            "LDR_FS: map_off=%d smallmap_off=%d catentries=%d rootsnum=%d\n"
            "LDR_FS: root_page=%d depth=%d first_file=%d empty_file=%d maxfiles=%d\n"
            "LDR_FS: leader_off=%d leader_pages=%d hintsize=%d flabel_off=%d\n",
            (int)ldr_fs.geo_firstblock, ldr_fs.data_size,
            (int)ldr_fs.slist_addr, ldr_fs.slist_packing, ldr_fs.slist_block_count,
            ldr_fs.map_offset, ldr_fs.smallmap_offset, ldr_fs.catentries, ldr_fs.rootsnum,
            (int)ldr_fs.root_page, ldr_fs.tree_depth,
            ldr_fs.first_file, ldr_fs.empty_file, ldr_fs.maxfiles,
            ldr_fs.leader_offset, ldr_fs.leader_pages, ldr_fs.hintsize,
            ldr_fs.flabel_offset);

    ldr_fs.initialized = true;
    ldr_fs.file_open = false;
    ldr_fs.curr_page = -1;
    ldr_fs.next_byte = ldr_fs.data_size;
    return true;
}

static bool ldr_read_page(lisa_t *lisa, int32_t page, uint8_t *dst) {
    /* For real Lisa OS images, geo.firstblock provides the page→block mapping.
     * For our cross-compiled images, fs_block0 (the MDDF's disk block) serves
     * the same role since we place the MDDF at page 0 = block BOOT_TRACK_BLOCKS.
     * Real images may have the MDDF at a different disk block than geo.firstblock
     * (e.g., MDDF at block 46 but geo.firstblock=30 for Twiggy-converted images). */
    int32_t block0 = ldr_fs.geo_firstblock;
    /* Fallback: if geo_firstblock is 0 or unreasonable, use MDDF block position */
    if (block0 <= 0 || block0 > ldr_fs.fs_block0)
        block0 = ldr_fs.fs_block0;
    return ldr_read_disk_block(lisa, block0 + page, dst);
}

static bool ldr_find_sentry(lisa_t *lisa, int16_t sfile_num,
                             int32_t *hint_addr, int32_t *file_size) {
    /* s_entry format (14 bytes per entry):
     *   hintaddr:  longint (4)  — fs page of hint/leader page
     *   fileaddr:  longint (4)  — first allocated data page
     *   filesize:  longint (4)  — file size in bytes
     *   version:   integer (2)  — version number
     *
     * slist_packing entries per 512-byte block.
     * Entry N is at slist page N/packing, byte offset (N%packing)*14 */
    if (ldr_fs.slist_packing <= 0) return false;
    int32_t spage = (sfile_num / ldr_fs.slist_packing) + ldr_fs.slist_addr;
    int soffset = (sfile_num % ldr_fs.slist_packing) * 14;
    uint8_t sbuf[512];
    if (!ldr_read_page(lisa, spage, sbuf)) return false;
    if (soffset + 14 > 512) return false;
    *hint_addr = ldr_get32(sbuf + soffset);       /* hintaddr at +0 */
    *file_size = ldr_get32(sbuf + soffset + 8);   /* filesize at +8 */
    return true;
}

static bool ldr_build_filemap(lisa_t *lisa, int32_t hint_addr, int32_t file_size) {
    for (int i = 0; i < LDR_MAP_MAX; i++) {
        ldr_fs.filemap[i].address = 0;
        ldr_fs.filemap[i].cpages = 0;
    }
    int bufsize = ldr_fs.data_size > 0 ? ldr_fs.data_size : 512;
    ldr_fs.last_page = file_size > 0 ? (file_size - 1) / bufsize : 0;
    ldr_fs.curr_page = -1;
    ldr_fs.next_byte = bufsize;

    int32_t map_page = hint_addr + ldr_fs.map_offset;
    int fmap_offset = ldr_fs.smallmap_offset;
    int ld_index = 0;
    int32_t abs_page = 0;

    for (int safety = 0; safety < 100; safety++) {
        if (map_page <= 0) break;
        uint8_t map_buf[512], tag_buf[24];
        if (!ldr_read_page(lisa, map_page, map_buf)) break;
        /* Read tag for fwdlink — use same block0 as ldr_read_page */
        int32_t block0 = ldr_fs.geo_firstblock;
        if (block0 <= 0 || block0 > ldr_fs.fs_block0)
            block0 = ldr_fs.fs_block0;
        ldr_read_disk_tag(lisa, block0 + map_page, tag_buf);

        int16_t max_entries = ldr_get16(map_buf + fmap_offset + 4);
        uint8_t *entries = map_buf + fmap_offset + 8;

        for (int i = 0; i <= max_entries && ld_index < LDR_MAP_MAX; i++) {
            int eo = i * 6;
            if (fmap_offset + 8 + eo + 6 > 512) break;
            int32_t addr = ldr_get32(entries + eo);
            int16_t cpg = ldr_get16(entries + eo + 4);
            if (cpg <= 0) break;
            ldr_fs.filemap[ld_index].address = addr;
            ldr_fs.filemap[ld_index].cpages = cpg;
            abs_page += cpg;
            ld_index++;
            if (abs_page > ldr_fs.last_page) goto fm_done;
        }
        /* fwdlink is int4 at offset 16 in pagelabel/tag */
        int32_t fwdlink = ldr_get32(tag_buf + 16);
        if (fwdlink <= 0) break;
        map_page = fwdlink;
        fmap_offset = 0;
    }
fm_done:
    fprintf(stderr, "LDR_FS: filemap %d entries, last_page=%d\n",
            ld_index, (int)ldr_fs.last_page);
    return ld_index > 0;
}

static bool ldr_find_position(int32_t page, int32_t *blk, int32_t *len) {
    int32_t ap = 0;
    for (int i = 0; i < LDR_MAP_MAX; i++) {
        if (ldr_fs.filemap[i].cpages <= 0) break;
        int32_t end = ap + ldr_fs.filemap[i].cpages;
        if (page < end) {
            *blk = ldr_fs.filemap[i].address + (page - ap);
            *len = end - page;
            return true;
        }
        ap = end;
    }
    return false;
}

static bool ldr_fillbuf(lisa_t *lisa, int32_t byte_addr) {
    int bufsize = ldr_fs.data_size > 0 ? ldr_fs.data_size : 512;
    int32_t page = byte_addr / bufsize;
    if (page < 0 || page > ldr_fs.last_page) return false;
    ldr_fs.next_byte = byte_addr - page * bufsize;
    if (page != ldr_fs.curr_page) {
        int32_t block, length;
        if (!ldr_find_position(page, &block, &length)) return false;
        if (!ldr_read_page(lisa, block, ldr_fs.buf)) return false;
        ldr_fs.curr_page = page;
    }
    return true;
}

static uint8_t ldr_getbyte(lisa_t *lisa) {
    int bufsize = ldr_fs.data_size > 0 ? ldr_fs.data_size : 512;
    if (ldr_fs.next_byte >= bufsize)
        ldr_fillbuf(lisa, (ldr_fs.curr_page + 1) * bufsize);
    uint8_t val = ldr_fs.buf[ldr_fs.next_byte];
    ldr_fs.next_byte++;
    return val;
}

static int16_t ldr_getword(lisa_t *lisa) {
    uint8_t hi = ldr_getbyte(lisa);
    uint8_t lo = ldr_getbyte(lisa);
    return (int16_t)((hi << 8) | lo);
}

static int32_t ldr_getlong(lisa_t *lisa) {
    int16_t hi = ldr_getword(lisa);
    int16_t lo = ldr_getword(lisa);
    return ((int32_t)(uint16_t)hi << 16) | (uint16_t)lo;
}

static void ldr_movemultiple(lisa_t *lisa, int32_t count, uint32_t dest) {
    int bufsize = ldr_fs.data_size > 0 ? ldr_fs.data_size : 512;
    while (count > 0) {
        int avail = bufsize - ldr_fs.next_byte;
        if (avail > count) avail = count;
        if (avail > 0 && dest + avail <= LISA_RAM_SIZE) {
            memcpy(&lisa->mem.ram[dest], &ldr_fs.buf[ldr_fs.next_byte], avail);
            dest += avail;
            count -= avail;
            ldr_fs.next_byte += avail;
        }
        if (count > 0) {
            if (!ldr_fillbuf(lisa, (ldr_fs.curr_page + 1) * bufsize)) break;
        }
    }
}

/* Find a file by scanning the s-list and reading leader page names.
 * This works for both flat-catalog and B-tree volumes without needing
 * to navigate the directory structure. */
static bool ldr_find_in_slist(lisa_t *lisa, const char *upper_name, int nlen,
                               int32_t *out_hint, int32_t *out_size) {
    int16_t start = ldr_fs.first_file;
    int16_t end = ldr_fs.empty_file;
    if (end <= start) end = ldr_fs.maxfiles;

    for (int16_t sf = start; sf < end && sf < 6000; sf++) {
        int32_t hint_addr, file_size;
        if (!ldr_find_sentry(lisa, sf, &hint_addr, &file_size)) continue;
        if (hint_addr <= 0 || file_size <= 0) continue;

        /* Read the leader page to get the filename.
         * The leader page is at hint_addr + leader_offset.
         * The filename is a Pascal string at byte 0 of the leader page. */
        int32_t leader_page = hint_addr + ldr_fs.leader_offset;
        uint8_t leader[512];
        if (!ldr_read_page(lisa, leader_page, leader)) continue;

        int fname_len = leader[0];
        if (fname_len <= 0 || fname_len > 32 || fname_len != nlen) continue;

        bool match = true;
        for (int i = 0; i < nlen; i++) {
            char c = leader[1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c != upper_name[i]) { match = false; break; }
        }
        if (match) {
            *out_hint = hint_addr;
            *out_size = file_size;
            fprintf(stderr, "LDR_FS: slist scan found '%s' sfile=%d hint=%d size=%d\n",
                    upper_name, sf, (int)hint_addr, (int)file_size);
            return true;
        }
    }
    return false;
}

static bool ldr_open_file(lisa_t *lisa, const char *name) {
    if (!ldr_fs.initialized && !ldr_fs_init(lisa)) return false;

    char upper[64];
    int nlen = (int)strlen(name);
    if (nlen > 32) nlen = 32;
    for (int i = 0; i < nlen; i++)
        upper[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    upper[nlen] = '\0';

    int bufsize = ldr_fs.data_size > 0 ? ldr_fs.data_size : 512;

    /* Strategy 1: Try the catalog (works for our cross-compiled images) */
    int32_t cat_hint, cat_size;
    bool found_in_cat = false;
    if (ldr_find_sentry(lisa, ldr_fs.rootsnum, &cat_hint, &cat_size) &&
        cat_hint > 0 && cat_size > 0 &&
        ldr_build_filemap(lisa, cat_hint, cat_size)) {
        ldr_fs.file_open = true;

        int centry_size = 54;
        for (int entry = 0; entry < ldr_fs.catentries; entry++) {
            int32_t bpos = (int32_t)entry * centry_size;
            if (!ldr_fillbuf(lisa, bpos)) break;

            uint8_t eb[64];
            int sp = ldr_fs.curr_page, sb = ldr_fs.next_byte;
            for (int i = 0; i < centry_size && i < 64; i++)
                eb[i] = ldr_getbyte(lisa);
            ldr_fs.curr_page = sp;
            ldr_fs.next_byte = sb;

            int enl = eb[0];
            if (enl <= 0 || enl > 32 || enl != nlen) continue;

            bool match = true;
            for (int i = 0; i < nlen; i++) {
                char c = eb[1+i];
                if (c >= 'a' && c <= 'z') c -= 32;
                if (c != upper[i]) { match = false; break; }
            }
            if (!match) continue;

            int16_t cetype = ldr_get16(eb + 34);
            int16_t sfile = ldr_get16(eb + 36);
            if (cetype != 3) continue; /* not fileentry */

            fprintf(stderr, "LDR_FS: catalog found '%s' sfile=%d\n", upper, sfile);

            int32_t hint_addr, file_size;
            if (!ldr_find_sentry(lisa, sfile, &hint_addr, &file_size)) continue;
            fprintf(stderr, "LDR_FS: hint=%d size=%d\n", (int)hint_addr, (int)file_size);

            if (!ldr_build_filemap(lisa, hint_addr, file_size)) continue;
            ldr_fs.file_open = true;
            ldr_fs.curr_page = -1;
            ldr_fs.next_byte = bufsize;
            found_in_cat = true;
            break;
        }
    }

    if (found_in_cat) return true;

    /* Strategy 2: Scan the s-list directly (works for real Lisa OS images
     * where the B-tree catalog may be hard to navigate) */
    fprintf(stderr, "LDR_FS: catalog search failed, scanning s-list for '%s'...\n", upper);
    int32_t hint_addr, file_size;
    if (ldr_find_in_slist(lisa, upper, nlen, &hint_addr, &file_size)) {
        if (ldr_build_filemap(lisa, hint_addr, file_size)) {
            ldr_fs.file_open = true;
            ldr_fs.curr_page = -1;
            ldr_fs.next_byte = bufsize;
            return true;
        }
    }

    fprintf(stderr, "LDR_FS: '%s' not found\n", upper);
    return false;
}

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


uint32_t g_hle_smt_base = 0;

void lisa_hle_prog_mmu(uint32_t domain, uint32_t index,
                       uint32_t count, uint32_t smt_base) {
    if (!g_lisa) return;
    lisa_mem_t *mem = &g_lisa->mem;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t seg = index + i;
        if (seg >= MMU_NUM_SEGMENTS) break;

        uint32_t entry_addr = (smt_base + domain * 512 + seg * 4) & 0xFFFFFF;
        uint16_t origin = (mem->ram[entry_addr] << 8) | mem->ram[entry_addr + 1];
        uint8_t access = mem->ram[entry_addr + 2];
        uint8_t limit = mem->ram[entry_addr + 3];

        uint16_t sor = origin;
        uint16_t slr = ((uint16_t)access << 8) | limit;

        if (domain < 4) {
            int ctx = 1 + (int)domain;
            if (ctx >= MMU_NUM_CONTEXTS) ctx = MMU_NUM_CONTEXTS - 1;
            if (seg == 85 || ((sor & 0xFFF) == 0 && seg > 2 && seg < 126)) {
                extern uint32_t g_last_cpu_pc;
                fprintf(stderr,
                        "HLE-PROG-MMU: PC=$%06X dom=%u seg=%u sor=$%03X slr=$%03X "
                        "smt=$%06X entry=$%06X raw=%02X %02X %02X %02X\n",
                        g_last_cpu_pc, domain, seg, sor & 0xFFF, slr & 0xFFF,
                        smt_base, entry_addr,
                        mem->ram[entry_addr], mem->ram[entry_addr + 1],
                        mem->ram[entry_addr + 2], mem->ram[entry_addr + 3]);
            }
            mem->segments[ctx][seg].sor = sor & 0xFFF;
            mem->segments[ctx][seg].slr = slr & 0xFFF;
            mem->segments[ctx][seg].changed = 3;
            if (ctx == 1)
                mem->segments[0][seg] = mem->segments[1][seg];
            if (seg == 84 || seg == 126 || seg == 127) {
                for (int c = 0; c < MMU_NUM_CONTEXTS; c++)
                    if (c != ctx) mem->segments[c][seg] = mem->segments[ctx][seg];
            }
        }
    }
}

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
    {
        DBGSTATIC(int, io_trace, 0);
        extern uint32_t g_last_cpu_pc;
        uint32_t pc = g_last_cpu_pc & 0xFFFFFF;
        io_trace++;
        /* Log I/O reads from SYSTEM.LLD area or late in boot */
        if (io_trace <= 5 || (pc >= 0x200000 && io_trace <= 50) || (io_trace > 100000 && io_trace <= 100005)) {
            fprintf(stderr, "IO_READ[%d]: offset=$%04X PC=$%06X\n", io_trace, offset, pc);
        }
    }
    lisa_t *lisa = g_lisa;

    /* VIA1 - parallel/ProFile ($FCD800-$FCDCFF with aliases).
     * VIA1 uses RS0-RS3 from CPU A3-A6 — register stride is 8 bytes.
     * Per libhw-DRIVERS: PORTB1=$00, PORTA1=$08, DDRB1=$10, ..., IFR1=$68,
     * IER1=$70. So register = (offset >> 3) & 0xF. */
    if (offset >= 0xD800 && offset < 0xDC00) {
        uint8_t reg = (offset >> 3) & 0xF;
        return via_read(&lisa->via1, reg);
    }

    /* VIA2 - keyboard/COPS ($FCDD00-$FCDEFF with aliases) */
    if (offset >= 0xDC00 && offset < 0xE000) {
        uint8_t reg = (offset >> 1) & 0xF;
        uint8_t val = via_read(&lisa->via2, reg);
        DBGSTATIC(int, v2trace, 0);
        if (v2trace < 30) {
            extern uint32_t g_last_cpu_pc;
            v2trace++;
            fprintf(stderr, "VIA2_RD[%d]: reg=%d val=$%02X ddra=$%02X PC=$%06X\n",
                    v2trace, reg, val, lisa->via2.ddra, g_last_cpu_pc & 0xFFFFFF);
        }
        return val;
    }

    /* Vertical retrace acknowledge — reading clears the IRQ */
    if (offset >= 0xE018 && offset <= 0xE019) {
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        /* Recalculate IRQ level */
        int level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;  /* VIA2/COPS is IRQ level 2 on Lisa */
        m68k_set_irq(&lisa->cpu, level);
        return lisa->mem.vretrace_irq ? 0x80 : 0x00;
    }

    /* Hardware status register ($FCF800-$FCF8FF)
     * Bit 7: Unused (always 1)
     * Bit 6: Inverse video (1 = normal, 0 = inverse)
     * Bit 5: CSYNC (horizontal sync, toggles with video timing)
     * Bit 4: Video bit (current display pixel)
     * Bit 3: Bus timeout error (0 = no error)
     * Bit 2: Vertical retrace (1 = in retrace)
     * Bit 1: Hard memory error (1 = no error, active low)
     * Bit 0: Soft memory error (1 = no error, active low) */
    if (offset >= 0xF800 && offset < 0xF900) {
        uint8_t status = 0x00;
        status |= 0x80;  /* Bit 7: unused, always 1 */
        status |= 0x40;  /* Bit 6: normal video (not inverse) */
        /* Bit 5: CSYNC — toggle based on frame count for realism */
        if (lisa->total_frames & 1) status |= 0x20;
        /* Bit 4: video bit — always 0 (black pixel) */
        /* Bit 3: no bus timeout */
        /* Bit 2: Vertical retrace.
         * Per Lisa_Source/LISA_OS/OS/source-SERNUM (line 87-94): bit 2 is
         * ACTIVE LOW. BTST #2/BEQ waits with "BEQ vretrace-occurred", so
         * bit=0 means in retrace. Default high (not retrace), clear for
         * the last 10% of the frame. */
        {
            uint64_t pos = lisa->cpu.total_cycles % (uint64_t)LISA_CYCLES_PER_FRAME;
            if (pos < (uint64_t)(LISA_CYCLES_PER_FRAME * 9 / 10))
                status |= 0x04;  /* NOT in vretrace (first 90% of frame) */
        }
        status |= 0x02;  /* Bit 1: no hard memory error */
        status |= 0x01;  /* Bit 0: no soft memory error */
        return status;
    }

    /* Contrast */
    if (offset == 0xD01C) {
        return lisa->mem.contrast;
    }

    /* I/O board type register ($FCC031) — determines Lisa model.
     * Return 0 → iob_lisa (Lisa 1 with parallel ProFile).
     * With bootdev=2, FIND_BOOT maps this to cd_paraport.
     * Previous value 0x80 (-128) → iob_pepsi → cd_intdisk which requires
     * different driver loading that our HLE doesn't support yet. */
    if (offset == 0xC031) {
        return 0x00;  /* 0 (positive) → iob_lisa */
    }

    /* Internal disk type register ($FCC015) — 0=twiggy, nonzero=Sony/ProFile.
     * With iob_lisa, this isn't checked for iomodel determination. */
    if (offset == 0xC015) {
        return 0x00;  /* 0 = twiggy (default for Lisa 1) */
    }

    /* COPS parameter memory ($FCC181-$FCC1FF) — read via MOVEP.L in INIT_READ_PM.
     * MOVEP reads every other byte, so addresses are at odd offsets.
     * 16 iterations × 4 bytes = 64 bytes of pram data.
     * Return default/empty pram: all zeros (checksum will fail → DEFAULTPM). */
    if (offset >= 0xC181 && offset <= 0xC1FF) {
        /* Return valid pram with a ProFile CDD entry.
         * pram layout: see SOURCE-PMEM.TEXT.
         * Packed CDD entry format: see SOURCE-CDCONFIGASM.TEXT.
         * ProFile on parallel port: slot=9(→10), chan=6(→7=empty), dev=30(→31=empty), id=34 */
        static const uint8_t pram[64] = {
            0x00, 0x04,  /* [0-1] version = 4 (cd_pm_version) */
            0x00, 0x01,  /* [2-3] timestamp = 1 */
            0x27,        /* [4] bootVol=2, contrast=7 */
            0x33,        /* [5] dim=3, beep=3 */
            0xE3,        /* [6] mouseOn, extMem, scaleMouse, dblClick=3 */
            0x33,        /* [7] fadeDelay=3, beginRepeat=3 */
            0x30,        /* [8] subRepeat=3 */
            0x01,        /* [9] cdCount=1 */
            0x9C,        /* [10] CDD: [slot=9:4][chan=6:3][idsize=0:1] */
            0xF0,        /* [11] CDD: [dev=30:5][nExt=0:2][hibit=0:1] */
            0x22,        /* [12] CDD: driverID=34 byte */
            0xF0,        /* [13] terminator: slot=15 */
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* [14-29] */
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* [30-45] */
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,     /* [46-59] */
            0x00, 0x00,  /* [60-61] mem_loss */
            /* [62-63] checksum: XOR of all 32 words must = 0.
             * Words: 0004 0001 2733 E333 3001 9CF0 22F0 0000...0000
             * XOR = 0x4A04. Checksum = 0x4A04 so total XOR = 0. */
            0x4A, 0x04,
        };
        int byte_idx = (offset - 0xC181) / 2;
        if (byte_idx >= 0 && byte_idx < 64)
            return pram[byte_idx];
        return 0;
    }

    /* Disk controller shared memory */
    if (offset < 0x2000) {
        return 0;
    }

    return 0xFF;
}

static void io_write_cb(uint32_t offset, uint8_t val) {
    lisa_t *lisa = g_lisa;

    /* VIA1 ($FCD800-$FCDCFF with aliases) — stride 8 (see read path). */
    if (offset >= 0xD800 && offset < 0xDC00) {
        uint8_t reg = (offset >> 3) & 0xF;
        via_write(&lisa->via1, reg, val);
        return;
    }

    /* VIA2 ($FCDC00-$FCDEFF with aliases) */
    if (offset >= 0xDC00 && offset < 0xE000) {
        uint8_t reg = (offset >> 1) & 0xF;
        via_write(&lisa->via2, reg, val);
        return;
    }

    /* Vertical retrace acknowledge — writing also clears */
    if (offset >= 0xE018 && offset <= 0xE019) {
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        int level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;  /* VIA2/COPS is IRQ level 2 on Lisa */
        m68k_set_irq(&lisa->cpu, level);
        return;
    }

    /* Contrast latch */
    if (offset == 0xD01C) {
        lisa->mem.contrast = val;
        return;
    }

    /* Video page latch — main FB at $1F8000, alt FB at $1F0000. */
    if (offset >= 0xE800 && offset < 0xE900) {
        lisa->mem.video_alt = (val & 1) != 0;
        lisa->mem.video_addr = lisa->mem.video_alt
            ? (2 * 1024 * 1024 - 0x10000)
            : (2 * 1024 * 1024 - 0x8000);
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

    /* Loader trap port ($FCC100) — the ROM loader stub writes the
     * fake_parms pointer here to request disk I/O from the emulator.
     * This is called byte-by-byte by the MOVE.L instruction, so we
     * accumulate 4 bytes into a 32-bit address. The actual operation
     * triggers on the last byte (offset 0xC103). */
    if (offset >= 0xC100 && offset <= 0xC103) {
        DBGSTATIC(uint32_t, loader_parms_addr, 0);
        int byte_pos = offset - 0xC100;
        loader_parms_addr &= ~(0xFFU << (24 - byte_pos * 8));
        loader_parms_addr |= ((uint32_t)val << (24 - byte_pos * 8));

        if (byte_pos == 3) {
            /* All 4 bytes received — process the loader call.
             * fake_parms layout (from SOURCE-CD.TEXT):
             *   offset 0: error (2 bytes, integer)
             *   offset 2: opcode (2 bytes, integer)
             *   offset 4: addr (4 bytes, longint)
             *   offset 8: header (4 bytes, longint)
             *   offset 12: blok (4 bytes, longint)
             *   offset 16: count (4 bytes, longint)
             *   offset 20: result (2 bytes, boolean — Pascal boolean = 2 bytes)
             *   offset 22: longvalue (4 bytes)
             *   offset 26: wordvalue (2 bytes)
             *   offset 28: bytevalue (1 byte + pad)
             *   offset 30: path (ld_filename = pascal string) */
            /* fake_parms comes in as a LOGICAL (MMU-translated) address —
             * the caller lives in user-mode code with the MMU enabled, so
             * it passes a virtual pointer. Route all reads through
             * lisa_mem_read{8,16,32} which handle the translation. */
            uint32_t pa = loader_parms_addr;
            /* Probe the first and last byte we'll touch (offset +30 for the
             * Pascal string length) to catch obviously bogus pointers. We
             * don't need an exact bound check here — the MMU will pass
             * unmapped segments through and the emulator will flag the
             * resulting out-of-range physical address. */
            (void)lisa_mem_read8(&lisa->mem, pa);
            (void)lisa_mem_read8(&lisa->mem, pa + 30);

            uint16_t opcode = lisa_mem_read16(&lisa->mem, pa + 2);
            uint32_t addr   = lisa_mem_read32(&lisa->mem, pa + 4);
            uint32_t blok   = lisa_mem_read32(&lisa->mem, pa + 12);
            uint32_t count  = lisa_mem_read32(&lisa->mem, pa + 16);

            fprintf(stderr, "LOADER TRAP: opcode=%u addr=$%08X blok=%u count=%u\n",
                    opcode, addr, (unsigned)blok, (unsigned)count);

            switch (opcode) {
                case 0: { /* call_open — open a file on boot disk */
                    /* Extract Pascal string from path field at offset 30 */
                    int path_len = lisa_mem_read8(&lisa->mem, pa + 30);
                    if (path_len > 32) path_len = 32;
                    char path_str[64];
                    for (int i = 0; i < path_len; i++)
                        path_str[i] = lisa_mem_read8(&lisa->mem, pa + 31 + i);
                    path_str[path_len] = '\0';

                    bool ok = ldr_open_file(lisa, path_str);
                    /* result at offset 20 (Pascal boolean = 2 bytes) */
                    lisa_mem_write16(&lisa->mem, pa + 20, ok ? 0x0001 : 0x0000);
                    lisa_mem_write16(&lisa->mem, pa + 0, 0);
                    fprintf(stderr, "LOADER: call_open '%s' → %s\n",
                            path_str, ok ? "OK" : "FAIL");
                    break;
                }

                case 1: { /* call_fill — position to byte offset in file */
                    bool ok = ldr_fillbuf(lisa, (int32_t)addr);
                    lisa_mem_write16(&lisa->mem, pa + 0, ok ? 0x0000 : 0x00FF);
                    break;
                }

                case 2: { /* call_byte — read next byte from file */
                    uint8_t b = ldr_getbyte(lisa);
                    lisa_mem_write8(&lisa->mem, pa + 28, b); /* bytevalue */
                    lisa_mem_write16(&lisa->mem, pa + 0, 0);
                    break;
                }

                case 3: { /* call_word — read next word from file */
                    int16_t w = ldr_getword(lisa);
                    lisa_mem_write16(&lisa->mem, pa + 26, (uint16_t)w);
                    lisa_mem_write16(&lisa->mem, pa + 0, 0);
                    break;
                }

                case 4: { /* call_long — read next longint from file */
                    int32_t l = ldr_getlong(lisa);
                    lisa_mem_write32(&lisa->mem, pa + 22, (uint32_t)l);
                    lisa_mem_write16(&lisa->mem, pa + 0, 0);
                    break;
                }

                case 5: { /* call_move — move bytes from file to memory */
                    ldr_movemultiple(lisa, (int32_t)count, addr);
                    lisa_mem_write16(&lisa->mem, pa + 0, 0);
                    fprintf(stderr, "LOADER: call_move %u bytes → $%08X\n",
                            (unsigned)count, addr);
                    break;
                }

                case 6: /* call_read — read blocks from boot disk.
                         * Original loader adds block0 to blok internally. */
                    if (lisa->profile.mounted && lisa->profile.data) {
                        if (!ldr_fs.initialized) ldr_fs_init(lisa);
                        uint32_t block0 = BOOT_TRACK_BLOCKS;
                        if (ldr_fs.initialized) {
                            /* Use geo_firstblock for page→block mapping,
                             * falling back to MDDF block position */
                            block0 = (uint32_t)ldr_fs.geo_firstblock;
                            if (block0 == 0 || block0 > (uint32_t)ldr_fs.fs_block0)
                                block0 = (uint32_t)ldr_fs.fs_block0;
                        }
                        for (uint32_t b = 0; b < count; b++) {
                            uint32_t block_num = block0 + blok + b;
                            size_t src_offset = (size_t)block_num * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
                            uint32_t dst = addr + b * 512;
                            if (src_offset + 512 <= lisa->profile.data_size &&
                                dst + 512 <= LISA_RAM_SIZE) {
                                memcpy(&lisa->mem.ram[dst],
                                       lisa->profile.data + src_offset, 512);
                            } else if (dst + 512 <= LISA_RAM_SIZE) {
                                memset(&lisa->mem.ram[dst], 0, 512);
                            }
                        }
                        /* Fill header/tag data */
                        uint32_t hdr_addr = lisa_mem_read32(&lisa->mem, pa + 8);
                        if (hdr_addr > 0 && hdr_addr + PROFILE_TAG_SIZE <= LISA_RAM_SIZE) {
                            size_t tag_offset = (size_t)(block0 + blok) * PROFILE_BLOCK_SIZE;
                            if (tag_offset + PROFILE_TAG_SIZE <= lisa->profile.data_size)
                                memcpy(&lisa->mem.ram[hdr_addr],
                                       lisa->profile.data + tag_offset, PROFILE_TAG_SIZE);
                        }
                        lisa_mem_write16(&lisa->mem, pa + 0, 0);
                        fprintf(stderr, "LOADER: call_read %u blocks @%u (abs %u) → $%08X\n",
                                (unsigned)count, (unsigned)blok, (unsigned)(block0+blok), addr);
                    } else {
                        lisa_mem_write16(&lisa->mem, pa + 0, 0xFFFF);
                        fprintf(stderr, "LOADER: call_read FAIL — no disk\n");
                    }
                    break;

                default:
                    fprintf(stderr, "LOADER: unknown opcode %u\n", opcode);
                    lisa_mem_write16(&lisa->mem, pa + 0, 0xFFFF);
                    break;
            }
            loader_parms_addr = 0;
        }
        return;
    }

    /* Disk shared memory */
    if (offset < 0x2000) {
        return;
    }
}


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
    /* Trace first 10 ORB writes */
    {
        DBGSTATIC(int, orb_trace_count, 0);
        if (orb_trace_count < 10) {
            fprintf(stderr, "VIA1_ORB_WRITE #%d: val=$%02X ddr=$%02X (CMD=%d DIR=%d)\n",
                    ++orb_trace_count, val, ddr,
                    (val >> 2) & 1, (val >> 3) & 1);
        }
    }
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
    /* ProFile VIA1 Port B bits:
     * Bit 0 (OCD): Open Cable Detect — 1 = cable connected
     * Bit 1 (BSY): Busy — 1 = ProFile busy, 0 = ready
     * Bit 2 (DEN): Data Enable
     * Bit 3 (RRW): Read/Write direction
     * Bit 4 (CMD): Command line
     * Bit 5 (PARITY): Parity */
    uint8_t val = 0;

    if (lisa->prof.mounted) {
        val |= 0x01; /* OCD = 1: cable connected */
        /* BSY: 0 = ready, 1 = busy. Don't set bit 1 when idle. */
        if (profile_bsy(&lisa->prof))
            val |= 0x02; /* BSY = 1: ProFile is busy */
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
    /* VIA2 Port B: COPS interface (from Lisa OS source COPSCMD)
     * Bit 0: Data available from COPS
     * Bit 1-3: Volume control (output)
     * Bit 4: (unused input, reads as 0)
     * Bit 5: Parity reset (output)
     * Bit 6: CRDY — COPS ready for command (1=ready, 0=busy processing)
     * Bit 7: Controller reset (output)
     *
     * COPSCMD protocol:
     *   1. Check CRDY=1 (ready for command)
     *   2. Write command to port A
     *   3. Check CRDY=1 (sanity check, should still be 1)
     *   4. Poll 16 times for CRDY=0 (COPS accepted command)
     *   5. When CRDY=0, set DDRA to output, spin, set back to input
     */
    uint8_t val = 0;

    /* Bit 0: data available from COPS */
    if (lisa->cops_rx.count > 0)
        val |= 0x01;

    /* Bit 6: CRDY — COPS ready for command.
     * After a command is written (via porta_write), CRDY goes low
     * for a few port B reads, then returns high. The COPSCMD routine
     * writes the command, does 1 sanity check (expects CRDY=1), then
     * polls up to 16 times for CRDY=0. So we keep CRDY=1 for the
     * first read after a command, then go to 0. */
    if (lisa->cops_crdy_count == 0) {
        val |= 0x40;  /* CRDY=1: ready for command (no command pending) */
    } else {
        /* Command was written. First read: CRDY still 1 (sanity check).
         * Subsequent reads: CRDY=0 (COPS processing). */
        if (lisa->cops_crdy_count == 1)
            val |= 0x40;  /* First read after command: still 1 */
        /* else: CRDY=0 (COPS busy — command accepted) */
        lisa->cops_crdy_count++;
        if (lisa->cops_crdy_count > 4)
            lisa->cops_crdy_count = 0;  /* Reset: CRDY returns to 1 */
    }

    return val;
}

static void via2_porta_write(uint8_t val, uint8_t ddr, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;

    /* COPS command protocol (from LisaEm reference):
     * $00 = Turn I/O port ON (reset COPS)
     * $01 = Turn I/O port OFF
     * $02 = Read clock data
     * $10-$1F = Write nibble to clock buffer
     * $20-$2F = Clock mode control (set/power/timer)
     * $30-$4F = Keyboard LED control
     * $50-$6F = NMI key configuration
     * $70-$77 = Mouse OFF
     * $78-$7F = Mouse ON with interval
     * $80-$FF = NOP */
    DBGSTATIC(int, cops_cmd_count, 0);
    cops_cmd_count++;
    if (cops_cmd_count <= 10)
        fprintf(stderr, "COPS CMD[%d]: $%02X\n", cops_cmd_count, val);

    /* Start CRDY handshake: next port B read will see CRDY=1 (sanity),
     * then subsequent reads see CRDY=0 (COPS accepted command). */
    lisa->cops_crdy_count = 1;

    if (val >= 0x80) {
        /* NOP — high bit set commands are ignored */
    } else if (val == 0x00 || val == 0x01) {
        /* Reset/power — queue keyboard ID */
        cops_enqueue(&lisa->cops_rx, 0x80);  /* Reset marker */
        cops_enqueue(&lisa->cops_rx, 0x2F);  /* Keyboard ID: US layout */
    } else if (val == 0x02) {
        /* Read clock — queue clock data (LisaEm format) */
        cops_enqueue(&lisa->cops_rx, 0x80);  /* Reset/clock marker */
        cops_enqueue(&lisa->cops_rx, 0xE6);  /* Year nibble (1986 = 0xE6) */
        cops_enqueue(&lisa->cops_rx, 0x61);  /* Days high (061 = Feb) */
        cops_enqueue(&lisa->cops_rx, 0x10);  /* Days low + hours high */
        cops_enqueue(&lisa->cops_rx, 0x20);  /* Hours low + mins high */
        cops_enqueue(&lisa->cops_rx, 0x00);  /* Mins low + secs high */
        cops_enqueue(&lisa->cops_rx, 0x00);  /* Secs low + tenths */
    } else if (val >= 0x10 && val <= 0x1F) {
        /* Write clock nibble — acknowledge silently */
    } else if (val >= 0x20 && val <= 0x2F) {
        /* Clock mode control — acknowledge silently */
    } else if (val >= 0x70 && val <= 0x7F) {
        /* Mouse control */
        if (val >= 0x78) {
            /* Mouse ON — no response needed. COPS starts sending
             * mouse delta data ($00, Dx, Dy) on movement. */
        }
    } else {
        /* Other commands — acknowledge */
    }

    /* After processing a COPS command, if there's queued data waiting
     * (e.g., keyboard ID from boot), trigger CA1 to notify the OS.
     * The Level 2 interrupt handler will read VIA2 port A to consume
     * the data and clear the CA1 flag. We only do this after the first
     * command, when the interrupt infrastructure is fully initialized. */
    if (lisa->cops_rx.count > 0 && cops_cmd_count >= 1) {
        via_trigger_ca1(&lisa->via2);
    }
}

static uint8_t via2_porta_read(void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;

    /* Read byte from COPS */
    if (lisa->cops_rx.count > 0) {
        uint8_t val = cops_dequeue(&lisa->cops_rx);
        DBGSTATIC(int, read_count, 0);
        if (read_count++ < 10)
            fprintf(stderr, "COPS READ[%d]: $%02X (remain=%d) PC=$%06X\n",
                    read_count, val, lisa->cops_rx.count, lisa->cpu.pc & 0xFFFFFF);

        /* If more COPS data is queued, re-trigger CA1 so the Level 2
         * handler fires again. On real hardware, COPS holds CA1 asserted
         * as long as it has data to send. The VIA ORA read clears CA1,
         * then we re-assert it here if more data is available. */
        if (lisa->cops_rx.count > 0) {
            via_trigger_ca1(&lisa->via2);
        }

        return val;
    }
    return 0xFF;  /* No data */
}

/* VIA2 IRQ → CPU IRQ level 2 (COPS).
 * On the Lisa, VIA1 is level 1 (shared with vretrace) and VIA2/COPS
 * is level 2. The OS installs separate Level 1 and Level 2 handlers.
 * Level 1: vretrace + VIA1 (ProFile, Timer1)
 * Level 2: VIA2 CA1 (COPS keyboard/mouse/clock) */
static void via2_irq(bool state, void *ctx) {
    lisa_t *lisa = (lisa_t *)ctx;
    lisa->irq_via2 = state ? 1 : 0;
    /* Set CPU IRQ to highest active level */
    int level = 0;
    if (lisa->irq_vretrace || lisa->irq_via1) level = 1;
    if (lisa->irq_via2) level = 2;  /* VIA2/COPS is IRQ level 2 on Lisa */
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

/* ProFile disk operations now in profile.c */

/* HLE callback wrapper — adapts void* to typed call.
 * Forward declaration; implementation at bottom of file. */
static bool hle_cpu_check(void *ctx, void *cpu);

/* ========================================================================
 * Public API
 * ======================================================================== */

void lisa_init(lisa_t *lisa) {
    memset(lisa, 0, sizeof(lisa_t));
    g_lisa = lisa;
    g_emu_generation++;

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

    /* Wire HLE intercept callback */
    lisa->cpu.hle_check = hle_cpu_check;
    lisa->cpu.hle_ctx = lisa;

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

    /* Cold reset: clear video RAM (both framebuffer pages) and the
     * rendered framebuffer so the display goes blank instead of
     * showing the previous run's last frame. Main FB at $1F8000,
     * alt (debug) FB at $1F0000 — clear the top 64KB which covers
     * both. Full RAM is deliberately NOT cleared here because the
     * OS image reload below rewrites everything it cares about. */
    memset(&lisa->mem.ram[0x1F0000], 0, 0x10000);
    memset(lisa->framebuffer, 0,
           sizeof(uint32_t) * LISA_SCREEN_WIDTH * LISA_SCREEN_HEIGHT);

    /* Reset loader filesystem state for fresh boot */
    memset(&ldr_fs, 0, sizeof(ldr_fs));
    ldr_fs.initialized = false;

    /* Pre-load OS from ProFile disk image into RAM.
     * Uses the loader filesystem to find system.os by scanning the MDDF,
     * s-list, and leader pages. Works with both our cross-compiled images
     * (24-block boot track, MDDF at block 24) and real Lisa OS images
     * (variable boot track, MDDF typically at block 46).
     *
     * Boot track → RAM at $20000
     * system.os file data → RAM at $0 (where the linker placed it) */
    uint32_t os_loaded_bytes = 0;
    if (lisa->profile.mounted && lisa->profile.data) {
        /* Initialize the loader filesystem to find MDDF and parse it */
        if (!ldr_fs_init(lisa)) {
            fprintf(stderr, "Pre-loader: cannot init filesystem\n");
        }

        /* Determine boot track size from MDDF location or geo.firstblock */
        int boot_track_end = BOOT_TRACK_BLOCKS;
        if (ldr_fs.initialized) {
            /* The boot track extends up to the MDDF block */
            boot_track_end = (int)ldr_fs.fs_block0;
            if (boot_track_end < BOOT_TRACK_BLOCKS)
                boot_track_end = BOOT_TRACK_BLOCKS;
        }

        /* Load boot track at $20000 */
        uint32_t dest = 0x20000;
        int boot_blocks = 0;
        for (int blk = 0; blk < boot_track_end && dest + PROFILE_DATA_SIZE < LISA_RAM_SIZE; blk++) {
            size_t src_offset = (size_t)blk * PROFILE_BLOCK_SIZE;
            if (src_offset + PROFILE_BLOCK_SIZE <= lisa->profile.data_size) {
                memcpy(&lisa->mem.ram[dest], lisa->profile.data + src_offset + PROFILE_TAG_SIZE, PROFILE_DATA_SIZE);
                dest += PROFILE_DATA_SIZE;
                boot_blocks++;
            }
        }
        printf("Pre-loaded %d boot blocks at RAM $20000\n", boot_blocks);

        /* For real Lisa OS disk images (MDDF not at block 24), the boot track
         * contains the complete Pascal-based loader that will read SYSTEM.LLD
         * and SYSTEM.OS using LDRCALL operations (intercepted by our HLE).
         * We do NOT pre-load system.os — the boot track loader handles it.
         *
         * For our cross-compiled images (MDDF at block 24), the boot track is
         * minimal and we use the simplified catalog to pre-load system.os. */
        bool is_real_image = (ldr_fs.initialized && ldr_fs.fs_block0 != BOOT_TRACK_BLOCKS);
        if (is_real_image) {
            printf("Real Lisa OS image detected (MDDF at block %d) — "
                   "boot track loader will load OS via HLE\n",
                   (int)ldr_fs.fs_block0);
            /* Set os_loaded_bytes to a reasonable estimate for memory layout.
             * The actual loading will be done by the boot track loader. */
            os_loaded_bytes = 185344;  /* Approximate system.os size */
        } else if (ldr_fs.initialized && ldr_open_file(lisa, "SYSTEM.OS")) {
            /* Cross-compiled image: load system.os directly via filemap */
            uint32_t ram_dest = 0;
            uint32_t loaded_pages = 0;
            for (int mi = 0; mi < LDR_MAP_MAX; mi++) {
                if (ldr_fs.filemap[mi].cpages <= 0) break;
                int32_t addr = ldr_fs.filemap[mi].address;
                int16_t cpg = ldr_fs.filemap[mi].cpages;
                for (int p = 0; p < cpg && ram_dest + PROFILE_DATA_SIZE <= LISA_RAM_SIZE; p++) {
                    uint8_t page_buf[512];
                    if (ldr_read_page(lisa, addr + p, page_buf)) {
                        memcpy(&lisa->mem.ram[ram_dest], page_buf, PROFILE_DATA_SIZE);
                        ram_dest += PROFILE_DATA_SIZE;
                        loaded_pages++;
                    }
                }
            }
            os_loaded_bytes = loaded_pages * PROFILE_DATA_SIZE;
            printf("Pre-loaded system.os: %u pages (%u bytes) at RAM $0\n",
                   loaded_pages, os_loaded_bytes);
        } else {
            /* Fallback: try our simplified catalog format (cross-compiled images) */
            uint32_t cat_block = BOOT_TRACK_BLOCKS + 1;
            size_t cat_offset = (size_t)cat_block * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
            if (cat_offset + PROFILE_DATA_SIZE <= lisa->profile.data_size) {
                uint8_t *catalog = lisa->profile.data + cat_offset;
                uint32_t file_size = ((uint32_t)catalog[34] << 24) | ((uint32_t)catalog[35] << 16) |
                                     ((uint32_t)catalog[36] << 8)  | (uint32_t)catalog[37];
                uint32_t start_block = ((uint32_t)catalog[38] << 8) | (uint32_t)catalog[39];
                uint32_t num_blocks = ((uint32_t)catalog[40] << 8) | (uint32_t)catalog[41];

                if (file_size > 0 && start_block > 0) {
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
                    printf("Pre-loaded system.os (fallback): %u blocks (%u bytes) at RAM $0\n",
                           loaded, os_loaded_bytes);
                }
            }
        }

        if (!is_real_image) {
            /* Vector table ($0-$3FF) is pre-installed by the linker in system.os.
             * Only set Vector 0 (SSP) and Vector 1 (PC) for boot. */
            /* Vector 0: Initial SSP */
            lisa->mem.ram[0] = 0x00; lisa->mem.ram[1] = 0x07;
            lisa->mem.ram[2] = 0x90; lisa->mem.ram[3] = 0x00;
            /* Vector 1: Initial PC (points to first code at $400) */
            lisa->mem.ram[4] = 0x00; lisa->mem.ram[5] = 0x00;
            lisa->mem.ram[6] = 0x04; lisa->mem.ram[7] = 0x00;
        }
        /* For real images, the boot track loader will set up vectors */

        /* Log vector table state */
        printf("Vector table: TRAP1[$84]=$%02X%02X%02X%02X TRAP7[$9C]=$%02X%02X%02X%02X\n",
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

        /* For real images, the boot track loader handles TRAP setup, parameter
         * blocks, and memory layout. We only install safety vectors.
         * For cross-compiled images, we set up everything. */

        /* Boot device identifier — needed by both real and cross-compiled */
        lisa->mem.ram[0x1B3] = 2;      /* adr_bootdev: 2 = parallel port (ProFile on iob_lisa) */

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

        /* Fill ALL zero TRAP vectors ($80-$BC) with the ROM RTE handler. */
        for (uint32_t vec = 0x80; vec <= 0xBC; vec += 4) {
            uint32_t val = (lisa->mem.ram[vec] << 24) |
                           (lisa->mem.ram[vec+1] << 16) |
                           (lisa->mem.ram[vec+2] << 8) |
                            lisa->mem.ram[vec+3];
            if (val == 0) {
                WRITE32(vec, 0x00FE0300);
            }
        }

        if (!is_real_image) {
            /* Verify chan_select at $1F0 — must be 0 for screen console.
             * The linker output may have data at this offset from code/relocs. */
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
        /* Report 2MB to the loader even though we have 2.25MB physical RAM.
         * The extra 256KB is for the mmucodemmu segment (SOR near $FE4) which
         * wraps past 2MB. The loader must not allocate above 2MB because the
         * SOR register is only 12 bits (max page $FFF = physical $1FFE00). */
        uint32_t reported_ram = 2 * 1024 * 1024;  /* 2MB */
        WRITE32(0x294, reported_ram);  /* prom_memsize: last byte + 1 */
        WRITE32(0x2A4, 0x00000000);   /* prom_byte0: physical byte 0 */
        WRITE32(0x2A8, reported_ram);  /* prom_realsize: amount of memory */

        /* For real images, set fs_block0 at $210 and loader link.
         * The boot track loader handles everything else. */
        if (is_real_image) {
            int16_t fs_b0 = (int16_t)ldr_fs.geo_firstblock;
            if (fs_b0 <= 0) fs_b0 = (int16_t)ldr_fs.fs_block0;
            WRITE16(0x210, fs_b0);      /* ld_fs_block0 */
            WRITE16(0x22E, 1);          /* dev_type: profile */
            WRITE32(0x21C, 0x00020000); /* ldbaseptr: loader base */
            /* The boot track loader will build its own parameter block,
             * set up MMU, allocate memory, and load SYSTEM.LLD + SYSTEM.OS */
        }

        if (!is_real_image) {
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
        /* SMT (System Mapping Table): 512 entries × 4 bytes = 2048 bytes.
         * Must be ABOVE os_end to avoid overlapping with OS code. */
        uint32_t smt_base    = os_end;
        uint32_t l_smt       = 0x800;     /* 2KB for 512 entries */
        extern uint32_t g_hle_smt_base;
        g_hle_smt_base = smt_base;
        uint32_t b_sysjt     = smt_base + l_smt;
        uint32_t l_sysjt     = 0x1000;    /* 4KB jump table */
        uint32_t b_sysglobal = b_sysjt + l_sysjt;
        /* 28KB sysglobal. Apple's layout is 24KB ($6000), but our Pascal
         * codegen produces globals totaling ~24906 bytes — 330 bytes over
         * Apple's hardcoded PASCALDEFS offsets. With l_sysglobal=$6000,
         * A5 ends up at $CC5FFC and A5-relative offsets beyond $5FFC
         * cross into logical segment 101 (the supervisor-stack segment),
         * so Pascal globals share physical memory with the stack —
         * `sg_free_pool_addr` at A5-24906 = logical $CBFEB2 gets
         * overwritten by supervisor stack pushes.
         *
         * Bumping to $7000 pushes A5 to $CC6FFC so all compiled globals
         * fit within segment 102, with a healthy margin. */
        uint32_t l_sysglobal = 0x7000;
        uint32_t b_superstack = b_sysglobal + l_sysglobal;
        uint32_t l_superstack = 0x4000;   /* 16KB supervisor stack */
        uint32_t b_sgheap    = b_superstack + l_superstack;
        uint32_t l_sgheap    = 0x7E00;    /* ~31.5KB sysglobal heap.
                                          * Must be (a) page-aligned ($200 = 512 bytes)
                                          * for MMU SOR consistency, and (b) ≤ 32767 because
                                          * INIT_FREEPOOL takes fp_size as int2. */
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
        /* himem = highest free byte + 1.  Must be below the screen
         * buffers (which sit at top of physical RAM).  For a 2MB Lisa,
         * the debug screen is at $1F0000 and main screen at $1F8000.
         * Free memory extends from lomem up to the debug screen base. */
        uint32_t himem       = b_dbscreen;

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

        /* Write fields downward: p starts at version_addr, decreases.
         * Each W32D does p -= 4 first, then writes. The GETLDMAP copy loop
         * reads 2-byte words starting at ldmapbase (=version_addr) and going down.
         * 4-byte fields occupy two consecutive words in the param block. */
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
        {
            /* fs_block0: the disk block of the MDDF (filesystem page 0).
             * For real images this may differ from BOOT_TRACK_BLOCKS (e.g. 46 vs 24).
             * The loader passes this to the OS via the parameter block at $210. */
            int16_t fs_b0 = BOOT_TRACK_BLOCKS;
            if (ldr_fs.initialized) {
                /* Use geo.firstblock from MDDF — this is what the OS expects for
                 * page→block translation in its filesystem operations. */
                fs_b0 = (int16_t)ldr_fs.geo_firstblock;
                if (fs_b0 <= 0) fs_b0 = (int16_t)ldr_fs.fs_block0;
            }
            W16D(fs_b0);  /* fs_block0 */
        }
        W16D(0);             /* debugmode = false */
        W32D(smt_base);      /* smt_base — above OS code, not in vector table */
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
        W32D(lomem);         /* toplomem — highest legal value of the lo-memory ptr */
        W32D(lomem);         /* bothimem — lowest legal value of the hi-memory ptr.
                              * Guards against overwriting the loader during allocation.
                              * Set to lomem since we have no resident loader to protect. */
        W16D(0xFFFF);        /* parmend sentinel */

        #undef W32D
        #undef W16D

        /* Low-memory system pointers (PASCALDEFS.TEXT EQU definitions) */
        WRITE32(0x200, b_sysglobal);   /* SGLOBAL/B_SYSGLOBAL: ptr to sysglobal base */
        WRITE32(0x204, 0x00FE0600);    /* loader_link: ptr to loader stub in ROM */
        WRITE32(0x208, 0);             /* C_DOMAIN_PTR: ptr to current domain cell */

        /* Low-memory pointers for PASCALINIT */
        WRITE32(0x218, version_addr);  /* adrparamptr → version */
        WRITE32(0x21C, 0x00020000); /* ldbaseptr: loader base */
        /* DRIVRJT ($210) — Driver Jump Table pointer.
         * The linker writes the DRIVERASM module base to $210.
         * CALLDRIVER dispatches through this jump table. */
        b_sysjt = smt_base + l_smt;
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

            /* Stack segments: Lisa MMU DO_AN_MMU computes SOR differently
             * for stack type ($600): SOR = origin + length - hw_adjust,
             * where hw_adjust = $100 pages (128KB = one full segment).
             * This places the stack's physical TOP at the TOP of the MMU
             * segment, so downward growth works correctly.
             * Formula: SOR = (base >> 9) + (length >> 9) - 0x100 */
            #define STACK_SOR(base, len) \
                (uint16_t)(((base) >> 9) + ((len) >> 9) - 0x100)

            /* superstkmmu (101): maps $CA0000 → physical supervisor stack */
            SET_MMU_SEG(ctx, 101, 0x0600, STACK_SOR(b_superstack, l_superstack));

            /* stackmmu (123): maps $F60000 → physical user stack + jump table */
            uint32_t b_stack = b_syslocal + l_syslocal;
            uint32_t b_opustack = b_stack;
            SET_MMU_SEG(ctx, 123, 0x0600, STACK_SOR(b_opustack, l_opustack));

            /* Also map segment 104 for legacy compatibility */
            SET_MMU_SEG(ctx, 104, 0x0600, STACK_SOR(b_stack, l_opustack));

            #undef STACK_SOR

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

        /* Param block diagnostics */
        {
            uint32_t off28 = version_addr - 28;
            uint32_t val28 = (lisa->mem.ram[off28]<<24)|(lisa->mem.ram[off28+1]<<16)|
                             (lisa->mem.ram[off28+2]<<8)|lisa->mem.ram[off28+3];
            fprintf(stderr, "Param block: version=$%X, b_intrin_ptrs @$%X=$%08X\n",
                    version_addr, off28, val28);
        }

        } /* end if (!is_real_image) */

        #undef WRITE32
        #undef WRITE16
    }

    /* Debug: verify ROM is loaded before reset */
    printf("Reset: setup_mode=%d, rom[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
           lisa->mem.setup_mode,
           lisa->mem.rom[0], lisa->mem.rom[1], lisa->mem.rom[2], lisa->mem.rom[3],
           lisa->mem.rom[4], lisa->mem.rom[5], lisa->mem.rom[6], lisa->mem.rom[7]);

    /* Patch ROM BEFORE reset: set initial SSP and A6 to user stack area.
     * INITSYS's frame must be within the user stack MMU segment so that
     * REG_TO_MAPPED's physical→mapped address translation works. */
    {
        #define RAM32(a) ((uint32_t)lisa->mem.ram[(a)]<<24 | (uint32_t)lisa->mem.ram[(a)+1]<<16 | \
                          (uint32_t)lisa->mem.ram[(a)+2]<<8 | (uint32_t)lisa->mem.ram[(a)+3])
        uint32_t va = RAM32(0x218);  /* version_addr from parameter block */
        if (va > 0 && va < LISA_RAM_SIZE) {
            uint32_t b_op = RAM32(va - 68);  /* b_opustack (17th field) */
            uint32_t l_op = RAM32(va - 72);  /* l_opustack (18th field) */
            uint32_t e_us = b_op + l_op;     /* top of user stack */
            if (e_us > 0x1000 && e_us < LISA_RAM_SIZE) {
                /* Patch ROM initial SSP (vector 0) */
                lisa->mem.rom[0] = (e_us >> 24) & 0xFF;
                lisa->mem.rom[1] = (e_us >> 16) & 0xFF;
                lisa->mem.rom[2] = (e_us >> 8) & 0xFF;
                lisa->mem.rom[3] = e_us & 0xFF;
                /* Patch ALL MOVEA.L #$00079000,Ax in ROM (A7 at $FE0400, A6 later).
                 * Pattern: xx7C 00079000 where xx = 2E (A7) or 2C (A6). */
                for (int ri = 0; ri < 0x3F00; ri += 2) {
                    if ((lisa->mem.rom[ri] == 0x2E || lisa->mem.rom[ri] == 0x2C) &&
                        lisa->mem.rom[ri+1] == 0x7C &&
                        lisa->mem.rom[ri+2] == 0x00 && lisa->mem.rom[ri+3] == 0x07 &&
                        lisa->mem.rom[ri+4] == 0x90 && lisa->mem.rom[ri+5] == 0x00) {
                        lisa->mem.rom[ri+2] = (e_us >> 24) & 0xFF;
                        lisa->mem.rom[ri+3] = (e_us >> 16) & 0xFF;
                        lisa->mem.rom[ri+4] = (e_us >> 8) & 0xFF;
                        lisa->mem.rom[ri+5] = e_us & 0xFF;
                    }
                }
                fprintf(stderr, "Boot stack patched: SSP=A6=$%06X (b_opustack=$%X+$%X)\n",
                        e_us, b_op, l_op);
            }
        }
        #undef RAM32
    }

    /* Copy MMU context 1 to context 0 (setup/start mode context).
     * The boot ROM programs segments via CXASEL which targets context 1.
     * But during setup mode, the CPU uses context 0 for all address
     * translation (except I/O and ROM). Without this copy, code that
     * toggles setup mode ON (like DO_AN_MMU in the boot loader) would
     * lose access to MMU-mapped segments since context 0 is empty. */
    for (int s = 0; s < MMU_NUM_SEGMENTS; s++) {
        if (lisa->mem.segments[1][s].changed) {
            lisa->mem.segments[0][s] = lisa->mem.segments[1][s];
        }
    }

    m68k_reset(&lisa->cpu);

    /* Set A5 to initial global data pointer (loader normally does this).
     * PASCALINIT in starasm1 expects A5 to point to the end of the
     * user-stack global area, which it copies into sysglobal. */
    lisa->cpu.a[5] = 0x14000;  /* Point into sysglobal area */

    printf("After reset: PC=$%08X SSP=$%08X A5=$%08X\n",
           lisa->cpu.pc, lisa->cpu.ssp, lisa->cpu.a[5]);

    /* Queue initial COPS data (keyboard ID) so the OS boot spin loop
     * at $520842 can proceed. The spin loop polls VIA2 port B bit 0
     * for data availability, then reads port A to get the data.
     *
     * We write directly to the queue WITHOUT triggering VIA2 CA1.
     * If we used cops_enqueue, it would set CA1 in IFR, causing an
     * immediate interrupt after INTSON. The interrupt handler can't
     * handle COPS data yet (driver not initialized), leading to an
     * infinite loop. By writing the queue directly, the data is
     * available via port B polling but no interrupt fires. */
    cops_queue_init(&lisa->cops_rx);
    lisa->cops_rx.queue[0] = 0x80;  /* Reset/status indicator */
    lisa->cops_rx.queue[1] = 0x2F;  /* Keyboard ID: US layout */
    lisa->cops_rx.count = 2;
    lisa->cops_rx.tail = 2;

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

    /* Simple sparse PC histogram (page-level) for diag frames.
     * Uses a small closed hash table — good enough for rough location. */
    #define PC_HIST_SLOTS 64
    static uint32_t pc_hist_page[PC_HIST_SLOTS];
    static uint32_t pc_hist_cnt[PC_HIST_SLOTS];
    static int pc_hist_gen = 0;
    if (pc_hist_gen != g_emu_generation) { memset(pc_hist_page, 0, sizeof(pc_hist_page)); memset(pc_hist_cnt, 0, sizeof(pc_hist_cnt)); pc_hist_gen = g_emu_generation; }

    while (cycles_this_frame < LISA_CYCLES_PER_FRAME) {
        int batch = 64;
        int executed = m68k_execute(&lisa->cpu, batch);
        cycles_this_frame += executed;
        via_tick_accum += executed;

        {
            uint32_t page = (lisa->cpu.pc & 0xFFFFFF) >> 8;  /* 256-byte pages */
            uint32_t h = (page * 2654435761u) & (PC_HIST_SLOTS - 1);
            for (int probe = 0; probe < PC_HIST_SLOTS; probe++) {
                uint32_t slot = (h + probe) & (PC_HIST_SLOTS - 1);
                if (pc_hist_cnt[slot] == 0) {
                    pc_hist_page[slot] = page;
                    pc_hist_cnt[slot] = 1;
                    break;
                }
                if (pc_hist_page[slot] == page) {
                    pc_hist_cnt[slot]++;
                    break;
                }
            }
        }

        /* VIA 6522 is clocked at Φ2 = CPU/10 (≈500 kHz on a 5 MHz Lisa).
         * Accumulate CPU cycles, then pass VIA cycles (/10) to via_tick,
         * keeping the remainder so we don't lose fractional ticks. */
        if (via_tick_accum >= 10) {
            int via_cycles = via_tick_accum / 10;
            via_tick(&lisa->via1, via_cycles);
            via_tick(&lisa->via2, via_cycles);
            via_tick_accum -= via_cycles * 10;
        }
    }

    /* Debug: log CPU/VIA/vector state once after significant execution */
    DBGSTATIC(int, frame_count, 0);
    frame_count++;

    /* Don't force-unmask interrupts or generate vretrace during init.
     * The OS must complete INITSYS before interrupt handlers are ready.
     * INTSON(0) at the end of BOOT_IO_INIT enables interrupts naturally. */
    if (frame_count == 5) {
        /* Dump the DO_AN_MMU code at mmucodemmu physical address */
        uint16_t sor84 = lisa->mem.segments[1][84].sor;
        if (sor84 != 0) {
            uint32_t phys84 = ((uint32_t)sor84 << 9) + 0x4000;
            phys84 %= LISA_RAM_SIZE;
            fprintf(stderr, "=== MMUCODEMMU CODE at phys $%06X (SOR=$%03X) ===\n", phys84, sor84);
            for (int row = 0; row < 16; row++) {
                uint32_t a = (phys84 + row * 16) % LISA_RAM_SIZE;
                fprintf(stderr, "  +$%03X:", row * 16);
                for (int i = 0; i < 16; i++)
                    fprintf(stderr, " %02X", lisa->mem.ram[(a + i) % LISA_RAM_SIZE]);
                fprintf(stderr, "\n");
            }
        }
        /* Dump memory around PC=$3015C to decode the wait loop */
        uint32_t pc = lisa->cpu.pc;
        fprintf(stderr, "=== FRAME 5 DIAGNOSTIC DUMP ===\n");
        /* Dump INIT_FREEPOOL generated code */
        fprintf(stderr, "INIT_FREEPOOL @$BD034 (64 bytes):\n");
        for (int row = 0; row < 4; row++) {
            uint32_t a = 0xBD034 + row * 16;
            fprintf(stderr, "  $%06X:", a);
            for (int i = 0; i < 16; i += 2)
                fprintf(stderr, " %02X%02X", lisa->mem.ram[a+i], lisa->mem.ram[a+i+1]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "Code $0490-$04F8:");
        for (int i = 0; i < 104; i += 2)
            fprintf(stderr, " %02X%02X", lisa->mem.ram[0x490+i], lisa->mem.ram[0x490+i+1]);
        fprintf(stderr, "\n");
        fprintf(stderr, "PC=$%06X SR=$%04X stopped=%d pending_irq=%d\n",
                pc, lisa->cpu.sr, lisa->cpu.stopped, lisa->cpu.pending_irq);

        /* Dump bytes at $30150-$30170 (physical RAM, since PC is low) */
        fprintf(stderr, "Memory $30150-$30170:");
        for (uint32_t a = 0x30150; a <= 0x30170; a += 2)
            fprintf(stderr, " %02X%02X", lisa->mem.ram[a], lisa->mem.ram[a+1]);
        fprintf(stderr, "\n");

        /* Also dump bytes around current PC */
        fprintf(stderr, "Memory @PC-8..+16 ($%06X):", pc);
        for (uint32_t a = (pc > 8 ? pc - 8 : 0); a < pc + 16; a += 2)
            fprintf(stderr, " %02X%02X", lisa->mem.ram[a], lisa->mem.ram[a+1]);
        fprintf(stderr, "\n");

        /* All CPU registers */
        fprintf(stderr, "D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                lisa->cpu.d[0], lisa->cpu.d[1], lisa->cpu.d[2], lisa->cpu.d[3]);
        fprintf(stderr, "D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                lisa->cpu.d[4], lisa->cpu.d[5], lisa->cpu.d[6], lisa->cpu.d[7]);
        fprintf(stderr, "A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n",
                lisa->cpu.a[0], lisa->cpu.a[1], lisa->cpu.a[2], lisa->cpu.a[3]);
        fprintf(stderr, "A4=$%08X A5=$%08X A6=$%08X A7=$%08X\n",
                lisa->cpu.a[4], lisa->cpu.a[5], lisa->cpu.a[6], lisa->cpu.a[7]);

        /* VIA1 and VIA2 IER/IFR state */
        fprintf(stderr, "VIA1: IER=$%02X IFR=$%02X PORTA=$%02X PORTB=$%02X DDRA=$%02X DDRB=$%02X\n",
                lisa->via1.ier, lisa->via1.ifr,
                lisa->via1.ora, lisa->via1.orb,
                lisa->via1.ddra, lisa->via1.ddrb);
        fprintf(stderr, "VIA2: IER=$%02X IFR=$%02X PORTA=$%02X PORTB=$%02X DDRA=$%02X DDRB=$%02X\n",
                lisa->via2.ier, lisa->via2.ifr,
                lisa->via2.ora, lisa->via2.orb,
                lisa->via2.ddra, lisa->via2.ddrb);
        fprintf(stderr, "=== END FRAME 5 DUMP ===\n");
    }
    /* NOTE: Pre-built Lisa OS 3.1 uses hardcoded initialization, not
     * the parameter-block/GETLDMAP mechanism. No $218 fix needed. */
    if (frame_count == 500 || frame_count == 600 || frame_count == 700 || frame_count == 800) {
        /* Dump code at stuck PC */
        uint32_t pc = lisa->cpu.pc;
        pc = lisa->cpu.pc;
        fprintf(stderr, "  CODE @$%06X: ", pc);
        for (int i = -16; i < 24; i += 2)
            fprintf(stderr, "%04X ", lisa_mem_read16(&lisa->mem, pc + i));
        /* Dump the COPS polling subroutine at $208904 */
        fprintf(stderr, "\n  COPSPOLL @$208904: ");
        for (int i = 0; i < 48; i += 2)
            fprintf(stderr, "%04X ", lisa_mem_read16(&lisa->mem, 0x208904 + i));
        fprintf(stderr, "\n  D0=$%08X D1=$%08X A0=$%08X A6=$%08X SP=$%08X\n",
                lisa->cpu.d[0], lisa->cpu.d[1], lisa->cpu.a[0], lisa->cpu.a[6], lisa->cpu.a[7]);
    }
    /* Trace POOL_INIT: check if its code was reached by frame 10 */
    if (frame_count == 10 && lisa->hle.system_error) {
        /* POOL_INIT address is at the JSR target before ADDA.W #24,SP in STARTUP */
        uint32_t pool_init_addr = 0;
        for (uint32_t a = 0x4F00; a < 0x5100 && a + 10 < LISA_RAM_SIZE; a += 2) {
            uint16_t w = (lisa->mem.ram[a] << 8) | lisa->mem.ram[a+1];
            if (w == 0x4EB9) {
                uint32_t tgt = ((uint32_t)lisa->mem.ram[a+2]<<24) | ((uint32_t)lisa->mem.ram[a+3]<<16) |
                               ((uint32_t)lisa->mem.ram[a+4]<<8) | lisa->mem.ram[a+5];
                uint16_t after = (lisa->mem.ram[a+6] << 8) | lisa->mem.ram[a+7];
                if (tgt >= 0x5000 && tgt < 0x10000 && after == 0xDEFC) {
                    uint16_t clean = (lisa->mem.ram[a+8] << 8) | lisa->mem.ram[a+9];
                    if (clean == 0x0018) { pool_init_addr = tgt; break; }
                }
            }
        }
        if (pool_init_addr) {
            /* Dump POOL_INIT function prologue to see if it was modified/reached */
            fprintf(stderr, "  POOL_INIT @$%06X (first 32 bytes): ", pool_init_addr);
            for (uint32_t i = 0; i < 32; i += 2)
                fprintf(stderr, "%04X ", lisa_mem_read16(&lisa->mem, pool_init_addr + i));
            fprintf(stderr, "\n");
            /* Check INIT_FREEPOOL address from first JSR in POOL_INIT */
            for (uint32_t a = pool_init_addr; a < pool_init_addr + 0x100; a += 2) {
                uint16_t w2 = lisa_mem_read16(&lisa->mem, a);
                if (w2 == 0x4EB9) {
                    uint32_t tgt = lisa_mem_read32(&lisa->mem, a + 2);
                    fprintf(stderr, "  POOL_INIT calls JSR $%06X at $%06X\n", tgt, a);
                    break;
                }
            }
        }
        /* Check STARTUP call chain: dump INITSYS body hex and JSR targets */
        fprintf(stderr, "  INITSYS @$4F34 first 32 bytes (phys RAM): ");
        for (int i = 0; i < 32; i += 2)
            fprintf(stderr, "%02X%02X ", lisa->mem.ram[0x4F34+i], lisa->mem.ram[0x4F34+i+1]);
        fprintf(stderr, "\n  INITSYS @$4F34 first 32 bytes (MMU read): ");
        for (int i = 0; i < 32; i += 2)
            fprintf(stderr, "%04X ", lisa_mem_read16(&lisa->mem, 0x4F34+i));
        /* Debug: verify RAM read at known JSR location $4F46 */
        fprintf(stderr, "\n  RAM[$4F46]=%02X%02X (expect 4EB9), word=%04X\n",
                lisa->mem.ram[0x4F46], lisa->mem.ram[0x4F47],
                (uint16_t)((lisa->mem.ram[0x4F46] << 8) | lisa->mem.ram[0x4F47]));
        fprintf(stderr, "  INITSYS JSR targets ($4F34-$5140):\n    ");
        int jsr_count = 0;
        for (uint32_t a = 0x4F34; a < 0x5140; a += 2) {
            uint16_t w = (uint16_t)((lisa->mem.ram[a] << 8) | lisa->mem.ram[a+1]);
            if (w == 0x4EB9) {
                uint32_t tgt = ((uint32_t)lisa->mem.ram[a+2]<<24) | ((uint32_t)lisa->mem.ram[a+3]<<16) |
                               ((uint32_t)lisa->mem.ram[a+4]<<8) | lisa->mem.ram[a+5];
                fprintf(stderr, "$%06X→$%06X  ", a, tgt);
                if (++jsr_count % 4 == 0) fprintf(stderr, "\n    ");
            }
        }
        fprintf(stderr, "(found %d JSRs)\n", jsr_count);
    }
    if (frame_count == 10 || frame_count == 60 || frame_count == 300 || frame_count == 800 || frame_count == 1500 || frame_count == 3000 || frame_count == 6000) {
        fprintf(stderr, "DIAG frame %d: PC=$%06X SR=$%04X stopped=%d pending_irq=%d setup=%d\n",
                frame_count, lisa->cpu.pc, lisa->cpu.sr, lisa->cpu.stopped,
                lisa->cpu.pending_irq, lisa->mem.setup_mode);
        /* Dump top PC histogram buckets and reset */
        {
            uint32_t top_page[6] = {0};
            uint32_t top_cnt[6] = {0};
            for (int s = 0; s < PC_HIST_SLOTS; s++) {
                if (pc_hist_cnt[s] == 0) continue;
                for (int k = 0; k < 6; k++) {
                    if (pc_hist_cnt[s] > top_cnt[k]) {
                        for (int j = 5; j > k; j--) {
                            top_page[j] = top_page[j-1];
                            top_cnt[j]  = top_cnt[j-1];
                        }
                        top_page[k] = pc_hist_page[s];
                        top_cnt[k]  = pc_hist_cnt[s];
                        break;
                    }
                }
            }
            fprintf(stderr, "  PC hot pages:");
            for (int k = 0; k < 6 && top_cnt[k] > 0; k++)
                fprintf(stderr, " $%06X×%u", top_page[k] << 8, top_cnt[k]);
            fprintf(stderr, "\n");
            for (int s = 0; s < PC_HIST_SLOTS; s++) {
                pc_hist_cnt[s] = 0;
                pc_hist_page[s] = 0;
            }
        }
        /* VEC-HIST: top exception vectors since last DIAG frame, then reset. */
        {
            int top_v[6] = {-1,-1,-1,-1,-1,-1};
            int top_c[6] = {0};
            for (int v = 0; v < 256; v++) {
                int c = m68k_exception_histogram[v];
                if (c == 0) continue;
                for (int k = 0; k < 6; k++) {
                    if (c > top_c[k]) {
                        for (int j = 5; j > k; j--) { top_v[j]=top_v[j-1]; top_c[j]=top_c[j-1]; }
                        top_v[k] = v; top_c[k] = c;
                        break;
                    }
                }
            }
            fprintf(stderr, "  VEC-HIST:");
            for (int k = 0; k < 6 && top_c[k] > 0; k++)
                fprintf(stderr, " v%d×%d", top_v[k], top_c[k]);
            fprintf(stderr, "\n");
            for (int v = 0; v < 256; v++) m68k_exception_histogram[v] = 0;
        }
        /* TRAP #5 selector histogram: which HW interface routine is hot? */
        {
            int top_s[6] = {-1,-1,-1,-1,-1,-1};
            int top_c[6] = {0};
            for (int s = 0; s < 256; s++) {
                int c = m68k_trap5_selector_histogram[s];
                if (c == 0) continue;
                for (int k = 0; k < 6; k++) {
                    if (c > top_c[k]) {
                        for (int j = 5; j > k; j--) { top_s[j]=top_s[j-1]; top_c[j]=top_c[j-1]; }
                        top_s[k] = s; top_c[k] = c;
                        break;
                    }
                }
            }
            fprintf(stderr, "  TRAP5-SEL:");
            for (int k = 0; k < 6 && top_c[k] > 0; k++)
                fprintf(stderr, " d7=%d×%d", top_s[k], top_c[k]);
            fprintf(stderr, "\n");
            for (int s = 0; s < 256; s++) m68k_trap5_selector_histogram[s] = 0;
        }
        /* Sysglobal pool header at logical $CC0000+$A000 — look for forward progress. */
        {
            uint32_t base = 0xCC0000 + 0xA000;
            fprintf(stderr, "  SGHEAP @$%06X:", base);
            for (int i = 0; i < 32; i += 4) {
                uint32_t w = lisa_mem_read32(&lisa->mem, base + i);
                fprintf(stderr, " %08X", w);
            }
            fprintf(stderr, "\n");
        }
        /* Screen-address globals (libhw-DRIVERS equs: $110 alt, $160 main, $170/$174 phys). */
        {
            uint32_t scrn_log = lisa_mem_read32(&lisa->mem, 0x160);
            uint32_t altscrn_log = lisa_mem_read32(&lisa->mem, 0x110);
            uint32_t scrn_phy = lisa_mem_read32(&lisa->mem, 0x174);
            uint32_t altscrn_phy = lisa_mem_read32(&lisa->mem, 0x170);
            fprintf(stderr, "  SCRN log=$%08X alt=$%08X phys=$%08X altphys=$%08X\n",
                    scrn_log, altscrn_log, scrn_phy, altscrn_phy);
        }
        fprintf(stderr, "  VIA1: t1_run=%d t1_cnt=%d t1_latch=%d ier=$%02X ifr=$%02X\n",
                lisa->via1.t1_running, lisa->via1.t1_counter, lisa->via1.t1_latch,
                lisa->via1.ier, lisa->via1.ifr);
        fprintf(stderr, "  VIA2: t1_run=%d t1_cnt=%d t1_latch=%d ier=$%02X ifr=$%02X\n",
                lisa->via2.t1_running, lisa->via2.t1_counter, lisa->via2.t1_latch,
                lisa->via2.ier, lisa->via2.ifr);
        /* Test MMU write at runtime */
        if (frame_count == 10) {
            /* Write $AB to logical $CC0000, check translated physical address.
             * sysglobmmu(102) SOR maps $CC0000 to physical b_sysglobal. */
            uint16_t sor102 = lisa->mem.segments[1][102].sor;
            uint32_t expected_phys = (uint32_t)sor102 << 9;
            lisa_mem_write8(&lisa->mem, 0xCC0000, 0xAB);
            uint8_t got = (expected_phys < LISA_RAM_SIZE) ? lisa->mem.ram[expected_phys] : 0;
            fprintf(stderr, "  MMU WRITE TEST: wrote $AB to $CC0000, phys $%06X = $%02X (%s)\n",
                    expected_phys, got, got == 0xAB ? "PASS" : "FAIL");
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

            /* Decode instruction at current PC */
            uint32_t pc = lisa->cpu.pc;
            fprintf(stderr, "=== FRAME 60 INSTRUCTION DECODE at PC=$%06X ===\n", pc);
            fprintf(stderr, "  Bytes at PC-16..+32:");
            for (uint32_t a = (pc > 16 ? pc - 16 : 0); a < pc + 32 && a < LISA_RAM_SIZE; a += 2) {
                if (a == pc) fprintf(stderr, " [");
                fprintf(stderr, "%02X%02X", lisa->mem.ram[a], lisa->mem.ram[a+1]);
                if (a == pc) fprintf(stderr, "]");
                else fprintf(stderr, " ");
            }
            fprintf(stderr, "\n");

            /* Decode the opcode word */
            uint16_t opcode = (lisa->mem.ram[pc] << 8) | lisa->mem.ram[pc+1];
            fprintf(stderr, "  Opcode=$%04X", opcode);
            if (opcode == 0x4E72) {
                uint16_t imm = (lisa->mem.ram[pc+2] << 8) | lisa->mem.ram[pc+3];
                fprintf(stderr, " → STOP #$%04X (waiting for interrupt level > %d)\n",
                        imm, (imm >> 8) & 7);
            } else if ((opcode & 0xFFC0) == 0x0800) {
                /* BTST #n,<ea> - static bit test */
                uint16_t bit = (lisa->mem.ram[pc+2] << 8) | lisa->mem.ram[pc+3];
                fprintf(stderr, " → BTST #%d,...\n", bit & 0x1F);
            } else if ((opcode & 0xF1C0) == 0x0100) {
                /* BTST Dn,<ea> - dynamic bit test */
                int reg = (opcode >> 9) & 7;
                fprintf(stderr, " → BTST D%d,...\n", reg);
            } else if ((opcode & 0xFF00) == 0x4A00) {
                /* TST */
                fprintf(stderr, " → TST\n");
            } else if ((opcode & 0xF000) == 0x6000) {
                /* Bcc */
                int cond = (opcode >> 8) & 0xF;
                const char *cc[] = {"BRA","BSR","BHI","BLS","BCC","BCS","BNE","BEQ",
                                    "BVC","BVS","BPL","BMI","BGE","BLT","BGT","BLE"};
                int8_t disp8 = opcode & 0xFF;
                if (disp8 == 0) {
                    int16_t disp16 = (int16_t)((lisa->mem.ram[pc+2] << 8) | lisa->mem.ram[pc+3]);
                    fprintf(stderr, " → %s.W $%06X (disp=%d)\n", cc[cond], pc + 2 + disp16, disp16);
                } else {
                    fprintf(stderr, " → %s.S $%06X (disp=%d)\n", cc[cond], pc + 2 + disp8, disp8);
                }
            } else if ((opcode & 0xFFF8) == 0x4E50) {
                /* LINK */
                fprintf(stderr, " → LINK A%d\n", opcode & 7);
            } else if (opcode == 0x4E75) {
                fprintf(stderr, " → RTS\n");
            } else if (opcode == 0x4E73) {
                fprintf(stderr, " → RTE\n");
            } else {
                fprintf(stderr, " → (unknown pattern, check 68000 manual)\n");
            }

            /* All registers at frame 60 */
            fprintf(stderr, "  D0=$%08X D1=$%08X D2=$%08X D3=$%08X\n",
                    lisa->cpu.d[0], lisa->cpu.d[1], lisa->cpu.d[2], lisa->cpu.d[3]);
            fprintf(stderr, "  D4=$%08X D5=$%08X D6=$%08X D7=$%08X\n",
                    lisa->cpu.d[4], lisa->cpu.d[5], lisa->cpu.d[6], lisa->cpu.d[7]);
            fprintf(stderr, "  A0=$%08X A1=$%08X A2=$%08X A3=$%08X\n",
                    lisa->cpu.a[0], lisa->cpu.a[1], lisa->cpu.a[2], lisa->cpu.a[3]);
            fprintf(stderr, "  A4=$%08X A5=$%08X A6=$%08X A7=$%08X\n",
                    lisa->cpu.a[4], lisa->cpu.a[5], lisa->cpu.a[6], lisa->cpu.a[7]);
            fprintf(stderr, "  VIA1: IER=$%02X IFR=$%02X ORA=$%02X ORB=$%02X\n",
                    lisa->via1.ier, lisa->via1.ifr, lisa->via1.ora, lisa->via1.orb);
            fprintf(stderr, "  VIA2: IER=$%02X IFR=$%02X ORA=$%02X ORB=$%02X\n",
                    lisa->via2.ier, lisa->via2.ifr, lisa->via2.ora, lisa->via2.orb);
            fprintf(stderr, "=== END FRAME 60 DECODE ===\n");
        }
        if (frame_count == 10) {
            /* Dump pool state after POOL_INIT.
             * sysglobal at $CC0000, expected pool header written by POOL_INIT. */
            int ctx = lisa->mem.current_context;
            fprintf(stderr, "  MMU state: enabled=%d ctx=%d seg102: sor=$%03X seg101: sor=$%03X seg123: sor=$%03X\n",
                    lisa->mem.mmu_enabled, ctx,
                    lisa->mem.segments[ctx][102].sor,
                    lisa->mem.segments[ctx][101].sor,
                    lisa->mem.segments[ctx][123].sor);
            /* Dump first 64 bytes of sysglobal pool (pool header) */
            fprintf(stderr, "  sysglobal pool @$CC0000 (first 64 bytes):\n");
            for (int row = 0; row < 4; row++) {
                uint32_t base = 0xCC0000 + row * 16;
                fprintf(stderr, "    $%06X:", base);
                for (int i = 0; i < 16; i += 2) {
                    fprintf(stderr, " %04X",
                            lisa_mem_read16(&lisa->mem, base + i));
                }
                fprintf(stderr, "\n");
            }
            /* Also check physical RAM directly */
            uint16_t sor102 = lisa->mem.segments[ctx][102].sor;
            uint32_t phys_sg = (uint32_t)sor102 << 9;
            fprintf(stderr, "  physical sysglobal @$%06X (first 32 bytes):\n    ", phys_sg);
            for (int i = 0; i < 32; i++) {
                fprintf(stderr, "%02X ", lisa->mem.ram[phys_sg + i]);
                if (i == 15) fprintf(stderr, "\n    ");
            }
            fprintf(stderr, "\n");
            /* Check pool header area at sgheap (offset $A000 into sysglobal) */
            fprintf(stderr, "  physical sgheap @$%06X (first 32 bytes):\n    ", phys_sg + 0xA000);
            for (int i = 0; i < 32; i++) {
                fprintf(stderr, "%02X ", lisa->mem.ram[phys_sg + 0xA000 + i]);
                if (i == 15) fprintf(stderr, "\n    ");
            }
            fprintf(stderr, "\n");
            /* Check A5 region (globals, negative offsets from A5) */
            fprintf(stderr, "  A5-relative globals (A5=$CC5FFC, physical @$%06X):\n", phys_sg + 0x5FFC);
            fprintf(stderr, "    A5-8: %08X  A5-4: %08X  A5+0: %08X\n",
                    lisa_mem_read32(&lisa->mem, 0xCC5FFC - 8),
                    lisa_mem_read32(&lisa->mem, 0xCC5FFC - 4),
                    lisa_mem_read32(&lisa->mem, 0xCC5FFC));
            /* Dump specific key globals using PASCALDEFS offsets (A5-relative) */
            uint32_t a5v = 0xCC6FFC; /* expected A5 from boot setup */
            fprintf(stderr, "  KEY GLOBALS (A5=$%06X, PASCALDEFS offsets):\n", a5v);
            fprintf(stderr, "    SGLOBAL @$200:                 $%08X\n",
                    lisa_mem_read32(&lisa->mem, 0x200));
            fprintf(stderr, "    sg_free_pool_addr (A5-24575):  $%08X\n",
                    lisa_mem_read32(&lisa->mem, a5v - 24575));
            fprintf(stderr, "    size_sglobal      (A5-24577):  $%04X\n",
                    lisa_mem_read16(&lisa->mem, a5v - 24577));
            fprintf(stderr, "    c_pcb_ptr         (A5-24617):  $%08X\n",
                    lisa_mem_read32(&lisa->mem, a5v - 24617));
            fprintf(stderr, "    b_syslocal_ptr    (A5-24785):  $%08X\n",
                    lisa_mem_read32(&lisa->mem, a5v - 24785));
            fprintf(stderr, "    mmrb_addr         (A5-25691):  $%08X\n",
                    lisa_mem_read32(&lisa->mem, a5v - 25691));
            fprintf(stderr, "    invoke_sched      (A5-24786):  $%02X\n",
                    lisa_mem_read8(&lisa->mem, a5v - 24786));
        }
        /* Check both framebuffers (main $1F8000, alt $1F0000). */
        {
            uint32_t main_addr = 2 * 1024 * 1024 - 0x8000;
            uint32_t alt_addr  = 2 * 1024 * 1024 - 0x10000;
            int nz_main = 0, nz_alt = 0;
            for (int i = 0; i < LISA_SCREEN_BYTES; i++) {
                if (main_addr + i < LISA_RAM_SIZE && lisa->mem.ram[main_addr + i] != 0x00) nz_main++;
                if (alt_addr  + i < LISA_RAM_SIZE && lisa->mem.ram[alt_addr  + i] != 0x00) nz_alt++;
            }
            fprintf(stderr, "  Screen main=%d/%d alt=%d/%d (active=%s)\n",
                    nz_main, LISA_SCREEN_BYTES, nz_alt, LISA_SCREEN_BYTES,
                    lisa->mem.video_alt ? "alt" : "main");
            /* At frame 800, render a simple ASCII thumbnail of whichever FB
             * has content so we can *see* what the OS is drawing. */
            if (frame_count == 800 || frame_count == 1500 || frame_count == 3000) {
                uint32_t dump_addr = nz_alt > nz_main ? alt_addr : main_addr;
                const char *which  = nz_alt > nz_main ? "ALT" : "MAIN";
                char path[128];
                snprintf(path, sizeof(path),
                         "./.claude-tmp/screen-frame%04d-%s.pbm",
                         frame_count, which);
                FILE *f = fopen(path, "wb");
                if (f) {
                    /* PBM P4 binary: width height, then packed bits.
                     * Lisa FB is 720×364 already bit-packed with MSB-first
                     * per-row and byte stride 90. A Lisa "1" bit means
                     * "black pixel"; PBM "1" also means "black" — match. */
                    fprintf(f, "P4\n%d %d\n", LISA_SCREEN_WIDTH, LISA_SCREEN_HEIGHT);
                    fwrite(&lisa->mem.ram[dump_addr],
                           1, LISA_SCREEN_BYTES, f);
                    fclose(f);
                    fprintf(stderr, "  SCREEN saved %s (%d non-zero bytes)\n",
                            path, which[0] == 'A' ? nz_alt : nz_main);
                }
            }
        }
        /* Check key exception vectors in RAM */
        uint32_t trap1_vec = ((uint32_t)lisa->mem.ram[0x84] << 24) |
                             ((uint32_t)lisa->mem.ram[0x85] << 16) |
                             ((uint32_t)lisa->mem.ram[0x86] << 8) |
                             lisa->mem.ram[0x87];
        uint32_t int1_vec = ((uint32_t)lisa->mem.ram[0x64] << 24) |
                            ((uint32_t)lisa->mem.ram[0x65] << 16) |
                            ((uint32_t)lisa->mem.ram[0x66] << 8) |
                            lisa->mem.ram[0x67];
        uint32_t int2_vec = ((uint32_t)lisa->mem.ram[0x68] << 24) |
                            ((uint32_t)lisa->mem.ram[0x69] << 16) |
                            ((uint32_t)lisa->mem.ram[0x6A] << 8) |
                            lisa->mem.ram[0x6B];
        fprintf(stderr, "  Vectors (RAM): TRAP1=$%08X INT1=$%08X INT2=$%08X\n",
                trap1_vec, int1_vec, int2_vec);
        /* Also read via CPU path to compare */
        uint32_t trap1_cpu = lisa_mem_read32(&lisa->mem, 0x84);
        fprintf(stderr, "  Vectors (CPU): TRAP1=$%08X  SGLOBAL@$200=$%08X\n",
                trap1_cpu,
                lisa_mem_read32(&lisa->mem, 0x200));
    }

    /* FORCE_UNMASK removed: with proper interrupt levels (VIA1=level 1,
     * VIA2=level 2, vretrace=level 1), interrupts should be delivered
     * naturally. Force-lowering IPL inside an existing handler caused
     * nested interrupt cascades. */

    /* Vertical retrace: pulse the IRQ for one instruction only.
     * Only enable after the OS has initialized interrupt handlers
     * (frame_count > 200 gives INITSYS time to complete). */
    if (lisa->mem.vretrace_enabled && frame_count > 200) {
        lisa->mem.vretrace_irq = true;
        lisa->irq_vretrace = 1;
        int level = 1;  /* vretrace is IRQ level 1 on Lisa */
        if (lisa->irq_via2) level = 2;  /* VIA2/COPS is IRQ level 2 on Lisa */
        m68k_set_irq(&lisa->cpu, level);

        /* Execute one instruction — enough for the CPU to take the IRQ */
        m68k_execute(&lisa->cpu, 50);

        /* Immediately clear */
        lisa->mem.vretrace_irq = false;
        lisa->irq_vretrace = 0;
        level = 0;
        if (lisa->irq_via1) level = 1;
        if (lisa->irq_via2) level = 2;  /* VIA2/COPS is IRQ level 2 on Lisa */
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
 *   Key down: keycode with bit 7 SET   ($80 | keycode)
 *   Key up:   keycode with bit 7 CLEAR (keycode & $7F)
 *
 * See Lisa_Source/LISA_OS/LIBS/LIBHW/libhw-DRIVERS.TEXT (COPS0 handler):
 *     AND.W  #$7F,D0      ; keycode
 *     AND.W  #$80,D1      ; $00=up, $80=down
 */
void lisa_key_down(lisa_t *lisa, int keycode) {
    if (keycode < 0 || keycode > 127) return;
    lisa->keys_down[keycode] = true;
    cops_enqueue(&lisa->cops_rx, (uint8_t)(keycode | 0x80));
    via_trigger_ca1(&lisa->via2);
}

void lisa_key_up(lisa_t *lisa, int keycode) {
    if (keycode < 0 || keycode > 127) return;
    lisa->keys_down[keycode] = false;
    cops_enqueue(&lisa->cops_rx, (uint8_t)(keycode & 0x7F));
    via_trigger_ca1(&lisa->via2);
}

/*
 * Mouse movement: COPS sends a 3-byte packet —
 *   byte 0: $00  (header: "mouse data follows", per libhw-DRIVERS COPS0/@4)
 *   byte 1: Dx   (signed 8-bit delta)
 *   byte 2: Dy   (signed 8-bit delta)
 *
 * WITHOUT the $00 header, the COPS0 handler takes the "keycode" branch
 * on the first byte (bit 7 = up/down, bits 0..6 = keycode), pumping
 * phantom keystrokes into Lisa OS on every mouse motion.
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

    /* 3-byte packet: header $00, then Dx, Dy */
    cops_enqueue(&lisa->cops_rx, 0x00);
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

/* ========================================================================
 * HLE (High-Level Emulation) — Disk I/O Bypass
 *
 * Intercepts OS disk I/O functions at the CPU level and performs reads/writes
 * directly from/to the disk image, bypassing the ProFile driver/VIA path.
 *
 * Inspired by LisaEm's hle.c (Ray Arachelian, GPLv2) which patches byte-level
 * transfer loops. Our approach intercepts at CALLDRIVER instead, since our
 * cross-compiled binary has different addresses than LisaEm's LOS 3.1 offsets.
 *
 * CALLDRIVER stack (from SOURCE-MOVER.TEXT):
 *   SP+0:  return address
 *   SP+4:  parameters ptr (param_ptr)
 *   SP+8:  config_ptr (ptrdevrec)
 *   SP+12: errnum ptr (var integer)
 * ======================================================================== */

#define HLE_DINTERRUPT  0
#define HLE_DINIT       1
#define HLE_DDOWN       2
#define HLE_DSKUNCLAMP  3
#define HLE_DSKFORMAT   4
#define HLE_SEQIO       5
#define HLE_DSKIO       6
#define HLE_DCONTROL    7
#define HLE_REQRESTART  8
#define HLE_DDISCON     9
#define HLE_DATTACH     12
#define HLE_HDINIT      13
#define HLE_HDSKIO      14
#define HLE_DUNATTACH   16
#define HLE_DALARMS     17
#define HLE_HDDOWN      18

/* Request block field offsets (from driverdefs reqblk record) */
#define REQ_OPERATN     12
#define REQ_BUFF_ADDR   16
#define REQ_LENGTH      20
#define REQ_DISK_ADDR   22

void lisa_hle_set_addresses(lisa_t *lisa, uint32_t calldriver, uint32_t call_hdisk,
                            uint32_t hdiskio, uint32_t prodriver,
                            uint32_t system_error, uint32_t badcall,
                            uint32_t parallel, uint32_t use_hdisk) {
    lisa->hle.calldriver = calldriver;
    lisa->hle.call_hdisk = call_hdisk;
    lisa->hle.hdiskio = hdiskio;
    lisa->hle.prodriver = prodriver;
    lisa->hle.system_error = system_error;
    lisa->hle.badcall = badcall;
    lisa->hle.parallel = parallel;
    lisa->hle.use_hdisk = use_hdisk;
    lisa->hle.active = (calldriver != 0 || system_error != 0);
    lisa->hle.boot_config_done = false;
    lisa->hle.reads = 0;
    lisa->hle.writes = 0;
    if (lisa->hle.active) {
        fprintf(stderr, "HLE: Disk I/O intercepts ACTIVE\n");
        fprintf(stderr, "  CALLDRIVER=$%06X  CALL_HDISK=$%06X\n", calldriver, call_hdisk);
        fprintf(stderr, "  HDISKIO=$%06X  PRODRIVER=$%06X\n", hdiskio, prodriver);
        fprintf(stderr, "  SYSTEM_ERROR=$%06X  BADCALL=$%06X\n", system_error, badcall);
        fprintf(stderr, "  PARALLEL=$%06X  USE_HDISK=$%06X\n", parallel, use_hdisk);
    }
}

static int hle_read_block(lisa_t *lisa, uint32_t block_num,
                          uint8_t *data_out, uint8_t *tag_out) {
    if (!lisa->profile.mounted || !lisa->profile.data) return -1;
    size_t offset = (size_t)block_num * PROFILE_BLOCK_SIZE;
    if (offset + PROFILE_BLOCK_SIZE > lisa->profile.data_size) return -2;
    if (tag_out)  memcpy(tag_out, lisa->profile.data + offset, PROFILE_TAG_SIZE);
    if (data_out) memcpy(data_out, lisa->profile.data + offset + PROFILE_TAG_SIZE, PROFILE_DATA_SIZE);
    return 0;
}

static int hle_write_block(lisa_t *lisa, uint32_t block_num,
                           const uint8_t *data_in, const uint8_t *tag_in) {
    if (!lisa->profile.mounted || !lisa->profile.data) return -1;
    size_t offset = (size_t)block_num * PROFILE_BLOCK_SIZE;
    if (offset + PROFILE_BLOCK_SIZE > lisa->profile.data_size) return -2;
    if (tag_in)  memcpy(lisa->profile.data + offset, tag_in, PROFILE_TAG_SIZE);
    if (data_in) memcpy(lisa->profile.data + offset + PROFILE_TAG_SIZE, data_in, PROFILE_DATA_SIZE);
    return 0;
}

static bool hle_handle_calldriver(lisa_t *lisa, m68k_t *cpu) {
    uint32_t ret_addr   = cpu_read32(cpu, cpu->a[7]);
    uint32_t params_ptr = cpu_read32(cpu, cpu->a[7] + 4);
    uint32_t config_ptr = cpu_read32(cpu, cpu->a[7] + 8);
    uint32_t errnum_ptr = cpu_read32(cpu, cpu->a[7] + 12);

    int16_t fnctn_code = (int16_t)cpu_read16(cpu, params_ptr + 4);

    /* For nil config_ptr: only intercept disk-related functions.
     * Non-disk calls (SCC, keyboard) should use original error path. */
    if (config_ptr == 0) {
        if (fnctn_code != HLE_DINIT && fnctn_code != HLE_HDINIT &&
            fnctn_code != HLE_DSKIO && fnctn_code != HLE_HDSKIO &&
            fnctn_code != HLE_DSKUNCLAMP && fnctn_code != HLE_DDOWN &&
            fnctn_code != HLE_HDDOWN && fnctn_code != HLE_DINTERRUPT &&
            fnctn_code != HLE_DALARMS && fnctn_code != HLE_DCONTROL)
            return false;
    }

    /* If config_ptr points to BADCALL device (bitbucket), intercept disk functions.
     * Let real driver calls through. */
    if (config_ptr != 0 && lisa->hle.badcall != 0) {
        uint32_t entry_pt = cpu_read32(cpu, config_ptr);
        if (entry_pt != 0 && entry_pt != lisa->hle.badcall &&
            entry_pt != 0x000003F0)  /* stub address */
            return false;  /* Real driver, let OS handle */
    }

    DBGSTATIC(int, hle_trace, 0);
    if (hle_trace < 50) {
        hle_trace++;
        fprintf(stderr, "HLE CALLDRIVER: fnctn=%d config=$%06X params=$%06X\n",
                fnctn_code, config_ptr, params_ptr);
    }

    int16_t error = 0;

    switch (fnctn_code) {
    case HLE_DINIT:
        fprintf(stderr, "HLE: dinit → success\n");
        break;
    case HLE_HDINIT:
        fprintf(stderr, "HLE: hdinit → success\n");
        break;
    case HLE_DSKIO: {
        uint32_t req_ptr = cpu_read32(cpu, params_ptr + 6);
        if (req_ptr == 0) { error = 605; break; }
        int16_t operatn   = (int16_t)cpu_read16(cpu, req_ptr + REQ_OPERATN);
        uint32_t buf_addr = cpu_read32(cpu, req_ptr + REQ_BUFF_ADDR);
        int16_t length    = (int16_t)cpu_read16(cpu, req_ptr + REQ_LENGTH);
        uint32_t block_no = cpu_read32(cpu, req_ptr + REQ_DISK_ADDR);
        if (hle_trace < 200)
            fprintf(stderr, "HLE dskio: op=%d block=%u buf=$%06X len=%d\n",
                    operatn, block_no, buf_addr, length);
        if (operatn == 0) {
            uint8_t data[PROFILE_DATA_SIZE];
            int rc = hle_read_block(lisa, block_no, data, NULL);
            if (rc == 0) {
                for (int i = 0; i < PROFILE_DATA_SIZE && i < length; i++)
                    cpu->write8(mask_24(buf_addr + i), data[i]);
                lisa->hle.reads++;
            } else { error = 654; }
        } else if (operatn == 1) {
            uint8_t data[PROFILE_DATA_SIZE];
            for (int i = 0; i < PROFILE_DATA_SIZE && i < length; i++)
                data[i] = cpu->read8(mask_24(buf_addr + i));
            int rc = hle_write_block(lisa, block_no, data, NULL);
            if (rc == 0) { lisa->hle.writes++; }
            else { error = 654; }
        } else { error = 605; }
        break;
    }
    case HLE_HDSKIO:
        fprintf(stderr, "HLE: hdskio → success\n");
        break;
    case HLE_DDOWN: case HLE_HDDOWN: case HLE_DSKUNCLAMP:
    case HLE_DINTERRUPT: case HLE_DALARMS: case HLE_DCONTROL:
        break;
    default:
        if (hle_trace < 200)
            fprintf(stderr, "HLE: unhandled fnctn=%d, passing through\n", fnctn_code);
        return false;
    }

    /* Simulate CALLDRIVER return sequence */
    cpu->a[7] += 4;   /* skip return address */
    cpu->a[7] += 8;   /* skip params_ptr + config_ptr */
    errnum_ptr = cpu_read32(cpu, cpu->a[7]);
    cpu->a[7] += 4;
    cpu->write16(mask_24(errnum_ptr), (uint16_t)error);
    cpu->pc = ret_addr;
    cpu->cycles += 40;
    return true;
}

/* Handle SYSTEM_ERROR intercept.
 * SYSTEM_ERROR(integer) is called as: procedure SYSTEM_ERROR(err: integer).
 * Lisa Pascal calling convention: parameter is on stack above return address.
 * Stack: [return_addr(4)] [err(2)]
 * SYSTEM_ERROR never returns normally, but our stub at $3F0 does return. */
static bool hle_handle_system_error(lisa_t *lisa __attribute__((unused)), m68k_t *cpu) {
    uint32_t ret_addr = cpu_read32(cpu, cpu->a[7]);
    int16_t err_code = (int16_t)cpu_read16(cpu, cpu->a[7] + 4);

    /* Log all SYSTEM_ERROR calls with stack context */
    DBGSTATIC(int, se_trace, 0);
    if (se_trace < 10) {
        se_trace++;
        fprintf(stderr, "HLE SYSTEM_ERROR(%d) at ret=$%06X SP=$%08X A6=$%08X\n",
                err_code, ret_addr, cpu->a[7], cpu->a[6]);
        fprintf(stderr, "  Stack: %08X %08X %08X %08X\n",
                cpu_read32(cpu, cpu->a[7]),
                cpu_read32(cpu, cpu->a[7]+4),
                cpu_read32(cpu, cpu->a[7]+8),
                cpu_read32(cpu, cpu->a[7]+12));
        fprintf(stderr, "  PC=$%06X D0=$%08X D1=$%08X D2=$%08X\n",
                cpu->pc, cpu->d[0], cpu->d[1], cpu->d[2]);
        fprintf(stderr, "  A0=$%08X A5=$%08X A6=$%08X\n",
                cpu->a[0], cpu->a[5], cpu->a[6]);
    }

    /* Error 10738 = stup_find_boot: can't find boot CD in pram.
     * This is normal for us — we bypass pram entirely.
     * Suppress the error and return to let INIT_BOOT_CDS continue.
     * The EXIT(init_boot_cds) after SYSTEM_ERROR will return from the procedure. */
    if (err_code >= 10738 && err_code <= 10741) {
        fprintf(stderr, "HLE: Suppressing SYSTEM_ERROR(%d) — boot device handled by HLE\n",
                err_code);
        /* Simulate: pop return address + pop parameter, return */
        cpu->a[7] += 4;  /* return address */
        cpu->a[7] += 2;  /* err parameter (int2) */
        cpu->pc = ret_addr;
        cpu->cycles += 20;
        return true;
    }
    /* P76: Error 10707 = stup_fsinit. With P75's for-loop fix unlocked
     * FS_INIT naturally; its body runs FS_MASTER_INIT (fails, no real
     * disk) → SYSTEM_ERROR(10707) → suppress → return to repeat-loop
     * → loop check `until error=0` fails → infinite loop. Fix by
     * exiting the enclosing procedure directly: pop the full stack up
     * through FS_INIT's frame and RTS back to its caller. */
    if (err_code == 10707) {
        fprintf(stderr, "HLE: Suppressing SYSTEM_ERROR(%d) — unwind to FS_INIT caller\n",
                err_code);
        /* Walk back up to find the frame where FS_INIT was called.
         * We pop retPC + err arg + repeatedly unwind A6 link chain
         * until we find a return address outside FS_INIT. FS_INIT is
         * at $0026F4..~$0027D0 per linker map; its caller return is
         * in BOOT_IO_INIT. */
        uint32_t fs_init_pc = 0x0026F4;
        uint32_t fs_init_end = 0x002800;
        cpu->a[7] += 4 + 2;  /* pop SYSTEM_ERROR retPC + err */
        /* Unwind A6 chain until saved retPC is outside FS_INIT. */
        for (int i = 0; i < 8; i++) {
            uint32_t a6 = cpu->a[6] & 0xFFFFFF;
            if (a6 < 0x400 || a6 >= 0xFE0000) break;
            uint32_t saved_a6 = lisa_mem_read32(&lisa->mem, a6);
            uint32_t saved_ret = lisa_mem_read32(&lisa->mem, a6 + 4);
            if (saved_ret < fs_init_pc || saved_ret >= fs_init_end) {
                /* Outside FS_INIT — this is the caller frame. Unwind. */
                cpu->a[7] = (cpu->a[7] & 0xFF000000) | ((a6 + 8) & 0xFFFFFF);
                cpu->a[6] = saved_a6;
                cpu->pc = saved_ret;
                cpu->cycles += 40;
                return true;
            }
            cpu->a[6] = saved_a6;
        }
        /* Fallback: plain SYSTEM_ERROR suppress */
        cpu->pc = ret_addr;
        cpu->cycles += 20;
        return true;
    }

    /* P80d: hard exception during SYS_PROC_INIT — unwind past the entire
     * SYS_PROC_INIT by walking the A6 chain to find the STARTUP return
     * address. SYS_PROC_INIT's process creation code hits a frame pointer
     * corruption in SEG_IO during FinishCreate/CreateProcess, causing a
     * bus error → hard_excep → SYSTEM_ERROR(10201). Instead of trying to
     * return to the corrupt context, skip SYS_PROC_INIT entirely and let
     * the boot continue with INIT_DRIVER_SPACE. */
    extern int g_vec_guard_active;
    if (g_vec_guard_active && (err_code == 10201 || err_code == 10204)) {
        static int supp_10201 = 0;
        if (supp_10201++ < 10) {
            fprintf(stderr, "HLE: SYSTEM_ERROR(%d) during SYS_PROC_INIT — unwinding to STARTUP\n",
                    err_code);
        }
        /* Restore SP to the boot stack level (before SYS_PROC_INIT was called).
         * Walk the supervisor stack to find a valid return frame. */
        uint32_t boot_sp = 0xCBFFFC;  /* SP at SYS_PROC_INIT entry from P80 diag */
        cpu->a[7] = boot_sp;
        /* Pop the SYS_PROC_INIT return address and continue */
        uint32_t ret = cpu_read32(cpu, boot_sp);
        cpu->a[7] = boot_sp + 4;
        cpu->pc = ret;
        /* Restore A6 to the STARTUP frame */
        cpu->a[6] = cpu_read32(cpu, boot_sp - 4);  /* saved A6 is below SP */
        cpu->cycles += 50;
        return true;
    }

    /* SYSTEM_ERROR should halt — it never returns on a real Lisa.
     * Stop the CPU to prevent infinite recursion. */
    fprintf(stderr, "SYSTEM_ERROR(%d): HALTING CPU\n", err_code);
    cpu->stopped = true;
    /* Dump the boot-progress report once on first halt so we can see
     * how far we got before failing. */
    static bool reported_once = false;
    if (!reported_once) {
        reported_once = true;
        boot_progress_report(stderr);
    }
    return true;
}

/* prof_entry intercept — read a ProFile block directly from disk image.
 * Interface: D1=sector(interleaved), A1=tag dest(20b), A2=data dest(512b) */
static bool hle_prof_entry(lisa_t *lisa, m68k_t *cpu) {
    uint32_t sector = cpu->d[1];
    uint32_t tag_dest = cpu->a[1] & 0xFFFFFF;
    uint32_t data_dest = cpu->a[2] & 0xFFFFFF;

    /* The LDPROF interleave routine converts logical→physical sector numbers
     * before calling prof_entry. The disk image stores blocks in physical
     * order. So the sector number we receive IS the physical block number —
     * no deinterleaving needed. */

    DBGSTATIC(int, prof_reads, 0);
    if (prof_reads < 5)
        fprintf(stderr, "PROF_ENTRY[%d]: sector %u → tag@$%06X data@$%06X\n",
                prof_reads, sector, tag_dest, data_dest);
    prof_reads++;

    /* Read from mounted ProFile image */
    if (!lisa->prof.mounted || !lisa->prof.data) {
        cpu->sr |= 1;  /* Set carry = error */
        cpu->pc += 2;   /* Skip NOP, hit RTS */
        return true;
    }

    uint32_t total_blocks = (uint32_t)(lisa->prof.data_size / 532);

    /* Special block $FFFFFF: spare table / drive identity */
    uint8_t spare_block[532];
    uint8_t *block;
    if (sector >= 0xFFFFF0) {
        memset(spare_block, 0, sizeof(spare_block));
        /* Tag: all zeros */
        /* Data at offset 20: device info */
        spare_block[20] = 0x00;  /* Device type: ProFile */
        spare_block[21] = 0x00;
        spare_block[22] = 0x00;
        spare_block[23] = 0x00;
        /* Number of blocks (3 bytes, big-endian) */
        spare_block[24] = (total_blocks >> 16) & 0xFF;
        spare_block[25] = (total_blocks >> 8) & 0xFF;
        spare_block[26] = total_blocks & 0xFF;
        /* Bytes per block: 532 */
        spare_block[27] = 0x02;
        spare_block[28] = 0x14;
        /* Spares: 0 */
        spare_block[29] = 0x00;
        spare_block[30] = 0x00;
        spare_block[31] = 0x00;
        spare_block[32] = 0x00;
        block = spare_block;
    } else if (sector >= total_blocks) {
        cpu->sr |= 1;  /* Error: beyond disk */
        cpu->pc += 2;
        return true;
    } else {
        block = lisa->prof.data + (size_t)sector * 532;
    }

    /* Write 20 tag bytes to A1 */
    for (int i = 0; i < 20; i++)
        cpu->write8((tag_dest + i) & 0xFFFFFF, block[i]);

    /* Write 512 data bytes to A2 */
    for (int i = 0; i < 512; i++)
        cpu->write8((data_dest + i) & 0xFFFFFF, block[20 + i]);

    cpu->sr &= ~1;  /* Clear carry = success */
    cpu->pc += 2;    /* Skip NOP, hit RTS */
    return true;
}

bool lisa_hle_intercept(lisa_t *lisa, m68k_t *cpu) {
    uint32_t pc = cpu->pc & 0x00FFFFFF;

    /* ALWAYS intercept prof_entry at $FE0090 (ROM PROM routine) */
    if (pc == 0xFE0090)
        return hle_prof_entry(lisa, cpu);


    /* HLE: Level 2 interrupt handler (COPS) at $2082D2.
     * The binary's Level 2 handler enters a long initialization phase
     * that never completes (copies data through $FAxxxx-$FBxxxx for
     * thousands of frames). Instead, we handle COPS data directly:
     * read VIA2 port A to consume queued data, process it, clear CA1,
     * and RTE back to the interrupted code.
     *
     * On the real Lisa, the Level 2 handler (from libhw-DRIVERS.TEXT):
     *   1. Save D0, raise IPL to 5
     *   2. Check VIA2 IFR bit 1 (CA1 = COPS data ready)
     *   3. Read VIA2 port A → gets COPS byte
     *   4. Dispatch through state machine (keycodes, mouse, clock)
     *   5. ENABLE, RTE */
    if (pc == 0x2082D2) {
        /* Check if VIA2 CA1 is pending */
        if (lisa->via2.ifr & VIA_IRQ_CA1) {
            /* Read all available COPS data (like the real handler would) */
            while (lisa->cops_rx.count > 0) {
                uint8_t byte = via_read(&lisa->via2, VIA_ORA);
                DBGSTATIC(int, hle_cops, 0);
                if (hle_cops++ < 20)
                    fprintf(stderr, "HLE COPS: read $%02X (remain=%d)\n",
                            byte, lisa->cops_rx.count);
            }
        }

        /* RTE: pop SR and PC from stack (the interrupt acceptance pushed them) */
        cpu->sr = (uint16_t)((cpu->read8(cpu->a[7]) << 8) | cpu->read8(cpu->a[7] + 1));
        cpu->a[7] += 2;
        cpu->pc = (cpu->read8(cpu->a[7]) << 24) | (cpu->read8(cpu->a[7]+1) << 16) |
                  (cpu->read8(cpu->a[7]+2) << 8) | cpu->read8(cpu->a[7]+3);
        cpu->a[7] += 4;
        cpu->cycles = 20;
        return true;
    }

    /* The pre-built Lisa OS 3.1 uses hardcoded initialization at $52051C.
     * It sets up VIA1/VIA2 and enters the COPS loop but leaves interrupts
     * masked at IPL 7. We lower the IPL when the scheduler starts, matching
     * what INTSON(0) would do at the end of BOOT_IO_INIT.
     *
     * We also enable VIA1 CA1 interrupt (ProFile BSY transition) and set
     * the ProFile driver polling pointer at $494. The hardcoded OS init
     * skips the CALLDRIVER(dinit) that would normally do this. */
    {
        DBGSTATIC(bool, intson_done, false);
        if (!intson_done && pc >= 0x520840 && pc <= 0x520844) {
            uint16_t sr = cpu->sr;
            if ((sr & 0x0700) >= 0x0400) {  /* IPL >= 4 */
                cpu->sr = (sr & ~0x0700) | 0x0000;  /* Set IPL to 0 */
                intson_done = true;
                fprintf(stderr, "HLE: INTSON — lowered IPL from %d to 0 at PC=$%06X\n",
                        (sr >> 8) & 7, pc);

                /* The function at $520824 already pushed SR ($2708) onto
                 * the stack at $52083A and raised IPL to 7 via ORI. We
                 * caught it at $520840 after the ORI. Lowering the live
                 * SR is not enough — when the function exits at $52089E
                 * via MOVE (A7)+, SR, it will restore IPL to 7 from the
                 * stacked copy. Patch the stacked SR to have IPL=0 so the
                 * pop restores to 0 instead. */
                uint32_t stacked_sr_addr = cpu->a[7];
                uint16_t stacked_sr = ((uint16_t)cpu->read8(stacked_sr_addr) << 8) |
                                       cpu->read8(stacked_sr_addr + 1);
                uint16_t new_stacked_sr = stacked_sr & ~0x0700;
                cpu->write8(stacked_sr_addr,     (new_stacked_sr >> 8) & 0xFF);
                cpu->write8(stacked_sr_addr + 1,  new_stacked_sr       & 0xFF);
                fprintf(stderr, "HLE: Patched stacked SR at A7=$%06X from $%04X to $%04X\n",
                        stacked_sr_addr, stacked_sr, new_stacked_sr);

                /* Set up VIA1 for ProFile operation.
                 * On the real Lisa, CALLDRIVER(dinit) does this. */
                via_write(&lisa->via1, VIA_DDRB, 0x1C);  /* Bits 2-4 output: DEN, RRW, CMD */
                via_write(&lisa->via1, VIA_DDRA, 0xFF);   /* Port A all outputs (data bus) */

                /* Start VIA1 Timer1 free-run for the 20ms OS tick.
                 * libhw-DRIVERS sets ACR=$48 (T1 continuous), writes T1 latches,
                 * then IER=$C0 to enable T1 interrupt. 5MHz clock × 20ms = 100_000
                 * cycles; with the VIA /2 prescaler that's counter value ~50_000
                 * but the OS caller provides LCounterInit/HCounterInit. We pick
                 * 20000 (~8ms) just to keep the flag pulsing — the OS cares about
                 * T1 interrupts firing, not exact cadence. */
                via_write(&lisa->via1, VIA_ACR,  0x48);
                via_write(&lisa->via1, VIA_T1LL, 0x20);
                via_write(&lisa->via1, VIA_T1LH, 0x4E);
                via_write(&lisa->via1, VIA_T1CL, 0x20);
                via_write(&lisa->via1, VIA_T1CH, 0x4E);  /* starts T1 */
                via_write(&lisa->via1, VIA_IER,  0xC2);  /* T1 (bit 6) + CA1 (bit 1) */
                fprintf(stderr, "HLE: ProFile init — DDRB=$%02X DDRA=$%02X "
                        "ACR=$%02X IER=$%02X\n",
                        lisa->via1.ddrb, lisa->via1.ddra,
                        lisa->via1.acr, lisa->via1.ier);

                /* Copy ProFile driver entry from $49C to $494 so the
                 * polling dispatcher at $208904 can find it. The dispatcher
                 * checks $498 (COPS) then $494 (ProFile). */
                uint32_t prof_drv = lisa_mem_read32(&lisa->mem, 0x49C);
                if (prof_drv > 0 && prof_drv < 0x300000) {
                    lisa_mem_write32(&lisa->mem, 0x494, prof_drv);
                    fprintf(stderr, "HLE: Set ProFile driver $494=$%06X (from $49C)\n", prof_drv);
                }

            }
        }
    }

    if (!lisa->hle.active) return false;


    /* Intercept CALLDRIVER */
    if (pc == lisa->hle.calldriver) return hle_handle_calldriver(lisa, cpu);
    if (lisa->hle.call_hdisk != 0 && pc == lisa->hle.call_hdisk)
        return hle_handle_calldriver(lisa, cpu);

    /* Intercept SYSTEM_ERROR for boot failure suppression */
    if (lisa->hle.system_error != 0 && pc == lisa->hle.system_error)
        return hle_handle_system_error(lisa, cpu);

    return false;
}

/* HLE callback wrapper for m68k_t.hle_check */
static bool hle_cpu_check(void *ctx, void *cpu) {
    return lisa_hle_intercept((lisa_t *)ctx, (m68k_t *)cpu);
}
