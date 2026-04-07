# LisaEmu — Next Session Plan (April 7, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source
```

## Where We Are

**Past stack corruption, jump table crashes, and memory mapping bugs.**
The OS boot reaches POOL_INIT with correct mapped addresses. GETLDMAP
copies all 582 bytes of boot parameters. REG_TO_MAPPED transitions
physical→mapped stack correctly.

**Stops at:** SYSTEM_ERROR(0) — POOL_INIT gets wrong parameters.

## What Was Fixed (this session: 6 fixes, 3 commits)

### 1. Variant record sizing (parser)
Variant record fields were completely skipped → zero-size records →
LINK A6,#0 → saved frame pointer overwritten → SP corruption.
Fix: Parse variant fields, use largest arm for sizing.

### 2. DRIVERASM symbol priority (linker)  
DRIVERASM thunks (GETSPACE, INTSOFF, etc.) shadowed real implementations →
indirect through uninitialized jump table at $210 → crash to vector table.
Fix: DRIVERASM entries always yield to non-DRIVERASM entries.

### 3. Duplicate global symbols (codegen)
Interface + external declarations created two global entries per proc.
The first (ENTRY, val=0) blocked real assembly implementations.
Fix: add_global_sym returns existing symbols by name.

### 4. Procedure vs function distinction (codegen)
Added is_function flag to cg_proc_sig_t. Procedure names in expressions
push their address; functions are called and return values. Fixes
INITSYS(PASCALINIT) — PASCALINIT is a function returning ptr.

### 5. Boot stack in user stack area (lisa.c)
ROM set SP/A6 to $079000 (low memory) but REG_TO_MAPPED expects stack in
user stack MMU segment ($EA800-$EE800). Physical→mapped translation failed.
Fix: Patch ROM SSP, A7, A6 to user stack top before CPU reset.

### 6. SP delta trace (m68k.c)
Per-instruction SP delta check catches when SP changes by >$1000.
Identified UNLK A6 crash, GETSPACE/DRIVERASM crashes, and verified fixes.

## Current Blocker: POOL_INIT Argument Evaluation

POOL_INIT call in INITSYS body (SOURCE-STARTUP.TEXT line 2162):
```pascal
POOL_INIT(MMU_BASE(sysglobmmu), l_sys_global,
          MMU_BASE(sysglobmmu)+b_sgheap-b_sys_global, l_sgheap,
          MMU_BASE(syslocmmu), l_opsyslocal);
```

Observed POOL_INIT params (all wrong):
- mb_sysglob=$F7FFF4 (A6/frame pointer — should be ~$CC0000)
- l_sysglob=$4B7A (code address — should be $6000)
- rest: zeros

### Root cause analysis
1. `MMU_BASE` is a function in MMPRIM: `mmu * maxpgmmu * hmempgsize * 2 = mmu * $20000`
2. `sysglobmmu` is a constant (= 6, giving $C0000)
3. `l_sys_global` is an INITSYS local at A6-118 (verified correct value $6000 before REG_TO_MAPPED)

The codegen is pushing wrong values. Either:
- The function call `MMU_BASE(sysglobmmu)` doesn't evaluate correctly
- The local variable reference `l_sys_global` reads from wrong A6 offset
- The arithmetic expression compilation is broken

### Next step
1. Disassemble the POOL_INIT call site in the generated STARTUP code
2. Check if MMU_BASE call is generated correctly (JSR + use return value)
3. Check if A6-relative local variable offsets match the GETLDMAP copy destinations
4. The locals were verified correct at REG_TO_MAPPED entry — compare with POOL_INIT call offsets

## Verified Working
```
✅ Param block at $D3100: version=22, all fields correct
✅ GETLDMAP: copies 582 bytes, version=22, b_sys_global=$D4800, l_sys_global=$6000
✅ Boot stack: SSP=A6=$EE800 (user stack area)
✅ REG_TO_MAPPED: A6=$EE7F4→$CBFFE0, correct physical→mapped translation
✅ INTSOFF/INTSON → mover.text (real implementations, not DRIVERASM thunks)
✅ SYSTEM_ERROR → SYSGLOBAL (real implementation)
```

## Boot Sequence
```
✅ ROM → SSP=$EE800, A6=$EE800 (user stack area)
✅ PASCALINIT → %initstdio → returns param block ptr ($D3100)
✅ INITSYS → GETLDMAP copies boot parameters → REG_TO_MAPPED
✅ POOL_INIT reached (mapped stack, valid frame)
💥 POOL_INIT wrong params → SYSTEM_ERROR(0)
❌ BOOT_IO_INIT → ProFile handshake
❌ FS_INIT → mount boot volume  
❌ ENTER_SCHEDULER → Desktop
```

## Key Files
```
src/toolchain/pascal_parser.c   — variant record field parsing
src/toolchain/pascal_codegen.c  — dedup globals, proc/func, expressions
src/toolchain/pascal_codegen.h  — is_function flag
src/toolchain/linker.c          — DRIVERASM priority
src/m68k.c                      — SP delta trace, boot sequence traces
src/lisa.c                      — boot stack relocation, parameter block
src/toolchain/bootrom.c         — ROM code (SP/A6 patched at runtime)
```

## Reference: MMU_BASE function (source-MMPRIM.TEXT line 796)
```pascal
function MMU_BASE(mmu: int2): absptr;
begin
  mmu_base := ord4(mmu) * maxpgmmu * hmempgsize * 2;
  {= mmu * 256 * 256 * 2 = mmu * $20000}
end;
```
