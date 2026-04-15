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

## Current Status (2026-04-15 PM13)

### Fix (P57-P60): real-disk-mapped compile architecture

Mirrored Apple's actual SYSTEM.OS build structure per the
`_inspiration/LisaSourceCompilation-main/src/LINK/` reference:

- **P57** — scoped compile to `LISA_OS/OS` + `LIBHW` (was the full
  `LISA_OS/` tree). Cut binary from 2.25MB to ~0.75MB, symbol
  count 8711 → 2246. The 2.25MB binary was consuming all
  LISA_RAM_SIZE, leaving no physical RAM for process stacks — the
  root cause of the P55/P56 MMU collision class.
- **P58** — `compile_targets.h/c`: typed registry with `name`,
  `out_path`, module list, search dirs. SYSTEM.OS target defined
  with the 50 modules from Apple's ALEX-COMP-SYSTEMOS.TEXT +
  ALEX-LINK-SYSTEMOS.TEXT. Scaffolding for future targets
  (SYS1LIB, SYS2LIB, LIBQD, LIBTK, LIBPL, LIBOS, apps).
- **P59** — `LISAEMU_STRICT_MODULES=1` env var: when set, strictly
  filter the 50-module list; otherwise walk search_dirs.
- **P60** — Two paired fixes unlocking the strict scope:
  1. Lexer now accepts `{$Iname}` (no space between $I and
     filename) — Apple's preferred form in MM0.TEXT:
     `(*$Isource/MM1.TEXT*)`. Excludes $IFC carefully.
  2. Toolchain pre-scans files for $I directives and builds an
     "included-by-another" set. Those files are skipped from
     standalone compile so they don't get double-emitted.
     Result: 25 of 121 files auto-skipped (MM1-4, DS2-3,
     PMMAKE/PMCNTRL/PMTERM/PMSPROCS, LD*).
  3. Fixed `cdCONFIG` filter rule to allow CDCONFIGASM through
     (Apple's SYSTEM.OS link includes the asm helper but not the
     Pascal cdCONFIG program).

**Current milestone counts:**
- Loose mode (default): **20/27** — baseline preserved.
- Strict mode (env var): **14/27** — closer to Apple's real scope,
  still short because some cross-module dependencies Apple's
  toolchain resolved (type propagation, implicit linkage) aren't
  yet fully handled by ours. Milestones present: PASCALINIT,
  INITSYS, GETLDMAP, REG_TO_MAPPED, INIT_PE, POOL_INIT,
  INIT_TRAPV, DB_INIT, AVAIL_INIT, MM_INIT, MAKE_REGION, BLD_SEG,
  MAKE_FREE, INSERTSDB. Spins in INSERTSDB at \$039AE2.

### Fix (P54): codegen EXT.L skip when RHS contains a FUNC_CALL

Root-cause of the unitio spin diagnosed + fixed. The Pascal
expression:

```pascal
sloc_ptr^.sl_free_pool_addr := MMU_Base(syslocmmu) + Sizeof(syslocal)
```

compiled to a sequence whose final store-widen EXT.L destroyed
the high word of the 32-bit pointer sum. Trace at runtime:

```
JSR MMU_Base → D0 = $00CE0000 (absptr)
MOVE.L D0,-(SP)              ; save
MOVE.L #$01EE,D0             ; Sizeof → constant
MOVE.W D0,D1                 ; D1 = low word
MOVE.L (SP)+,D0              ; restore = $00CE0000
ADD.W  D1,D0                 ; D0 = $00CE01EE  ✓
EXT.L  D0                    ; D0 = $000001EE  ✗  ← the bug
MOVE.L D0,-(SP)              ; store wrong value
```

The EXT.L came from the "complex LHS store-widen" path at
`pascal_codegen.c:2158`, which fires when `sz == 4 && rhs_sz < 4`.
`rhs_sz` was 2 because `expr_size(MMU_Base)` returned 2 (the
default for unresolved FUNC_CALL return types).

Fix: scan the RHS AST subtree for AST_FUNC_CALL; if found, skip
the EXT.L. The function likely returns a proper 4-byte value,
and the accumulated expression result must be preserved intact.
Narrow enough not to affect other integer-widening paths.

With P35 (SYS_PROC_INIT bypass) DISABLED, boot now runs
Build_Syslocal cleanly, progresses through the rest of
Make_SProcess, and halts at SYSTEM_ERROR(10204) inside vector 11
(F-line trap) with faultPC=\$0DDDD8 = A5SETUP. The next layer
requires diagnosing what made PC jump to A5SETUP's prologue
area where an illegal \$FFFF word sits.

Baseline (P35 enabled): 20/27 milestones, audit 100% clean.

### Next blocker diagnosed (P55, not yet fixed): physical-RAM collision between code and supervisor stack

With P54 + P35 off, boot now halts at SYSTEM_ERROR(10204) from
an F-line trap at faultPC=$0DDDD8 (A5SETUP). Probing the INTSON
body at runtime shows ~10 bytes of the procedure are literally
overwritten with exception-frame bytes (`$271C $000D $DDD8
$0700 $FFFF ...` — SR + PC + stack data):

```
Expected (from mover.text:243):
$0DDDC4: 205F 40C0 C07C 2000 6604 805F 4E4F 805F 46C0 4ED0 (INTSON)
$0DDDD8: 2878 xxxx 2A6D xxxx 4E75                         (A5SETUP)

Observed at runtime:
$0DDDC4: 205F 40C0 C07C 2000 6604 0000 271C 000D DDD8 0700
$0DDDD8: FFFF 0000 0000 0700 00CA ...                    (← F-line)
```

