# Lisa_Source — Complete Reference Map

## Origin

The Apple Lisa source code was released by Apple in 2018 in partnership with the Computer History Museum.

**Source**: https://info.computerhistory.org/apple-lisa-code

**License**: Apple Academic License Agreement — permits use, reproduction, compilation and modification for non-commercial, academic research, educational teaching, and personal study purposes only. **Redistribution is prohibited.** See `Lisa_Source/Apple Academic License Agreement.txt`.

**Version**: Lisa OS 3.1 (Office System), circa 1983-1984

---

## Overview

| Category | Directories | Files | Description |
|----------|------------|-------|-------------|
| Operating System | LISA_OS/OS | 121 | Kernel, drivers, memory manager, file system, scheduler |
| System Libraries | LISA_OS/LIBS | 174 | 21 libraries (graphics, windows, fonts, I/O, printing, etc.) |
| Applications | APPS | 272 | 13 office applications (word processor, spreadsheet, etc.) |
| Fonts | LISA_OS/FONTS | 57 | Binary bitmap font files (.F) |
| Build System | LISA_OS/BUILD + OS exec files | 63 | Compiler/linker scripts, linkmaps, release packaging |
| Installation | LISA_OS/APIN | 21 | Office System diskette builder |
| Interface Manager | LISA_OS/GUIDE_APIM | 31 | Scripting/automation system for Office apps |
| Toolkit | Lisa_Toolkit | 234 | Application development framework (TK1-TK5) |
| Dictionary | DICT | 6 | Spell checker word lists and tools |
| Linkmaps | LISA_OS/Linkmaps* | 24 | Linker output showing segment layout of each binary |
| **Total** | | **~1,280** | |

## Languages

All source files have the extension pattern `*.TEXT.unix.txt` (converted from original Lisa filesystem format).

- **Lisa Pascal** — Apple's custom Pascal dialect with units, compiler directives (`{$U}`, `{$IFC}`, `{$R-}`), and Lisa-specific extensions. Used for all high-level OS code, libraries, and applications.
- **Motorola 68000 Assembly** — Used for hardware drivers, interrupt handlers, memory management primitives, and performance-critical paths. Identifiable by `.func`, `.proc`, `.ref`, `equ`, `move.l`, etc.

## Pre-compiled Binary Artifacts

The archive includes 8 compiled 68000 object files and 57 font files:

### Object Files (.OBJ)
| File | Location | Size | Description |
|------|----------|------|-------------|
| IconEdit.OBJ | Lisa_Toolkit/TK3 | 11 KB | Icon editor tool |
| TK-ALERT.OBJ | Lisa_Toolkit/TK3 | 18 KB | Toolkit alert dialog |
| TK-NullChange.OBJ | Lisa_Toolkit/TK3 | 2 KB | Null change handler |
| TK-WorkDir.OBJ | Lisa_Toolkit/TK3 | 1 KB | Working directory utility |
| UFIXUTEXT.OBJ | Lisa_Toolkit/TK5 | 5.5 KB | Unicode text fix |
| LIBPL-PASLIBCALL.OBJ | Lisa_Toolkit/TK Sources 4 | 2.5 KB | Pascal library call |
| LIBPL-PPASLIBC.OBJ | Lisa_Toolkit/TK Sources 4 | 2.5 KB | Pascal library C interface |
| libtk-passwd.OBJ | Lisa_Toolkit/TK Sources 4 | 1.5 KB | Password utility |

### Font Files (.F)
57 binary bitmap fonts in `LISA_OS/FONTS/`, including:
- Century (12, 14, 18, 24pt variants)
- Tile (7, 12, 20pt variants)
- Helvetica (14, 18pt)
- Apple Wide, Calculator, Toolkit, Window Manager fonts
- Total: ~263 KB

---

## Directory-by-Directory Breakdown

### APPS/ — Lisa Office Applications

| Code | Application | Files | Description |
|------|-------------|-------|-------------|
| APBG | LisaGraph | 8 | 2D Graphics / Business Graphics |
| APCL | Clock | 3 | System clock with timer |
| APDM | Desktop Manager (Filer) | 14 | File/volume/icon management, desktop |
| APEW | Selector Shell | 1 | System startup shell (Twiggy version) |
| APHP | HP Calculator | 2 | Hewlett-Packard style calculator |
| APIN | Installation Shell | 21 | Office System master diskette installer |
| APLC | LisaCalc | 52 | Spreadsheet with formula engine |
| APLD | LisaDraw | 37 | Vector drawing application |
| APLL | LisaList | 36 | Database/list manager with forms and queries |
| APLP | LisaProject | 18 | Project planning with Gantt/Task charts |
| APLT | LisaTerminal | 23 | Terminal emulation |
| APLW | LisaWrite | 42 | Word processor with formatting and spell check |
| APPW | Preferences | 14 | System preferences and print configuration |

