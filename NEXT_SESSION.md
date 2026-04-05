# LisaEmu — Toolchain Status (April 5, 2026)

## How to Audit

Run `make audit` to get a full report of all 4 toolchain stages.
Run individual stages: `make audit-parser`, `make audit-codegen`, `make audit-asm`, `make audit-linker`.

Detailed errors go to `build/audit_errors.txt`.

## Current Metrics

```
STAGE 1: PARSER
  Pascal files: 413, OK: 364 (88%), FAIL: 49 (all non-code: build scripts, docs)
  Actual source code: 100% parse success
  AST nodes: 657,327

STAGE 2: CODEGEN
  2,623 KB code generated from 413 Pascal files
  15,610 globals, 32,591 relocations
  Symbol resolution: 93.0% of unique symbols, 74.0% of references

STAGE 3: ASSEMBLER
  118 assembly files, 104 OK (88%)
  Failures: LIBHW include-fragments (assembled via DRIVERS master) + LIBWM-ASM
  108 KB code, 1,228 .DEF exports, 496 .PROC/.FUNC entries

STAGE 4: LINKER
  435 modules linked (338 Pascal + 97 Assembly), ~2.7 MB output
  9,907 symbols all resolved
  33,557 JSR instructions: 92.5% to real code, 6.5% to stub
```

## What Needs Fixing (Priority Order)

### 1. FP wrapper functions (fpaddx, fpdivx, fpmulx... ~400+ refs)
The FPLIB Pascal unit declares fpaddx/fpdivx/etc. in INTERFACE but their
implementation bodies are in `{$I}` included files (libfp/x80.text, etc.).
The `{$I}` include directive IS implemented but some included files may
not resolve due to path mapping. The assembly implementations use `%` prefixed
names: `%f_ADD`, `%f_SUB`, etc. in NEWFPSUB.

Fix approach: trace which `{$I}` includes fail to resolve and fix the path
mapping. The implementations may also be in files that need `{$IFC}` to
conditionally include the right variant.

### 2. Clascal polymorphic method dispatch (~500+ refs)
Methods like AddImage, DelObject, GetAt are virtual method calls that need
vtable dispatch. The linker's `.MethodName` suffix matching handles some,
but polymorphic methods (same name in multiple classes) need proper dispatch
through the class method table.

The codegen emits `JSR AddImage` but should emit code that:
1. Gets the object's class descriptor pointer
2. Indexes into the method dispatch table
3. Calls through the vtable

### 3. `%` prefixed runtime functions (~100+ refs)
Lisa Pascal uses `%_FuncName` for internal runtime (InClass→%_InObCN,
HALT→%_HALT, etc.). The lexer accepts `%` as identifier start character,
but the codegen/linker may not be exporting these symbols properly from
UCLASCAL and other runtime units.

Fix: verify that `%_InObCN`, `%_HALT`, etc. appear in the linker symbol
table. If not, trace why the codegen doesn't export them.

### 4. WRITELN/READLN name mangling (354 refs)
WRITELN→%W_LN/%_WriteLn, READLN→%R_LN/%_ReadLn in LIBPL.
Currently treated as no-op stubs. The real runtime functions exist in
libpl-appastext (assembly) and libpl-BLOCKIO2 (Pascal).
Need to emit JSR to the mangled name.

### 5. Assembly LIBHW include fragments (14 failures)
The LIBHW sub-files (TIMERS, KEYBD, MOUSE, CURSOR, MACHINE, LEGENDS,
SPRKEYBD) are .INCLUDE fragments of DRIVERS.TEXT. Should be skipped
in should_skip_file since DRIVERS assembles them all.

## Architecture Notes

- `make audit` is the single source of truth for toolchain health
- `{$I filename}` include directive is implemented in the lexer with path resolution
- `{$SETC}` supports AND/OR/NOT boolean expressions for conditional compilation
- `{$IFC}/{$ELSEC}/{$ENDC}` conditional compilation is implemented
- Shared globals AND types tables pass symbols between compilation units
- STARTUP compiled LAST (sees all globals) but linked FIRST (at $400)
- File sorting: case-insensitive sort ensures units compile before fragments
- Linker matches: exact case-insensitive → 8-char prefix → `.MethodName` suffix
- LIBFP: assembly fragments skipped after content detection; Pascal files compiled
- Assembler: .PROC/.FUNC always exported to linker; .REF→.PROC upgrades flags
