# Harissa64 V2 🌶️

**A Nintendo 64 emulator for the Xbox 360, written from scratch — work in progress.**

This branch (`v2`) is a complete rewrite of [Harissa64](https://github.com/kernel64/Harissa64). The current, playable emulator (V1, a port of Mupen64-360 with the Rice video plugin) lives on the `master` branch and keeps shipping until V2 is better on real hardware.

> **For educational purposes.** Harissa64 is a homebrew project made to learn about emulation and Xbox 360 development. Use it only with ROMs dumped from N64 cartridges you own. No ROMs are provided, and none will be.

## Goals

- A portable, testable core (CPU, RSP, RDP, VI/AI/PI/SI, PIF, saves) in plain C++, built both with the Xbox 360 SDK and with modern host compilers.
- Validation without a console: a headless runner (`h64test`), public N64 test ROMs, and a big-endian PowerPC build run under QEMU.
- A thin Xbox 360 layer (Xenos renderer, XAudio2, XInput, front end).
- Then speed: a MIPS→PowerPC recompiler and the console's three cores.

## Status

Milestone M0 (environment and foundations). See [`docs/PROGRESS.md`](docs/PROGRESS.md). Nothing is playable yet: use V1 from `master` to play.

## Building

See [`CLAUDE.md`](CLAUDE.md) for the exact commands. In short:

- **Host** (Windows, Visual Studio 2026 + CMake/Ninja, or WSL gcc): `tests/scripts/run_all.ps1` builds and runs the unit tests.
- **Big-endian** (WSL, `powerpc64-linux-gnu-g++` + `qemu-ppc64`): included in `run_all.ps1`.
- **Xbox 360** (Visual Studio 2010 + Xbox 360 SDK): `platform/xbox360/harissa64v2.sln`, platform `Xbox 360`.

## Licence

GPL v2, like V1. See [`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md) for the origin of any third-party code.
