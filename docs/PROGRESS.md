# Progress

## Current milestone: M0 — Environment and foundations

| Step | Status |
|---|---|
| 1. Environment discovery | Done (see CLAUDE.md, "Environment") |
| 2. XDK C++ feature probe | Done (see CLAUDE.md, "Core language subset") |
| 3. Executable memory proof of concept | Built; runs in Xenia only up to the first call (Xenia cannot execute generated code). **Waiting for the console test.** |
| 4. Branch `v2`, skeleton, three builds, test script | Done: MSVC 2026, gcc (LE) and ppc64 (BE, qemu) pass the unit tests; the XDK `.xex` passes them in Xenia and **on the console** (2026-10-05: big-endian, 32-bit pointers, 5 tests / 97 checks, 0 failures) |

## To test on the console

1. `platform\xbox360\tools\execmem\execmem.xex` (build it with `tools\execmem\build.cmd` or `run_all.ps1 -ExecMem`).
   - First console run (2026-10-05): method 1 (VirtualAlloc RWX) allocated `0x00070000`, then the title died without the `__except` handler firing, so the other methods never ran.
   - The tool now survives crashes: relaunch it until the summary screen appears. Each relaunch resumes after the method that crashed (`execmem_state.txt`, `execmem_results.txt` and `execmem.log` next to the `.xex`; Y resets). Checked in Xenia: 8 launches cover the 10 methods.
   - Second console run: every crash was DashLaunch's "fatal crash intercepted" and a return to Aurora. Method 10 (VirtualAlloc RWX + KeSweepIcacheRange) gave a catchable call fault: not executable. The log only kept its last write (append bug, fixed).
   - Third version (13 methods, see CLAUDE.md): delete the old `execmem*` files next to the `.xex`, relaunch until the summary appears, send back `execmem.log` and `execmem_results.txt`.
2. Done (console OK). `platform\xbox360\Release\harissa64v2.xex`: should show "HARISSA64 V2", version 2.0.0-dev, "Big-endian, 32-bit pointers" and "Unit tests: PASSED" in green; Back returns to Aurora. Send back `harissa64v2.log` (next to the `.xex`).

## Next

M1 — CPU, memory, boot (after the user confirms M0).
