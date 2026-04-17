# HLE (High-Level Emulation) Inventory — LisaEmu

## Executive Summary

**Total HLE patches identified: 28 active + 9 disabled (commented-out) = 37 total**

### By Category

| Category | Active | Disabled | Subtotal |
|----------|--------|----------|----------|
| kernel-proc-stub | 8 | 8 | 16 |
| error-suppress | 1 | 1 | 2 |
| driver-intercept | 7 | 0 | 7 |
| mmu-patch | 2 | 0 | 2 |
| codegen-workaround | 5 | 0 | 5 |
| trap-hle | 1 | 0 | 1 |
| memory-prime | 1 | 0 | 1 |
| probe | 3 | 0 | 3 |
| **TOTAL** | **28** | **9** | **37** |

### Front-Line Demolition Targets

These HLEs are currently active and blocking transition to real subsystems:

1. **HLE TRAP #6 (PROG_MMU)** — src/m68k.c:2265. All MMU programming goes through this. Must implement real TRAP #6 handler or MMU programming in the bootrom before removing.
2. **SYSTEM_ERROR error suppression** — src/lisa.c:2858-2935. Catches fatal errors and unwinds the stack. Keeping this until all error paths properly propagate through real exception handling.
3. **CALLDRIVER disk I/O bypass** — src/lisa.c:2733-2826. Intercepts all disk operations. Keep active until real ProFile/VIA6522 driver chain is complete.
4. **P89c smt_adr prime** — src/m68k.c:3015. Lazy initialization of LOADER's smt_adr VAR. Can be removed once LOADER's BOOTINIT runs natively or smt_adr is part of bootrom priming.
5. **P80c INIT_FREEPOOL pool-header repair** — src/m68k.c:3867. Patches a record field-offset bug that corrupts pool structure. Remove once codegen ensures correct offsets.
6. **P37 FS_CLEANUP bypass** — src/m68k.c:3613. Skips filesystem init. Remove when real FS is functional.
7. **P50 unitio bypass** — src/m68k.c:3848. Disk I/O wrapper. Remove with real driver.

---

## Complete Inventory

### kernel-proc-stub — Active (8)

#### P37: FS_CLEANUP bypass
- **Location**: src/m68k.c:3613-3625
- **What it hides**: FS_CLEANUP (fsinit.text:136) — filesystem initialization cleanup
- **Trigger**: cpu->pc == pc_FS_CLEANUP
- **Side-effect**: Records milestone, pops return address, returns immediately
- **Why needed**: FS_CLEANUP body crashes into $F8xxxx wild-PC space (code-corruption region)
- **Status**: ACTIVE

#### P38: PR_CLEANUP bypass (DISABLED)
- **Location**: src/m68k.c:3631-3690 (wrapped in `if (0 && ...)`)
- **What it hides**: PR_CLEANUP (STARTUP:2082) — pre-scheduler PCB cleanup
- **Trigger**: cpu->pc == pc_PR_CLEANUP
- **Side-effect**: Walks ready queue, unlinks startup pseudo-PCB, enters scheduler
- **Why needed**: Original: c_pcb_ptr nil during boot, crash on unlink. Now c_pcb_ptr valid via CreateProcess native.
- **Status**: DISABLED (P81c) — natural body works with CreateProcess HLE fixed

#### P50: unitio bypass
- **Location**: src/m68k.c:3848-3860
- **What it hides**: unitio (OSUNITIO.TEXT:32) — unified disk I/O procedure
- **Trigger**: cpu->pc == pc_unitio
- **Side-effect**: Zeros errnum/actual output params, returns with success
- **Why needed**: Body enters infinite loop (LINK frame issue); real body is thin wrapper around lisaio which we HLE elsewhere
- **Status**: ACTIVE

