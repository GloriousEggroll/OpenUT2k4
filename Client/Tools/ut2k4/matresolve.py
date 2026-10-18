"""UT2004 material graph -> one base texture plus UE material-instance parameters.

Pure Python (standard library only): runs in plain python3 (Tools/ut2k4_materials.py audit / bake)
and inside the editor (Tools/ImportUT2004.py MaterialInfo). It works on the exporter's ImportCache
files (materials/<Pkg>/<Group.Name>.json and textures/<Pkg>/<Group.Name>.json), so materials can
be re-resolved without reading the UT2004 packages again.

The UE side has one texture per material (masters M_Imported*), so a UT2004 material graph is
reduced to the texture that carries its colour:

  Texture                 itself (Cubemap: face 0, flagged)
  Shader                  Diffuse, then SelfIllumination, then FallbackMaterial; Specular / Opacity only as
                          a last resort (they are env maps and masks, not colour)
  Combiner                by CombineOperation: Use_Color_From_Material1/2 and Use_Color_From_Mask pick that
                          input; Multiply / Add / Subtract pick the input that is not a lightmap, colour mask,
                          detail or flat texture; AlphaBlend_With_Mask picks the input the mask shows most.
                          A FallbackMaterial that resolves to one of the inputs' textures wins (it is the
                          author's own single-texture version). Add of two plain textures is baked into one
                          texture by Tools/ut2k4_materials.py (team skins: diffuse + team colour layer).
  FinalBlend, TexModifier family, ColorModifier, MaterialSwitch, MaterialSequence
                          follow Material (Materials[Current], SequenceItems[0]); TexScaler / fixed TexRotator
                          matrices and TexPanner pan rates along the chosen path become the UV transform.

Texture statistics (mean colour, contrast, alpha coverage) come from ImportCache/texture_stats.json,
written by Tools/ut2k4_materials.py; without them only names, sizes and the graph are used.
"""

import json
import math
import os
import re

RESOLVER_VERSION = 2

# UE2 class defaults (Engine.u) for properties the exporter omits when they are default
COMBINE_OPS = {0: 'material1', 1: 'material2', 2: 'multiply', 3: 'add', 4: 'subtract',
               5: 'alphablend_mask', 6: 'add_mask_mod', 7: 'mask'}
SHADER_OUTPUT = {0: 'Normal', 1: 'Masked', 2: 'Modulate', 3: 'Translucent',
                 4: 'Invisible', 5: 'Brighten', 6: 'Darken'}
FB_BLEND = {0: 'Overwrite', 1: 'Modulate', 2: 'AlphaBlend', 3: 'AlphaModulate',
            4: 'Translucent', 5: 'Darken', 6: 'Brighten', 7: 'Invisible'}
_SHADER_BLEND = {0: 'opaque', 1: 'masked', 2: 'modulate', 3: 'additive',
                 4: 'invisible', 5: 'additive', 6: 'modulate'}
_FB_TO_BLEND = {0: 'opaque', 1: 'modulate', 2: 'translucent', 3: 'translucent',
                4: 'additive', 5: 'modulate', 6: 'additive', 7: 'invisible'}
TCS_NOCHANGE = 13

# names of textures that are not colour: masks, lightmaps, detail, specular, env / cube maps, bump
SUSPICIOUS_NAME = re.compile(r'(mask|msk|spec|detail|alpha|opac|env|cube|bump|normal|lightmap|shadow)', re.I)
LIGHTMAP_NAME = re.compile(r'(lightmap|LM\d*$|LM_|_LM)')
NON_COLOUR_NAME = re.compile(r'(mask|msk|spec|detail|opac|bump|normal|lightmap|shadow|LM\d*$|LM_|_LM)', re.I)

IDENTITY_UV = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0)   # a, b, c, d, e, f, pan_u, pan_v


def _lower_path(path):
    return path.lower() if isinstance(path, str) else None


def is_ref(v):
    return isinstance(v, str) and v and v != 'None'


