# Third-party code and data

Harissa64 V2 is licensed under the GPL v2. Code or data not written for this project is listed here with its origin and licence.

| What | Where | Origin | Licence |
|---|---|---|---|
| 8x8 bitmap font (`font8x8_basic`, U+0000–U+007F) | `platform/xbox360/font8x8_basic.h` | Daniel Hepper, https://github.com/dhepper/font8x8, based on Marcel Sondaar's font8x8 and IBM's public-domain VGA fonts | Public domain |
| CIC-NUS-6105 challenge/response algorithm | `core/pif/h64_pif.cpp` (`cic_6105_response`) | X-Scale (2011), as distributed with mupen64plus (`n64_cic_nus_6105.c`) and Harissa64 V1; rewritten in the V2 style, same algorithm and tables | BSD 2-clause (notice kept in the source) |
| PI DMA block model and duration formula | `core/system/h64_devices.cpp` (`pi_dma`, `pi_dma_cycles`), PI bus address after CPU accesses in `core/memory/h64_bus.cpp` | ares, `ares/n64/pi/dma.cpp`, `io.cpp`, `bus.hpp` (https://github.com/ares-emulator/ares, master, October 2026), followed closely | ISC (below) |

Behaviour learned from test ROMs is credited in comments but no code is taken from them: n64-systemtest (Lemmy, MIT; studied, never copied), Dillonb/n64-tests (no licence stated) and PeterLemon/N64 (Unlicense). The ROMs themselves are downloaded by `tests/scripts/fetch_test_roms.py` and never committed.

## ares licence

```text
Copyright (c) 2004-2025 ares team, Near et al

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```
