/*
 * LisaEm Toolchain — Disk Image Builder
 *
 * Creates bootable Lisa disk images from linked executables.
 *
 * Lisa disk formats:
 *   ProFile: 532 bytes/block (512 data + 20 tag bytes)
 *   Sony 3.5": 400K/800K
 *
 * Lisa filesystem:
 *   - Hierarchical directory with file catalog
 *   - Boot track at blocks 0-23 (device-specific boot code)
 *   - MDDF (Master Directory Data File) for volume metadata
 *   - File entries with type, creation date, data fork
 */

#ifndef DISKIMAGE_H
#define DISKIMAGE_H

#include <stdint.h>
#include <stdbool.h>

/* ProFile disk geometry */
#define PROFILE_BLOCK_SIZE    532    /* 512 data + 20 tag */
#define PROFILE_DATA_SIZE     512
#define PROFILE_TAG_SIZE      20
#define PROFILE_5MB_BLOCKS    9728   /* 5MB ProFile */
#define PROFILE_10MB_BLOCKS   19456  /* 10MB ProFile */

/* Boot track capacity: sized for SYSTEM.BT_PROFILE's linked blob.
 * Apple's real Lisa boot track is bigger than the 24-block (12 KB)
 * value we used when SYSTEM.OS was being mis-written as boot code —
 * 64 blocks (32 KB) comfortably holds a minimal BT_PROFILE (~24 KB
 * today, may grow as module filtering tightens). Block 0 carries
 * the LDPROF entry + tag 0xAAAA; subsequent boot blocks carry the
 * rest of the boot-track binary that LDPROF loads into RAM before
 * the Pascal loader takes over. */
#define BOOT_TRACK_BLOCKS     64

/* File types */
#define FTYPE_NONE      0
#define FTYPE_CODE      1    /* Executable code */
#define FTYPE_DATA      2    /* Data file */
#define FTYPE_OBJ       3    /* Object file */
#define FTYPE_TEXT      4    /* Text file */
#define FTYPE_FONT      5    /* Font file */

/* Maximum files in image */
#define MAX_FILES       256

/* File entry for the disk image */
typedef struct {
    char name[64];          /* Lisa filename */
    uint8_t file_type;
    uint32_t size;          /* Data size in bytes */
    uint8_t *data;          /* File data */
    uint32_t start_block;   /* Starting block on disk */
    uint32_t num_blocks;    /* Number of blocks used */
} disk_file_t;

/* Disk image builder state */
typedef struct {
    /* Disk parameters */
    uint32_t total_blocks;
    uint32_t block_size;    /* Always PROFILE_BLOCK_SIZE for ProFile */

    /* Raw disk image */
    uint8_t *image;
    uint32_t image_size;

    /* File catalog */
    disk_file_t files[MAX_FILES];
    int num_files;

    /* Volume info */
    char volume_name[64];
    uint32_t next_free_block;

    /* Error tracking */
    int num_errors;
    char errors[50][256];
} disk_builder_t;

/* Public API */
void disk_init(disk_builder_t *db, uint32_t num_blocks);
void disk_free(disk_builder_t *db);

/* Set volume name */
void disk_set_volume_name(disk_builder_t *db, const char *name);

/* Write boot track from a boot binary */
bool disk_write_boot_track(disk_builder_t *db, const uint8_t *boot_code, uint32_t size);

/* Add a file to the disk image */
bool disk_add_file(disk_builder_t *db, const char *name, uint8_t file_type,
                   const uint8_t *data, uint32_t size);

/* Add a file from disk */
bool disk_add_file_from_path(disk_builder_t *db, const char *name, uint8_t file_type,
                              const char *path);

/* Finalize the disk image (write catalog, MDDF, etc.) */
bool disk_finalize(disk_builder_t *db);

/* Write the image to a file */
bool disk_write_image(disk_builder_t *db, const char *filename);

/* Get error count */
int disk_get_error_count(disk_builder_t *db);

#endif /* DISKIMAGE_H */
