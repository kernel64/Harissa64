# Progress

## Current milestone: M2 — RSP LLE, software RDP, VI → first images

| Part | Status |
|---|---|
| RSP interpreter (scalar + vector unit), SP registers, timed SP DMA | Done (`core/rsp`, port of ares) |
| RDP command interface (DPC registers, XBUS, SYNC_FULL interrupt) | Done (`core/rdp/h64_rdp.cpp`) |
| Software RDP: rasteriser, TMEM, textures (all formats, TLUT, YUV, LOD), combiner, blender, depth, dither, coverage, framebuffer formats | Done (`core/rdp`, port of ParaLLEl-RDP's shaders; TMEM loads written from the hardware description) |
| TMEM unit tests (RGBA16/32, IA/I 4/8, CI4/CI8 + TLUT RGBA16/IA16, LoadBlock with dxt, wrap/mirror/clamp/shift, YUV, odd-line swap) | Done (`tests/unit/test_rdp_tmem.cpp`, 7 tests) |
| VI stage (AA, dither filter, divot, scaling, gamma) | Done (`core/vi`, port of ParaLLEl-RDP's VI); per-scanline register changes and the fetch bug are not emulated |
| AI → WAV, controller input script, timed screenshots | Done (h64test `--wav`, `--input`, `--shot`) |
| Game scenarios with expected hashes | Done (`tests/scripts/run_games.py`, `tests/expected/games.txt`) |

### Test ROM scores (2026-10-06)

| Suite | M1 | M2 |
|---|---|---|
| n64-systemtest (base) | 3407 / 3721 | **3706 / 3721** — every RSP and RDP test passes; left: 9 caches (not emulated), 6 reverse-endian user mode (unused by games) |
| Dillonb n64-tests | 26 / 26 | 26 / 26 |
| PeterLemon CPUTest | 93 / 94 | 93 / 94 |
| PeterLemon RSPTest | — | 50 / 56 (6 reserved opcodes: undocumented accumulator results, ares has the same) |
| PeterLemon RDPTest | — | 2 / 2 |

The SM64 title frame is bit-identical on MSVC, gcc (little-endian) and ppc64 big-endian under QEMU.

### Games (h64test, reference path: RSP LLE + software RDP + VI)

| Game | Result |
|---|---|
| Super Mario 64 | Intro, title (animated head), file select, Peach's letter, Lakitu's fly-over, **Mario out of the pipe, controllable** in the castle grounds (HUD, dialogue). Sound: music and voices in the WAV. |
| Mario Kart 64 | Nintendo logo, title, game/player/map select, **race on Luigi Raceway** (Mario accelerating, 1st place, timer). |
| Ocarina of Time (Master Quest) | Intro on Hyrule field, title, file select, name entry, file created and opened, **in-engine intro cutscene** (the Deku Tree, Ganondorf's nightmare, Navi). **Open issue**: the cutscene stops on the box "It seems the time has come for the boy without a fairy to begin his journey..." although the A presses reach the game (controller reads logged) and audio and DMAs keep running; the CPU sits in the idle thread. To investigate with better tools (savestates, M5). |

The six scenarios give the same image and sound hashes on MSVC and gcc; the two title scenarios were also run on ppc64 big-endian under QEMU, with the same hashes. Speed on the host (reference path, MSVC Release, i7-11800H): about 0.4× real time. Scenarios and their hashes: `tests/scripts/run_games.py`, `tests/expected/games.txt` (hashes only: game images and sound are copyrighted).

## M1 — CPU, memory, boot (complete)

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

Optional: `platform\xbox360\Release\harissa64v2.xex` still only runs the unit tests at start-up (the emulator itself does not run on the console before M4). It should now show 12 tests / 557 checks, 0 failures: the new TMEM tests then also run on the console's big-endian CPU.

## Next

M3 — Dynarec (after the user confirms M2).
