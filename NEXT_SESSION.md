# LisaEmu — Status (April 5, 2026)

## Quick Commands

```bash
make audit              # Full toolchain report (all 4 stages)
make audit-linker       # Just linker stage (fastest)
make                    # Build SDL2 standalone emulator
```

## Toolchain Status: 96.9% JSR Resolution

```
Modules:     436 (338 Pascal + 98 Assembly)
Symbols:     10,823
JSR analysis:
  Total:     33,602
  Real code: 32,575 (96.9%)
  Stub:         837 (2.5%)
  Other:        190

Stub detail: 108 relocations, 39 unique symbols — all truly missing or intractable
```

## Phase 2: Runtime Correctness (NEXT)

The toolchain produces a ~2.7 MB linked binary. The emulator needs to:

### Boot sequence to verify
1. **ROM** → jumps to $400 (STARTUP entry)
2. **STARTUP** → calls PASCALINIT (runtime init)
3. **PASCALINIT** → sets up A5 (globals), A6 (frame), stack
4. **INITSYS** → OS initialization, memory setup
5. **INIT_TRAPV** → installs TRAP exception vectors
6. **ENTER_SCHEDULER** → starts the process scheduler
7. **Desktop** → eventually renders display at $7A000

### Known runtime issues from previous sessions
- TRAP vectors write wrong values ($08000000 instead of handler addresses)
  - Root cause: b_sysglobal_ptr is a cross-unit variable resolved to 0
  - The shared globals mechanism should now provide it (needs verification)
- VIA timers tick but are never loaded by OS code
- Display shows artifacts but nothing recognizable
- STOP instruction halts CPU waiting for interrupts that never arrive

### What to do
1. Rebuild the emulator with current toolchain (`make` or Xcode)
2. Run and capture boot log (first 1M cycles)
3. Check if TRAP vectors are now correct
4. Trace execution path: does it reach INITSYS? ENTER_SCHEDULER?
5. If stuck, identify the exact instruction/address where it fails

## Toolchain Fixes Applied (this session, 20+ commits)

Key infrastructure:
- `make audit` — unified diagnostic tool for all 4 stages
- `{$I filename}` include directive in lexer
- `{$SETC}` AND/OR/NOT boolean expressions
- Shared types + shared globals across compilation units
- File sorting: units before fragments

Key linker fixes:
- .PROC/.FUNC exported without .DEF
- .REF→.PROC/.DEF flag upgrade (cleared external flag)
- Strict exact-match for symbol add (8-char prefix was destroying 915 symbols)
- Clascal method suffix matching
- Name mapping table: InClass→%_InObCN, HALT→%_HALT, FP→%f_*, math→%_SIN etc.

Key codegen fixes:
- Type casts as inline (not JSR) + shared type table
- LogCall/LogSeg as no-op
- SUBCLASS types registered
- Procedure parameter indirect calls via JSR (A0)
- EOF/EOLN as intrinsics

Key skip list fixes:
- MENUS.TEXT/DBOX.TEXT removed (caught real source)
- LIBFP content-based asm/Pascal split
- libqd-STRETCH.TEXT restored (standalone, not include fragment)
