#!/usr/bin/env python3
"""Material resolution support for the UT2004 import (plain python3 with numpy + Pillow; run with -I).

Steps (default: all, in this order):
  stats   ImportCache/texture_stats.json: mean colour, contrast, black / white / alpha coverage of every
          exported texture PNG (sampled at <= 128 px). Tools/ut2k4/matresolve.py uses it to tell colour
          textures from masks, lightmaps and flat images. Up-to-date entries are kept.
  bake    Combiner Add / Add_With_Mask_Modulation of two plain textures (no modifiers, so both share the
          same UVs, e.g. the Onslaught team skins: uncoloured diffuse + team colour layer) is baked into
          ImportCache/textures/<Pkg>/<Group.Name>_Baked.png, which the resolver then uses as the Base.
  audit   Resolves every cached material with the current rules and with the exporter's original pick
          (the 'base_texture' of the material json), classifies how the Base was found, flags suspicious
          picks (mask / spec / detail / ... names, near-black / near-white / flat images, much smaller than
          the other textures of the graph), and writes Saved/ImportUT2004/material_audit.json plus the list
          of material instances whose Base / blend / parameters change (material_fixes.json), which
          'ImportUT2004.py --fix-materials' applies in place.

Usage: python3 -I Tools/ut2k4_materials.py [--steps stats,bake,audit] [--jobs N] [--maps M1 M2 ...]
"""

import argparse
import collections
import json
import os
import re
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(TOOLS)
CACHE = os.path.join(PROJECT, 'ImportCache')
OUT_DIR = os.path.join(PROJECT, 'Saved', 'ImportUT2004')
sys.path.insert(0, TOOLS)

from ut2k4 import matresolve as mr  # noqa: E402

STATS_FILE = os.path.join(CACHE, 'texture_stats.json')
STATS_VERSION = 1
SAMPLE = 128


# ---- stats -------------------------------------------------------------------------------

def _stats_of(png):
    import numpy as np
    from PIL import Image
    im = Image.open(png)
    im.load()
    has_alpha = 'A' in im.getbands() or (im.mode == 'P' and 'transparency' in im.info)
    if im.mode in ('I;16', 'I;16B', 'I'):
        a = np.asarray(im, dtype=np.float64)
        a = a / (65535.0 if a.max() > 255 else 255.0)
        im = Image.fromarray((a * 255).clip(0, 255).astype('uint8'))
    im = im.convert('RGBA')
    w, h = im.size
    if max(w, h) > SAMPLE:
        f = max(w, h) / SAMPLE
        im = im.resize((max(1, int(w / f)), max(1, int(h / f))), Image.BOX)
    a = np.asarray(im, dtype=np.float64) / 255.0
    rgb, alpha = a[..., :3], a[..., 3]
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722])
    mean = rgb.reshape(-1, 3).mean(0)
    return {
        'w': w, 'h': h, 'mean': [round(float(x), 4) for x in mean],
        'lum': round(float(lum.mean()), 4), 'std': round(float(lum.std()), 4),
        'chroma': round(float((rgb.max(-1) - rgb.min(-1)).mean()), 4),
        'black': round(float((lum < 0.04).mean()), 4), 'white': round(float((lum > 0.94).mean()), 4),
        'has_alpha': bool(has_alpha), 'alpha': round(float(alpha.mean()), 4),
    }


def _stats_job(item):
    key, png, mtime = item
    try:
        s = _stats_of(png)
    except Exception as ex:  # noqa: BLE001 - a broken PNG must not stop the batch
        return key, {'error': str(ex), 'mtime': mtime, 'v': STATS_VERSION}
    s['mtime'] = mtime
    s['v'] = STATS_VERSION
    return key, s


