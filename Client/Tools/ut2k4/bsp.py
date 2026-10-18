"""UE2 (UT2004) UModel BSP parsing and triangulation."""

import numpy as np


# EPolyFlags (UE2)
PF_INVISIBLE = 0x00000001
PF_MASKED = 0x00000002
PF_TRANSLUCENT = 0x00000004
PF_NOTSOLID = 0x00000008
PF_ENVIRONMENT = 0x00000010
PF_SEMISOLID = 0x00000020
PF_MODULATED = 0x00000040
PF_FAKEBACKDROP = 0x00000080
PF_TWOSIDED = 0x00000100
PF_NOSMOOTH = 0x00000800
PF_ALPHATEXTURE = 0x00001000
PF_FLAT = 0x00004000
PF_NOMERGE = 0x00010000
PF_NOZTEST = 0x00020000
PF_ADDITIVE = 0x00040000
PF_SPECIALLIT = 0x00100000
PF_WIREFRAME = 0x00200000
PF_UNLIT = 0x00400000
PF_PORTAL = 0x04000000
# UT2004 (UE2.5) uses 0x08000000 for mirrored (reflective) surfaces such as DM-Deck17's polished
# floors; they are ordinary visible, solid geometry. Anti-portals are actors in UE2.5, not poly flags.
PF_MIRRORED = 0x08000000
PF_FLAG29 = 0x20000000

POLY_FLAG_NAMES = {
    PF_INVISIBLE: 'Invisible', PF_MASKED: 'Masked', PF_TRANSLUCENT: 'Translucent',
    PF_NOTSOLID: 'NotSolid', PF_ENVIRONMENT: 'Environment', PF_SEMISOLID: 'Semisolid',
    PF_MODULATED: 'Modulated', PF_FAKEBACKDROP: 'FakeBackdrop', PF_TWOSIDED: 'TwoSided',
    PF_NOSMOOTH: 'NoSmooth', PF_ALPHATEXTURE: 'AlphaTexture', PF_FLAT: 'Flat',
    PF_NOMERGE: 'NoMerge', PF_NOZTEST: 'NoZTest', PF_ADDITIVE: 'Additive',
    PF_SPECIALLIT: 'SpecialLit', PF_WIREFRAME: 'Wireframe', PF_UNLIT: 'Unlit',
    PF_PORTAL: 'Portal', PF_MIRRORED: 'Mirrored', PF_FLAG29: 'Flag29',
}

SKIP_FLAGS = PF_INVISIBLE | PF_PORTAL | PF_FAKEBACKDROP
# flags that matter for rendering; surfaces are grouped by material + these
RENDER_FLAGS = (PF_MASKED | PF_TRANSLUCENT | PF_MODULATED | PF_TWOSIDED | PF_ALPHATEXTURE
                | PF_ADDITIVE | PF_UNLIT | PF_ENVIRONMENT | PF_MIRRORED | PF_NOTSOLID)


def flag_names(flags):
    return [n for bit, n in sorted(POLY_FLAG_NAMES.items()) if flags & bit]


class BspError(Exception):
    pass


def read_model(obj):
    """Parse a Model export: vectors, points, nodes, surfs, verts."""
    r = obj.native_reader()
    r.box()
    r.sphere()
    nvec = r.index()
    vectors = np.frombuffer(r.bytes(12 * nvec), '<f4').reshape(nvec, 3).astype(np.float64)
    npt = r.index()
    points = np.frombuffer(r.bytes(12 * npt), '<f4').reshape(npt, 3).astype(np.float64)
    nn = r.index()
    if nn < 0 or nn > 4000000:
        raise BspError('bad node count')
    n_plane = np.empty((nn, 4))
    n_vpool = np.empty(nn, np.int64)
    n_surf = np.empty(nn, np.int64)
    n_nverts = np.empty(nn, np.int64)
    n_flags = np.empty(nn, np.int64)
    n_zone = np.empty((nn, 2), np.int64)
    idx = r.index
    for i in range(nn):
        n_plane[i] = r.plane()
        r.u64()                 # ZoneMask
        n_flags[i] = r.u8()     # NodeFlags
        n_vpool[i] = idx()      # iVertPool
        n_surf[i] = idx()       # iSurf
        idx()
        idx()
        idx()                   # iBack, iFront, iPlane
        idx()
        idx()                   # iCollisionBound, iRenderBound
        r.plane()               # ExclusiveSphereBound
        n_zone[i, 0] = r.u8()
        n_zone[i, 1] = r.u8()
        n_nverts[i] = r.u8()
        r.skip(20)              # iLeaf[2], iSection, iFirstVertex, iLightMap
    ns = r.index()
    if ns < 0 or ns > 4000000:
        raise BspError('bad surf count')
    surfs = []
    for _ in range(ns):
        mat = idx()
        flags = r.u32()
        pbase = idx()
        vnormal = idx()
        vtu = idx()
        vtv = idx()
        brushpoly = idx()
        actor = idx()
        plane = r.plane()
        lms = r.f32()
        surfs.append((mat, flags, pbase, vnormal, vtu, vtv, brushpoly, actor, plane, lms))
    nv = r.index()
    if nv < 0 or nv > 20000000:
        raise BspError('bad vert count')
    verts = np.empty(nv, np.int64)
    for i in range(nv):
        verts[i] = idx()
        idx()                   # iSide
    model = {
        'vectors': vectors, 'points': points,
        'node_plane': n_plane, 'node_vpool': n_vpool, 'node_surf': n_surf,
        'node_nverts': n_nverts, 'node_flags': n_flags, 'node_zone': n_zone,
        'surfs': surfs, 'verts': verts,
    }
    _validate(model)
    return model


