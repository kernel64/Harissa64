// Harissa64 V2 - render tests on the console (docs-local/render_tests_study.md).
//
// rendertest=FOLDER in harissa64v2.ini: every FOLDER\<case>\scene.rdp (a scene
// from h64test --rdp-dump, core/rtest/h64_rtscene.h) is played through the
// Xenos renderer at the internal resolution rendertestscale= (default 1:
// one GPU pixel per N64 pixel), without worker threads. At the end of every
// frame that drew something, the images the GPU drew are read back into RDRAM
// in their own format (copy backs) and written to FOLDER\<case>\xenos_f<N>.img:
// records of big-endian (RDRAM address, byte count) followed by the bytes. The
// PC turns them into RDRAM files for tools/rt_compare.py (tools/rt_console.py).
#ifndef XB_RTEST_H
#define XB_RTEST_H

#include <xtl.h>

// Returns the number of cases drawn.
int XbRenderTest(IDirect3DDevice9 *dev, const char *folder, int scale);

#endif
