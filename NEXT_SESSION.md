# LisaEmu — Status (April 5, 2026)

## Quick Commands

```bash
make audit              # Full toolchain report
make audit-linker       # Just linker (fastest)
```

## MAJOR MILESTONE: OS kernel boots and executes real code!

The CPU now:
1. ROM → $400 (STARTUP) ✅
2. STARTUP → INITSYS ✅  
3. INITSYS runs, calls INTSOFF, processes data ✅
4. Hits TRAP #1 at $04DCB4 — first OS system call! ✅
5. Crashes because TRAP vectors not installed yet ❌

system.os is 331KB (was 2.8MB), fits in 1MB RAM.

## Next: Fix TRAP vector installation

INITSYS calls INIT_TRAPV to install proper exception vectors. Either:
- INIT_TRAPV isn't being called before the first TRAP
- INIT_TRAPV runs but writes wrong values
- The codegen for INIT_TRAPV call is wrong

Check: does INITSYS call INIT_TRAPV before calling INTSOFF?
The STARTUP source calls INIT_TRAPV(b_sysglobal_ptr) early in INITSYS.
b_sysglobal_ptr is a SYSGLOBAL variable — needs correct A5-relative offset.

## Session Summary

### Critical fixes this session:
1. **CONST resolution in STRING[n]** — e_name was 1.6GB instead of 33 bytes
2. **Module index fix after STARTUP swap** — ALL cross-module calls had wrong addresses
3. **Kernel separation** — only 46 kernel modules linked into system.os (was ALL 428)
4. **PC ring buffer trace** — catches any Line-F crash with last 20 PCs

### Toolchain metrics:
- Parser: 99.5% real code
- Assembler: 100%
- Linker: 97.2% JSR resolution
- system.os: 331KB (fits in 1MB RAM)
