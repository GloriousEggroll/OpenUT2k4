"""Map (.ut2) export: map.json with actors and gameplay markers, plus bsp.gltf."""

import math
import os
import time

import numpy as np

from .archive import ReadError
from .package import ObjRef
from .properties import Vector, Rotator
from . import bsp
from . import texture as tex
from .gltf import GltfBuilder
from .exporter import rot_to_deg, hsv_color, _NO_WALK

DT_STATIC_MESH = 8
SCHEMA_VERSION = 3

# Properties omitted from the per-actor "properties" dump (engine bookkeeping).
SKIP_PROPS = {'Level', 'Region', 'PhysicsVolume', 'PathList', 'StaticMeshInstance',
              'nextNavigationPoint', 'Touching', 'UpstreamPaths', 'NeededJump',
              'myMarker', 'XLevel', 'ColLocation', 'Brush', 'Layers', 'DecoLayers',
              'TerrainMap', 'Sectors', 'Vertices', 'ManualExcludes'}

LIGHT_TYPES = {0: 'None', 1: 'Steady', 2: 'Pulse', 3: 'Blink', 4: 'Flicker', 5: 'Strobe',
               6: 'BackdropLight', 7: 'SubtlePulse', 8: 'TexturePaletteOnce',
               9: 'TexturePaletteLoop', 10: 'FadeOut'}
LIGHT_EFFECTS = {0: 'None', 1: 'TorchWaver', 2: 'FireWaver', 3: 'WateryShimmer',
                 4: 'Searchlight', 5: 'SlowWave', 6: 'FastWave', 7: 'CloudCast',
                 8: 'StaticSpot', 9: 'Shock', 10: 'Disco', 11: 'Warp', 12: 'Spotlight',
                 13: 'NonIncidence', 14: 'Shell', 15: 'OmniBumpMap', 16: 'Interference',
                 17: 'Cylinder', 18: 'Rotor', 19: 'Negative', 20: 'Sunlight',
                 21: 'QuadraticNonIncidence'}


