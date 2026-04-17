/*
 * boot_progress.h — boot-time instrumentation for measuring how far
 * into the Lisa OS boot sequence we've gotten.
 *
 * Loads the linker symbol map on init, then records every time the CPU
 * enters a known procedure. Prints a summary at end of run (or on
 * SYSTEM_ERROR halt) showing:
 *   - last milestone reached (curated STARTUP checkpoint sequence)
 *   - count/percent of unique procedures entered
 *   - list of reached milestones
 *
 * Hotpath: boot_progress_record_pc() is called once per instruction.
 * It does a single array lookup and early-returns for non-entry PCs.
 */
#ifndef BOOT_PROGRESS_H
#define BOOT_PROGRESS_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Load symbol map from `path` (format: "$XXXXXX  NAME" per line).
 * Returns true on success. Safe to call multiple times (last wins).
 * If never called or if path is missing, record_pc() is a no-op. */
bool boot_progress_init(const char *map_path);

/* Hot path: call on every instruction (or at least on every JSR target).
 * O(1). Marks the proc as visited if PC is a known entry point. */
void boot_progress_record_pc(uint32_t pc);

/* Print a human-readable summary to the given file. Shows:
 *   - Total procs entered / total procs in map
 *   - Last milestone reached (from curated list)
 *   - Per-milestone status (✅ / ❌)
 */
void boot_progress_report(FILE *out);

/* Reset all visited state (keeps loaded symbol map). */
void boot_progress_reset(void);

/* Free all state. */
void boot_progress_shutdown(void);

/* Look up a symbol by name. Returns its address if found, else 0. */
uint32_t boot_progress_lookup(const char *name);

/* True iff the named milestone has been marked reached. */
bool boot_progress_reached(const char *name);

#endif