#### P35: SYS_PROC_INIT bypass (DISABLED)
- **Location**: src/m68k.c:4004-4012 (wrapped in `#if 0`)
- **What it hides**: SYS_PROC_INIT (STARTUP:2042) — kernel process creation
- **Trigger**: cpu->pc == pc_SYS_PROC_INIT
- **Side-effect**: Pops return address and returns
- **Why needed**: Earlier: cascading failures in Make_SProcess → MM_Setup → crea_ecb. Now P79 fixes (record layouts, enum constants) allow natural execution.
- **Status**: DISABLED (P80) — body runs natively

#### P43: Wait_sem bypass (DISABLED)
- **Location**: src/m68k.c:3823-3834 (wrapped in `if (0 && ...)`)
- **What it hides**: Wait_sem (procprims:721) — semaphore wait operation
- **Trigger**: cpu->pc == pc_Wait_sem
- **Side-effect**: Decrements semaphore count, returns
- **Why needed**: Original: Pascal MOVE.B codegen left stale D0 upper bits; `if priority < semPriority` incorrectly branched, wrote $E2 to priority. Fixed by P80h2 zero-extend.
- **Status**: DISABLED (P81a) — codegen bug fixed

#### P44: MM_Setup bypass (DISABLED)
- **Location**: src/m68k.c:3808-3817 (wrapped in `if (0 && ...)`)
- **What it hides**: MM_Setup (load2.text:637) — per-process memory manager setup
- **Trigger**: cpu->pc == pc_MM_Setup
- **Side-effect**: Writes zero to varptr (sl_sdbRP), returns
- **Why needed**: Original: slocal_sdbRP chains weren't initialized. Static-link ABI bug; natural body works post-fix.
- **Status**: DISABLED (P81c) — ABI fixed

#### P81c: Make_File bypass (DISABLED)
- **Location**: src/m68k.c:4497-4499 (reference only, fully disabled in `#if 0`)
- **What it hides**: Make_File (fsui1.text:593) — backing file creation for data segments
- **Trigger**: cpu->pc == pc_Make_File
- **Side-effect**: Would write ecode=0 and return (no file needed for memory-only segments)
- **Why needed**: Original: filesystem non-functional, DecompPath/SplitPathname writes to garbage addresses. Static-link bug; disabled.
- **Status**: DISABLED (P81c) — natural body works

#### P81a: CreateProcess HLE (DISABLED)
- **Location**: src/m68k.c:4229-4341 (wrapped in `if (0 && ...)`)
- **What it hides**: CreateProcess (process creation procedure) — initializes PCB and syslocal
- **Trigger**: cpu->pc == pc_cp (looked up via boot_progress_lookup or hardcoded $04E348)
- **Side-effect**: Initializes PCB fields (priority, domain), env_save_area, stack top, A5/A6 pointers
- **Why needed**: Original: Build_Syslocal/Build_Stack had A6 corruption → $FD800000. Record field offset corruption. Now: static-link ABI fixed, natural body works.
- **Status**: DISABLED (P81a) — tested; natural CreateProcess succeeds

---

### kernel-proc-stub — Disabled (8) — all listed above

---

### error-suppress — Active (1)

#### SYSTEM_ERROR suppression (general case)
- **Location**: src/lisa.c:2833-2982
- **What it hides**: SYSTEM_ERROR (panic handler) — unrecoverable error callback
- **Trigger**: cpu->pc == lisa->hle.system_error
- **Side-effect**: Logs error code, analyzes error number, applies context-specific unwinds or suppresses entirely
- **Why needed**: Without this, any SYSTEM_ERROR halts the CPU. During boot, many errors are transient or handled by HLE (e.g., missing boot device).
- **Notes**: Handles specific error codes:
  - 10738-10741 (stup_find_boot): suppress; HLE boot device handles it
  - 10707 (stup_fsinit): unwind to FS_INIT caller
  - 10101 (Make_SProcess error): logged; let natural handler run (P86g behavior)
  - 10201, 10204 (SYS_PROC_INIT hard exception): unwind to STARTUP
  - Other: halt CPU and print error
- **Status**: ACTIVE (essential)