### LISA_OS/OS/ — Operating System Kernel (121 files)

#### Memory Management
| File(s) | Description |
|---------|-------------|
| MM0, MM1, MM2, MM3, MM4 | Memory manager modules (segmented) |
| MMPRIM, MMPRIM2 | Memory management primitives |
| MMASM | Memory manager assembly routines |
| PMEM | Physical memory management |
| PMMAKE | Memory manager initialization |
| VMSTUFF | Virtual memory support |

#### File System
| File(s) | Description |
|---------|-------------|
| FSINIT, FSINIT1, FSINIT2 | File system initialization |
| FSDIR, FSDIRDEBUG | Directory operations and debug |
| FSPRIM | File system primitives |
| FSUI, FSUI1, FSUI2, FSUI3 | File system user interface layers |
| FSASM | File system assembly |
| SFILEIO, SFILEIO1, SFILEIO2 | Sequential/structured file I/O |
| OBJIO | Object I/O |

#### Disk Drivers
| File(s) | Description |
|---------|-------------|
| SONY, SONYASM | Sony 3.5" floppy controller |
| TWIGGY, TWIG, LDTWIG | Twiggy floppy drive (Apple's proprietary) |
| HDISK | Generic hard disk driver |
| PRIAM, PRIAMASM, PRIAMCARD, PRIAMCARDASM | Priam hard disk interface |
| 2PORTCARD | Dual-port card controller |
| PROFILE, PROFILEASM | ProFile hard disk (Apple's 5/10MB drive) |
| ARCHIVE, ARCHIVEASM | Archive streaming tape backup |
| DRIVERASM, DRIVERDEFS, DRIVERMAIN, DRIVERSUBS | Generic driver infrastructure |
| DEVCONTROL | Device control subsystem |

#### Process Management
| File(s) | Description |
|---------|-------------|
| SCHED | Process scheduler |
| PROCMGMT | Process management |
| PROCPRIMS, PROCASM | Process primitives and assembly |
| PMCNTRL, PMSPROCS, PMTERM | Process manager control, procedures, termination |

#### Exception & Interrupt Handling
| File(s) | Description |
|---------|-------------|
| EXCEPMGR, EXCEPRIM | Exception manager and primitives |
| EXCEPASM, EXCEPNR1, EXCEPRES | Exception assembly and non-resident handlers |
| EVENTCHN | Event channel IPC |
| NMIHANDLER | Non-maskable interrupt handler |
| INITRAP | Trap initialization |

#### Loader
| File(s) | Description |
|---------|-------------|
| LOADER, LOAD, LOAD1, LOAD2 | Program loader (segments, overlays) |
| LDASM, LDUTIL, LDPROF | Loader assembly, utilities, profiling |
| LDMICRO, LDLFS, LDPRAM | Loader variants for microcode, LFS, PRAM |
| UNPACK | Object file unpacker |

#### System Services
| File(s) | Description |
|---------|-------------|
| STARTUP | System initialization and boot |
| STARASM1, STARASM2, STARASM3 | Startup assembly (3 stages) |
| SYSGLOBAL, SYSG1 | Global system data definitions |
| GDATALIST | Global data structure list |
| SYSCALL, PSYSCALL | System call interface |
| TIMEMGR | Timer/alarm manager |
| CLOCK | Real-time clock driver |
| CONSOLE, CONSOLEASM | Console I/O |

#### Communications
| File(s) | Description |
|---------|-------------|
| RS232, RSASM | RS-232 serial port driver |
| SERCARD, SERNUM | Serial card and serial number |
| MODEMA, MODEMASM | Modem driver |
| PARALLELCABLE | Parallel cable driver |
| ASYNCTR | Asynchronous transfer |

#### Utilities
| File(s) | Description |
|---------|-------------|
| SCAVENGER | Disk repair/recovery tool |
| VOLCHK | Volume integrity checker |
| MEASURE | Performance measurement |
| GENIO | Generic I/O abstraction |
| MOVER | Block memory mover |
| NWSHELL | Network shell |
| PASCALDEFS, PASLIBDEFS | Pascal language definitions for assembly |
| OSLIB, OSINTPASLIB, POSLIB | OS library interfaces |
| PASMATH | Pascal math routines |

### LISA_OS/LIBS/ — System Libraries (21 modules)

| Library | Full Name | Files | Key Components |
|---------|-----------|-------|----------------|
| LIBAM | Alert Manager | 4 | User dialogs, alerts, notifications |
| LIBDB | Database | 8 | B-tree, heap, scan, pooler |
| LIBFC | Filer Communications | 6 | File browser/finder IPC |
| LIBFE | Field Editor | 2 | Form field input with validation |
| LIBFM | Font Manager | 8 | Typography (FontMgr, heuristics, volumes) |
| LIBFP | Floating Point | 8 | IEEE math (FPLib, MathLib, SANE) |
| LIBHW | Hardware Interface | 14 | **Critical for emulation**: Drivers, Keyboard, Mouse, Cursor, Timers, Machine control, NMI, Speaker, Video |
| LIBIN | Internationalization | 3 | Character sets, locale support |
| LIBOS | OS Interface | 14 | SysCall, storage, volume, path operations |
| LIBPL | Pascal Library | 13 | Pascal runtime (BlockIO, abort, library calls) |
| LIBPM | Print Manager | 5 | Print job management (PMDecl, PMM) |
| LIBPR | Print Drivers | 14 | Printer interface, buffers, queues, dialogs |
| LIBQD | QuickDraw | 13 | **Graphics engine**: primitives, regions, text, 3D, bitmap ops |
| LIBQP | QuickPort | 18 | Terminal emulation (VT100, Soroc), serial comm, baud rate |
| LIBSB | Standard Boxes | 5 | UI widgets (scroll bars, grow boxes, cursors) |
| LIBSM | Standard Memory | 8 | Heap/heap zone manager (UnitStd, UnitHz) |
| LIBSU | Standard Utilities | 11 | Clipboard, file utils, formatting, atoms |
| LIBTE | Table Editor | 8 | Spreadsheet cell editing, cut/paste |
| LIBTK | Toolkit | 12 | UI framework (UObject, UDialog, UText, UDraw, Clascal) |
| LIBUT | Universal Text | 4 | Cross-app text handling |
| LIBWM | Window Manager | 5 | Windows, menus, events, folders |

### LISA_OS/GUIDE_APIM/ — Application Interface Manager (31 files)

Scripting and automation system for Office applications:
- Core interpreter (`imcore`, `iminterp`, `imscript`)
- Event loop (`imevtloop`)
- Folder/document handling (`imfolders`)
- Menu system (`immenus`)
- Script editor (`imsedit`)
- File I/O (`imstream`, `imft.save`, `imft.load`)
- Pattern matching (`impatmat`)
- Tutorial menus and filer interface
- Alert dialogs

### LISA_OS/TKIN/ — Toolkit Input (2 files)

Minimal entry point module for toolkit infrastructure integration.

### LISA_OS/APIN/ — Installation Scripts (21 files)

Orchestrates building and packaging the Office System master diskettes:
- `APIN-BUILD` — Master build script
- `APIN-MAKE-DISK1` through `APIN-MAKE-OFFICE5` — Creates 5-disk set
- `APIN-COMP-*` — Compilation control
- `APIN-LINK-*` — Linking control
- `APIN-PACK-*` — Packaging
- `APIN-INSTALL-SYSLIB` — System library installation
- `APIN-BACKUP`, `APIN-DONE` — Backup and verification

### Lisa_Toolkit/ — Development Framework

| Directory | Files | Description |
|-----------|-------|-------------|
| TK Sources 1 | 12 | Toolkit source code v1 |
| TK Sources 2 | 12 | Toolkit source code v2 |
| TK Sources 3 | 12 | Toolkit source code v3 |
| TK Sources 4 | 33 | Extended toolkit sources v4 (includes .OBJ files) |
| TK3 | 54 | Toolkit 3: dialogs, boxing utilities, IconEdit.OBJ |
| TK4 | 62 | Toolkit 4: extended components |
| TK5 | 49 | Toolkit 5: latest revision |

### DICT/ — Dictionary (6 files)

Spell checker support: word lists, replacement tables, shuffle encoding, format conversion.

### LISA_OS/Linkmaps 3.0/ — Linker Output Maps

Detailed segment layout for each linked binary:
- `linkmap-lisacalc`, `linkmap-lisadraw`, `linkmap-lisawrite`, `linkmap-lisalist`
- `linkmap-lisaproject`, `linkmap-lisaterminal`
- `linkmap-filer`, `linkmap-calculator`, `linkmap-clock`, `linkmap-preferences`
- Library linkmaps: `linkmap-sys1lib`, `linkmap-sys2lib`, `linkmap-iosfplib`

---

## Build Process

The complete build pipeline (from source analysis of BUILD-*.TEXT scripts):

```
Lisa Pascal Source (.TEXT)     68000 Assembly Source (.TEXT)
         |                              |
    P{ascal Compile}              A{ssemble}
         |                              |
         v                              v
    Object Files (.OBJ)         Object Files (.OBJ)
         |                              |
         +----------+------------------+
                    |
              IUManager (Intrinsic Unit Manager)
                    |
                    v
           intrinsic.lib (Pascal runtime + system libraries)
                    |
              L{ink} (Linker)
                    |
                    v
         Executables (.os, .shell, app binaries)
                    |
         BUILD-NEWRELEASE (copy to disk)
                    |
                    v
         Bootable Lisa OS Disk Image
```

### Build Tools Required

| Tool | Purpose | Notes |
|------|---------|-------|
| Pascal Compiler | Compiles Lisa Pascal units | Uses `-newdisk-intrinsic.lib` |
| Assembler | Assembles 68000 source | Assigns segment names |
| Code Generator | Produces final object code | Intermediate step after compile |
| IUManager | Manages intrinsic library | Installs/removes units |
| Linker | Links objects into executables | Uses linklist files |
| ChangeSeg | Reassigns segment names | Post-processing tool |

### Build Order

1. **iospaslib** — I/O and Pascal runtime library
2. **iosfplib** — Floating point library (196 modules, 40 segments)
3. **sys1lib** — System Library 1: QuickDraw, Window Manager, Font Manager, etc. (1069 modules, 23 segments, 854 entries)
4. **sys2lib** — System Library 2: additional system support
5. **prlib** — Printing library
6. **tklib** — Toolkit library
7. **system.os** — OS kernel (46 object files linked, 871 modules, 25 segments, 1095 entries)
8. **Applications** — Each app linked against system libraries (e.g., Filer = 12 files, LisaDraw = 25 files)
9. **Release packaging** — Copy all binaries + configs + boot code to disk image

### OS Kernel Composition (46 linked objects)

```
Startup: startup, starasm1-3
Memory:  mm0, mmasm, mmprim, pmem, vmstuff
Process: sched, procmgmt, procprims, procasm
Except:  excepmgr, exceprim, excepasm, nmihandler, initrap, eventchn
Time:    timemgr, clock
FileSystem: fsinit, fsdir, fsprim, fsui, fsasm, sfileio, objio
Drivers: hdisk, twiggy, genio, asynctr
Loader:  load
Config:  cd, cdconfigasm, sysglobal
I/O:     osunitio, genio, mover
System:  startup, measure, scavenger, volcheck, pasmath, osintpaslib
Hardware: hwintl, dbgasm
```

### Final Disk Contents

```
system.os          — Linked OS kernel
system.shell       — Environment shell
system.debug       — Debug symbols
system.lld         — Link load data
intrinsic.lib      — Runtime libraries
iospaslib.obj      — I/O Pascal library
psyscall.obj       — Protected system calls
syscall.obj        — System call interface
cdconfig           — Device configuration
cd_*, ch_*         — 9 device driver configs each (Serial, Priam, Archive,
                     Sony, Profile, 2-Port, Parallel, Console, Modem)
system.bt_Profile  — ProFile boot code
system.bt_Sony     — Sony floppy boot code
system.bt_Twig     — Twiggy boot code
system.bt_Priam    — Priam boot code
devcontrol         — Device control
[Application binaries — Filer, LisaCalc, LisaDraw, LisaWrite, etc.]
[Font files — 57 .F files]
```

---

## Key Source Files for Emulator Development

These files define the hardware interface that the emulator must replicate:

| File | Location | What it Defines |
|------|----------|-----------------|
| DRIVERS.TEXT | LIBHW | VIA initialization, interrupt vectors, I/O port access |
| HWIEQU.TEXT | LIBHW | Trap #5 dispatch table (90+ hardware functions) |
| HWINT.TEXT | LIBHW | Pascal interface to all hardware |
| KEYBOARD.TEXT | LIBHW | Key scanning, event queue, ASCII translation tables |
| MOUSE.TEXT | LIBHW | Mouse tracking, delta computation, scaling |
| CURSOR.TEXT | LIBHW | Hardware cursor management |
| TIMERS.TEXT | LIBHW | VIA timer programming, millisecond/microsecond counters |
| MACHINE.TEXT | LIBHW | Video page flip, contrast, volume, soft power |
| SYSGLOBAL.TEXT | OS | System data structures, constants, type definitions |
| STARTUP.TEXT | OS | Boot sequence, MMU setup, domain configuration |
| DRIVERDEFS.TEXT | OS | All hardware constants and driver data structures |
| STARASM1-3.TEXT | OS | Low-level startup assembly |
