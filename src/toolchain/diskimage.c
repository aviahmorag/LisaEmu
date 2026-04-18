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

/* Write tag bytes for a block */
static void write_block_tag(disk_builder_t *db, uint32_t block,
                             uint16_t file_id, uint16_t abs_page,
                             uint16_t rel_page, uint16_t fwd_link,
                             uint16_t bkwd_link) {
    if (block >= db->total_blocks) return;
    uint32_t offset = block * PROFILE_BLOCK_SIZE;
    /* Tag format (simplified):
     * Bytes 0-1: file ID
     * Bytes 2-3: absolute page number
     * Bytes 4-5: relative page within file
     * Bytes 6-7: forward link (next block)
     * Bytes 8-9: backward link (prev block)
     * Bytes 10-19: reserved/checksum */
    db->image[offset + 0] = (file_id >> 8) & 0xFF;
    db->image[offset + 1] = file_id & 0xFF;
    db->image[offset + 2] = (abs_page >> 8) & 0xFF;
    db->image[offset + 3] = abs_page & 0xFF;
    db->image[offset + 4] = (rel_page >> 8) & 0xFF;
    db->image[offset + 5] = rel_page & 0xFF;
    db->image[offset + 6] = (fwd_link >> 8) & 0xFF;
    db->image[offset + 7] = fwd_link & 0xFF;
    db->image[offset + 8] = (bkwd_link >> 8) & 0xFF;
    db->image[offset + 9] = bkwd_link & 0xFF;
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
        /* Tag boot blocks with file_id = 0xAAAA (boot marker) */
        write_block_tag(db, b, 0xAAAA, (uint16_t)b, (uint16_t)b,
                        (b + 1 < blocks) ? (uint16_t)(b + 1) : 0,
                        (b > 0) ? (uint16_t)(b - 1) : 0);
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
        /* Tag with file ID (index + 1) */
        uint16_t file_id = (uint16_t)(db->num_files);
        write_block_tag(db, block_num, file_id, (uint16_t)block_num, (uint16_t)b,
                        (b + 1 < blocks) ? (uint16_t)(block_num + 1) : 0,
                        (b > 0) ? (uint16_t)(block_num - 1) : 0);
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

    /* slist — starts after bitmap */
    uint32_t slist_start = 1 + bitmap_pages_val;
    W32(mddf, 148, slist_start);                /* slist_addr */
    W16(mddf, 152, 36);                         /* slist_packing (36 entries per block) */
    W16(mddf, 154, 1);                          /* slist_block_count */

    /* File management fields */
    W16(mddf, 156, 4);                          /* first_file */
    W16(mddf, 158, (uint16_t)(4 + db->num_files)); /* empty_file */
    W16(mddf, 160, 256);                        /* maxfiles */
    W16(mddf, 162, 1);                          /* hintsize (pages) */
    W16(mddf, 164, 0);                          /* leader_offset */
    W16(mddf, 166, 1);                          /* leader_pages */
    W16(mddf, 168, 0);                          /* flabel_offset */
    W16(mddf, 170, 0);                          /* unusedi1 */
    W16(mddf, 172, 0);                          /* map_offset */
    W16(mddf, 174, 84);                         /* map_size */
    W16(mddf, 176, (uint16_t)db->num_files);   /* filecount */

    /* Free space tracking */
    uint32_t used = db->next_free_block - BOOT_TRACK_BLOCKS;
    uint32_t free_pages = (total_fs_pages > used) ? total_fs_pages - used : 0;
    W32(mddf, 178, used);                       /* freestart */
    W32(mddf, 186, free_pages);                 /* freecount */

    /* Root catalog */
    W16(mddf, 190, 3);                          /* rootsnum (s-file 3) */
    W16(mddf, 192, 64);                         /* rootmaxentries */

    /* Late fields (after booleans at 274-275) */
    W16(mddf, 276, 0);                          /* smallmap_offset */

    #undef W16
    #undef W32

    /* Catalog start block (filesystem page after slist) */
    uint16_t cat_block = BOOT_TRACK_BLOCKS + 1;

    write_block_data(db, BOOT_TRACK_BLOCKS, mddf, sizeof(mddf));
    write_block_tag(db, BOOT_TRACK_BLOCKS, 0x0001, BOOT_TRACK_BLOCKS, 0, 0, 0);

    /* Write file catalog at block BOOT_TRACK_BLOCKS+1 */
    /* Catalog entry format (32 bytes each):
     * Offset 0:     Name length
     * Offset 1-31:  Name
     * Offset 32-33: File type
     * Offset 34-37: File size
     * Offset 38-39: Start block
     * Offset 40-41: Block count
     */
    uint8_t catalog[PROFILE_DATA_SIZE];
    memset(catalog, 0, sizeof(catalog));
    int cat_offset = 0;
    for (int i = 0; i < db->num_files && cat_offset + 42 < PROFILE_DATA_SIZE; i++) {
        disk_file_t *f = &db->files[i];
        int nlen = (int)strlen(f->name);
        if (nlen > 31) nlen = 31;
        catalog[cat_offset] = (uint8_t)nlen;
        memcpy(catalog + cat_offset + 1, f->name, nlen);
        catalog[cat_offset + 32] = 0;
        catalog[cat_offset + 33] = f->file_type;
        catalog[cat_offset + 34] = (f->size >> 24) & 0xFF;
        catalog[cat_offset + 35] = (f->size >> 16) & 0xFF;
        catalog[cat_offset + 36] = (f->size >> 8)  & 0xFF;
        catalog[cat_offset + 37] = f->size & 0xFF;
        catalog[cat_offset + 38] = (f->start_block >> 8) & 0xFF;
        catalog[cat_offset + 39] = f->start_block & 0xFF;
        catalog[cat_offset + 40] = (f->num_blocks >> 8) & 0xFF;
        catalog[cat_offset + 41] = f->num_blocks & 0xFF;
        cat_offset += 42;
    }

    write_block_data(db, cat_block, catalog, sizeof(catalog));
    write_block_tag(db, cat_block, 0x0002, cat_block, 0, 0, 0);

    printf("Disk image: %s, %d files, %u blocks used of %u\n",
           db->volume_name, db->num_files, db->next_free_block, db->total_blocks);

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
