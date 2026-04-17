# Next Session Handoff (2026-04-18 — Phase 1 PMEM, partial)

## What happened this session

Picked up Phase 1 (PMEM layer) from the previous handoff. Two concrete wins,
one big structural bug found, one still-open blocker.

### Wins (both committed)

1. **PRAM stub now has a correct `cd_paraport` packed CD entry**
   (`src/lisa.c` `io_read_cb` at `$FCC181`). Previous stub encoded
   `pm_slot=9` (decodes to `cd_scc`) with comment saying "ProFile" — wrong.
   New stub encodes `pm_slot=11` (= `cd_paraport` per
   `source-syscall.text:80`), `pm_chan=7` (emptychan), `pm_dev=31`
   (emptydev), `driverID=34`, short form. Checksum recomputed to `0x680C`
   so `VERIFY_CKSUM` returns 0 → `pm_good := true`.

2. **Cross-assembler DBRA encoded cc=0 (DBT) instead of cc=1 (DBF)**
   (`src/toolchain/asm68k.c:1394`). On 68000, `DBT` is "decrement and
   branch while TRUE" — since the condition is always TRUE, the loop
   bails out after one iteration. Every DBRA in Apple's asm sources was
   silently running exactly once. This was the DIRECT cause of
   `INIT_READ_PM` only reading 4 bytes (one `MOVEP.L` iter) instead of
   the full 64 — which made `pm_image` mostly garbage and kept `pm_good`
   stuck at false. Fix: fold `DBRA` and `DBF` onto cc=1.

   Affected asm files (14 loops total): `source-LDASM` (4), `source-LDPROF`
   (7), `source-LDTWIG` (1), `source-STARASM2` (2). INIT_READ_PM and
   INIT_WRITE_PM are the most immediately visible.

### Verified with probes (then removed)

- `INIT_READ_PM` now iterates 16× `MOVEP.L` correctly (was 1).
- `pm_image` populated exactly matching the PRAM stub.
- `VERIFY_CKSUM` returns 0 for the valid image, 0 for the all-zero
  snap_image (falls into `snap_good := false` branch).
- `pm_good = 0x0001`, `snap_good = 0x0000` at FIND_PM_IDS entry.
- FIND_PM_IDS's `if snap_good or pm_good` branch is TRUE, so it calls
  INIT_READ_PM again to populate `param_mem.parm_mem[]` at
  `$CC0ECD` (odd! — see notes below).
- `parm_mem[0..15] = 00 04 00 01 27 33 E3 33 30 01 BE F8 22 F0 00 00`
  — the cd_paraport entry is intact at bytes 10–12.

### Still open: FIND_PM_IDS returns false despite valid input

With all of the above correct, `FIND_PM_IDS` still ends the pass loop with
`foundall = false`. INIT_BOOT_CDS takes the twiggy fallback and fires
`SYSTEM_ERROR(10738)` at line 1632 (bootdev=3 > 2).

The 10738 suppression **was NOT removed** this session — it's still the
only thing keeping boot moving past INIT_BOOT_CDS.

### Milestone regression 22/27 → 21/27

Before the DBRA fix, the boot "reached" `FS_INIT` (milestone 22) —
mostly because several `DBRA`-dependent loops were bailing early and
silently skipping work that would otherwise have caused visible
failures. Post-fix, the loops iterate correctly; downstream code
exposes real bugs and the run-away PC ends up in unmapped pages
($9E4000, $206000, ...) without reaching FS_INIT. This is the
expected "fix exposes downstream" pattern from the multilayer plan;
milestone count may bounce while phases are in flight.

---

## Phase 1 — what's left for next session

The remaining Phase 1 deliverable: **understand why FIND_PM_IDS
returns false, fix it, then remove the 10738 suppression**.

### Entry point

Read `src/toolchain/asm68k.c:1391-1403` for the DBRA fix already in
tree. Read `src/lisa.c:763-805` for the new PRAM stub.

Then reproduce and diagnose:

```
make && ./build/lisaemu --headless Lisa_Source 300 2>&1 | grep "10738"
```

You should see `SYSTEM_ERROR(10738) at ret=$004906` (i.e. INIT_BOOT_CDS
line 1632, twiggy fallback). That means `FIND_PM_IDS` returned false.

### Likely suspects (not yet probed)

1. **Packed-record field offsets inside `PMConfigEntry`**. FIND_PM_IDS
   iterates via `GetNxtConfig`, which ultimately calls `CRAK_PM` to
   decode the packed bytes. Our codegen might not be laying out
   `PMConfigEntry` the same way Apple's asm `CRAK_PM` writes into it.
   Compare the layout the Pascal compiler generated vs. what
   `source-CDCONFIGASM.TEXT` assumes (slot@0, chan@1, dev@2,
   nExtWords@3, idsize@4, pad@5, driverID@6+).

