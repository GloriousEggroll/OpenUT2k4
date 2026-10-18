"""Minimal glTF 2.0 writer (.gltf JSON + external .bin) and a sanity checker."""

import json
import os
import sys

import numpy as np

FLOAT = 5126
UINT16 = 5123
UINT32 = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963

COORD_NOTE = {
    'coordinateSystem': 'Unreal (X forward, Y right, Z up, left-handed); positions are NOT '
                        'converted to glTF Y-up space',
    'units': 'project centimetres (UE2 units * scale)',
    'winding': 'Unreal native (same as UE2/UE5 static meshes): cross(v1 - v0, v2 - v0) points '
               'OPPOSITE the face normal in the stored coordinates',
    'uvOrigin': 'top-left (U right, V down)',
}


class GltfBuilder:
    def __init__(self, scale):
        self.scale = scale
        self.bin = bytearray()
        self.buffer_views = []
        self.accessors = []
        self.materials = []
        self._mat_index = {}
        self.meshes = []
        self.nodes = []

    def _add_view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        self.buffer_views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(data),
                                  'target': target})
        return len(self.buffer_views) - 1

    def _accessor(self, arr, ctype, atype, target, minmax=False):
        view = self._add_view(arr.tobytes(), target)
        acc = {'bufferView': view, 'componentType': ctype, 'count': int(arr.shape[0]),
               'type': atype}
        if minmax:
            acc['min'] = [float(x) for x in arr.min(axis=0)]
            acc['max'] = [float(x) for x in arr.max(axis=0)]
        self.accessors.append(acc)
        return len(self.accessors) - 1

    def material(self, name, extras=None):
        idx = self._mat_index.get(name)
        if idx is None:
            m = {'name': name, 'pbrMetallicRoughness': {'metallicFactor': 0.0,
                                                        'roughnessFactor': 1.0}}
            if extras:
                m['extras'] = extras
            self.materials.append(m)
            idx = len(self.materials) - 1
            self._mat_index[name] = idx
        return idx

    def add_primitive_list(self, mesh_name, prims):
        """prims: list of dicts with positions (N,3) float32, normals (N,3), uvs (N,2),
        indices (M,) and material (name), optional extras."""
        out = []
        for p in prims:
            pos = np.ascontiguousarray(p['positions'], dtype=np.float32)
            if len(pos) == 0 or len(p['indices']) == 0:
                continue
            attrs = {'POSITION': self._accessor(pos, FLOAT, 'VEC3', ARRAY_BUFFER, True)}
            if p.get('normals') is not None:
                nrm = np.ascontiguousarray(p['normals'], dtype=np.float32)
                ln = np.linalg.norm(nrm, axis=1, keepdims=True)
                ln[ln == 0] = 1.0
                nrm = (nrm / ln).astype(np.float32)
                attrs['NORMAL'] = self._accessor(nrm, FLOAT, 'VEC3', ARRAY_BUFFER)
            if p.get('uvs') is not None:
                attrs['TEXCOORD_0'] = self._accessor(
                    np.ascontiguousarray(p['uvs'], dtype=np.float32), FLOAT, 'VEC2', ARRAY_BUFFER)
            idx = np.asarray(p['indices'])
            if len(pos) < 65536:
                idx = idx.astype(np.uint16)
                ct = UINT16
            else:
                idx = idx.astype(np.uint32)
                ct = UINT32
            iacc = self._accessor(idx.reshape(-1), ct, 'SCALAR', ELEMENT_ARRAY_BUFFER)
            prim = {'attributes': attrs, 'indices': iacc, 'mode': 4,
                    'material': self.material(p['material'], p.get('material_extras'))}
            if p.get('extras'):
                prim['extras'] = p['extras']
            out.append(prim)
        self.meshes.append({'name': mesh_name, 'primitives': out})
        self.nodes.append({'name': mesh_name, 'mesh': len(self.meshes) - 1})
        return len(out)

    def write(self, gltf_path, extras=None):
        base = os.path.splitext(os.path.basename(gltf_path))[0]
        bin_name = base + '.bin'
        doc = {
            'asset': {'version': '2.0', 'generator': 'ut2k4_export',
                      'extras': dict(COORD_NOTE, scale=self.scale, **(extras or {}))},
            'scene': 0,
            'scenes': [{'nodes': list(range(len(self.nodes)))}],
            'nodes': self.nodes,
            'meshes': self.meshes,
            'materials': self.materials,
            'accessors': self.accessors,
            'bufferViews': self.buffer_views,
            'buffers': [{'uri': bin_name, 'byteLength': len(self.bin)}],
        }
        with open(os.path.join(os.path.dirname(gltf_path), bin_name), 'wb') as f:
            f.write(self.bin)
        with open(gltf_path, 'w', encoding='utf-8') as f:
            json.dump(doc, f, indent=1)


