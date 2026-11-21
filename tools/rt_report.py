#!/usr/bin/env python3
"""Render tests: summary of a scene corpus and comparison with a baseline
(docs-local/render_tests_study.md).

Reads DIR/<case>/report.json (tools/rt_compare.py) and writes DIR/summary.json:
every image of every case with its verdict (PASS, XENOS, SOFT_PORT,
ORACLES_DIFFER, NO_REFERENCE) and, when Xenos was compared, the percent of its
pixels beyond tolerance.

  python tools/rt_report.py DIR [--baseline FILE]

With --baseline (an earlier summary.json), each image is REGRESSION (passed
before, fails now), WORSE / BETTER (Xenos further from / closer to the
reference), SAME or NEW. Exit code: 0 everything passed, 1 some images fail,
2 regressions, 3 bad arguments.
"""
import argparse
import json
import os
import sys


def load_reports(d):
    out = {}
    for case in sorted(os.listdir(d)):
        p = os.path.join(d, case, 'report.json')
        if not os.path.isfile(p):
            continue
        with open(p, encoding='utf-8') as f:
            rows = json.load(f)
        imgs = {}
        for r in rows:
            key = 'f%d:%s:%s' % (r['frame'], r['kind'], r['addr'])
            imgs[key] = {'verdict': r.get('verdict', 'NO_REFERENCE')}
            if 'xenos_far_pct' in r:
                imgs[key]['xenos_far_pct'] = r['xenos_far_pct']
        out[case] = imgs
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--baseline')
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        print('no such folder: ' + a.dir)
        return 3
    cases = load_reports(a.dir)
    counts = {}
    for imgs in cases.values():
        for m in imgs.values():
            counts[m['verdict']] = counts.get(m['verdict'], 0) + 1
    summary = {'cases': cases, 'counts': counts}
    with open(os.path.join(a.dir, 'summary.json'), 'w', encoding='utf-8') as f:
        json.dump(summary, f, indent=1, sort_keys=True)
    print('%d cases, %d images: %s' % (len(cases), sum(counts.values()),
                                       ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    for case, imgs in sorted(cases.items()):
        bad = ['%s %s' % (k, m['verdict']) for k, m in sorted(imgs.items()) if m['verdict'] != 'PASS']
        if bad:
            print('  %-16s %s' % (case, '; '.join(bad[:6]) + (' ...' if len(bad) > 6 else '')))
    regressions = 0
    if a.baseline:
        with open(a.baseline, encoding='utf-8') as f:
            base = json.load(f)['cases']
        tally = {}
        for case, imgs in sorted(cases.items()):
            for key, m in sorted(imgs.items()):
                b = base.get(case, {}).get(key)
                if b is None:
                    st = 'NEW'
                elif b['verdict'] == 'PASS' and m['verdict'] != 'PASS':
                    st = 'REGRESSION'
                elif 'xenos_far_pct' in m and 'xenos_far_pct' in b and m['xenos_far_pct'] > b['xenos_far_pct']:
                    st = 'WORSE'
                elif 'xenos_far_pct' in m and 'xenos_far_pct' in b and m['xenos_far_pct'] < b['xenos_far_pct']:
                    st = 'BETTER'
                elif b['verdict'] != 'PASS' and m['verdict'] == 'PASS':
                    st = 'BETTER'
                else:
                    st = 'SAME'
                tally[st] = tally.get(st, 0) + 1
                if st in ('REGRESSION', 'WORSE', 'BETTER'):
                    print('  %-10s %s %s (%s -> %s)' % (st, case, key, b['verdict'], m['verdict']))
                regressions += st == 'REGRESSION'
        print('against the baseline: ' + ', '.join('%s %d' % kv for kv in sorted(tally.items())))
    if regressions:
        return 2
    return 0 if counts.get('PASS', 0) == sum(counts.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
