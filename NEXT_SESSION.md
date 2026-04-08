# LisaEmu — Next Session Plan (April 8, 2026)

## Quick Start

```bash
make
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built: reaches SYSTEM.LLD
build/lisaemu Lisa_Source                                    # cross-compiled: needs codegen fix
```

## Where We Are

### Pre-built image: SYSTEM.LLD executing
The pre-built OS loads through the complete boot sequence:
Boot ROM → LDPROF → Pascal loader → BOOTINIT (212 TRAP6 calls) →
LOADSYS → SYSTEM.LLD loaded from disk → INSTALL_LLD → segment 16 mapped →
**SYSTEM.LLD code running at $207F4A** (INIT_LLD hardware initialization).

Stuck in a wait loop — LLD code is polling VIA2 or COPS for a hardware
response that our emulator doesn't generate yet.

### Cross-compiled: INIT_FREEPOOL codegen bug
POOL_INIT gets correct params, but INIT_FREEPOOL has a codegen bug:
parameters loaded as constants instead of from stack frame. Error 10701.

### Both paths converge at hardware emulation
Once the cross-compiled codegen is fixed, it will hit the same hardware
wait as the pre-built image. Both need COPS/VIA hardware responses.

## Session Fixes (14 fixes, 15 commits)

| # | Fix | Both paths? |
|---|-----|:-----------:|
| 1 | MMU stack SOR (hw_adjust) | ✅ |
| 2 | 32-bit multiply | ✅ codegen |
| 3 | 32-bit divide | ✅ codegen |
| 4 | Pre-built image boot (--image) | Pre-built |
| 5 | ProFile no-deinterleave | ✅ |
| 6 | RAM address wrapping | ✅ |
| 7 | Context 0/1 mirroring | ✅ |
| 8 | RAM 2.25MB (mmucodemmu alias fix) | ✅ |
| 9 | **MMU enabled during setup mode** | ✅ |
| 10 | Segment 84/126/127 → all contexts | ✅ |
| 11 | Report 2MB to loader (SOR 12-bit) | ✅ |

## Next Steps (Priority Order)

### 1. COPS/VIA2 hardware (unblocks both paths)
SYSTEM.LLD's INIT_LLD polls VIA2 for COPS responses. The COPS
microcontroller handles keyboard, mouse, clock, and power. We need:
- VIA2 CA1 handshake (COPS data ready signal)
- COPS command/response protocol (at minimum: keyboard ID, clock data)
- VIA2 port A/B read/write for COPS communication

### 2. INIT_FREEPOOL codegen fix (unblocks cross-compiled path)
The codegen for `(* inherited params *)` doesn't register parameters
properly. `fp_ptr` loaded as constant instead of A6+offset read.
Also: LINK allocates 4 bytes for 2 pointer locals (should be 8).

### 3. Continue OS loading (after SYSTEM.LLD init completes)
After INIT_LLD succeeds, the loader reads SYSTEM.OS segments from disk.
Each segment gets mapped via TRAP #6. Then ENTEROP jumps to the OS
entry point (STARTUP.TEXT). This is where our cross-compiled path
and pre-built path fully converge.

## Boot Sequence (pre-built image)
```
✅ Boot ROM → LDPROF → Pascal loader entry
✅ BOOTINIT → INITMMUTIL → DO_AN_MMU (212 TRAP6 calls)
✅ LOADSYS → SYSTEM.LLD read from disk (sectors 258+)
✅ INSTALL_LLD → segment 16 mapped (SOR=$EF9)
✅ INIT_LLD → SYSTEM.LLD code executing at $207F4A
⏳ INIT_LLD waiting for COPS/VIA2 hardware response
❌ LOAD_OS → read SYSTEM.OS segments
❌ ENTEROP → jump to OS entry point (STARTUP.TEXT)
❌ OS initialization → Desktop
```

## Key Hardware Needed

| Component | Status | What it does |
|-----------|--------|-------------|
| CPU (68000) | ✅ Working | All instructions |
| MMU | ✅ Working | 5 contexts, 128 segments |
| ProFile disk | ✅ Working | Block reads via HLE |
| VIA1 | Partial | ProFile handshake (needs improvement) |
| VIA2 | Partial | COPS communication (needs implementation) |
| COPS | ❌ Missing | Keyboard, mouse, clock, power |
| Display | Partial | 720×364 mono bitmap, no screen updates yet |
| Floppy | ❌ Missing | Not needed for ProFile boot |
