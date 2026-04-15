# LisaEmu - Apple Lisa Emulator

## Project Vision

A native macOS app that takes Apple's officially released Lisa OS source code (`Lisa_Source/`) as input, compiles/assembles it into runnable binaries, and runs a complete Apple Lisa system — like Parallels, but for the Lisa.

The source code (Lisa OS version 3.1) was released by Apple in 2018 via the Computer History Museum:
https://info.computerhistory.org/apple-lisa-code

It is freely available under an Apple Academic License but **cannot be redistributed**. Users supply their own copy.

## Architecture

### Two layers:

1. **Toolchain** — Cross-assembler (68000) + Lisa Pascal cross-compiler + linker that processes `Lisa_Source/` into bootable disk images and ROM
2. **Emulator** — Motorola 68000 CPU + Lisa hardware emulation (memory, VIAs, display, keyboard, mouse, disks)

### Directory Structure

```
/
├── Lisa_Source/           # Apple's source (NOT in git, user-supplied)
├── src/                   # C emulator core + toolchain (canonical source)
│   ├── m68k.h/c           # Motorola 68000 CPU emulator
│   ├── lisa_mmu.h/c       # Memory controller + MMU
│   ├── via6522.h/c        # VIA 6522 chip emulation (x2)
│   ├── lisa.h/c           # Main machine integration
│   ├── lisa_bridge.h/c    # C-to-Swift bridge API
│   ├── main_sdl.c         # Standalone SDL2 frontend (for testing)
│   └── toolchain/         # Cross-compilation pipeline
│       ├── pascal_lexer.h/c      # Lisa Pascal tokenizer
│       ├── pascal_parser.h/c     # Recursive descent parser → AST
│       ├── pascal_codegen.h/c    # AST → 68000 machine code
│       ├── asm68k.h/c            # Two-pass 68000 cross-assembler
│       ├── linker.h/c            # Multi-module linker
│       ├── bootrom.c             # Boot ROM generator
│       ├── diskimage.h/c         # Disk image builder
│       ├── toolchain_bridge.h/c  # Orchestrates full compile pipeline
│       ├── audit_toolchain.c     # Diagnostic tool (make audit)
│       └── test_*.c              # Per-component test tools
├── lisaOS/                # Xcode macOS app (SwiftUI, Swift 6)
│   └── lisaOS/
│       ├── Emulator/      # SYMLINKS to src/ (not copies!)
│       ├── ContentView.swift
│       ├── EmulatorViewModel.swift
│       ├── LisaDisplayView.swift
│       └── lisaOSApp.swift
├── docs/                  # Documentation (Lisa_Source reference, hardware specs)
├── build/                 # Build output (gitignored)
├── Makefile               # Standalone SDL2 build + audit targets
├── CLAUDE.md              # This file
└── NEXT_SESSION.md        # Current status and prioritized fix list
```

## Key Commands

```bash
# Build standalone emulator (SDL2)
make

# Run toolchain audit — the primary diagnostic tool
make audit              # Full report (all 4 stages)
make audit-parser       # Stage 1: Parser only
make audit-codegen      # Stage 2: Codegen only
make audit-asm          # Stage 3: Assembler only
make audit-linker       # Stage 4: Full pipeline + linker

# Xcode build (or just open in Xcode)
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build 2>&1 | grep -E "(error:|BUILD)"
```

## Current Status (2026-04-15 PM5)

### Fix (P23): proc-local TYPE registration + WITH/array codegen

Three related codegen bugs in `src/toolchain/pascal_codegen.c` that
together caused `INIT_JTDRIVER` to scramble the CPU vector table:

**(a) Procedure-local TYPE decls were never registered.** Global
TYPE decls were processed in `process_declarations`, but proc-local
TYPE decls were silently skipped in `gen_proc_or_func`'s declaration
loop. So `type djt = ^driverjt; driverjt = record ... end;` inside a
proc left both names as unregistered types — `^djt` fell back to an
opaque pointer and driverjt's real record size was never computed.
Fix: two-pass proc-local type processing in `gen_proc_or_func` —
first register each `AST_TYPE_DECL` name as a stub (so forward
references like `^driverjt` resolve before `driverjt` is fully
defined), then resolve the actual bodies.

**(b) Array-bounds lookup missed proc-local CONSTs.** Array
`[lo..hi] of T` bound resolution used `find_global` / `find_imported`
but not `find_symbol_any`, so a local `CONST maxdtable = 35` left the
bounds at (0..0) → single-element array, record size collapsed to
first field's size, sizeof wrong. Fix: switch to `find_symbol_any`
for array-bound CONST resolution (bounds resolution happens in
`AST_TYPE_ARRAY` case of `resolve_type`).

