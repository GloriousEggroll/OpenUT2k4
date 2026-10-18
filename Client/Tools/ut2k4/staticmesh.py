"""UE2 StaticMesh parsing (UT2004, package version >= 112)."""

import numpy as np

from .archive import ReadError


class MeshError(Exception):
    pass


def read_static_mesh(obj):
    """Returns dict with sections, positions (N,3), normals (N,3), uvs [ (N,2) ... ],
    indices (M,), bounds, collision_model ref index."""
    if obj.pkg.version < 112:
        raise MeshError('StaticMesh version %d not supported' % obj.pkg.version)
    r = obj.native_reader()
    bbox = r.box()
    sphere = r.sphere()
    nsec = r.index()
    if not (0 <= nsec < 4096):
        raise MeshError('bad section count %d' % nsec)
    sections = []
    for _ in range(nsec):
        f4 = r.i32()
        first_index = r.u16()
        first_vertex = r.u16()
        last_vertex = r.u16()
        r.u16()
        num_faces = r.u16()
        sections.append({'f4': f4, 'first_index': first_index, 'first_vertex': first_vertex,
                         'last_vertex': last_vertex, 'num_faces': num_faces})
    r.box()  # bounding box serialized twice
    nv = r.index()
    if nv < 0 or nv * 24 > r.remaining():
        raise MeshError('bad vertex count %d' % nv)
    vdata = np.frombuffer(r.bytes(nv * 24), '<f4').reshape(nv, 6)
    r.i32()  # revision
    nc = r.index()
    r.skip(4 * nc)
    r.i32()
    na = r.index()
    r.skip(4 * na)
    r.i32()
    nuvs = r.index()
    uvs = []
    for _ in range(nuvs):
        c = r.index()
        uv = np.frombuffer(r.bytes(c * 8), '<f4').reshape(c, 2)
        r.i32()
        r.i32()
        uvs.append(uv)
    ni = r.index()
    indices = np.frombuffer(r.bytes(ni * 2), '<u2').astype(np.uint32)
    r.i32()
    ni2 = r.index()
    r.skip(ni2 * 2)
    r.i32()
    collision_ref = None
    try:
        collision_ref = r.index()
    except ReadError:
        pass
    return {
        'bbox': bbox, 'sphere': sphere, 'sections': sections,
        'positions': vdata[:, :3].copy(), 'normals': vdata[:, 3:].copy(),
        'uvs': uvs, 'indices': indices, 'wire_index_count': ni2,
        'collision_ref': collision_ref, 'vertex_colors': nc, 'alpha_colors': na,
    }


def build_primitives(obj, mesh, scale):
    """glTF primitive dicts, one per section, with section-local vertex sets.
    'material' is a placeholder; the exporter fills in the section material path."""
    pos = mesh['positions']
    nrm = mesh['normals']
    uv0 = mesh['uvs'][0] if mesh['uvs'] else None
    idx = mesh['indices']
    nverts = len(pos)
    prims = []
    for si, s in enumerate(mesh['sections']):
        a = s['first_index']
        b = a + 3 * s['num_faces']
        if b > len(idx):
            raise MeshError('section %d indices out of range (%d > %d)' % (si, b, len(idx)))
        tri = idx[a:b]
        if len(tri) == 0:
            continue
        if int(tri.max()) >= nverts:
            raise MeshError('section %d vertex index out of range' % si)
        used, remap = np.unique(tri, return_inverse=True)
        p = {
            'positions': pos[used] * scale,
            'normals': nrm[used],
            'uvs': uv0[used] if uv0 is not None and len(uv0) == nverts else None,
            'indices': remap.astype(np.uint32),
            'material': 'None',
            'extras': {'section': si, 'enable_collision': _section_collision(obj, si)},
        }
        prims.append(p)
    return prims


def _section_collision(obj, si):
    mats = obj.props.get('Materials')
    if isinstance(mats, list) and si < len(mats) and isinstance(mats[si], dict):
        return bool(mats[si].get('EnableCollision', False))
    return False


def winding_score(mesh):
    """Fraction of triangles whose geometric normal agrees with vertex normals."""
    pos, nrm, idx = mesh['positions'], mesh['normals'], mesh['indices']
    if len(idx) < 3:
        return None
    t = idx[:len(idx) // 3 * 3].reshape(-1, 3)
    a, b, c = pos[t[:, 0]], pos[t[:, 1]], pos[t[:, 2]]
    g = np.cross(b - a, c - a)
    n = nrm[t[:, 0]] + nrm[t[:, 1]] + nrm[t[:, 2]]
    d = (g * n).sum(1)
    valid = np.abs(d) > 1e-9
    if not valid.any():
        return None
    return float((d[valid] > 0).mean())