class Graph:
    """ImportCache lookups (case-insensitive, like UE2 object paths)."""

    def __init__(self, cache_dir, stats=None):
        self.cache_dir = cache_dir
        self._files = {'materials': {}, 'textures': {}}
        for kind in self._files:
            root = os.path.join(cache_dir, kind)
            if not os.path.isdir(root):
                continue
            for pkg in os.listdir(root):
                d = os.path.join(root, pkg)
                if not os.path.isdir(d):
                    continue
                for fn in os.listdir(d):
                    if fn.endswith('.json'):
                        self._files[kind][(pkg + '.' + fn[:-5]).lower()] = os.path.join(d, fn)
        self._nodes = {}
        if stats is None:
            stats = load_stats(cache_dir)
        self.stats = stats

    @staticmethod
    def _key(path):
        pkg, _, rest = path.partition('.')
        safe = re.sub(r'[^A-Za-z0-9._\-]', '_', pkg) + '.' + re.sub(r'[^A-Za-z0-9._\-]', '_', rest)
        return safe.lower()

    def file_of(self, kind, path):
        return self._files[kind].get(self._key(path))

    def node(self, path, texture=False):
        """{'path', 'kind', 'class', 'chain', 'props', ...} or None when the object is not in the cache.
        A material and a texture can share a path (e.g. a Combiner named after its own colour layer):
        the material wins unless texture=True."""
        if not is_ref(path):
            return None
        k = path.lower() + ('#tex' if texture else '')
        if k in self._nodes:
            return self._nodes[k]
        n = None
        mf = None if texture else self.file_of('materials', path)
        tf = self.file_of('textures', path)
        try:
            if mf:
                with open(mf, 'r', encoding='utf-8') as f:
                    d = json.load(f)
                n = {'path': d.get('path') or path, 'kind': 'material', 'class': d.get('class', ''),
                     'chain': d.get('class_chain') or [d.get('class', '')], 'props': d.get('properties') or {},
                     'json': mf, 'doc': d}
            elif tf:
                with open(tf, 'r', encoding='utf-8') as f:
                    d = json.load(f)
                png = os.path.join(os.path.dirname(tf), d['png']) if d.get('png') else None
                n = {'path': d.get('path') or path, 'kind': 'texture', 'class': d.get('class', 'Texture'),
                     'chain': [d.get('class', 'Texture'), 'Texture'], 'props': {}, 'json': tf, 'doc': d,
                     'png': png, 'w': d.get('width') or 0, 'h': d.get('height') or 0,
                     'masked': bool(d.get('bMasked')), 'alpha_tex': bool(d.get('bAlphaTexture')),
                     'two_sided': bool(d.get('bTwoSided')), 'has_alpha': bool(d.get('has_alpha')),
                     'procedural': d.get('procedural_fallback'), 'baked': d.get('baked')}
        except (OSError, ValueError, KeyError):
            n = None
        self._nodes[k] = n
        return n

    def tex_stats(self, path):
        n = self.node(path)
        if n is None:
            return None
        s = n['doc'].get('stats') if n['kind'] == 'texture' else None
        return s or self.stats.get(n['path'].lower())


def load_stats(cache_dir):
    p = os.path.join(cache_dir, 'texture_stats.json')
    if not os.path.isfile(p):
        return {}
    try:
        with open(p, 'r', encoding='utf-8') as f:
            d = json.load(f)
        return {k.lower(): v for k, v in (d.get('textures') or {}).items()}
    except (OSError, ValueError):
        return {}


def _mul(a, b):
    return tuple(x * y for x, y in zip(a, b)) if a is not None else tuple(b)


def is_a(node, *names):
    return node is not None and any(c in node['chain'] for c in names)


def _num(v, default):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _matrix(props):
    """UE2 FMatrix rows (XPlane, YPlane, ZPlane) -> 2D affine (a, b, c, d, e, f): u' = u a + v c + e,
    v' = u b + v d + f (row vectors, as UE2 multiplies texture coordinates)."""
    m = props.get('M')
    if not isinstance(m, dict):
        return None
    try:
        x, y, z = m['XPlane'], m['YPlane'], m['ZPlane']
        return (float(x['X']), float(x['Y']), float(y['X']), float(y['Y']), float(z['X']), float(z['Y']))
    except (KeyError, TypeError, ValueError):
        return None


def _compose(uv, step):
    """uv (outer, applied first) then step (inner). Both (a, b, c, d, e, f, pu, pv)."""
    a, b, c, d, e, f, pu, pv = uv
    A, B, C, D, E, F, PU, PV = step
    return (a * A + b * C, a * B + b * D,
            c * A + d * C, c * B + d * D,
            e * A + f * C + E, e * B + f * D + F,
            pu * A + pv * C + PU, pu * B + pv * D + PV)