**(c) ARRAY_ACCESS inside WITH couldn't see the array's element type.**
When the array's base name is a field of an outer WITH record (e.g.
`jt[i]` inside `WITH jtpointer^^ do`), `find_symbol_any` fails and
the code fell back to `elem_size = 2`. Fix: mirror the existing
FIELD_ACCESS WITH-lookup — if the base symbol isn't found and
`with_depth > 0`, try `with_lookup_field` and use the field's type.

Also fixed: `AST_WITH` handler didn't understand `WITH ptr^^ do` —
only single-deref was handled. Added a `ptr^^` case so the record
type resolves through the double pointer chain.

Result: **VEC-WRITE probe fires 0 times** (was 30 — pre-fix all 17
jump-table entries plus 13 garbage writes landed in the CPU vector
table). `INIT_JTDRIVER` now installs the jump-table in the allocated
sysglobal buffer. Toolchain audit still 100% clean (382 modules,
8711/8711 symbols resolved, link OK). Output +8356 bytes vs P22.

Boot advances: still reaches `BOOT_IO_INIT` but now with a clean
vector table. New blocker: **SYSTEM_ERROR(10201) at ret=$072CC6**
during BOOT_IO_INIT → FS_INIT transition (next milestone `FS_INIT`
not reached). Next session to diagnose — likely a separate bug in
I/O subsystem init or first FS call, not related to INIT_JTDRIVER.

---

## Prior Status (2026-04-15 PM4)

### Fix (P22): TRAP #6 HLE reads SMT from linker `smt_base` symbol

`DO_AN_MMU` (LDASM) hard-references `smt_base` via `lea smt_base,a1`.
Our TRAP #6 HLE was reading `smt_ptr = (A5-4)` — a Pascal local slot
with no SMT pointer — yielding `smt_base=$000000`. It then programmed
segment 85 with SOR=0 from bytes at physical $154, so logical writes
to $AA2600 (seg 85) aliased to **physical page 0**, overwriting
SCTAB2 code and triggering **SYSTEM_ERROR(10201)**.

Fix (`src/m68k.c` `hle_trap6_do_an_mmu`): resolve `smt_base` via
`boot_progress_lookup` on the linker map (falls back to
`g_hle_smt_base` for the synthetic-loader path). Exposed a public
`boot_progress_lookup(name) → addr` accessor over the already-loaded
symbol table.

Boot progress: now **reaches BOOT_IO_INIT**. Three new milestones
crossed since P21: INIT_MEASINFO, INIT_SCTAB completion, BOOT_IO_INIT.
`WATCH-$2600` fires 0 times (was 17). Toolchain audit still 100% clean.

### New blocker: illegal-instruction at $000094 during BOOT_IO_INIT

After BOOT_IO_INIT the CPU takes vector 4 (illegal instruction) from
PC=$000094, lands at `$615262A8` (garbage), A7 gets zeroed, PC hot
pages show `$006D00 / $006E00 / $0A0600` — all in I/O init territory.
Vectors taken before the crash: v39 (TRAP) × 3, v37 (TRAP) × 2.

PC-trail decoded via linker map + targeted probes this session:

1. **HIPC-TRIP probe** (PC-first-leaves-24-bit) fires at $615262A8,
   which comes from **vector-4 handler read at $10** (illegal-instr
   vector). That means vector 4 was corrupted.

2. **VEC-WRITE probe** (writes into $00-$FF from post-boot code)
   catches the corrupter: a 17-iteration unrolled loop at
   `$004192, $0041B6, $0041EE, $004226, $00425E, $004296, ...`
   (56-byte stride), each doing `MOVE.W D0,(A0)` where A0=vector
   MSB-position and D0's low word has a non-zero high byte:
   ```
   PC=$004192 val=$4E (msb of vec0..17)   ← good initial pass
   PC=$0041B6 val=$5F (msb of vec0)
   PC=$0041EE val=$64 (msb of vec1)
   PC=$004226 val=$6A (msb of vec2)
   PC=$004296 val=$61 (msb of vec4)   ← the one that kills us
   ```

3. **Per-iteration code pattern** (each 56-byte block starts with):
   ```
   30 80         MOVE.W D0,(A0)       ; stores prev D0 low word
   41 ED <d16>   LEA d16(A5),A0       ; A0 := A5+offset
   20 08         MOVE.L A0,D0         ; D0 := A0 (32-bit)
   3F 00         MOVE.W D0,-(SP)      ; push low word
   41 F9 <long>  LEA abs.L,A0         ; placeholder (zeros)
   70 0x         MOVEQ #x,D0
   32 00         MOVE.W D0,D1
   C2 FC 00 02   MULU.W #2,D1
   D1 C1         ADDA.L D1,A0
   30 1F         MOVE.W (A7)+,D0     ; pop low word (→ next iter store)
   ```

