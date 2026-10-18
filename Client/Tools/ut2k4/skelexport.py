"""Skeletal mesh / animation / vertex-animated mesh -> skinned glTF (Unreal space).

Conventions (same as the static mesh export, see gltf.COORD_NOTE):
  * coordinates stay in Unreal space (X forward, Y right, Z up, left-handed), project
    centimetres (UE2 units * scale), Unreal triangle winding;
  * the UE2 mesh-to-actor transform (MeshOrigin, MeshScale, RotOrigin) is baked in, so a
    character stands upright at its in-game size with its feet near the origin, exactly as
    the engine draws it for an actor with DrawScale 1 (the raw values are kept in the json);
  * node rotations are standard local-to-parent quaternions (x, y, z, w) in that space,
    inverse bind matrices are the inverses of the bind-pose joint world matrices
    (column-major, glTF layout);
  * animations: one glTF animation per UE2 sequence, keys at every frame (time = frame / rate).

The editor import step converts these files to standard glTF exactly like static meshes:
(x, y, z) -> (x, z, y) for positions/translations, quaternions (x, y, z, w) -> (-x, -z, -y, w),
matrices M -> P M P with P the Y/Z swap.
"""

import json
import math
import os

import numpy as np

from . import skeletal as sk
from .gltf import GltfBuilder, COORD_NOTE, FLOAT, UINT16, ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER, UINT32

SKEL_NOTE = dict(COORD_NOTE)
SKEL_NOTE.update({
    'skeletal': 'joints/IBMs/animations in the same Unreal space; mesh-to-actor transform '
                '(MeshOrigin, MeshScale, RotOrigin) baked in',
    'rotations': 'standard quaternions (x, y, z, w), local-to-parent',
})


# ---------------------------------------------------------------------------
# quaternion helpers (x, y, z, w)
# ---------------------------------------------------------------------------

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ])