def step_stats(jobs):
    t0 = time.time()
    old = {}
    if os.path.isfile(STATS_FILE):
        with open(STATS_FILE, 'r', encoding='utf-8') as f:
            old = json.load(f).get('textures') or {}
    out, todo = {}, []
    root = os.path.join(CACHE, 'textures')
    for pkg in sorted(os.listdir(root)):
        d = os.path.join(root, pkg)
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            if not fn.endswith('.json'):
                continue
            try:
                with open(os.path.join(d, fn), 'r', encoding='utf-8') as f:
                    doc = json.load(f)
            except (OSError, ValueError):
                continue
            if not doc.get('png'):
                continue
            png = os.path.join(d, doc['png'])
            if not os.path.isfile(png):
                continue
            key = doc.get('path') or (pkg + '.' + fn[:-5])
            mtime = int(os.path.getmtime(png))
            o = old.get(key)
            if o and o.get('mtime') == mtime and o.get('v') == STATS_VERSION:
                out[key] = o
            else:
                todo.append((key, png, mtime))
    print('stats: %d up to date, %d to sample' % (len(out), len(todo)), flush=True)
    if todo:
        from multiprocessing import Pool
        with Pool(jobs) as pool:
            for i, (k, s) in enumerate(pool.imap_unordered(_stats_job, todo, chunksize=16)):
                out[k] = s
                if (i + 1) % 2000 == 0:
                    print('  %d/%d' % (i + 1, len(todo)), flush=True)
    tmp = STATS_FILE + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump({'version': STATS_VERSION, 'textures': out}, f, separators=(',', ':'))
    os.replace(tmp, STATS_FILE)
    print('stats: %d textures in %.1fs -> %s' % (len(out), time.time() - t0, STATS_FILE), flush=True)


# ---- bake --------------------------------------------------------------------------------

def _material_docs():
    root = os.path.join(CACHE, 'materials')
    for pkg in sorted(os.listdir(root)):
        d = os.path.join(root, pkg)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith('.json'):
                p = os.path.join(d, fn)
                try:
                    with open(p, 'r', encoding='utf-8') as f:
                        yield p, json.load(f)
                except (OSError, ValueError):
                    continue


def step_bake():
    import numpy as np
    from PIL import Image
    t0 = time.time()
    g = mr.Graph(CACHE)
    n = 0
    for _p, doc in _material_docs():
        if doc.get('class') != 'Combiner':
            continue
        props = doc.get('properties') or {}
        op = int(props.get('CombineOperation', 0) or 0)
        if mr.COMBINE_OPS.get(op) not in ('add', 'add_mask_mod'):
            continue
        def tex_node(ref):
            n = g.node(ref)
            if n is not None and n['kind'] == 'material' and n['path'].lower() == doc['path'].lower():
                n = g.node(ref, texture=True)      # same-named texture, not the Combiner itself
            return n
        t1, t2 = tex_node(props.get('Material1')), tex_node(props.get('Material2'))
        if not (t1 and t2 and t1['kind'] == 'texture' and t2['kind'] == 'texture' and t1.get('png') and t2.get('png')):
            continue
        if not (os.path.isfile(t1['png']) and os.path.isfile(t2['png'])):
            continue
        path = doc['path'] + '_Baked'
        pkg, rest = path.split('.', 1)
        safe = re.sub(r'[^A-Za-z0-9._\-]', '_', rest)
        d = os.path.join(CACHE, 'textures', re.sub(r'[^A-Za-z0-9._\-]', '_', pkg))
        png = os.path.join(d, safe + '.png')
        js = os.path.join(d, safe + '.json')
        src_m = max(os.path.getmtime(t1['png']), os.path.getmtime(t2['png']))
        if os.path.isfile(png) and os.path.isfile(js) and os.path.getmtime(png) >= src_m:
            continue
        a = Image.open(t1['png']).convert('RGBA')
        b = Image.open(t2['png']).convert('RGBA')
        size = (max(a.size[0], b.size[0]), max(a.size[1], b.size[1]))
        a = np.asarray(a.resize(size, Image.BILINEAR) if a.size != size else a, dtype=np.float32)
        b = np.asarray(b.resize(size, Image.BILINEAR) if b.size != size else b, dtype=np.float32)
        out = b.copy()
        out[..., :3] = np.clip(a[..., :3] + b[..., :3], 0, 255)   # D3DTOP_ADD (saturating)
        has_alpha = bool(t2.get('has_alpha'))
        img = Image.fromarray(out.astype(np.uint8), 'RGBA')
        if not has_alpha:
            img = img.convert('RGB')
        os.makedirs(d, exist_ok=True)
        img.save(png, optimize=False, compress_level=6)
        src_doc = t2['doc']
        meta = {
            'type': 'Texture', 'class': 'Texture', 'path': path, 'scale': src_doc.get('scale', 2.0),
            'png': os.path.basename(png), 'format': 'RGBA8', 'width': size[0], 'height': size[1],
            'mip_count': 0, 'bMasked': bool(src_doc.get('bMasked')), 'bAlphaTexture': False,
            'bTwoSided': bool(src_doc.get('bTwoSided')), 'has_alpha': has_alpha, 'png_bit_depth': 8,
            'procedural_fallback': None,
            'baked': {'combiner': doc['path'], 'operation': mr.COMBINE_OPS[op],
                      'material1': t1['path'], 'material2': t2['path'],
                      'note': 'Material1 + Material2 (saturating add); Mask ignored'},
            'material_hints': dict(src_doc.get('material_hints') or {}, blend_mode='opaque'),
        }
        with open(js, 'w', encoding='utf-8') as f:
            json.dump(meta, f, indent=1)
        n += 1
        print('  baked %s (%dx%d)' % (path, size[0], size[1]))
    print('bake: %d textures in %.1fs' % (n, time.time() - t0), flush=True)


