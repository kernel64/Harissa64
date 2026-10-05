# Progress

## Current milestone: M0 — Environment and foundations (complete, waiting for the go for M1)

| Step | Status |
|---|---|
| 1. Environment discovery | Done (see CLAUDE.md, "Environment") |
| 2. XDK C++ feature probe | Done (see CLAUDE.md, "Core language subset") |
| 3. Executable memory proof of concept | **Done on the console (run 3, 2026-10-05): a dynarec is possible.** Code written into an ERW image section (`.jitc`) or into `.text` runs and can be rewritten; dynamically allocated memory is never executable. See CLAUDE.md, "Executable memory". |
| 4. Branch `v2`, skeleton, three builds, test script | Done: MSVC 2026, gcc (LE) and ppc64 (BE, qemu) pass the unit tests; the XDK `.xex` passes them in Xenia and **on the console** (2026-10-05: big-endian, 32-bit pointers, 5 tests / 97 checks, 0 failures) |

## To test on the console

Nothing pending.

## Next

M1 — CPU, memory, boot (after the user confirms M0).