4. Root cause: **codegen record-size + WITH-double-deref bug in
   INIT_JTDRIVER** (`SOURCE-STARTUP.TEXT:1761-1840`). The Pascal is:
   ```pascal
   CONST drivrjt = $210;
   TYPE  djt = ^driverjt;
         driverjt = record
             jt: array[0..35] of record
                 jmpinstr: integer;       { 2 bytes }
                 routine:  ^integer       { 4 bytes }
             end
         end;
   VAR   jtpointer: ^djt;
   begin
     if GETSPACE(sizeof(driverjt), ..., d_ptr) then begin
       jtpointer := pointer(drivrjt);    { jtpointer := @$210 }
       jtpointer^ := pointer(d_ptr);     { *$210 := d_ptr }
       WITH jtpointer^^ do begin          { scope = **$210 = d_ptr }
         for i := 0 to 35 do
           jt[i].jmpinstr := $4EF9;
         jt[0].routine := @CANCEL_REQ;
         jt[1].routine := @ENQUEUE;
         ...
   ```

   Observed generated code (per-element):
   - Record-element stride = `MULU.W #2,D1` → **size = 2**, but the
     real record size is 6 (2-byte int + 4-byte pointer). Our codegen
     is ignoring `routine: ^integer` when computing array-element size.
   - Base address: `LEA $0,A0; ADDA.L D1,A0` → writes target
     **address 0 + i*2**, i.e. straight into the low-memory vector
     table. The `WITH jtpointer^^ do` scope (which should set A0 to
     `(*$210)` = `d_ptr`) is dropped entirely — codegen treats WITH
     over a double-deref as WITH over base 0.
   - The handler addresses that each block tries to store — values
     like `@CANCEL_REQ`, `@ENQUEUE`, `@GETSPACE`, `@SYSTEM_ERROR`,
     etc. — are being computed as `LEA d16(A5),A0` where `d16` is
     where the linker placed those procs. Those procs are Pascal
     bodies at high addresses (e.g. `@SYSTEM_ERROR` → ~$53C6 + A5 =
     garbage $6152xx because A5 holds sysglobal base). That's where
     the high-byte-$61 values in VEC-WRITE come from.

   So *two* codegen bugs (not one):
   (a) Record-array element size ignores non-primitive tail fields.
   (b) `WITH ptr^^ do ...` double-deref case forgets the base address
       and emits zero.

Next-session task (concrete):
- Pascal codegen, `AST_WITH` handling: find the case that resolves
  a pointer-deref-chain base. Likely only single `AST_DEREF` is
  handled; add recursion for double-deref (AST_DEREF whose child
  is AST_DEREF or an AST_VARREF that resolves to a pointer-to-
  pointer).
- Pascal codegen, record-size computation (`sizeof` for array
  elements): for `array[0..N] of record F1: int; F2: ^T; end`, the
  stride must be 6 (or 8 padded), not 2. Look at how expr_size
  computes array element size — likely summing only the first field.
- Verify: after fix, VEC-WRITE probe should fire only for the initial
  clean pass (18 writes of $4E for $4EF9 opcodes). The 13 subsequent
  garbage writes should move to their real sysjt destination (write
  target near $CCA000 + offset, NOT near $00).

Probes to keep in place (all committed this session):
- `HIPC-TRIP` in m68k_execute (first PC with high byte → dump ring).
- `PUSH-HIPC` / `POP-HIPC` in push32/pop32.
- `TRAP-FRAME` in take_exception for vectors 37/39.
- `VEC-WRITE` in lisa_mem_write8 (writes to $00-$FF, MSB positions).

---

## Prior Status (2026-04-15 PM3) — diagnosis that led to P22

### Diagnosis (P21): SYSTEM_ERROR(10201) at hard_excep+594

Added a targeted PC-probe in `src/m68k.c` that fires when PC=$002600 (the
only hard-exception vector 4 site per VEC-FIRST log). Dump shows:

```
$25F0: 3F00            MOVE.W D0,-(SP)
$25F2: 41ED FB2C       LEA -1236(A5),A0
$25F6: D0FC 0258       ADDA.W #$258,A0
$25FA: 203C 0000 0080  MOVE.L #$80,D0
$2600: 00CC A04E 00CC A482 0013 FFEC 00ED 952C   ← raw DATA, illegal as code
$2610: 2008 3F00 41ED FB2C 203C 0000 0081 3200   ← code resumes, next iter #$81
```

Execution falls straight through $25FA → $2600 (no branch). The 16 bytes
at $2600..$260F look like an inline relocation table (sysglobal
addresses $00CCA04E / $00CCA482, plus two offsets). Hypothesis: codegen
either (a) emitted a data table mid-stream without a BRA over it, or
(b) dropped the JSR at $2600 between the arg-prep and the next loop
iteration at $2610. Need to identify which Pascal proc compiled here
(MMU segmentation makes the raw map lookup unreliable — $2600 has
multiple segment symbols). Boot progress markers reach INIT_SCTAB
(15/27), so the proc is between that and INIT_MEASINFO.

