/*
 * boot_progress.c — see boot_progress.h.
 *
 * Implementation notes:
 *   - Map file has ~8k symbols. We keep a flat array of (addr, name).
 *   - For O(1) hot-path lookup we maintain a 24-bit-addressable lookup
 *     table `pc_to_idx[1 << 20]` (2 MB, uint16_t), where 0 = no entry
 *     at that PC and N = (1-based) index into sym[] array.
 *   - Entries only populated for addresses < 16 MB (Lisa's address
 *     space). Higher (e.g. ROM at $FE0000) also handled via &0xFFFFF
 *     since ROM is small and we only care about Pascal code entries.
 *   - Visited bitset uses 1 bit per symbol.
 *   - Milestones are a curated subset, looked up by name at init time.
 */
#include "boot_progress.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LOOKUP_MASK  0xFFFFF        /* 20-bit index = 1 MB window */
#define LOOKUP_SIZE  (LOOKUP_MASK + 1)
#define MAX_SYMS     16384

typedef struct {
    uint32_t addr;
    char     name[64];
} bp_sym_t;

static bp_sym_t *sym = NULL;
static int       num_sym = 0;
static uint16_t *pc_to_idx = NULL;   /* sparse lookup, 0 = empty, else 1-based idx */
static uint32_t *visited_bits = NULL; /* bitset: num_sym bits */
static int       num_visited = 0;

/* Curated milestone list — order matters: this is the expected boot
 * sequence. Each entry's `stage` string is shown in the report. */
typedef struct {
    const char *symbol;
    const char *stage;
    int         sym_idx;   /* -1 if not resolved, else index into sym[] */
    bool        reached;
} bp_milestone_t;

static bp_milestone_t milestones[] = {
    /* Early boot / Pascal runtime setup */
    { "PASCALINIT",     "Pascal runtime entry",            -1, false },
    { "INITSYS",        "OS init (STARTUP main)",          -1, false },
    { "GETLDMAP",       "Load segment map",                 -1, false },
    { "REG_TO_MAPPED",  "Register-to-mapped translation",   -1, false },

    /* Memory manager init */
    { "POOL_INIT",      "sysglobal + sgheap pool init",    -1, false },
    { "INIT_PE",        "Parity handler init",             -1, false },
    { "MM_INIT",        "Memory manager init",             -1, false },
    { "INSERTSDB",      "First SDB inserted",              -1, false },
    { "MAKE_FREE",      "Free-space chain built",          -1, false },
    { "BLD_SEG",        "First segment built (BLD_SEG)",   -1, false },
    { "MAKE_REGION",    "Region created (MAKE_REGION)",    -1, false },

    /* STARTUP body sequence (SOURCE-STARTUP.TEXT:2161+) */
    { "INIT_TRAPV",     "TRAP vectors initialized",        -1, false },
    { "DB_INIT",        "Debugger init",                   -1, false },
    { "AVAIL_INIT",     "Available memory init",           -1, false },
    { "INIT_PROCESS",   "First process structure",         -1, false },
    { "INIT_EM",        "Exception mgr + shell ecb",       -1, false },
    { "EXCEP_SETUP",    "Exception setup",                 -1, false },
    { "INIT_EC",        "Event channels init",             -1, false },
    { "INIT_SCTAB",     "System call table init",          -1, false },
    { "INIT_MEASINFO",  "Measurement facility init",       -1, false },
    { "BOOT_IO_INIT",   "I/O subsystem init",              -1, false },
    { "FS_INIT",        "Filesystem init",                 -1, false },
    { "SYS_PROC_INIT",  "System processes created",        -1, false },
    { "INIT_DRIVER_SPACE", "Driver allocator init",        -1, false },
    { "FS_CLEANUP",     "Filesystem cleanup",              -1, false },
    { "MEM_CLEANUP",    "Memory cleanup",                  -1, false },
    { "PR_CLEANUP",     "Enter scheduler idle loop",       -1, false },

    /* Post-scheduler (far future) */
    { "SHELL",          "Shell loaded",                    -1, false },
    { "WS_MAIN",        "Workshop main",                   -1, false },
};
static const int NUM_MILESTONES = (int)(sizeof(milestones) / sizeof(milestones[0]));

/* ------------------------------------------------------------------ */

static void ensure_alloc(void) {
    if (!sym) sym = calloc(MAX_SYMS, sizeof(*sym));
    if (!pc_to_idx) pc_to_idx = calloc(LOOKUP_SIZE, sizeof(*pc_to_idx));
    if (!visited_bits) visited_bits = calloc((MAX_SYMS + 31) / 32, sizeof(*visited_bits));
}

static int sym_find_by_name(const char *name) {
    for (int i = 0; i < num_sym; i++) {
        if (strcasecmp(sym[i].name, name) == 0) return i;
    }
    return -1;
}

static void resolve_milestones(void) {
    for (int m = 0; m < NUM_MILESTONES; m++) {
        milestones[m].sym_idx = sym_find_by_name(milestones[m].symbol);
        milestones[m].reached = false;
    }
}

