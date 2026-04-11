# LisaEmu

A native macOS emulator for the Apple Lisa. Takes Apple's officially released
Lisa OS 3.1 source code (released 2018 via the Computer History Museum) and
compiles it into a bootable system, then runs it on an emulated Motorola 68000
+ Lisa hardware stack.

Two layers:

1. **Toolchain** — 68000 cross-assembler + Lisa Pascal cross-compiler + linker
   that processes `Lisa_Source/` into bootable disk images and ROM.
2. **Emulator** — 68000 CPU core + Lisa hardware (MMU, dual VIA 6522, video,
   keyboard, mouse, ProFile hard disk, floppy).

`Lisa_Source/` is NOT included — Apple's Academic License permits use but
prohibits redistribution. You must supply your own copy.

## Build & Run

```bash
# Standalone SDL2 build
make
build/lisaemu --image prebuilt/los_compilation_base.image

# Toolchain audit (primary diagnostic)
make audit

# Xcode macOS app
cd lisaOS && xcodebuild -scheme lisaOS -destination 'generic/platform=macOS' build
```

## Layout

See `CLAUDE.md` for the full directory map, hardware specs, and current status.
See `docs/` for Lisa_Source reference material and hardware documentation.
