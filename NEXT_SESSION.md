# Next Session Handoff (2026-04-18 — Phase 1 PMEM essentially complete; 21/27 milestones)

## What happened this session (P90 — five structural Pascal-codegen fixes)

Picked up where the previous handoff left off: `FIND_PM_IDS` returned
false despite `pm_good` being true and `parm_mem[]` having the right
bytes. Instrumented the inner match loop via boot-progress-lookup
probes, found the REAL chain of bugs (not just the obvious one), and
fixed five compounding issues in our Pascal code generator. `FIND_PM_IDS`
now natively returns true for our ProFile-on-parallel-port PRAM stub;
the 10738 suppression is no longer load-bearing at the INIT_BOOT_CDS
level (it is still firing from LOADEM, a Phase-2 problem).

### The bug chain — in the order I found it

1. **`MAKE_INTERNAL` compared signed bytes as zero-extended unsigned**
   (`src/toolchain/pascal_codegen.c`). `pmbyte = -128..127` is a signed
   subrange. After `chan := 0 - 1 = -1` stored as byte `$FF`, the
   re-read was `MOVEQ #0,D0; MOVE.B (A0),D0` — zero-extend to word
   `$00FF`. Compared to the `-1` literal (`MOVEQ #$FF,D0; MOVE.W D0,D1`
   = `$FFFF`), `$00FF != $FFFF`, so `if chan = empty` was always
   false. Result: `chan` stayed `$FF` instead of becoming
   `emptychan = 7`, and `dev` stayed `$FF` instead of `emptydev = 31`.
   FIND_PM_IDS's match then compared `boot_chan=7` against
   `pos.chan=$00FF` and never fired.
   - Fix: when reading a signed byte-subrange (`t->kind == TK_SUBRANGE
     && t->size == 1 && t->range_low < 0`), follow the zero-extending
     load with `EXT.W D0; EXT.L D0`. Applied at AST_IDENT_EXPR
     (VAR-param, local/param, global), AST_FIELD_ACCESS, and the WITH
     field-lookup path.
   - Helper: `type_is_signed_byte(t)` + `emit_sign_ext_byte(cg)`.

2. **Packed variant records with `boolean` left the arms different
   sizes**, so the variants didn't overlap cleanly and
   `cracked_ne.l := NextEntry` left one byte stale in the `true` arm.
   `c_ne` = `packed record case boolean of true:
   (PutLastOp:boolean; lastsize:pmbyte; offset:integer); false:(l:longint)`.
   Boolean defaulted to 2 bytes even in packed mode unless the record
   had a Tnibble field (P87c guard for segstates). That made the true
   arm 2+1+2=5 bytes while `l:longint` is 4 bytes: assigning `l`
   touched bytes 0-3 but `offset` lived at byte 3, so reading `offset`
   pulled a stale byte 4. GetNxtConfig's `if offset <= 8` then
   evaluated wrong, taking the `errnum := e_badnext` path every time.
   - Fix: add a `record_has_variant` pre-scan (looks for the
     `__variant_begin__` sentinel the parser inserts). When a packed
     record has EITHER a nibble OR a variant, collapse boolean to
     1 byte. Same change mirrored in the toolchain_bridge.c Phase-2
     fixup so imported types are also corrected.

3. **Packed-record `offset` variable tracked placed_byte_offset, not
   end-of-field**, so `variant_max_end` and the final `t->size` missed
   the last field's size. In packed mode the whole-byte branch was:
   ```c
   placed_byte_offset = offset;
   bit_cursor = (offset + fs) * 8;   // cursor advanced
   // offset NOT advanced
   ```
   For c_ne, offset stuck at 2 (the start of the integer field) instead
   of 4 (past it), so t->size came out 2. Our `process_var_decl`
   allocated only 2 bytes for `cracked_ne`, `LINK A6,#-22` was 2 bytes
   short, and `cracked_ne.l := NextEntry` stored a LONG at `-2(A6)` —
   stomping the saved A6 in the LINK frame. GetNxtConfig's UNLK then
   restored a corrupt A6, and FIND_PM_IDS ran the rest of its body
   with A6 = `$01FEA4` instead of `$CBFEA4`, reading locals from
   low memory.
   - Fix: in the whole-byte branch, when `cg->in_packed` is true, also
     do `offset += fs` so it reflects end-of-field, matching the
     bit-packed branch (`offset = byte_idx + 1`) and the unpacked
     branch (`offset += fs`). Toolchain_bridge's phase-2 already did
     this correctly.

