#!/usr/bin/env python3
"""Scale ONLY the gain arrays of the committed ElasticModalData.hpp by the
per-preset GAIN_CAL factors (measured from the real engine), leaving freq and
decay bit-exact. The engine path is linear in gain, so one multiplicative
constant per preset equalizes RMS to Classic at any strike/modeCount/decay.

Rationale for in-place edit over re-baking: the current scipy cannot re-run
elastic_bake.modes() (values[6:6+nmax] TypeError), and a re-bake would taint
freq/decay via eigsh nondeterminism. This edit touches only gains.
"""
import re, sys

SCALE = [7.1841,8.5952,4.1660,15.4803,11.2872,3.8970,5.2738,3.7559,
         5.0155,7.5557,4.9341,3.9318,1.9235,3.6008,4.9460,6.4131,8.7939,7.9340]

FLOAT = re.compile(r'([-+]?[0-9]+\.?[0-9]*[eE][-+]?[0-9]+)f')

def split_top(s):
    """Split s on commas at brace-depth 0; returns (cooked_parts, separators_uniform)."""
    parts = []; depth = 0; cur = []
    for ch in s:
        if ch == '{': depth += 1
        elif ch == '}': depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur)); cur = []
        else:
            cur.append(ch)
    if cur: parts.append(''.join(cur))
    return parts

def scale_floats(s, k):
    return FLOAT.sub(lambda m: f'{float(m.group(1))*k:.9e}f', s)

def main(path):
    src = open(path, encoding='utf-8').read()
    # --- kElasticPresets: each entry is "{Name,n,{freq},{decay},{GAIN},{ffreq},{fdecay}}" ---
    m = re.search(r'(kElasticPresets\[[^]]+\]\s*=\s*\{)(.*?)(\};)', src, re.S)
    if not m:
        print('FATAL: kElasticPresets block not found'); sys.exit(1)
    head, body, tail = m.group(1), m.group(2), m.group(3)
    presets = split_top(body)
    if len(presets) != 18:
        print(f'FATAL: expected 18 presets, got {len(presets)}'); sys.exit(1)
    new_presets = []
    for i, pr in enumerate(presets):
        inner = pr.strip()
        if inner.startswith('{') and inner.endswith('}'): inner = inner[1:-1]
        fields = split_top(inner)  # ["Name", n, {freq}, {decay}, {GAIN}, {ff}, {fd}]
        if len(fields) != 7:
            print(f'FATAL: preset {i} has {len(fields)} fields (expected 7)'); sys.exit(1)
        k = SCALE[i]
        fields[4] = scale_floats(fields[4], k)   # the gains array
        new_presets.append('{' + ','.join(fields) + '}')
    q = re.search(r'(kElasticFineGains(?:\[[^\]]+\])+\s*=\s*\{)(.*?)(\};)', src, re.S)
    if not q:
        print('FATAL: kElasticFineGains block not found'); sys.exit(1)
    ghead, gbody, gtail = q.group(1), q.group(2), q.group(3)
    fine_blocks = split_top(gbody)
    if len(fine_blocks) != 18:
        print(f'FATAL: expected 18 fine-gain blocks, got {len(fine_blocks)}'); sys.exit(1)
    fine_blocks = [scale_floats(b, SCALE[i]) for i, b in enumerate(fine_blocks)]
    src = src[:src.index(m.group(0))] + head + ',\n'.join(new_presets) + tail + src[src.index(m.group(0))+len(m.group(0)):]
    src = src[:src.index(q.group(0))] + ghead + ',\n'.join(fine_blocks) + gtail + src[src.index(q.group(0))+len(q.group(0)):]
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    print('Scaled gains for 18 presets in', path)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else
        'plugins/MultiScaleBody/src/ElasticModalData.hpp'))