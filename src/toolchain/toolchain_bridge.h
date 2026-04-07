/*
 * LisaEm Toolchain — C Bridge API for Swift
 *
 * Provides a simple C API that Swift can call through the bridging header
 * to compile Lisa source, link objects, and build disk images.
 */

#ifndef TOOLCHAIN_BRIDGE_H
#define TOOLCHAIN_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build progress callback */
typedef void (*build_progress_fn)(const char *message, int current, int total);

/* Build result */
typedef struct {
    bool success;
    char error_message[512];
    char output_path[512];    /* Path to the built disk image */
    int files_compiled;
    int files_assembled;
    int files_linked;
    int errors;
} build_result_t;

/*
 * Build a bootable Lisa disk image from source.
 *
 * source_dir: path to the Lisa_Source directory
 * output_dir: path to write build artifacts
 * progress:   callback for progress updates (may be NULL)
 *
 * Returns: build result with success/failure and output path
 */
build_result_t toolchain_build(const char *source_dir,
                                const char *output_dir,
                                build_progress_fn progress);

/* HLE addresses exported by toolchain_build (valid after successful build) */
extern uint32_t hle_addr_calldriver;
extern uint32_t hle_addr_system_error;

/*
 * Quick check if a directory looks like a valid Lisa_Source tree.
 * Returns true if expected subdirectories exist.
 */
bool toolchain_validate_source(const char *source_dir);

/*
 * Get the path to a specific built artifact.
 * artifact: "profile" for ProFile image, "rom" for ROM, "floppy" for floppy
 */
const char *toolchain_get_artifact(const char *output_dir, const char *artifact);

#ifdef __cplusplus
}
#endif

#endif /* TOOLCHAIN_BRIDGE_H */
