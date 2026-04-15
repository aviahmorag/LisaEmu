#include "compile_targets.h"
#include <string.h>
#include <strings.h>

/* ---- SYSTEM.OS kernel modules (from ALEX-LINK-SYSTEMOS.TEXT) ----
 *
 * These 46 modules (plus SYSTEM.OS itself as the output name) form
 * the OS kernel loaded by the boot ROM → boot loader → OS loader.
 * Apple's original link order matters for segment placement, but our
 * compile order is adjusted so INTERFACE/DEFS files come first and
 * STARTUP (the boot entry) compiles last. */
/* Matches _inspiration/LisaSourceCompilation-main/src/COMP/ALEX-COMP-
 * SYSTEMOS.TEXT (source files compiled for SYSTEM.OS) plus LINK-
 * SYSTEMOS.TEXT (link-time ordering). Some modules use $I-include to
 * pull in sub-files (e.g. PROCMGMT includes PMMAKE/PMSPROCS). */
static const char *const SYSTEM_OS_MODULES[] = {
    /* Assembly hardware/runtime */
    "HWINTL",
    /* Global definitions + primitives */
    "DRIVERDEFS",
    "SYSGLOBAL",
    "PASCALDEFS",
    "MMPRIM",
    "PROCPRIMS",
    "EXCEPRIM",
    "FSPRIM",
    /* Assembly implementations */
    "MMASM",
    "PROCASM",
    "EXCEPASM",
    "FSASM",
    "DBGASM",
    "CDCONFIGASM",
    "INITRAP",
    "NMIHANDLER",
    "MOVER",
    /* Pascal bodies (order roughly matches COMP-SYSTEMOS.TEXT) */
    "ASYNCTR",
    "SCHED",
    "GENIO",
    "TWIGGY",
    "HDISK",
    "VMSTUFF",
    "SFILEIO",
    "FSDIR",
    "FSUI",
    "PMEM",
    "CLOCK",
    "OBJIO",
    "MM0",
    "DS0",
    "OSUNITIO",
    "EVENTCHN",
    "EXCEPMGR",
    "LOAD",
    "MEASURE",
    "TIMEMGR",
    "SCAVENGER",
    "VOLCHK",      /* source file is VOLCHK; exports VOLCHECK */
    "FSINIT",
    "CD",
    "DRIVERSUBS",
    "LDUTIL",
    "PROCMGMT",    /* $I-includes PMMAKE, PMCNTRL, PMTERM, PMSPROCS */
    "OSINTPASLIB",
    "PASMATH",
    "STARASM1",
    "STARASM2",
    "STARASM3",
    /* Boot entry — linker places at $400 */
    "STARTUP",
    NULL
};

/* Relative to the source_dir root passed to the toolchain (typically
 * `Lisa_Source`). Do NOT prefix with "Lisa_Source/". */
static const char *const SYSTEM_OS_DIRS[] = {
    "LISA_OS/OS",
    "LISA_OS/LIBHW",
    NULL
};

static const compile_target_t TARGET_SYSTEM_OS = {
    .name = "SYSTEM.OS",
    .out_path = "OBJECT/SYSTEM.OS",
    .modules = SYSTEM_OS_MODULES,
    .search_dirs = SYSTEM_OS_DIRS,
};

/* Registry. New targets (SYS1LIB, LIBQD, Shell, apps...) go here as
 * each ships. For now we only build SYSTEM.OS; other targets are
 * declared in comments as future work. */
static const compile_target_t *const ALL_TARGETS[] = {
    &TARGET_SYSTEM_OS,
    /* TODO: SYS1LIB, SYS2LIB, LIBQD, LIBTK, LIBPL, LIBHW (as standalone),
     * LIBOS, APBG, APDM, APIM, APLC, APLD, APLL, APLP, APLT, APLW, APPW,
     * Shell, Desktop. Each compiles separately, writes a linked object
     * onto the target disk at OBJECT/<name>.OBJ where the OS loader
     * finds it at runtime. */
    NULL
};

const compile_target_t *compile_targets_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; ALL_TARGETS[i]; i++) {
        if (strcasecmp(ALL_TARGETS[i]->name, name) == 0)
            return ALL_TARGETS[i];
    }
    return NULL;
}

const compile_target_t *const *compile_targets_all(void) {
    return ALL_TARGETS;
}
