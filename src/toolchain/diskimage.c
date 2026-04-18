/*
 * LisaEm Toolchain — Disk Image Builder
 *
 * Creates ProFile-format disk images with the Lisa filesystem.
 *
 * ProFile block format:
 *   Bytes 0-19:  Tag (file ID, block type, etc.)
 *   Bytes 20-531: Data (512 bytes)
 *
 * Lisa filesystem layout:
 *   Block 0-23:   Boot track (device-specific loader)
 *   Block 24:     MDDF (Master Directory Data File)
 *   Block 25+:    File data and directory
 *
 * This is a simplified implementation sufficient to create bootable
 * images that the Lisa OS can mount and boot from.
 */

#include "diskimage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ========================================================================
 * Helpers
 * ======================================================================== */

static void disk_error(disk_builder_t *db, const char *fmt, ...) {
    if (db->num_errors >= 50) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(db->errors[db->num_errors], 256, fmt, args);
    va_end(args);
    fprintf(stderr, "diskimage: %s\n", db->errors[db->num_errors]);
    db->num_errors++;
}

/* Write data into a specific block's data area */
static void write_block_data(disk_builder_t *db, uint32_t block,
                              const uint8_t *data, uint32_t size) {
    if (block >= db->total_blocks) return;
    uint32_t offset = block * PROFILE_BLOCK_SIZE + PROFILE_TAG_SIZE;
    uint32_t to_write = (size > PROFILE_DATA_SIZE) ? PROFILE_DATA_SIZE : size;
    memcpy(db->image + offset, data, to_write);
}

/* Write the 20-byte on-disk tag for a block.
 *
 * Layout matches the FIRST 20 bytes of Apple's 24-byte pagelabel record
 * (source-DRIVERDEFS.TEXT.unix.txt:207). The 4-byte bkwdlink tail is
 * omitted on disk — real ProFile driver recovers it from RAM (see
 * source-LDPROF.TEXT:202 reading only disk_header=20 bytes, while
 * SOURCE-PROFILEASM.TEXT:637 checksums 24 bytes once in RAM).
 *
 *   @0-1  version (2)          0 = no checksum-present
 *   @2-3  datastat/filler/volume (2 packed)  0 = dataok + defaults
 *   @4-5  fileid (2)           block-role id (0xAAAA boot, 0x0001 MDDF,
 *                               0x0002 bitmap, 0x0003 slist,
 *                               0x0004 rootcat, sfile# for file pages)
 *   @6-7  dataused (2)         valid bytes in data portion (0 = unset)
 *   @8-11 abspage (int4)       absolute FS page of this block
 *   @12-15 relpage (int4)      relative page within its file
 *   @16-19 fwdlink (int4)      next chained block (0 = end of chain)
 *
 * This matches what the HLE in src/lisa.c:322 already reads
 * (ldr_get32(tag_buf + 16) for fwdlink) and what the real LOADER's
 * OPEN_FILE chain walking expects. */
static void write_block_tag(disk_builder_t *db, uint32_t block,
                             uint16_t file_id,
                             uint32_t abs_page, uint32_t rel_page,
                             uint32_t fwd_link) {
    if (block >= db->total_blocks) return;
    uint32_t offset = block * PROFILE_BLOCK_SIZE;
    uint8_t *t = db->image + offset;

    /* @0-1 version = 0 */
    t[0] = 0; t[1] = 0;
    /* @2-3 datastat/filler/volume = 0 */
    t[2] = 0; t[3] = 0;
    /* @4-5 fileid (big-endian int16) */
    t[4] = (uint8_t)((file_id >> 8) & 0xFF);
    t[5] = (uint8_t)(file_id & 0xFF);
    /* @6-7 dataused = 0 */
    t[6] = 0; t[7] = 0;
    /* @8-11 abspage (big-endian int4) */
    t[8]  = (uint8_t)((abs_page >> 24) & 0xFF);
    t[9]  = (uint8_t)((abs_page >> 16) & 0xFF);
    t[10] = (uint8_t)((abs_page >> 8)  & 0xFF);
    t[11] = (uint8_t)(abs_page & 0xFF);
    /* @12-15 relpage (big-endian int4) */
    t[12] = (uint8_t)((rel_page >> 24) & 0xFF);
    t[13] = (uint8_t)((rel_page >> 16) & 0xFF);
    t[14] = (uint8_t)((rel_page >> 8)  & 0xFF);
    t[15] = (uint8_t)(rel_page & 0xFF);
    /* @16-19 fwdlink (big-endian int4) */
    t[16] = (uint8_t)((fwd_link >> 24) & 0xFF);
    t[17] = (uint8_t)((fwd_link >> 16) & 0xFF);
    t[18] = (uint8_t)((fwd_link >> 8)  & 0xFF);
    t[19] = (uint8_t)(fwd_link & 0xFF);
}