4. **Record/array value assignment copied at most 4 bytes** — no
   aggregate-copy path existed at all. `mypmem := pmptr^` (where
   pmemrec = 64 bytes) compiled as `MOVE.L -72(A6),D0; MOVEA.L D0,A0;
   MOVE.W (A0),D0; MOVE.L D0,-68(A6)` — a 2-byte load and a 4-byte
   store, nothing more. `booter[i] := booter[devcd]` likewise dropped
   all 88 bytes. That explained why the previous session saw
   `for_pos[2]` and `for_pos[3]` filled with uninitialized garbage.
   - Fix: at AST_ASSIGN entry, detect `expr_size(lhs) > 4 ||
     expr_size(rhs) > 4`. Emit a MOVE.B (A0)+,(A1)+ / DBRA D0,-4
     block-copy loop for `max(lhs_type_sz, rhs_type_sz)` bytes.
     Source address via gen_ptr_expression (for AST_DEREF) or
     gen_lvalue_addr (for field/array lvalues). DBRA displacement is
     `loop_pc - cg->code_size` (relative to the displacement word's
     own address — see existing `0x51CB 0xFFF2` DIVS/MOD loops for the
     pattern).

5. **`gen_lvalue_addr` for AST_ARRAY_ACCESS clobbered A0 across the
   index expression.** `PMRec[offset]` in GetNxtConfig, where `offset`
   is a WITH-field of cracked_ne, compiled as:
   ```
   MOVEA.L 16(A6),A0        ; PMRec base → A0
   LEA -4(A6),A0             ; WITH base → A0  ← STOMPS PMRec!
   ADDA.W #2,A0              ; +offset-field offset
   MOVE.W (A0),D0            ; D0 = offset
   MULU #1,D0; ADDA.L D0,A0  ; A0 = WITH-base + offset — garbage
   ```
   CRAK_PM then got called with `&cracked_ne.offset + offset` instead
   of `&mypmem + offset`, producing entirely bogus pos decoding.
   - Fix: push/pop A0 across the index expression:
     ```
     MOVE.L A0,-(SP)    ; save PMRec base
     gen_expression(index) ; index → D0 (may clobber A0)
     MOVEA.L (SP)+,A0    ; restore base
     ```

6. **PRAM stub used the external (1-based, syscall.text) cd_paraport=11
   instead of the internal (0-based, DRIVERDEFS) cd_paraport=10**
   (`src/lisa.c`). STARTUP uses the DRIVERDEFS form: `boot_slot :=
   cd_paraport = 10`. GetNxtConfig converts packed `pm_slot → pos.slot
   = pm_slot + 1` (external), and MAKE_INTERNAL subtracts 1 to return
   to internal. So the packed byte must encode `pm_slot = 10`, not 11.
   - Fix: byte 10 `$BE` → `$AE`. Checksum recomputed from `$680C` to
     `$780C` so the 32-word XOR stays 0.

### What works now

- `pm_good = true` after INIT_CONFIG ✓
- `FIND_PM_IDS` natively returns `$0001` (true) — verified via
  pre-UNLK probe.
- INIT_BOOT_CDS no longer takes the `if not FIND_PM_IDS` twiggy
  fallback. The 10738 suppression at that level is dead code.

### What's still firing 10738

`LOADEM` calls `LD_OPENINPUT('SYSTEM.CD_' + drvr_name)`, which needs a
real `SYSTEM.CD_PROFILE` driver file on the boot disk. We don't have
a populated disk-image catalog yet, so `LD_OPENINPUT` returns false
and LOADEM fires `SYSTEM_ERROR(stup_find_boot=10738)`. The existing
suppression still catches it and the boot continues.

This is exactly the Phase 2 (ProFile driver wiring) boundary from
`docs/HLE_INVENTORY.md`. Phase 1 (PMEM) is complete; Phase 2 starts
when LOADEM needs to actually load a driver.

### Milestone count

21/27 — same as start of session. No regression from the P90 fixes.
BOOT_IO_INIT still reached, FS_INIT still blocked. The regression
pattern is the expected multilayer one: Phase 1 is now running
natively, which means we're hitting the NEXT layer's issues earlier
and in slightly different ways. The 10738 suppression has shifted
from "FIND_PM_IDS returns false" to "LOADEM can't open SYSTEM.CD_".

### Files touched

- `src/toolchain/pascal_codegen.c`:
  - `type_is_signed_byte` + `emit_sign_ext_byte` helpers
  - Sign-ext after byte reads at AST_IDENT_EXPR (3 branches),
    AST_FIELD_ACCESS, WITH-field
  - `record_has_variant` pre-scan in AST_TYPE_RECORD, gates boolean=1
    in packed variant records
  - `offset += fs` in packed whole-byte field path
  - Aggregate-copy path at start of AST_ASSIGN
  - A0 push/pop around `gen_expression(index)` in gen_lvalue_addr
    AST_ARRAY_ACCESS
- `src/toolchain/toolchain_bridge.c`:
  - Mirror `record_has_variant` gate for Phase-2 fixup