bool boot_progress_init(const char *map_path) {
    if (!map_path) return false;
    FILE *f = fopen(map_path, "r");
    if (!f) return false;

    ensure_alloc();
    if (!sym || !pc_to_idx || !visited_bits) { fclose(f); return false; }

    /* Reset state */
    num_sym = 0;
    memset(pc_to_idx, 0, LOOKUP_SIZE * sizeof(*pc_to_idx));
    memset(visited_bits, 0, ((MAX_SYMS + 31) / 32) * sizeof(*visited_bits));
    num_visited = 0;

    /* Parse lines: "$XXXXXX  NAME" (whitespace-tolerant). */
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '$') continue;
        uint32_t addr;
        char name[64];
        if (sscanf(line, "$%x %63s", &addr, name) != 2) continue;
        /* Skip obviously-bogus synthetic entries (addr=0 catch-alls,
         * very high addresses for pseudo-types, etc.) */
        if (addr == 0) continue;
        if (addr >= 0x00F00000 && addr < 0xFE0000) continue;
        if (num_sym >= MAX_SYMS) break;
        sym[num_sym].addr = addr;
        strncpy(sym[num_sym].name, name, 63);
        sym[num_sym].name[63] = 0;
        /* Only populate lookup for first-time addresses — if two symbols
         * resolve to the same PC (common with FORWARD/EXTERNAL dupes),
         * keep the first. */
        uint32_t slot = addr & LOOKUP_MASK;
        if (pc_to_idx[slot] == 0) {
            pc_to_idx[slot] = (uint16_t)(num_sym + 1);
        }
        num_sym++;
    }
    fclose(f);

    resolve_milestones();
    int m_resolved = 0;
    for (int i = 0; i < NUM_MILESTONES; i++)
        if (milestones[i].sym_idx >= 0) m_resolved++;
    fprintf(stderr, "boot_progress: loaded %d symbols from %s (%d/%d milestones resolved)\n",
            num_sym, map_path, m_resolved, NUM_MILESTONES);
    return true;
}

void boot_progress_record_pc(uint32_t pc) {
    if (!pc_to_idx || !visited_bits) return;
    uint16_t idx1 = pc_to_idx[pc & LOOKUP_MASK];
    if (!idx1) return;
    int idx = idx1 - 1;
    /* Confirm actual address matches (same slot may collide with different
     * address outside LOOKUP_MASK window — cheap double-check). */
    if (idx >= num_sym || sym[idx].addr != pc) return;
    uint32_t mask = 1u << (idx & 31);
    if (visited_bits[idx >> 5] & mask) return;  /* already visited */
    visited_bits[idx >> 5] |= mask;
    num_visited++;

    /* Check if this is a milestone */
    for (int m = 0; m < NUM_MILESTONES; m++) {
        if (milestones[m].sym_idx == idx && !milestones[m].reached) {
            milestones[m].reached = true;
            fprintf(stderr, "✅ boot-progress: reached %-18s ($%06X) — %s\n",
                    milestones[m].symbol, pc, milestones[m].stage);
            return;
        }
    }
}

void boot_progress_report(FILE *out) {
    if (!out) out = stderr;
    if (!sym) {
        fprintf(out, "boot_progress: not initialized\n");
        return;
    }
    /* Suppress back-to-back duplicate reports (e.g., SYSTEM_ERROR fires it,
     * then headless exit fires it again with identical state). */
    static int last_reported_visited = -1;
    if (last_reported_visited == num_visited) return;
    last_reported_visited = num_visited;
    /* Compute last-reached milestone */
    int last_reached = -1;
    int milestones_reached = 0;
    int milestones_resolved = 0;
    for (int m = 0; m < NUM_MILESTONES; m++) {
        if (milestones[m].sym_idx >= 0) milestones_resolved++;
        if (milestones[m].reached) { last_reached = m; milestones_reached++; }
    }

    fprintf(out, "\n");
    fprintf(out, "======== BOOT PROGRESS REPORT ========\n");
    fprintf(out, "Procedures entered: %d / %d  (%.1f%%)\n",
            num_visited, num_sym,
            num_sym ? 100.0 * num_visited / num_sym : 0.0);
    fprintf(out, "Milestones reached: %d / %d resolved  (of %d in map)\n",
            milestones_reached, milestones_resolved, NUM_MILESTONES);
    if (last_reached >= 0) {
        fprintf(out, "Last checkpoint:    %s — %s\n",
                milestones[last_reached].symbol, milestones[last_reached].stage);
    } else {
        fprintf(out, "Last checkpoint:    (none reached)\n");
    }
    /* Next expected — scan milestones after last_reached for the first
     * one that resolved but wasn't reached. Indicates where we blocked. */
    for (int m = last_reached + 1; m < NUM_MILESTONES; m++) {
        if (milestones[m].sym_idx >= 0 && !milestones[m].reached) {
            fprintf(out, "Next expected:      %s — %s  (not reached)\n",
                    milestones[m].symbol, milestones[m].stage);
            break;
        }
    }
    fprintf(out, "\nMilestone status:\n");
    for (int m = 0; m < NUM_MILESTONES; m++) {
        const char *mark;
        if (milestones[m].sym_idx < 0)   mark = "⧗";  /* not in map */
        else if (milestones[m].reached)  mark = "✅";
        else                             mark = "  ";
        fprintf(out, "  %s  %-20s  %s\n",
                mark, milestones[m].symbol, milestones[m].stage);
    }
    fprintf(out, "======================================\n\n");
}

void boot_progress_reset(void) {
    if (!visited_bits) return;
    memset(visited_bits, 0, ((MAX_SYMS + 31) / 32) * sizeof(*visited_bits));
    num_visited = 0;
    for (int m = 0; m < NUM_MILESTONES; m++) milestones[m].reached = false;
}

void boot_progress_shutdown(void) {
    free(sym); sym = NULL;
    free(pc_to_idx); pc_to_idx = NULL;
    free(visited_bits); visited_bits = NULL;
    num_sym = 0;
    num_visited = 0;
}
