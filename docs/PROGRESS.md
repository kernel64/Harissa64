# Progress

## Current milestone: M1 — CPU, memory, boot (complete, waiting for the go for M2)

| Part | Status |
|---|---|
| VR4300 reference interpreter: integer, COP0, TLB, exceptions | Done (`core/r4300/h64_cpu.cpp`) |
| FPU (COP1): IEEE behaviour of the VR4300, causes, flush, half mode | Done (`core/r4300/h64_fpu.cpp`) |
| Memory map, MMIO, open bus, ISViewer | Done (`core/memory/h64_bus.cpp`) |
| PI DMA (ares block model), SI/PIF, CIC (6101–6106, 7102, 8303), MI interrupts and repeat mode | Done |
| Count/Compare, scheduler, VI/AI timing | Done |
| HLE boot | Done; DK64 needs an LLE IPL3 boot (deferred, see CLAUDE.md) |
| RSP | M1 stub: tasks are logged and completed after a fixed delay (real RSP in M2) |

### Test ROM scores (2026-10-06)

Same numbers on Windows (MSVC 2026, little-endian), Linux gcc (little-endian) and ppc64 big-endian under QEMU:

| Suite | Score | Remaining failures |
|---|---|---|
| n64-systemtest (base) | 3407 / 3721 (91.6 %) | 294 RSP/RDP (M2); 9 caches (not emulated); 6 reverse-endian user mode (Status.RE, unused by games); 5 SP tests that need the RSP (M2) |
| Dillonb n64-tests | 26 / 26 | — |
| PeterLemon CPUTest (compared with the hardware captures) | 93 / 94 | `DMAAlignment-PI-ROM-FROM`: all of the ROM's own checks pass; the capture differs on the `1->0` line (it shows the `2->0` data while the ROM expects the `0->0` CRC, which we produce, as ares does) and one register digit. The capture probably comes from another build of the test. |

Commercial ROMs (HLE boot, 5 emulated seconds, RSP stubbed): Super Mario 64 398 RSP tasks, Mario Kart 64 404, Ocarina of Time (Master Quest) 361, Star Fox 64 391, Banjo-Kazooie 134, Paper Mario 521. Donkey Kong 64 waits for a value that only the real IPL3 leaves in RDRAM.

## M0 — Environment and foundations (complete)

| Step | Status |
|---|---|
| 1. Environment discovery | Done (see CLAUDE.md, "Environment") |
| 2. XDK C++ feature probe | Done (see CLAUDE.md, "Core language subset") |
| 3. Executable memory proof of concept | **Done on the console (run 3, 2026-10-05): a dynarec is possible.** Code written into an ERW image section (`.jitc`) or into `.text` runs and can be rewritten; dynamically allocated memory is never executable. See CLAUDE.md, "Executable memory". |
| 4. Branch `v2`, skeleton, three builds, test script | Done: MSVC 2026, gcc (LE) and ppc64 (BE, qemu) pass the unit tests; the XDK `.xex` passes them in Xenia and **on the console** (2026-10-05: big-endian, 32-bit pointers, 5 tests / 97 checks, 0 failures) |

## To test on the console

Optional: `platform\xbox360\Release\harissa64v2.xex` still only runs the unit tests (the core is compiled in but not run yet). It should show 5 tests / 97 checks, 0 failures, as for M0.

## Next

M2 — RSP and RDP (after the user confirms M1).
