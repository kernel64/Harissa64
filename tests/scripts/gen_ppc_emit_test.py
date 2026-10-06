#!/usr/bin/env python3
"""Generates tests/unit/test_ppc_emit.cpp: each case is a PowerPC instruction
written for GNU as and the matching call to our encoder. The expected words
come from powerpc64-linux-gnu-as (run in WSL), so the encoder is checked
against a real assembler on every target.

Run (from Windows): python tests/scripts/gen_ppc_emit_test.py
"""
import os
import subprocess
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

CASES = [
    ("addi 3,4,-5", "ppc_addi(&c, 3, 4, -5)"),
    ("addis 3,4,0x1234", "ppc_addis(&c, 3, 4, 0x1234)"),
    ("li 3,42", "ppc_li(&c, 3, 42)"),
    ("lis 3,-32768", "ppc_lis(&c, 3, -32768)"),
    ("ori 3,4,0xBEEF", "ppc_ori(&c, 3, 4, 0xBEEF)"),
    ("oris 3,4,0xBEEF", "ppc_oris(&c, 3, 4, 0xBEEF)"),
    ("xori 3,4,7", "ppc_xori(&c, 3, 4, 7)"),
    ("xoris 3,4,7", "ppc_xoris(&c, 3, 4, 7)"),
    ("andi. 3,4,0xFF", "ppc_andi_(&c, 3, 4, 0xFF)"),
    ("andis. 3,4,0xFF", "ppc_andis_(&c, 3, 4, 0xFF)"),
    ("mulli 3,4,-3", "ppc_mulli(&c, 3, 4, -3)"),
    ("subfic 3,4,31", "ppc_subfic(&c, 3, 4, 31)"),
    ("add 3,4,5", "ppc_add(&c, 3, 4, 5)"),
    ("subf 3,4,5", "ppc_subf(&c, 3, 4, 5)"),
    ("neg 3,4", "ppc_neg(&c, 3, 4)"),
    ("mullw 3,4,5", "ppc_mullw(&c, 3, 4, 5)"),
    ("mulhw 3,4,5", "ppc_mulhw(&c, 3, 4, 5)"),
    ("mulhwu 3,4,5", "ppc_mulhwu(&c, 3, 4, 5)"),
    ("mulld 3,4,5", "ppc_mulld(&c, 3, 4, 5)"),
    ("mulhd 3,4,5", "ppc_mulhd(&c, 3, 4, 5)"),
    ("mulhdu 3,4,5", "ppc_mulhdu(&c, 3, 4, 5)"),
    ("divw 3,4,5", "ppc_divw(&c, 3, 4, 5)"),
    ("divwu 3,4,5", "ppc_divwu(&c, 3, 4, 5)"),
    ("divd 3,4,5", "ppc_divd(&c, 3, 4, 5)"),
    ("divdu 3,4,5", "ppc_divdu(&c, 3, 4, 5)"),
    ("and 3,4,5", "ppc_and(&c, 3, 4, 5)"),
    ("andc 3,4,5", "ppc_andc(&c, 3, 4, 5)"),
    ("or 3,4,5", "ppc_or(&c, 3, 4, 5)"),
    ("mr 3,4", "ppc_mr(&c, 3, 4)"),
    ("xor 3,4,5", "ppc_xor(&c, 3, 4, 5)"),
    ("nor 3,4,5", "ppc_nor(&c, 3, 4, 5)"),
    ("extsb 3,4", "ppc_extsb(&c, 3, 4)"),
    ("extsh 3,4", "ppc_extsh(&c, 3, 4)"),
    ("extsw 3,4", "ppc_extsw(&c, 3, 4)"),
    ("slw 3,4,5", "ppc_slw(&c, 3, 4, 5)"),
    ("srw 3,4,5", "ppc_srw(&c, 3, 4, 5)"),
    ("sraw 3,4,5", "ppc_sraw(&c, 3, 4, 5)"),
    ("srawi 3,4,7", "ppc_srawi(&c, 3, 4, 7)"),
    ("sld 3,4,5", "ppc_sld(&c, 3, 4, 5)"),
    ("srd 3,4,5", "ppc_srd(&c, 3, 4, 5)"),
    ("srad 3,4,5", "ppc_srad(&c, 3, 4, 5)"),
    ("sradi 3,4,7", "ppc_sradi(&c, 3, 4, 7)"),
    ("sradi 3,4,45", "ppc_sradi(&c, 3, 4, 45)"),
    ("rlwinm 3,4,5,6,7", "ppc_rlwinm(&c, 3, 4, 5, 6, 7)"),
    ("rldicl 3,4,5,6", "ppc_rldicl(&c, 3, 4, 5, 6)"),
    ("rldicl 3,4,40,50", "ppc_rldicl(&c, 3, 4, 40, 50)"),
    ("rldicr 3,4,5,6", "ppc_rldicr(&c, 3, 4, 5, 6)"),
    ("rldicr 3,4,33,40", "ppc_rldicr(&c, 3, 4, 33, 40)"),
    ("rldimi 3,4,32,0", "ppc_rldimi(&c, 3, 4, 32, 0)"),
    ("sldi 3,4,32", "ppc_sldi(&c, 3, 4, 32)"),
    ("srdi 3,4,32", "ppc_srdi(&c, 3, 4, 32)"),
    ("clrldi 3,4,32", "ppc_clrldi(&c, 3, 4, 32)"),
    ("cmpw 0,3,4", "ppc_cmpw(&c, 0, 3, 4)"),
    ("cmpd 1,3,4", "ppc_cmpd(&c, 1, 3, 4)"),
    ("cmplw 0,3,4", "ppc_cmplw(&c, 0, 3, 4)"),
    ("cmpld 7,3,4", "ppc_cmpld(&c, 7, 3, 4)"),
    ("cmpwi 0,3,-5", "ppc_cmpwi(&c, 0, 3, -5)"),
    ("cmpdi 0,3,5", "ppc_cmpdi(&c, 0, 3, 5)"),
    ("cmplwi 0,3,5", "ppc_cmplwi(&c, 0, 3, 5)"),
    ("cmpldi 0,3,5", "ppc_cmpldi(&c, 0, 3, 5)"),
    ("lbz 3,-8(4)", "ppc_lbz(&c, 3, -8, 4)"),
    ("lhz 3,8(4)", "ppc_lhz(&c, 3, 8, 4)"),
    ("lha 3,8(4)", "ppc_lha(&c, 3, 8, 4)"),
    ("lwz 3,8(4)", "ppc_lwz(&c, 3, 8, 4)"),
    ("lwa 3,8(4)", "ppc_lwa(&c, 3, 8, 4)"),
    ("ld 3,-16(4)", "ppc_ld(&c, 3, -16, 4)"),
    ("stb 3,8(4)", "ppc_stb(&c, 3, 8, 4)"),
    ("sth 3,8(4)", "ppc_sth(&c, 3, 8, 4)"),
    ("stw 3,8(4)", "ppc_stw(&c, 3, 8, 4)"),
    ("stwu 1,-160(1)", "ppc_stwu(&c, 1, -160, 1)"),
    ("std 3,16(4)", "ppc_std(&c, 3, 16, 4)"),
    ("stdu 1,-160(1)", "ppc_stdu(&c, 1, -160, 1)"),
    ("lbzx 3,4,5", "ppc_lbzx(&c, 3, 4, 5)"),
    ("lhzx 3,4,5", "ppc_lhzx(&c, 3, 4, 5)"),
    ("lhax 3,4,5", "ppc_lhax(&c, 3, 4, 5)"),
    ("lwzx 3,4,5", "ppc_lwzx(&c, 3, 4, 5)"),
    ("lwax 3,4,5", "ppc_lwax(&c, 3, 4, 5)"),
    ("ldx 3,4,5", "ppc_ldx(&c, 3, 4, 5)"),
    ("stbx 3,4,5", "ppc_stbx(&c, 3, 4, 5)"),
    ("sthx 3,4,5", "ppc_sthx(&c, 3, 4, 5)"),
    ("stwx 3,4,5", "ppc_stwx(&c, 3, 4, 5)"),
    ("stdx 3,4,5", "ppc_stdx(&c, 3, 4, 5)"),
    ("mflr 0", "ppc_mflr(&c, 0)"),
    ("mtlr 0", "ppc_mtlr(&c, 0)"),
    ("mtctr 12", "ppc_mtctr(&c, 12)"),
    ("blr", "ppc_blr(&c)"),
    ("bctr", "ppc_bctr(&c)"),
    ("bctrl", "ppc_bctrl(&c)"),
    ("nop", "ppc_nop(&c)"),
]