### Fix (P20): non-primitive value params + prefer Pascal body over external sig

Two related codegen fixes that unblock MAKE_REGION → BLD_SEG → INSERTSDB
in STARTUP:

**ARG_BY_REF helper** (`src/toolchain/pascal_codegen.c`): P16 made
`register_proc_sig` / `gen_proc_or_func` treat string/record/array value
params as pass-by-reference (4-byte pointer frame slot). The caller side
was never updated — it still evaluated the record as a value expression
(first field into D0, pushed as 4-byte long with garbage upper word).
Fix: detect non-VAR params of kind `TK_RECORD`/`TK_STRING`/`TK_ARRAY` at
every call site and emit `LEA @arg; MOVE.L A0,-(SP)` (same as VAR args).
Applied in both `AST_FUNC_CALL` and `AST_PROC_CALL`, callee-clean and
caller-clean branches.

**find_proc_sig prefers non-external sig**: When an EXTERNAL declaration
in one unit registered a sig with `is_external=true`, and the Pascal body
in another unit later registered a fresh sig for the same name, the
APPEND-only `shared_proc_sigs` table kept both entries; `find_proc_sig`
returned the first (external) match. Callers then treated the proc as
assembly/callee-clean with left-to-right push, which mismatched the
body's right-to-left Pascal frame. The VAR output param landed at the
wrong offset — BLD_SEG wrote nil to address 0 instead of the caller's
local. Fix: when multiple sigs exist, prefer the one with
`is_external=false`; also clear `is_external` on own sig/sym when
`gen_proc_or_func` emits a body.

**Boot progress**:
- Before P20: hung in INSERTSDB at `$0A38AA-$0A38D8`, IPL=4, because
  `*c_sdb_ptr` was never populated.
- After P20: MAKE_REGION → BLD_SEG correctly passes `@disca` by-ref
  (per P16), pushes right-to-left, pops 24 bytes post-JSR (caller-clean).
  INSERTSDB gets a valid sdb pointer. Boot advances.
