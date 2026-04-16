# LisaEmu — Next Session Handoff (2026-04-16)

## Accomplished this session

### Structural codegen fixes (P80–P80h)
1. **8-char significant identifiers** (`src/toolchain/pascal_codegen.c:57`) — `str_eq_nocase` returns true after 8 matching chars
2. **Iterative pre-pass record fixup** (`src/toolchain/toolchain_bridge.c:784`) — 27 records corrected after types-only pre-pass
3. **Imported type preservation** (`src/toolchain/pascal_codegen.c:3513`) — full-pass type declarations skip overwriting imported records with valid offsets
4. **Non-local goto A6 restore** (`src/toolchain/pascal_codegen.c:2693`) — nested proc `goto` follows static link chain
5. **Non-local exit() A6 restore** (`src/toolchain/pascal_codegen.c:1962`) — `exit(proc)` follows static link chain before UNLK/RTS
6. **Boolean NOT for function calls** (`src/toolchain/pascal_codegen.c:1502`) — parameterless functions detected via proc sig lookup
7. **Enum/const priority** (`src/toolchain/pascal_codegen.c:492`) — enum registration checks imported_globals before overwriting CONSTs
8. **Generalized record repair** (`src/toolchain/pascal_codegen.c:696`) — `repair_corrupt_record()` auto-detects all-zero offsets, matches anonymous records by first field name
9. **find_type imported preference** (`src/toolchain/pascal_codegen.c:128`) — prefer imported records with valid offsets over corrupt local ones
10. **Post-creation record repair** (`src/toolchain/pascal_codegen.c:433`) — copy offsets from imported at resolve_type time
11. **Field type_name storage** (`src/toolchain/pascal_codegen.h:68`) — 12-byte type name for pre-pass re-resolution

### HLE mechanisms (`src/m68k.c`, `src/lisa.c`)
12. **MAKE_SYSDATASEG bypass** — all segment creation as resident, allocates from sgheap ($CCC000+)
13. **CreateProcess HLE** — initializes PCB priority/blk_state/domain
14. **FinishCreate HLE** — inserts PCB into fwd_ReadyQ
15. **ModifyProcess/Move_MemMgr bypass**
16. **CHK_LDSN_FREE bypass** — system LDSNs allowed
17. **INIT_FREEPOOL pool repair** — runtime pool header validation
18. **SYS_PROC_INIT crash unwind** — 10101/10201 suppression
19. **MAKE_SYSDATASEG stack pop fix** — pop only ret, caller does ADDA.W #$1A

### Result
- **25/27 kernel milestones** (INIT through PR_CLEANUP)
- **Both MemMgr and Root processes created and queued**
- **Scheduler dispatches processes** for the first time
- Processes crash immediately (env save area uninitialized)

## In progress

### Process environment initialization
The scheduler dispatches but processes crash because:
1. **`ord(@proc)` codegen bug** — `ord(@MemMgr)` generates $CCB802 (A5-relative global offset) instead of $043F56 (code address). Fix needed in `src/toolchain/pascal_codegen.c` around AST_UNARY_OP `@` handling for procedure identifiers.
2. **Environment save area empty** — CreateProcess HLE (`src/m68k.c:3515`) needs to write A5/PC/A6/A7/SR to the syslocal's env_save_area. The env_save_area is at a specific offset in the `syslocal` record (check `source-SYSGLOBAL.TEXT.unix.txt` for the offset of `env_save_area`).

### Segment addresses for both processes
```
MemMgr: PCB=$CCB880, syslocal=$CCC000(3072B), stack=$CCCC00(4608B), code=$043F56
Root:   PCB=$CCB8C4, syslocal=$CCDE00(3072B), stack=$CCEA00(10752B), code=check map for 'Root'
```

## Blocked / open questions

1. **Root cause of record offset corruption**: The `*existing = *t` struct copy in type-decl processing (`src/toolchain/pascal_codegen.c:3526`) creates dangling type pointers (cg->types is freed after each unit). Multiple layers of repair mitigate it but don't eliminate it. A proper fix would remap ALL internal type pointers during the copy, like the REMAP_TYPE_PTR logic in the pre-pass export code (`src/toolchain/toolchain_bridge.c:404`).

2. **Multi-target build pipeline**: Even after processes run, Root → CreateShell → Make_Process('SYSTEM.SHELL') needs SYSTEM.SHELL compiled as a separate binary.

## Pick up here

> Read NEXT_SESSION.md. The scheduler dispatches but processes crash because (1) `ord(@MemMgr)` generates wrong address ($CCB802 instead of $043F56) and (2) the syslocal env_save_area is empty. Fix ord(@proc) in the codegen (check AST_UNARY_OP '@' for procedure names), then write PC/A5/A6/A7/SR to the env_save_area in the CreateProcess HLE. After that, MemMgr should actually START RUNNING.
