#!/usr/bin/env python3
"""Harissa64 V2 - builds the menu text tables (platform/xbox360/xb_lang_table.h).

Reads platform/xbox360/lang/<code>.txt (UTF-8, KEY = text lines; en.txt is the
reference) and checks every language before writing anything:
  - the same keys as English, each text non-empty;
  - the same %d / %s in the same order (they are filled by sprintf);
  - button prompts ("A:Select|B:Back") with the same buttons as English, and
    labels without ':' or '|';
  - every character in the menu font (xb_ui_atlas.h);
  - the texts fit the places they are shown in (widths measured with the
    atlas's advances, as the console draws them).
Arabic is turned into its joined presentation forms here (isolated, initial,
medial, final letters, lam-alef ligatures), in logical order: the UI shows
lines holding Arabic right to left.

Run from anywhere:
    python platform/xbox360/tools/gen_lang.py [--check]
--check writes nothing: it fails when a text is wrong or xb_lang_table.h is
not what the texts give (tests/scripts/run_all.ps1 runs it).
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
LANG_DIR = os.path.join(ROOT, "platform", "xbox360", "lang")
ATLAS = os.path.join(ROOT, "platform", "xbox360", "xb_ui_atlas.h")
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_lang_table.h")
# The order of the language list (and of LANG_* in xb_lang.h).
LANGS = ["en", "es", "de", "fr", "ar", "pt", "ru"]

# ---- Arabic shaping ----
# Letter -> (isolated, final, initial, medial) presentation forms; None where
# the letter has no such form (right-joining letters do not join to the next).
FORMS = {
    0x0621: (0xFE80, None, None, None),
    0x0622: (0xFE81, 0xFE82, None, None),
    0x0623: (0xFE83, 0xFE84, None, None),
    0x0624: (0xFE85, 0xFE86, None, None),
    0x0625: (0xFE87, 0xFE88, None, None),
    0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),
    0x0627: (0xFE8D, 0xFE8E, None, None),
    0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92),
    0x0629: (0xFE93, 0xFE94, None, None),
    0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98),
    0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),
    0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0),
    0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),
    0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8),
    0x062F: (0xFEA9, 0xFEAA, None, None),
    0x0630: (0xFEAB, 0xFEAC, None, None),
    0x0631: (0xFEAD, 0xFEAE, None, None),
    0x0632: (0xFEAF, 0xFEB0, None, None),
    0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8),
    0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0),
    0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8),
    0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0),
    0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8),
    0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0),
    0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8),
    0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x0648: (0xFEED, 0xFEEE, None, None),
    0x0649: (0xFEEF, 0xFEF0, None, None),
    0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}
# Lam followed by one of these alefs: one ligature (isolated, final).
LAM_ALEF = {0x0622: (0xFEF5, 0xFEF6), 0x0623: (0xFEF7, 0xFEF8), 0x0625: (0xFEF9, 0xFEFA), 0x0627: (0xFEFB, 0xFEFC)}


def joins_next(c):
    """The letter connects to the letter after it (dual-joining)."""
    return c in FORMS and FORMS[c][2] is not None


def shape_arabic(text):
    cps = [ord(ch) for ch in text]
    out = []
    i = 0
    while i < len(cps):
        c = cps[i]
        if c not in FORMS:
            out.append(c)
            i += 1
            continue
        prev = cps[i - 1] if i > 0 else None
        joined_before = prev is not None and joins_next(prev)
        if c == 0x0644 and i + 1 < len(cps) and cps[i + 1] in LAM_ALEF:
            out.append(LAM_ALEF[cps[i + 1]][1 if joined_before else 0])
            i += 2
            continue
        nxt = cps[i + 1] if i + 1 < len(cps) else None
        joined_after = joins_next(c) and nxt in FORMS
        iso, fin, ini, med = FORMS[c]
        if joined_before and joined_after:
            f = med
        elif joined_before:
            f = fin
        elif joined_after:
            f = ini
        else:
            f = iso
        out.append(f if f is not None else iso)
        i += 1
    return "".join(chr(c) for c in out)


# ---- Reading ----
def read_lang(code):
    path = os.path.join(LANG_DIR, code + ".txt")
    keys, texts = [], {}
    for n, line in enumerate(open(path, encoding="utf-8-sig"), 1):
        line = line.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if " = " not in line:
            raise SystemExit("%s:%d: expected KEY = text" % (path, n))
        k, v = line.split(" = ", 1)
        k, v = k.strip(), v.strip().replace("\\n", "\n")
        if not re.match(r"^[A-Z][A-Z0-9_]*$", k):
            raise SystemExit("%s:%d: bad key %r" % (path, n, k))
        if k in texts:
            raise SystemExit("%s:%d: %s given twice" % (path, n, k))
        keys.append(k)
        texts[k] = v
    return keys, texts


def read_atlas():
    """Code points and advances (em units) of both fonts, from xb_ui_atlas.h."""
    src = open(ATLAS, encoding="utf-8").read()
    base = float(re.search(r"#define UI_ATLAS_BASE ([0-9.]+)", src).group(1))
    adv = [{}, {}]
    font = -1
    for line in src.splitlines():
        if line.startswith("  {   // "):
            font += 1
        m = re.match(r"\s*\{ \d+, \d+, \d+, \d+, [-0-9.]+f, [-0-9.]+f, ([-0-9.]+)f \},   // U\+([0-9A-F]{4})", line)
        if m and 0 <= font < 2:
            adv[font][int(m.group(2), 16)] = float(m.group(1))
    return base, adv


def main():
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        print("ERROR:", msg)

    base, adv = read_atlas()

    def width(text, size, bold):
        a = adv[1 if bold else 0]
        return max(sum(a.get(ord(ch), a[ord("?")]) for ch in line) * size / base for line in text.split("\n"))

    btn_r, pill_h, hint = 15.0, 26.0, 19.0

    def footer_width(hints):
        x, first = 0.0, True
        for item in hints.split("|"):
            label = None
            if ":" in item:
                item, label = item.split(":", 1)
            if not first:
                x += 30
            first = False
            btns = item.split("/")
            for i, b in enumerate(btns):
                if b in ("A", "B", "X", "Y", "DPAD"):
                    x += btn_r * 2
                else:
                    x += max(width(b, 14, True) + 22, pill_h + 8)
                if i + 1 < len(btns):
                    x += 6
            if label:
                x += 10 + width(label, hint, False)
        return x

    def value_width(v):
        # draw_value: "< text >" as the text between two chevrons.
        return width(v, 21, True) + 2 * (width("›", 30, True) + 10)

    en_keys, en = read_lang("en")
    langs = {}
    for code in LANGS:
        keys, t = read_lang(code)
        name = code + ".txt"
        for k in en_keys:
            if k not in t:
                fail("%s: missing %s" % (name, k))
        for k in keys:
            if k not in en:
                fail("%s: %s is not in en.txt" % (name, k))
        for k in en_keys:
            if k not in t:
                continue
            v = t[k]
            if re.findall(r"%[a-z]", v) != re.findall(r"%[a-z]", en[k]) or v.count("%%") != en[k].count("%%"):
                fail("%s: %s must keep %s in the same order: %r" % (name, k, re.findall(r"%[a-z]", en[k]), v))
            if k.startswith("FOOT_"):
                eb = [it.split(":", 1)[0] for it in en[k].split("|")]
                vb = [it.split(":", 1)[0] for it in v.split("|")]
                if eb != vb:
                    fail("%s: %s must keep the buttons %s: %r" % (name, k, eb, v))
                for it in v.split("|"):
                    if it.count(":") > 1:
                        fail("%s: %s: a label holds ':' (%r)" % (name, k, it))
        if code == "ar":
            t = dict((k, shape_arabic(v)) for k, v in t.items())
        for k, v in t.items():
            for ch in v:
                if ch != "\n" and ord(ch) not in adv[0]:
                    fail("%s: %s: %r (U+%04X) is not in the menu font" % (name, k, ch, ord(ch)))
        langs[code] = t
    if not ok:
        sys.exit(1)

    # ---- Widths (the layouts of xb_menu.cpp and main.cpp) ----
    def check(code, what, w, limit):
        if w > limit + 0.5:
            fail("%s: %s is %.0f pixels wide, at most %.0f" % (code, what, w, limit))

    for code in LANGS:
        t = langs[code]
        names = [langs[c]["LANG_NAME"] for c in LANGS]
        onoff = [t["ON"], t["OFF"]]
        menus = {
            "settings": [("CPU_NEXT", [t["RECOMPILER"], t["INTERPRETER"]], 1), ("RSP_NEXT", ["HLE"], 1), ("AUDIO_MARGIN", ["500 ms"], 1),
                         ("SHOW_FPS", [t["OFF"], t["FPS_ONLY"], t["FPS_CPU"], t["FPS_ALL"]], 1), ("GRAPHICS", [], 1), ("CONTROLS", [t["CUSTOM"], t["DEFAULT"]], 0),
                         ("LANGUAGE", names, 1), ("ART_DOWNLOADS", onoff, 1), ("ART_ALL", [], 1), ("SAVE_ALL", [], 1),
                         ("SAVE_GAME", [], 1), ("BACK", [], 1)],
            "graphics": [("PRESET", [t["PRESET_ORIGINAL"], t["PRESET_ENHANCED"], t["PRESET_SMOOTH"], t["PRESET_CRT"], t["CUSTOM"]], 1),
                         ("RESOLUTION", [t["RES_NATIVE"], "HD 1280x720 (16:9)"], 1), ("STRETCH", onoff, 1), ("PIC_SIZE", ["100%"], 1),
                         ("PIC_LR", ["+160"], 1), ("PIC_UD", ["+120"], 1),
                         ("BRIGHTNESS", [t["NORMAL"], t["LIGHTER"], t["MUCH_LIGHTER"]], 1), ("VSYNC", onoff, 1), ("ANTIFLICKER", onoff, 1),
                         ("TEXFILTER", [t["FILTER_3POINT"], t["FILTER_BILINEAR"], t["FILTER_NEAREST"]], 1), ("FXAA", onoff, 1),
                         ("SHARPEN", [t["OFF"], t["LOW"], t["HIGH"]], 1), ("BLUR", [t["OFF"], t["SOFT"], t["STRONG"]], 1),
                         ("SCREEN_EFFECT", [t["SCREEN_NONE"], t["SCANLINES"], t["CRT"], t["CRT_CURVED"], t["LCD_GRID"], t["LIGHT_SCANLINES"]], 1),
                         ("BACK", [], 1)],
            "pause": [("RESUME", [], 1), ("SAVE_STATE", [], 1), ("LOAD_STATE", [], 1), ("STATE_SLOT", ["9"], 1), ("SETTINGS", [], 1),
                      ("RESET_GAME", [], 1), ("BACK_TO_LIST", [], 1), ("QUIT_DASHBOARD", [], 1)],
        }
        # UiMenuDraw: a 660-pixel panel, labels 44 pixels in, values 40 from the right, some room between.
        for menu, rows in menus.items():
            for key, values, chev in rows:
                lw = width(t[key], 22, True)
                vw = max([value_width(v) if chev else width(v, 21, False) for v in values] + [0])
                check(code, "%s row %s with its widest value" % (menu, key), lw + vw + (24 if values else 0), 660 - 84)
        for key in ("SETTINGS", "SETTINGS_ALL", "GRAPHICS", "CONTROLS", "PAUSED"):
            check(code, "title " + key, width(t[key], 30, True), 660 - 80)
        for key in ("FOOT_SELECT", "FOOT_PAUSED"):
            check(code, key, footer_width(t[key]), 660 - 80)
        # Controls page: 352-pixel columns, text 24 in, prompts 18 from the right.
        col = 352 - 24 - 18 - 12
        dirs = {"N64_DUP": "DIR_UP", "N64_DDOWN": "DIR_DOWN", "N64_DLEFT": "DIR_LEFT", "N64_DRIGHT": "DIR_RIGHT",
                "N64_CUP": "DIR_UP", "N64_CDOWN": "DIR_DOWN", "N64_CLEFT": "DIR_LEFT", "N64_CRIGHT": "DIR_RIGHT"}
        for key, d in dirs.items():
            pad = ("DPAD:" if key.startswith("N64_D") else "RS:") + t[d]
            check(code, "controls row %s with its default input" % key, width(t[key], 20, True) + footer_width(pad), col)
            check(code, "controls row %s while waiting" % key, width(t[key], 20, True) + width(t["PRESS_INPUT"], 18, True), col)
        for key, values in (("ANALOG_STICK", [t["STICK_LEFT"], t["STICK_RIGHT"], t["STICK_DPAD"], t["OFF"]]),
                            ("INVERT", [t["OFF"], "X", "Y", t["X_AND_Y"]]), ("DEAD_ZONE", ["25%"]), ("SENSITIVITY", ["100%"]), ("RUMBLE", [t["ON"], t["OFF"]]),
                            ("RESET_DEFAULT", []), ("BACK", [])):
            check(code, "controls row %s with its widest value" % key,
                  width(t[key], 20, True) + max([value_width(v) for v in values] + [0]), col)
        for key in ("HEAD_BUTTONS", "HEAD_DPAD", "HEAD_CBUTTONS", "HEAD_STICK"):
            check(code, key, width(t[key], 15, True), 330)
        longest_n64 = max([t[k] for k in dirs] + ["START"], key=lambda s: width(s, 20, True))
        longest_src = max([t[k] for k in t if k.startswith("SRC_")], key=lambda s: width(s, 18, True))
        foot = 1140 - 80
        check(code, "FOOT_CTL_BUTTON and BACK_RESERVED", footer_width(t["FOOT_CTL_BUTTON"]) + 30 + width(t["BACK_RESERVED"], 16, False), foot)
        check(code, "FOOT_CTL_OTHER and BACK_RESERVED", footer_width(t["FOOT_CTL_OTHER"]) + 30 + width(t["BACK_RESERVED"], 16, False), foot)
        note = t["INPUT_ADDED"].replace("%s", longest_n64, 1).replace("%s", longest_src, 1)
        check(code, "FOOT_CTL_BUTTON and the longest INPUT_ADDED note", footer_width(t["FOOT_CTL_BUTTON"]) + 30 + width(note, 18, True), foot)
        check(code, "PRESS_FOR, its bar and FOOT_CANCEL",
              width(t["PRESS_FOR"].replace("%s", longest_n64), 20, True) + 40 + 220 + footer_width(t["FOOT_CANCEL"]), foot)
        check(code, "PLAYER, USED_WHEN", width(t["USED_WHEN"], 16, False), 700)
        # Quit dialog, browser, picker, messages.
        check(code, "QUIT_ASK", width(t["QUIT_ASK"], 30, True), 640 - 96)
        check(code, "FOOT_BROWSER and the box art progress", footer_width(t["FOOT_BROWSER"]) + 40 + width(t["ART_PROGRESS"] % (888, 888), 17, False) + 16 + 150,
              1280 - 128)
        check(code, "DOWNLOADING", width(t["DOWNLOADING"], 15, False), 300)
        check(code, "GAME_CODE", width(t["GAME_CODE"], 13, True), 110)
        check(code, "SIZE", width(t["SIZE"], 13, True), 90)
        check(code, "SAVE", width(t["SAVE"], 13, True), 200)
        check(code, "NOT_N64", width(t["NOT_N64"], 19, False), 520)
        for key in ("NO_ROM",):
            check(code, key, width(t[key], 30, True), 540)
        for key in ("COPY_ROMS", "INTO_ROMS"):
            check(code, key, width(t[key], 21, False), 540)
        check(code, "WIKI_TEXT", width(t["WIKI_TEXT"], 13, False), 520)
        for key in ("UPDATE_AVAILABLE", "NO_DOWNLOADS", "GETTING_ART", "ART_DONE_LOST", "STATE_SAVED", "STATE_WRITE_FAILED", "NO_STATE",
                    "STATE_LOAD_FAILED", "STATE_LOADED", "KEEP_HOLDING", "NOT_SAVED_SCRIPT", "SAVED_ALL", "SAVED_GAME",
                    "SETTINGS_WRITE_FAILED", "GAME_RESET"):
            check(code, "toast " + key, width(t[key], 21, False) + 72, 1280 - 128)
        check(code, "LANG_TITLE", width(t["LANG_TITLE"], 34, True), 1000)
        check(code, "LANG_NAME", width(t["LANG_NAME"], 22, True), 230)
        for key in ("CRASHED", "CANNOT_READ", "RENDERER_FAILED", "NO_ROM"):
            check(code, "message title " + key, width(t[key], 30, True), 1100)
    if not ok:
        sys.exit(1)

    # ---- Writing ----
    def c_string(s):
        out, prev_hex = '"', False
        for b in s.encode("utf-8"):
            ch = chr(b)
            if b >= 0x80:
                out += "\\x%02X" % b
                prev_hex = True
                continue
            if prev_hex and ch in "0123456789abcdefABCDEF":
                out += '" "'
            prev_hex = False
            if ch == "\\":
                out += "\\\\"
            elif ch == '"':
                out += '\\"'
            elif ch == "\n":
                out += "\\n"
            else:
                out += ch
        return out + '"'

    lines = []
    lines.append("// Harissa64 V2 - menu texts. GENERATED by platform/xbox360/tools/gen_lang.py from platform/xbox360/lang/*.txt: do not edit.")
    lines.append("// UTF-8; Arabic already in its joined presentation forms (logical order).")
    lines.append("#ifndef XB_LANG_TABLE_H")
    lines.append("#define XB_LANG_TABLE_H")
    lines.append("")
    lines.append("#define LANG_COUNT %d" % len(LANGS))
    lines.append("")
    lines.append("enum")
    lines.append("{")
    for i, k in enumerate(en_keys):
        # The English text as a comment, without backslashes (one at the end would continue the comment).
        lines.append("    T_%s,   // %s" % (k, en[k].replace("\n", " ").replace("\\", "/")))
    lines.append("    T_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("#ifdef XB_LANG_DEFINE_TABLES   // xb_lang.cpp only")
    lines.append("static const char *const s_langCode[LANG_COUNT] = { %s };" % ", ".join('"%s"' % c for c in LANGS))
    lines.append("")
    lines.append("static const char *const s_langText[LANG_COUNT][T_COUNT] = {")
    for code in LANGS:
        lines.append("  {   // %s" % code)
        for k in en_keys:
            lines.append("    %s," % c_string(langs[code][k]))
        lines.append("  },")
    lines.append("};")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif")
    text = "\n".join(lines) + "\n"
    if "--check" in sys.argv[1:]:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if old.replace("\r\n", "\n") != text:
            print("ERROR: %s is not up to date: run platform/xbox360/tools/gen_lang.py" % OUT)
            sys.exit(1)
        print("texts OK: %d languages, %d texts, %s up to date" % (len(LANGS), len(en_keys), os.path.basename(OUT)))
        return
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)
    print("wrote %s: %d languages, %d texts" % (OUT, len(LANGS), len(en_keys)))


if __name__ == "__main__":
    main()