#### P76: SYSTEM_ERROR(10707) unwind
- **Location**: src/lisa.c:2874-2917
- **What it hides**: Exception unwinding for FS_INIT failure
- **Trigger**: err_code == 10707 && g_vec_guard_active
- **Side-effect**: Walks A6 frame chain to find FS_INIT caller, restores stack and returns to caller
- **Why needed**: FS_INIT body loops with `until error=0`, fails (no real disk), hits SYSTEM_ERROR(10707), suppression returns to loop, loop doesn't see error=0, infinite loop. Unwind skips FS_INIT entirely.
- **Status**: ACTIVE

---

### error-suppress — Disabled (1)

#### P86g: SYSTEM_ERROR(10101) — no longer suppressed
- **Location**: src/lisa.c:2925-2934 (reference and diagnostics only)
- **What it hides**: Make_SProcess error
- **Trigger**: err_code == 10101 && g_vec_guard_active
- **Side-effect**: Just logs it; lets natural SYSTEM_ERROR handler run
- **Why needed**: Original: Make_SProcess failing → suppress → continue. Now: static-link ABI fixed, CreateProcess native. Log the error for diagnostic purposes but don't suppress.
- **Status**: DISABLED — logged for diagnostics

---

### driver-intercept — Active (7)

#### CALLDRIVER — disk I/O intercept
- **Location**: src/lisa.c:2733-2826
- **What it hides**: CALLDRIVER (SOURCE-MOVER.TEXT) — generic driver call interface
- **Trigger**: cpu->pc == lisa->hle.calldriver
- **Side-effect**: Intercepts at CALLDRIVER entry. Reads function code and dispatches:
  - HLE_DINIT, HLE_HDINIT: return success
  - HLE_DSKIO, HLE_HDSKIO: read/write blocks directly from ProFile image
  - HLE_DDOWN, HLE_HDDOWN, HLE_DSKUNCLAMP, HLE_DINTERRUPT, HLE_DALARMS, HLE_DCONTROL: no-op
  - Other: pass through to real driver
- **Why needed**: ProFile driver / VIA6522 not yet emulated. Boot needs to load SYSTEM.OS from disk.
- **Reads from**: lisa->profile.data (mounted disk image)
- **Status**: ACTIVE (essential for boot)

#### HLE_DSKIO — disk read/write
- **Location**: src/lisa.c:2777-2803 (part of CALLDRIVER)
- **What it hides**: DSKIO driver function — block I/O operation
- **Trigger**: fnctn_code == HLE_DSKIO inside CALLDRIVER
- **Side-effect**: Reads operatn (0=read, 1=write), buffer address, length, block number. Direct memcpy to/from disk image.
- **Status**: ACTIVE (core disk I/O)

#### HLE_DINIT, HLE_HDINIT — disk initialization
- **Location**: src/lisa.c:2771-2776 (part of CALLDRIVER)
- **What it hides**: Driver init functions
- **Trigger**: fnctn_code == HLE_DINIT or HLE_HDINIT inside CALLDRIVER
- **Side-effect**: Log "success", return with error=0
- **Status**: ACTIVE

#### HLE_DDOWN, HLE_HDDOWN, HLE_DSKUNCLAMP — disk powerdown/state
- **Location**: src/lisa.c:2808-2810 (part of CALLDRIVER)
- **What it hides**: Disk power-down and state management
- **Trigger**: fnctn_code in {HLE_DDOWN, HLE_HDDOWN, HLE_DSKUNCLAMP, ...}
- **Side-effect**: No-op (return success)
- **Status**: ACTIVE (harmless)

#### HLE prof_entry — ProFile block read (boot path)
- **Location**: src/lisa.c:2986-3058
- **What it hides**: prof_entry (PROM routine at $FE0090) — ProFile sector read
- **Trigger**: cpu->pc == $FE0090 (intercepted in m68k exception handler before ROM RTS)
- **Side-effect**: Reads tag (20 bytes) and data (512 bytes) from disk image given sector number (D1), stores at A1/A2
- **Why needed**: Boot track (blocks 0-23) pre-loaded at $20000, but individual sectors still need prof_entry to work. Real implementation would toggle ProFile via VIA6522.
- **Status**: ACTIVE (boot only)