2. **MAKE_INTERNAL vs. our packed pos decoding**. CRAK_PM returns pos
   values in 1-based "external" form; MAKE_INTERNAL subtracts 1 from
   each. If our codegen doesn't handle the subrange-byte write
   correctly, pos may end up with wrong bytes.

3. **Odd alignment of param_mem**. `param_mem` lands at `$CC0ECB` (odd)
   because the PASCALDEFS pin table has `PARAM_MEM .EQU -24853`. Apple's
   asm also uses odd offsets for other sysglobal fields (Invoke_sched at
   -24786, b_syslocal_ptr at -24785, etc.). MOVE.L / MOVE.W at an odd
   address is an address-error fault on a real 68000 — our emulator
   doesn't enforce alignment so it "works", but could be causing subtle
   data corruption somewhere. Unlikely to be the primary cause (the
   dumped bytes look right) but worth a look.

4. **`nonslot` logic**. At `FIND_PM_IDS:1424`,
   `nonslot := (for_pos[slotcd].boot_slot > cd_slot3)`. For our case
   `boot_slot = cd_paraport = 11 > cd_slot3 = 3 → nonslot = true`. The
   pass loop `until foundall or nonslot or (pass = 3)` exits after pass
   1 if `nonslot`. So we only get pass 1 — which only matches when
   slot+chan+dev all line up. Our entry should match (slot=11/chan=7/
   dev=31 vs boot's slot=11/chan=7/dev=31 after pre-loop init), but if
   one field doesn't, the first pass returns false and the loop exits
   before pass 2/3 would catch it.

### Suggested first probe for next session

Add a probe inside FIND_PM_IDS right after it evaluates
`boot_slot = pos.slot`, `boot_chan = pos.chan`, `boot_dev = pos.dev`
inside the inner `for i := devcd to slotcd do with for_pos[i] do` loop.
Log all six values for each iteration of GetNxtConfig. That will pinpoint
whether our packed entry decodes correctly, and whether the match test
fires as expected.

Alternative: instrument `GetNxtConfig` entry + exit and capture
`get_it.pos.slot/chan/dev` + `driverid` returned on each call.

---

## Multilayer arc — reminder

(Copied from the prior handoff, still current — this session was pure
Phase 1 work, didn't touch other phases.)

| # | Phase | Status |
|---|---|---|
| 1 | **PMEM + boot-CD layer** | **In progress**. PRAM stub correct, pm_good true, FIND_PM_IDS still returns false. |
| 2 | ProFile driver wiring | Not started. |
| 3 | MDDF / disk-image layout | Not started. |
| 4 | IRQ-driven I/O completion | Not started. |
| 5 | SYS_PROC_INIT + processes | Not started. |
| 6 | Cleanup HLEs | Not started. |
| 7 | Safety nets | Not started. |

Durable rule: **HLEs are load-bearing layers.** Removal only in the same
commit as the replacement subsystem. See
`.claude/memory/feedback_hle_layers_load_bearing.md`.

---

## Current state at session end

- DBRA fix + PRAM stub fix committed and pushed.
- 10738 suppression **still present** — next session should remove it
  as proof of Phase 1 completion, once FIND_PM_IDS returns true.
- Milestone count: 21/27 (down 1 from start of session; expected given
  the DBRA fix exposes downstream bugs that were previously masked).
- Build green, no warnings other than the pre-existing unused-parameter
  warning in `via2_porta_write`.

## Pick-up prompt

> Continue Phase 1 of the LisaEmu multilayer-subsystem rewrite. Read
> `NEXT_SESSION.md`, `docs/HLE_INVENTORY.md`, and `CLAUDE.md` to orient.
> Rule: HLEs are load-bearing; remove only in the same commit as the
> replacement.
>
> Phase 1 PMEM is partially done: cross-assembler DBRA bug fixed, PRAM
> stub now correctly encodes a cd_paraport CD entry with valid checksum,
> `pm_good` is true after INIT_CONFIG, and FIND_PM_IDS reads the correct
> `parm_mem[]` bytes. But FIND_PM_IDS still returns false and the 10738
> suppression is still load-bearing. Your job: find why the match in
> FIND_PM_IDS's pass-1 loop doesn't fire, fix it, then remove the 10738
> suppression at `src/lisa.c:~2858`. See NEXT_SESSION.md "Phase 1 — what's
> left" for concrete probes to start with.
