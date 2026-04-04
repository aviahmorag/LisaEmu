# Toolchain — Compiling Lisa OS 3.1 from Source

## The Challenge

Lisa_Source contains everything needed to build Lisa OS: Pascal source, assembly source, build scripts, linkmaps, font files, and a handful of pre-compiled objects. But the original build tools (Lisa Pascal compiler, assembler, linker) ran on the Lisa Workshop — they don't exist for modern systems.

To go from source to a running Lisa, we need to build (or find) cross-compilation tools that produce Lisa-compatible binaries.

## What the Original Build Used

From analysis of BUILD-*.TEXT scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/`:

| Tool | Command | Purpose |
|------|---------|---------|
| Pascal Compiler | `P{ascal Compile}` | Compiles Lisa Pascal → intermediate |
| Code Generator | `G{enerate}` | Intermediate → object code |
| Assembler | `A{ssemble}` | 68000 assembly → object file |
| Linker | `L{ink}` | Objects → executable |
| IUManager | `IUManager` | Manages intrinsic.lib (runtime library container) |
| ChangeSeg | `changeseg` | Reassigns segment names in objects |

## What We Need to Build

### 1. Motorola 68000 Cross-Assembler

**Status**: Largely solved. Several options exist:
- **vasm** (open source, mature) — supports Motorola syntax
- **Easy68K** — educational 68000 assembler
- Custom assembler matching Lisa syntax (`.func`, `.proc`, `.ref`, `equ` directives)

**Complications**: Lisa assembly uses some custom directives and the `.INCLUDE` mechanism references Lisa filesystem paths (`SOURCE/PASCALDEFS.TEXT`).

### 2. Lisa Pascal Cross-Compiler

**Status**: This is the hard part. Lisa Pascal is a custom Apple dialect with:
- `UNIT` / `INTERFACE` / `IMPLEMENTATION` structure
- Compiler directives: `{$U path}` (use unit), `{$IFC}` (conditional), `{$R-}` (range check off), `{$X-}` (stack expansion off)
- `STRING[n]` types
- `PACKED ARRAY` and `PACKED RECORD`
- `SET OF` with large ranges
- 68000-specific calling conventions
- Segment management directives
- The `USES` clause with object file paths

**Options**:
1. **Write a Lisa Pascal → C transpiler** — Parse Lisa Pascal, generate equivalent C, compile with a 68000 C cross-compiler
2. **Write a Lisa Pascal cross-compiler** — Full compiler targeting 68000 object format
3. **Use the Lisa Workshop itself** — Run the Lisa Workshop in the emulator to compile (chicken-and-egg, but possible with a bootstrap ROM)
4. **Adapt an existing Pascal compiler** — Free Pascal or GNU Pascal as a base, add Lisa extensions

### 3. Linker

**Status**: Needs to be written.
- Must understand Lisa object file format (segment tables, module lists, entry points)
- Must handle the intrinsic library format (`.lib` files)
- Must produce the Lisa executable format
- Linkmaps in the source show exactly what the output should look like

### 4. Disk Image Builder

**Status**: Needs to be written.
- Must create a bootable disk image (ProFile or Sony format)
- Must write boot tracks (`system.bt_Profile`, `system.bt_Sony`, etc.)
- Must lay out the file system (Lisa FS format)
- Must install all system files, libraries, applications, fonts, and device configs

## Build Order (from source scripts)

```
Phase 1: Libraries
  1. iospaslib      — I/O and Pascal runtime
  2. iosfplib       — Floating point (IEEE/SANE)
  3. sys1lib        — QuickDraw, Window Manager, Font Manager (1069 modules)
  4. sys2lib        — Additional system support
  5. prlib          — Print system
  6. tklib          — Toolkit

Phase 2: OS Kernel
  7. Compile all OS/*.TEXT sources → object/*.OBJ
  8. Link 46 objects → object/system.os

Phase 3: Applications
  9. Compile each app (APLC, APLD, APLW, etc.)
  10. Link each app against libraries

Phase 4: Packaging
  11. Install libraries into intrinsic.lib
  12. Write boot tracks for target device
  13. Copy system.os, system.shell, configs, apps, fonts to disk
  14. Build APIN installer diskettes (optional)
```

## Pragmatic Bootstrap Strategy

Since building the full toolchain is a major project, here's a phased approach:

### Phase A: Emulator with External Binaries
Use pre-existing Lisa ROM dumps and disk images (from preservation communities) to verify the emulator works. This proves the hardware emulation is correct.

### Phase B: 68000 Assembler Integration
Integrate an existing 68000 cross-assembler (vasm) to compile the assembly source files. This covers ~40% of the codebase (all hardware drivers, startup code, performance-critical routines).

### Phase C: Lisa Pascal Compiler
Build a Lisa Pascal cross-compiler. Start with a subset that can compile the simpler units, then expand. The linkmaps provide exact expected output to validate against.

### Phase D: Full Self-Hosted Build
Once the toolchain can compile all source files, build Lisa OS entirely from `Lisa_Source/` and boot it in the emulator. No external binaries needed.

## Object File Format

From linkmap analysis, Lisa object files contain:
- **Modules**: Named code/data units (typically one per Pascal unit or assembly file)
- **Segments**: Named memory regions (e.g., `graf`, `SMwork`, `FMwork`, `JT`)
- **Entries**: Named symbols (functions/procedures) visible to the linker
- **References**: Cross-module symbol references to resolve at link time
- **Global data**: Statically allocated data with assigned offsets

Example from Filer linkmap:
```
Files read:    12
Segments:      69
Modules:       795 (617 active)
Entries:       1029 (296 visible)
Ref lists:     1752
References:    2891
Global data:   $0006D2 bytes
```
