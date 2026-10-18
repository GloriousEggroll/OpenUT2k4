"""UE2 Texture decoding (top mip) and material resolution."""

import numpy as np

from .archive import ReadError

TEXF = {
    0: 'P8', 1: 'RGBA7', 2: 'RGB16', 3: 'DXT1', 4: 'RGB8', 5: 'RGBA8', 6: 'NODATA',
    7: 'DXT3', 8: 'DXT5', 9: 'L8', 10: 'G16', 11: 'RRRGGGBBB',
}


class TextureError(Exception):
    pass


# ---------------------------------------------------------------------------
# Native data
# ---------------------------------------------------------------------------

def read_mips(obj):
    """List of (offset, length, usize, vsize) for every mip of a Texture."""
    r = obj.native_reader()
    n = r.index()
    if n < 0 or n > 32:
        raise TextureError('bad mip count %d' % n)
    mips = []
    for _ in range(n):
        off, ln = r.lazy_bytes_span(obj.pkg.version > 61)
        us, vs = r.i32(), r.i32()
        r.u8()
        r.u8()
        mips.append((off, ln, us, vs))
    return mips


def read_palette(pal_obj):
    r = pal_obj.native_reader()
    n = r.index()
    if not (0 < n <= 256):
        raise TextureError('bad palette size %d' % n)
    data = np.frombuffer(r.bytes(4 * n), dtype=np.uint8).reshape(n, 4).copy()
    if n < 256:
        data = np.vstack([data, np.zeros((256 - n, 4), np.uint8)])
    return data  # RGBA


# ---------------------------------------------------------------------------
# Block decoders (numpy)
# ---------------------------------------------------------------------------

def _565(c):
    c = c.astype(np.uint32)
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def _color_block(blk, force4):
    """blk: (N, 8) uint8 -> (N, 16, 4) uint8 RGBA."""
    c0 = blk[:, 0].astype(np.uint16) | (blk[:, 1].astype(np.uint16) << 8)
    c1 = blk[:, 2].astype(np.uint16) | (blk[:, 3].astype(np.uint16) << 8)
    bits = (blk[:, 4].astype(np.uint32) | (blk[:, 5].astype(np.uint32) << 8)
            | (blk[:, 6].astype(np.uint32) << 16) | (blk[:, 7].astype(np.uint32) << 24))
    p0 = _565(c0)
    p1 = _565(c1)
    four = np.ones(len(blk), bool) if force4 else (c0 > c1)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    n = len(blk)
    pal = np.empty((n, 4, 4), np.uint8)
    pal[:, 0, :3] = p0
    pal[:, 1, :3] = p1
    pal[:, 2, :3] = p2
    pal[:, 3, :3] = p3
    pal[:, :, 3] = 255
    pal[:, 3, 3] = np.where(four, 255, 0)
    shifts = (np.arange(16, dtype=np.uint32) * 2)
    idx = (bits[:, None] >> shifts[None, :]) & 3
    return np.take_along_axis(pal, idx[:, :, None].astype(np.intp).repeat(4, 2), axis=1)