- `src/lisa.c`:
  - PRAM stub byte 10 `$BE` → `$AE` (cd_paraport internal form)
  - Checksum recomputed to `$780C`

All committed in the same series so downstream sessions see the
entire fix atomically.

---

## Phase 1 status

**COMPLETE** in the natural sense: `FIND_PM_IDS` works with our PRAM
stub. `init_boot_cds`'s Phase-1 10738 path is no longer triggered.

The 10738 suppression in `src/lisa.c:2886` is NOT removed because it
is still load-bearing for Phase 2 (LOADEM). Its scope narrowed:

- **Was**: catching "can't find boot CD in pram" from `init_boot_cds`
  line 1632.
- **Now**: catching "can't open SYSTEM.CD_* driver file" from
  `loadem` line 1539.

When Phase 2 (ProFile driver wiring) lands, that suppression can go.

---

## Phase 2 — what's next

The 10738 now fires from LOADEM: `if not LD_OPENINPUT(itsname) then
SYSTEM_ERROR(stup_find_boot)`. `itsname` = `'SYSTEM.CD_PROFILE'`.

Phase 2 options, increasing order of real fidelity:

1. **Narrow the 10738 suppression** to only fire from LOADEM (PC range
   inside `$04A0C..$04A6E`). Leaves INIT_BOOT_CDS's line 1632 path
   genuinely unreachable. Documents that Phase 1 is done.
2. **HLE LOADCD / LD_OPENINPUT** to return a synthetic SYSTEM.CD_PROFILE
   binary from our bundled `_inspiration/` or a built-in stub. Skips
   the actual catalog lookup but gives a runnable driver binary.
3. **Real Phase 2**: populate the profile.image's MDDF/catalog with a
   compiled SYSTEM.CD_PROFILE driver. Requires multi-target compile
   support + intrinsic-library format support. This is the "real fix"
   the durable memory rule prefers.

Pick (1) for a cheap session-end; (3) is the correct long-term work.

---

## Multilayer arc reminder

| # | Phase | Status |
|---|---|---|
| 1 | PMEM + boot-CD layer | **COMPLETE** — FIND_PM_IDS works natively |
| 2 | ProFile driver wiring | **Next**. LOADEM now blocks. |
| 3 | MDDF / disk-image layout | Not started. Phase 2 depends on this. |
| 4 | IRQ-driven I/O completion | Not started. |
| 5 | SYS_PROC_INIT + processes | Not started. |
| 6 | Cleanup HLEs | Not started. |
| 7 | Safety nets | Not started. |

Durable rule: HLEs are load-bearing layers. Remove only in the same
commit as the replacement subsystem. See
`.claude/memory/feedback_hle_layers_load_bearing.md`.

---

## Probe / debug context (if you need to rerun)

The probe scaffolding I used is removed. To re-add, insert a block at
the top of the main instruction-dispatch loop in `src/m68k.c` that
looks up `FIND_PM_IDS` via `boot_progress_lookup`, scans its body for
`4EB9` JSR opcodes + target-address matches to find the post-JSR
addresses of GetNxtConfig and MAKE_INTERNAL, and reads:
- get_it at `-122(A6)` (3 bytes: slot, chan, dev)
- pm_error at `-124(A6)` (word)
- key (NextEntry) at `-132(A6)` (longint)
- mypmem at `-68(A6)` (64 bytes)
- Return value in D0 right before UNLK A6 at the end of FIND_PM_IDS

Full local layout after `LINK A6,#-144`:
- -4: static link
- -68: end of mypmem (mypmem is 64 bytes from -132 to -69)
- -72: pmptr
- -122: get_it (ConfigDev, ~56 bytes)
- -124: pm_error (integer)
- -132: key (longint)
- -140: foundall (word)
- -142: nonslot (word)
- -144: find_pm_ids return slot (word)

---

## Pick-up prompt

> Continue Phase 2 of the LisaEmu multilayer rewrite. Phase 1 (PMEM +
> boot-CD selection) is done: `FIND_PM_IDS` now works naturally thanks
> to five Pascal-codegen structural fixes + a PRAM stub correction in
> this session. The 10738 suppression at `src/lisa.c:2886` is still
> firing but now from LOADEM, not INIT_BOOT_CDS — narrow the
> suppression scope or build the LOADCD HLE / real driver-load path
> per the Phase 2 plan in NEXT_SESSION.md.
>
> Before writing any Phase 2 code, read CLAUDE.md, the Phase 2 section
> of NEXT_SESSION.md, and `docs/HLE_INVENTORY.md` for the ledger. The
> P90 fixes are structural — any records using packed variants
> (c_ne, `Ch_info`-ish variants, blockPart) now have correct layouts,
> and record/array `:=` actually copies bytes — expect downstream
> code to behave better than last session but don't be surprised if
> something ELSE that relied on the old quirk now trips.
