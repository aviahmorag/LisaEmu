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
    /* Write MDDF (Master Directory Data File) at block BOOT_TRACK_BLOCKS */
    uint8_t mddf[PROFILE_DATA_SIZE];
    memset(mddf, 0, sizeof(mddf));

    /* MDDF format (simplified):
     * Offset 0-1:   MDDF version ($1000)
     * Offset 2-3:   Volume ID
     * Offset 4-35:  Volume name (Pascal string: length byte + chars)
     * Offset 36-37: Number of files
     * Offset 38-39: Total blocks
     * Offset 40-43: Block size
     * Offset 44-45: First data block
     * Offset 46-47: Catalog start block */
    mddf[0] = 0x10; mddf[1] = 0x00; /* Version */
    mddf[2] = 0x00; mddf[3] = 0x01; /* Volume ID */

    /* Volume name as Pascal string */
    int namelen = (int)strlen(db->volume_name);
    if (namelen > 31) namelen = 31;
    mddf[4] = (uint8_t)namelen;
    memcpy(mddf + 5, db->volume_name, namelen);

    /* File count */
    mddf[36] = (db->num_files >> 8) & 0xFF;
    mddf[37] = db->num_files & 0xFF;

    /* Total blocks */
    mddf[38] = (db->total_blocks >> 8) & 0xFF;
    mddf[39] = db->total_blocks & 0xFF;

    /* Block size */
    uint32_t bs = PROFILE_DATA_SIZE;
    mddf[40] = (bs >> 24) & 0xFF;
    mddf[41] = (bs >> 16) & 0xFF;
    mddf[42] = (bs >> 8)  & 0xFF;
    mddf[43] = bs & 0xFF;

    /* Catalog start */
    uint16_t cat_block = BOOT_TRACK_BLOCKS + 1;
    mddf[46] = (cat_block >> 8) & 0xFF;
    mddf[47] = cat_block & 0xFF;

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
