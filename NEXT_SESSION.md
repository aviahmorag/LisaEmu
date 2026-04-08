# LisaEmu — Next Session Plan (April 8, 2026)

## Quick Start

```bash
make
build/lisaemu Lisa_Source                                    # cross-compiled
build/lisaemu --image prebuilt/los_compilation_base.image   # pre-built
```

## CRITICAL: Both paths need the SAME hardware fix

**Pre-built:** SYSTEM.LLD executing, stuck polling $FCF801 (status register)
**Cross-compiled:** Past POOL_INIT, deep in OS init, TRAP #5/6/7 calls active

### Immediate fix: Hardware Status Register ($FCF801)

SYSTEM.LLD at PC=$207F50 reads $FCF801 in a tight loop. Returns 0.
Check `_inspiration/lisaem-master/` for exact bit pattern. One byte fix.

Also: VIA1 aliases at $FCD9xx need handling (partial address decoding).

## Session: 16 fixes, 20 commits

### Key breakthroughs:
- MMU enabled during setup mode (CRITICAL)
- Interface files compiled first → inherited params work
- All unresolved types default to 4 bytes
- SYSTEM.LLD loaded and executing (pre-built)
- Past POOL_INIT into deep OS init (cross-compiled)
