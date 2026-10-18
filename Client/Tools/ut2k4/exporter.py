"""Exports UT2004 objects into ImportCache/<kind>/<Package>/<Group.>Name.ext."""

import json
import math
import os
import re
import sys
import time
import traceback

import numpy as np

from .archive import ReadError
from .package import ObjRef
from .properties import Vector, Rotator
from . import texture as tex
from . import staticmesh as smesh
from . import sound as snd
from . import skeletal as skel
from . import skelexport as skx
from .gltf import GltfBuilder
from .png import write_png

TOOL_VERSION = 1
ANIM_TOOL_VERSION = 2   # animations / vertex meshes: 1-frame sequences carry two keys
_SAFE = re.compile(r'[^A-Za-z0-9._\-]')

# Classes whose objects are never walked for dependencies.
_NO_WALK = {'model', 'polys', 'reachspec', 'level', 'staticmeshinstance', 'levelsummary',
            'class', 'package', 'textbuffer', 'palette'}


def safe(name):
    return _SAFE.sub('_', name)


def rot_to_deg(rot):
    """UE2 rotator units (65536 = 360) -> degrees, normalised to (-180, 180]."""
    out = []
    for v in rot:
        d = (v % 65536) * 360.0 / 65536.0
        if d > 180.0:
            d -= 360.0
        out.append(round(d, 5))
    return {'pitch': out[0], 'yaw': out[1], 'roll': out[2]}


def hsv_color(hue, sat):
    """Unreal light hue/saturation (bytes) -> linear RGB in 0..1 (brightness separate).
    Saturation 255 means white in UE1/UE2."""
    h = hue
    if h < 86:
        c = ((85 - h) / 85.0, h / 85.0, 0.0)
    elif h < 171:
        c = (0.0, (170 - h) / 85.0, (h - 85) / 85.0)
    else:
        c = ((h - 170) / 85.0, 0.0, (255 - h) / 84.0)
    s = sat / 255.0
    rgb = [ci + s * (1.0 - ci) for ci in c]
    m = max(rgb) or 1.0
    return [round(x / m, 4) for x in rgb]


