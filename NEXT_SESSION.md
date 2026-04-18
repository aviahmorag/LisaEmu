# Next Session Handoff (2026-04-18 — P90 Phase 1 PMEM complete)

## Accomplished this session

Five structural Pascal-codegen fixes + PRAM stub correction. `FIND_PM_IDS`
now returns true natively; Phase 1 PMEM layer is complete.

1. **Signed byte-subrange sign-extension** — `src/toolchain/pascal_codegen.c`
   - Added `type_is_signed_byte(t)` helper (TK_SUBRANGE && size==1 &&
     range_low < 0) and `emit_sign_ext_byte(cg)` (EXT.W + EXT.L).
   - Called after byte reads at:
     - AST_IDENT_EXPR VAR-param branch (~line 1973)
     - AST_IDENT_EXPR local/param branch (~line 2009)
     - AST_IDENT_EXPR global branch (~line 2024)
     - WITH-field lookup path (~line 2046)
     - AST_FIELD_ACCESS (~line 2830)
   - Fixes MAKE_INTERNAL's `if chan = empty` test: -1 byte compared
     to -1 word now actually matches.

2. **Packed variant records pack booleans to 1 byte** —
   `src/toolchain/pascal_codegen.c` (~line 374) +
   `src/toolchain/toolchain_bridge.c` (~line 847).
   - Added `record_has_variant` pre-scan that looks for the
     `__variant_begin__` sentinel the parser emits.
   - When a packed record has EITHER a Tnibble OR a variant, collapse
     boolean to 1 byte so variant arms align.
   - c_ne (`packed record case boolean of true:(b:boolean; p:pmbyte;
     o:integer); false:(l:longint)`) now has true/false arms both 4 bytes.

3. **Packed whole-byte field advances `offset` past the field** —
   `src/toolchain/pascal_codegen.c` (~line 594).
   - Added `if (cg->in_packed) offset += fs;` in the whole-byte
     branch of the record resolver.
   - Previously `offset` stayed at placed_byte_offset, so variant_max_end
     and the final `t->size` missed the last field's size.
   - c_ne: size 2 → 4. `cracked_ne` local allocation 2 bytes → 4 bytes.
     Stops `cracked_ne.l := NextEntry` from stomping the saved A6 in
     the LINK frame.

4. **Aggregate record/array assignment** — `src/toolchain/pascal_codegen.c`
   at start of AST_ASSIGN case (~line 2945).
   - Detect `expr_size(lhs) > 4 || expr_size(rhs) > 4`.
   - Emit a MOVE.B (A0)+,(A1)+ / DBRA D0,-4 block-copy loop of
     `max(lhs_sz, rhs_sz)` bytes.
   - Source addr from gen_ptr_expression (AST_DEREF) or gen_lvalue_addr
     (field/array lvalues); LHS addr pushed/popped on SP.
   - DBRA displacement: `loop_pc - cg->code_size` (relative to the
     displacement word address — same convention as existing DIVS/MOD
     loops using `0x51CB 0xFFF2`).
   - Fixes `mypmem := pmptr^` (pmemrec = 64 bytes) and
     `booter[i] := booter[devcd]` (bootinfo = 88 bytes) which
     previously copied only 2-4 bytes.

5. **`gen_lvalue_addr` for AST_ARRAY_ACCESS saves A0 across index** —
   `src/toolchain/pascal_codegen.c` (~line 1650).
   - `MOVE.L A0,-(SP)` before `gen_expression(index)`.
   - `MOVEA.L (SP)+,A0` after.
   - Fixes cases where the index is a WITH-field (e.g.
     `PMRec[cracked_ne.offset]` in GetNxtConfig) — previously
     `gen_with_base` would load the WITH record into A0, stomping
     the array base.

6. **PRAM stub uses internal cd_paraport=10** — `src/lisa.c`
   `io_read_cb` (~line 763).
   - Byte 10: `$BE` (slot=11) → `$AE` (slot=10). STARTUP's find_boot
     uses DRIVERDEFS (0-based) internal form; GetNxtConfig converts
     to external (+1). So pm_slot must = 10, not 11.
   - Checksum byte 62-63: `$680C` → `$780C` (XOR of all 32 big-endian
     words must stay 0).

**Verification**: FIND_PM_IDS return D0.W = $0001 (pre-UNLK probe).
INIT_BOOT_CDS no longer takes the `if not FIND_PM_IDS` fallback.
Boot progresses into LOADEM, which is the Phase-2 boundary.

**Milestone count**: 21/27 — same as session start, no regression.

**Commits**:
- `fb17f53` — auto-checkpoint: code (pascal_codegen.c, toolchain_bridge.c,
  lisa.c) + NEXT_SESSION.md intermediate
- `8a254b1` — docs: CLAUDE.md — P90 Phase 1 complete summary
- Both pushed to origin/main.

## In progress

Phase 1 PMEM is complete in the natural sense. But the 10738
suppression at `src/lisa.c:2886` is still present and still firing —
**the firing site has shifted from Phase 1 to Phase 2**:

- Was (pre-P90): INIT_BOOT_CDS line 1632, `bootdev > 2` after
  FIND_PM_IDS returned false.