/* Compute blocks needed for a given byte size */
static uint32_t blocks_needed(uint32_t bytes) {
    return (bytes + PROFILE_DATA_SIZE - 1) / PROFILE_DATA_SIZE;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void disk_init(disk_builder_t *db, uint32_t num_blocks) {
    memset(db, 0, sizeof(disk_builder_t));
    db->total_blocks = num_blocks;
    db->block_size = PROFILE_BLOCK_SIZE;
    db->image_size = num_blocks * PROFILE_BLOCK_SIZE;
    db->image = calloc(1, db->image_size);
    db->next_free_block = BOOT_TRACK_BLOCKS + 2; /* After boot track + MDDF + catalog */
    strncpy(db->volume_name, "LisaOS", sizeof(db->volume_name) - 1);
}

void disk_free(disk_builder_t *db) {
    for (int i = 0; i < db->num_files; i++) {
        free(db->files[i].data);
    }
    free(db->image);
    memset(db, 0, sizeof(disk_builder_t));
}

void disk_set_volume_name(disk_builder_t *db, const char *name) {
    strncpy(db->volume_name, name, sizeof(db->volume_name) - 1);
}

bool disk_write_boot_track(disk_builder_t *db, const uint8_t *boot_code, uint32_t size) {
    if (!boot_code || size == 0) return true;

    /* Truncate to boot track capacity if needed */
    uint32_t max_boot = BOOT_TRACK_BLOCKS * PROFILE_DATA_SIZE;
    if (size > max_boot) {
        printf("diskimage: boot code truncated from %u to %u bytes (boot track limit)\n",
               size, max_boot);
        size = max_boot;
    }
    uint32_t blocks = blocks_needed(size);

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t offset = b * PROFILE_DATA_SIZE;
        uint32_t remaining = size - offset;
        uint32_t chunk = (remaining > PROFILE_DATA_SIZE) ? PROFILE_DATA_SIZE : remaining;
        write_block_data(db, b, boot_code + offset, chunk);
        /* Boot track: fileid 0xAAAA marks block as bootable code for
         * the boot ROM / LDPROF. abspage = relpage = b since the boot
         * track is its own "file". fwdlink chains to next boot block
         * or 0 at end. */
        write_block_tag(db, b, 0xAAAA, b, b,
                        (b + 1 < blocks) ? (b + 1) : 0);
    }

    return true;
}

bool disk_add_file(disk_builder_t *db, const char *name, uint8_t file_type,
                   const uint8_t *data, uint32_t size) {
    if (db->num_files >= MAX_FILES) {
        disk_error(db, "too many files");
        return false;
    }

    uint32_t blocks = blocks_needed(size);
    if (db->next_free_block + blocks > db->total_blocks) {
        disk_error(db, "disk full, cannot add '%s' (%u bytes)", name, size);
        return false;
    }

    disk_file_t *f = &db->files[db->num_files++];
    strncpy(f->name, name, sizeof(f->name) - 1);
    f->file_type = file_type;
    f->size = size;
    f->data = malloc(size);
    if (f->data && size > 0) memcpy(f->data, data, size);
    f->start_block = db->next_free_block;
    f->num_blocks = blocks;

    /* Write file data to blocks */
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t offset = b * PROFILE_DATA_SIZE;
        uint32_t remaining = size - offset;
        uint32_t chunk = (remaining > PROFILE_DATA_SIZE) ? PROFILE_DATA_SIZE : remaining;
        uint32_t block_num = f->start_block + b;
        write_block_data(db, block_num, data + offset, chunk);
        /* File data: fileid = sfile number (FIRST_FILE_SFILE + index).
         * abspage = FS page of this block; relpage = b (0-based within
         * file); fwdlink = next block's FS page or 0 at last block. */
        uint32_t abs_fs_page = block_num - BOOT_TRACK_BLOCKS;
        uint32_t fwd_fs_page = (b + 1 < blocks)
            ? (block_num + 1 - BOOT_TRACK_BLOCKS) : 0;
        uint16_t sfile_id = (uint16_t)(4 + (db->num_files - 1));
        write_block_tag(db, block_num, sfile_id, abs_fs_page, b, fwd_fs_page);
    }

    db->next_free_block += blocks;
    return true;
}

