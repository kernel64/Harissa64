# Progress

## Current milestone: M5 — Platform and features (next)

## M4 — HLE and the Xenos renderer (complete, 2026-10-07)

Final console test (RGH, PAL games, 50 VI/s target), share of 2-second periods at 49 VI/s or more:

| Game | 1 core | 2 cores (graphics worker) | 3 cores (+ audio worker) |
|---|---|---|---|
| SM64 | 94 % | - | 92 % |
| MK64 | 89 % | - | 89 % |
| OoT | 76 % | 82 % | 87 % |

All three are playable with sound on 3 cores. In OoT's heaviest scenes (36–47 VI/s) the CPU thread runs ~13 ms a frame and then waits 8–11 ms for the graphics worker: the game waits for its display list, so the worker barely overlaps; the graphics time there is mostly texture lookups (3–6 ms: TMEM hashing on every memo miss) and the RDP state (1.5–2 ms). Audio: 1–7 underruns per 2 s in those scenes only (rate control at 0.93–0.95).

| Part | Status |
|---|---|
| Audio task HLE (port of mupen64plus-rsp-hle) | Done (`--hle-audio`); against LLE with `--hle-audio-check`: MK64 within 16 LSB, OoT mostly 1–16 LSB (FILTER larger, upstream), SM64 −38 dB on the output |
| Graphics task HLE: F3D, F3DEX, F3DEX2, F3DZEX | Done (`--hle-gfx`); other microcodes and G_LOAD_UCODE to them fall back to LLE per task |
| `render/api.h`; software RDP draws HLE triangles (`h64_rdp_build_triangle`, libdragon's setup) | Done |
| Xenos renderer: combiner → HLSL, TMEM-exact textures, blender, depth, alpha compare, rectangles | Done |
| LLE graphics through Xenos (raw RDP triangles rebuilt by `h64_rdp_decode_triangle`) | Done: SM64 logo with `hle=0` in Xenia |
| Framebuffer emulation: copy back to RDRAM before the RDP reads a frame as a texture; new colour images start from RDRAM | Done (not yet seen exercised by the three games) |
| Xbox front end: ROM, recompiler/interpreter, HLE, XAudio2, XInput, ini, scripted input, screenshots, crash handler | Done |
| S2DEX2 HLE | G_BG_COPY (OoT's prerendered rooms): done; BG_1CYC and the object commands still go to LLE |
| VI filters on Xenos | The three games use VI control 0x3016 (AA + resample, divot, gamma dither, no gamma): edge smoothing (FXAA) when the frame is shown stands for the coverage AA Xenos cannot do (ini `smooth=0` to compare) |
| Xbox audio | Rate control (0.90–1.02) toward a 100 ms queue and re-buffering after an underrun, as V1 |

### Console speed (RGH, PAL, 50 VI/s target), 2026-10-07

Measured with the `[prof]`/`[jit]`/`[xprof]` lines. The steps that mattered:
- Native FPU arithmetic (MK64 race CPU 110 -> 55 ms/frame with `jitfpu=0` as the reference).
- Texture windows for OoT's backgrounds (10.5M -> ~5k texels decoded a frame: 890 ms -> 2 ms).
- Block linking and a dispatcher without the H64Cpu copy: ~23k -> ~2.7k dispatches a frame on MK64.
- BC1, DIV, MFC0, odd-register COP1 moves native; 64-byte code map (interpreted instructions per frame: MK64 ~2000 -> ~350, OoT ~13000 -> ~1000).

Result before S2DEX2 HLE: MK64 menus and most of the race at 49–52 VI/s (race CPU ~12 ms, rendering 4–7 ms); OoT at full speed in many places, 35–45 VI/s outdoors (CPU 13–17 ms, graphics HLE 5–10 ms), 26 VI/s in prerendered rooms (S2DEX task in LLE: ~30 ms).

Then:
- S2DEX2 G_BG_COPY in HLE: OoT's prerendered rooms at full speed (RSP LLE 0 ms).
- The console measured ~700 cycles for a jump to far code (`[jit] code locality`): generated code was made compact (shared entry/exit, load/store and FPU checks as shared routines, slow paths in a separate cold region through a shared call, direct use of cached registers): 148 -> 97 bytes per MIPS instruction, 66 -> 50 in the hot part. OoT outdoors CPU 13–16 -> ~12 ms.
- GPR caching in host registers: correct (lockstep) but no measurable gain on its own.
- `[cprof]`: the interpreter helper (~0.7 ms) and scheduler events (~0.1 ms) are small; the rest of `cpu` is generated code.

Known issues: OoT's Kokiri paths flicker on Xenos (parked until save states make the scene reachable here); Paper Mario hangs after a while, Mario Party 3 at boot (EEPROM: M5).

### Checks in Xenia (interpreter, RSP HLE, Xenos), 2026-10-06

- SM64: logo (compared with the software RDP at the same VI: same geometry and animation frame, mean difference 3.6 per channel, differences on edges: VI anti-aliasing and 320x240 upscale vs native 640x480), title screen, gameplay in the castle grounds with the input script of `run_games.py`.
- MK64: title screen, race on Luigi Raceway with the scenario's input script.
- OoT: title screen over Hyrule field.
- Speed 5–6 VI/s (Xenia cannot run the recompiler); audio buffers queued to XAudio2.

## M3 — Dynarec (complete, console check pending)

- Lockstep with the interpreter under QEMU ppc64, no divergence: SM64 68 s, MK64 43 s, OoT 76 s (with inputs); n64-systemtest full lockstep clean; Dillonb 26/26.
- Speed under QEMU: SM64 6 s with HLE, interpreter 83 s, dynarec 17.6 s (4.7×).

## M2 — RSP LLE, software RDP, VI → first images (complete)

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
