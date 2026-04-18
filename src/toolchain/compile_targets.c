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
    /* Loose mode: walk search_dirs and compile every source found.
     * The kernel pulls in dozens of $I-included helper files whose
     * names don't appear in SYSTEM_OS_MODULES; flipping to strict
     * regresses the kernel build until the module list + $I audit
     * is finished. Tracked as pre-existing tech debt. */
    .strict = false,
    .boot_entry = "STARTUP",
};

/* ---- SYSTEM.BT_PROFILE boot-track binary (Phase 2 target) ----
 *
 * Apple's boot-track binary for a ProFile boot device. Source:
 *   _inspiration/LisaSourceCompilation-main/src/MAKE/ALEX-MAKE-BTPROFILE.TEXT
 *
 * The real Lisa ROM reads boot-track blocks from the boot device and
 * jumps to LDPROF's entry. LDPROF + PROF provide ProFile-specific
 * block-read primitives; the seven common modules (LDASM, SERNUM,
 * LDUTIL, LOADER, LDLFS, PASMATH, OSINTPASLIB) implement the real
 * filesystem-walking loader that finds SYSTEM.OS by name and jumps
 * into INITSYS.
 *
 * Apple's link order (from ALEX-MAKE-BTPROFILE.TEXT, output file
 * system.bt_Profile):
 *
 *   LDPROF  (boot entry — assembled from source-LDPROF.TEXT)
 *   LDASM   (shared asm helpers)
 *   PROF    (Pascal unit: lddrivers — ProFile driver)
 *   SERNUM  (serial-number asm)
 *   LDUTIL  (Pascal unit: boot utility)
 *   LOADER  (Pascal PROGRAM: main loader body — calls OPENINPUT etc.)
 *   LDLFS   (Pascal unit: filesystem / catalog walker)
 *   PASMATH (Pascal runtime: math)
 *   OSINTPASLIB (Pascal runtime: I/O + interrupts)
 *
 * LOADER.TEXT imports proc_prims / asynctr / genio / twiggy / vmstuff /
 * sfileio / fs_primitives / driverdefs / sysglobal via $U — these
 * resolve INTERFACE-only at link time (LOADER only uses their TYPE
 * declarations and constants; no kernel bodies are linked into the
 * boot-track blob).
 *
 * Our compile order (analogous to how SYSTEM.OS puts STARTUP last):
 * runtime + utility first, LDPROF's boot-entry asm last. Final link
 * order will match Apple's for segment placement. */
static const char *const BT_PROFILE_MODULES[] = {
    /* Pascal runtime — no kernel deps */
    "PASMATH",
    "OSINTPASLIB",
    /* Boot utilities */
    "LDUTIL",
    "LDLFS",
    /* Device driver (Pascal unit `lddrivers`) */
    "PROF",
    /* Shared asm helpers */
    "LDASM",
    "SERNUM",
    /* Main loader PROGRAM body */
    "LOADER",
    /* Boot entry — assembled last so its entry point lands at the
     * start of the linked blob, matching Apple's link order. */
    "LDPROF",
    NULL
};

static const compile_target_t TARGET_BT_PROFILE = {
    .name = "SYSTEM.BT_PROFILE",
    .out_path = "system.bt_Profile",   /* on-disk filename (lowercase per Apple) */
    .modules = BT_PROFILE_MODULES,
    .search_dirs = SYSTEM_OS_DIRS,     /* all 9 sources live in LISA_OS/OS */
    /* Strict mode: only the 9 modules listed above get compiled +
     * linked into this blob. The boot-track binary must be small and
     * contain exactly the loader chain — Apple's linked boot blob is
     * ~30-50 KB, not the full kernel. Shared type/symbol definitions
     * from the SYSTEM.OS compile pass remain resolvable via the
     * shared_types / shared_globals tables (see toolchain_bridge
     * comment above the ALL_TARGETS loop). */
    .strict = true,
    .boot_entry = "LDPROF",
};

/* Registry. New targets (SYS1LIB, LIBQD, Shell, apps...) go here as
 * each ships. */
static const compile_target_t *const ALL_TARGETS[] = {
    &TARGET_SYSTEM_OS,
    &TARGET_BT_PROFILE,
    /* TODO: SYSTEM.CD_PROFILE (Phase 3), SYS1LIB, SYS2LIB, LIBQD,
     * LIBTK, LIBPL, LIBHW (as standalone), LIBOS, APBG, APDM, APIM,
     * APLC, APLD, APLL, APLP, APLT, APLW, APPW, Shell, Desktop. */
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