#### Loader trap — LDRCALL bypass
- **Location**: src/bootrom.c:382-400 (code generator); intercepted in src/lisa.c (HLE handler)
- **What it hides**: LDRTRAP / DRIVER_CALL — boot loader's disk I/O interface
- **Trigger**: write to $FCC100 (loader trap port)
- **Side-effect**: Emulator's io_write_cb intercepts the port write, reads fake_parms pointer from D2, processes loader call directly
- **Why needed**: Boot loader reads SYSTEM.LLD / SYSTEM.OS using LDRCALL. Our cross-compiled binary has different addresses than original LOS 3.1.
- **Status**: ACTIVE (essential for real Lisa images)

---

### mmu-patch — Active (2)

#### HLE TRAP #6 — PROG_MMU (DO_AN_MMU)
- **Location**: src/m68k.c:2265-2367
- **What it hides**: TRAP #6 handler from LDASM — MMU segment programming
- **Trigger**: vector == 38 (TRAP #6); handler address at $98 == $3F8
- **Side-effect**: Reads D0 (domain), D2/D3 (segment/count), reads SMT base from g_hle_smt_base, writes SORG/SLR/ACCESS values to CPU's MMU hardware (via lisa_hle_prog_mmu)
- **Why needed**: Real LDASM TRAP #6 handler uses setup-mode MMU register access (physical addresses $8000/$8008 stride). We emulate this by intercepting and writing directly to MMU state.
- **Post-P89**: reads real SORG/SLR/ACCESS values that SETMMU wrote to SMT entries (via P89b codegen fix)
- **Status**: ACTIVE (essential)

#### P89c — smt_adr prime on SETMMU entry
- **Location**: src/m68k.c:3015-3062
- **What it hides**: LOADER's BOOTINIT initialization of smt_adr VAR
- **Trigger**: cpu->pc == pc_SETMMU (first time); A5 in $CC0000-$CE0000 (sysglobal)
- **Side-effect**: Writes g_hle_smt_base to A5-6112 (smt_adr slot)
- **Why needed**: LOADER's BOOTINIT (`smt_adr := pointer(smt_base)`) is in the boot sequence we skip. SETMMU reads smt_adr to know where SMT entries live. Lazy prime mimics BOOTINIT's effect.
- **Post-P89**: Belt-and-suspenders after STARTUP's SETMMU now uses smt_addr at A5-24887 instead
- **Status**: ACTIVE

---

### codegen-workaround — Active (5)

#### P80c — INIT_FREEPOOL pool header repair
- **Location**: src/m68k.c:3867-3913
- **What it hides**: INIT_FREEPOOL record field layout bug
- **Trigger**: cpu->pc enters and exits INIT_FREEPOOL (via boot_progress_lookup)
- **Side-effect**: Detects pool_size corruption (field wrote to offset 0 instead of 2), repairs hdr_freepool structure in-place
- **Why needed**: Record field offset corruption bug: INIT_FREEPOOL's compiled code writes firstfree (int4) at offset 0 instead of offset 2, overwriting pool_size with $00000008
- **Codegen source**: Field offset miscalculation in pascal_codegen.c record layout (pre-P82 era)
- **Status**: ACTIVE (diagnostic patch, prevents pool corruption)

#### P80c — GETSPACE tracing
- **Location**: src/m68k.c:3915-3939
- **What it hides**: GETSPACE (memory allocation) — diagnostic only
- **Trigger**: cpu->pc == pc_GETSPACE
- **Side-effect**: Logs allocation requests; if A6 in suspicious range ($D00000+), dumps call chain
- **Status**: ACTIVE [PROBE] — observation only, doesn't change behavior

#### P78d — vector table guard activation
- **Location**: src/m68k.c:3940-3996
- **What it hides**: g_vec_guard_active flag control and SYS_PROC_INIT diagnostics
- **Trigger**: cpu->pc == pc_SYS_PROC_INIT (first time)
- **Side-effect**: Sets g_vec_guard_active flag, dumps sysglobal state (MMRB, semaphores, pools)
- **Why needed**: Instrumentation to catch VEC-WRITE and vector-table corruption during process creation
- **Status**: ACTIVE [PROBE] — observation only

#### P85 — buffer pool and memory allocation tracing
- **Location**: src/m68k.c:3248-3437
- **What it hides**: FlushNodes, RELSPACE, GETSPACE, QUEUE_PR, MAP_SEGMENT, PROG_MMU, InitBuf, InitBufPool — comprehensive boot tracing
- **Trigger**: cpu->pc == pc_FlushNodes, pc_RELSPACE, etc. (via boot_progress_lookup)
- **Side-effect**: Logs entry points, parameters, return addresses
- **Status**: ACTIVE [PROBE] — observation only

#### P85d — RELSPACE badptr1 guard
- **Location**: src/m68k.c:3290-3318
- **What it hides**: RELSPACE (segment deallocation) — safety net for badptr1
- **Trigger**: cpu->pc == pc_RELSPACE && ordaddr == $414231 (badptr1, HLE boot-device marker)
- **Side-effect**: Skips RELSPACE body (returns immediately) when ordaddr is the badptr1 sentinel created by HLE
- **Why needed**: HLE boot-device SYSTEM_ERROR(10738) returns early from mount logic, leaving badptr1 on stack. RELSPACE shouldn't try to deallocate this fake pointer.
- **Status**: ACTIVE (safety net)

---

### trap-hle — Active (1)

#### HLE TRAP #5 — hardware interface (ROM)
- **Location**: src/bootrom.c:165-185 (code generation); src/m68k.c exception handling
- **What it hides**: TRAPTOHW handler — early boot hardware calls
- **Trigger**: TRAP #5 vector = $FE0330
- **Side-effect**: Checks D7 function number: ScreenAddr ($18) or AltScreenAddr ($1A) loads screen base from $110 into A0; others RTE
- **Status**: ACTIVE (boot ROM only)

---

### memory-prime — Active (1)

#### g_hle_smt_base initialization
- **Location**: src/lisa.c:1601-1605
- **What it hides**: SMT (Segment Mapping Table) base address setup
- **Trigger**: lisa_init() during bootrom_build
- **Side-effect**: Sets g_hle_smt_base = os_end (end of OS binary), allocates 2KB for SMT, 4KB for jump table
- **Why needed**: TRAP #6 PROG_MMU reads/writes SMT entries to program the MMU. SMT must be at a known location.
- **Bootstrapping**: Synth bootrom primes this; SETMMU writes entries there
- **Status**: ACTIVE (essential initialization)

---

### probe — Active (3)

#### P78b — VEC-WRITE source trace
- **Location**: src/m68k.c:2946-2975
- **What it hides**: Vector table write detection
- **Trigger**: cpu->pc in $02B000-$02D000 (FS code) during SYS_PROC_INIT
- **Side-effect**: Traces A6 frame chain when FS code first enters; logs call stack
- **Notes**: P78 diagnostic probes mostly removed; this one kept for FS investigation
- **Status**: ACTIVE [PROBE] — observation only

#### P86 — scheduler and process-dispatch tracing
- **Location**: src/m68k.c:3465-3551
- **What it hides**: Scheduler, Launch, SYS_PROC_INIT entry instrumentation
- **Trigger**: cpu->pc == pc_Scheduler, pc_Launch, pc_SYS_PROC_INIT
- **Side-effect**: Logs A5/A6/A7, PCB pointers, syslocal, invocation counter
- **Status**: ACTIVE [PROBE] — observation only

#### P80e — memory and A6 watchpoints
- **Location**: src/m68k.c:4106-4149
- **What it hides**: A6 frame-pointer tracking and memory corruption detection
- **Trigger**: g_vec_guard_active && (a6 == $CBFD48 || a6 in $D00000-$F00000)
- **Side-effect**: Watches saved A6 at $CBFD48 (SEG_IO frame), logs any changes; detects when A6 moves into syslocal/process area
- **Status**: ACTIVE [PROBE] — observation only

---

### Disabled Procs Summary

All 9 disabled-proc HLEs are conditional jumps wrapped in `if (0 && ...)` or `#if 0`:

1. **P38 PR_CLEANUP** — natural body works with CreateProcess
2. **P35 SYS_PROC_INIT** — P79/P80/P81 codegen fixes enabled natural execution
3. **P43 Wait_sem** — P80h2 zero-extend fixed byte-subrange bug
4. **P44 MM_Setup** — static-link ABI fixed; natural body works
5. **P81a CreateProcess** — static-link ABI fixed; natural body works
6. **P81a ModifyProcess** — disabled with CreateProcess
7. **P81a FinishCreate** — disabled with CreateProcess
8. **P81c Make_File** — static-link bug fixed
9. **P81c DecompPath/parse_pathname** — disabled (partially; P80a notes fixed codegen)

---

## Known Stale References (removed, for cleanup)

- **P31** — removed (code-corruption workaround)
- **P69** — removed (fake MAKE_DATASEG with carved seg_ptr)
- **P77 UNLOCKSEGS** — disabled (seglock sentinel issue)
- **P78c SplitPathname** — disabled (FS issue)
- **P79 MAKE_SYSDATASEG** — disabled (partial; P80g behavior replaces)
- **P80a DecompPath** — disabled (codegen fixed)

---

## Integration Points

### Boot Flow (src/lisa.c, src/m68k.c)

1. **bootrom_generate()** — synthesizes ROM with identity MMU map, TRAP #5 handlers
2. **lisa_init()** — primes g_hle_smt_base, creates SMT region
3. **lisa_hle_set_addresses()** — wires CALLDRIVER, SYSTEM_ERROR, BADCALL addresses from linker map
4. **boot_progress_init()** — loads symbol map for dynamic HLE lookups
5. **emu_run_frame()** → **m68k_execute()** — executes CPU, checks hle_cpu_check() at each TRAP/RTE

### CPU Exception Handler (src/m68k.c:2262-2370)

- Intercepts TRAP #6 (vector 38): checks handler address $98 == $3F8, dispatches to HLE PROG_MMU
- prof_entry (src/lisa.c) intercepted at PC=$FE0090 before ROM RTS

### Memory Write Watchpoint (src/lisa_mmu.c)

- SYSTEM_ERROR writes trigger hle_handle_system_error via lisa->hle.system_error address

---

## Removal Priority (Next Session)

**CRITICAL BLOCKERS** (keep until replacements in place):

1. **TRAP #6 PROG_MMU** — need real MMU register interface or bootrom pre-programming
2. **CALLDRIVER** — need VIA6522 + ProFile driver chain
3. **SYSTEM_ERROR** — need complete exception handler stack

**MEDIUM PRIORITY** (remove after testing natural code paths):

4. **P89c smt_adr prime** — once bootrom or natural BOOTINIT handles SMT init
5. **P80c INIT_FREEPOOL repair** — once codegen ensures correct field offsets
6. **P37 FS_CLEANUP bypass** — once filesystem is functional
7. **P50 unitio bypass** — once real disk I/O works

**LOW PRIORITY** (purely diagnostic; safe to remove):

- All `[PROBE]` entries (P78b, P85/P85d, P86, P80e, GETSPACE trace)
- P80/P80g diagnostic dumping in SYS_PROC_INIT

---

**Generated**: 2026-04-18  
**Source**: Live inspection of src/m68k.c, src/lisa.c, src/lisa_bridge.c, src/toolchain/bootrom.c, src/toolchain/pascal_codegen.c, src/toolchain/toolchain_bridge.c
