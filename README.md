# Harissa64 🌶️

**A Nintendo 64 emulator for the Xbox 360, written from scratch — work in progress.**

Harissa64 runs as a native `.xex` on modded consoles (RGH/JTAG), launched from Aurora, FreeStyle Dash or the dashboard. It is named after harissa, the Tunisian chili paste.

> **For educational purposes.** Harissa64 is a homebrew project made to learn about emulation and Xbox 360 development. Use it only with ROMs dumped from N64 cartridges you own. No ROMs are provided, and none will be.

## Features

- **CPU**: a MIPS R4300i → PowerPC dynamic recompiler with block linking, a register cache and the host FPU, checked instruction by instruction against a reference interpreter.
- **RSP**: high-level emulation of graphics and audio tasks (F3D, F3DEX, F3DEX2, F3DZEX, S2DEX for OoT's backgrounds, and Rare's microcodes: GoldenEye, Perfect Dark, Diddy Kong Racing, Jet Force Gemini, Conker's Bad Fur Day…), with a cycle-timed low-level interpreter for everything else.
- **RDP**: drawn on the Xenos GPU (combiner and blender translated into shaders); a bit-exact software RDP serves as the reference.
- **Graphics settings**: internal resolution (native 320×240, ×2, ×3), 16:9 widescreen that widens the 3D view instead of stretching it, N64 3-point / bilinear / nearest texture filtering, FXAA, sharpening, blur, scanlines, CRT and curved CRT, LCD grid, and four presets (Original, N64 Enhanced, N64 Smooth, N64 CRT).
- **Saves**: EEPROM, SRAM, FlashRAM and Controller Pak, in a clean per-game folder; 9 save-state slots per game.
- **Controllers**: up to four players (Xbox 360 controllers 1–4 are N64 ports 1–4).
- **ROM browser**: `.z64`, `.n64`, `.v64` and zipped ROMs, with global settings and per-game profiles.

## Installing

1. Build `harissa64v2.xex` (below) and copy it to a folder on the console, for example `Hdd1:\Emulators\Harissa64\`.
2. Put your ROMs (`.z64`, `.n64`, `.v64` or `.zip`) in a `roms` folder next to it.
3. Start the `.xex` from your dashboard.

In game, a short press on **BACK** opens the menu (save/load state, settings, graphics, ROM list); holding **BACK** for 2 s returns to the dashboard.

## Building

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for the details. In short:

- **Xbox 360** (Visual Studio 2010 + Xbox 360 SDK): `platform/xbox360/harissa64v2.sln`, platform `Xbox 360`.
- **Host** (Windows with Visual Studio 2022+ and CMake/Ninja, or Linux gcc): `tests/scripts/run_all.ps1` builds the core and the headless runner `h64test`, and runs the unit tests.
- **Big-endian PowerPC** (Linux, `powerpc64-linux-gnu-g++` + `qemu-ppc64`): runs the recompiler under emulation, for checks without a console.

Progress and measurements are in [`docs/PROGRESS.md`](docs/PROGRESS.md).

## Licence

GPL v2. See [`LICENSE`](LICENSE), and [`THIRD_PARTY.md`](THIRD_PARTY.md) for the origin of any third-party code and data (ares, ParaLLEl-RDP, mupen64plus, libdragon, zlib…).

Nintendo 64 is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo.
