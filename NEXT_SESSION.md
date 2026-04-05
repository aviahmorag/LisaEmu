# LisaEmu — Next Session Plan (April 5, 2026)

## Quick Start

```bash
make audit              # Full toolchain report (all 4 stages)
make audit-linker       # Just linker stage
```

## Current State: OS kernel boots, hits first TRAP syscall

The CPU executes this path successfully:
```
ROM ($FE0400) → JMP $400 → STARTUP → BRA $4CE0 → INITSYS ($4B44)
→ LINK A6 → load globals → call INTSOFF ($4DC9C) → ... → TRAP #1 ($04DCB4)
```

Then crashes because TRAP vectors are still $FE0300 (ROM default RTE handler).

### Runtime logs from last run:
```
Pre-loaded system.os: 663 blocks (339456 bytes) at RAM $0
INTSOFF → $4DC9C (correct kernel address)
TRAP1 target=$00050CDE (linker patched correctly)
Exception: vector 39 (TRAP) at PC=$04DCB4, new PC=$00FE0300
Line-F at $200002 (ran off end of 1MB RAM after bad RTE return)
A6=$00078EF2 (real frame pointer), D3=$0000302D (computed value)
```

## Immediate Task: Fix TRAP Vector Installation

### The problem
INIT_TRAPV should install proper exception handlers into RAM $0-$3FF.
The linker resolves TRAP1 to $00050CDE (correct). But the vectors in
RAM stay at $FE0300 (ROM defaults set by emulator during boot).

### What to investigate
1. **Does INITSYS call INIT_TRAPV before INTSOFF?**
   - Source: `Lisa_Source/LISA_OS/OS/SOURCE-STARTUP.TEXT.unix.txt` line ~2150
   - Original order: `INIT_TRAPV(b_sysglobal_ptr)` is called early
   - But our codegen may not emit this call, or may call it after INTSOFF

2. **Is b_sysglobal_ptr resolving to a real value?**
   - It's a SYSGLOBAL variable (imported via shared globals)
   - If it resolves to 0, INIT_TRAPV writes vectors to address 0 with offset 0
   - Check: what offset does b_sysglobal_ptr have in the shared globals table?

3. **Does INIT_TRAPV actually run?**
   - INIT_TRAPV is assembly code in SOURCE-INITRAP.TEXT
   - Its address in the kernel: look for it in linker output
   - Add a PC breakpoint trace in m68k.c for the INIT_TRAPV address range

4. **Is the INITRAP assembly code correct?**
   - 19 relocations for TRAP handler addresses (TRAP1, SCHDTRAP, BUS_ERR, etc.)
   - All resolve to kernel addresses in the linker
   - The assembly uses LEA TRAP1,A0 then MOVE.L A0,(A1)+ to install vectors

### Key files to read
- `src/toolchain/toolchain_bridge.c` — compile/link pipeline, is_kernel_module()
- `src/toolchain/linker.c` — linker with module_idx fixup, name mapping table
- `src/toolchain/pascal_codegen.c` — code generation, CONST resolution
- `src/m68k.c` line ~2596 — PC ring buffer trace (catches any Line-F in code range)
- `src/lisa.c` line ~534 — system.os loader (reads from ProFile disk image)
- `src/toolchain/bootrom.c` — boot ROM that jumps to $400

## Toolchain Metrics

```
Parser:    405 Pascal files, 360 OK (99.5% real code, 2 edge cases)
Assembler: 105 files, 100% success, 0 errors
Codegen:   93.2% symbol resolution (rest resolve at link time)
Linker:    97.2% JSR to real code, 108 stub relocations (39 truly missing symbols)
Output:    331KB system.os (kernel only, fits in 1MB RAM)
```

## Architecture

### Lisa OS structure (now implemented correctly)
- **system.os** = 46 kernel modules from BUILD-LINKLIST.TEXT (~331KB)
- **intrinsic.lib** = libraries (not yet built — LIBQD, LIBWM, LIBFP, etc.)
- **Applications** = separate executables (not yet built)

### Toolchain pipeline
1. Find source files → detect Pascal vs Assembly via content probe
2. Compile all Pascal files (STARTUP last, sees all shared globals/types)
3. Assemble all assembly files
4. Tag kernel vs library modules via is_kernel_module()
5. Move STARTUP to module[0], fix all symbol table module_idx references
6. Link: layout kernel modules at $400+, resolve symbols, apply relocations
7. Non-kernel modules contribute symbols but excluded from output binary
8. Write system.os to ProFile disk image, generate boot ROM

### Key fixes applied this session
- **CONST resolution**: STRING[max_ename] was getting size 1.6GB → fixed to 33 bytes
- **Module index fixup**: STARTUP swap invalidated ALL symbol addresses → fixed
- **Kernel separation**: Only 46 OS modules in system.os (was all 428)
- **.DEF/.REF flag fixes**: Assembly symbols stuck as EXTERN → fixed
- **{$I filename}**: Pascal source includes now work (155/158 resolve)
- **{$SETC} AND/OR/NOT**: Conditional compilation boolean expressions
- **Type cast inline**: TYPE names no longer emit JSR
- **Procedure parameter dispatch**: Indirect call via JSR (A0)
- **Linker name mapping**: InClass→%_InObCN, HALT→%_HALT, FP→%f_*, etc.
- **Strict symbol add**: 8-char prefix match no longer destroys symbols during add

### Remaining toolchain issues (not blocking boot)
- 39 truly missing symbols (print system, SU runtime, etc.)
- Clascal vtable dispatch not implemented
- {$I} 3 files not found (Apple didn't release them)
- 2 parser edge cases (MATHLIB param syntax, LCUT conditional)
