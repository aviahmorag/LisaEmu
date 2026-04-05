# LisaEmu — Toolchain Status (April 5, 2026)

## How to Audit

```bash
make audit              # Full report (all 4 stages)
make audit-linker       # Just linker (fastest for iterating)
```

## Current Metrics

```
STAGE 4: LINKER (full pipeline) — the key metric
  Modules:     435 (338 Pascal + 97 Assembly)
  Symbols:     10,822 total
  JSR analysis:
    Total:     33,685
    Real code: 32,239 (95.7%)  ← UP FROM 88.6% AT SESSION START
    Stub:       1,265 (3.8%)
    Other:        181

  Stub breakdown (537 relocations, 130 unique symbols):
    49 FP_NOBODY:     ~300 refs — pre-compiled FP internals, no source
    33 TRULY_MISSING:  ~80 refs — not in released source (Pr*, SU*, etc.)
    48 HAS_SOURCE:    ~100 refs — source exists, linker matching issue
```

## Remaining Work

### 1. Synthesize FP internal wrappers (~300 refs)
49 FPLIB functions (xmovefp, fp%normalize, x%pot, fpintx, etc.) are declared
in INTERFACE but have no implementation body. They were pre-compiled by Apple.
These are thin wrappers around FP68K calls. Can be synthesized:
- xmovefp: move extended to FP register (MOVE to f[0])
- fp%normalize: normalize extended float
- x%pot: power of ten
- x%int: integer part
- fpintx: round to integer

### 2. Fix remaining 48 HAS_SOURCE symbols (~100 refs)
These have implementations in source but the linker can't match:
- Type names used as calls: TGraphView, WordPtr, TpInteger, PicHandle
- Assembly table refs: xDownTbl, xWeakTbl, EXTFLD, DEPFLD
- Short method names: BP(7), EP(7), DoToObject(7)
- Clascal methods: sorted, doProc, ImageProc

### 3. The 33 truly missing symbols (~80 refs)
Not in Apple's released source:
- Print system: PrSetSpool, PrMetrics, PrSysDbg, PrStartPage, PrEndPage, etc.
- Runtime: SUInit, SUAddExtension, SUErrText, SUStopExec
- Types: TXLRect, TOffsets, TPMouseEvent, ThByte
- Misc: HRule(13), VRule(11), PsCheckError(9), eoln(5), ObjInCat(4)

### 4. Phase 2 preparation: Runtime correctness
Once toolchain is stable, focus shifts to:
- TRAP vector installation
- Boot sequence verification
- Interrupt system
- Display rendering

## Fixes Applied This Session (14 total)

1. Unified audit tool (`make audit`)
2. `.PROC/.FUNC` exported to linker without `.DEF`
3. Assembler `.REF→.PROC` flag upgrade
4. `MENUS.TEXT`/`DBOX.TEXT` skip patterns removed
5. Type casts as inline + shared types + SUBCLASS registration
6. LogCall no-op
7. File sorting (units before fragments)
8. Clascal method suffix matching in linker
9. LIBFP content-based asm/Pascal split
10. `{$I filename}` include directive (+6,294 real JSRs)
11. `{$SETC}` AND/OR/NOT expression support
12. Linker name mapping (InClass→%_InObCN, HALT→%_HALT, FP→%f_*)
13. **Critical: strict exact-match for symbol ADD** (8-char prefix was destroying 915 symbols)
14. Math function mappings (SINx→%_SIN, etc.)

Session progress: 88.6% → 95.7% JSR resolution (+7.1pp)
