# LisaEmu — Next Session Handoff (2026-04-16)

## Session: 17 structural codegen fixes (P80–P80f) — 26/27 milestones

### Key fixes:
1. **8-char significant identifiers** (P80)
2. **27 record layouts** via iterative pre-pass fixup (P80b)  
3. **Imported type preservation** — prevents full-pass offset corruption (P80c)
4. **Non-local goto A6 restore** — follows static link chain (P80e)
5. **Boolean NOT for functions** — SYS_CALLED fix (P80e)
6. **Enum/const priority** — enum ordinals don't overwrite CONST values (P80f)
7. **Anonymous record repair** — matches by first field name (P80f)
8. **MAKE_SYSDATASEG HLE** — resident segment bypass (P80f)

### Result: 26/27 milestones, full kernel boot INIT→PR_CLEANUP

## Current blocker: CreateProcess hard exception

### Symptom
Make_SProcess → Get_Resources → [MAKE_SYSDATASEG HLE bypass succeeds] →
CreateProcess → A6 corrupted to $FD800000 → crash to vector table →
SYSTEM_ERROR(10201).

### Analysis
CreateProcess initializes the new process's PCB, stack frame, and
syslocal structures. The allocated segments are at $CCC000 (syslocal)
and $CCCC00 (stack) — both in mapped sysglobal segment 102.

The crash has A6=$FD800000 — frame pointer completely out of range.
This happens because CreateProcess's compiled code contains WITH blocks
that access the new process's syslocal and stack structures. If any
record type used in those WITH blocks has corrupt field offsets, the
code writes to wrong addresses, corrupting the stack frame.

### Error sequence
1. MAKE_SYSDATASEG HLE bypass: memsize=3072 (syslocal), 4096 (stack)
2. CreateProcess starts initializing process structures
3. A6 gets corrupted during a WITH block in process init code
4. Code crashes to vector table → SYSTEM_ERROR(10201)
5. Unwound past SYS_PROC_INIT → boot continues cleanly

### Next steps

#### 1. Identify which record in CreateProcess is corrupt
CreateProcess uses: PCB, syslocal, init_stack, segHandle, stkInfo_rec.
Add type debug prints to find which record has all-zero offsets in the
local type table. Then ensure repair_corrupt_record catches it.

#### 2. Root-cause the offset corruption
The `*existing = *t` struct copy in type-decl processing zeroes field
offsets for SOME records but not others. Need to understand WHY some
copies corrupt and others don't. Possible: the source type `t` was
already corrupt when copied (created by a resolve_type that used a
corrupt imported type as a field type).

#### 3. Alternative: HLE CreateProcess
If the codegen fix is too deep, bypass CreateProcess entirely with an
HLE that sets up a minimal PCB, stack frame, and process state. The
PCB is already allocated by GETSPACE. The syslocal and stack are at
known addresses from the MAKE_SYSDATASEG HLE.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `2ebac61` (26/27, kernel boot complete, process creation WIP)