- Now (post-P90): LOADEM line 1539, `not LD_OPENINPUT('SYSTEM.CD_PROFILE')`.

LOADEM is at `$04A0C` (file offset; check current `build/LisaOS.lisa/linked.map`).
The 10738 fires at ret=$004A50 (~line 1539 in source-STARTUP.TEXT).

The suppression catches it (err_code 10738..10741) and lets the boot
continue past LOADEM, but downstream code sees a half-initialized
boot state. That's why we don't reach FS_INIT.

## Blocked / open questions

**Phase 2 strategy decision needed before writing code**. Three options,
increasing fidelity:

1. **Narrow the 10738 suppression** to only fire from LOADEM's PC
   range (`$04A0C..$04A6E`). Adds a `pc_LOADEM_start/end` lookup +
   guard. Leaves INIT_BOOT_CDS's line 1632 path genuinely unreachable.
   Documents Phase 1 completion cleanly. Low effort, no new code.

2. **HLE LOADCD / LD_OPENINPUT** to return a synthetic
   SYSTEM.CD_PROFILE binary — maybe from `_inspiration/lisaem-master/`
   ROMs or a built-in stub driver. Skips the catalog lookup entirely.
   Medium effort, still hacky.

3. **Real Phase 2**: populate the profile.image's MDDF/catalog with a
   compiled SYSTEM.CD_PROFILE driver. Needs:
   - Multi-target compile support in our toolchain (currently only
     compiles SYSTEM.OS).
   - Intrinsic-library format support (.LIB files read by the loader
     at runtime).
   - Catalog entries in the disk image so LD_OPENINPUT can find the
     file by name.
   Heavy lift, but it's the "real fix" the durable rule prefers
   (`.claude/memory/feedback_do_the_real_fix.md`).

Pick option 1 for a short session, option 3 for a several-session arc.
The user has preferred real structural fixes throughout the project
(see feedback memory).

**Side question**: do any other packed variant records benefit from
fix #2 (boolean packing)? Possibilities: any `case of` record used for
disk-catalog entries, any queue-link structure that Apple packed for
space, blockPart-like records. Worth grepping `packed record case` in
`Lisa_Source/LISA_OS/` to audit.

## Pick up here

> Continue Phase 2 of the LisaEmu multilayer rewrite. Phase 1 (PMEM +
> boot-CD selection) is complete: `FIND_PM_IDS` works naturally thanks
> to five Pascal-codegen structural fixes in session P90 (see
> `CLAUDE.md` and this file for detail). The 10738 suppression at
> `src/lisa.c:2886` is still firing but now from LOADEM, not
> INIT_BOOT_CDS.
>
> Before touching any code, read NEXT_SESSION.md's "Blocked / open
> questions" section and decide which of the three Phase 2 options
> to pursue. If I don't have a strong preference, start with
> Option 1 (narrow the 10738 suppression scope to LOADEM only) — it's
> a 10-minute task that cleanly marks Phase 1 done and surfaces
> whether Phase 2's LOADEM bypass still holds the boot together.
> Then ask me whether to continue to Option 2 or 3.
>
> Rule: HLEs are load-bearing layers (see
> `.claude/memory/feedback_hle_layers_load_bearing.md`). Remove only
> in the same commit as the replacement subsystem.

---

## Phase context reminder

| # | Phase | Status |
|---|---|---|
| 1 | PMEM + boot-CD layer | **COMPLETE** — FIND_PM_IDS natively returns true |
| 2 | ProFile driver wiring | **Next**. LOADEM blocks. |
| 3 | MDDF / disk-image layout | Needed for Phase 2 option 3. |
| 4 | IRQ-driven I/O completion | Not started. |
| 5 | SYS_PROC_INIT + processes | Not started. |
| 6 | Cleanup HLEs (FS/MEM/PR_CLEANUP) | Not started. |
| 7 | Safety nets (REG_OPEN_LIST, excep_setup) | Not started. |

---

## Probe scaffolding (if you need to rerun diagnostics)

Removed at session end. To re-add, instrument `src/m68k.c` near the
main instruction-dispatch loop with:
- `boot_progress_lookup("FIND_PM_IDS")` for the entry address
- Scan FIND_PM_IDS body (pc..pc+0x600) for JSR abs.L (op 0x4EB9) +
  target == `boot_progress_lookup("GetNxtConfig")` to find the
  post-JSR return point
- Read get_it/pm_error/key at the below offsets

FIND_PM_IDS local layout (post-P90, after `LINK A6,#-144`):
- -4: static link
- -68..-5: mypmem (64 bytes pmemrec)
- -72: pmptr (longint)
- -122: get_it (ConfigDev — bytes 0,1,2 = pos.slot/chan/dev)
- -124: pm_error (integer)
- -132: key (longint)
- -140: foundall (word)
- -142: nonslot (word)
- -144: find_pm_ids return slot (word)

INIT_READ_PM fills `param_mem.parm_mem[1..32]` starting at
`A5 - 24853 + 2` (= $CC0ECD with A5=$CC6FE0). After INIT_READ_PM runs,
the 64 bytes should match the PRAM stub in `src/lisa.c`.
