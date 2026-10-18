"""UE2 SkeletalMesh, MeshAnimation and VertMesh parsing (UT2003/UT2004 packages,
versions 119-128).

Layouts follow the UE2 serializers (ULodMesh / USkeletalMesh / FStaticLODModel /
UMeshAnimation / MotionChunk / AnalogTrack). Everything stays in UE2 mesh space
(X forward, Y right, Z up, UE2 units); the exporter applies the scale.

Bone orientation convention (UE2): VJointPos.Orientation of every bone except the
root is stored conjugated (the engine builds child bone matrices from the inverse
quaternion). read_skeletal_mesh() / read_animation() return *standard* local
rotations (local-to-parent, q * v * q^-1), i.e. children are conjugated on load.
The same applies to animation keys. See skeleton_world() for the composition used.
"""

import numpy as np

from .archive import ReadError


class SkelError(Exception):
    pass


# ---------------------------------------------------------------------------
# small readers
# ---------------------------------------------------------------------------

def _arr(r, elem_size, dtype=None):
    """TArray of fixed-size POD elements -> raw bytes / numpy view."""
    n = r.index()
    if n < 0 or n * elem_size > r.remaining():
        raise SkelError('bad array count %d (elem %d, %d left)' % (n, elem_size, r.remaining()))
    data = r.bytes(n * elem_size)
    if dtype is None:
        return n, data
    return np.frombuffer(data, dtype).copy()


def _lazy(r, elem_size, dtype=None, has_skip=True):
    if has_skip:
        r.i32()
    return _arr(r, elem_size, dtype)


def _names(r, pkg):
    n = r.index()
    if n < 0 or n > r.remaining():
        raise SkelError('bad name array')
    return [pkg.name_at(r.index()) for _ in range(n)]


def _u16_array(r):
    return _arr(r, 2, '<u2')


_WEDGE = np.dtype([('v', '<u2'), ('u', '<f4'), ('t', '<f4')])          # FMeshWedge, 10 bytes
_TRI = np.dtype([('w', '<u2', 3), ('mat', 'u1'), ('aux', 'u1'), ('sg', '<u4')])   # VTriangle, 12 bytes
_INF = np.dtype([('w', '<f4'), ('p', '<u2'), ('b', '<u2')])             # FVertInfluence, 8 bytes
_FACE = np.dtype([('w', '<u2', 3), ('mat', '<u2')])                     # FMeshFace, 8 bytes
_SECTION = np.dtype([('mat', '<u2'), ('min_stream', '<u2'), ('min_wedge', '<u2'), ('max_wedge', '<u2'),
                     ('num_stream', '<u2'), ('bone', '<u2'), ('fe', '<u2'), ('first_face', '<u2'),
                     ('num_faces', '<u2')])                              # FSkelMeshSection, 18 bytes
_ANIMVERT = np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2)])  # FAnimMeshVertex, 32 bytes


def quat_conj(q):
    q = np.array(q, dtype=np.float64)
    q[..., :3] *= -1.0
    return q


def quat_norm(q):
    q = np.asarray(q, dtype=np.float64)
    n = np.linalg.norm(q, axis=-1, keepdims=True)
    n[n < 1e-12] = 1.0
    return q / n


