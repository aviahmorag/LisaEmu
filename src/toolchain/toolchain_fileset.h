#ifndef TOOLCHAIN_FILESET_H
#define TOOLCHAIN_FILESET_H

#include <stdbool.h>

/*
 * Shared file/directory exclusion rules used by every source-enumeration
 * point in the toolchain (audit_toolchain.c + toolchain_bridge.c).
 *
 * Keeping a single source of truth prevents the two enumerators from
 * drifting — a past bug that caused `make audit` to report phantom
 * "Parser fail" on files the real pipeline was already correctly
 * skipping.
 *
 * Both functions take just the leaf name, not the full path, so they
 * match whether the caller is walking subdirectories recursively or
 * handing a single file.
 */

bool tc_should_skip_dir(const char *name);
bool tc_should_skip_file(const char *name);

#endif