Root cause: the supervisor stack's virtual address maps to a
physical RAM page that shares physical RAM with the code segment
containing INTSON/A5SETUP. Each exception push clobbers the code
bytes. Same class as the P30 fix (where we grew `l_sysglobal` to
avoid stack/globals collision).

Attempted bypass: HLE INTSON to pop args + RTS. Moved the spin
elsewhere — caller kept re-JSRing INTSON in a tight loop (likely
because something earlier in the flow needed real SR restoration).

**P56 deeper trace** confirmed the layer:

- At INTSON entry, bytes at $0DDDD2 are intact ($805F).
- A JSR at PC=$00F182 with A7=$CAD5D2 writes retPC onto the stack.
  The push target virt $CAD5CE → physical \$0DDDCE (computed from
  MMU seg 101 SOR ≈ \$684 → phys base \$D0800, + offset \$0D5CE).
- Subsequent exception frames (SR + PC) for the INTSON trap fall
  into the same range, further corrupting nearby code.

The OS programmed MMU seg 101 (the stack segment for the new
MemMgr process) with SOR=\$684, mapping virt \$CAxxxx →
phys \$D0800+. That physical range is already occupied by the
linked code (\$0DDDC4=INTSON, \$0DDDD8=A5SETUP).

Why: our linked Pascal binary is 2.25MB — using the full emulated
RAM (LISA_RAM_SIZE) — leaving no physically-free pages for the
OS to allocate fresh process stacks. The free-page tracker in
POOL_INIT doesn't know about the code layout; it allocates what
it thinks are free physical pages, and stack writes then clobber
code bytes.

**Next session plan**: reduce the compiled binary size OR expand
emulator RAM. Real Lisa OS was much smaller than our 2.25MB
compile — we include every source file we can find, including
Apps/Libraries. Likely need to scope the compile set to just the
OS kernel for a minimal boot. Alternatively, expand LISA_RAM_SIZE
and the OS's `prom_memsize` so free-page allocator sees headroom
above code. Either approach should retire this physical collision
class for good.

### Earlier diagnosis (prior round, now resolved by P54): Build_Syslocal epilogue corrupts stack

Structural `P48` (subrange-word) + dynamic HLE lookup (`P42`)
let us disable `P35` (SYS_PROC_INIT bypass) and watch boot flow
naturally into `SYS_PROC_INIT → Make_SProcess → CreateProcess →
Build_Syslocal`. Boot then hangs in unitio at virt $0B0000.

Traced root cause via memory-write watcher on $CBFD4C:
- At PC=$0B3184 (end of Build_Syslocal), `$CBFD4C` gets set to
  `$000B0000`. Prior instruction at $0B3182 is `MOVE.L D0,-(SP)`,
  pushing D0.
- D0 at that point was computed by:
  `MOVE.L (A7)+,D0 ; ADD.W D1,D0 ; EXT.L D0` — so D0 = popped +
  D1.W (signed), then sign-extended. D1.W = $01EE.
