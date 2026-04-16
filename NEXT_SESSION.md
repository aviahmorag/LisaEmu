# LisaEmu — Next Session Handoff (2026-04-16)

## Session summary — 16 structural codegen fixes (P80–P80f)

### Key discoveries:
1. **8-char significant identifiers** (P80): fundamental codegen fix
2. **27 record layouts corrected** (P80b): pre-pass type resolution ordering
3. **Non-local goto A6 restore** (P80e): nested proc gotos follow static link
4. **Boolean NOT for functions** (P80e): `not SYS_CALLED` used bitwise NOT.W
5. **Record field offset corruption**: full-pass `*existing = *t` zeroes offsets
6. **Generalized record repair** (P80f): auto-detect and replace corrupt records
7. **e_ldsnfree constant mismatch**: compiled code uses 3, source says 1

### Result: 25/27 kernel milestones, full boot INIT→PR_CLEANUP

## Current blocker: DS_OPEN fails during process creation

MAKE_DATASEG → DS_OPEN returns error, even with CHK_LDSN_FREE bypassed
and e_ldsnfree constant matched. The DS_OPEN failure (RECOVER error=0)
is from deep inside the segment open path. Two calls fail:
1. ret=$01A380 (syserrbase+$808) — inside DS_OPEN/OPENIT code
2. ret=$01A8D2 (MAKE_DATASEG+$398) — another error check in MAKE_DATASEG

## Root cause: constant value resolution bug

The e_ldsnfree mismatch (source=1, compiled=3) suggests our compiler
incorrectly resolves CONST values. This likely affects OTHER constants
too (e_dupds, e_nodiscspace, etc.), causing DS_OPEN's internal checks
to use wrong comparison values. This is a systemic issue that needs a
proper fix in the codegen's constant resolution.

## Next priorities

### 1. Fix constant value resolution
Check how CONST declarations in DS0's INTERFACE are imported and
resolved in DS2. The issue might be:
- Constants imported from the pre-pass get wrong values
- The 8-char matching collides two constant names
- Constants are resolved as declaration ORDER instead of explicit VALUE

### 2. Alternative: bypass MAKE_DATASEG for system segments
For disc_size=0 and ldsn<0, create a minimal memory segment directly:
- Allocate physical memory via GETSPACE
- Program MMU for the new segment
- Create a minimal SDB (segment descriptor block)
- Return seg_ptr and refnum
This avoids the complex DS_OPEN path entirely.

## Quick reference
- Build: `make`
- Run: `rm -f build/lisa_profile.image && ./build/lisaemu --headless Lisa_Source 5000`
- Commit: `d9ea218`