# ---- audit -------------------------------------------------------------------------------

def old_resolution(g, doc):
    """What the importer used before: the exporter's base_texture and blend (masked base -> masked)."""
    base = doc.get('base_texture') if doc.get('base_texture_file') else None
    blend = doc.get('blend_mode') or 'opaque'
    if base and blend == 'opaque':
        t = g.node(base)
        if t is not None and t.get('masked'):
            blend = 'masked'
    if base:
        t = g.node(base)
        if t is None or not t.get('png') or not os.path.isfile(t['png']):
            base = None
    return {'base': base, 'blend': blend, 'two_sided': bool(doc.get('two_sided')), 'unlit': bool(doc.get('unlit'))}


def suspicious(g, tex_path, sibling_sizes):
    """Heuristic flags for a picked texture."""
    out = []
    if not tex_path:
        return out
    name = tex_path.rsplit('.', 1)[-1]
    m = mr.SUSPICIOUS_NAME.search(name)
    if m:
        out.append('name:' + m.group(1).lower())
    if mr.LIGHTMAP_NAME.search(name) and 'name:lightmap' not in out:
        out.append('name:lightmap')
    s = g.tex_stats(tex_path)
    if s and 'error' not in s:
        if s['lum'] < 0.06 or s['black'] > 0.8:
            out.append('near_black')
        elif s['lum'] > 0.9 and s['std'] < 0.05:
            out.append('near_white')
        if s['std'] < 0.015 and s['chroma'] < 0.03:
            out.append('flat')
    n = g.node(tex_path)
    if n is not None and sibling_sizes:
        area = (n['w'] or 1) * (n['h'] or 1)
        if area * 16 <= max(sibling_sizes):
            out.append('smaller_than_siblings')
    return out


def graph_textures(g, path, depth=0, seen=None):
    """Every texture reachable from a material (for the sibling-size heuristic)."""
    seen = set() if seen is None else seen
    n = g.node(path)
    if n is None or depth > 24 or n['path'].lower() in seen:
        return []
    seen.add(n['path'].lower())
    if n['kind'] == 'texture':
        return [n]
    out = []
    for k, v in n['props'].items():
        vals = v if isinstance(v, list) else [v]
        for x in vals:
            if isinstance(x, dict):
                x = x.get('Material')
            if mr.is_ref(x) and k not in ('DefaultMaterial',):
                out += graph_textures(g, x, depth + 1, seen)
    return out


def map_materials(maps):
    """Material paths used by the given exported maps (BSP, meshes they place, actor skins)."""
    used = set()
    for m in maps:
        p = os.path.join(CACHE, 'maps', m, 'map.json')
        if not os.path.isfile(p):
            continue
        with open(p, 'r', encoding='utf-8') as f:
            d = json.load(f)
        for x in (d.get('dependencies') or {}).get('materials') or []:
            used.add(x['path'].lower())
        for x in (d.get('dependencies') or {}).get('textures') or []:
            used.add(x['path'].lower())
    return used