class Exporter:
    def __init__(self, manager, out_dir, scale=2.0, png_level=6, verbose=False, assets=True):
        self.mgr = manager
        self.assets = assets      # False: maps only (map.json + bsp), no referenced assets
        self.out = os.path.abspath(out_dir)
        self.scale = scale
        self.png_level = png_level
        self.verbose = verbose
        self.done = {}            # canonical path (lower) -> output rel path or None
        self.errors = []
        self.counts = {'textures': 0, 'materials': 0, 'staticmeshes': 0, 'sounds': 0,
                       'maps': 0, 'skipped': 0, 'skeletalmeshes': 0, 'animations': 0,
                       'vertmeshes': 0, 'music': 0}
        self.skip_existing = False   # True: reuse up-to-date files from an earlier run
        self._anim_meshes = None     # MeshAnimation path (lower) -> [SkeletalMesh paths]
        self._canon = {}
        self._paths = {}          # key -> object path (original case)
        self._by_path = {}        # object path (lower) -> keys
        self.obj_deps = {}        # key -> set of keys exported while exporting it
        self._collect = []        # stack of dependency collectors

    # -- dependency tracking ---------------------------------------------------
    def _note(self, key):
        for c in self._collect:
            c.add(key)
            c.update(self.obj_deps.get(key, ()))

    def begin_collect(self):
        self._collect.append(set())

    def end_collect(self):
        """Pop a collector -> {kind: [{'path', 'file'}]} of every asset exported or
        reused (transitively) since begin_collect()."""
        keys = self._collect.pop()
        out = {}
        for k in sorted(keys):
            f = self.done.get(k)
            if f:
                out.setdefault(f.split('/', 1)[0], []).append({'path': self._paths.get(k, k), 'file': f})
        return out

    # -- logging --------------------------------------------------------------
    def log(self, msg):
        print(msg, flush=True)

    def error(self, what, exc):
        msg = '%s: %s' % (what, exc)
        self.errors.append(msg)
        print('  [error] ' + msg, file=sys.stderr, flush=True)
        if self.verbose:
            traceback.print_exc()

    # -- paths ------------------------------------------------------------------
    def canon(self, ref):
        """Canonical full path of an ObjRef (target package's own names)."""
        if ref is None or not ref:
            return None
        key = (ref.pkg.path, ref.index)
        hit = self._canon.get(key)
        if hit is None:
            res = None
            try:
                res = ref.resolve()
            except (ReadError, IndexError):
                res = None
            hit = res[0].export_path(res[1]) if res else ref.path
            self._canon[key] = hit
        return hit

    def out_path(self, kind, obj, ext):
        pkg = obj.pkg.name
        full = obj.path
        rel = full.split('.', 1)[1] if '.' in full else full
        d = os.path.join(self.out, kind, safe(pkg))
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, safe(rel) + ext)

    def rel(self, path):
        return os.path.relpath(path, self.out).replace(os.sep, '/')

    def write_json(self, path, data):
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=1, ensure_ascii=False, sort_keys=False)

    # -- JSON conversion ----------------------------------------------------------
    def jsonify(self, v, depth=0):
        if isinstance(v, ObjRef):
            return self.canon(v) if v else None
        if isinstance(v, Rotator):
            return {'Pitch': v[0], 'Yaw': v[1], 'Roll': v[2]}
        if isinstance(v, Vector):
            return [round(x, 5) for x in v]
        if isinstance(v, dict):
            return {k: self.jsonify(x, depth + 1) for k, x in v.items()}
        if isinstance(v, (list, tuple)):
            return [self.jsonify(x, depth + 1) for x in v]
        if isinstance(v, float):
            if math.isnan(v) or math.isinf(v):
                return None
            return round(v, 6)
        if isinstance(v, bytes):
            return None
        return v

    # -- dispatch -------------------------------------------------------------------
    def kind_of(self, obj):
        if obj.is_a('Texture') and not obj.is_a('Cubemap'):
            return 'textures'
        if obj.is_a('Material'):
            return 'materials'
        if obj.is_a('StaticMesh'):
            return 'staticmeshes'
        if obj.is_a('Sound') and not obj.is_a('SoundGroup'):
            return 'sounds'
        if obj.is_a('SoundGroup'):
            return 'soundgroup'
        if obj.is_a('SkeletalMesh'):
            return 'skeletalmeshes'
        if obj.is_a('MeshAnimation'):
            return 'animations'
        if obj.is_a('VertMesh'):
            return 'vertmeshes'
        return None

    def export_ref(self, ref):
        """Export whatever an object reference points at (if it is an asset)."""
        if ref is None or not ref:
            return None
        if ref.index < 0:
            cn = ref.class_name
            if cn in ('Class', 'Package') or cn.endswith('Property') or cn == 'Function':
                return None
        try:
            obj = ref.load()
        except (ReadError, IndexError) as e:
            self.error('resolve %s' % ref.path, e)
            return None
        if obj is None:
            key = ref.path.lower()
            if key not in self.done:
                self.done[key] = None
                self.error('resolve %s' % ref.path, 'object not found (missing package?)')
            return None
        return self.export_object(obj)

    def file_for(self, path, kind=None):
        """Output file of an exported object path (objects of different classes may share a
        path, e.g. a texture and a static mesh both called Group.Name)."""
        for key in self._by_path.get(path.lower(), ()):
            f = self.done.get(key)
            if f and (kind is None or f.startswith(kind + '/')):
                return f
        return None

    def export_object(self, obj):
        if not self.assets:
            return None
        # keyed by path and class: one package may hold a Texture and a StaticMesh (or a
        # SkeletalMesh and a MeshAnimation) with the same name
        key = '%s|%s' % (obj.path.lower(), obj.class_name.lower())
        self._by_path.setdefault(obj.path.lower(), []).append(key) if key not in self.done else None
        if key in self.done:
            self._note(key)
            return self.done[key]
        self.done[key] = None  # recursion guard
        self._paths[key] = obj.path
        try:
            kind = self.kind_of(obj)
        except Exception as e:  # class resolution problems
            self.error(obj.path, e)
            return None
        if kind is None:
            return None
        self._collect.append(set())
        try:
            result = self._export_kind(obj, kind)
        finally:
            self.obj_deps[key] = self._collect.pop()
        self.done[key] = result
        self._note(key)
        return result

    def _export_kind(self, obj, kind):
        result = None
        try:
            if kind == 'textures':
                result = self.export_texture(obj)
            elif kind == 'materials':
                result = self.export_material(obj)
            elif kind == 'staticmeshes':
                result = self.export_static_mesh(obj)
            elif kind == 'sounds':
                result = self.export_sound(obj)
            elif kind == 'skeletalmeshes':
                result = self.export_skeletal_mesh(obj)
            elif kind == 'animations':
                result = self.export_animation(obj)
            elif kind == 'vertmeshes':
                result = self.export_vert_mesh(obj)
            elif kind == 'soundgroup':
                for r in obj.props.get('Sounds', []) or []:
                    if isinstance(r, ObjRef):
                        self.export_ref(r)
        except Exception as e:
            self.error('%s (%s)' % (obj.path, obj.class_name), e)
            result = None
        return result

    # -- textures --------------------------------------------------------------------
    def export_texture(self, obj):
        png_path = self.out_path('textures', obj, '.png')
        p = obj.props
        if self.fresh(obj, png_path):
            for k in ('Detail', 'AnimNext'):
                if p.get(k):
                    self.export_ref(p[k])
            return self.rel(png_path)
        procedural = None
        try:
            img, info = tex.decode_texture(obj)
        except tex.TextureError as e:
            # procedural textures (Fire/Wet/Wave/Ice/FluidTexture) keep no pixels in the package:
            # fall back to their source texture, else a flat image in the palette's mean colour
            img, info, procedural = self._procedural_fallback(obj, e)
        if img.ndim == 3 and not info['has_alpha']:
            img = img[:, :, :3]
        write_png(png_path, img, self.png_level)
        p = obj.props
        hints = tex.resolve_material(obj)
        meta = {
            'type': 'Texture', 'class': obj.class_name, 'path': obj.path, 'scale': self.scale,
            'png': os.path.basename(png_path),
            'format': info['format'], 'width': info['USize'], 'height': info['VSize'],
            'mip_count': info['mip_count'],
            'bMasked': info['bMasked'], 'bAlphaTexture': info['bAlphaTexture'],
            'bTwoSided': info['bTwoSided'], 'has_alpha': info['has_alpha'],
            'png_bit_depth': 16 if img.dtype == np.uint16 else 8,
            'procedural_fallback': procedural,
            'material_hints': {
                'blend_mode': hints['blend_mode'],
                'two_sided': hints['two_sided'],
                'detail_texture': self.canon(p['Detail']) if p.get('Detail') else None,
                'detail_scale': p.get('DetailScale'),
                'u_clamp_mode': p.get('UClampMode'), 'v_clamp_mode': p.get('VClampMode'),
                'lod_set': p.get('LODSet'),
                'palette': self.canon(p['Palette']) if p.get('Palette') else None,
                'anim_next': self.canon(p['AnimNext']) if p.get('AnimNext') else None,
                'max_frame_rate': p.get('MaxFrameRate'),
                'surface_type': p.get('SurfaceType'),
            },
        }
        self.write_json(os.path.splitext(png_path)[0] + '.json', meta)
        self.counts['textures'] += 1
        if self.verbose:
            self.log('  texture  %s (%s %dx%d)' % (obj.path, info['format'], info['USize'], info['VSize']))
        if p.get('Detail'):
            self.export_ref(p['Detail'])
        if p.get('AnimNext'):
            self.export_ref(p['AnimNext'])
        return self.rel(png_path)

    def _procedural_fallback(self, obj, err):
        p = obj.props
        procedural = obj.is_a('FractalTexture', 'ScriptedTexture') or any(
            c in obj.class_chain for c in ('FireTexture', 'WaterTexture', 'WetTexture', 'WaveTexture',
                                           'IceTexture', 'FluidTexture'))
        if not procedural:
            raise err
        for k in ('SourceTexture', 'GlassTexture'):
            ref = p.get(k)
            src = ref.load() if isinstance(ref, ObjRef) and ref else None
            if src is not None and src.is_a('Texture'):
                try:
                    img, info = tex.decode_texture(src)
                    return img, info, {'from': k, 'texture': src.path, 'reason': str(err)}
                except tex.TextureError:
                    pass
        info = tex.texture_info(obj)
        w = max(1, min(int(obj.get('USize', 64) or 64), 512))
        h = max(1, min(int(obj.get('VSize', 64) or 64), 512))
        rgb = (128, 128, 128)
        pal_ref = p.get('Palette')
        pal_obj = pal_ref.load() if isinstance(pal_ref, ObjRef) and pal_ref else None
        if pal_obj is not None:
            try:
                pal = np.asarray(tex.read_palette(pal_obj), dtype=np.float64)
                rgb = tuple(int(x) for x in pal[:, :3].mean(0))
            except Exception:
                pass
        img = np.empty((h, w, 4), np.uint8)
        img[:, :, :3] = rgb
        img[:, :, 3] = 255
        info.update({'USize': w, 'VSize': h, 'mip_count': 0, 'has_alpha': False})
        return img, info, {'from': 'palette_mean' if pal_obj is not None else 'grey', 'reason': str(err)}

    # -- materials ---------------------------------------------------------------------
    def _material_refs(self, v, out):
        if isinstance(v, ObjRef):
            if v:
                out.append(v)
        elif isinstance(v, dict):
            for x in v.values():
                self._material_refs(x, out)
        elif isinstance(v, list):
            for x in v:
                self._material_refs(x, out)

    def export_material(self, obj):
        path = self.out_path('materials', obj, '.json')
        refs = []
        self._material_refs(obj.props, refs)
        for r in refs:
            self.export_ref(r)
        res = tex.resolve_material(obj)
        base = res['base_texture']
        base_file = None
        size = None
        if base is not None:
            base_file = self.export_object(base)
            size = list(tex.texture_size(base))
        meta = {
            'type': 'Material', 'class': obj.class_name, 'class_chain': obj.class_chain,
            'path': obj.path, 'scale': self.scale,
            'base_texture': base.path if base is not None else None,
            'base_texture_file': base_file,
            'base_texture_size': size,
            'blend_mode': res['blend_mode'],
            'two_sided': res['two_sided'],
            'unlit': res['unlit'],
            'emissive': res['emissive'], 'opacity': res['opacity'],
            'specular': res['specular'], 'detail': res['detail'],
            'resolve_chain': res['chain'],
            'properties': self.jsonify(obj.props),
        }
        for k in ('shader_output_blending', 'frame_buffer_blending', 'alpha_ref', 'constant_color'):
            if k in res:
                meta[k] = res[k]
        self.write_json(path, meta)
        self.counts['materials'] += 1
        return self.rel(path)

    # -- static meshes ------------------------------------------------------------------
    def export_static_mesh(self, obj):
        gpath = self.out_path('staticmeshes', obj, '.gltf')
        if self.fresh(obj, gpath):
            for m in obj.props.get('Materials') or []:
                ref = m.get('Material') if isinstance(m, dict) else m
                if isinstance(ref, ObjRef) and ref:
                    self.export_ref(ref)
            return self.rel(gpath)
        mesh = smesh.read_static_mesh(obj)
        nsec = len(mesh['sections'])
        mats = obj.props.get('Materials')
        mat_paths = []
        for i in range(nsec):
            ref = None
            if isinstance(mats, list) and i < len(mats):
                m = mats[i]
                ref = m.get('Material') if isinstance(m, dict) else m
            if isinstance(ref, ObjRef) and ref:
                self.export_ref(ref)
                mat_paths.append(self.canon(ref))
            else:
                mat_paths.append('None')
        prims = smesh.build_primitives(obj, mesh, self.scale)
        for p in prims:
            p['material'] = mat_paths[p['extras']['section']]
        gb = GltfBuilder(self.scale)
        gb.add_primitive_list(obj.name, prims)
        gb.write(gpath, {'source': obj.path})
        s = self.scale
        bb = mesh['bbox']
        karma = obj.props.get('KarmaProps')
        karma_props = None
        if karma:
            ko = karma.load()
            if ko is not None:
                karma_props = self.jsonify(ko.props)
        col_model = None
        if mesh['collision_ref']:
            try:
                col_model = obj.pkg.ref_path(mesh['collision_ref'])
            except IndexError:
                col_model = None
        meta = {
            'type': 'StaticMesh', 'path': obj.path, 'scale': s,
            'gltf': os.path.basename(gpath),
            'vertex_count': int(len(mesh['positions'])),
            'triangle_count': int(len(mesh['indices']) // 3),
            'uv_channels': len(mesh['uvs']),
            'has_vertex_colors': bool(mesh['vertex_colors']),
            'bounds': {'min': [x * s for x in bb['Min']], 'max': [x * s for x in bb['Max']]},
            'sections': [{'index': i, 'material': mat_paths[i],
                          'triangles': sec['num_faces'],
                          'enable_collision': smesh._section_collision(obj, i)}
                         for i, sec in enumerate(mesh['sections'])],
            'collision': {
                # StaticMesh is a native class without script defaults; the defaults
                # below are inferred from which values packages store explicitly.
                'UseSimpleLineCollision': bool(obj.props.get('UseSimpleLineCollision', False)),
                'UseSimpleBoxCollision': bool(obj.props.get('UseSimpleBoxCollision', True)),
                'UseSimpleKarmaCollision': bool(obj.props.get('UseSimpleKarmaCollision', True)),
                'UseVertexColor': bool(obj.props.get('UseVertexColor', False)),
                'collision_model': col_model,
                'karma_props': karma_props,
                'note': 'karma_props values are raw UE2 units (multiply lengths by scale)',
            },
        }
        self.write_json(os.path.splitext(gpath)[0] + '.json', meta)
        self.counts['staticmeshes'] += 1
        return self.rel(gpath)

    # -- sounds -------------------------------------------------------------------------------
    def export_sound(self, obj):
        meta_path = self.out_path('sounds', obj, '.json')
        if self.skip_existing and self.fresh(obj, meta_path, meta_path):
            try:
                with open(meta_path, 'r', encoding='utf-8') as f:
                    prev = json.load(f)
                main = os.path.join(os.path.dirname(meta_path), prev['file']) if prev.get('file') else meta_path
                if os.path.exists(main):
                    base = obj.props.get('BaseSound')
                    if base:
                        self.export_ref(base)
                    return self.rel(main)
            except (OSError, ValueError, KeyError):
                pass
        fmt, data = snd.read_sound(obj)
        meta = {'type': 'Sound', 'class': obj.class_name, 'path': obj.path, 'scale': self.scale,
                'format_name': fmt, 'bytes': len(data), 'properties': self.jsonify(obj.props)}
        if not data:
            # ProceduralSound and friends: no samples of their own, they reference BaseSound
            path = self.out_path('sounds', obj, '.json')
            base = obj.props.get('BaseSound')
            meta.update({'file': None, 'container': None,
                         'base_sound': self.canon(base) if base else None,
                         'base_sound_file': self.export_ref(base) if base else None})
            self.write_json(path, meta)
            self.counts['sounds'] += 1
            return self.rel(path)
        kind = snd.sound_kind(data) or safe(fmt.lower()) or 'bin'
        path = self.out_path('sounds', obj, '.' + kind)
        with open(path, 'wb') as f:
            f.write(data)
        meta.update({'file': os.path.basename(path), 'container': kind})
        if kind == 'wav':
            meta.update(snd.wav_info(data))
        self.write_json(os.path.splitext(path)[0] + '.json', meta)
        self.counts['sounds'] += 1
        return self.rel(path)

    # -- resumable runs ------------------------------------------------------------------------
    def fresh(self, obj, main_path, meta_path=None, version=TOOL_VERSION):
        """True when skip_existing is set and an earlier run already wrote this object's
        files (the json sidecar is written last) after the package file last changed."""
        if not self.skip_existing:
            return False
        meta_path = meta_path or os.path.splitext(main_path)[0] + '.json'
        try:
            mt = os.path.getmtime(meta_path)
            if main_path != meta_path and not os.path.exists(main_path):
                return False
            if mt < os.path.getmtime(obj.pkg.path):
                return False
            with open(meta_path, 'r', encoding='utf-8') as f:
                meta = json.load(f)
            return meta.get('tool_version', 1) >= version
        except (OSError, ValueError):
            return False

    # -- skeletal meshes --------------------------------------------------------------------------
    def _mesh_material_names(self, obj, d):
        names = []
        for m in d['materials']:
            ti = int(m['tex'])
            ref = None
            if 0 <= ti < len(d['textures']):
                ref = ObjRef(obj.pkg, d['textures'][ti])
            if ref:
                self.export_ref(ref)
                names.append(self.canon(ref))
            else:
                names.append('None')
        return names

    def _ref_path(self, pkg, index):
        if not index:
            return None
        try:
            return self.canon(ObjRef(pkg, index))
        except (ReadError, IndexError):
            return None

    def export_skeletal_mesh(self, obj):
        gpath = self.out_path('skeletalmeshes', obj, '.gltf')
        d = skel.read_skeletal_mesh(obj)
        anim_ref = ObjRef(obj.pkg, d['animation']) if d['animation'] else None
        if self.fresh(obj, gpath):
            self._mesh_material_names(obj, d)
            if anim_ref:
                self.export_ref(anim_ref)
            return self.rel(gpath)
        geo = skel.mesh_geometry(d)
        xf = skx.MeshTransform(d['mesh_scale'], d['mesh_origin'], d['rot_origin'], self.scale)
        sk_ = skx.Skeleton.from_mesh(d, xf)
        mat_names = self._mesh_material_names(obj, d)
        prims = skx.section_primitives(geo, xf, mat_names)
        gb = skx.SkinnedGltf(self.scale)
        roots, joints = gb.add_skeleton(sk_)
        skin = gb.add_skin(sk_, joints, roots[0])
        mnode = gb.add_skinned_mesh(obj.name, prims, skin)
        gb.write_doc(gpath, roots + [mnode], {'source': obj.path})
        pos = np.concatenate([p['positions'] for p in prims]) if prims else np.zeros((1, 3))
        sockets = []
        for alias, bone, c in zip(d['attach_aliases'], d['attach_bones'], d['attach_coords']):
            bi = next((i for i, b in enumerate(d['bones']) if b['name'].lower() == bone.lower()), None)
            axes = np.stack([c[1], c[2], c[3]], 1)       # columns: socket X/Y/Z axes in bone space
            try:
                q = skx.mat_to_quat(axes)
            except Exception:
                q = np.array([0.0, 0.0, 0.0, 1.0])
            sockets.append({
                'name': alias, 'bone': sk_.names[bi] if bi is not None else bone, 'bone_original': bone,
                'location': [round(float(x) * xf.s, 5) for x in c[0]],
                'rotation_quat': [round(float(x), 6) for x in q],
                'rotation': skx.quat_to_rotator(q),
                'raw_coords': {'origin': c[0].tolist(), 'x_axis': c[1].tolist(), 'y_axis': c[2].tolist(),
                               'z_axis': c[3].tolist()},
            })
        anim_path = self.canon(anim_ref) if anim_ref else None
        meta = {
            'type': 'SkeletalMesh', 'path': obj.path, 'scale': self.scale, 'tool_version': TOOL_VERSION,
            'gltf': os.path.basename(gpath),
            'geometry_source': geo['source'],
            'vertex_count': int(sum(len(p['positions']) for p in prims)),
            'triangle_count': int(len(geo['tris'])),
            'point_count': geo['point_count'], 'unweighted_points': geo['unweighted_points'],
            'bounds': {'min': [round(float(x), 4) for x in pos.min(0)],
                       'max': [round(float(x), 4) for x in pos.max(0)]},
            'mesh_transform': {
                'mesh_scale': list(d['mesh_scale']), 'mesh_origin': list(d['mesh_origin']),
                'rot_origin': list(d['rot_origin']), 'rot_origin_deg': rot_to_deg(d['rot_origin']),
                'baked': True, 'mirrored': bool(xf.mirror), 'uniform_scale': xf.uniform,
                'bone_scale': round(xf.s, 6),
                'note': 'actor = R(RotOrigin) * (MeshScale * (v - MeshOrigin)) * scale, baked into '
                        'vertices, bind pose and animations'},
            'bones': [{'name': sk_.names[i], 'original_name': b['name'], 'parent': b['parent'] if i else -1,
                       'location': [round(float(x), 5) for x in sk_.local_pos[i]],
                       'rotation_quat': [round(float(x), 7) for x in sk_.local_rot[i]],
                       'world_location': [round(float(x), 4) for x in sk_.world[i][:3, 3]]}
                      for i, b in enumerate(d['bones'])],
            'sockets': sockets,
            'materials': mat_names,
            'sections': [{'index': p['extras']['section'], 'material': p['material'],
                          'triangles': int(len(p['indices']) // 3)} for p in prims],
            'animation': anim_path,
            'animation_file': None,
            'lod_count': len(d['lods']),
            'lods': [{'distance_factor': l['distance_factor'], 'points': int(len(l['points']))}
                     for l in d['lods']],
            'collision': {
                'karma_props': self._ref_path(obj.pkg, d['karma_props']),
                'collision_mesh': self._ref_path(obj.pkg, d['collision_mesh']),
                'bone_spheres': [dict(s, offset=[x * xf.s for x in s['offset']], radius=s['radius'] * xf.s)
                                 for s in d['bone_spheres']],
                'bone_boxes': [dict(b, offset=[x * xf.s for x in b['offset']], radii=[x * xf.s for x in b['radii']])
                               for b in d['bone_boxes']],
                'note': 'offsets/radii scaled by bone_scale (bone space)'},
        }
        meta_path = os.path.splitext(gpath)[0] + '.json'
        if anim_ref:
            meta['animation_file'] = self.export_ref(anim_ref)
        self.write_json(meta_path, meta)
        self.counts['skeletalmeshes'] += 1
        if self.verbose:
            self.log('  skelmesh %s (%d bones, %d tris)' % (obj.path, len(d['bones']), len(geo['tris'])))
        return self.rel(gpath)

    # -- animations ---------------------------------------------------------------------------------
    def anim_mesh_index(self):
        """MeshAnimation path (lower) -> SkeletalMesh objects that use it as their Animation,
        over every non-map package of the install."""
        if self._anim_meshes is not None:
            return self._anim_meshes
        idx = {}
        for key, path in sorted(self.mgr.package_files().items()):
            if path.lower().endswith('.ut2'):
                continue
            ext = os.path.splitext(path)[1].lower()
            if ext not in ('.ukx', '.u', '.utx', '.usx'):
                continue
            pkg = self.mgr.load_package(key)
            if pkg is None:
                continue
            for i in pkg.exports_of_class('SkeletalMesh'):
                try:
                    o = pkg.get_object(i)
                    d = skel.read_skeletal_mesh(o)
                    if d['animation']:
                        a = self.canon(ObjRef(pkg, d['animation']))
                        idx.setdefault(a.lower(), []).append(o.path)
                except Exception:
                    continue
        self._anim_meshes = idx
        return idx

    def _anim_representative(self, obj, anim):
        """The SkeletalMesh whose skeleton the animation is exported onto."""
        cands = [self.mgr.resolve_path(p, 'SkeletalMesh') for p in self.anim_mesh_index().get(obj.path.lower(), [])]
        cands = [o for o in cands if o is not None]
        # meshes inside the same package as the animation (e.g. a map's own content)
        have = {o.path.lower() for o in cands}
        for i in obj.pkg.exports_of_class('SkeletalMesh'):
            o = obj.pkg.get_object(i)
            if o.path.lower() not in have:
                try:
                    d = skel.read_skeletal_mesh(o)
                except Exception:
                    continue
                if d['animation'] and self.canon(ObjRef(obj.pkg, d['animation'])).lower() == obj.path.lower():
                    cands.append(o)
        anames = {b['name'].lower() for b in anim['bones']}
        if not cands:
            # no mesh names this animation: any mesh of the same package whose skeleton has all of
            # the animation's bones (e.g. third-person weapon meshes)
            for i in obj.pkg.exports_of_class('SkeletalMesh'):
                o = obj.pkg.get_object(i)
                try:
                    d = skel.read_skeletal_mesh(o)
                except Exception:
                    continue
                if anames and anames <= {b['name'].lower() for b in d['bones']}:
                    cands.append(o)
        best = None
        for o in cands:
            try:
                d = skel.read_skeletal_mesh(o)
                skel.mesh_geometry(d)
            except Exception:
                continue
            names = {b['name'].lower() for b in d['bones']}
            score = (len(anames & names), o.pkg is obj.pkg, -len(names - anames))
            if best is None or score > best[0]:
                best = (score, o, d)
        if best is None:
            return None, None
        return best[1], best[2]

    def export_animation(self, obj):
        path = self.out_path('animations', obj, '.gltf')
        anim = skel.read_animation(obj)
        rep, d = self._anim_representative(obj, anim)
        meta_path = os.path.splitext(path)[0] + '.json'
        seqs = []
        notify_objs = []
        for i, s in enumerate(anim['sequences']):
            m = anim['moves'][i] if i < len(anim['moves']) else None
            nf = int(s['num_frames'])
            rate = float(s['rate']) or 30.0
            ns = []
            for n in s['notifies']:
                nobj = self._ref_path(obj.pkg, n.get('object'))
                entry = {'time': round(n['time'], 6), 'frame': round(n['time'] * nf, 3),
                         'function': n['function'], 'object': nobj}
                if n.get('object'):
                    try:
                        no = ObjRef(obj.pkg, n['object']).load()
                    except (ReadError, IndexError):
                        no = None
                    if no is not None:
                        entry['object_class'] = no.class_name
                        entry['object_properties'] = self.jsonify(no.props)
                        notify_objs.append(no)
                ns.append(entry)
            seqs.append({
                'index': i, 'name': s['name'], 'groups': s['groups'], 'start_frame': s['start_frame'],
                'num_frames': nf, 'rate': round(s['rate'], 6),
                'duration': round((nf - 1) / rate, 6) if nf > 1 else 0.0,
                'loop_duration': round(nf / rate, 6),
                'notifies': ns,
                'root_speed': [round(x * self.scale, 4) for x in m['root_speed']] if m else None,
                'track_time': round(m['track_time'], 6) if m else None,
                'key_counts': {'max_rot': max((len(t['quat']) for t in m['tracks']), default=0),
                               'max_pos': max((len(t['pos']) for t in m['tracks']), default=0)} if m else None,
            })
        meta = {
            'type': 'MeshAnimation', 'path': obj.path, 'scale': self.scale, 'tool_version': TOOL_VERSION,
            'gltf': None, 'representative_mesh': rep.path if rep is not None else None,
            'ref_bones': [{'name': b['name'], 'parent': b['parent']} for b in anim['bones']],
            'sequence_count': len(seqs), 'sequences': seqs,
            'compression': 'UE2 AnalogTrack: float quaternion/position key lists with per-key times '
                           '(1 key = constant track); resampled to every frame',
            'note': 'glTF animation i is sequence i; keys at time = frame / rate. Root keys carry the '
                    'baked mesh-to-actor transform of representative_mesh.',
        }
        for no in notify_objs:      # sounds / effects / materials the notifies use
            refs = []
            self._material_refs(no.props, refs)
            for r in refs:
                if r.class_name not in ('Class', 'Package'):
                    self.export_ref(r)
        if rep is None:
            meta['error'] = 'no SkeletalMesh uses this animation; not exported to glTF'
            self.write_json(meta_path, meta)
            self.counts['animations'] += 1
            return self.rel(meta_path)
        if self.fresh(obj, path, version=ANIM_TOOL_VERSION):
            return self.rel(path)
        meta['tool_version'] = ANIM_TOOL_VERSION
        xf = skx.MeshTransform(d['mesh_scale'], d['mesh_origin'], d['rot_origin'], self.scale)
        sk_ = skx.Skeleton.from_mesh(d, xf)
        gb = skx.SkinnedGltf(self.scale)
        roots, joints = gb.add_skeleton(sk_)
        skin = gb.add_skin(sk_, joints, roots[0])
        # a one-triangle placeholder mesh bound to the root keeps the joints a real skin
        r0 = sk_.world[0][:3, 3]
        tri = {'positions': np.array([r0, r0 + [1, 0, 0], r0 + [0, 1, 0]], np.float32),
               'normals': None, 'uvs': np.zeros((3, 2), np.float32),
               'joints': np.zeros((3, 4), np.uint16), 'weights': np.tile([1.0, 0, 0, 0], (3, 1)),
               'indices': np.array([0, 2, 1], np.uint32), 'material': 'None'}
        mnode = gb.add_skinned_mesh('AnimSkeleton', [tri], skin)
        unmatched_all = set()
        for i, s in enumerate(anim['sequences']):
            nf, chans, unmatched = skx.sample_sequence(anim, i, sk_, xf)
            unmatched_all.update(unmatched)
            rate = float(s['rate']) or 30.0
            times = np.arange(nf, dtype=np.float64) / rate
            channels = []
            for ji in range(len(sk_.names)):
                if ji in chans:
                    rot, pos = chans[ji]
                else:   # bones the animation does not drive keep their bind pose
                    rot = np.repeat(sk_.local_rot[ji][None], nf, 0)
                    pos = np.repeat(sk_.local_pos[ji][None], nf, 0)
                if nf == 1:   # a single pose: two identical keys (zero-length clips are dropped on import)
                    rot, pos = np.repeat(rot, 2, 0), np.repeat(pos, 2, 0)
                channels.append((joints[ji], 'rotation', rot))
                channels.append((joints[ji], 'translation', pos))
            if nf == 1:
                times = np.array([0.0, 1.0 / rate])
            gb.add_animation(s['name'], times, channels,
                             {'num_frames': nf, 'rate': s['rate'], 'sequence_index': i})
        gb.write_doc(path, roots + [mnode], {'source': obj.path, 'skeleton_from': rep.path})
        meta['gltf'] = os.path.basename(path)
        meta['skeleton_bones'] = sk_.names
        meta['unmatched_bones'] = sorted(unmatched_all)
        meta['mesh_transform'] = {'mesh_scale': list(d['mesh_scale']), 'mesh_origin': list(d['mesh_origin']),
                                  'rot_origin': list(d['rot_origin']), 'bone_scale': round(xf.s, 6)}
        self.write_json(meta_path, meta)
        self.counts['animations'] += 1
        if self.verbose:
            self.log('  anim     %s (%d sequences on %s)' % (obj.path, len(seqs), rep.path))
        return self.rel(path)

    # -- vertex-animated meshes ----------------------------------------------------------------------
    def export_vert_mesh(self, obj):
        """UE2 VertMesh -> skinned glTF with one joint per vertex under a root joint; every
        sequence becomes a glTF animation that moves the vertex joints (translation keys),
        so it imports as an ordinary skeletal mesh + AnimSequences."""
        gpath = self.out_path('vertmeshes', obj, '.gltf')
        d = skel.read_vert_mesh(obj)
        mat_names = self._mesh_material_names(obj, d)
        if self.fresh(obj, gpath, version=ANIM_TOOL_VERSION):
            return self.rel(gpath)
        frames = skel.unpack_vert_mesh_frames(d)          # (F, V, 3) mesh space
        nfr, nv, _ = frames.shape
        xf = skx.MeshTransform(d['mesh_scale'], d['mesh_origin'], d['rot_origin'], self.scale)
        W = d['wedges']
        faces = d['faces']
        if not len(W) or not len(faces):
            raise skel.SkelError('VertMesh without wedges/faces')
        bind = xf.points(frames[0])                       # baked frame 0 = bind pose
        names = ['Root'] + ['V%04d' % i for i in range(nv)]
        parents = [-1] + [0] * nv
        lrot = [np.array([0.0, 0.0, 0.0, 1.0])] * (nv + 1)
        lpos = [np.zeros(3)] + [bind[i] for i in range(nv)]
        sk_ = skx.Skeleton(names, parents, lrot, lpos)
        geo = {
            'positions': frames[0][W['v'].astype(np.int64)],
            'uvs': np.stack([W['u'], W['t']], 1).astype(np.float32),
            'joints': np.zeros((len(W), 4), np.int32), 'weights': np.zeros((len(W), 4)),
            'tris': faces['w'].astype(np.int64), 'mat': faces['mat'].astype(np.int64),
        }
        geo['joints'][:, 0] = W['v'].astype(np.int64) + 1
        geo['weights'][:, 0] = 1.0
        # FMeshFace.MaterialIndex -> FMeshMaterial -> Textures (same as skeletal meshes)
        prims = skx.section_primitives(geo, xf, mat_names, flip=True)
        gb = skx.SkinnedGltf(self.scale)
        roots, joints = gb.add_skeleton(sk_)
        skin = gb.add_skin(sk_, joints, roots[0])
        mnode = gb.add_skinned_mesh(obj.name, prims, skin)
        seqs = []
        for i, s in enumerate(d['sequences']):
            a = int(s['start_frame'])
            n = max(int(s['num_frames']), 1)
            fr = [min(max(a + k, 0), nfr - 1) for k in range(n)]
            rate = float(s['rate']) or 30.0
            if n == 1:   # a single pose: two identical keys (zero-length clips are dropped on import)
                fr = fr * 2
            P = np.stack([xf.points(frames[f]) for f in fr], 1)   # (V, n, 3)
            channels = [(joints[v + 1], 'translation', P[v]) for v in range(nv)]
            gb.add_animation(s['name'], np.arange(len(fr)) / rate, channels,
                             {'num_frames': n, 'rate': s['rate'], 'start_frame': a})
            seqs.append({'index': i, 'name': s['name'], 'groups': s['groups'], 'start_frame': a,
                         'num_frames': n, 'rate': round(s['rate'], 6),
                         'duration': round((n - 1) / rate, 6) if n > 1 else 0.0,
                         'loop_duration': round(n / rate, 6),
                         'notifies': [{'time': round(x['time'], 6), 'function': x['function'],
                                       'object': self._ref_path(obj.pkg, x.get('object'))}
                                      for x in s['notifies']]})
        gb.write_doc(gpath, roots + [mnode], {'source': obj.path, 'vertex_animation': True})
        meta = {
            'type': 'VertMesh', 'path': obj.path, 'scale': self.scale, 'tool_version': ANIM_TOOL_VERSION,
            'gltf': os.path.basename(gpath), 'frame_count': nfr, 'vertex_count': nv,
            'triangle_count': int(len(faces)),
            'bounds': {'min': [round(float(x), 4) for x in bind.min(0)],
                       'max': [round(float(x), 4) for x in bind.max(0)]},
            'mesh_transform': {'mesh_scale': list(d['mesh_scale']), 'mesh_origin': list(d['mesh_origin']),
                               'rot_origin': list(d['rot_origin']), 'baked': True},
            'materials': mat_names,
            'sections': [{'index': p['extras']['section'], 'material': p['material'],
                          'triangles': int(len(p['indices']) // 3)} for p in prims],
            'skeleton': 'Root + one joint per vertex (V0000...); animations move the vertex joints',
            'sequences': seqs,
        }
        self.write_json(os.path.splitext(gpath)[0] + '.json', meta)
        self.counts['vertmeshes'] += 1
        return self.rel(gpath)

    # -- music ----------------------------------------------------------------------------------
    def export_music_file(self, path):
        """Music/<Name>.ogg -> music/<Name>.ogg (copied as is) + music/<Name>.json."""
        from . import music
        import shutil
        name = os.path.splitext(os.path.basename(path))[0]
        d = os.path.join(self.out, 'music')
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, safe(name) + '.ogg')
        meta_path = os.path.join(d, safe(name) + '.json')
        if self.skip_existing and os.path.exists(dst) and os.path.exists(meta_path) \
                and os.path.getmtime(meta_path) >= os.path.getmtime(path):
            return self.rel(dst)
        with open(path, 'rb') as f:
            data = f.read()
        info = music.ogg_info(data)
        shutil.copyfile(path, dst)
        meta = {'type': 'Music', 'name': name, 'source_file': os.path.basename(path),
                'file': os.path.basename(dst), 'bytes': len(data), 'tool_version': TOOL_VERSION,
                'container': 'ogg'}
        meta.update(info)
        self.write_json(meta_path, meta)
        self.counts['music'] += 1
        return self.rel(dst)

    def music_files(self):
        out = []
        mdir = os.path.join(self.mgr.game_dir, 'Music')
        if os.path.isdir(mdir):
            for fn in sorted(os.listdir(mdir)):
                if fn.lower().endswith('.ogg'):
                    out.append(os.path.join(mdir, fn))
        return out

    # -- whole packages -----------------------------------------------------------------------
    def export_package(self, name):
        pkg = self.mgr.load_package(name)
        if pkg is None:
            self.error('package %s' % name, 'not found')
            return
        t0 = time.time()
        self.log('Package %s (%d exports)' % (pkg.name, len(pkg.exports)))
        n = 0
        for i in range(len(pkg.exports)):
            cn = pkg.export_class_name(i)
            if cn in ('Package', 'Class', 'Palette', 'Polys', 'Model', 'ReachSpec', 'Function', 'State',
                      'Enum', 'Const', 'Struct', 'TextBuffer', 'Field') or cn.endswith('Property'):
                continue
            try:
                obj = pkg.get_object(i)
                if self.kind_of(obj) is None:
                    continue
            except Exception as e:
                self.error(pkg.export_path(i), e)
                continue
            if self.export_object(obj):
                n += 1
        self.log('  %d objects in %.1fs' % (n, time.time() - t0))

    # -- maps ---------------------------------------------------------------------------------
    def export_map(self, name):
        from .level import MapExporter
        return MapExporter(self, name).run()