- **New blocker**: `SYSTEM_ERROR(10201)` (`e_hardsyscode`, "hardware
  exception while in system code") from `ret=$071FD8`, which maps to
  `hard_excep+594` (hard_excep at `$071D86`). So a real CPU hardware
  exception (bus / address / illegal / etc.) is firing somewhere and
  funneling into the runtime handler.

Toolchain audit: **100% clean** (317/317 Pascal, 103/103 ASM, 8711
symbols resolved, link OK). Output size delta vs P19: +12 bytes.

---

## Prior Status (2026-04-15 AM)

### Fix (P19): narrow RHS → wide longint/pointer stores now sign-extend

When Pascal compiles `count := pages` where `count : longint` and
`pages : integer`, our codegen emitted `MOVE.W pages → D0` then
`MOVE.L D0 → count` — but D0's upper 16 bits were stale scratch from
the previous operation. Result: `count = (pages << 16) | garbage`.
In LD_READSEQ this produced `count = 65739` for `pages = 1`, causing
the loader HLE to attempt a 32MB disk read and corrupt the TRAP
handler frame, crashing the emulator.

Fix: emit `EXT.L D0` (sign-extend 16→32) immediately before any
MOVE.L store when:
- The LHS type is `TK_LONGINT` or `TK_POINTER`,
- Store width is 4, but RHS expression width is < 4.

Applied symmetrically in four assignment paths (pascal_codegen.c,
AST_ASSIGN case):
1. WITH-field stores (~L1920)
2. VAR-param writes (~L1960)
3. Local / non-global writes (~L1990)
4. Global-var writes (~L2010)
5. Complex LHS (array/field/deref) writes (~L2025)

Boot now passes PASCALINIT, reaches a new blocker: tight loop inside
**INSERTSDB at `$0A38AA-$0A38D8`** with IPL=4. INSERTSDB is being
called from a much later code path than the earlier P13 MM_INIT
case — likely another list-walk over an uninitialized field. Next
session to diagnose.

### Fix (P18): LOADER TRAP HLE uses MMU-translated reads/writes

Pre-fix, the loader-trap port handler (src/lisa.c:815) indexed
`lisa->mem.ram[pa+N]` directly, treating the fake_parms pointer as a
physical offset. That worked only while the MMU was disabled
(pre-boot). After the MMU comes up, Pascal OS passes a **logical**
fake_parms pointer (e.g. `$CBFDC8`), which is well above the 2.25 MB
physical RAM — so the old code rejected the call as "invalid parms
addr" and the TRAP handler's RTE chain crashed into the vector table.

Fix: route all fake_parms reads/writes through
`lisa_mem_read{8,16,32}` / `lisa_mem_write{8,16,32}`, which handle
MMU translation transparently. Removed the now-wrong physical-RAM
bound check.

**New failure** (not yet fixed): the HLE now decodes the trap, but
one call comes through with `opcode=6 count=65739` — an obviously
garbage block count. Suggests either the fake_parms struct layout
differs post-Pascal-OS, or we're re-entering the trap handler when
we shouldn't. Next session to diagnose — may need to gate the
LOADER HLE on the CPU still being in boot-ROM code range.

### Fix (P17): `goto` / numeric-label support (was silently dropped)

Parser created `AST_GOTO` nodes but no `AST_LABEL_DECL`; label prefixes
like `9:` were consumed and the following statement parsed as if the
label never existed. Codegen had no handler for either. Consequence:
`goto N` emitted **zero bytes** and control fell through straight into
the labeled statement. In EXCEP_SETUP this meant `goto 9` (skip-past-
error-handler) was silently dropped — every crea_ecb chain fell through
into `system_error(e_excep_setup)` unconditionally.

Fix:
- **Parser**: on `<int>:` prefix, emit `AST_LABEL_DECL(int_val=N)` with
  the following statement as child (not just drop the label).
- **Codegen**: per-procedure label map + pending-GOTO patch list reset
  at each procedure entry. GOTOs emit `BRA.W` (6000 + 16-bit disp).
  Backward targets emit disp inline; forward targets queue for patching
  when `AST_LABEL_DECL` is reached. BRA.W's ±32KB range is ample for
  intra-procedure jumps in Lisa OS.

Result: boot now passes EXCEP_SETUP cleanly, reaches GETSPACE #25 at
a brand-new call site (`$0A2194` in a later procedure). New failure is
further downstream — wild PC jumps to non-canonical addresses
(`$57C2EC48`, `$60AA0094`, `$6A6C0000`) suggesting a pointer corruption
or uninitialized vector; next session to diagnose.

### Fix (P16): string/record value param push/frame mismatch

`crea_ecb(errnum, sys_terminate, @term_def_hdl, ecb_ptr, b_sloc_ptr)` in
EXCEP_SETUP was passing `b_sloc_ptr = $104C00CE` (upper-word stale,
lower word = high byte of syslocal base) because string-value params
(`excep_name : t_ex_name = string[16]`) were being **pushed as MOVE.W
(2 bytes)** while the callee's frame layout **reserved the full 17+
bytes for the string**. The stack was misaligned by 15 bytes;
b_sloc_ptr's read at offset 38(A6) actually picked up adjacent garbage.

Root cause — two matching places in `pascal_codegen.c`:
1. `register_proc_sig`: fallback `param_size = 2` for any type whose
   `ptype->size != 4` — strings (17), records, and arrays all got 2.
2. `gen_proc_or_func` (both forward-decl reconstruction and direct
   param lists): frame offset incremented by `ptype->size`, so the
   callee expected strings to occupy their real byte size.

Fix: treat any non-primitive value param (size not in {1,2,4}) as
**pass-by-reference** (4-byte pointer). Caller LEAs the string into
D0 (already done), pushes MOVE.L; callee frame reserves 4 bytes.
Works with our existing LENGTH/COPY intrinsics which already expect
a string pointer in D0/A0.

Result: `b_area` to GETSPACE now correctly $00CE0000. EXCEP_SETUP's
4 crea_ecb calls all reach GETSPACE with valid args. SYSTEM_ERROR(10207)
still fires but from a different downstream cause (likely errnum
propagation or enqueue write — next session).

Symbol count: 8527 → 8711. Toolchain audit still 100% clean (317/317
Pascal, 103/103 ASM, link OK, 0 unresolved).

---

## Prior Status (2026-04-13)

### Prebuilt image (`prebuilt/los_compilation_base.image`)

Boots through MMU init (280+ TRAP #6), COPS handshake, ProFile driver,
Lisabug DB_INIT bypass (IPL-gated `$234` HLE), `$4FBC` NOP-skip.
After `G` at Lisabug prompt, reaches OS LOADER → **SYSTEM ERROR 10100**
(`Setup_IUInfo` can't open IU directory file). **Disk I/O / HLE issue.**

### Source compile (`Lisa_Source/` → disk image)

Toolchain: 317 Pascal + 103 ASM files → linked binary → disk image.
Boot reaches PASCALINIT → INITSYS → GETLDMAP → REG_TO_MAPPED →
POOL_INIT → MM_INIT. Runs 2000+ frames cleanly, no SYSTEM_ERROR,
then hangs in a tight loop at **$0A17BC-$0A17F6 with IPL=7**.

**ROOT CAUSE IDENTIFIED (2026-04-15):** codegen bug — `MM_INIT`'s
self-referential sentinel writes inside a nested `with head_sdb do`
block never emit store instructions. The hang is `INSERTSDB`
walking an uninitialized list.

**Symbol resolution** (now emitted as `build/lisa_linked.map`):
- $A17A0 = `INSERTSDB`  (the stuck loop)
- $A18E6 = `MAKE_FREE`  (caller; STARTUP:416)
- $A6F3A = `BLDPGLEN`   (earlier STARTUP:405-406 calls)
- $A69F2 = `MM_INIT`    (called from AVAIL_INIT:359)

**Evidence chain:**
1. STARTUP main body → AVAIL_INIT → MM_INIT → GETSPACE.
2. GETSPACE correctly writes `mmrb_addr = $CCA00A` to **A5-1268**.
3. INSERTSDB correctly reads `mmrb_addr` from **A5-1268** →
   c_mmrb = $CCA00A (valid sysglobal pointer). Our lisa.c diag
   label "A5-1264" is off by 4 bytes; the global really lives at
   -1268. (Low priority cleanup.)
4. `head_sdb` lives at c_mmrb + $16 = **$CCA020**.
5. **Memory at $CCA020..$CCA044 is ALL ZEROS** → sentinels never
   written. MM4.TEXT:237-248 has:
   ```pascal
   with head_sdb do begin
     memchain.fwd_link := @memchain.fwd_link;
     memchain.bkwd_link := @memchain.fwd_link;
     freechain.fwd_link := @freechain.fwd_link;
     freechain.bkwd_link := @freechain.fwd_link;
     ...
   ```
   Those four self-referential address-of-field stores are silently
   dropped by codegen. Result: INSERTSDB reads fwd_link=0, walks to
   address 0, reads SSP/garbage, loop never terminates.

**Related bugs already fixed in this family:** P4 (WITH field
offsets), P12 (WITH true/false). This is a new WITH-block case:
nested `with outer^.field do` + assignment of form
`innerfield.ptr := @innerfield.ptr` (address-of a field of the
WITH variable, written back to that same field). Likely the
codegen path for `@X` inside a WITH loses the WITH base and emits
store to a wrong (or zero) address — need to inspect
pascal_codegen.c's WITH+address-of path.

**Next:** fix the codegen. Read MM4.TEXT:237 compilation. Then
either (a) add a WITH-nested address-of case in pascal_codegen.c
mirroring P12's fallthrough, or (b) as a surgical fix, add a
codegen-emitted prologue to MM_INIT that pokes the sentinels via
inline pointer arithmetic if the source pattern is hard to match.
Option (a) is the real fix; (b) is a backstop.

---

Call chain confirmed via one-shot trace at $A17BC-$A17F6 in
src/m68k.c:

```
STARTUP $0008C0 → PMMAKE $0A6F3A (×2 calls)
STARTUP $000934↔$000928 (×30) — small loop
STARTUP $000950↔$00095E (×30) — 32-bit DIVIDE-by-subtraction:
                                 adjust_index := membase div maxmmusize
STARTUP $000962 → MODEMA-seg $0A18E6 — JSR to multiply helper
                                       (i-realmemmmu)*maxmmusize
MODEMA-seg $0A1996 → $0A17A0 — nested JSR (stuck here)
```

The `$A17A0` loop walks memory at A0=$B9F608 in 8-byte strides,
reading word and comparing against D2. iospacemmu=126 → $FC0000
(not $B9F608), so it's NOT an I/O poll. $B9F608 = seg 92 offset
$1F608 — looks like garbage/uninitialized, not a real data
structure. Suspect: the SETMMU inline code inside the multiply
helper is walking a corrupt MMU descriptor list.

Also suspicious: `sg_free_pool_addr = $A0000000` (non-canonical).

Inspiration projects: `_inspiration/lisaem-master/src/lisa/` has
full Z8530 SCC, VIA, COPS, floppy emulation — worth mining if we
later need SCC. Not needed for this bug.

Next: (a) add a trace at the START of AVAIL_INIT to confirm the
procedure identity; (b) log the actual parameters passed into
$A17A0 (the stuck sub) — specifically the pointer at -4(A6) of
$A18E6's frame, which becomes A0=$B9F608; (c) find where that
pointer originates, likely a miscompiled MMU-descriptor-base
computation.

**Key progress (2026-04-13):**
- P6: Boolean size (1→2 bytes) — Lisa Pascal stores booleans as words.
  Fixed PARMS frame misalignment (swappedin[1..48] was half-sized).
- P6: expr_size const ordering — check is_const BEFORE type.
  Fixes large constants like maxmmusize=131072 treated as 16-bit.
- P7: Interface declarations no longer export linker symbols.
  Fixed GETFREE/BLDPGLEN/MAKE_REGION all resolving to module base
  (offset 0) instead of their actual code offsets.
- P8: Procedure-local const declarations now processed in gen_proc_or_func.
  Fixes nospace=610 (imported) overriding local nospace=10701.
- P8: No EXT.L on function call results in binary ops.
  Function calls return full 32-bit values; EXT.L zeroed MMU_BASE's
  $CC0000 return value, corrupting POOL_INIT's mb_sgheap parameter.
- P9: Boolean NOT — bitwise NOT.W (1→$FFFE) replaced with logical
  TST/SEQ/ANDI (1→0, 0→1). ALL `if not func(...)` patterns were
  always-true due to this bug.
- P9: l_sgheap $8000→$7E00 (page-aligned, fits int2).
- P10: Function result variables — create local for return value,
  load into D0 before RTS.
- P11: Nested binary ops save D2 on stack — when the right operand
  is complex (binary op, func call, unary), use MOVE.L D0,-(SP)
  instead of D2 to prevent clobbering.
- P12: true/false/nil inside WITH blocks — the WITH fallthrough
  handler emitted MOVE.W #0 instead of checking for built-in
  constants. Every `x := true` inside a WITH block stored false.
- P13: WITH-field bases in nested WITH blocks. Three paths in
  pascal_codegen.c (AST_WITH type resolution, expr_size
  FIELD_ACCESS, gen_lvalue_addr FIELD_ACCESS) didn't resolve
  identifiers that are fields of an outer WITH record. MM_INIT's
  `with c_mmrb^ do ... with head_sdb do ... memchain.fwd_link :=
  @memchain.fwd_link` silently stored to address 0 with wrong
  width; head_sdb stayed all zeros; INSERTSDB spun. Fix: fall
  back to with_lookup_field in all three paths.
- P14: ptr.field^.subfield width/offset resolution. P_ENQUEUE
  (MM0.TEXT:207) `newlink.fwd_link^.bkwd_link := @newlink`
  compiled to MOVE.W (2-byte) stores instead of MOVE.L. Fix:
  extend expr_size + gen_lvalue_addr's AST_FIELD_ACCESS handling
  to recursively resolve the base type when children[0] is
  AST_DEREF whose pointer is itself an AST_FIELD_ACCESS.
- P15: Pascal variant records — parser previously kept only the
  largest variant arm, dropping fields from smaller arms (e.g.
  `freechain` in codesdb's `free` arm). Fix: emit ALL variant
  fields bracketed by `__variant_begin__` / `__variant_arm__` /
  `__variant_end__` sentinels; type builder resets offset at
  each arm boundary so arms overlap (proper Pascal semantics).

Boot passes MM_INIT + INSERTSDB + INIT_PROCESS. Latest blocker:
**SYSTEM_ERROR(10207)** `e_excep_setup` from INIT_EM /
EXCEP_SETUP — "no system data space in excep_setup".

Earlier blocker (resolved by source-file patch):
**SYSTEM_ERROR(10701)** from INIT_PROCESS's GETSPACE for `mrbt`
against an empty syslocal pool at \$CE0000. Root cause: Apple's
released Lisa source has a TYPO in source-SYSG1.TEXT.unix.txt —
POOL_INIT writes `sl_free_pool_adr` but syslocal record declares
`sl_free_pool_addr` (with double `d`). Old Apple Pascal likely
truncated identifiers; ours doesn't, so the store silently went
to address 0. **Fix: patched Lisa_Source/.../source-SYSG1.TEXT.unix.txt**
to use `sl_free_pool_addr` consistently (3 occurrences on lines
766, 768, 769). This is a source patch, not a codegen fix — the
Apple source file is user-supplied and per-user patchable, same
approach the `_inspiration/LisaSourceCompilation-main` project
uses for its own set of fix-ups.

Evidence chain:
- GETSPACE correctly gets amount=$1F0 (496 bytes, = 124 × 4-byte
  mrbtEnt) — sizing is fine.
- b_area=$CE0000 (syslocal pool base) reads back NULL.
- Earlier GETSPACE calls against **sysglobal** (b_area=$CCBF65)
  all succeeded, so sysglobal pool is OK.
- Conclusion: `POOL_INIT` either didn't initialize the syslocal
  pool header, or the mb_syslocal/l_syslocal args were
  miscompiled when passing 4-byte longints (possible P3-style
  width bug when the caller passes `MMU_BASE(syslocmmu)` as an
  absptr).

Infrastructure added this session:
- `build/lisa_linked.map` (sorted symbol map) — lookup is now O(1).
- One-shot m68k.c probes for MM_INIT / INSERTSDB / MAKE_FREE /
  P_ENQUEUE / GETSPACE / SYSTEM_ERROR entry points with caller
  chain dumps.

Next: trace POOL_INIT — is it being called for the syslocal
pool, and does it write to $CE0000? Likely the fix is another
arg-width bug on the POOL_INIT call from STARTUP main body.
- Memory layout: himem = b_dbscreen, bothimem = lomem.

**Key progress (2026-04-12):**
- P4: Fixed compile order, field offsets, store-width, global offset reuse
- P5: Fixed stale-upper-word bugs throughout codegen:
  - SIZEOF/integer literals now use MOVEQ/MOVE.L (never MOVE.W)
  - ORD4 sign-extends 16-bit args to 32-bit
  - Binary ops sign-extend operands when use_long=true
  - Store-width guards on WITH-field, var-param, complex-LHS paths
  - Result: GETSPACE works, MM_INIT completes, boot progresses

### Toolchain metrics
- Parser: **100%** (317/317), Assembler: **100%** (103/103)
- Linker: 8527 symbols, output ~2.2 MB
- Codegen: P1-P5 ptr/store, P6 bool+const, P7 iface, P8 local-const, P9 bool-NOT, P10 func-result, P11 D2-stack, P12 WITH-true

### Key HLE mechanisms
- `$234` fetch bypass: IPL=7→RTE (DB_INIT skip), IPL=0→execute (Lisabug)
- `$4FBC` NOP-skip: illegal opcode in Workshop init loop (13 hits)
- ProFile HLE: intercepts CALLDRIVER, reads from disk image
- INTSON/INTSOFF: manages IPL for compiled OS code

### Debug infrastructure
- `DBGSTATIC` macro + `g_emu_generation`: all debug statics reset on power cycle
- Bounded print budgets on all diagnostic output
- `VEC-FIRST` per-vector first-fire trace, periodic DIAG frame dumps
- 256-entry PC ring for crash analysis

### TODO
- **XPC process isolation**: run emulator in child process so power
  cycle = process kill + restart (eliminates static-state leaks)

See `.claude-handoffs/` for per-session handoffs. Run `make audit` for
toolchain metrics.


## Lisa_Source Reference

See `docs/LISA_SOURCE_MAP.md` for the complete catalog (~1,280 files).
See `docs/HARDWARE_SPECS.md` for hardware specifications derived from source.
See `docs/TOOLCHAIN.md` for the compilation pipeline needed to build from source.

Key facts:
- **Version**: Lisa OS 3.1 (Office System), circa 1983-1984
- **Languages**: Motorola 68000 assembly + Lisa Pascal (Apple's custom Pascal dialect)
- **~1,280 files** across OS kernel, 21 libraries, 13 applications, fonts, toolkit
- Contains 8 pre-compiled .OBJ files (68000 binaries) and 57 binary font files
- Build scripts in `LISA_OS/BUILD/` and `LISA_OS/OS exec files/` describe the full build process
- Linkmaps in `LISA_OS/Linkmaps 3.0/` show exact segment layout of every linked binary
- No pre-built ROM images or bootable disk images — everything must be compiled from source

## Hardware Specs (from source analysis)

| Component | Specification |
|-----------|---------------|
| CPU | Motorola 68000, 5 MHz |
| RAM | 1 MB (24-bit address bus) |
| Display | 720 x 364, monochrome bitmap |
| ROM | 16 KB at $FE0000 |
| I/O Base | $FC0000 |
| VIA1 | $FCD801 — Parallel port / ProFile hard disk |
| VIA2 | $FCDD81 — Keyboard / COPS (mouse, clock, power) |
| Video | Dual page, base at $7A000, contrast latch at $FCD01C |
| Interrupts | 7 levels (M68000 standard), VIA-based |
| Storage | Twiggy floppy, Sony 3.5" floppy, ProFile hard disk (5/10 MB) |
| Keyboard | 128 keys via COPS microcontroller, event queue |
| Mouse | Delta tracking via COPS, hardware cursor |

## Code Conventions

- **Swift**: Swift 6, `@Observable` (not ObservableObject), `@State` (not @StateObject), modern SwiftUI APIs (`.foregroundStyle`, `fileImporter`, etc.)
- **C**: C17, `-Wall -Wextra`, no external dependencies beyond SDL2 (standalone) or AppKit (Xcode)
- **Target**: Apple Silicon (arm64-apple-darwin), macOS 15+
- **Emulator/ files are SYMLINKS**: `lisaOS/lisaOS/Emulator/` contains symlinks to `src/`. No copying needed — edit `src/` and Xcode picks it up automatically.

## Git Conventions

- No Claude attribution in commit messages
- Lisa_Source/ is gitignored (Apple license prohibits redistribution)
- .claude/ directory is gitignored