def _validate(m):
    """Every node polygon vertex must lie on the node plane; catches layout drift."""
    nn = len(m['node_vpool'])
    if nn == 0:
        return
    vp, cnt = m['node_vpool'], m['node_nverts']
    if (vp < 0).any() or ((vp + cnt) > len(m['verts'])).any():
        raise BspError('node vertex pool out of range')
    if (m['node_surf'] < 0).any() or (m['node_surf'] >= len(m['surfs'])).any():
        raise BspError('node surface index out of range')
    sample = np.arange(0, nn, max(1, nn // 500))
    bad = 0
    total = 0
    pts = m['points']
    for i in sample:
        pl = m['node_plane'][i]
        for k in range(cnt[i]):
            pi = m['verts'][vp[i] + k]
            if not (0 <= pi < len(pts)):
                raise BspError('vertex index out of range')
            d = abs(pts[pi] @ pl[:3] - pl[3])
            total += 1
            if d > 2.0:
                bad += 1
    if total and bad > total * 0.02:
        raise BspError('node vertices are not on their planes (%d/%d): unsupported layout'
                       % (bad, total))


def triangulate(model, surf_material_path, texture_size_for, scale):
    """Build glTF primitive dicts grouped by (material, render flags).

    surf_material_path(ref_index) -> material path string
    texture_size_for(material_path) -> (USize, VSize)
    """
    pts = model['points']
    vecs = model['vectors']
    surfs = model['surfs']
    verts = model['verts']
    groups = {}
    skipped = {'invisible_or_portal': 0, 'degenerate': 0}
    mat_cache = {}
    for i in range(len(model['node_vpool'])):
        n = int(model['node_nverts'][i])
        if n < 3:
            continue
        s = surfs[model['node_surf'][i]]
        mat_ref, flags = s[0], s[1]
        if flags & SKIP_FLAGS:
            skipped['invisible_or_portal'] += 1
            continue
        mp = mat_cache.get(mat_ref)
        if mp is None:
            mp = surf_material_path(mat_ref)
            mat_cache[mat_ref] = mp
        key = (mp, flags & RENDER_FLAGS)
        g = groups.get(key)
        if g is None:
            g = groups[key] = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'count': 0,
                               'surfs': set(), 'flags': flags & RENDER_FLAGS}
        vp = int(model['node_vpool'][i])
        poly = pts[verts[vp:vp + n]]
        normal = vecs[s[3]] if 0 <= s[3] < len(vecs) else model['node_plane'][i][:3]
        # Newell normal of the polygon as stored
        nx = poly - np.roll(poly, -1, axis=0)
        sx = poly + np.roll(poly, -1, axis=0)
        newell = np.array([(nx[:, 1] * sx[:, 2]).sum(), (nx[:, 2] * sx[:, 0]).sum(),
                           (nx[:, 0] * sx[:, 1]).sum()])
        if np.linalg.norm(newell) < 1e-6:
            skipped['degenerate'] += 1
            continue
        base = pts[s[2]] if 0 <= s[2] < len(pts) else poly[0]
        tu = vecs[s[4]] if 0 <= s[4] < len(vecs) else np.zeros(3)
        tv = vecs[s[5]] if 0 <= s[5] < len(vecs) else np.zeros(3)
        us, vs = texture_size_for(mp)
        rel = poly - base
        u = (rel @ tu) / float(us)
        v = (rel @ tv) / float(vs)
        b = g['count']
        g['pos'].append(poly)
        g['nrm'].append(np.repeat(normal[None, :], n, 0))
        g['uv'].append(np.stack([u, v], 1))
        # Unreal-native winding: geometric normal (right-hand rule) opposite the face normal
        if newell @ normal > 0:
            fan = [(b, b + k + 1, b + k) for k in range(1, n - 1)]
        else:
            fan = [(b, b + k, b + k + 1) for k in range(1, n - 1)]
        g['idx'].extend(fan)
        g['count'] += n
        g['surfs'].add(int(model['node_surf'][i]))
    prims = []
    for (mp, fl), g in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        prims.append({
            'positions': np.concatenate(g['pos']) * scale,
            'normals': np.concatenate(g['nrm']),
            'uvs': np.concatenate(g['uv']),
            'indices': np.asarray(g['idx'], np.uint32).reshape(-1),
            'material': mp,
            'extras': {'poly_flags': fl, 'poly_flag_names': flag_names(fl),
                       'surface_count': len(g['surfs'])},
        })
    return prims, skipped