bool disk_add_file_from_path(disk_builder_t *db, const char *name, uint8_t file_type,
                              const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        disk_error(db, "cannot open '%s'", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    bool ok = disk_add_file(db, name, file_type, data, (uint32_t)size);
    free(data);
    return ok;
}

bool disk_finalize(disk_builder_t *db) {
    /* Write MDDF (Master Directory Data File) at block BOOT_TRACK_BLOCKS (= disk block 24).
     * This is filesystem page 0 (MDDF_HOME = 0).
     *
     * The MDDF layout matches the Lisa OS MDDFdb record (SOURCE-VMSTUFF.TEXT).
     * IMPORTANT: Lisa Pascal string[32] = 34 bytes (1 len + 32 chars + 1 pad
     * for word alignment), verified against real "AOS 3.0" disk image.
     *
     *   Offset  Field                  Type         Size
     *   0       fsversion              integer      2
     *   2       volid (UID: a,b)       2x longint   8
     *   10      volnum                 integer      2
     *   12      volname                string[32]   34 (1 len + 32 chars + 1 pad)
     *   46      password               string[32]   34
     *   80      init_machine_id        longint      4
     *   84      master_machine_id      longint      4
     *   88      DT_created             longint      4
     *   92      DT_copy_created        longint      4
     *   96      DT_copied              longint      4
     *   100     DT_scavenged           longint      4
     *   104     copy_thread            longint      4
     *   108     geography.firstblock   longint      4
     *   112     geography.lastblock    longint      4
     *   116     geography.lastfspage   longint      4
     *   120     blockcount             longint      4
     *   124     blocksize              integer      2  (536 = 512 data + 24 label)
     *   126     datasize               integer      2  (512)
     *   128     cluster_size           integer      2
     *   130     MDDFaddr               longint      4  (must be 0)
     *   134     MDDFsize               integer      2
     *   136     bitmap_addr            longint      4
     *   140     bitmap_size            longint      4
     *   144     bitmap_bytes           integer      2
     *   146     bitmap_pages           integer      2
     *   148     slist_addr             longint      4
     *   152     slist_packing          integer      2
     *   154     slist_block_count      integer      2
     *   156     first_file             integer      2
     *   158     empty_file             integer      2
     *   160     maxfiles               integer      2
     *   162     hintsize               integer      2
     *   164     leader_offset          integer      2
     *   166     leader_pages           integer      2
     *   168     flabel_offset          integer      2
     *   170     unusedi1               integer      2
     *   172     map_offset             integer      2
     *   174     map_size               integer      2
     *   176     filecount              integer      2
     *   178     freestart              longint      4
     *   182     unusedl1               longint      4
     *   186     freecount              longint      4
     *   190     rootsnum               integer      2
     *   192     rootmaxentries         integer      2
     *   ...     (mountinfo, pmem, booleans, late fields)
     *   276     smallmap_offset        integer      2
     *   302     root_page              longint      4
     *   306     tree_depth             integer      2
     *
     * The OS validates (source-sfileio2.text, real_mount):
     *   - fsversion must be 14, 15, or 17 (SPRING_VERSION=17 = CUR_VERSION)
     *   - MDDFaddr must be 0
     *   - length(volname) must not exceed 32
     */

    /* sizeof(MDDFdb) including pmem(66), booleans, and trailing fields */
    #define MDDF_RECORD_SIZE 314

    uint8_t mddf[PROFILE_DATA_SIZE];
    memset(mddf, 0, sizeof(mddf));

    /* Helper macros for big-endian writes */
    #define W16(buf, off, val) do { (buf)[(off)] = ((val) >> 8) & 0xFF; \
                                    (buf)[(off)+1] = (val) & 0xFF; } while(0)
    #define W32(buf, off, val) do { (buf)[(off)]   = ((val) >> 24) & 0xFF; \
                                    (buf)[(off)+1] = ((val) >> 16) & 0xFF; \
                                    (buf)[(off)+2] = ((val) >> 8)  & 0xFF; \
                                    (buf)[(off)+3] = (val) & 0xFF; } while(0)

    /* fsversion = 15 — pre-B-tree "conventional lookup" path in
     * source-ldlfs.text (line 530: `if fs_version < btree_fsversion`
     * where btree_fsversion = 16). Keeps the real LOADER on the
     * LOOKUP_ENAME + LDHASH flat-hash catalog path that our Step 3d3
     * centry layout targets. Step 3d2+ will build the matching slist
     * and catalog. Apple's fsversion 17 needs a B-tree catalog (future
     * upgrade for parity with shipped AOS 3.x disks). */
    W16(mddf, 0, 15);

    /* volid — UID with two longints, just use a simple ID */
    W32(mddf, 2, 0x00000001);  /* volid.a */
    W32(mddf, 6, 0x00000001);  /* volid.b */

    /* volnum */
    W16(mddf, 10, 1);

    /* volname as Pascal string[32]: 34 bytes (1 len + 32 chars + 1 pad for
     * word alignment). This matches the Lisa Pascal record layout. */
    int namelen = (int)strlen(db->volume_name);
    if (namelen > 32) namelen = 32;
    mddf[12] = (uint8_t)namelen;
    memcpy(mddf + 13, db->volume_name, namelen);
    /* string[32] occupies bytes 12..45 (34 bytes total) */

    /* password — empty string at offset 46 (34 bytes: 46..79) */
    mddf[46] = 0;

    /* init_machine_id at offset 80 */
    W32(mddf, 80, 0x00000001);  /* init_machine_id */
    W32(mddf, 84, 0x00000001);  /* master_machine_id */

    /* Filesystem geometry — corrected offsets (34-byte strings) */
    uint32_t total_fs_pages = db->total_blocks - BOOT_TRACK_BLOCKS;
    uint32_t last_fs_page = (total_fs_pages > 0) ? total_fs_pages - 1 : 0;
    W32(mddf, 108, BOOT_TRACK_BLOCKS);          /* geography.firstblock */
    W32(mddf, 112, (uint32_t)(db->total_blocks - 1)); /* geography.lastblock */
    W32(mddf, 116, last_fs_page);               /* geography.lastfspage */

    /* blockcount, blocksize, datasize, cluster_size */
    W32(mddf, 120, db->total_blocks);           /* blockcount */
    W16(mddf, 124, 536);                        /* blocksize (536 = 512 data + 24 label) */
    W16(mddf, 126, PROFILE_DATA_SIZE);          /* datasize (512) */
    W16(mddf, 128, 1);                          /* cluster_size */

    /* MDDFaddr = 0 (filesystem page 0, i.e. disk block BOOT_TRACK_BLOCKS) */
    W32(mddf, 130, 0);

    /* MDDFsize = sizeof(MDDFdb) */
    W16(mddf, 134, MDDF_RECORD_SIZE);

    /* bitmap — starts at filesystem page 1 */
    W32(mddf, 136, 1);                          /* bitmap_addr */
    W32(mddf, 140, total_fs_pages);             /* bitmap_size (bits) */
    uint32_t bitmap_bytes_val = (total_fs_pages + 7) / 8;
    uint32_t bitmap_pages_val = (bitmap_bytes_val + PROFILE_DATA_SIZE - 1) / PROFILE_DATA_SIZE;
    W16(mddf, 144, (uint16_t)bitmap_bytes_val); /* bitmap_bytes */
    W16(mddf, 146, (uint16_t)bitmap_pages_val); /* bitmap_pages */

    /* slist — as many blocks as needed, allocated from next_free_block.
     * Must come after files so allocating here doesn't collide with data.
     * slist_packing = 36 = floor(512 / sizeof(s_entry)) where
     * s_entry = hintaddr(4) + fileaddr(4) + filesize(4) + version(2)
     * = 14 bytes (source-sfileio.text:45-50). */
    #define SENTRY_SIZE 14
    #define SLIST_PACKING 36
    #define FIRST_FILE_SFILE 4   /* sfiles 0..3 reserved (0=unused, 1=MDDF, 2=bitmap, 3=rootcat) */
    uint32_t total_sfiles = (uint32_t)(FIRST_FILE_SFILE + db->num_files);
    uint32_t slist_block_count =
        (total_sfiles + SLIST_PACKING - 1) / SLIST_PACKING;
    if (slist_block_count == 0) slist_block_count = 1;
    uint32_t slist_block = db->next_free_block;
    db->next_free_block += slist_block_count;
    uint32_t slist_fs_page = slist_block - BOOT_TRACK_BLOCKS;
    W32(mddf, 148, slist_fs_page);              /* slist_addr */
    W16(mddf, 152, SLIST_PACKING);              /* slist_packing */
    W16(mddf, 154, (uint16_t)slist_block_count);/* slist_block_count */

    /* File management fields */
    W16(mddf, 156, FIRST_FILE_SFILE);                                /* first_file */
    W16(mddf, 158, (uint16_t)total_sfiles);                          /* empty_file */
    W16(mddf, 160, (uint16_t)(slist_block_count * SLIST_PACKING));   /* maxfiles */
    W16(mddf, 162, 1);                          /* hintsize (pages) */
    W16(mddf, 164, 0);                          /* leader_offset */
    W16(mddf, 166, 1);                          /* leader_pages */
    W16(mddf, 168, 0);                          /* flabel_offset */
    W16(mddf, 170, 0);                          /* unusedi1 */
    W16(mddf, 172, 0);                          /* map_offset (filemap is AT hintaddr) */
    W16(mddf, 174, 84);                         /* map_size */
    W16(mddf, 176, (uint16_t)db->num_files);   /* filecount */

    /* Free space tracking */
    uint32_t used = db->next_free_block - BOOT_TRACK_BLOCKS;
    uint32_t free_pages = (total_fs_pages > used) ? total_fs_pages - used : 0;
    W32(mddf, 178, used);                       /* freestart */
    W32(mddf, 186, free_pages);                 /* freecount */

    /* Root catalog: rootsnum=3 (by convention), 64 centry slots so the
     * real LOADER's LDHASH mod catentries lands in a known range. Each
     * centry is 54 bytes (see write_rootcat below), so total =
     * 64 * 54 = 3456 bytes, spanning 7 data pages. */
    #define CENTRY_SIZE 54
    #define CATENTRIES 64
    W16(mddf, 190, 3);                          /* rootsnum (s-file 3) */
    W16(mddf, 192, CATENTRIES);                 /* rootmaxentries */

    /* Late fields (after booleans at 274-275) */
    W16(mddf, 276, 0);                          /* smallmap_offset */

    #undef W16
    #undef W32

    /* --- Allocate trailing metadata blocks (rootcat filemap + rootcat
     *      data + per-file filemaps). Happens BEFORE slist is written
     *      so slist hintaddr / rootcat fields reference real blocks. */
    uint32_t rootcat_bytes = CATENTRIES * CENTRY_SIZE;
    uint32_t rootcat_data_pages =
        (rootcat_bytes + PROFILE_DATA_SIZE - 1) / PROFILE_DATA_SIZE;

    uint32_t rootcat_filemap_block = db->next_free_block;
    db->next_free_block += 1;
    uint32_t rootcat_data_block = db->next_free_block;
    db->next_free_block += rootcat_data_pages;

    uint32_t file_filemap_block[MAX_FILES];
    for (int i = 0; i < db->num_files; i++) {
        file_filemap_block[i] = db->next_free_block++;
    }

    /* MDDF freestart / freecount recompute after tail allocations. */
    used = db->next_free_block - BOOT_TRACK_BLOCKS;
    free_pages = (total_fs_pages > used) ? total_fs_pages - used : 0;
    mddf[178] = (used >> 24) & 0xFF; mddf[179] = (used >> 16) & 0xFF;
    mddf[180] = (used >> 8)  & 0xFF; mddf[181] =  used        & 0xFF;
    mddf[186] = (free_pages >> 24) & 0xFF; mddf[187] = (free_pages >> 16) & 0xFF;
    mddf[188] = (free_pages >> 8)  & 0xFF; mddf[189] =  free_pages        & 0xFF;

    write_block_data(db, BOOT_TRACK_BLOCKS, mddf, sizeof(mddf));
    /* MDDF lives at FS page 0; fileid 0x0001 identifies MDDF. */
    write_block_tag(db, BOOT_TRACK_BLOCKS, 0x0001, 0, 0, 0);

    /* --- slist: slist_block_count pages of 36 packed 14-byte s_entry
     * records each. s_entry layout (source-sfileio.text:45-50, big-
     * endian on disk):
     *   +0  hintaddr : longint   (FS page of this file's filemap)
     *   +4  fileaddr : longint   (FS page of first data chunk, informational)
     *   +8  filesize : longint   (bytes)
     *  +12  version  : integer   (1 for user files)
     *
     * sfiles 0..2 reserved (0=unused, 1=MDDF placeholder, 2=bitmap).
     * sfile 3 = root catalog — hintaddr points at rootcat filemap.
     * sfiles 4..(4+num_files-1) = user files in db->files[] order. */
    uint8_t slist_buf[PROFILE_DATA_SIZE];

    #define SLIST_W16(off, v) do { slist_buf[(off)]   = ((v) >> 8) & 0xFF; \
                                    slist_buf[(off)+1] = (v) & 0xFF; } while (0)
    #define SLIST_W32(off, v) do { slist_buf[(off)]   = ((v) >> 24) & 0xFF; \
                                    slist_buf[(off)+1] = ((v) >> 16) & 0xFF; \
                                    slist_buf[(off)+2] = ((v) >> 8)  & 0xFF; \
                                    slist_buf[(off)+3] = (v) & 0xFF; } while (0)

    for (uint32_t sb = 0; sb < slist_block_count; sb++) {
        memset(slist_buf, 0, sizeof(slist_buf));
        uint32_t first_sfile_on_block = sb * SLIST_PACKING;
        for (uint32_t slot = 0; slot < SLIST_PACKING; slot++) {
            uint32_t sfile = first_sfile_on_block + slot;
            if (sfile >= total_sfiles) break;
            int off = (int)(slot * SENTRY_SIZE);
            if (sfile == 3) {
                /* rootcat s_entry */
                uint32_t hint_fs  = rootcat_filemap_block - BOOT_TRACK_BLOCKS;
                uint32_t file_fs  = rootcat_data_block    - BOOT_TRACK_BLOCKS;
                SLIST_W32(off + 0, hint_fs);             /* hintaddr */
                SLIST_W32(off + 4, file_fs);             /* fileaddr */
                SLIST_W32(off + 8, rootcat_bytes);       /* filesize */
                SLIST_W16(off + 12, 1);                  /* version */
                continue;
            }
            if (sfile < FIRST_FILE_SFILE) continue; /* 0,1,2 stay zero */
            int fi = (int)(sfile - FIRST_FILE_SFILE);
            disk_file_t *f = &db->files[fi];
            uint32_t file_fs_page = f->start_block     - BOOT_TRACK_BLOCKS;
            uint32_t hint_fs_page = file_filemap_block[fi] - BOOT_TRACK_BLOCKS;
            SLIST_W32(off + 0, hint_fs_page);        /* hintaddr */
            SLIST_W32(off + 4, file_fs_page);        /* fileaddr */
            SLIST_W32(off + 8, f->size);             /* filesize */
            SLIST_W16(off + 12, 1);                  /* version */
        }
        uint32_t disk_block = slist_block + sb;
        uint32_t fs_page = disk_block - BOOT_TRACK_BLOCKS;
        uint32_t fwd_page = (sb + 1 < slist_block_count)
            ? (fs_page + 1) : 0;
        write_block_data(db, disk_block, slist_buf, sizeof(slist_buf));
        write_block_tag(db, disk_block, 0x0003, fs_page, sb, fwd_page);
    }

    #undef SLIST_W16
    #undef SLIST_W32

    /* --- Filemap writer. filemap layout (source-sfileio.text:66-71):
     *   @0  size: longint          (number of blocks in this s-file)
     *   @4  max_entries: integer   (array upper bound, 0 for single extent)
     *   @6  ecount: integer        (used entries count)
     *   @8+ mapentry[] = (address: longint, cpages: integer) = 6 bytes each
     *
     * Our files are always single-extent (contiguous allocation by
     * disk_add_file), so max_entries=0 + one mapentry is sufficient.
     * OPEN_FILE exits when abs_page > last_page before attempting to
     * read entry 1. */
    #define FMAP_W16(buf, off, v) do { (buf)[(off)]   = ((v) >> 8) & 0xFF; \
                                        (buf)[(off)+1] = (v) & 0xFF; } while (0)
    #define FMAP_W32(buf, off, v) do { (buf)[(off)]   = ((v) >> 24) & 0xFF; \
                                        (buf)[(off)+1] = ((v) >> 16) & 0xFF; \
                                        (buf)[(off)+2] = ((v) >> 8)  & 0xFF; \
                                        (buf)[(off)+3] = (v) & 0xFF; } while (0)

    /* Rootcat filemap: points at the 7 contiguous rootcat data pages. */
    {
        uint8_t fm[PROFILE_DATA_SIZE];
        memset(fm, 0, sizeof(fm));
        uint32_t rc_fs_page = rootcat_data_block - BOOT_TRACK_BLOCKS;
        FMAP_W32(fm, 0, rootcat_data_pages);      /* size (pages) */
        FMAP_W16(fm, 4, 0);                       /* max_entries */
        FMAP_W16(fm, 6, 1);                       /* ecount */
        FMAP_W32(fm, 8, rc_fs_page);              /* map[0].address */
        FMAP_W16(fm, 12, (uint16_t)rootcat_data_pages); /* map[0].cpages */
        write_block_data(db, rootcat_filemap_block, fm, sizeof(fm));
        uint32_t fs_page = rootcat_filemap_block - BOOT_TRACK_BLOCKS;
        /* rootcat filemap is a single page (contiguous extent), no
         * fwdlink needed. fileid 0x0004 = catalog file. */
        write_block_tag(db, rootcat_filemap_block, 0x0004, fs_page, 0, 0);
    }

    /* Per-file filemaps */
    for (int i = 0; i < db->num_files; i++) {
        disk_file_t *f = &db->files[i];
        uint8_t fm[PROFILE_DATA_SIZE];
        memset(fm, 0, sizeof(fm));
        uint32_t file_fs_page = f->start_block - BOOT_TRACK_BLOCKS;
        FMAP_W32(fm, 0, f->num_blocks);
        FMAP_W16(fm, 4, 0);
        FMAP_W16(fm, 6, 1);
        FMAP_W32(fm, 8, file_fs_page);
        FMAP_W16(fm, 12, (uint16_t)f->num_blocks);
        write_block_data(db, file_filemap_block[i], fm, sizeof(fm));
        uint32_t fs_page = file_filemap_block[i] - BOOT_TRACK_BLOCKS;
        uint16_t sfile_tag = (uint16_t)(FIRST_FILE_SFILE + i);
        write_block_tag(db, file_filemap_block[i], sfile_tag,
                        fs_page, 0, 0);
    }

    #undef FMAP_W16
    #undef FMAP_W32

    /* --- Root catalog data: CATENTRIES slots of CENTRY_SIZE each,
     * laid out linearly across rootcat_data_pages. Each slot is a
     * centry record (source-fsprim.text:126):
     *   @0  name: e_name       (34 bytes = 1 len + 32 chars + 1 pad)
     *   @34 cetype: entrytype  (2 bytes big-endian int16, value from
     *                           emptyentry=0..threadentry=8 per
     *                           source-sfileio.text:57. fileentry=3.)
     *   @36 sfile: integer     (2 bytes)
     *   @38 attributes..writeoffset (variant arm, zeros for our use)
     *
     * Slot placement via LDHASH (source-ldlfs.text:396) — Fibonacci-
     * style hash of the uppercase name modulo CATENTRIES. Linear-
     * probe to next empty slot on collision; LOOKUP_ENAME scans the
     * same way. Our HLE in src/lisa.c:461 also walks this format. */
    uint32_t rc_len = rootcat_data_pages * PROFILE_DATA_SIZE;
    uint8_t *rc_buf = (uint8_t *)calloc(1, rc_len);
    if (!rc_buf) {
        disk_error(db, "out of memory allocating rootcat buffer");
        return false;
    }
    /* Mark all slots as emptyentry (0) by calloc — already there. */

    for (int i = 0; i < db->num_files; i++) {
        disk_file_t *f = &db->files[i];
        /* Uppercase the name for hashing + storage (LOOKUP_ENAME does
         * SHIFTNAME on both sides, so either case works; uppercase is
         * Apple convention). */
        char upper[33];
        int nlen = (int)strlen(f->name);
        if (nlen > 32) nlen = 32;
        for (int c = 0; c < nlen; c++) {
            unsigned char ch = (unsigned char)f->name[c];
            upper[c] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : (char)ch;
        }
        upper[nlen] = 0;

        /* LDHASH: Fibonacci-like hash, mod CATENTRIES */
        int32_t temp;
        if (nlen <= 0) {
            temp = 0;
        } else {
            temp = (int32_t)(uint8_t)upper[0] *
                   ((int32_t)(uint8_t)upper[nlen - 1] + 1);
            int m = (nlen > 2) ? (nlen - 2) : 0;
            while (m > 0) {
                temp += (int32_t)(uint8_t)upper[m] *
                        ((int32_t)(uint8_t)upper[m + 1] + 1);
                m--;
            }
            if (temp < 0) temp = -temp;
        }
        int slot = (int)(temp % CATENTRIES);

        /* Linear probe until empty slot */
        int probes = 0;
        while (probes < CATENTRIES) {
            uint8_t *e = rc_buf + slot * CENTRY_SIZE;
            uint16_t cetype = (uint16_t)((e[34] << 8) | e[35]);
            if (cetype == 0 /* emptyentry */) break;
            slot = (slot + 1) % CATENTRIES;
            probes++;
        }
        if (probes >= CATENTRIES) {
            disk_error(db, "rootcat full — can't place '%s'", f->name);
            break;
        }

        uint8_t *e = rc_buf + slot * CENTRY_SIZE;
        e[0] = (uint8_t)nlen;
        memcpy(e + 1, upper, nlen);
        /* e[1+nlen .. 33] already zero */
        /* cetype = fileentry (3), big-endian int16 */
        e[34] = 0; e[35] = 3;
        /* sfile = FIRST_FILE_SFILE + i */
        uint16_t sfile = (uint16_t)(FIRST_FILE_SFILE + i);
        e[36] = (uint8_t)((sfile >> 8) & 0xFF);
        e[37] = (uint8_t)(sfile & 0xFF);
        /* attributes, readpage/offset, writepage/offset stay zero */
    }

    /* Write rootcat data pages with fileid 0x0004 (catalog) and
     * fwdlink chaining through the contiguous extent so the real
     * LOADER's OPEN_FILE filemap walk stays consistent. */
    for (uint32_t p = 0; p < rootcat_data_pages; p++) {
        uint32_t block = rootcat_data_block + p;
        uint32_t fs_page = block - BOOT_TRACK_BLOCKS;
        const uint8_t *src = rc_buf + p * PROFILE_DATA_SIZE;
        uint32_t remaining = rc_len - p * PROFILE_DATA_SIZE;
        uint32_t chunk = remaining > PROFILE_DATA_SIZE
                         ? (uint32_t)PROFILE_DATA_SIZE : remaining;
        uint32_t fwd = (p + 1 < rootcat_data_pages) ? (fs_page + 1) : 0;
        write_block_data(db, block, src, chunk);
        write_block_tag(db, block, 0x0004, fs_page, p, fwd);
    }
    free(rc_buf);

    printf("Disk image: %s, %d files, %u blocks used of %u "
           "(slist @ %u/pg %u, rootcat fm @ %u pg %u, rootcat data @ %u..%u pg %u)\n",
           db->volume_name, db->num_files, db->next_free_block, db->total_blocks,
           slist_block, slist_fs_page,
           rootcat_filemap_block, rootcat_filemap_block - BOOT_TRACK_BLOCKS,
           rootcat_data_block, rootcat_data_block + rootcat_data_pages - 1,
           rootcat_data_block - BOOT_TRACK_BLOCKS);

    return true;
}

bool disk_write_image(disk_builder_t *db, const char *filename) {
    if (!db->image) {
        disk_error(db, "no image data");
        return false;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        disk_error(db, "cannot create '%s'", filename);
        return false;
    }

    fwrite(db->image, 1, db->image_size, f);
    fclose(f);

    printf("Wrote: %s (%u bytes, %u blocks)\n",
           filename, db->image_size, db->total_blocks);
    return true;
}

int disk_get_error_count(disk_builder_t *db) {
    return db->num_errors;
}