def quat_to_matrix(q):
    """(x, y, z, w) -> 3x3 rotation matrix (column vectors: v' = M @ v)."""
    x, y, z, w = quat_norm(q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def skeleton_world(parents, local_rot, local_pos):
    """Bone world matrices (4x4, column vectors) from local (standard) rotations and positions."""
    n = len(parents)
    world = [None] * n
    for i in range(n):
        m = np.eye(4)
        m[:3, :3] = quat_to_matrix(local_rot[i])
        m[:3, 3] = local_pos[i]
        p = parents[i]
        world[i] = m if (i == 0 or p < 0 or p >= i) else world[p] @ m
    return world


# ---------------------------------------------------------------------------
# ULodMesh (shared by SkeletalMesh and VertMesh)
# ---------------------------------------------------------------------------

def _read_lod_mesh(obj, r):
    pkg = obj.pkg
    d = {}
    d['bbox'] = r.box()
    d['sphere'] = r.sphere()
    d['version'] = version = r.i32()
    d['vertex_count'] = r.i32()
    d['verts'] = _arr(r, 4, '<u4')                     # FMeshVert packed (VertMesh only)
    if version <= 1:
        _arr(r, 38)                                    # FMeshTri2 (obsolete)
    n = r.index()
    if n < 0 or n > r.remaining():
        raise SkelError('bad texture count')
    d['textures'] = [r.index() for _ in range(n)]
    d['mesh_scale'] = r.vector()
    d['mesh_origin'] = r.vector()
    d['rot_origin'] = r.rotator()
    if version <= 1:
        _u16_array(r)
    d['face_level'] = _u16_array(r)
    d['faces'] = _arr(r, 8, _FACE)
    d['collapse_wedge_thus'] = _u16_array(r)
    d['wedges'] = _arr(r, 10, _WEDGE)
    d['materials'] = _arr(r, 8, np.dtype([('flags', '<u4'), ('tex', '<i4')]))
    d['mesh_scale_max'] = r.f32()
    d['lod_hysteresis'] = r.f32()
    d['lod_strength'] = r.f32()
    d['lod_min_verts'] = r.i32()
    d['lod_morph'] = r.f32()
    d['lod_zdisplace'] = r.f32()
    if version >= 3:
        r.i32()                    # HasImpostor
        r.index()                  # SpriteMaterial
        r.vector()
        r.rotator()
        r.vector()
        r.u32()                    # ImpColor
        r.i32()
        r.i32()
        r.i32()
    if version >= 4:
        r.f32()                    # SkinTesselationFactor
    return d


# ---------------------------------------------------------------------------
# USkeletalMesh
# ---------------------------------------------------------------------------

def _read_bone(r, pkg):
    name = pkg.name_at(r.index())
    flags = r.u32()
    q = r.plane()                  # FQuat X Y Z W
    pos = r.vector()
    length = r.f32()
    size = r.vector()
    nchildren = r.i32()
    parent = r.i32()
    return {'name': name, 'flags': flags, 'quat': q, 'pos': pos, 'length': length,
            'size': size, 'num_children': nchildren, 'parent': parent}


def _read_raw_index_buffer(r):
    idx = _u16_array(r)
    r.i32()
    return idx


def _read_static_lod(r, has_skip):
    lod = {}
    lod['skinning_data'] = _arr(r, 4, '<u4')
    lod['skin_points'] = _arr(r, 16)[0]
    lod['num_soft_wedges'] = r.i32()
    lod['soft_sections'] = _arr(r, 18, _SECTION)
    lod['rigid_sections'] = _arr(r, 18, _SECTION)
    lod['soft_indices'] = _read_raw_index_buffer(r)
    lod['rigid_indices'] = _read_raw_index_buffer(r)
    r.i32()
    r.i32()
    r.i32()                                            # vertex stream header
    lod['stream_verts'] = _arr(r, 32, _ANIMVERT)
    lod['influences'] = _lazy(r, 8, _INF, has_skip)
    lod['wedges'] = _lazy(r, 10, _WEDGE, has_skip)
    lod['faces'] = _lazy(r, 8, _FACE, has_skip)
    lod['points'] = _lazy(r, 12, '<f4', has_skip).reshape(-1, 3)
    lod['distance_factor'] = r.f32()
    lod['hysteresis'] = r.f32()
    lod['num_shared_verts'] = r.i32()
    lod['max_influences'] = r.i32()
    r.i32()
    r.i32()
    return lod


def read_skeletal_mesh(obj):
    """Parse a SkeletalMesh export. Returns a dict with bones, sockets, the base
    (import) mesh and LOD models, plus ULodMesh fields."""
    pkg = obj.pkg
    ver = pkg.version
    has_skip = ver > 61
    r = obj.native_reader()
    d = _read_lod_mesh(obj, r)
    d['points2'] = _arr(r, 12, '<f4').reshape(-1, 3)
    nb = r.index()
    if nb <= 0 or nb > 4096:
        raise SkelError('bad bone count %d' % nb)
    d['bones'] = [_read_bone(r, pkg) for _ in range(nb)]
    d['animation'] = r.index()
    d['skeletal_depth'] = r.i32()
    nwi = r.index()
    for _ in range(nwi):
        _u16_array(r)
        r.i32()
    _arr(r, 4)                                         # BoneInfluences
    d['attach_aliases'] = _names(r, pkg)
    d['attach_bones'] = _names(r, pkg)
    nc = r.index()
    if nc < 0 or nc * 48 > r.remaining():
        raise SkelError('bad attach coords')
    d['attach_coords'] = [np.frombuffer(r.bytes(48), '<f4').reshape(4, 3).astype(np.float64)
                          for _ in range(nc)]
    d['lods'] = []
    d['points'] = d['wedges_base'] = d['triangles'] = d['influences'] = None
    if d['version'] <= 1:
        _arr(r, 18)
        _arr(r, 18)
        _u16_array(r)
        d['points'] = d['points2']
        d['influences'] = None   # old meshes are not expected in UT2004
    else:
        nl = r.index()
        if nl < 0 or nl > 32:
            raise SkelError('bad LOD count %d' % nl)
        d['lods'] = [_read_static_lod(r, has_skip) for _ in range(nl)]
        r.index()                                      # unused object ref
        d['points'] = _lazy(r, 12, '<f4', has_skip).reshape(-1, 3)
        d['wedges_base'] = _lazy(r, 10, _WEDGE, has_skip)
        d['triangles'] = _lazy(r, 12, _TRI, has_skip)
        d['influences'] = _lazy(r, 8, _INF, has_skip)
        _lazy(r, 2, None, has_skip)                    # CollapseWedge
        _lazy(r, 2, None, has_skip)                    # f1C8
    d['auth_key'] = r.u32() if ver >= 120 else None
    d['karma_props'] = d['collision_mesh'] = None
    d['bone_spheres'] = []
    d['bone_boxes'] = []
    if ver >= 122:
        try:
            d['karma_props'] = r.index()
            for _ in range(r.index()):
                s = {'bone': pkg.name_at(r.index()), 'offset': r.vector(), 'radius': r.f32()}
                if ver > 123:
                    s['block_karma'] = r.i32()
                if ver > 124:
                    s['block_zero_extent'] = r.i32()
                    s['block_nonzero_extent'] = r.i32()
                d['bone_spheres'].append(s)
            for _ in range(r.index()):
                b = {'bone': pkg.name_at(r.index()), 'offset': r.vector(), 'radii': r.vector()}
                if ver > 123:
                    b['block_karma'] = r.i32()
                if ver > 124:
                    b['block_zero_extent'] = r.i32()
                    b['block_nonzero_extent'] = r.i32()
                d['bone_boxes'].append(b)
            for _ in range(r.index()):
                r.index()
            if ver >= 127:
                d['collision_mesh'] = r.index()
        except (ReadError, IndexError):
            pass
    return d


def _geometry_from(points, wedges, faces_w, faces_mat, influences, nbones):
    """-> dict(positions (W,3), uvs (W,2), joints (W,4), weights (W,4), tris (T,3), mat (T,))"""
    npts = len(points)
    if npts == 0 or len(wedges) == 0 or len(faces_w) == 0:
        raise SkelError('empty geometry')
    if int(wedges['v'].max()) >= npts:
        raise SkelError('wedge point index out of range')
    if int(faces_w.max()) >= len(wedges):
        raise SkelError('face wedge index out of range')
    # per point: top-4 influences, normalised
    joints = np.zeros((npts, 4), np.int32)
    weights = np.zeros((npts, 4), np.float64)
    count = np.zeros(npts, np.int32)
    if influences is not None and len(influences):
        order = np.argsort(-influences['w'], kind='stable')
        for k in order:
            inf = influences[k]
            p = int(inf['p'])
            b = int(inf['b'])
            w = float(inf['w'])
            if p >= npts or w <= 0.0:
                continue
            if b >= nbones:
                b = 0
            c = count[p]
            if c < 4:
                # merge duplicate bone entries
                hit = np.where(joints[p, :c] == b)[0]
                if len(hit):
                    weights[p, hit[0]] += w
                else:
                    joints[p, c] = b
                    weights[p, c] = w
                    count[p] = c + 1
    missing = count == 0
    weights[missing, 0] = 1.0
    s = weights.sum(1, keepdims=True)
    s[s <= 0] = 1.0
    weights /= s
    wv = wedges['v'].astype(np.int64)
    return {
        'positions': points[wv].astype(np.float64),
        'uvs': np.stack([wedges['u'], wedges['t']], 1).astype(np.float32),
        'joints': joints[wv],
        'weights': weights[wv],
        'tris': faces_w.astype(np.int64),
        'mat': faces_mat.astype(np.int64),
        'unweighted_points': int(missing.sum()),
        'point_count': npts,
    }


def mesh_geometry(mesh):
    """Best available geometry: the base (import) mesh when complete, else LOD 0."""
    nb = len(mesh['bones'])
    tri = mesh.get('triangles')
    if mesh.get('points') is not None and len(mesh['points']) and mesh.get('wedges_base') is not None \
            and len(mesh['wedges_base']) and tri is not None and len(tri) \
            and mesh.get('influences') is not None and len(mesh['influences']):
        g = _geometry_from(mesh['points'], mesh['wedges_base'], tri['w'], tri['mat'],
                           mesh['influences'], nb)
        g['source'] = 'base'
        return g
    for li, lod in enumerate(mesh['lods']):
        if not (len(lod['points']) and len(lod['wedges']) and len(lod['influences'])):
            continue
        faces_w, faces_m = [], []
        for s in lod['soft_sections']:
            f = lod['faces'][int(s['first_face']):int(s['first_face']) + int(s['num_faces'])]
            faces_w.append(f['w'])
            faces_m.append(np.full(len(f), int(s['mat'])))
        ri = lod['rigid_indices']
        for s in lod['rigid_sections']:
            a = int(s['first_face']) * 3
            idx = ri[a:a + int(s['num_faces']) * 3].reshape(-1, 3)
            faces_w.append(idx)
            faces_m.append(np.full(len(idx), int(s['mat'])))
        if not faces_w:
            f = lod['faces']
            faces_w, faces_m = [f['w']], [f['mat']]
        g = _geometry_from(lod['points'], lod['wedges'], np.concatenate(faces_w),
                           np.concatenate(faces_m), lod['influences'], nb)
        g['source'] = 'lod%d' % li
        return g
    # stripped meshes: only the render streams are left. Rigid sections reference the
    # vertex stream (mesh space, absolute indices) and are bound to one bone each.
    for li, lod in enumerate(mesh['lods']):
        sv = lod['stream_verts']
        if len(lod['soft_sections']) or not len(lod['rigid_sections']) or not len(sv):
            continue
        ri = lod['rigid_indices']
        tris, mats = [], []
        joints = np.zeros((len(sv), 4), np.int32)
        weights = np.zeros((len(sv), 4), np.float64)
        weights[:, 0] = 1.0
        for s in lod['rigid_sections']:
            a = int(s['first_face']) * 3
            idx = ri[a:a + int(s['num_faces']) * 3].astype(np.int64)
            if len(idx) == 0:
                continue
            if int(idx.max()) >= len(sv):
                raise SkelError('rigid section index out of range')
            joints[idx, 0] = min(int(s['bone']), nb - 1)
            tris.append(idx.reshape(-1, 3))
            mats.append(np.full(len(idx) // 3, int(s['mat'])))
        if not tris:
            continue
        return {
            'positions': sv['p'].astype(np.float64), 'uvs': sv['uv'].astype(np.float32),
            'normals': sv['n'].astype(np.float64),
            'joints': joints, 'weights': weights, 'tris': np.concatenate(tris),
            'mat': np.concatenate(mats), 'unweighted_points': 0, 'point_count': len(sv),
            'source': 'lod%d-rigid-stream' % li,
        }
    if any(len(l['soft_sections']) for l in mesh['lods']):
        raise SkelError('only packed soft-skinned render data (SkinningData) is present; '
                        'the base mesh was stripped from the package')
    raise SkelError('no usable geometry (base mesh and LODs empty)')


def bone_local(mesh):
    """Standard local rotations (x, y, z, w) and positions of the reference skeleton.
    UE2 stores non-root bone quaternions conjugated."""
    rots, poss, parents = [], [], []
    for i, b in enumerate(mesh['bones']):
        q = np.array(b['quat'], dtype=np.float64)
        rots.append(quat_norm(q if i == 0 else quat_conj(q)))
        poss.append(np.array(b['pos'], dtype=np.float64))
        parents.append(b['parent'] if i else -1)
    return parents, rots, poss


# ---------------------------------------------------------------------------
# UMeshAnimation
# ---------------------------------------------------------------------------

def _read_analog_track(r):
    flags = r.u32()
    q = _arr(r, 16, '<f4').reshape(-1, 4)
    p = _arr(r, 12, '<f4').reshape(-1, 3)
    t = _arr(r, 4, '<f4')
    return {'flags': flags, 'quat': q, 'pos': p, 'time': t}


_FLEX_UNSUPPORTED = 'flex (compressed) animation tracks are not used by UT2004'


def _read_motion_chunk(r):
    m = {}
    m['root_speed'] = r.vector()
    m['track_time'] = r.f32()
    m['start_bone'] = r.i32()
    m['flags'] = r.u32()
    m['bone_indices'] = _arr(r, 4, '<i4')
    n = r.index()
    if n < 0 or n > 4096:
        raise SkelError('bad track count %d' % n)
    m['tracks'] = [_read_analog_track(r) for _ in range(n)]
    m['root_track'] = _read_analog_track(r)
    if m['flags'] >= 3:
        raise SkelError(_FLEX_UNSUPPORTED)
    return m


def read_animation(obj):
    pkg = obj.pkg
    ver = pkg.version
    r = obj.native_reader()
    d = {'version': r.i32()}
    nb = r.index()
    if nb < 0 or nb > 4096:
        raise SkelError('bad ref bone count %d' % nb)
    d['bones'] = [{'name': pkg.name_at(r.index()), 'flags': r.u32(), 'parent': r.i32()} for _ in range(nb)]
    nm = r.index()
    if nm < 0 or nm > 100000:
        raise SkelError('bad move count %d' % nm)
    d['moves'] = [_read_motion_chunk(r) for _ in range(nm)]
    ns = r.index()
    if ns < 0 or ns > 100000:
        raise SkelError('bad sequence count %d' % ns)
    seqs = []
    for _ in range(ns):
        s = {}
        if ver >= 115:
            s['f28'] = r.f32()
        s['name'] = pkg.name_at(r.index())
        s['groups'] = _names(r, pkg)
        s['start_frame'] = r.i32()
        s['num_frames'] = r.i32()
        notifies = []
        for _ in range(r.index()):
            n = {'time': r.f32(), 'function': pkg.name_at(r.index())}
            if ver >= 112:
                n['object'] = r.index()
            notifies.append(n)
        s['notifies'] = notifies
        s['rate'] = r.f32()
        seqs.append(s)
    d['sequences'] = seqs
    return d


def sample_track(track, nframes, track_time, frames):
    """Sample one AnalogTrack at integer frame numbers.

    Key times are in frames (UE2 sometimes stores them in another time base:
    they are rescaled by num_frames / track_time). Missing quat or pos keys -> None.
    Interpolation: slerp / lerp between surrounding keys, clamped at the ends.
    Returns (quats (F,4) raw UE2 convention or None, positions (F,3) or None)."""
    q = track['quat']
    p = track['pos']
    t = track['time'].astype(np.float64)
    if track_time > 0 and nframes > 0:
        t = t * (nframes / track_time)
    fr = np.asarray(frames, dtype=np.float64)

    def sample(keys, is_quat):
        if keys is None or len(keys) == 0:
            return None
        keys = keys.astype(np.float64)
        if len(keys) == 1 or len(t) < 2:
            return np.repeat(keys[:1], len(fr), 0)
        kt = t[:len(keys)] if len(t) >= len(keys) else np.linspace(0, max(nframes - 1, 1), len(keys))
        out = np.empty((len(fr), keys.shape[1]))
        for i, f in enumerate(fr):
            j = int(np.searchsorted(kt, f, side='right')) - 1
            if j < 0:
                out[i] = keys[0]
                continue
            if j >= len(keys) - 1:
                out[i] = keys[-1]
                continue
            t0, t1 = kt[j], kt[j + 1]
            a = 0.0 if t1 <= t0 else (f - t0) / (t1 - t0)
            if is_quat:
                out[i] = _slerp(keys[j], keys[j + 1], a)
            else:
                out[i] = keys[j] * (1 - a) + keys[j + 1] * a
        return out

    return sample(q, True), sample(p, False)


def _slerp(a, b, t):
    d = float(np.dot(a, b))
    if d < 0:
        b = -b
        d = -d
    if d > 0.9995:
        r = a + t * (b - a)
        return r / (np.linalg.norm(r) or 1.0)
    th = np.arccos(min(1.0, d))
    s = np.sin(th)
    return (np.sin((1 - t) * th) * a + np.sin(t * th) * b) / s


# ---------------------------------------------------------------------------
# UVertMesh (vertex-animated meshes, e.g. some Invasion monsters)
# ---------------------------------------------------------------------------

def read_vert_mesh(obj):
    pkg = obj.pkg
    r = obj.native_reader()
    d = _read_lod_mesh(obj, r)
    _arr(r, 32)                                        # AnimMeshVerts (generated at load)
    r.i32()                                            # StreamVersion
    d['verts2'] = _arr(r, 4, '<u4')
    _arr(r, 4)                                         # f150
    seqs = []
    ver = pkg.version
    for _ in range(r.index()):
        s = {}
        if ver >= 115:
            s['f28'] = r.f32()
        s['name'] = pkg.name_at(r.index())
        s['groups'] = _names(r, pkg)
        s['start_frame'] = r.i32()
        s['num_frames'] = r.i32()
        notifies = []
        for _ in range(r.index()):
            n = {'time': r.f32(), 'function': pkg.name_at(r.index())}
            if ver >= 112:
                n['object'] = r.index()
            notifies.append(n)
        s['notifies'] = notifies
        s['rate'] = r.f32()
        seqs.append(s)
    d['normals'] = _arr(r, 4, '<u4')
    d['vert_count'] = r.i32()
    d['frame_count'] = r.i32()
    d['sequences'] = seqs
    return d


def unpack_vert_mesh_frames(d):
    """Packed FMeshVert (X:11 Y:11 Z:10, signed) -> (frames, verts, 3) float positions."""
    v = (d['verts'] if len(d['verts']) else d['verts2']).astype(np.uint32)
    nv = d['vert_count']
    nf = d['frame_count']
    if nv <= 0 or nf <= 0 or len(v) < nv * nf:
        raise SkelError('VertMesh frame data size mismatch (%d verts x %d frames, %d packed)'
                        % (nv, nf, len(v)))
    v = v[:nv * nf]
    x = (v & 0x7FF).astype(np.int32)
    y = ((v >> 11) & 0x7FF).astype(np.int32)
    z = ((v >> 22) & 0x3FF).astype(np.int32)
    x = np.where(x >= 1024, x - 2048, x)
    y = np.where(y >= 1024, y - 2048, y)
    z = np.where(z >= 512, z - 1024, z)
    return np.stack([x, y, z], 1).astype(np.float64).reshape(nf, nv, 3)
