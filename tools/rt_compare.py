#!/usr/bin/env python3
"""Render test comparison (prototype, docs-local/render_tests_study.md).

Reads a scene (platform/host/h64_rtdump.h) to know which colour and depth
images each frame drew, then compares the RDRAM files the renderers wrote
(h64test --rt-play: soft_f<N>.rdram; tools/rdp_oracle: parallel_f<N>.rdram,
angrylion_f<N>.rdram) image by image.

  python tools/rt_compare.py SCENE DIR [--ref angrylion] [--png]

Per image and renderer: identical pixels, the largest channel error, and a
histogram of channel errors in the image's own steps (5-bit for 16-bit
images: 0, 1, 2-3, 4-7, 8-15, 16+; 8-bit steps for 32-bit images: 0, 1-2,
3-8, 9-32, 33-128, 129+). With --png: <DIR>/cmp_f<N>_<addr>.png holds the
reference, each renderer and a heat map of the largest error per pixel.
"""
import argparse
import json
import os
import struct
import sys

try:
    import numpy as np
except ImportError:
    sys.exit('numpy is needed')

REC_SIZES = {4: 0, 5: 0, 6: 0, 7: 0, 9: 0}


def parse_scene(path):
    """Images drawn per frame: {frame: [(kind, addr, fmt, size, width, height)]}."""
    out = {}
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'RDPDUMP2':
        sys.exit('not a scene: ' + path)
    pos, frame = 16, 0
    images = {}
    color = None
    zaddr = None
    scissor_h = 240
    n = len(data)

    def word(p):
        return struct.unpack_from('<I', data, p)[0]

    # Lowest line each image gets drawn on (per frame): its compared height.
    # The scissor's height alone counted lines no primitive reaches: Mario
    # Party 3's minigames set a 339-line scissor over a 240-line frame, and
    # the 99 lines below (the other frame buffer's memory) were compared.
    bottom = {}

    def note(kind, key, ybot=None):
        images.setdefault(frame, {})
        if key not in images[frame]:
            images[frame][key] = kind
        if ybot is not None:
            k = (frame, key)
            bottom[k] = max(bottom.get(k, 0), min(ybot, key[4]))
    while pos + 4 <= n:
        rec = word(pos)
        pos += 4
        if rec in (1, 8):
            size = word(pos + 4)
            pos += 8 + size
        elif rec == 2:
            cnt = word(pos + 4)
            w = struct.unpack_from('<%dI' % cnt, data, pos + 8)
            pos += 8 + cnt * 4
            op = (w[0] >> 24) & 0x3F
            if op == 0x3F:
                color = ((w[0] >> 21) & 7, (w[0] >> 19) & 3, (w[0] & 0x3FF) + 1, w[1] & 0xFFFFFF)
            elif op == 0x3E:
                zaddr = w[1] & 0xFFFFFF
            elif op == 0x2D:
                scissor_h = ((w[1] & 0xFFF) + 3) >> 2
            elif op in (0x24, 0x25, 0x36) or 0x08 <= op <= 0x0F:
                if color:
                    # Bottom line: rectangles' YL (10.2), triangles' YL (11.2, signed 14 bits).
                    if op in (0x24, 0x25, 0x36):
                        ybot = ((w[0] & 0xFFF) + 3) >> 2
                    else:
                        yl = w[0] & 0x3FFF
                        yl = yl - 0x4000 if yl & 0x2000 else yl
                        ybot = max(0, (yl + 3) >> 2)
                    note('color', color + (scissor_h,), ybot)
                    if zaddr is not None and 0x08 <= op <= 0x0F and (op & 1):
                        note('depth', (0, 2, color[2], zaddr, scissor_h), ybot)
        elif rec == 3:
            pos += 8
        elif rec == 0x48:
            pos += 33 * 4
        elif rec == 4:
            frame += 1
        elif rec == 6:
            break
        elif rec in (5, 7, 9):
            pass
        else:
            sys.exit('unknown record %d at %d' % (rec, pos - 4))
    for fr, imgs in images.items():
        lst = []
        for key, kind in imgs.items():
            fmt, size, width, addr, h = key
            if (fr, key) in bottom and bottom[(fr, key)] > 0:
                h = bottom[(fr, key)]
            # keep the tallest height seen for an address
            lst.append((kind, addr, fmt, size, width, h))
        merged = {}
        for it in lst:
            k = (it[0], it[1], it[3], it[4])
            if k not in merged or merged[k][5] < it[5]:
                merged[k] = it
        # An address used as the depth image is depth, even where the game
        # clears it by drawing to it as a colour image.
        depth = set(it[1] for it in merged.values() if it[0] == 'depth')
        out[fr] = [it for it in merged.values() if it[0] == 'depth' or it[1] not in depth]
    return out