def _rot_matrix(rot):
    """Rows are the X, Y, Z axes of an Unreal rotator (pitch, yaw, roll)."""
    p, y, r = (v * 2.0 * math.pi / 65536.0 for v in rot)
    sp, cp = math.sin(p), math.cos(p)
    sy, cy = math.sin(y), math.cos(y)
    sr, cr = math.sin(r), math.cos(r)
    return np.array([
        [cp * cy, cp * sy, sp],
        [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
        [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp],
    ])


# Marker kinds added in schema 2 (see MapExporter._extra_marker / _post_markers).
EXTRA_MARKER_KINDS = ('vehicle_factories', 'vehicles', 'power_cores', 'power_nodes',
                      'power_link_setups', 'power_links', 'assault_objectives', 'triggers',
                      'spawn_managers', 'invasion_spawns', 'monsters')

MOVER_EVENTS = ('OpeningEvent', 'OpenedEvent', 'ClosingEvent', 'ClosedEvent', 'LoopEvent',
                'BumpEvent', 'PlayerBumpEvent', 'ReturnGroup')

TEAM_NAMES = {0: 'red', 1: 'blue', 255: 'neutral'}


def _s(v):
    """Name/str property -> str, None for empty / 'None'."""
    if v is None:
        return None
    if isinstance(v, ObjRef):
        return v.name if v else None
    v = str(v)
    return None if v in ('', 'None') else v


def _names(v):
    """Array of names / object refs / strings -> list of names."""
    if v is None:
        return []
    if not isinstance(v, (list, tuple)):
        v = [v]
    out = []
    for x in v:
        n = _s(x)
        if n:
            out.append(n)
    return out


def objective_type(chain):
    c = [x.lower() for x in chain]
    for key, name in (('onspowernode', 'power_node'), ('onspowercore', 'power_core'),
                      ('destroyvehicleobjective', 'destroy_vehicle'),
                      ('holdobjective', 'hold'), ('proximityobjective', 'proximity'),
                      ('useobjective', 'use'), ('triggeredobjective', 'triggered'),
                      ('destroyableobjective', 'destroyable'), ('lockedobjective', 'locked')):
        if key in c:
            return name
    return 'other'


def pickup_kind(chain):
    c = {x.lower() for x in chain}
    if 'tournamenthealth' in c or 'healthpack' in c:
        return 'health'
    if 'shieldpickup' in c or 'shieldpack' in c or 'supershieldpack' in c:
        return 'shield'
    if 'adrenalinepickup' in c:
        return 'adrenaline'
    if 'udamagepack' in c:
        return 'udamage'
    if 'ammo' in c:
        return 'ammo'
    if 'weaponpickup' in c:
        return 'weapon'
    return 'other'


class MapExporter:
    def __init__(self, ex, name):
        self.ex = ex
        self.mgr = ex.mgr
        self.name = name
        self.s = ex.scale
        self.deps = {}
        self._actor_cache = {}
        self._size_cache = {}

    # -- helpers ------------------------------------------------------------------
    def sv(self, v):
        """Scale a position vector."""
        if v is None:
            return None
        return [round(x * self.s, 4) for x in v]

    def dep(self, ref_or_obj):
        """Export a referenced asset; the exporter's collector records it (and
        everything it pulls in) as a dependency of this map."""
        if isinstance(ref_or_obj, ObjRef):
            return self.ex.export_ref(ref_or_obj)
        return self.ex.export_object(ref_or_obj)

    def walk_refs(self, obj, depth=0, seen=None):
        """Export every asset referenced by obj's properties; descend into
        non-actor sub-objects of the map (emitters, karma params, actions...)."""
        if seen is None:
            seen = set()
        refs = []
        self.ex._material_refs(obj.props, refs)
        for r in refs:
            if r.index < 0:
                cn = r.class_name
                if cn in ('Class', 'Package'):
                    continue
                self.dep(r)
                continue
            if r.pkg is not obj.pkg and r.index > 0:
                self.dep(r)
                continue
            key = (r.pkg.path, r.index)
            if key in seen:
                continue
            seen.add(key)
            cn = r.class_name.lower()
            if cn in _NO_WALK:
                continue
            try:
                o = r.load()
            except (ReadError, IndexError):
                continue
            if o is None:
                continue
            if self.ex.kind_of(o):
                self.dep(o)
            elif depth < 3 and not o.is_a('Actor'):
                self.walk_refs(o, depth + 1, seen)

    def actor_location(self, ref):
        if not ref:
            return None
        o = ref.load()
        if o is None:
            return None
        loc = o.props.get('Location')
        return self.sv(loc) if loc is not None else self.sv((0.0, 0.0, 0.0))

    def class_ref_info(self, ref):
        if ref is None or not ref:
            return None
        chain = self.mgr.class_chain(ref.pkg, ref.index)
        return {'class': ref.name, 'path': self.ex.canon(ref), 'class_chain': chain}

    def tex_size(self, mpath):
        hit = self._size_cache.get(mpath)
        if hit is None:
            hit = (256, 256)
            if mpath and mpath != 'None':
                try:
                    o = self.mgr.resolve_path(mpath)
                    if o is not None:
                        res = tex.resolve_material(o)
                        if res['base_texture'] is not None:
                            hit = tex.texture_size(res['base_texture'])
                except Exception:
                    pass
            self._size_cache[mpath] = hit
        return hit

    # -- level structure ----------------------------------------------------------
    def read_level(self, pkg):
        lv_idx = pkg.exports_of_class('Level')
        actors, model_idx = None, None
        if lv_idx:
            lv = pkg.get_object(lv_idx[0])
            try:
                r = lv.native_reader()
                n = r.i32()
                r.i32()
                if not (0 <= n <= r.remaining()):
                    raise ReadError('bad actor count')
                actors = [r.index() for _ in range(n)]
                for _ in range(4):
                    r.fstring()          # URL protocol, host, map, portal
                for _ in range(r.index()):
                    r.fstring()          # URL options
                r.i32()
                r.i32()                  # port, valid
                model_idx = r.index()
                if model_idx <= 0 or pkg.export_class_name(model_idx - 1) != 'Model':
                    model_idx = None
            except (ReadError, IndexError) as e:
                self.ex.error('%s level header' % pkg.name, e)
                actors = None
        if actors is None:
            actors = []
            for i in range(len(pkg.exports)):
                try:
                    if pkg.get_object(i).is_a('Actor'):
                        actors.append(i + 1)
                except Exception:
                    pass
        if model_idx is None:
            models = pkg.exports_of_class('Model')
            if models:
                model_idx = max(models, key=lambda i: pkg.exports[i].size) + 1
        return [a for a in actors if a > 0], model_idx

    # -- main -------------------------------------------------------------------------
    def run(self):
        t0 = time.time()
        ex = self.ex
        pkg = self.mgr.load_package(self.name)
        if pkg is None:
            if os.path.isfile(self.name):
                pkg = self.mgr.open_file(self.name)
            else:
                ex.error('map %s' % self.name, 'not found')
                return None
        map_name = pkg.name
        out_dir = os.path.join(ex.out, 'maps', ex_safe(map_name))
        if ex.skip_existing and self.up_to_date(pkg, out_dir):
            ex.log('Map %s is up to date' % map_name)
            ex.counts['maps_skipped'] = ex.counts.get('maps_skipped', 0) + 1
            return None
        ex.log('Map %s (v%d, %d exports)' % (map_name, pkg.version, len(pkg.exports)))
        os.makedirs(out_dir, exist_ok=True)
        timings = {}

        t = time.time()
        depth = len(ex._collect)
        ex.begin_collect()
        try:
            return self._run(pkg, map_name, out_dir, timings, t0)
        finally:
            del ex._collect[depth:]

    @staticmethod
    def up_to_date(pkg, out_dir):
        import json
        mj = os.path.join(out_dir, 'map.json')
        try:
            if os.path.getmtime(mj) < os.path.getmtime(pkg.path):
                return False
            with open(mj, 'r', encoding='utf-8') as f:
                head = f.read(4096)
            # schema_version is near the top of the file
            i = head.find('"schema_version"')
            if i < 0:
                return False
            v = int(head[i:].split(':', 1)[1].split(',', 1)[0])
            return v >= SCHEMA_VERSION
        except (OSError, ValueError, IndexError):
            return False

    def _run(self, pkg, map_name, out_dir, timings, t0):
        ex = self.ex
        n_err0 = len(ex.errors)
        t = time.time()
        actor_idx, model_idx = self.read_level(pkg)
        actors = []
        objs = []
        for ai in actor_idx:
            try:
                o = pkg.get_object(ai - 1)
                rec = self.actor_record(o, len(actors))
            except Exception as e:
                ex.error('%s actor %s' % (map_name, pkg.ref_name(ai)), e)
                continue
            actors.append(rec)
            objs.append(o)
        timings['actors'] = round(time.time() - t, 3)

        t = time.time()
        for o, rec in zip(objs, actors):
            try:
                self.walk_refs(o)
                if rec.get('static_mesh'):
                    sm = o.get('StaticMesh')
                    if sm:
                        self.dep(sm)
            except Exception as e:
                ex.error('%s deps of %s' % (map_name, o.name), e)
        timings['assets'] = round(time.time() - t, 3)

        t = time.time()
        bsp_info = None
        if model_idx:
            try:
                bsp_info = self.export_bsp(pkg, model_idx, out_dir)
            except Exception as e:
                ex.error('%s BSP' % map_name, e)
        timings['bsp'] = round(time.time() - t, 3)

        t = time.time()
        markers = self.markers(objs, actors)
        timings['markers'] = round(time.time() - t, 3)

        info = self.level_info(pkg, objs)
        self.deps = ex.end_collect()
        doc = {
            'schema': 'ut2k4-map', 'schema_version': SCHEMA_VERSION,
            'scale': self.s,
            'units': 'project centimetres (UE2 units * scale)',
            'coordinate_system': 'Unreal (X forward, Y right, Z up, left-handed) - same axes as UE5',
            'rotation_units': 'degrees (pitch, yaw, roll); rotation_raw is UE2 65536-per-turn',
            'transform_note': 'world = location + R(rotation) * (draw_scale * draw_scale3d * '
                              '(vertex - pre_pivot)); mesh vertices are already scaled',
            'properties_note': 'actor "properties" are raw UE2 values (not scaled); object '
                               'references are full object paths',
            'map': map_name,
            'source_file': os.path.basename(pkg.path),
            'package_version': pkg.version,
            'licensee_version': pkg.licensee,
            'game_type_prefix': map_name.split('-', 1)[0].upper() if '-' in map_name else None,
            'summary': info['summary'],
            'level_info': info['level_info'],
            'kill_z': info['kill_z'],
            'bsp': bsp_info,
            'markers': markers,
            'dependencies': self.deps,
            'actor_count': len(actors),
            'actors': actors,
        }
        timings['total'] = round(time.time() - t0, 3)
        doc['export_stats'] = {'timings_s': timings, 'errors': len(ex.errors) - n_err0}
        ex.write_json(os.path.join(out_dir, 'map.json'), ex.jsonify(doc))
        ex.counts['maps'] += 1
        ex.log('  %d actors, %s, %d deps, %.1fs' % (
            len(actors),
            ('%d BSP triangles' % bsp_info['triangles']) if bsp_info else 'no BSP',
            sum(len(v) for v in self.deps.values()), timings['total']))
        return doc

    # -- actors -------------------------------------------------------------------------
    def actor_record(self, o, idx):
        p = o.props
        loc = p.get('Location', Vector((0.0, 0.0, 0.0)))
        rot = p.get('Rotation', Rotator((0, 0, 0)))
        ds3 = o.get('DrawScale3D', Vector((1.0, 1.0, 1.0)))
        draw_type = o.get('DrawType', 1)
        rec = {
            'index': idx,
            'name': o.name,
            'path': o.path,
            'class': o.class_name,
            'class_chain': o.class_chain,
            'location': self.sv(loc),
            'rotation': rot_to_deg(rot),
            'rotation_raw': list(rot),
            'draw_scale': o.get('DrawScale', 1.0),
            'draw_scale3d': list(ds3) if not isinstance(ds3, dict) else [1.0, 1.0, 1.0],
            'pre_pivot': self.sv(p.get('PrePivot', (0.0, 0.0, 0.0))),
            'draw_type': draw_type,
            'static_mesh': None,
            'tag': o.get('Tag', o.class_name),
            'event': o.get('Event', 'None'),
            'hidden': bool(o.get('bHidden', False)),
            'collision_radius': round(o.get('CollisionRadius', 0.0) * self.s, 4),
            'collision_height': round(o.get('CollisionHeight', 0.0) * self.s, 4),
            'base': self.ex.canon(p['Base']) if p.get('Base') else None,
        }
        if draw_type == DT_STATIC_MESH or o.is_a('StaticMeshActor', 'Mover'):
            sm = o.get('StaticMesh')
            if sm:
                rec['static_mesh'] = self.ex.canon(sm)
        skins = p.get('Skins')
        if isinstance(skins, list) and skins:
            rec['skins'] = [self.ex.canon(s) if isinstance(s, ObjRef) and s else None for s in skins]
        rec['properties'] = {k: v for k, v in p.items() if k.split('[')[0] not in SKIP_PROPS}
        return rec

    # -- markers -------------------------------------------------------------------------
    def markers(self, objs, actors):
        m = {k: [] for k in (
            'player_starts', 'path_nodes', 'inventory_spots', 'jump_spots', 'other_nav_points',
            'weapon_bases', 'weapon_lockers', 'pickup_bases', 'pickups', 'jump_pads',
            'teleporters', 'flag_bases', 'bomb_spawns', 'bomb_deliveries', 'domination_points',
            'game_objectives', 'lifts', 'lights', 'zones', 'volumes', 'terrains',
            'ambient_sounds', 'movers')}
        for k in EXTRA_MARKER_KINDS:
            m[k] = []
        for o, rec in zip(objs, actors):
            try:
                self._marker(o, rec, m)
            except Exception as e:
                self.ex.error('%s marker %s' % (self.name, o.name), e)
            try:
                self._extra_marker(o, rec, m)
            except Exception as e:
                self.ex.error('%s extra marker %s' % (self.name, o.name), e)
        try:
            self._post_markers(m)
        except Exception as e:
            self.ex.error('%s marker links' % self.name, e)
        return m

    def _base(self, rec, **kw):
        d = {'actor': rec['name'], 'actor_index': rec['index'], 'class': rec['class'],
             'location': rec['location'], 'rotation': rec['rotation']}
        d.update(kw)
        return d

    def _marker(self, o, rec, m):
        p = o.props
        is_a = o.is_a
        if is_a('PlayerStart'):
            m['player_starts'].append(self._base(
                rec, team=o.get('TeamNumber', 0), enabled=bool(o.get('bEnabled', True)),
                primary=bool(o.get('bPrimaryStart', True)), tag=_s(rec['tag']),
                event=_s(o.get('Event'))))
        elif is_a('xWeaponBase'):
            wt = p.get('WeaponType')
            m['weapon_bases'].append(self._base(
                rec, weapon_class=self.class_ref_info(wt) if wt else None,
                weapon_class_property='WeaponType'))
        elif is_a('WeaponLocker'):
            ws = []
            for w in p.get('Weapons', []) or []:
                if isinstance(w, dict):
                    ws.append({'weapon_class': self.class_ref_info(w.get('WeaponClass')),
                               'extra_ammo': w.get('ExtraAmmo', 0)})
            m['weapon_lockers'].append(self._base(rec, weapons=ws, weapon_class_property='Weapons[].WeaponClass'))
        elif is_a('WildCardBase'):
            cls = [self.class_ref_info(v) for k, v in sorted(p.items())
                   if k.split('[')[0] == 'PickupClasses' and isinstance(v, ObjRef) and v]
            m['pickup_bases'].append(self._base(
                rec, powerup=None, kind='wildcard', pickup_classes=cls,
                powerup_property='PickupClasses[]'))
        elif is_a('xPickUpBase'):
            pu = o.get('PowerUp')
            info = self.class_ref_info(pu) if pu else None
            m['pickup_bases'].append(self._base(
                rec, powerup=info, kind=pickup_kind(info['class_chain']) if info else 'none',
                powerup_property='PowerUp'))
        elif is_a('Pickup'):
            m['pickups'].append(self._base(
                rec, kind=pickup_kind(o.class_chain),
                inventory_type=self.ex.canon(o.get('InventoryType')) if o.get('InventoryType') else None))
        elif is_a('JumpPad'):
            tgt = p.get('JumpTarget')
            jv = o.get('JumpVelocity', (0.0, 0.0, 0.0))
            m['jump_pads'].append(self._base(
                rec, jump_target=tgt.name if tgt else None,
                jump_target_location=self.actor_location(tgt) if tgt else None,
                jump_velocity=self.sv(jv), jump_velocity_raw=list(jv),
                jump_z_modifier=o.get('JumpZModifier', 1.0)))
        elif is_a('Teleporter'):
            m['teleporters'].append(self._base(
                rec, url=o.get('URL', ''), tag=rec['tag'], enabled=bool(o.get('bEnabled', True))))
        elif is_a('CTFBase'):
            cn = o.class_name.lower()
            team = 0 if 'red' in cn else 1 if 'blue' in cn else o.get('DefenderTeamIndex', 0)
            m['flag_bases'].append(self._base(rec, team=team))
        elif is_a('xBombSpawn'):
            m['bomb_spawns'].append(self._base(rec))
        elif is_a('xBombDelivery'):
            m['bomb_deliveries'].append(self._base(
                rec, team=o.get('Team', o.get('DefenderTeamIndex', 0)),
                touchdown_difficulty=o.get('TouchDownDifficulty'), tag=_s(o.get('Tag')),
                event=_s(o.get('Event'))))
        elif is_a('xDomPoint', 'DominationPoint'):
            cn = o.class_name.lower()
            letter = 'A' if cn.endswith('a') and 'dompoint' in cn else 'B' if cn.endswith('b') and 'dompoint' in cn else None
            m['domination_points'].append(self._base(
                rec, point_name=o.get('PointName', ''), letter=letter,
                primary_team=o.get('PrimaryTeam'), defender_team=o.get('DefenderTeamIndex', 255),
                controllable=bool(o.get('bControllable', True)), control_event=_s(o.get('ControlEvent')),
                tag=_s(o.get('Tag')), event=_s(o.get('Event'))))
        elif is_a('GameObjective'):
            d = self._base(rec, defender_team=o.get('DefenderTeamIndex', 0),
                           objective_name=o.get('ObjectiveName', ''))
            d.update(self._objective_fields(o, rec))
            m['game_objectives'].append(d)
        elif is_a('LiftCenter', 'LiftExit'):
            m['lifts'].append(self._base(rec, lift_tag=o.get('LiftTag', 'None')))
        elif is_a('InventorySpot'):
            m['inventory_spots'].append(self._base(rec))
        elif is_a('JumpSpot', 'JumpDest'):
            m['jump_spots'].append(self._base(rec))
        elif is_a('PathNode'):
            m['path_nodes'].append(self._base(rec))
        elif is_a('NavigationPoint'):
            m['other_nav_points'].append(self._base(rec))
        elif is_a('Light'):
            hue = o.get('LightHue', 0)
            sat = o.get('LightSaturation', 255)
            rad = o.get('LightRadius', 64.0)
            lt = o.get('LightType', 1)
            le = o.get('LightEffect', 0)
            m['lights'].append(self._base(
                rec, light_type=LIGHT_TYPES.get(lt, lt), light_effect=LIGHT_EFFECTS.get(le, le),
                brightness=o.get('LightBrightness', 64.0), hue=hue, saturation=sat,
                color_rgb=hsv_color(hue, sat), radius=rad,
                radius_world=round(25.0 * (rad + 1.0) * self.s, 3),
                cone=o.get('LightCone', 128), dynamic=bool(o.get('bDynamicLight', False)),
                directional=bool(o.get('bDirectional', False)) or is_a('Spotlight', 'Sunlight'),
                sunlight=is_a('Sunlight')))
        elif is_a('ZoneInfo'):
            if is_a('LevelInfo'):
                return
            fog = o.get('DistanceFogColor')
            m['zones'].append(self._base(
                rec, sky_zone=is_a('SkyZoneInfo'), zone_tag=o.get('ZoneTag', 'None'),
                kill_z=self.sv([0, 0, p['KillZ']])[2] if 'KillZ' in p else None,
                soft_kill_z=bool(o.get('bSoftKillZ', False)),
                distance_fog=bool(o.get('bDistanceFog', False)),
                fog_color=fog, fog_start=o.get('DistanceFogStart'), fog_end=o.get('DistanceFogEnd'),
                ambient_brightness=o.get('AmbientBrightness', 0),
                ambient_hue=o.get('AmbientHue', 0), ambient_saturation=o.get('AmbientSaturation', 255),
                terrain_zone=bool(o.get('bTerrainZone', False))))
        elif is_a('Volume'):
            m['volumes'].append(self._volume(o, rec))
        elif is_a('TerrainInfo'):
            m['terrains'].append(self._terrain(o, rec))
        elif is_a('AmbientSound'):
            snd = o.get('AmbientSound')
            m['ambient_sounds'].append(self._base(
                rec, sound=self.ex.canon(snd) if snd else None,
                radius=o.get('SoundRadius'), volume=o.get('SoundVolume')))
        if is_a('Mover'):
            m['movers'].append(self._base(
                rec, tag=_s(rec['tag']), event=_s(o.get('Event')),
                trigger_events={k: _s(o.get(k)) for k in MOVER_EVENTS if _s(o.get(k))},
                bump_type=o.get('BumpType'), stay_open_time=o.get('StayOpenTime'),
                trigger_once_only=bool(o.get('bTriggerOnceOnly', False)),
                damage_triggered=bool(o.get('bDamageTriggered', False)),
                static_mesh=rec['static_mesh'], key_num=o.get('NumKeys', 0),
                key_pos=[self.sv(v) for k, v in sorted(p.items()) if k.split('[')[0] == 'KeyPos'],
                key_rot=[rot_to_deg(v) for k, v in sorted(p.items()) if k.split('[')[0] == 'KeyRot'],
                move_time=o.get('MoveTime', 1.0), initial_state=o.get('InitialState')))


    # -- schema 2 markers --------------------------------------------------------------------
    def _ref(self, v):
        return self.ex.canon(v) if isinstance(v, ObjRef) and v else None

    def _props(self, o):
        return {k: v for k, v in o.props.items() if k.split('[')[0] not in SKIP_PROPS}

    def _objective_fields(self, o, rec):
        g = o.get
        d = {
            'objective_type': objective_type(o.class_chain),
            'tag': _s(rec['tag']), 'event': _s(g('Event')),
            'defense_priority': g('DefensePriority', 0),
            'objective_priority': g('ObjectivePriority', 0),
            'description': g('ObjectiveDescription', ''),
            'info_attacker': g('Objective_Info_Attacker', ''),
            'info_defender': g('Objective_Info_Defender', ''),
            'destruction_message': g('DestructionMessage', ''),
            'use_description': g('UseDescription'),
            'score': g('Score', 5),
            'optional': bool(g('bOptionalObjective', False)),
            'initially_active': bool(g('bInitiallyActive', True)),
            'bot_only': bool(g('bBotOnlyObjective', False)),
            'team_controlled': bool(g('bTeamControlled', False)),
            'trigger_once_only': bool(g('bTriggerOnceOnly', False)),
            'must_board_vehicle_first': bool(g('bMustBoardVehicleFirst', False)),
            'defense_script_tags': _s(g('DefenseScriptTags')),
            'area_volume_tag': _s(g('AreaVolumeTag')),
            'critical_volume_tag': _s(g('CriticalObjectiveVolumeTag')),
            'physical_actors_tag': _s(g('PhysicalObjectiveActorsTag')),
            'vehicle_path_name': _s(g('VehiclePathName')),
            'end_camera_tag': _s(g('EndCameraTag')),
            'icon': self._ref(g('ObjectiveTypeIcon')),
            'announcer': {k: self._ref(g(k)) for k in ('Announcer_ObjectiveInfo', 'Announcer_DisabledObjective',
                                                        'Announcer_DefendObjective') if self._ref(g(k))},
        }
        if o.is_a('DestroyableObjective'):
            d.update({'damage_capacity': g('DamageCapacity', 100), 'health': g('Health'),
                      'can_defender_damage': bool(g('bCanDefenderDamage', False)),
                      'damage_event': _s(g('TakeDamageEvent')),
                      'damage_event_threshold': g('DamageEventThreshold'),
                      'link_heal_mult': g('LinkHealMult'),
                      'constraint_pawn_class': self._ref(g('ConstraintPawnClass'))})
        if o.is_a('ProximityObjective'):
            d['constraint_pawn_class'] = self._ref(g('ConstraintPawnClass'))
        if o.is_a('DestroyVehicleObjective'):
            d['vehicle_factory_tag'] = _s(g('VehicleFactoryName')) or _s(g('VehicleFactoryTag'))
        return d

    def _core_fields(self, o, rec):
        g = o.get
        team = g('DefenderTeamIndex', 0)
        neutral = bool(g('bStartNeutral', False)) or o.is_a('ONSPowerNodeNeutral')
        return self._base(
            rec, is_node=o.is_a('ONSPowerNode'), final_core=bool(g('bFinalCore', False)),
            team=255 if neutral and o.is_a('ONSPowerNode') else team,
            team_name=TEAM_NAMES.get(255 if neutral and o.is_a('ONSPowerNode') else team, str(team)),
            start_neutral=neutral, node_num=g('NodeNum'),
            linked_nodes=_names(g('LinkedNodes')),
            construction_time=g('ConstructionTime'),
            tag=_s(rec['tag']), event=_s(g('Event')),
            destroyed_event=_s(g('DestroyedEvent')), constructed_event=_s(g('ConstructedEvent')),
            red_activation_event=_s(g('RedActivationEventName')),
            blue_activation_event=_s(g('BlueActivationEventName')),
            area_volume_tag=_s(g('AreaVolumeTag')), defense_script_tags=_s(g('DefenseScriptTags')),
            properties=self._props(o))

    def _extra_marker(self, o, rec, m):
        is_a = o.is_a
        g = o.get
        if is_a('SVehicleFactory'):
            ons = is_a('ONSVehicleFactory')
            assault = is_a('ASVehicleFactory')
            vc = g('VehicleClass')
            if assault:
                team = g('VehicleTeam', 1)
                respawn = g('RespawnDelay', 3.0)
            else:
                team = g('TeamNum', 0)
                respawn = g('RespawnTime', 15.0)
            d = self._base(
                rec, kind='onslaught' if ons else 'assault' if assault else 'other',
                vehicle_class=self.class_ref_info(vc) if isinstance(vc, ObjRef) and vc else None,
                vehicle_class_name=vc.name if isinstance(vc, ObjRef) and vc else _s(g('VehicleClassStr')),
                vehicle_class_str=_s(g('VehicleClassStr')),
                team=team, team_name=TEAM_NAMES.get(team, str(team)),
                respawn_time=respawn, max_vehicle_count=g('MaxVehicleCount', 1),
                tag=_s(rec['tag']), event=_s(g('Event')))
            if ons:
                d.update(active=bool(g('bActive', False)), never_activate=bool(g('bNeverActivate', False)),
                         reverse_blue_team_direction=bool(g('bReverseBlueTeamDirection', False)),
                         pre_spawn=bool(g('bPreSpawn', False)))
            if assault:
                d.update(enabled=bool(g('bEnabled', True)), vehicle_health=g('VehicleHealth'),
                         respawn_when_destroyed=bool(g('bRespawnWhenDestroyed', False)),
                         team_locked=bool(g('bVehicleTeamLock', False)),
                         key_vehicle=bool(g('bKeyVehicle', False)),
                         triggered_function=g('TriggeredFunction'),
                         vehicle_tag=_s(g('VehicleTag')), vehicle_event=_s(g('VehicleEvent')),
                         vehicle_possessed_event=_s(g('VehiclePossessedEvent')),
                         next_factory_tag=_s(g('NextFactoryTag')),
                         objective_tags=_names([v for k, v in sorted(o.props.items())
                                                if k.split('[')[0] == 'ObjectiveTag']))
            d['properties'] = self._props(o)
            m['vehicle_factories'].append(d)
        elif is_a('Vehicle') or is_a('ONSWeaponPawn'):
            m['vehicles'].append(self._base(
                rec, team=g('Team'), class_chain=o.class_chain, tag=_s(rec['tag']),
                properties=self._props(o)))
        if is_a('ONSPowerCore'):
            m['power_nodes' if is_a('ONSPowerNode') else 'power_cores'].append(self._core_fields(o, rec))
        if is_a('ONSPowerLinkOfficialSetup', 'ONSPowerLinkCustomSetup', 'ONSPowerLinkSetup'):
            links = []
            for st in g('LinkSetups') or []:
                if isinstance(st, dict):
                    links.append({'base_node': _s(st.get('BaseNode')),
                                  'linked_nodes': _names(st.get('LinkedNodes'))})
            m['power_link_setups'].append(self._base(
                rec, setup_name=g('SetupName', 'Default' if is_a('ONSPowerLinkOfficialSetup') else None),
                custom=is_a('ONSPowerLinkCustomSetup'), links=links))
        if is_a('PlayerSpawnManager'):
            m['spawn_managers'].append(self._base(
                rec, tag=_s(rec['tag']), enabled=bool(g('bEnabled', True)),
                player_start_team=g('PlayerStartTeam', 0), assault_team=g('AssaultTeam', 0),
                allow_teleporting=bool(g('bAllowTeleporting', True)),
                disabled_vehicle_factories_tags=_names(g('DisabledVehicleFactoriesTag')),
                forced_pawn_class=self._ref(g('ForcedPawnClass')),
                override_pawn_class=g('OverridePawnClass')))
        if is_a('InvasionPoint') or 'invasion' in o.class_name.lower():
            m['invasion_spawns'].append(self._base(rec, tag=_s(rec['tag']), properties=self._props(o)))
        if is_a('Monster'):
            m['monsters'].append(self._base(rec, tag=_s(rec['tag']), class_chain=o.class_chain))
        cn = o.class_name.lower()
        if is_a('Triggers', 'ScriptedTrigger', 'DestroyableTrigger', 'ASCriticalObjectiveVolume') \
                or ('trigger' in cn and not is_a('NavigationPoint')):
            d = self._base(
                rec, tag=_s(rec['tag']), event=_s(g('Event')), class_chain=o.class_chain,
                initially_active=bool(g('bInitiallyActive', True)),
                trigger_type=g('TriggerType'), initial_state=_s(g('InitialState')),
                message=g('Message'), objective_tags=_names([v for k, v in sorted(o.props.items())
                                                             if k.split('[')[0] == 'ObjectiveTag']),
                team=g('Team', g('TeamNum')), health=g('Health'),
                collision_radius=rec['collision_radius'], collision_height=rec['collision_height'],
                properties=self._props(o))
            if is_a('ScriptedSequence'):
                acts = []
                for r in g('Actions') or []:
                    if not isinstance(r, ObjRef) or not r:
                        continue
                    try:
                        a = r.load()
                    except Exception:
                        a = None
                    if a is None:
                        continue
                    acts.append({'class': a.class_name, 'properties': dict(a.props)})
                d['actions'] = acts
            m['triggers'].append(d)

    def _post_markers(self, m):
        # --- Onslaught links: the official "Default" setup, else the cores' LinkedNodes
        cores = m['power_cores'] + m['power_nodes']
        names = {c['actor'] for c in cores}
        pairs = set()
        source = None
        default = [s for s in m['power_link_setups'] if not s['custom']
                   and str(s.get('setup_name') or 'Default').lower() == 'default']
        if default:
            source = 'ONSPowerLinkOfficialSetup:%s' % default[0]['actor']
            for l in default[0]['links']:
                for n in l['linked_nodes']:
                    if l['base_node'] and n:
                        pairs.add(tuple(sorted((l['base_node'], n))))
        if not pairs:
            for c in cores:
                for n in c['linked_nodes']:
                    pairs.add(tuple(sorted((c['actor'], n))))
            if pairs:
                source = 'LinkedNodes'
        if not pairs and m['power_link_setups']:
            s0 = m['power_link_setups'][0]
            source = '%s:%s' % (s0['class'], s0['actor'])
            for l in s0['links']:
                for n in l['linked_nodes']:
                    if l['base_node'] and n:
                        pairs.add(tuple(sorted((l['base_node'], n))))
        for a, b in sorted(pairs):
            m['power_links'].append({'a': a, 'b': b, 'source': source,
                                     'valid': a in names and b in names})
        # owning node of each ONS vehicle factory (UT2004 assigns factories to the closest core/node)
        for f in m['vehicle_factories']:
            if f['kind'] != 'onslaught' or not cores:
                continue
            best = min(cores, key=lambda c: sum((c['location'][i] - f['location'][i]) ** 2 for i in range(3)))
            f['nearest_power_node'] = best['actor']
            f['nearest_power_node_distance'] = round(math.sqrt(sum(
                (best['location'][i] - f['location'][i]) ** 2 for i in range(3))), 2)

        # --- Assault: stage order = DefensePriority, highest first (equal priority = same stage)
        objs = [o for o in m['game_objectives'] if o.get('objective_type') not in ('power_core', 'power_node')]
        prios = sorted({o.get('defense_priority') or 0 for o in objs if not o.get('optional')}, reverse=True)
        for o in objs:
            p = o.get('defense_priority') or 0
            a = dict(o)
            a['order'] = (prios.index(p) + 1) if p in prios else None
            a['order_note'] = 'stage rank by DefensePriority (highest first); optional objectives have no stage'
            m['assault_objectives'].append(a)
        m['assault_objectives'].sort(key=lambda o: (o['order'] is None, o['order'] or 0, o['actor']))

        # --- event graph: triggers / movers relevant to objectives
        nodes = []
        for kind in ('triggers', 'movers', 'vehicle_factories', 'spawn_managers'):
            for t in m[kind]:
                # UE2's default Tag is the class name: it links nothing
                ins = {x for x in (t.get('tag'),) if x and x.lower() != str(t.get('class', '')).lower()}
                outs = {x for x in (t.get('event'),) if x}
                outs |= set(t.get('objective_tags') or [])
                outs |= {v for v in (t.get('trigger_events') or {}).values() if v}
                for a in t.get('actions') or []:
                    for k, v in a['properties'].items():
                        kl = k.lower()
                        if isinstance(v, str) and v not in ('', 'None') and ('event' in kl or 'tag' in kl):
                            outs.add(v)
                nodes.append((t, {x.lower() for x in ins}, {x.lower() for x in outs}))
        rel = set()
        for o in m['game_objectives'] + cores:
            for k in ('tag', 'event', 'destroyed_event', 'constructed_event', 'physical_actors_tag',
                      'damage_event', 'critical_volume_tag'):
                if o.get(k):
                    rel.add(str(o[k]).lower())
        for _ in range(8):
            grown = False
            for t, ins, outs in nodes:
                if t.get('objective_relevant'):
                    continue
                if (ins & rel) or (outs & rel):
                    t['objective_relevant'] = True
                    rel |= ins | outs
                    grown = True
            if not grown:
                break
        by_event = {}
        for t, ins, outs in nodes:
            t.setdefault('objective_relevant', False)
            for e in outs:
                by_event.setdefault(e, []).append(t['actor'])
        for t, ins, outs in nodes:
            t['triggered_by'] = sorted({a for i in ins for a in by_event.get(i, []) if a != t['actor']})

    def _volume(self, o, rec):
        p = o.props
        d = self._base(rec, physics=o.is_a('PhysicsVolume'),
                       water=bool(o.get('bWaterVolume', False)),
                       pain_causing=bool(o.get('bPainCausing', False)),
                       damage_per_sec=o.get('DamagePerSec', 0.0),
                       kill_volume=o.is_a('xFallingVolume') or 'kill' in o.class_name.lower())
        bref = p.get('Brush')
        if bref:
            try:
                mo = bref.load()
                rr = mo.native_reader()
                rr.box()
                rr.sphere()
                nvec = rr.index()
                rr.skip(12 * nvec)
                npt = rr.index()
                pts = np.frombuffer(rr.bytes(12 * npt), '<f4').reshape(npt, 3).astype(np.float64)
                if npt:
                    pre = np.array(p.get('PrePivot', (0.0, 0.0, 0.0)))
                    ms = p.get('MainScale', {})
                    ps = p.get('PostScale', {})
                    msv = np.array(ms.get('Scale', (1.0, 1.0, 1.0))) if isinstance(ms, dict) else 1.0
                    psv = np.array(ps.get('Scale', (1.0, 1.0, 1.0))) if isinstance(ps, dict) else 1.0
                    R = _rot_matrix(p.get('Rotation', (0, 0, 0)))
                    w = ((pts - pre) * msv) @ R * psv + np.array(p.get('Location', (0.0, 0.0, 0.0)))
                    w *= self.s
                    d['bounds_world'] = {'min': [round(x, 3) for x in w.min(0)],
                                         'max': [round(x, 3) for x in w.max(0)]}
                    d['brush_point_count'] = int(npt)
            except Exception as e:
                d['bounds_error'] = str(e)
        return d

    def _terrain(self, o, rec):
        p = o.props
        hm = p.get('TerrainMap')
        layers = []
        for k, v in sorted(p.items()):
            if k.split('[')[0] == 'Layers' and isinstance(v, dict) and v.get('Texture'):
                layers.append({
                    'texture': self.ex.canon(v['Texture']),
                    'alpha_map': self.ex.canon(v['AlphaMap']) if v.get('AlphaMap') else None,
                    'u_scale': v.get('UScale', 1.0), 'v_scale': v.get('VScale', 1.0)})
        if hm:
            self.dep(hm)
        sc = o.get('TerrainScale', (64.0, 64.0, 64.0))
        return self._base(
            rec, heightmap=self.ex.canon(hm) if hm else None,
            heightmap_file=self.ex.file_for(self.ex.canon(hm), 'textures') if hm else None,
            terrain_scale=self.sv(sc), terrain_scale_raw=list(sc), layers=layers,
            note='UE2 terrain: vertex(x,y) = location + ((x - W/2) * sx, (y - H/2) * sy, '
                 '(h - 32768) * sz / 256); heightmap is a 16-bit PNG')

    # -- level info / summary -----------------------------------------------------------
    def level_info(self, pkg, objs):
        summary = {}
        si = pkg.exports_of_class('LevelSummary')
        if si:
            sp = pkg.get_object(si[0]).props
            summary = {'title': sp.get('Title', ''), 'author': sp.get('Author', ''),
                       'description': sp.get('Description', ''),
                       'ideal_player_count': [sp.get('IdealPlayerCountMin'), sp.get('IdealPlayerCountMax')],
                       'screenshot': self.ex.canon(sp['Screenshot']) if sp.get('Screenshot') else None}
        li = None
        for o in objs:
            if o.is_a('LevelInfo'):
                li = o
                break
        level = None
        kill_z = None
        if li is not None:
            p = li.props
            level = {'title': p.get('Title', ''), 'author': p.get('Author', ''),
                     'song': p.get('Song'), 'default_game_type': p.get('DefaultGameType'),
                     'kill_z_raw': p.get('KillZ'), 'soft_kill_z': bool(li.get('bSoftKillZ', False)),
                     'stall_z_raw': p.get('StallZ'),
                     'default_gravity': li.get('DefaultGravity'),
                     'distance_fog': bool(li.get('bDistanceFog', False)),
                     'fog_color': li.get('DistanceFogColor'),
                     'fog_start': li.get('DistanceFogStart'), 'fog_end': li.get('DistanceFogEnd')}
            if not summary:
                summary = {'title': level['title'], 'author': level['author']}
            if 'KillZ' in p:
                kill_z = {'z': self.sv([0, 0, p['KillZ']])[2], 'z_raw': p['KillZ'], 'source': li.name}
        if kill_z is None:
            zs = [(o.props['KillZ'], o.name) for o in objs
                  if o.is_a('ZoneInfo') and 'KillZ' in o.props]
            if zs:
                z, n = min(zs)
                kill_z = {'z': self.sv([0, 0, z])[2], 'z_raw': z, 'source': n}
        return {'summary': summary, 'level_info': level, 'kill_z': kill_z}

    # -- BSP ---------------------------------------------------------------------------------
    def export_bsp(self, pkg, model_idx, out_dir):
        mo = pkg.get_object(model_idx - 1)
        model = bsp.read_model(mo)

        def surf_mat(idx):
            if idx == 0:
                return 'None'
            ref = ObjRef(pkg, idx)
            self.dep(ref)
            return self.ex.canon(ref)

        prims, skipped = bsp.triangulate(model, surf_mat, self.tex_size, self.s)
        gb = GltfBuilder(self.s)
        gb.add_primitive_list('BSP_' + pkg.name, prims)
        path = os.path.join(out_dir, 'bsp.gltf')
        gb.write(path, {'source': mo.path, 'map': pkg.name})
        tris = int(sum(len(p['indices']) for p in prims) // 3)
        verts = int(sum(len(p['positions']) for p in prims))
        return {
            'file': 'bsp.gltf', 'model': mo.path, 'triangles': tris, 'vertices': verts,
            'primitives': len(prims), 'nodes': int(len(model['node_vpool'])),
            'surfaces': len(model['surfs']), 'skipped_nodes': skipped,
            'materials': sorted({p['material'] for p in prims}),
            'uv_note': 'u = dot(P - pBase, TextureU) / USize, v = dot(P - pBase, TextureV) / VSize '
                       '(UE2 surfaces carry no PanU/PanV; panning is baked into pBase)',
        }


def ex_safe(name):
    from .exporter import safe
    return safe(name)
