# Third-party code and data

Harissa64 V2 is licensed under the GPL v2. Code or data not written for this project is listed here with its origin and licence.

| What | Where | Origin | Licence |
|---|---|---|---|
| 8x8 bitmap font (`font8x8_basic`, U+0000–U+007F) | `platform/xbox360/font8x8_basic.h` | Daniel Hepper, https://github.com/dhepper/font8x8, based on Marcel Sondaar's font8x8 and IBM's public-domain VGA fonts | Public domain |
| CIC-NUS-6105 challenge/response algorithm | `core/pif/h64_pif.cpp` (`cic_6105_response`) | X-Scale (2011), as distributed with mupen64plus (`n64_cic_nus_6105.c`) and Harissa64 V1; rewritten in the V2 style, same algorithm and tables | BSD 2-clause (notice kept in the source) |
| RSP interpreter: scalar unit, vector unit (scalar "SISD" paths), reciprocal tables, SP DMA and registers | `core/rsp/` | ares, `ares/n64/rsp/` (`interpreter*.cpp`, `dma.cpp`, `io.cpp`, `rsp.cpp`) at commit `a776c509`, ported to the V2 C++ subset; no pipeline/dual-issue timing | ISC (below) |
| Software RDP: command decoding, span setup, coverage, interpolation, perspective divide, texture sampling and LOD, combiner, blender, depth test, dither, noise, framebuffer formats | `core/rdp/h64_rdp_render.cpp`, `core/rdp/h64_rdp_tex.cpp`, `core/rdp/h64_rdp_state.h` | ParaLLEl-RDP (Themaister/parallel-rdp, commit `1cecd042`): `rdp_device.cpp`, `rdp_renderer.cpp` and the compute shaders, ported to serial C++ (TMEM loads rewritten as the hardware walks them) | MIT (below) |
| Blender divider and VI gamma tables | `core/rdp/h64_rdp_luts.h` | ParaLLEl-RDP `parallel-rdp/luts.hpp` (same commit), data generated from it | MIT (notice in the file and below) |
| PI DMA block model and duration formula | `core/system/h64_devices.cpp` (`pi_dma`, `pi_dma_cycles`), PI bus address after CPU accesses in `core/memory/h64_bus.cpp` | ares, `ares/n64/pi/dma.cpp`, `io.cpp`, `bus.hpp` (https://github.com/ares-emulator/ares, master, October 2026), followed closely | ISC (below) |

Behaviour learned from test ROMs is credited in comments but no code is taken from them: n64-systemtest (Lemmy, MIT; studied, never copied), angrylion-rdp-plus (MAME licence, not GPL-compatible: not studied for code), Dillonb/n64-tests (no licence stated) and PeterLemon/N64 (Unlicense). The ROMs themselves are downloaded by `tests/scripts/fetch_test_roms.py` and never committed.

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

## ParaLLEl-RDP licence

```text
Copyright (c) 2020 Themaister

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