def step_audit(maps):
    t0 = time.time()
    g = mr.Graph(CACHE)
    res = mr.Resolver(g)
    docs = list(_material_docs())
    rules_new, rules_old = collections.Counter(), collections.Counter()
    flags_old, flags_new = collections.Counter(), collections.Counter()
    change = collections.Counter()
    resolver_flags = collections.Counter()
    fixes, rows = [], []
    for _p, doc in docs:
        path = doc['path']
        r = res.resolve(path)
        old = old_resolution(g, doc)
        sib = [(t['w'] or 1) * (t['h'] or 1) for t in graph_textures(g, path)]
        f_old = suspicious(g, old['base'], sib)
        f_new = suspicious(g, r.base, sib)
        rules_new[r.rule] += 1
        old_rule = 'texture' if doc.get('class') in ('Texture',) else (doc.get('class') + '>first_texture_dfs')
        rules_old[old_rule] += 1
        for f in f_old:
            flags_old[f] += 1
        for f in f_new:
            flags_new[f] += 1
        for f in set(r.flags):
            resolver_flags[f] += 1
        what = []
        if (old['base'] or '').lower() != (r.base or '').lower():
            what.append('base')
        if old['blend'] != r.blend:
            what.append('blend')
        if old['two_sided'] != r.two_sided:
            what.append('two_sided')
        if old['unlit'] != r.unlit:
            what.append('unlit')
        params = r.params()
        for k in params:
            what.append('param:' + k)
        for w in what:
            change[w] += 1
        row = {'path': path, 'class': doc.get('class'), 'rule': r.rule, 'steps': r.steps,
               'old_base': old['base'], 'new_base': r.base, 'old_blend': old['blend'], 'new_blend': r.blend,
               'old_flags': f_old, 'new_flags': f_new, 'flags': sorted(set(r.flags)), 'params': params,
               'changes': what}
        rows.append(row)
        if what:
            fixes.append({'path': path, 'changes': what, 'old_base': old['base'], 'new_base': r.base,
                          'old_blend': old['blend'], 'new_blend': r.blend, 'rule': r.rule})
    # per-pattern counts of what changed the base
    base_changes = collections.Counter()
    for row in rows:
        if 'base' in row['changes']:
            base_changes[row['rule']] += 1
    used = map_materials(maps) if maps else None
    os.makedirs(OUT_DIR, exist_ok=True)
    summary = {
        'materials': len(docs),
        'rules_new': dict(rules_new.most_common()),
        'rules_old': dict(rules_old.most_common()),
        'suspicious_old': dict(flags_old.most_common()),
        'suspicious_new': dict(flags_new.most_common()),
        'changes': dict(change.most_common()),
        'base_changes_by_rule': dict(base_changes.most_common()),
        'resolver_flags': dict(resolver_flags.most_common()),
        'fixes': len(fixes),
    }
    with open(os.path.join(OUT_DIR, 'material_audit.json'), 'w', encoding='utf-8') as f:
        json.dump({'summary': summary, 'materials': rows}, f, indent=1)
    with open(os.path.join(OUT_DIR, 'material_fixes.json'), 'w', encoding='utf-8') as f:
        json.dump({'resolver_version': mr.RESOLVER_VERSION, 'created': time.strftime('%Y-%m-%d %H:%M:%S'),
                   'fixes': fixes}, f, indent=1)
    print(json.dumps(summary, indent=1))
    if used is not None:
        hit = [x for x in fixes if x['path'].lower() in used]
        print('maps %s: %d of the fixes are used there' % (' '.join(maps), len(hit)))
        for x in hit:
            print('  %-60s %s -> %s %s' % (x['path'], x['old_base'], x['new_base'], ','.join(x['changes'])))
    print('audit: %d materials, %d fixes in %.1fs -> %s' % (len(docs), len(fixes), time.time() - t0, OUT_DIR))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--steps', default='stats,bake,audit')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--maps', nargs='*', default=None, help='audit: also list the fixes used by these maps')
    a = ap.parse_args()
    steps = a.steps.split(',')
    if 'stats' in steps:
        step_stats(a.jobs)
    if 'bake' in steps:
        step_bake()
        if 'stats' in steps:
            step_stats(a.jobs)     # sample the baked textures too
    if 'audit' in steps:
        step_audit(a.maps)


if __name__ == '__main__':
    main()