def mat_to_quat(m):
    m = np.asarray(m, dtype=np.float64)
    tr = m[0, 0] + m[1, 1] + m[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = [(m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s, 0.25 * s]
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        q = [0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s, (m[2, 1] - m[1, 2]) / s]
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        q = [(m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s, (m[0, 2] - m[2, 0]) / s]
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        q = [(m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s, (m[1, 0] - m[0, 1]) / s]
    q = np.array(q)
    if q[3] < 0:
        q = -q
    return q / np.linalg.norm(q)


def quat_to_rotator(q):
    """Standard quaternion -> UE rotator in degrees (FMatrix::Rotator)."""
    m = sk.quat_to_matrix(q)
    x, y, z = m[:, 0], m[:, 1], m[:, 2]
    pitch = math.atan2(x[2], math.sqrt(x[0] * x[0] + x[1] * x[1]))
    yaw = math.atan2(x[1], x[0])
    sy = rotator_matrix((pitch * 32768 / math.pi, yaw * 32768 / math.pi, 0))[:, 1]
    roll = math.atan2(float(np.dot(z, sy)), float(np.dot(y, sy)))
    return {'pitch': round(math.degrees(pitch), 4), 'yaw': round(math.degrees(yaw), 4),
            'roll': round(math.degrees(roll), 4)}


def rotator_matrix(rot):
    """UE rotator (pitch, yaw, roll; 65536 units) -> 3x3 matrix, columns = X/Y/Z axes
    (FRotationMatrix transposed into column-vector form)."""
    p, y, r = (v * 2.0 * math.pi / 65536.0 for v in rot)
    sp, cp = math.sin(p), math.cos(p)
    sy, cy = math.sin(y), math.cos(y)
    sr, cr = math.sin(r), math.cos(r)
    rows = np.array([
        [cp * cy, cp * sy, sp],
        [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
        [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp],
    ])
    return rows.T


# ---------------------------------------------------------------------------
# mesh-to-actor transform
# ---------------------------------------------------------------------------

class MeshTransform:
    """actor = R(RotOrigin) * (MeshScale * (v - MeshOrigin)), then * global scale.
    For bones: rotation part R (polar decomposition), uniform scale s."""

    def __init__(self, mesh_scale, mesh_origin, rot_origin, global_scale):
        S = np.diag(np.array(mesh_scale, dtype=np.float64))
        Rot = rotator_matrix(rot_origin)
        self.L = global_scale * (Rot @ S)
        self.t = -self.L @ np.array(mesh_origin, dtype=np.float64)
        det = float(np.linalg.det(self.L))
        self.mirror = det < 0
        self.s = abs(det) ** (1.0 / 3.0) if det else global_scale
        u, _sv, vt = np.linalg.svd(self.L)
        R = u @ vt
        if np.linalg.det(R) < 0:
            u[:, -1] *= -1
            R = u @ vt
        self.R = R
        self.qR = mat_to_quat(R)
        self.uniform = bool(np.allclose(np.abs(np.array(mesh_scale)), abs(mesh_scale[0]), rtol=1e-4))

    def point(self, p):
        return self.L @ np.asarray(p, dtype=np.float64) + self.t

    def points(self, P):
        return np.asarray(P, dtype=np.float64) @ self.L.T + self.t

    def normals(self, N):
        n = np.asarray(N, dtype=np.float64) @ np.linalg.inv(self.L)
        ln = np.linalg.norm(n, axis=1, keepdims=True)
        ln[ln == 0] = 1
        return n / ln


# ---------------------------------------------------------------------------
# skeleton
# ---------------------------------------------------------------------------

def unique_names(names):
    seen = {}
    out = []
    for n in names:
        base = n or 'Bone'
        k = base.lower()
        if k in seen:
            seen[k] += 1
            nn = '%s_%d' % (base, seen[k])
            while nn.lower() in seen:
                seen[k] += 1
                nn = '%s_%d' % (base, seen[k])
            seen[nn.lower()] = 0
            out.append(nn)
        else:
            seen[k] = 0
            out.append(base)
    return out


class Skeleton:
    """Baked reference skeleton: names, parents, local rotation/translation, world matrices."""

    def __init__(self, names, parents, local_rot, local_pos):
        self.names = names
        self.parents = parents
        self.local_rot = [np.asarray(q, dtype=np.float64) for q in local_rot]
        self.local_pos = [np.asarray(p, dtype=np.float64) for p in local_pos]
        self.world = sk.skeleton_world(parents, self.local_rot, self.local_pos)

    @classmethod
    def from_mesh(cls, mesh, xf):
        parents, rots, poss = sk.bone_local(mesh)
        names = unique_names([b['name'] for b in mesh['bones']])
        W = sk.skeleton_world(parents, rots, poss)
        # baked world: rotation R * Wrot, position A(Wpos)
        bw = []
        for m in W:
            n = np.eye(4)
            n[:3, :3] = xf.R @ m[:3, :3]
            n[:3, 3] = xf.point(m[:3, 3])
            bw.append(n)
        lr, lp = [], []
        for i, m in enumerate(bw):
            p = parents[i]
            loc = m if p < 0 else np.linalg.inv(bw[p]) @ m
            lr.append(mat_to_quat(loc[:3, :3]))
            lp.append(loc[:3, 3])
        return cls(names, parents, lr, lp)


# ---------------------------------------------------------------------------
# glTF writing
# ---------------------------------------------------------------------------

class SkinnedGltf(GltfBuilder):
    def __init__(self, scale):
        super().__init__(scale)
        self.skins = []
        self.animations = []
        self.extras_nodes = {}

    def accessor(self, arr, ctype, atype, target=None, minmax=False):
        arr = np.ascontiguousarray(arr)
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        data = arr.tobytes()
        self.bin += data
        view = {'buffer': 0, 'byteOffset': off, 'byteLength': len(data)}
        if target:
            view['target'] = target
        self.buffer_views.append(view)
        acc = {'bufferView': len(self.buffer_views) - 1, 'componentType': ctype,
               'count': int(arr.shape[0]), 'type': atype}
        if minmax:
            flat = arr.reshape(arr.shape[0], -1)
            acc['min'] = [float(x) for x in flat.min(axis=0)]
            acc['max'] = [float(x) for x in flat.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def add_skeleton(self, skel):
        """Joint nodes; returns (root node indices, node index of each joint)."""
        base = len(self.nodes)
        for i, name in enumerate(skel.names):
            q = skel.local_rot[i]
            self.nodes.append({'name': name,
                               'translation': [float(x) for x in skel.local_pos[i]],
                               'rotation': [float(x) for x in q]})
        roots = []
        for i, p in enumerate(skel.parents):
            if p < 0 or p >= i:
                roots.append(base + i)
            else:
                self.nodes[base + p].setdefault('children', []).append(base + i)
        return roots, [base + i for i in range(len(skel.names))]

    def add_skin(self, skel, joint_nodes, root_node):
        ibm = np.zeros((len(joint_nodes), 16), np.float32)
        for i, w in enumerate(skel.world):
            ibm[i] = np.linalg.inv(w).T.reshape(16)   # column-major
        acc = self.accessor(ibm, FLOAT, 'MAT4')
        self.skins.append({'joints': joint_nodes, 'inverseBindMatrices': acc, 'skeleton': root_node})
        return len(self.skins) - 1

    def add_skinned_mesh(self, name, prims, skin):
        """prims: positions (N,3), normals or None, uvs (N,2), joints (N,4), weights (N,4),
        indices (M,), material name."""
        out = []
        for p in prims:
            pos = np.asarray(p['positions'], np.float32)
            if len(pos) == 0 or len(p['indices']) == 0:
                continue
            attrs = {'POSITION': self.accessor(pos, FLOAT, 'VEC3', ARRAY_BUFFER, True)}
            if p.get('normals') is not None:
                attrs['NORMAL'] = self.accessor(np.asarray(p['normals'], np.float32), FLOAT, 'VEC3', ARRAY_BUFFER)
            if p.get('uvs') is not None:
                attrs['TEXCOORD_0'] = self.accessor(np.asarray(p['uvs'], np.float32), FLOAT, 'VEC2', ARRAY_BUFFER)
            attrs['JOINTS_0'] = self.accessor(np.asarray(p['joints'], np.uint16), UINT16, 'VEC4', ARRAY_BUFFER)
            w = np.asarray(p['weights'], np.float32)
            attrs['WEIGHTS_0'] = self.accessor(w, FLOAT, 'VEC4', ARRAY_BUFFER)
            idx = np.asarray(p['indices'])
            if len(pos) < 65536:
                iacc = self.accessor(idx.astype(np.uint16).reshape(-1), UINT16, 'SCALAR', ELEMENT_ARRAY_BUFFER)
            else:
                iacc = self.accessor(idx.astype(np.uint32).reshape(-1), UINT32, 'SCALAR', ELEMENT_ARRAY_BUFFER)
            prim = {'attributes': attrs, 'indices': iacc, 'mode': 4,
                    'material': self.material(p['material'])}
            if p.get('extras'):
                prim['extras'] = p['extras']
            out.append(prim)
        self.meshes.append({'name': name, 'primitives': out})
        self.nodes.append({'name': name, 'mesh': len(self.meshes) - 1, 'skin': skin})
        return len(self.nodes) - 1

    def add_animation(self, name, times, channels, extras=None):
        """channels: list of (node index, 'rotation'|'translation', values (F,4|3))."""
        t = np.asarray(times, np.float32)
        tacc = self.accessor(t, FLOAT, 'SCALAR', None, True)
        samplers, chans = [], []
        for node, path, vals in channels:
            v = np.asarray(vals, np.float32)
            oacc = self.accessor(v, FLOAT, 'VEC4' if path == 'rotation' else 'VEC3')
            samplers.append({'input': tacc, 'output': oacc, 'interpolation': 'LINEAR'})
            chans.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
        anim = {'name': name, 'samplers': samplers, 'channels': chans}
        if extras:
            anim['extras'] = extras
        self.animations.append(anim)

    def write_doc(self, gltf_path, scene_nodes, extras=None):
        base = os.path.splitext(os.path.basename(gltf_path))[0]
        bin_name = base + '.bin'
        doc = {
            'asset': {'version': '2.0', 'generator': 'ut2k4_export',
                      'extras': dict(SKEL_NOTE, scale=self.scale, **(extras or {}))},
            'scene': 0,
            'scenes': [{'nodes': scene_nodes}],
            'nodes': self.nodes,
            'meshes': self.meshes,
            'materials': self.materials,
            'skins': self.skins,
            'accessors': self.accessors,
            'bufferViews': self.buffer_views,
            'buffers': [{'uri': bin_name, 'byteLength': len(self.bin)}],
        }
        if self.animations:
            doc['animations'] = self.animations
        with open(os.path.join(os.path.dirname(gltf_path), bin_name), 'wb') as f:
            f.write(self.bin)
        with open(gltf_path, 'w', encoding='utf-8') as f:
            json.dump(doc, f, indent=1)


def section_primitives(geo, xf, mat_names, flip=False):
    """Split baked geometry into one primitive per material index (local vertex sets)."""
    pos = xf.points(geo['positions'])
    nrm = xf.normals(geo['normals']) if geo.get('normals') is not None else None
    tris = geo['tris'].copy()
    if flip != xf.mirror:
        tris = tris[:, [0, 2, 1]]
    prims = []
    for m in sorted(set(int(x) for x in geo['mat'])):
        t = tris[geo['mat'] == m]
        used, remap = np.unique(t.reshape(-1), return_inverse=True)
        prims.append({
            'positions': pos[used], 'normals': nrm[used] if nrm is not None else None,
            'uvs': geo['uvs'][used], 'joints': geo['joints'][used], 'weights': geo['weights'][used],
            'indices': remap.astype(np.uint32),
            'material': mat_names[m] if m < len(mat_names) else 'None',
            'extras': {'section': m, 'material_index': m},
        })
    return prims


# ---------------------------------------------------------------------------
# animation sampling
# ---------------------------------------------------------------------------

def sample_sequence(anim, seq_index, skel, xf, anim_root_index=0):
    """Sample one sequence of a parsed MeshAnimation onto a baked Skeleton.

    Returns (frame count, {joint index: (rot (F,4), pos (F,3))}, unmatched anim bones).
    UE2 keys use the same convention as the reference skeleton (non-root quaternions
    conjugated); keys are converted to standard local rotations and baked like the bones."""
    seq = anim['sequences'][seq_index]
    move = anim['moves'][seq_index] if seq_index < len(anim['moves']) else None
    nf = max(int(seq['num_frames']), 1)
    frames = np.arange(nf, dtype=np.float64)
    by_name = {n.lower(): i for i, n in enumerate(skel.names)}
    out = {}
    unmatched = []
    if move is None:
        return nf, out, [b['name'] for b in anim['bones']]
    for ai, ab in enumerate(anim['bones']):
        ji = by_name.get(ab['name'].lower())
        if ji is None:
            unmatched.append(ab['name'])
            continue
        if ai >= len(move['tracks']):
            continue
        tr = move['tracks'][ai]
        q, p = sk.sample_track(tr, nf, move['track_time'], frames)
        is_root = ai == anim_root_index or ji == 0
        if q is None:
            rot = np.repeat(skel.local_rot[ji][None], nf, 0)
            q_std = None
        else:
            q_std = sk.quat_norm(q if is_root else sk.quat_conj(q))
        if p is None:
            pos = np.repeat(skel.local_pos[ji][None], nf, 0)
        if is_root:
            if q_std is not None:
                rot = np.array([qmul(xf.qR, qq) for qq in q_std])
            if p is not None:
                pos = xf.points(p)
        else:
            if q_std is not None:
                rot = q_std
            if p is not None:
                pos = p * xf.s
        # keep quaternion signs continuous (LINEAR interpolation of glTF)
        rot = np.array(rot, dtype=np.float64)
        for k in range(1, len(rot)):
            if np.dot(rot[k], rot[k - 1]) < 0:
                rot[k] = -rot[k]
        out[ji] = (rot, np.asarray(pos, dtype=np.float64))
    return nf, out, unmatched