def _unblock(px, w, h):
    """(N,16,4) blocks in raster order -> (h, w, 4) image."""
    bw = max(1, (w + 3) // 4)
    bh = max(1, (h + 3) // 4)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def _blocks(data, w, h, bsize):
    bw = max(1, (w + 3) // 4)
    bh = max(1, (h + 3) // 4)
    need = bw * bh * bsize
    if len(data) < need:
        raise TextureError('DXT data too short (%d < %d)' % (len(data), need))
    return np.frombuffer(data, dtype=np.uint8, count=need).reshape(bw * bh, bsize)


def decode_dxt1(data, w, h):
    return _unblock(_color_block(_blocks(data, w, h, 8), False), w, h)


def decode_dxt3(data, w, h):
    b = _blocks(data, w, h, 16)
    px = _color_block(b[:, 8:], True)
    a = b[:, :8]
    lo = a & 0x0F
    hi = a >> 4
    alpha = np.empty((len(b), 16), np.uint8)
    alpha[:, 0::2] = lo * 17
    alpha[:, 1::2] = hi * 17
    px[:, :, 3] = alpha
    return _unblock(px, w, h)


def decode_dxt5(data, w, h):
    b = _blocks(data, w, h, 16)
    px = _color_block(b[:, 8:], True)
    a0 = b[:, 0].astype(np.int32)
    a1 = b[:, 1].astype(np.int32)
    bits = np.zeros(len(b), np.uint64)
    for i in range(6):
        bits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    shifts = (np.arange(16, dtype=np.uint64) * np.uint64(3))
    idx = ((bits[:, None] >> shifts[None, :]) & np.uint64(7)).astype(np.intp)
    pal = np.empty((len(b), 8), np.int32)
    pal[:, 0] = a0
    pal[:, 1] = a1
    gt = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(gt, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(gt, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(gt, pal[:, 6], 0)
    pal[:, 7] = np.where(gt, pal[:, 7], 255)
    px[:, :, 3] = np.take_along_axis(pal, idx, axis=1).astype(np.uint8)
    return _unblock(px, w, h)


# ---------------------------------------------------------------------------
# Texture -> image
# ---------------------------------------------------------------------------

def texture_info(obj):
    p = obj.props
    fmt = p.get('Format', 0)
    return {
        'format_id': fmt,
        'format': TEXF.get(fmt, 'UNKNOWN_%d' % fmt),
        'USize': p.get('USize'),
        'VSize': p.get('VSize'),
        'bMasked': bool(p.get('bMasked', False)),
        'bAlphaTexture': bool(p.get('bAlphaTexture', False)),
        'bTwoSided': bool(p.get('bTwoSided', False)),
    }


def decode_texture(obj):
    """Returns (image ndarray, info dict). Image is HxWx4 uint8, or HxW uint16 for G16."""
    info = texture_info(obj)
    mips = read_mips(obj)
    if not mips:
        raise TextureError('texture has no mipmaps')
    off, ln, w, h = mips[0]
    if w <= 0 or h <= 0:
        raise TextureError('bad size %dx%d' % (w, h))
    info['USize'], info['VSize'] = w, h
    info['mip_count'] = len(mips)
    data = obj.pkg.buf[off:off + ln]
    fmt = info['format']
    npx = w * h
    if fmt == 'DXT1':
        img = decode_dxt1(data, w, h)
    elif fmt == 'DXT3':
        img = decode_dxt3(data, w, h)
    elif fmt == 'DXT5':
        img = decode_dxt5(data, w, h)
    elif fmt == 'RGBA8':
        if ln < npx * 4:
            raise TextureError('RGBA8 data too short')
        bgra = np.frombuffer(data, np.uint8, npx * 4).reshape(h, w, 4)
        img = bgra[:, :, [2, 1, 0, 3]].copy()
    elif fmt == 'RGB8':
        if ln < npx * 3:
            raise TextureError('RGB8 data too short')
        bgr = np.frombuffer(data, np.uint8, npx * 3).reshape(h, w, 3)
        img = np.empty((h, w, 4), np.uint8)
        img[:, :, :3] = bgr[:, :, ::-1]
        img[:, :, 3] = 255
    elif fmt == 'P8':
        if ln < npx:
            raise TextureError('P8 data too short')
        pal_ref = obj.props.get('Palette')
        pal_obj = pal_ref.load() if pal_ref else None
        if pal_obj is None:
            raise TextureError('P8 texture without palette')
        pal = read_palette(pal_obj)
        if info['bMasked']:
            pal = pal.copy()
            pal[0] = (0, 0, 0, 0)
        idx = np.frombuffer(data, np.uint8, npx).reshape(h, w)
        img = pal[idx]
    elif fmt == 'L8':
        g = np.frombuffer(data, np.uint8, npx).reshape(h, w)
        img = np.empty((h, w, 4), np.uint8)
        img[:, :, 0] = img[:, :, 1] = img[:, :, 2] = g
        img[:, :, 3] = 255
    elif fmt == 'G16':
        if ln < npx * 2:
            raise TextureError('G16 data too short')
        img = np.frombuffer(data, '<u2', npx).reshape(h, w).copy()
    elif fmt == 'RGB16':
        c = np.frombuffer(data, '<u2', npx).reshape(-1)
        rgb = _565(c).astype(np.uint8).reshape(h, w, 3)
        img = np.empty((h, w, 4), np.uint8)
        img[:, :, :3] = rgb
        img[:, :, 3] = 255
    else:
        raise TextureError('unsupported texture format %s' % fmt)
    if img.ndim == 3:
        info['has_alpha'] = bool((img[:, :, 3] != 255).any())
    else:
        info['has_alpha'] = False
    return img, info


# ---------------------------------------------------------------------------
# Material graph
# ---------------------------------------------------------------------------

SHADER_OUTPUT = {0: 'Normal', 1: 'Masked', 2: 'Modulate', 3: 'Translucent',
                 4: 'Invisible', 5: 'Brighten', 6: 'Darken'}
FB_BLEND = {0: 'Overwrite', 1: 'Modulate', 2: 'AlphaBlend', 3: 'AlphaModulate',
            4: 'Translucent', 5: 'Darken', 6: 'Brighten', 7: 'Invisible'}

# simplified blend modes for the UE side
_SHADER_BLEND = {0: 'opaque', 1: 'masked', 2: 'modulate', 3: 'additive',
                 4: 'invisible', 5: 'additive', 6: 'modulate'}
_FB_TO_BLEND = {0: 'opaque', 1: 'modulate', 2: 'translucent', 3: 'translucent',
                4: 'additive', 5: 'modulate', 6: 'additive', 7: 'invisible'}


def _ref(obj, name):
    v = obj.props.get(name)
    if v is None:
        return None
    o = v.load() if v else None
    return o


def material_children(obj):
    """Material references used to find the base diffuse, in priority order."""
    if obj.is_a('Shader'):
        return ['Diffuse', 'SelfIllumination', 'Specular', 'Opacity', 'FallbackMaterial']
    if obj.is_a('Combiner'):
        return ['Material1', 'Material2', 'Mask', 'FallbackMaterial']
    if obj.is_a('MaterialSwitch'):
        cur = obj.props.get('Current', 0)
        return ['Materials#%d' % cur, 'FallbackMaterial']
    if obj.is_a('MaterialSequence'):
        return ['SequenceItems#0', 'FallbackMaterial']
    if obj.is_a('Modifier'):  # FinalBlend, TexModifier family, ...
        return ['Material', 'FallbackMaterial']
    if obj.is_a('Cubemap'):
        return ['Faces[0]' if 'Faces[0]' in obj.props else 'Faces', 'FallbackMaterial']
    return ['FallbackMaterial']


def _child(obj, key):
    if '#' in key:
        name, i = key.split('#')
        arr = obj.props.get(name)
        if isinstance(arr, list) and len(arr) > int(i):
            v = arr[int(i)]
            if isinstance(v, dict):
                v = v.get('Material')
            if v is not None and hasattr(v, 'load') and v:
                return v.load()
        return None
    return _ref(obj, key)


def has_mips(obj):
    try:
        return bool(read_mips(obj))
    except (ReadError, TextureError):
        return False


def resolve_material(obj, depth=0, seen=None):
    """Follow a material graph down to its base diffuse Texture (depth-first, in
    the priority order of material_children, backtracking past branches that
    contain no texture such as ConstantColor).

    This is the exporter's first-texture pick: it fills the material json's
    'base_texture' and sizes the BSP texture coordinates (level.py). The importer
    does not use it for the Base texture; Tools/ut2k4/matresolve.py re-resolves
    the cached graph (Combiner operations, masks, lightmaps, team colour layers).

    Returns dict: base_texture (UObject or None), blend_mode, two_sided,
    chain [{class, path}], plus emissive/opacity/specular/detail object paths."""
    out = {'base_texture': None, 'blend_mode': None, 'two_sided': False,
           'chain': [], 'emissive': None, 'opacity': None, 'specular': None,
           'detail': None, 'unlit': False}
    seen = set() if seen is None else seen

    def visit(cur, d):
        if cur is None or d > 24 or cur.path in seen:
            return None
        seen.add(cur.path)
        out['chain'].append({'class': cur.class_name, 'path': cur.path})
        p = cur.props
        if cur.is_a('Texture') and not cur.is_a('Cubemap'):
            if out['blend_mode'] is None:
                if p.get('bMasked'):
                    out['blend_mode'] = 'masked'
                elif p.get('bAlphaTexture'):
                    out['blend_mode'] = 'translucent'
            if p.get('bTwoSided'):
                out['two_sided'] = True
            if out['detail'] is None and p.get('Detail'):
                out['detail'] = p['Detail'].path
            return cur
        if cur.is_a('Shader'):
            ob = p.get('OutputBlending', 0)
            if out['blend_mode'] is None:
                bm = _SHADER_BLEND.get(ob, 'opaque')
                if ob == 0 and p.get('Opacity'):
                    bm = 'translucent'
                out['blend_mode'] = bm
                out['shader_output_blending'] = SHADER_OUTPUT.get(ob, ob)
            if p.get('TwoSided'):
                out['two_sided'] = True
            for k, dst in (('SelfIllumination', 'emissive'), ('Opacity', 'opacity'),
                           ('Specular', 'specular'), ('Detail', 'detail')):
                v = p.get(k)
                if v and out[dst] is None:
                    out[dst] = v.path
            if p.get('SelfIllumination') and not p.get('Diffuse'):
                out['unlit'] = True
        elif cur.is_a('FinalBlend'):
            fb = p.get('FrameBufferBlending', 0)
            if out['blend_mode'] is None:
                out['frame_buffer_blending'] = FB_BLEND.get(fb, fb)
                bm = _FB_TO_BLEND.get(fb, 'opaque')
                if bm == 'opaque' and p.get('AlphaTest'):
                    bm = 'masked'
                out['blend_mode'] = bm
                if p.get('AlphaTest'):
                    out['alpha_ref'] = p.get('AlphaRef', 0)
            if p.get('TwoSided'):
                out['two_sided'] = True
        elif cur.is_a('ConstantColor', 'ConstantMaterial'):
            c = p.get('Color')
            if isinstance(c, dict) and 'constant_color' not in out:
                out['constant_color'] = [c.get('R', 0), c.get('G', 0), c.get('B', 0), c.get('A', 255)]
        for key in material_children(cur):
            try:
                child = _child(cur, key)
            except Exception:
                child = None
            found = visit(child, d + 1)
            if found is not None:
                return found
        return None

    out['base_texture'] = visit(obj, depth)
    if out['blend_mode'] is None:
        out['blend_mode'] = 'opaque'
    return out


def texture_size(tex):
    """(USize, VSize) of a Texture object from props or the first mip."""
    p = tex.props
    us, vs = p.get('USize'), p.get('VSize')
    if not us or not vs:
        try:
            m = read_mips(tex)
            if m:
                us, vs = m[0][2], m[0][3]
        except (ReadError, TextureError):
            pass
    return us or 256, vs or 256