def modifier_uv(node, n_tex=None):
    """UV step of one TexModifier: (step tuple or None, flag or None)."""
    p = node['props']
    cls = node['class']
    tcs = int(_num(p.get('TexCoordSource', TCS_NOCHANGE), TCS_NOCHANGE))
    if is_a(node, 'TexEnvMap') or (tcs != TCS_NOCHANGE and not is_a(node, 'TexPanner', 'TexScaler',
                                                                    'TexRotator', 'TexOscillator')):
        return None, 'texgen'
    if is_a(node, 'TexScaler'):
        m = _matrix(p)
        if m is None:
            us, vs = _num(p.get('UScale', 1.0), 1.0) or 1.0, _num(p.get('VScale', 1.0), 1.0) or 1.0
            m = (1.0 / us, 0.0, 0.0, 1.0 / vs, 0.0, 0.0)
        return m + (0.0, 0.0), None
    if is_a(node, 'TexPanner'):          # also VariableTexPanner (rate set by script: use the stored one)
        rate = _num(p.get('PanRate', 0.1), 0.1)
        rot = p.get('PanDirection') or {}
        yaw = _num(rot.get('Yaw', 0), 0.0) * 2.0 * math.pi / 65536.0
        pitch = _num(rot.get('Pitch', 0), 0.0) * 2.0 * math.pi / 65536.0
        return (1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                rate * math.cos(pitch) * math.cos(yaw), rate * math.cos(pitch) * math.sin(yaw)), None
    if is_a(node, 'TexRotator'):
        if int(_num(p.get('TexRotationType', 0), 0)) == 0:       # TR_FixedRotation: static matrix
            m = _matrix(p)
            if m is not None:
                return m + (0.0, 0.0), None
        return None, 'rotating'
    if is_a(node, 'TexOscillator'):
        return None, 'oscillating'
    return None, None


class Resolution:
    __slots__ = ('path', 'base', 'base_png', 'base_size', 'rule', 'steps', 'flags', 'blend', 'two_sided',
                 'unlit', 'uv', 'emissive', 'emissive_alpha', 'roughness', 'specular', 'chain', 'candidates',
                 'shader_output_blending', 'frame_buffer_blending', 'alpha_ref', 'found', 'tint')

    def __init__(self, path):
        self.path = path
        self.base = None
        self.base_png = None
        self.base_size = None
        self.rule = 'none'
        self.steps = []
        self.flags = []
        self.blend = None
        self.two_sided = False
        self.unlit = False
        self.uv = IDENTITY_UV
        self.emissive = 0.0
        self.emissive_alpha = 0.0
        self.roughness = None
        self.specular = None
        self.chain = []
        self.candidates = []
        self.shader_output_blending = None
        self.frame_buffer_blending = None
        self.alpha_ref = None
        self.found = False
        self.tint = None

    def params(self):
        """Material-instance parameters that differ from the master defaults."""
        out = {}
        if any(abs(x - y) > 1e-6 for x, y in zip(self.uv, IDENTITY_UV)):
            out['uv'] = [round(x, 6) for x in self.uv]
        if self.tint is not None and any(abs(x - 1.0) > 1e-3 for x in self.tint):
            out['Tint'] = [round(x, 4) for x in self.tint]
        if self.emissive:
            out['Emissive'] = self.emissive
        if self.emissive_alpha:
            out['EmissiveAlpha'] = self.emissive_alpha
        if self.roughness is not None:
            out['Roughness'] = self.roughness
        if self.specular is not None:
            out['Specular'] = self.specular
        return out

    def to_json(self):
        return {'resolver_version': RESOLVER_VERSION, 'base_texture': self.base, 'base_size': self.base_size,
                'rule': self.rule, 'steps': self.steps, 'flags': sorted(set(self.flags)),
                'blend_mode': self.blend, 'two_sided': self.two_sided, 'unlit': self.unlit,
                'params': self.params(), 'chain': self.chain}


class _Pick:
    """A texture reached from a sub-graph, with the nodes and UV transform on the way."""
    __slots__ = ('tex', 'path_nodes', 'uv', 'steps', 'flags', 'tint')

    def __init__(self, tex, path_nodes, uv, steps, flags):
        self.tex = tex
        self.path_nodes = path_nodes
        self.uv = uv
        self.steps = steps
        self.flags = flags
        self.tint = None        # linear RGB multiplied in (Combiner Multiply with a flat colour)