def decode(ram, kind, addr, fmt, size, width, height):
    """RGBA (uint8, height x width x 4) and the channel step in bits (5 or 8)."""
    if kind == 'depth' or size == 2:
        nbytes = width * height * 2
        raw = np.frombuffer(ram[addr:addr + nbytes], dtype='>u2')
        h = len(raw) // width
        v = raw[:h * width].reshape(h, width).astype(np.int32)
        if kind == 'depth':
            # 14-bit float depth: show as grey (the comparison uses the raw value)
            g = (v >> 8).astype(np.uint8)
            return np.stack([g, g, g, np.full_like(g, 255)], -1), v, 16
        r = (v >> 11) & 31
        g = (v >> 6) & 31
        b = (v >> 1) & 31
        a = (v & 1) * 31
        five = np.stack([r, g, b, a], -1)
        return (five * 255 // 31).astype(np.uint8), five, 5
    if size == 3:
        nbytes = width * height * 4
        raw = np.frombuffer(ram[addr:addr + nbytes], dtype='>u4')
        h = len(raw) // width
        v = raw[:h * width].reshape(h, width).astype(np.int64)
        c = np.stack([(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255], -1)
        return c.astype(np.uint8), c, 8
    nbytes = width * height
    raw = np.frombuffer(ram[addr:addr + nbytes], dtype=np.uint8)
    h = len(raw) // width
    v = raw[:h * width].reshape(h, width).astype(np.int32)
    return np.stack([v, v, v, np.full_like(v, 255)], -1).astype(np.uint8), v[..., None], 8


def histogram(err, bits):
    if bits == 5:
        edges = [(0, 0), (1, 1), (2, 3), (4, 7), (8, 15), (16, 1 << 30)]
    else:
        edges = [(0, 0), (1, 2), (3, 8), (9, 32), (33, 128), (129, 1 << 30)]
    return [int(((err >= lo) & (err <= hi)).sum()) for lo, hi in edges]


def heat(err, bits):
    scale = 8 if bits == 5 else 1
    e = err * scale
    out = np.zeros(err.shape + (3,), np.uint8)
    out[(e >= 1) & (e <= 2)] = (0, 0, 255)
    out[(e >= 3) & (e <= 8)] = (0, 200, 0)
    out[(e >= 9) & (e <= 32)] = (255, 255, 0)
    out[(e >= 33) & (e <= 128)] = (255, 128, 0)
    out[e > 128] = (255, 0, 0)
    return out


def far_pct(m, kind, steps):
    """Percent of pixels at least `steps` histogram bins away (bin 1 = the smallest error)."""
    hist = m['histogram']
    total = sum(hist)
    if kind == 'depth':
        return 100.0 * (total - hist[0]) / total if total else 0.0
    return 100.0 * sum(hist[steps:]) / total if total else 0.0


def verdict(row, xenos_bins, xenos_max_pct):
    """PASS, ORACLES_DIFFER (ParaLLEl against Angrylion), SOFT_PORT (our software RDP
    against Angrylion), XENOS (beyond its tolerance: errors in histogram bin
    `xenos_bins` or above), in that order; NO_REFERENCE without Angrylion."""
    vs = row['vs']
    if row.get('ref') != 'angrylion':
        return 'NO_REFERENCE'
    if 'parallel' in vs and far_pct(vs['parallel'], row['kind'], 1) > 0:
        return 'ORACLES_DIFFER'
    if 'soft' in vs and far_pct(vs['soft'], row['kind'], 1) > 0:
        return 'SOFT_PORT'
    if 'xenos' in vs and far_pct(vs['xenos'], row['kind'], xenos_bins) > xenos_max_pct:
        return 'XENOS'
    return 'PASS'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scene')
    ap.add_argument('dir')
    ap.add_argument('--ref', default='angrylion')
    ap.add_argument('--png', action='store_true')
    ap.add_argument('--frames', default='')
    ap.add_argument('--xenos-bins', type=int, default=3,
                    help='Xenos errors from this histogram bin on count (3: 4+ steps for 16-bit images, 9+ for 32-bit: '
                         'Xenos draws in floating point without the RDP dither, 1-3 steps are expected)')
    ap.add_argument('--xenos-max-pct', type=float, default=0.5,
                    help='XENOS when more pixels than this percent are that far')
    a = ap.parse_args()
    scene = parse_scene(a.scene)
    renderers = ['angrylion', 'parallel', 'soft', 'xenos']
    report = []
    for fr in sorted(scene):
        if a.frames and str(fr) not in a.frames.split(','):
            continue
        rams = {}
        covers = {}
        for r in renderers:
            p = os.path.join(a.dir, '%s_f%d.rdram' % (r, fr))
            if os.path.exists(p):
                with open(p, 'rb') as f:
                    rams[r] = f.read()
        cp = os.path.join(a.dir, 'xenos_f%d.cover' % fr)   # tools/rt_console.py: the ranges Xenos read back
        if os.path.exists(cp):
            with open(cp) as f:
                covers[fr] = json.load(f)
        if a.ref not in rams:
            continue
        for kind, addr, fmt, size, width, height in sorted(scene[fr], key=lambda x: x[1]):
            ref_rgba, ref_v, bits = decode(rams[a.ref], kind, addr, fmt, size, width, height)
            row = {'frame': fr, 'kind': kind, 'addr': '%06X' % addr, 'width': width, 'height': ref_v.shape[0],
                   'size': size, 'ref': a.ref, 'vs': {}}
            tiles = [ref_rgba[..., :3]]
            for r in renderers:
                if r == a.ref or r not in rams:
                    continue
                if r == 'xenos' and (kind == 'depth' or not any(c[0] <= addr < c[0] + c[1] for c in covers.get(fr, []))):
                    continue   # Xenos's depth stays in EDRAM; images it did not read back are not compared
                rgba, v, _ = decode(rams[r], kind, addr, fmt, size, width, height)
                d = np.abs(v.astype(np.int64) - ref_v.astype(np.int64))
                err = d.max(axis=-1) if d.ndim == 3 else d
                if kind == 'color' and d.ndim == 3:
                    err = d[..., :3].max(axis=-1)   # alpha (coverage bit) reported apart
                total = err.size
                row['vs'][r] = {
                    'identical_pct': round(100.0 * (err == 0).sum() / total, 3),
                    'max_error': int(err.max()),
                    'histogram': histogram(err, bits),
                    'alpha_diff': int((d[..., 3] != 0).sum()) if kind == 'color' and d.ndim == 3 and d.shape[-1] == 4 else 0,
                }
                tiles.append(rgba[..., :3])
                tiles.append(heat(err, bits))
            row['verdict'] = verdict(row, a.xenos_bins, a.xenos_max_pct)
            if 'xenos' in row['vs']:
                row['xenos_far_pct'] = round(far_pct(row['vs']['xenos'], kind, a.xenos_bins), 4)
            report.append(row)
            line = 'f%d %-5s %s %dx%d %d-bit %s:' % (fr, kind, row['addr'], width, row['height'], 4 << size, row['verdict'])
            for r, m in row['vs'].items():
                line += '  %s %.2f%% max %d hist %s' % (r, m['identical_pct'], m['max_error'], m['histogram'])
            print(line)
            if a.png:
                from PIL import Image
                gap = np.full((tiles[0].shape[0], 4, 3), 64, np.uint8)
                strip = [tiles[0]]
                for t in tiles[1:]:
                    strip += [gap, t]
                Image.fromarray(np.concatenate(strip, axis=1)).save(
                    os.path.join(a.dir, 'cmp_f%d_%s_%s.png' % (fr, kind, row['addr'])))
    with open(os.path.join(a.dir, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)


if __name__ == '__main__':
    main()
