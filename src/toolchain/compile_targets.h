#ifndef COMPILE_TARGETS_H
#define COMPILE_TARGETS_H

/* Compile targets matching Apple's real Lisa OS link structure.
 *
 * Reference: _inspiration/LisaSourceCompilation-main/src/LINK/
 *
 * Each target is a NAMED list of module basenames (e.g. "STARTUP" →
 * source-STARTUP.TEXT.unix.txt). The toolchain uses these lists
 * instead of directory-walk-and-include-everything to build the
 * separate binaries that existed on the real Lisa:
 *
 *   SYSTEM.OS   — kernel (47 modules) — boot target
 *   SYS1LIB     — shared library used by many apps
 *   SYS2LIB     — another shared library
 *   LIBQD       — QuickDraw (graphics)
 *   LIBTK       — Toolkit (windows/menus/events)
 *   LIBPL       — Pascal runtime library (blockio, etc.)
 *   ...
 *   APBG, APDM, APIM, ... — individual applications
 *
 * Files live on the disk image at their proper paths; the OS's
 * Load_Program loads them on demand (currently stubbed; real
 * implementation is a later phase).
 */

typedef struct {
    const char *name;       /* e.g. "SYSTEM.OS" */
    const char *out_path;   /* e.g. "OBJECT/SYSTEM.OS" on target disk */
    const char *const *modules; /* NULL-terminated list of module basenames */
    const char *const *search_dirs; /* NULL-terminated list of source dirs */
} compile_target_t;

/* Get the target by name (case-insensitive). */
const compile_target_t *compile_targets_find(const char *name);

/* Enumerate all defined targets. */
const compile_target_t *const *compile_targets_all(void);

#endif