def _srgb_to_linear(c):
    c = max(0.0, min(1.0, float(c)))
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


class Resolver:
    def __init__(self, graph):
        self.g = graph
        self._cache = {}

    # ---- texture scoring --------------------------------------------------------------
    def penalty(self, pick, op=None):
        """How unlikely a picked texture is to be the colour of the material (0 = fine)."""
        if pick is None:
            return 100.0
        n = pick.tex
        name = n['path'].rsplit('.', 1)[-1]
        pen = 0.0
        if LIGHTMAP_NAME.search(name) or any(LIGHTMAP_NAME.search(x['path'].rsplit('.', 1)[-1]) or
                                             'lightmap' in x['path'].lower() for x in pick.path_nodes):
            pen += 3.0
        elif NON_COLOUR_NAME.search(name):
            pen += 1.5
        if 'texgen' in pick.flags:
            pen += 1.0
        if is_a(n, 'Cubemap') or 'cubemap' in pick.flags:
            pen += 2.0
        # stretched far over the surface (lightmap-style scaler)
        a, b, c, d = pick.uv[:4]
        if abs(a * d - b * c) < 1.0 / 64.0:
            pen += 1.0
        s = self.g.tex_stats(n['path'])
        if s:
            lum, std = s.get('lum', 0.5), s.get('std', 0.1)
            if lum < 0.06 or s.get('black', 0.0) > 0.8:
                pen += 2.5 if op in ('add', 'add_mask_mod') else 1.5
            if std < 0.015:
                pen += 1.0
            if lum > 0.9 and std < 0.05:
                pen += 1.0
        if n.get('procedural'):
            pen += 0.5
        return pen

    # ---- graph walk -------------------------------------------------------------------
    def _walk(self, path, depth, seen, flags_in=()):
        """Best texture of a sub-graph: _Pick or None."""
        n = self.g.node(path)
        if n is not None and n['kind'] == 'material' and n['path'].lower() in seen:
            n = self.g.node(path, texture=True)     # a reference to a same-named texture, not a cycle
        if n is None or depth > 24 or (n['kind'] == 'material' and n['path'].lower() in seen):
            return None
        seen = seen | {n['path'].lower()}
        p = n['props']
        if n['kind'] == 'texture':
            if n['w'] == 0 and n['h'] == 0 and not n.get('png'):
                return None
            return _Pick(n, [n], IDENTITY_UV, ['Texture'], list(flags_in))
        if is_a(n, 'Cubemap'):
            face = p.get('Faces') if not isinstance(p.get('Faces'), list) else (p['Faces'] or [None])[0]
            sub = self._walk(face, depth + 1, seen) if is_ref(face) else None
            if sub is None:
                sub = self._walk(p.get('FallbackMaterial'), depth + 1, seen)
            return self._through(n, sub, 'Cubemap.Face0', ['cubemap'])
        if is_a(n, 'Shader'):
            for key, flag in (('Diffuse', None), ('SelfIllumination', 'selfillum_base'),
                              ('FallbackMaterial', 'fallback'), ('Specular', 'specular_base'),
                              ('Opacity', 'opacity_base')):
                sub = self._walk(p.get(key), depth + 1, seen)
                if sub is not None:
                    return self._through(n, sub, 'Shader.' + key, [flag] if flag else [])
            return None
        if is_a(n, 'Combiner'):
            return self._combiner(n, depth, seen)
        if is_a(n, 'MaterialSwitch'):
            mats = p.get('Materials')
            cur = int(_num(p.get('Current', 0), 0))
            cand = []
            if isinstance(mats, list) and mats:
                cand = [mats[cur] if 0 <= cur < len(mats) else mats[0]] + mats
            for c in cand + [p.get('Material'), p.get('FallbackMaterial')]:
                sub = self._walk(c, depth + 1, seen)
                if sub is not None:
                    return self._through(n, sub, 'MaterialSwitch.Current', [])
            return None
        if is_a(n, 'MaterialSequence'):
            items = p.get('SequenceItems')
            cand = []
            if isinstance(items, list):
                cand = [it.get('Material') if isinstance(it, dict) else it for it in items]
            for c in cand + [p.get('FallbackMaterial')]:
                sub = self._walk(c, depth + 1, seen)
                if sub is not None:
                    return self._through(n, sub, 'MaterialSequence.Item0', [])
            return None
        if is_a(n, 'Modifier'):        # FinalBlend, TexModifier family, ColorModifier, ...
            sub = self._walk(p.get('Material'), depth + 1, seen)
            label = n['class'] + '.Material'
            if sub is None:
                sub = self._walk(p.get('FallbackMaterial'), depth + 1, seen)
                label = n['class'] + '.FallbackMaterial'
            if sub is None:
                return None
            flags = []
            if is_a(n, 'TexModifier'):
                step, flag = modifier_uv(n)
                if flag:
                    flags.append(flag)
                if step is not None:
                    sub.uv = _compose(step, sub.uv)
            return self._through(n, sub, label, flags)
        # ConstantColor, FadeColor, VertexColor, ScriptedTexture, ...: no texture of their own
        sub = self._walk(p.get('FallbackMaterial'), depth + 1, seen)
        return self._through(n, sub, n['class'] + '.FallbackMaterial', ['fallback']) if sub else None

    @staticmethod
    def _through(n, sub, label, flags):
        if sub is None:
            return None
        sub.path_nodes = [n] + sub.path_nodes
        sub.steps = [label] + sub.steps
        sub.flags = sub.flags + flags
        return sub

    def _combiner(self, n, depth, seen):
        p = n['props']
        op = COMBINE_OPS.get(int(_num(p.get('CombineOperation', 0), 0)), 'material1')
        m1 = self._walk(p.get('Material1'), depth + 1, seen)
        m2 = self._walk(p.get('Material2'), depth + 1, seen)
        mask = self._walk(p.get('Mask'), depth + 1, seen) if is_ref(p.get('Mask')) else None
        label = 'Combiner.' + op

        # baked by Tools/ut2k4_materials.py: Add of two plain textures (team skins)
        baked = self.g.node(n['path'] + '_Baked')
        if baked is not None and baked['kind'] == 'texture' and baked.get('baked'):
            return _Pick(baked, [n, baked], IDENTITY_UV, [label + '>baked', 'Texture'], ['baked'])

        # the author's single-texture fallback names the texture that matters
        fb = p.get('FallbackMaterial')
        if is_ref(fb) and (m1 or m2):
            f = self._walk(fb, depth + 1, seen)
            if f is not None:
                for which, m in (('Material1', m1), ('Material2', m2)):
                    if m is not None and m.tex['path'].lower() == f.tex['path'].lower():
                        return self._through(n, m, label + '>fallback_hint.' + which, [])

        def choose(first, second, why):
            if first is not None:
                return first, why
            return second, why + '>other_missing'

        if op == 'material1':
            pick, why = choose(m1, m2, 'Material1')
        elif op == 'material2':
            pick, why = choose(m2, m1, 'Material2')
        elif op == 'mask':
            pick, why = choose(mask, m1, 'Mask')
        elif op == 'alphablend_mask':
            w2 = self._mask_weight(mask, p)
            if w2 is None:
                pick, why = self._by_penalty(m1, m2, op)
            elif w2 >= 0.5:
                pick, why = choose(m2, m1, 'Material2.mask%.2f' % w2)
            else:
                pick, why = choose(m1, m2, 'Material1.mask%.2f' % (1.0 - w2))
            # a lightmap / colour mask never wins over a real texture, whatever the coverage
            other = m1 if pick is m2 else m2
            if pick is not None and other is not None and self.penalty(pick, op) >= self.penalty(other, op) + 2.0:
                pick, why = other, why + '>penalty_swap'
        elif op == 'multiply':
            # a flat colour (ConstantColor or a single-colour texture) times a pattern: the pattern is
            # the Base and the colour its Tint
            c1, c2 = self._flat_colour(p.get('Material1'), m1), self._flat_colour(p.get('Material2'), m2)
            if c2 is not None and c1 is None and m1 is not None:
                pick, why = m1, 'Material1.tinted'
                pick.tint = _mul(pick.tint, c2)
            elif c1 is not None and c2 is None and m2 is not None:
                pick, why = m2, 'Material2.tinted'
                pick.tint = _mul(pick.tint, c1)
            else:
                pick, why = self._by_penalty(m1, m2, op)
        else:  # add, add_mask_mod, subtract
            pick, why = self._by_penalty(m1, m2, op)
        if pick is None:
            f = self._walk(p.get('FallbackMaterial'), depth + 1, seen)
            pick, why = f, 'FallbackMaterial'
        return self._through(n, pick, label + '>' + why, []) if pick else None

    def _flat_colour(self, ref, pick):
        """Linear RGB of a ConstantColor input or of a single-colour texture input, else None."""
        n = self.g.node(ref)
        if n is not None and n['kind'] == 'material' and is_a(n, 'ConstantColor'):
            c = n['props'].get('Color') or {}
            return tuple(_srgb_to_linear(_num(c.get(k, 0), 0.0) / 255.0) for k in ('R', 'G', 'B'))
        if pick is None or pick.tint is not None:
            return None
        s = self.g.tex_stats(pick.tex['path'])
        if s and 'mean' in s and s.get('std', 1.0) < 0.015 and s.get('chroma', 1.0) < 0.6 \
                and abs(pick.uv[0] * pick.uv[3] - pick.uv[1] * pick.uv[2]) > 1e-6:
            return tuple(_srgb_to_linear(x) for x in s['mean'])
        return None

    def _by_penalty(self, m1, m2, op):
        if m1 is None or m2 is None:
            return (m1, 'Material1') if m1 is not None else (m2, 'Material2')
        p1, p2 = self.penalty(m1, op), self.penalty(m2, op)
        if p2 < p1:
            return m2, 'Material2.penalty%.1f<%.1f' % (p2, p1)
        return m1, 'Material1' + ('.penalty%.1f<=%.1f' % (p1, p2) if p1 or p2 else '')

    def _mask_weight(self, mask, props):
        """Share of the surface showing Material2 for CO_AlphaBlend_With_Mask (mask alpha, inverted on
        InvertMask), or None without statistics."""
        if mask is None:
            return None
        s = self.g.tex_stats(mask.tex['path'])
        if not s:
            return None
        a = s.get('alpha', 1.0) if s.get('has_alpha') else s.get('lum', 0.5)
        if props.get('InvertMask'):
            a = 1.0 - a
        return a

    # ---- top level --------------------------------------------------------------------
    def resolve(self, path):
        key = (path or '').lower()
        if key in self._cache:
            return self._cache[key]
        r = Resolution(path)
        top = self.g.node(path)
        if top is not None:
            r.found = True
            pick = self._walk(path, 0, frozenset())
            self._fill(r, top, pick)
        self._cache[key] = r
        return r

    def _fill(self, r, top, pick):
        if pick is not None:
            t = pick.tex
            r.base = t['path']
            r.base_png = t.get('png') if t.get('png') and os.path.isfile(t['png']) else None
            if r.base_png is None:
                r.base = None
            r.base_size = [t.get('w'), t.get('h')]
            r.steps = pick.steps
            r.flags = list(pick.flags)
            r.uv = pick.uv
            r.tint = pick.tint
            r.chain = [x['path'] for x in pick.path_nodes]
            nodes = pick.path_nodes
        else:
            nodes = [top]
        r.rule = classify(top, pick)
        # blend / sidedness / lighting along the chosen path (outermost decides, as in UE2). Below a
        # Combiner the alpha comes from its AlphaOperation, which is not modelled: the material keeps the
        # alpha flags of the exporter's original pick (the first texture of the graph), as before.
        below_combiner = False
        for n in nodes:
            p = n['props']
            if n['kind'] == 'texture':
                if below_combiner and top['kind'] == 'material':
                    legacy = self.g.node(top['doc'].get('base_texture'), texture=True) \
                        if top['doc'].get('base_texture_file') else None
                    n = legacy if legacy is not None else n
                if r.blend is None:
                    if n.get('masked'):
                        r.blend = 'masked'
                    elif n.get('alpha_tex'):
                        r.blend = 'translucent'
                if n.get('two_sided'):
                    r.two_sided = True
                continue
            if is_a(n, 'Combiner'):
                below_combiner = True
            if is_a(n, 'Shader'):
                ob = int(_num(p.get('OutputBlending', 0), 0))
                if r.blend is None:
                    bm = _SHADER_BLEND.get(ob, 'opaque')
                    if ob == 0 and is_ref(p.get('Opacity')):
                        bm = 'translucent'
                    r.blend = bm
                    r.shader_output_blending = SHADER_OUTPUT.get(ob, ob)
                if p.get('TwoSided'):
                    r.two_sided = True
                if is_ref(p.get('SelfIllumination')) and not is_ref(p.get('Diffuse')):
                    r.unlit = True
                self._shader_extras(r, n, pick)
            elif is_a(n, 'FinalBlend'):
                fb = int(_num(p.get('FrameBufferBlending', 0), 0))
                if r.blend is None:
                    r.frame_buffer_blending = FB_BLEND.get(fb, fb)
                    bm = _FB_TO_BLEND.get(fb, 'opaque')
                    if bm == 'opaque' and p.get('AlphaTest'):
                        bm = 'masked'
                    r.blend = bm
                    if p.get('AlphaTest'):
                        r.alpha_ref = p.get('AlphaRef', 0)
                if p.get('TwoSided'):
                    r.two_sided = True
        if r.blend is None:
            r.blend = 'opaque'
        # modulate / additive surfaces ignore the lit-surface extras
        if r.blend not in ('opaque', 'masked', 'translucent'):
            r.emissive = 0.0
            r.emissive_alpha = 0.0
            r.roughness = r.specular = None
        if r.unlit:
            r.emissive = 0.0
            r.emissive_alpha = 0.0

    def _shader_extras(self, r, n, pick):
        """Self-illumination and specular of a Shader on the chosen path."""
        p = n['props']
        if pick is None or r.unlit:
            return
        base = pick.tex['path'].lower()
        if is_ref(p.get('Diffuse')) and is_ref(p.get('SelfIllumination')):
            si = self._walk(p.get('SelfIllumination'), 0, frozenset())
            msk = self._walk(p.get('SelfIlluminationMask'), 0, frozenset()) if is_ref(
                p.get('SelfIlluminationMask')) else None
            same = si is not None and si.tex['path'].lower() == base
            if msk is not None and msk.tex['path'].lower() == base and pick.tex.get('has_alpha') \
                    and same and r.blend in (None, 'opaque'):
                r.emissive_alpha = 1.0          # glows where the diffuse alpha is set
                r.flags.append('selfillum_alpha')
            elif msk is None and same:
                st = self.g.tex_stats(pick.tex['path']) or {}
                if pick.tex.get('has_alpha') and st.get('alpha', 1.0) < 0.98 and r.blend in (None, 'opaque'):
                    r.emissive_alpha = 1.0      # the self-illumination's own alpha masks the glow
                    r.flags.append('selfillum_alpha')
                else:
                    r.emissive = 1.0            # the whole diffuse glows
                    r.flags.append('selfillum_full')
            elif si is not None:
                r.flags.append('selfillum_unsupported')
        if is_ref(p.get('Specular')) and r.roughness is None:
            r.roughness = 0.5
            r.specular = 0.5
            r.flags.append('specular')