def main():
    src = ".text\n" + "\n".join(a for a, _ in CASES) + "\n"
    tmp = tempfile.mkdtemp()
    open(os.path.join(tmp, "t.s"), "w").write(src)
    wsl_tmp = subprocess.run(["wsl", "-d", "Ubuntu", "--", "wslpath", "-a", tmp.replace("\\", "/")],
                             capture_output=True, text=True).stdout.strip()
    subprocess.run(["wsl", "-d", "Ubuntu", "--", "powerpc64-linux-gnu-as", "-mppc64", "-o", wsl_tmp + "/t.o", wsl_tmp + "/t.s"], check=True)
    subprocess.run(["wsl", "-d", "Ubuntu", "--", "powerpc64-linux-gnu-objcopy", "-O", "binary", "-j", ".text",
                    wsl_tmp + "/t.o", wsl_tmp + "/t.bin"], check=True)
    data = open(os.path.join(tmp, "t.bin"), "rb").read()
    words = [int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data), 4)]
    assert len(words) == len(CASES), (len(words), len(CASES))
    out = ['// Generated by tests/scripts/gen_ppc_emit_test.py from GNU as (powerpc64-linux-gnu-as): do not edit.',
           '#include "unit_tests.h"', '', '#include "../../core/dynarec/h64_ppc_emit.h"', '',
           'void test_ppc_emit(H64TestContext *ctx)', '{',
           '    u32 buf[4];', '    H64PpcCode c;']
    for (asm, call), w in zip(CASES, words):
        out.append('    c.buf = buf; c.pos = 0; c.cap = 4; c.overflow = 0; %s;' % call)
        out.append('    H64_CHECK_EQ(ctx, buf[0], 0x%08Xu);   // %s' % (w, asm))
    out.append('}')
    path = os.path.join(ROOT, "tests", "unit", "test_ppc_emit.cpp")
    open(path, "w", newline="\n").write("\n".join(out) + "\n")
    print("wrote %s (%d cases)" % (path, len(CASES)))


if __name__ == "__main__":
    main()
