# Progress

## Current milestone: M0 — Environment and foundations

| Step | Status |
|---|---|
| 1. Environment discovery | Done (see CLAUDE.md, "Environment") |
| 2. XDK C++ feature probe | Done (see CLAUDE.md, "Core language subset") |
| 3. Executable memory proof of concept | Built; runs in Xenia only up to the first call (Xenia cannot execute generated code). **Waiting for the console test.** |
| 4. Branch `v2`, skeleton, three builds, test script | Done: MSVC 2026, gcc (LE) and ppc64 (BE, qemu) pass the unit tests; the XDK `.xex` builds and passes them in Xenia |

## To test on the console

1. `platform\xbox360\tools\execmem\execmem.xex` (build it with `tools\execmem\build.cmd` or `run_all.ps1 -ExecMem`). Launch it from Aurora. Note the bar colours (green = OK, red = allocation failed, magenta = write fault, orange = call fault, yellow = wrong value, grey = not run; bars 1 to 10 from top to bottom), the summary in the message box, and send back `execmem.log` (written next to the `.xex`). If the console freezes or the program quits early, the log shows the last method reached.
2. `platform\xbox360\Release\harissa64v2.xex`: should show "HARISSA64 V2", version 2.0.0-dev, "Big-endian, 32-bit pointers" and "Unit tests: PASSED" in green; Back returns to Aurora. Send back `harissa64v2.log` (next to the `.xex`).

## Next

M1 — CPU, memory, boot (after the user confirms M0).