def classify(top, pick):
    """Short label of how the base texture was found (for the audit)."""
    if top is None:
        return 'missing'
    if pick is None:
        return 'none:' + top['class']
    steps = pick.steps
    head = steps[0] if steps else 'Texture'
    if top['kind'] == 'texture':
        return 'texture'
    for s in steps:
        if s.startswith('Combiner.'):
            core = s.split('>')
            op = core[0]
            how = core[1].split('.')[0] if len(core) > 1 else ''
            if 'fallback_hint' in s:
                how = 'fallback_hint'
            elif 'baked' in s:
                how = 'baked'
            elif 'penalty' in s and 'penalty_swap' not in s:
                how = how + '(scored)'
            elif 'penalty_swap' in s:
                how = how + '(swapped)'
            return '%s|%s>%s' % (head.split('.')[0], op, how) if head != s else '%s>%s' % (op, how)
    return head


def uv_size(graph, path, depth=0):
    """UE2 Material::MaterialUSize/VSize: the size BSP texture coordinates are normalised by
    (Shader: Diffuse; Combiner: the larger input; Modifier: Material; Texture: its own)."""
    n = graph.node(path)
    if n is None or depth > 24:
        return (0, 0)
    p = n['props']
    if n['kind'] == 'texture':
        return (n['w'] or 0, n['h'] or 0)
    if is_a(n, 'Shader'):
        return uv_size(graph, p.get('Diffuse'), depth + 1)
    if is_a(n, 'Combiner'):
        a = uv_size(graph, p.get('Material1'), depth + 1)
        b = uv_size(graph, p.get('Material2'), depth + 1)
        return (max(a[0], b[0]), max(a[1], b[1]))
    if is_a(n, 'Modifier'):
        return uv_size(graph, p.get('Material'), depth + 1)
    return (0, 0)
