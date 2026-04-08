# LisaEmu — Next Session Plan (April 9, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## Where We Are: OS Kernel Running

Both paths reach deep OS initialization. The OS scheduler runs and
dispatches processes. SYSTEM.LLD and SYSTEM.OS are loaded and executing.

### Pre-built image: $520842 scheduler + $208A06 driver dispatch
- SYSTEM.LLD loaded, initialized, COPS commands sent ($7C mouse enable)
- SYSTEM.OS loaded at segment 41 ($520000+)
- OS scheduler at $520842 runs, dispatches driver code
- Driver dispatch at $208904 reads pointers from $494/$498 → both zero
- Driver never runs because BOOT_IO_INIT didn't complete

### Cross-compiled: past POOL_INIT, deep in OS init
- POOL_INIT, INIT_FREEPOOL, GETSPACE all work correctly
- TRAP #5/#6/#7 calls active → OS initializing
- Needs same hardware as pre-built path

## Immediate Next Step: BOOT_IO_INIT Hardware

The driver pointers at $494/$498 are set by BOOT_IO_INIT → CALLDRIVER(dinit).
This needs the ProFile/COPS hardware chain:

```
BOOT_IO_INIT
  → INIT_BOOT_CDS
    → FIND_BOOT (needs COPS PRAM for boot device)
    → NEW_CONFIG
      → UP
        → CALLDRIVER(dinit) → ProFile VIA1 handshake
```

### Option A: Full hardware emulation (correct approach)
1. ProFile VIA1 handshake: CMD/BSY/OCD state machine on Port B
2. COPS VIA2 protocol: keyboard ID, clock data, mouse enable ack
3. VIA timer operation for timeout detection

### Option B: HLE shortcut (faster)
1. Set driver pointers at $494/$498 to valid driver entry points
2. Populate device configuration in sysglobal
3. Skip BOOT_IO_INIT hardware dependencies

## Session Summary: 23 fixes, 27 commits

### Critical breakthroughs:
- **MMU always enabled during setup mode** → DO_AN_MMU works
- **Interface files compiled first** → inherited params work
- **All unresolved types default to 4 bytes** → pointer truncation fixed
- **Status register + vretrace + VIA aliases** → hardware init progresses
- **IRQ levels fixed** (all level 1) → interrupt delivery works
- **VIA Port A DDRA-aware reads** → COPS data preserved

### Boot sequence achieved:
```
Pre-built: ROM → LDPROF → Loader → BOOTINIT → SYSTEM.LLD → SYSTEM.OS → Scheduler ✅
Cross-compiled: ROM → PASCALINIT → INITSYS → POOL_INIT → INIT_TRAPV → Deep init ✅
```
