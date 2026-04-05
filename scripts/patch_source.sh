#!/bin/bash
# LisaEmu — Source preprocessing script
# Applies critical patches from LisaSourceCompilation project
# Run before compilation: ./scripts/patch_source.sh Lisa_Source
#
# Only patches files needed for OS boot (not apps/libraries).
# Does NOT modify originals — creates .patched copies.

set -e

SOURCE_DIR="${1:?Usage: $0 <path_to_Lisa_Source>}"

if [ ! -d "$SOURCE_DIR" ]; then
    echo "Error: $SOURCE_DIR is not a directory"
    exit 1
fi

PATCH_COUNT=0

# Helper: apply a sed replacement to a file (in-place)
patch_file() {
    local file="$1"
    local pattern="$2"
    local replacement="$3"

    if [ -f "$file" ]; then
        if grep -q "$pattern" "$file" 2>/dev/null; then
            sed -i.bak "s|$pattern|$replacement|g" "$file"
            rm -f "$file.bak"
            PATCH_COUNT=$((PATCH_COUNT + 1))
        fi
    fi
}

echo "Patching Lisa OS source files..."

# --- Critical OS build flags ---

# DRIVERDEFS: Disable DEBUG1 and TWIGGYBUILD
F="$SOURCE_DIR/LISA_OS/OS/SOURCE-DRIVERDEFS.TEXT.unix.txt"
patch_file "$F" 'SETC DEBUG1:=TRUE' 'SETC DEBUG1:=FALSE'
patch_file "$F" 'SETC TWIGGYBUILD:=TRUE' 'SETC TWIGGYBUILD:=FALSE'

# PASCALDEFS: Same flags in assembly
F="$SOURCE_DIR/LISA_OS/OS/source-PASCALDEFS.TEXT.unix.txt"
patch_file "$F" 'DEBUG1          .EQU    1' 'DEBUG1          .EQU    0'
patch_file "$F" 'TWIGGYBUILD     .EQU    1' 'TWIGGYBUILD     .EQU    0'

# --- ProFile large disk support ---
F="$SOURCE_DIR/LISA_OS/OS/SOURCE-PROFILE.TEXT.unix.txt"
patch_file "$F" 'discsize > 30000' 'discsize > 500000'

# --- LIBDB compilation flags ---
F="$SOURCE_DIR/LISA_OS/LIBS/LIBDB/libdb-LMSCAN.TEXT.unix.txt"
if [ -f "$F" ] && ! grep -q 'fSymOk' "$F" 2>/dev/null; then
    sed -i.bak 's/{$SETC OSBUILT := TRUE }/{$SETC OSBUILT := TRUE  }\n{$SETC fSymOk := FALSE  }\n{$SETC fTRACE := FALSE  }/' "$F"
    rm -f "$F.bak"
    PATCH_COUNT=$((PATCH_COUNT + 1))
fi

# --- LIBDB typo fix ---
patch_file "$F" 'procedure diffWAdDelete' 'procedure diffWADelete'

# --- LIBFP: Str2Dec external declaration ---
F="$SOURCE_DIR/LISA_OS/LIBS/LIBFP/libfp-NEWFPLIB.TEXT.unix.txt"
if [ -f "$F" ]; then
    patch_file "$F" '{$I libFP/str2dec }' 'procedure Str2Dec; external;'
fi

# --- LIBOS: Use correct syscall path ---
F="$SOURCE_DIR/LISA_OS/LIBS/LIBOS/libos-PSYSCALL.TEXT.unix.txt"
if [ -f "$F" ]; then
    patch_file "$F" 'object/syscall.obj' 'libos/syscall.obj'
fi

# --- Copy PASMATH to LIBPL if missing ---
LIBPL_DIR="$SOURCE_DIR/LISA_OS/LIBS/LIBPL"
PASMATH_SRC="$SOURCE_DIR/LISA_OS/OS/source-PASMATH.TEXT.unix.txt"
PASMATH_DST="$LIBPL_DIR/LIBPL-PASMATH.TEXT.unix.txt"
if [ -d "$LIBPL_DIR" ] && [ -f "$PASMATH_SRC" ] && [ ! -f "$PASMATH_DST" ]; then
    cp "$PASMATH_SRC" "$PASMATH_DST"
    echo "Created LIBPL/PASMATH.TEXT from SOURCE/PASMATH.TEXT"
    PATCH_COUNT=$((PATCH_COUNT + 1))
fi

echo "Applied $PATCH_COUNT patches"