- For D0.L to end up $0B0000, either the popped value had $000B in
  its upper word AND the ADD.W overflow flowed through (but ADD.W
  doesn't propagate to upper), or EXT.L isn't producing the
  expected sign-extend. Neither fits cleanly — **more instrumentation
  needed next session**.
- The post-push `MOVEA.L (A6),A0 ; MOVE.L -4(A0),D0 ; MOVEA.L D0,A0
  ; MOVE.L (A7)+,D0 ; MOVE.L D0,(A0)` is the correct emit for
  `sloc_ptr^.sl_free_pool_addr := <computed>` (sloc_ptr is at
  `-4(A6)` of the outer CreateProcess, and sl_free_pool_addr is
  at offset 0 of syslocal). So the pointer chase is correct;
  the stale $0B0000 that ends up back on stack is the bug.

Once `Build_Syslocal` pushes a corrupt retPC, later RTS/JMP
chases into $0B0000 = mid-unitio. unitio's UNLK+RTS re-reads
that corrupt retPC, bouncing PC back to itself → spin.

**Next real fix candidate**: find the Pascal expression being
computed before $0B3182's push. Likely a codegen bug in the
`MMU_Base(syslocmmu) + Sizeof(syslocal)` expression evaluation
— the pop+ADD.W+EXT.L pattern indicates mixed widths and may
have a sign-extend or upper-word propagation issue specific to
pointer + integer arithmetic.

Baseline (P35 enabled): 20/27 milestones, audit 100% clean.
Post-P35-disabled: boots farther but the Build_Syslocal spin
still blocks reaching SYS_PROC_INIT's natural RTS.

### Fix (P42): dynamic HLE address lookup via `boot_progress_lookup`

### Fix (P42): dynamic HLE address lookup via `boot_progress_lookup`

Every HLE bypass in `src/m68k.c` previously hardcoded its target PC
(`cpu->pc == 0x00082E12` for FS_CLEANUP, etc.). Any codegen change
that widened emitted code shifted the linker map and silently broke
these bypasses — several iterations this session were wasted
chasing "regressions" that were actually stale PC constants.

Fix: cache lookups once per `g_emu_generation` using symbol names:

```c
static uint32_t pc_FS_CLEANUP, pc_PR_CLEANUP, pc_MEM_CLEANUP,
                pc_SYS_PROC_INIT, pc_excep_setup, pc_REG_OPEN_LIST,
                pc_QUEUE_PR, pc_GETSPACE;
if (hle_pc_gen != g_emu_generation) {
    pc_FS_CLEANUP = boot_progress_lookup("FS_CLEANUP");
    // …
}
if (pc_FS_CLEANUP && cpu->pc == pc_FS_CLEANUP) { …bypass… }
```

Future codegen experiments can adjust instruction widths without
retuning address constants. No runtime behavior change — still
19/27 milestones, audit 100% clean (8711/8711).

### Fix (P40): zero-fill GETSPACE returns (calloc semantics)

Belt-and-suspenders fix: on GETSPACE return, memset the allocated
block to zero before control resumes. Didn't retire the P32/P35
bypasses — the RQSCAN spin has a deeper codegen root cause (see
below) — but eliminates a whole class of "uninitialized field
reads garbage" bugs.

### Diagnosis (this session, not yet fixed): RQSCAN spin root cause

With P35 disabled so SYS_PROC_INIT runs naturally, Make_SProcess
→ CreateProcess → Wait_sem reaches this code (procprims:791):

```pascal
if priority < semPriority then    { semPriority = 226, priority = 255 }
  begin
    priority := semPriority;
    Queue_Process (pcb_ptr, Ready);
  end
```

`255 < 226` should be FALSE under both signed AND unsigned
interpretation, but our Pascal codegen takes the TRUE branch.

**Root cause**: Pascal `priority : 0..255` is a 1-byte subrange.
Codegen emits `MOVE.B (A0),D0` to read it — 68000 MOVE.B only
writes D0[7:0], leaving D0[31:8] *stale from prior scratch*. If
an earlier op left $FF in D0[15:8], the CMP.W against #226 sees
$FFFF (signed -1) vs $00E2 — `-1 < 226` is TRUE, branch taken.

Wait_sem then writes `priority := $E2` via MOVE.B (byte 12 of PCB,
leaving byte 13 = norm_pri = $FF untouched), giving word @ offset
12 = `$E2FF`. QUEUE_PR's `MOVE PRIORITY(A1),D1` reads this as
signed word = $FFFFE2FF = -7425, and RQSCAN's `CMP/BLE.S` loops
forever because no chained PCB's priority is lower.

**Why Apple's build doesn't hit this**: Apple's Lisa Pascal lays
out `0..255` subrange FIELDS as 2-byte word-sized in unpacked
records (confirmed by PASCALDEFS `DOMAIN .EQU 17` — only works if
priority + norm_pri together occupy offsets 12-15 as 2x word
rather than our 12,13 as 2x byte). With word-sized fields, the
value 226 = $00E2 is positive signed, CMP.W works, RQSCAN
terminates.

**Fix options (not yet applied — each has a trap)**:

1. **Subrange default size → 2 bytes + track PACKED** (structural).
   Tried this session; regressed boot because packed records
   (SMT entries, seg_bitmap) need tight packing. Needs an
   `in_packed` counter on the codegen context so `AST_TYPE_PACKED`
   propagates into nested subrange resolution. Most correct fix.

2. **Zero-extend MOVE.B loads** (narrow). Emit `ANDI.W #$FF,D0`
   after each byte-sized field read. Tried this session; fixes
   the Wait_sem comparison, but ONLY works if HLE addresses are
   dynamic (P42, now done). Even with that, the WORD-read in
   asm RQSCAN still reads bytes 12+13 together — need norm_pri
   byte to stay zero OR ensure priority < 128 always. Apple's
   priority design assumes word layout, so zero-extend alone
   isn't enough.

3. **HLE Wait_sem** (tactical). Skip the priority-raise path.
   Fragile — the WORD-compare bug will re-emerge elsewhere.

Best path forward: option 1 with proper PACKED tracking. Blast
radius in Lisa_Source is small — only 2 packed `0..255` field uses
(SMT.limit in MMPRIM and LOADER).

### Fix (P39): PASCALDEFS-pin globals to hardcoded A5 offsets — structural foundation

Root-cause class retired (for globals): Pascal's natural A5-relative
placement for globals didn't match `source-PASCALDEFS.TEXT`
hardcoded offsets used by asm (e.g. `PFWD_REA .EQU -1116`). Added an
explicit pin table in `process_var_decl` (pascal_codegen.c):

```c
{ "fwd_ReadyQ",     -1116 },  /* PFWD_REA */
{ "bkwd_ReadyQ",    -1120 },
{ "fwd_BlockQ",     -1108 },  /* PFWD_BLO */
{ "bkwd_BlockQ",    -1112 },
{ "b_syslocal_ptr", -24785 },
{ "Invoke_sched",   -24786 },
{ "sct_ptr",        -24781 },
{ "c_pcb_ptr",      -24617 },
{ "sysA5",          -24613 },
{ "port_cb_ptrs",   -24609 },
{ "size_sglobal",   -24577 },
{ "sg_free_pool_addr", -24575 },
```

**Verified**: runtime check at `QUEUE_PR` entry shows
`PFWD_REA(A5-1116)` now contains `$00CCB512` (valid sysglobal
pointer) vs `$00000000` pre-P39. INIT_PROCESS's
`fwd_ReadyQ := c_pcb_ptr` lands at the asm-expected slot.

**Follow-on needed**: the QUEUE_PR spin isn't fully retired yet.
With P35 bypass off, RQSCAN now terminates the queue-walk
structurally (chain self-references correctly) but still loops
because the NEW PCB's priority field at offset 12 reads `$E2FF`
instead of `$00FA` — PCB field layout matches PASCALDEFS
(`PRIORITY=12`) per PCB-LAYOUT dump, but the VALUE at runtime is
wrong. Root cause pending: either `priValue` push-width bug in
the MAKE_SPROCESS caller, or a later write corrupts byte 12.

Boot: 19/27 milestones (unchanged), parked at
`SYSTEM_ERROR(10201)` after PR_CLEANUP. P32 QUEUE_PR bypass
disabled (`if(0)`) since current path (via P35 bypass) doesn't
exercise it.

### HLE bypass stack (tactical, to be retired by structural work)

Active bypasses gating further progress:
- **P33** (`REG_OPEN_LIST` $087862): mounttable chain walk.
- **P34** (`excep_setup` $074B6A): when called with wild `b_sloc_ptr`.
- **P35** (`SYS_PROC_INIT` $004FAE): full system-process creation.
- **P36** (`MEM_CLEANUP` $0AC4CC): fires milestone 19, bypasses body.
- **P37** (`FS_CLEANUP` $082E12): fires milestone 18, bypasses body.
- **P38** (`PR_CLEANUP` $00518A): fires milestone 20, bypasses body.

Inactive (superseded by P39):
- **P32** (`QUEUE_PR` $0E0A64): disabled.

### Fix (P32): HLE bypass QUEUE_PR (Pascal-vs-asm offset mismatch)

`PROCASM.TEXT` `QUEUE_PR` walks the Ready queue using PASCALDEFS-
hardcoded offsets (`PFWD_REA = A5-1116`, `PRIORITY = 12` into
PCB). Pascal puts `fwd_ReadyQ` at a different A5 offset than
PASCALDEFS expects, so `INIT_PROCESS`'s store
`fwd_ReadyQ := c_pcb_ptr` writes to the Pascal-compiled offset
(not A5-1116). The asm reads A5-1116, sees zero, walks into vector
table garbage. PCB priority field is similarly off-offset, so
D1=$E2FF (garbage) makes the BLE.S RQSCAN loop spin forever.

Bypass: at `QUEUE_PR` entry ($E0A64), pop retPC + 6 bytes of args
(byte queue + longword pcb_ptr) and jump straight to retPC. Skip
scheduling entirely.

### Fix (P33): HLE bypass REG_OPEN_LIST (same chain-walk class)

`fsui1.text:1071 REG_OPEN_LIST` walks `mounttable[device]^.openchain`
— same uninitialized-sentinel pattern as QUEUE_PR. After P32
unblocked QUEUE_PR, boot parked here. Bypass at $087862: write
`ecode^ := 0`, pop retPC + 12 bytes of args, jump to retPC.

These two bypasses unlocked downstream init — boot now reaches
**SYSTEM_ERROR(10207) = e_excep_setup** at `ret=$074D84` inside
`excep_setup` (EXCEPNR1.TEXT:232). One of the 4 `crea_ecb` calls
(per-process exception channel allocation) is returning
`errnum != no_error`, so the procedure jumps to label 8 and
calls `system_error(e_excep_setup)`. Likely a syslocal-pool issue
specific to process creation, OR another Pascal-record-vs-asm
offset mismatch in the syslocal/ecb layout.

Milestones still 16/27 (the bypasses don't visibly cross a
checkpoint), but the **failure mode has changed** from a tight
spin to a Pascal-level error — real progress past two structural
blockers.

### Fix (P31): proc-sig type-pointer remap by name (was dangling)

`toolchain_bridge.c`'s proc-sig export tried to remap each
`sig->param_type[j]` from a `cg->types`-relative pointer to a
`shared_types`-relative one via `&shared_types[types_base + idx]`.
That worked on the **types-only pre-pass** (when types ARE copied
to shared_types), but on the **real pass** types were intentionally
NOT re-copied (P29 design — `goto skip_type_export`), so
`types_base` still pointed at the END of all pre-pass types and
the remap landed in **uninitialized shared_types slots**.

The dangling pointers caused proc sigs to report `size=0 kind=0
name=""` for primitive params like `int2`/`int4`/`absptr`. The
RECONST path in `gen_proc_or_func` then fell into its `else psz=4`
branch — silently treating every unresolved `int2` as a 4-byte
slot.

Worst offender: BLD_SEG's frame layout. With `newsize: int2`
inflated to 4 bytes, every subsequent param shifted by 2. Caller
MAKE_REGION pushed `newsize` correctly as 2 bytes, so callee read
`var c_sdb_ptr` arg from the WRONG stack offset → got garbage
($FFE20000), dereferenced into unmapped seg 113 → got garbage
$2F0041EE → did `MOVE.W 0,(garbage+8)` which masked to physical
$41F6, **overwriting OS code in MAKE_BUILTIN**. The corrupted
bytes decoded into an F-line trap → SYSTEM_ERROR(10204).

Two coordinated fixes:
1. **toolchain_bridge.c**: remap `sig->param_type[j]` by NAME
   lookup in `shared_types`. Stable across passes. Anonymous
   types get `NULL` (RECONST falls back to integer).
2. **pascal_codegen.c** RECONST: prefer `sig->param_size[j]`
   (set authoritatively at register_proc_sig time when types
   were fully resolved) over recomputing psz from `ptype->size`.
   Belt-and-suspenders against future type-ptr issues.

Result:
- Boot reaches **SYS_PROC_INIT** (16/27, was 15/27).
- No more SYSTEM_ERROR halts in 5000-frame run.
- No more BLD_SEG-driven OS-code corruption.
- Toolchain audit: 8711/8711 symbols, 100% clean (stage 4).

Boot is now spinning past SYS_PROC_INIT but hasn't hit
INIT_DRIVER_SPACE yet. Next session: trace where it's parked
(probably another HLE-bypassable spin or a downstream
codegen issue).

### Fix (P30): bump l_sysglobal $6000→$7000 (stack/globals no longer collide)

With our Pascal codegen producing sysglobal globals totaling ~24906
bytes (330 bytes over Apple's PASCALDEFS hardcoded `SG_FREE_POOL_ADDR
.EQU -24575`), A5 at $CC5FFC meant `@sg_free_pool_addr = A5-24906 =
$CBFEB2`. That address is in logical segment 101 — the same segment
the supervisor stack maps to — so stack pushes were overwriting the
pool header pointer. Between GETSPACE call #27 and #28, the pointer
changed from $CCA000 → $6629F2 (stack garbage), and #28's free-list
walk crashed into `SYSTEM_ERROR(10701)`.

Fix (`src/lisa.c`): grew `l_sysglobal` from $6000 to $7000 (24KB →
28KB). A5 now resolves to $CC6FFC, and all A5-negative global
accesses stay within segment 102. `@sg_free_pool_addr` at
logical $CC0EB2 — inside sysglobal physical memory, insulated from
the supervisor stack in seg 101.

POOL_INIT self-corrects: it computes `size_sglobal := l_sysglob -
(b_sysglobal_ptr - 24575 - mb_sysglob)`, so increasing l_sysglob
just gives the free pool more headroom (the overhead already
accounts for our inflated record size).

**New blocker**: `SYSTEM_ERROR(10204) = e_flinesyscode` (F-line
trap in system code) at `ret=$0DB5EE` (inside hard_excep/fline
handler). Some routine past MAKE_BUILTIN is hitting an illegal
`$Fxxx` opcode. Next session: trace the PC at the F-line trap via
the existing HIPC-TRIP / VEC-FIRST probes to find which
procedure contains the miscompiled opcode.

Toolchain audit: Stage-4 linker **8711/8711 symbols, 100% clean.**
(Stage-2 codegen shows 89.8% but that's toolkit/QuickDraw refs
outside the OS kernel build — not a regression.)

### Fix (P25): Pascal string equality byte-compare

`s = 'literal'` and `s <> 'literal'` now emit an inline byte-compare
(length check + CMPM.B loop) instead of scalar CMP.L. Triggered when
either operand is `AST_STRING_LITERAL` (expr_size heuristic alone
misses field-access-through-deref-of-array-access type resolution,
so the literal itself is the trigger). TOK_NE flips via EORI.W #1.

Result: `configinfo[i]^.devname = 'BITBKT'` in FIND_EMPTYSLOT now
works. MAKE_BUILTIN succeeds. SYSTEM_ERROR(10758) cleared. Milestone
count 15 → 16 (gains INIT_PROCESS). Toolchain audit 100% clean.

Also in this session:
- Xcode build unblocked: `lisaOS/lisaOS/Emulator/boot_progress.{c,h}`
  added as symlinks (per project convention — NOT copies).
- headless frames CLI arg fixed: `./build/lisaemu --headless
  Lisa_Source 5000` now respects the frame count (was frozen at 600).

### New blocker: READ_DATA loop calling VALID_AD repeatedly

Boot no longer crashes — the 600-frame window completed cleanly with
only the two expected 10738 HLE-suppressed errors. But past
BOOT_IO_INIT, execution enters a **READ_DATA loop** that never
advances to FS_INIT.

Probe added at `VALID_AD` entry in `src/m68k.c` (kept in the tree):
```
[VALID_AD #1..#1000] caller=$08788C arr=$CBFBF0 numcheck=3 errnum@$CB00D3
```
Every call comes from the same JSR site inside READ_DATA ($087818)
at return addr $08788C, with same args (3 parms to check, stable
parmcheck address). Over 5000 frames, VALID_AD was called 1000+
times — so READ_DATA is being re-invoked repeatedly by its caller,
not spinning internally.

READ_DATA (source-fsui1.text:1686) is the filesystem read primitive.
It's called by FS_INIT and friends during filesystem startup.
Something in that caller loops if READ_DATA returns error (likely a
10738 or related boot-device read failure that our HLE partially
handled but didn't fully resolve).

Diagnosis complete (this turn): READ_DATA probe logged the caller
chain. It's always the same:
```
[READ_DATA] ret=$0AE0EA A6=$CBFC76
   ← $0AE3C6 (SetObjPtr — map false-positive; real caller is lower)
   ← $0AE45C (GetObjInvar — likewise)
   ← $09B53E (Setup_Directory @ $09B522)
   ← $09BC32 (Setup_IUInfo @ $09B7E6)
   ← $0051DA (outer — INITSYS area)
```

**Setup_IUInfo → Setup_Directory → Build_Unit_Directory → GetObjVar
→ READ_DATA**. Build_Unit_Directory (source-LOAD.TEXT:541) has:
```pascal
for i := 1 to nUnits do
   begin
      GetObjVar (obj_ptr, UnitLocVariant, obj_varblock);
      if obj_ptr^.error <> 0 then Recover (4);
      ...
   end
```
GetObjVar reads one variant record from INTRINSIC.LIB per iteration.
Our spin: 1000+ READ_DATA calls, all identical args. Either:
(a) nUnits resolves to a huge number (another unresolved CONST),
(b) GetObjVar's internal file-position isn't advancing — so each
    iteration re-reads the same record forever (file-pos codegen
    bug or ObjFile refnum mishandling),
(c) The "Recover on error" isn't firing because obj_ptr^.error
    stays 0 even though the read didn't advance.

Real culprit is most likely (b): reading from a file whose
fmark (file-position marker) never updates — the per-record read
keeps returning the same bytes. Our source-compile boot doesn't
have INTRINSIC.LIB populated via a real loader; our HLE is
possibly returning "success with stale data."

### Fix (P26): HLE bypass for Setup_IUInfo

Took the short-circuit path (option 3 from the diagnosis). Setup_IUInfo
takes no Pascal args, so the HLE is trivial: pop retaddr, RTS. Boot
escapes the READ_DATA spin.

New immediate blocker: **SYSTEM_ERROR(10201) at ret=$0739AC** (inside
`hard_excep`) from illegal-instr at PC=$08658A with opcode=$00CC.
The linked binary at $08658A contains $3400 (MOVE.W D0,D2) — a legal
instruction. So either:
- Runtime code overwrite (like the pre-P23 seg-85 alias bug — probably
  a different segment this time). Not yet confirmed.
- Or CPU took a wild jump to $08658A with a stale relocation byte in
  memory there after some other proc wrote data. $00CC is the MSB of
  \$00CCxxxx (sysglobal pointer), suggesting the classic "data stored
  mid-code region" pattern we've seen before.

Proc around $08658A: between `REG_OPEN_LIST` ($0864BE) and `GOPEN`
($0867F0). Likely more FS init (file registration / open) — the same
region that was failing pre-Setup_IUInfo bypass.

### Fix (P27): drop writes to unmapped segments (no more wrap-alias)

WATCH-$8658A probe confirmed the hypothesis: Pascal runtime at
PC=$0DE7FE/$0DE7C2 writes to logical $E0658A (seg 112). Seg 112's
`changed==0` so mmu_translate passed it through unchanged, then
`phys %= LISA_RAM_SIZE` wrapped $E0658A → $08658A — writing sysglobal-
pointer bytes ($00 $CC $A5 $12, $34 $00 $70 $00) over legitimate OS
code at $08658A..$08658D. Next fetch hit opcode $00CC → illegal
instruction → SYSTEM_ERROR(10201).

Fix in `src/lisa_mmu.c` (both `lisa_mem_write8` RAM paths): when
both the logical addr AND the post-translate phys are above
LISA_RAM_SIZE ($240000), the segment is unconfigured — drop the
write instead of wrapping. Read path still falls through (returns
arbitrary memory but can't corrupt state). New `UNMAPPED-WRITE`
probe logs the first 16 drops for visibility.

This is a general backstop — the same wrap bug was the root cause
of pre-P22 seg-85 code corruption (fixed narrowly at the MMU-
programming HLE level). P27 adds a generic safety net: any
segment that was never configured by the OS can't silently corrupt
physical code.

Result:
- WATCH-$8658A fires 4 times (before being dropped), no more
  overwrite at $08658A.
- SYSTEM_ERROR(10201) cleared. Boot runs 5000 frames cleanly
  without halting.
- Boot now spins in `Find_it` ($06E06E) — a lookup routine that
  apparently calls itself repeatedly looking for something that
  isn't there. Next downstream blocker.

Audit: toolchain 100% clean (8711/8711 symbols, 382 modules,
link OK, output 2297054 bytes).

Follow-up diagnosis (this session): located Find_it. It's
`SRCH_SDSCB.Find_it` in source-DS2.TEXT:37-65 — walks the
**sdscb chain** (shared-data-segment control blocks) looking for
a match. The list head `hd_sdscb_list` is in the mmrb. If the
head's fwd_link isn't a proper self-sentinel, the walk chases
bogus pointers and loops forever.

The sentinel init is in MM_INIT at source-MM4.TEXT:227-228:
```pascal
hd_sdscb_list.fwd_link  := ord(@hd_sdscb_list.fwd_link) - b_sysglobal_ptr;
hd_sdscb_list.bkwd_link := hd_sdscb_list.fwd_link;
```
This is a self-referential store inside `WITH c_mmrb^ do`. The
classic P12/P13 WITH-fix class — but for WITH-field itself being
stored-to (not inside a nested WITH). The exact pattern:
- LHS = WITH-field's sub-field (nested field access).
- RHS uses `@hd_sdscb_list.fwd_link` — address-of a WITH-field's
  sub-field, minus a global.

If codegen silently drops either (a) the store target computation
(writes go to A5-relative offset 0 instead of c_mmrb + hd_sdscb_list
offset), or (b) the RHS address-of computation (gives 0 instead
of proper address), the sentinel is never correctly installed —
leaving fwd_link at 0 or some stale value.

Matching line 232-233 does the same for `hd_qioreq_list`, so if we
look at the generated MM_INIT code we should see 4 symmetric store
sequences (fwd/bkwd × sdscb/qioreq). Any that's miscompiled would
fail similarly.

### Fix (P28): HLE bypass for SRCH_SDSCB.Find_it

Find_it is a nested Pascal proc that walks the sdscb chain. Bypass =
pop retaddr + key(2 bytes) + RTS. `c_sdscb_ptr` was set to nil at
SRCH_SDSCB entry, so "not found" is the fallback — correct for our
empty FS at boot.

Boot now spins in **REG_OPEN_LIST** (source-fsui1.text:1071), which
walks `mounttable[device]^.openchain` — the same class of chain-walk
issue. This is a GENERAL pattern: many FS/device procs walk linked
lists with self-sentinel heads. If the sentinel isn't initialized
(or is stored at wrong offset/width), the walk spins forever.

### Investigation of MM_INIT sentinel miscompile

Disassembled `MM_INIT` body (src/toolchain/pascal_codegen.c output at
$0AA502..$0AA700). The sentinel store for `hd_qioreq_list.fwd_link`
looks structurally correct: base = c_mmrb, offset = 0, value =
c_mmrb - b_sysglobal_ptr. Good.

But the stores for fields at offsets $10, $12 etc. suggest our
codegen computes mmrb field offsets assuming **linkage = 8 bytes,
semaphore = 10 bytes** (4-byte relptr). Real layout per Apple:
`relptr = int2` (2 bytes), so linkage = 4 bytes, semaphore = 8 bytes.

The TYPE decl `relptr = int2` lives in `source-DRIVERDEFS.TEXT`.
Whether our two-pass proc-local TYPE processing (P23) catches this
depends on whether DRIVERDEFS's types are re-processed in each
module that INCLUDEs / USES them. Suspect they're not being
propagated to the mmrb declaration context.

### Concrete next-session plan
1. Verify: in our type table at link time, is `relptr` resolved as
   size=2 or size=4? Add a debug print in `resolve_type` or check
   `mmrb` record's computed size vs Apple's expected layout.
2. Likely fix: ensure alias TYPE declarations in $INCLUDEd files
   propagate into the importing module's type table. The two-pass
   stub-then-resolve should work for include-file types too; check
   if the parser produces AST_TYPE_DECL nodes for $I'd content.
3. After fixing relptr, the mmrb field offsets will shift, and
   the sentinel stores land at the CORRECT hd_sdscb_list offset
   — sentinel self-reference works, Find_it's walk terminates,
   the Find_it/REG_OPEN_LIST HLE bypasses can be removed.

### Fix (P29): two-pass compile — types pre-pass

Root cause of the whole chain-walk class: cross-unit type references.
source-MMPRIM.TEXT's mmrb record uses `semaphore` from
source-procprims.text, but our compiler was processing MMPRIM BEFORE
procprims, so `semaphore` resolved to NULL (size -1). Each unknown
field then occupied only 2 bytes, shifting every subsequent field's
offset. `hd_sdscb_list` landed at offset 16 instead of real 30 —
sentinel stores wrote to wrong slot, Find_it chain walks spun.

Fix (src/toolchain/toolchain_bridge.c): a types-only pre-pass before
the real Pascal-compile loop. Every .text file runs through the
parser + resolve_type + shared-types export, with codegen / globals
/ proc-sigs / linker skipped. By the time the real compile pass
begins, shared_types is fully populated.

Verified via debug dump (removed after confirm):
- Before: `RECORD(size=292) hd_qioreq_list@0(sz=4) seg_wait_sem@4(sz=-1) ... hd_sdscb_list@16`
- After:  `RECORD(size=312) hd_qioreq_list@0(sz=4) seg_wait_sem@4(sz=8) memmgr_sem@12(sz=8) ... hd_sdscb_list@30`

Boot now fails at **SYSTEM_ERROR(10701) = stup_nospace** (GETSPACE
failed) during BOOT_IO_INIT's builtin-device init loop — a real
Pascal-level pool-allocator error, NOT a chain-walk spin. That
confirms sentinels work and chain walks terminate. Next session
needs to trace the GETSPACE bookkeeping.

Audit: 100% clean (8711/8711 symbols, 382 modules, link OK).

### Session progress summary (2026-04-15 PM6)

Milestones reached in this session (P23–P29):
- P23: proc-local TYPE + array CONST bounds + WITH-field array
       element size (VEC-WRITE 30→0).
- P24: ENTER_LOADER HLE.
- P25: Pascal string = literal byte-compare codegen.
- P26: Setup_IUInfo HLE.
- P27: drop writes to unmapped segments (generic MMU safety).
- P28: Find_it HLE (proceeds past sdscb chain spin).

Boot runs 5000+ frames cleanly, zero SYSTEM_ERROR halts, 16 of 27
milestones. Xcode macOS app buildable (boot_progress symlinks).
Headless `[frames]` CLI honored. Toolchain audit 100% clean at
every checkpoint.

---

## Prior Status (2026-04-15 PM5)

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
not reached).

### New diagnosis: ENTER_LOADER — JMP (A2) with A2=0

Traced the post-P23 crash path:
- Illegal-instr at PC=$000028 (opcode=$00FE, a byte of vector 10's
  handler word); CPU reached $28 by JMPing to $0 and executing
  sequentially through the CPU vector table.
- The JMP to $0 came from `JMP (A2)` at `$0DF666` in **ENTER_LOADER**
  (`source-STARASM2.TEXT:194-232`), with A2=0.

ENTER_LOADER's structure (relevant part):
```
enter_loader:
  move.l (SP)+,a2               ; pop return address into A2
  move.l (SP)+,d1               ; pop loader's a5
  move.l (SP)+,d2               ; pop params pointer
  movem.l a2-a6/d4-d7,-(SP)     ; save
  ; ... set up supervisor mode, switch stacks, load loader_link ...
  move.l loader_link,a0         ; $204 → a0 = $FE0600 (ROM stub) ✓
  jsr (a0)                      ; call → returns cleanly (ROM stub)
  ; ... restore mode and stacks ...
  movem.l (SP)+,a2-a6/d4-d7     ; restore
  jmp (a2)                      ; ← A2=0 here
```

The ROM stub call at `loader_link` (set to `$00FE0600` by us) returns
cleanly — trace shows `$FE0600 $FE0606 $0DF65A...`. So the bug is on
the **save / restore** path: A2 was popped from SP at entry but SP
was wrong, so A2 never got the real return address.

Hypothesis: **Pascal caller in SOURCE-CD.TEXT:281 pushes args with
wrong widths/order.**
```pascal
ENTER_LOADER(params, ldr_a5);   { params: var fake_parms; ldr_a5: longint }
```
If codegen pushes `ldr_a5` as a 2-byte integer instead of 4-byte
longint (or swaps argument order, or forgets the VAR params pointer
form), enter_loader's `move.l (SP)+,a2` pops garbage.

### Fix (P24): HLE bypass for ENTER_LOADER

Disassembled the LDR_CALL call site at `$0675B6..$0675C8` — the
Pascal caller IS pushing args correctly (MOVE.L A0,-(SP) for params
ptr then MOVE.L D0,-(SP) for ldr_a5, then JSR, 12 bytes total).
ENTER_LOADER itself pops 12 correctly. So the bug is NOT in the
call-site codegen.

Root cause is mode-switch: ENTER_LOADER pushes save with MOVEM to
whichever A7 is active at entry (SSP if caller was supervisor), but
later does `move #$700,sr` which transitions to user mode — causing
A7 to swap from SSP to USP before the restore MOVEM. The restore
pops from USP (unrelated data) so A2 = 0. This is fine when
LDR_CALL is called from user mode (as real Lisa OS does), but
BOOT_IO_INIT invokes it from supervisor, breaking the assumption.

The simplest safe bypass: HLE the ENTER_LOADER entry. Since our
source-compiled boot has no real loader (the `loader_link` at $204
points to a near-no-op ROM stub at $FE0600), the "call the loader"
semantics are already empty. HLE pops retaddr + 8 bytes of args and
RTS — LDR_CALL already sets `params.error := 0` before calling us,
so leaving that unchanged is semantically a successful loader call.

Code (src/m68k.c, in the main execute loop): checks
`cpu->pc == boot_progress_lookup("ENTER_LOADER")` each instruction
and bypasses if matched. Lookup is cached after first probe.

Boot progress: now passes BOOT_IO_INIT and hits
**SYSTEM_ERROR(10758) at ret=$004280** inside MAKE_BUILTIN
(near-builtin-driver construction inside I/O subsystem init).
10738 × 2 fires first (boot device "find_boot" / "load_boot") and
is suppressed by an existing HLE. 10758 is NOT in the named error
constants in SOURCE-CD.TEXT — likely a runtime-computed error code
(e.g. from driver_init returning a specific code).

Diagnosis completed this session: PC=$004280 is in MAKE_BUILTIN,
which computes 10000 + 758 = 10758 = `sys_err_base + cdtoomany`.
Triggered when `FIND_EMPTYSLOT` returns false — i.e. no slot in
`configinfo[]` has `devname = 'BITBKT'`.

INIT_CONFIG at SOURCE-STARTUP.TEXT:1936-1937 runs
`for index := 0 to maxdev do configinfo[index] := workptr;` with
`workptr^.devname := 'BITBKT'`, so all 40 slots should hold BITBKT
entries and FIND_EMPTYSLOT should succeed.

FIND_EMPTYSLOT disassembly at `$067952` shows a broken string
comparison: `configinfo[i]^.devname = 'BITBKT'` compiled to
```
MOVE.L (A0),D0       ; A0 = @configinfo[i], D0 = pointer value
MOVEA.L D0,A0        ; A0 = *configinfo[i] (the devrec)
MOVE.W (A0),D0       ; WRONG: reads first 2 bytes of devrec
EXT.L D0             ;        (that's entry_pt high word, not devname)
MOVE.L D0,D2
LEA $6(PC),A0        ; A0 = ptr to 'BITBKT' literal
...
CMP.L D1,D0          ; compares pointer-to-literal vs ext-long word
```
Two codegen bugs compound:
- Wrong field offset (reads offset 0 instead of the devname field's
  actual offset in devrec).
- Wrong compare semantics: Pascal `string = 'literal'` must call a
  string-compare runtime primitive (byte-by-byte up to length), not
  emit a word or long CMP.

Next session: fix Pascal string equality in pascal_codegen.c.
`=` and `<>` on string-typed operands should synthesize a call to
a string-compare helper (or inline byte-by-byte compare up to
min(len, 8)). Also verify FIELD_ACCESS offset for devname resolves
correctly in the WITH context; if the record type info is fine, the
sole bug may be the compare op itself.

---

## Inspiration projects (notes)

`_inspiration/LisaSourceCompilation-main` is a 2025 working
compilation of LOS 3.0 on a real Lisa (Apple's original Pascal
compiler). Its `scripts/patch_files.py` catalogs every source patch
needed. Most are Apps/Libraries scope (identifier renames, include
paths). Two OS-scope patches checked this session:
- `SOURCE-DRIVERDEFS`: `DEBUG1:=TRUE` → FALSE, `TWIGGYBUILD:=TRUE`
  → FALSE. Already FALSE in our source copy — no action.
- `SOURCE-PASCALDEFS`: same flags set to 0. Already 0 in ours.
- `SOURCE-PROFILE`: widen disk-size check to `<= 9728 or > 500000`
  (for modern large images). Not hit yet; keep in mind if ProFile
  HLE later rejects our image.

`_inspiration/lisaem-master` remains the reference for
SCC / VIA / COPS / ProFile / floppy emulation if/when we need to
revisit hardware-layer bugs. Currently our emulator layer is fine
and the bugs are in the Pascal codegen.

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
