# Performance

Reproducible scenes (input scripts of `tests/scripts/run_games.py`), measured on the console for real numbers and in Xenia/host for trends only. Every performance claim needs numbers here.

## V2 measurements

| Date | Where | Scene | Configuration | Result |
|---|---|---|---|---|
| 2026-10-06 | Host (MSVC Release, i7-11800H) | SM64 title, 6 s | interpreter, RSP LLE, software RDP | ~0.4× real time |
| 2026-10-06 | Host | OoT, 20 s | interpreter, LLE / HLE audio / HLE + no RDP drawing | 55 s / 52 s / 28 s (the software RDP dominates) |
| 2026-10-06 | QEMU ppc64 | SM64, 6 s | HLE, no RDP drawing: interpreter / dynarec | 83 s / 17.6 s (**4.7×**, idle-loop skipping included; indication only) |
| 2026-10-06 | Xenia (interpreter: Xenia cannot run generated code) | SM64, MK64, OoT titles | interpreter, HLE, Xenos | 5–6 VI/s, 8–9 MIPS |
| | Console | | dynarec, HLE, Xenos | pending (M4 console test) |

## V1 reference numbers (console)

- Mario Kart 64 (PAL), races: 45–52 VI/s, ~13.5 MIPS in Accurate mode (V1 0.1.6, pure interpreter).
- Mortal Kombat 4, fights: 11–12 FPS, ~7000 texture uploads/s (V1 0.1.9).
- Paper Mario (NTSC), field: 60 VI/s, CPU ~75 %, ~12–13 MIPS (V1 0.1.9).