# ---------------------------------------------------------------------------
# Sanity checker (standard library only)
# ---------------------------------------------------------------------------

_CSIZE = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}
_NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
_ARRAY_CODE = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}


def check_gltf(path):
    """Validate a .gltf + .bin pair: buffer/bufferView/accessor bounds, attribute
    counts, finite values, POSITION min/max, index ranges, material indices.
    Returns dict with counts; raises ValueError on any problem."""
    import array
    import math

    with open(path, 'r', encoding='utf-8') as f:
        doc = json.load(f)
    if doc.get('asset', {}).get('version') != '2.0':
        raise ValueError('asset.version must be 2.0')
    bufs = []
    for b in doc.get('buffers', []):
        with open(os.path.join(os.path.dirname(path), b['uri']), 'rb') as f:
            data = f.read()
        if len(data) != b['byteLength']:
            raise ValueError('buffer length mismatch %s' % b['uri'])
        bufs.append(data)
    views = doc.get('bufferViews', [])
    for i, v in enumerate(views):
        if v['byteOffset'] + v['byteLength'] > len(bufs[v['buffer']]):
            raise ValueError('bufferView %d out of range' % i)

    def acc_data(ai):
        a = doc['accessors'][ai]
        v = views[a['bufferView']]
        n = _NCOMP[a['type']]
        size = a['count'] * n * _CSIZE[a['componentType']]
        off = v['byteOffset'] + a.get('byteOffset', 0)
        if off + size > v['byteOffset'] + v['byteLength']:
            raise ValueError('accessor %d exceeds its bufferView' % ai)
        arr = array.array(_ARRAY_CODE[a['componentType']])
        arr.frombytes(bufs[v['buffer']][off:off + size])
        if sys.byteorder != 'little':
            arr.byteswap()
        return a, n, arr

    tris = verts = prims = 0
    nmat = len(doc.get('materials', []))
    for m in doc.get('meshes', []):
        for p in m['primitives']:
            prims += 1
            attrs = p['attributes']
            pa, _n, pos = acc_data(attrs['POSITION'])
            cnt = pa['count']
            if not all(math.isfinite(x) for x in pos):
                raise ValueError('non-finite positions in %s' % m.get('name'))
            if 'min' in pa and cnt:
                for c in range(3):
                    comp = pos[c::3]
                    if min(comp) < pa['min'][c] - 1e-3 or max(comp) > pa['max'][c] + 1e-3:
                        raise ValueError('POSITION min/max wrong')
            for k in ('NORMAL', 'TEXCOORD_0'):
                if k in attrs:
                    a, _n, d = acc_data(attrs[k])
                    if a['count'] != cnt:
                        raise ValueError('%s count %d != POSITION count %d' % (k, a['count'], cnt))
                    if not all(math.isfinite(x) for x in d):
                        raise ValueError('non-finite %s' % k)
            ia, _n, idx = acc_data(p['indices'])
            if p.get('mode', 4) != 4:
                raise ValueError('only triangle lists are written')
            if len(idx) % 3:
                raise ValueError('index count not a multiple of 3')
            if len(idx) and max(idx) >= cnt:
                raise ValueError('index %d out of range (%d verts)' % (max(idx), cnt))
            if 'material' in p and not (0 <= p['material'] < nmat):
                raise ValueError('bad material index')
            tris += len(idx) // 3
            verts += cnt
    return {'primitives': prims, 'triangles': tris, 'vertices': verts, 'materials': nmat}
