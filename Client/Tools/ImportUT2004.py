"""
OpenUT2k4 importer for UT2004 placeholder content (Unreal Editor Python).

Turns the intermediate files written by Tools/ut2k4_export.py (ImportCache/, local only) into
editor assets and playable levels. Everything it writes is local placeholder content:

  /Game/Imported/Textures/<Pkg>/<Name>          textures (PNG import)
  /Game/Imported/Materials/Masters/M_Imported*  master materials (texture parameter "Base")
  /Game/Imported/Materials/<Pkg>/<Name>         one material instance per used material / texture
  /Game/Imported/StaticMeshes/<Pkg>/<Name>      static meshes (glTF via Interchange)
  /Game/Imported/StaticMeshes/<Map>/BSP_<Map>_NN  map BSP, chunked, complex-as-simple collision
  /Game/Imported/Maps/<Map>                     level (BSP, mesh actors, lights, sky, post process)
  /Game/Imported/SkeletalMeshes/<Pkg>/<Name>    skeletal meshes (+ <Name>_Skeleton, shared per animation set)
                                                and vertex-animated meshes (one joint per vertex), with sockets
  /Game/Imported/Animations/<Pkg>/<Set>_<Seq>   AnimSequences (UE2 MeshAnimation sequences / VertMesh sequences)
  /Game/Imported/Sounds/<Pkg>/<Name>            SoundWaves (every exported sound, not only map-referenced ones)
  /Game/Imported/Music/<Name>                   SoundWaves from Music/*.ogg
  Content/ImportedData/<Map>.json               gameplay markers read by AArenaLevelBuilder (+ "raw_markers":
                                                every marker list of map.json)
  Content/ImportedData/Assets/<Kind>.json       UT2004 path -> asset manifests (SkeletalMeshes, Animations,
                                                Sounds, Music); a subfolder, so it is not listed as a map

Replacement override: before an imported asset /Game/Imported/<Kind>/<Pkg>/<Name> is used, the
script checks /Game/Replacements/<Kind>/<Pkg>/<Name> and uses that asset instead (Kind is
Textures, Materials, StaticMeshes, SkeletalMeshes, Animations, Sounds or Music). Re-run the import
to pick up new replacements.

Skeletal glTF files are converted like static meshes, plus joint translations ((x, y, z) -> (x, z, y) / 100),
rotations ((x, y, z, w) -> (-x, -z, -y, w)), inverse bind matrices (P M P) and animation keys. Interchange
replaces spaces in bone names with "-" (sockets are matched by normalized name).

Coordinates: the exported glTF files are in Unreal space (X forward, Y right, Z up, left-handed,
centimetres, UE winding). Interchange's glTF reader maps glTF (x, y, z) metres to Unreal
(x, z, y) * 100 and keeps the index order, so the files are rewritten as standard glTF with
(x, y, z) -> (x, z, y) / 100 (positions) and (x, z, y) (normals). That swap is a reflection, which
already turns the Unreal winding into glTF's counter-clockwise front faces: no index flip is needed
and the imported mesh matches the source exactly (verified on import: bounds and triangle order).

Usage (normally through Scripts/import-ut2004.sh):
  UnrealEditor-Cmd OpenUT2k4.uproject -run=pythonscript \
      -script="Tools/ImportUT2004.py DM-Rankin CTF-BridgeOfFate" -unattended -nop4 -nosplash -nullrhi
  Arguments: map names, or --all-exported; --force re-imports existing assets; --markers-only
  writes only Content/ImportedData/*.json (also works with a plain python3, outside the editor).
  --all-assets imports everything in ImportCache (--kinds textures,materials,staticmeshes,sounds,
  skeletal,music; --packages P..; --chunk I/N); --skip-built skips levels already built from the
  current map.json. --report (plain python3) prints counts, sizes and maps that are missing.
  --fix-materials rewrites the existing material instances in place (Base, parent master, parameters)
  from Tools/ut2k4/matresolve.py, for the list written by Tools/ut2k4_materials.py.
  Per-asset failures are appended to Saved/ImportUT2004/import_errors.log.
  The arguments can also be given in the OPENUT2K4_IMPORT_ARGS environment variable.
"""

import argparse
import array
import json
import math
import os
import re
import shlex
import sys
import time

try:
    import unreal
except ImportError:  # plain python: only --markers-only is available
    unreal = None

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE_DIR = os.path.join(PROJECT_DIR, "ImportCache")
DATA_DIR = os.path.join(PROJECT_DIR, "Content", "ImportedData")
WORK_DIR = os.path.join(PROJECT_DIR, "Intermediate", "ImportUT2004")

IMPORTED_ROOT = "/Game/Imported"
REPLACEMENT_ROOT = "/Game/Replacements"
MASTER_DIR = IMPORTED_ROOT + "/Materials/Masters"
MAPS_DIR = IMPORTED_ROOT + "/Maps"
SKELETAL_DIR = IMPORTED_ROOT + "/SkeletalMeshes"
ANIM_DIR = IMPORTED_ROOT + "/Animations"
MUSIC_DIR = IMPORTED_ROOT + "/Music"
MANIFEST_DIR = os.path.join(DATA_DIR, "Assets")      # subfolder: not listed as a map
LOG_DIR = os.path.join(PROJECT_DIR, "Saved", "ImportUT2004")
STAMP_DIR = os.path.join(WORK_DIR, "stamps")
ASSET_KINDS = ("textures", "materials", "staticmeshes", "sounds", "skeletal", "music")

BSP_CHUNK_SIZE = 6000.0      # cm, XY grid used to split the BSP into separate meshes
BSP_CHUNK_MIN_TRIS = 4000    # maps below this triangle count keep a single BSP mesh
LIGHT_INTENSITY_SCALE = 1.0  # global multiplier on converted light intensities
LIGHT_FALLOFF_EXPONENT = 2.0
EXPOSURE_BIAS = 0.0

_errors = []
_warnings = []


def log(msg):
    if unreal:
        unreal.log("[ImportUT2004] " + msg)
    else:
        print("[ImportUT2004] " + msg, flush=True)


def warn(msg):
    _warnings.append(msg)
    if unreal:
        unreal.log_warning("[ImportUT2004] " + msg)
    else:
        print("[ImportUT2004] warning: " + msg, flush=True)


def _log_failure(msg):
    """Per-asset failures are appended to Saved/ImportUT2004/import_errors.log as they happen."""
    try:
        os.makedirs(LOG_DIR, exist_ok=True)
        with open(os.path.join(LOG_DIR, "import_errors.log"), "a", encoding="utf-8") as f:
            f.write("%s\t%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except OSError:
        pass


def error(msg):
    _errors.append(msg)
    _log_failure(msg)
    if unreal:
        unreal.log_error("[ImportUT2004] " + msg)
    else:
        print("[ImportUT2004] error: " + msg, file=sys.stderr, flush=True)


# =============================================================================
# Names and cache paths
# =============================================================================

_SAFE = re.compile(r"[^A-Za-z0-9._\-]")
_ASSET = re.compile(r"[^A-Za-z0-9_\-]")


def cache_safe(name):
    """Same sanitising as Tools/ut2k4/exporter.py safe()."""
    return _SAFE.sub("_", name)


def split_object_path(path):
    """'Pkg.Group.Name' -> ('Pkg', 'Group.Name')."""
    if "." in path:
        pkg, rest = path.split(".", 1)
    else:
        pkg, rest = path, path
    return pkg, rest


def asset_name(rest):
    return _ASSET.sub("_", rest)


def asset_folder(pkg):
    return _ASSET.sub("_", pkg)


def imported_path(kind, obj_path, prefix=""):
    pkg, rest = split_object_path(obj_path)
    return "%s/%s/%s/%s%s" % (IMPORTED_ROOT, kind, asset_folder(pkg), prefix, asset_name(rest))


def replacement_path(imported):
    return REPLACEMENT_ROOT + imported[len(IMPORTED_ROOT):]


def cache_file(kind, obj_path, ext):
    pkg, rest = split_object_path(obj_path)
    return os.path.join(CACHE_DIR, kind, cache_safe(pkg), cache_safe(rest) + ext)


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


# =============================================================================
# Gameplay markers -> Content/ImportedData/<Map>.json (no editor needed)
# =============================================================================

WEAPON_MAP = {
    "shockrifle": "PulseRifle",
    "linkgun": "ArcLinker",
    "sniperrifle": "RailRifle",
    "classicsniperrifle": "RailRifle",
    "rocketlauncher": "RocketLauncher",
    "flakcannon": "ShardCannon",
    "minigun": "RotaryGun",
    "biorifle": "GooLauncher",
    "assaultrifle": "Carbine",
    "redeemer": "RocketLauncher",
    "painter": "RocketLauncher",
}

# '<prefix>AmmoPickup' -> UT2004 weapon class
AMMO_PREFIX = {
    "shock": "shockrifle",
    "link": "linkgun",
    "sniper": "sniperrifle",
    "classicsniper": "classicsniperrifle",
    "rocket": "rocketlauncher",
    "flak": "flakcannon",
    "minigun": "minigun",
    "bio": "biorifle",
    "assault": "assaultrifle",
    "redeemer": "redeemer",
}

PICKUP_MAP = {
    "minihealthpack": "HealthVial",
    "healthpack": "HealthPack",
    "healthcharger": "HealthPack",
    "superhealthpack": "SuperHealth",
    "superhealthcharger": "SuperHealth",
    "shieldpack": "ShieldSmall",
    "shieldcharger": "ShieldSmall",
    "supershieldpack": "ShieldLarge",
    "supershieldcharger": "ShieldLarge",
    "udamagepack": "DoubleDamage",
    "udamagecharger": "DoubleDamage",
    "adrenalinepickup": "Adrenaline",
}


def map_weapon(cls):
    """UT2004 weapon class name -> our weapon name (None if unknown)."""
    if not cls:
        return None
    c = cls.lower()
    if c in WEAPON_MAP:
        return WEAPON_MAP[c]
    if c.startswith("ons") or "painter" in c or "redeemer" in c:
        return "RocketLauncher"
    return None


def map_ammo(cls):
    c = (cls or "").lower()
    if not c.endswith("ammopickup"):
        return None
    prefix = c[:-len("ammopickup")]
    if prefix in AMMO_PREFIX:
        return map_weapon(AMMO_PREFIX[prefix])
    if prefix.startswith("ons"):
        return "RocketLauncher"
    return None


def map_pickup(cls):
    """UT2004 pickup / pickup base / powerup class -> (EArenaPickupType name, weapon name or None)."""
    if not cls:
        return None, None
    c = cls.lower()
    if c in PICKUP_MAP:
        return PICKUP_MAP[c], None
    w = map_ammo(cls)
    if w:
        return "Ammo", w
    if c.endswith("pickup"):
        w = map_weapon(cls[:-len("pickup")])
        if w:
            return "Weapon", w
    return None, None


def _r(v, nd=1):
    return [round(float(x), nd) for x in v]


_MUSIC_MANIFEST = None


def map_music(level_info):
    """LevelInfo.Song -> (song name, imported SoundWave path or None). Same rules as Tools/ut2k4_map_music.py:
    "None" / "" -> no song, ".ogg" stripped, matched against Assets/Music.json case-insensitively and then
    ignoring punctuation ("KR-SniperTime" -> "KR-Sniper-Time")."""
    global _MUSIC_MANIFEST
    song = level_info.get("song")
    song = song.strip() if isinstance(song, str) else ""
    if song.lower().endswith(".ogg"):
        song = song[:-4]
    if not song or song.lower() == "none":
        return None, None
    if _MUSIC_MANIFEST is None:
        try:
            with open(os.path.join(MANIFEST_DIR, "Music.json"), "r", encoding="utf-8") as f:
                _MUSIC_MANIFEST = json.load(f).get("assets", {})
        except (OSError, ValueError):
            _MUSIC_MANIFEST = {}
    key = lambda n: re.sub(r"[^a-z0-9]", "", n.lower())
    hit = next((v for k, v in _MUSIC_MANIFEST.items() if k.lower() == song.lower()), None) or \
        next((v for k, v in _MUSIC_MANIFEST.items() if key(k) == key(song)), None)
    return song, (hit.get("imported") or hit.get("asset")) if hit else None


def slim_markers(doc):
    """The subset of map.json that AArenaLevelBuilder uses at runtime."""
    m = doc.get("markers", {})
    summary = doc.get("summary") or {}
    level_info = doc.get("level_info") or {}
    name = doc["map"]
    prefix = (doc.get("game_type_prefix") or name.split("-", 1)[0]).upper()

    starts_raw = m.get("player_starts", [])
    teams_present = {s.get("team", 0) for s in starts_raw if s.get("enabled", True)}
    team_starts = 0 in teams_present and 1 in teams_present
    starts = []
    for s in starts_raw:
        if not s.get("enabled", True):
            continue
        starts.append({"location": _r(s["location"]), "yaw": round(s["rotation"]["yaw"], 2),
                       "team": int(s.get("team", 0)) if team_starts else -1})

    pickups = []
    skipped = {}

    def skip(what):
        skipped[what] = skipped.get(what, 0) + 1

    for wb in m.get("weapon_bases", []):
        cls = (wb.get("weapon_class") or {}).get("class")
        w = map_weapon(cls)
        if w:
            pickups.append({"type": "Weapon", "weapon": w, "location": _r(wb["location"]), "source": wb["class"]})
        else:
            skip(cls or wb["class"])
    for lk in m.get("weapon_lockers", []):
        ws = [map_weapon((x.get("weapon_class") or {}).get("class")) for x in lk.get("weapons", [])]
        ws = [w for w in ws if w]
        loc = lk["location"]
        yaw = math.radians(lk["rotation"]["yaw"])
        spread = 70.0
        n = len(ws)
        for i, w in enumerate(ws):
            # spread along the locker's right axis, centred on it
            off = (i - (n - 1) * 0.5) * spread
            p = [loc[0] - math.sin(yaw) * off, loc[1] + math.cos(yaw) * off, loc[2]]
            pickups.append({"type": "Weapon", "weapon": w, "location": _r(p), "source": lk["class"]})
    for pb in m.get("pickup_bases", []):
        classes = []
        if pb.get("powerup"):
            classes.append(pb["powerup"]["class"])
        classes += [c["class"] for c in pb.get("pickup_classes") or [] if c]
        classes.append(pb["class"])
        t = w = None
        for c in classes:
            t, w = map_pickup(c)
            if t:
                break
        if t:
            pickups.append({"type": t, "weapon": w, "location": _r(pb["location"]), "source": pb["class"]})
        else:
            skip(pb["class"])
    for p in m.get("pickups", []):
        t, w = map_pickup(p["class"])
        if t:
            pickups.append({"type": t, "weapon": w, "location": _r(p["location"]), "source": p["class"]})
        else:
            skip(p["class"])

    pads = []
    for jp in m.get("jump_pads", []):
        if jp.get("jump_target_location"):
            pads.append({"location": _r(jp["location"]), "target": _r(jp["jump_target_location"]), "source": jp["class"]})

    # Lifts (movers with a LiftCenter) cannot move here: they become jump pads to the upper exit.
    lifts = m.get("lifts", [])
    for mv in m.get("movers", []):
        keys = mv.get("key_pos") or []   # KeyPos[1..]; KeyPos[0] is the base position
        if not keys:
            continue
        top = max(keys, key=lambda k: k[2])
        if top[2] < 120.0:
            continue
        actor = next((a for a in doc.get("actors", []) if a["index"] == mv["actor_index"]), None)
        tag = str((actor or {}).get("tag"))
        centers = [l for l in lifts if l["class"] == "LiftCenter" and str(l.get("lift_tag")) == tag]
        if not centers:
            continue
        base = mv["location"]
        center = min(centers, key=lambda l: (l["location"][0] - base[0]) ** 2 + (l["location"][1] - base[1]) ** 2)
        top_z = base[2] + top[2]
        exits = [l for l in lifts if l["class"] == "LiftExit" and str(l.get("lift_tag")) == tag
                 and l["location"][2] > base[2] + top[2] * 0.5]
        if not exits:
            exits = [l for l in lifts if l["class"] == "LiftExit" and abs(l["location"][2] - top_z) < 400.0]
        if not exits:
            continue
        c = center["location"]
        ex = min(exits, key=lambda l: (l["location"][0] - c[0]) ** 2 + (l["location"][1] - c[1]) ** 2)
        pads.append({"location": _r(c), "target": _r(ex["location"]), "source": "Lift:" + mv["actor"]})

    flags = []
    for fb in m.get("flag_bases", []):
        if fb.get("team") in (0, 1):
            flags.append({"team": int(fb["team"]), "location": _r(fb["location"])})

    # Playable bounds from navigation-relevant markers (keeps sky boxes out of the bot graph).
    pts = [s["location"] for s in starts] + [p["location"] for p in pickups]
    for k in ("path_nodes", "inventory_spots", "jump_spots", "other_nav_points", "lifts"):
        pts += [x["location"] for x in m.get(k, [])]
    for p in pads:
        pts += [p["location"], p["target"]]
    pts += [f["location"] for f in flags]
    bounds = None
    if pts:
        bounds = {"min": _r([min(p[i] for p in pts) for i in range(3)]),
                  "max": _r([max(p[i] for p in pts) for i in range(3)])}

    kill_z = (doc.get("kill_z") or {}).get("z")
    ideal = summary.get("ideal_player_count") or [None, None]
    title = summary.get("title") or level_info.get("title") or name
    music, music_asset = map_music(level_info)
    out = {
        "schema": "openut2k4-imported-map",
        "schema_version": 1,
        "map": name,
        "level": "%s/%s" % (MAPS_DIR, name),
        "title": title,
        "author": summary.get("author") or level_info.get("author") or "",
        "description": summary.get("description") or "",
        "game_type_prefix": prefix,
        "recommended_players": [x for x in ideal if isinstance(x, (int, float))],
        "kill_z": kill_z,
        "play_bounds": bounds,
        "player_starts": starts,
        "pickups": pickups,
        "jump_pads": pads,
        "flag_bases": flags,
        # LevelInfo.Song and its imported SoundWave (the game checks /Game/Replacements/<same path> first)
        "music": music,
        "music_asset": music_asset,
        "skipped": skipped,
        "note": "Local placeholder data converted from the user's own UT2004 install. Not for distribution.",
        # every marker list of the exporter's map.json (schema 2), unmodified: positions in project cm,
        # rotations in degrees; see Tools/ut2k4/level.py for the kinds and their fields
        "raw_markers_schema_version": doc.get("schema_version"),
        "raw_markers": m,
    }
    return out


def write_markers(doc):
    os.makedirs(DATA_DIR, exist_ok=True)
    data = slim_markers(doc)
    path = os.path.join(DATA_DIR, doc["map"] + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    log("%s: wrote %s (%d starts, %d pickups, %d jump pads, %d flags%s)" % (
        doc["map"], os.path.relpath(path, PROJECT_DIR), len(data["player_starts"]), len(data["pickups"]),
        len(data["jump_pads"]), len(data["flag_bases"]),
        (", skipped %s" % data["skipped"]) if data["skipped"] else ""))
    return data


# =============================================================================
# glTF (Unreal space) -> standard glTF conversion
# =============================================================================

_CTYPE = {5123: "H", 5125: "I", 5126: "f", 5121: "B"}
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def read_gltf(path):
    doc = load_json(path)
    with open(os.path.join(os.path.dirname(path), doc["buffers"][0]["uri"]), "rb") as f:
        data = f.read()
    return doc, data


def read_accessor(doc, data, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    n = acc["count"] * _NCOMP[acc["type"]]
    a = array.array(_CTYPE[acc["componentType"]])
    off = view["byteOffset"] + acc.get("byteOffset", 0)
    a.frombytes(data[off:off + n * a.itemsize])
    if sys.byteorder != "little":
        a.byteswap()
    return a


def read_primitives(path):
    """-> (doc, [dict(pos, nrm, uv, idx, material(name), extras)]) in the file's Unreal space."""
    doc, data = read_gltf(path)
    prims = []
    for mesh in doc["meshes"]:
        for p in mesh["primitives"]:
            attrs = p["attributes"]
            prims.append({
                "pos": read_accessor(doc, data, attrs["POSITION"]),
                "nrm": read_accessor(doc, data, attrs["NORMAL"]) if "NORMAL" in attrs else None,
                "uv": read_accessor(doc, data, attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else None,
                "idx": read_accessor(doc, data, p["indices"]),
                "material": doc["materials"][p["material"]]["name"] if "material" in p else "None",
                "extras": p.get("extras") or {},
            })
    return doc, prims


def write_standard_gltf(path, name, prims, slot_names):
    """prims: [dict(pos, nrm, uv, idx, slot)] in Unreal space -> glTF 2.0 (Y up, metres)."""
    bin_data = bytearray()
    views, accessors, out_prims = [], [], []

    def add(arr, ctype, atype, target, minmax=None):
        while len(bin_data) % 4:
            bin_data.append(0)
        off = len(bin_data)
        b = arr.tobytes()
        bin_data.extend(b)
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(b), "target": target})
        acc = {"bufferView": len(views) - 1, "componentType": ctype,
               "count": len(arr) // _NCOMP[atype], "type": atype}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    def finite(a):
        """Some source meshes carry NaN normals/UVs: zero them (Interchange would, noisily)."""
        if a is not None and any(v != v or v in (float("inf"), float("-inf")) for v in a):
            return array.array("f", [0.0 if (v != v or v in (float("inf"), float("-inf"))) else v for v in a])
        return a

    for p in prims:
        p = dict(p, pos=finite(p["pos"]), nrm=finite(p.get("nrm")), uv=finite(p.get("uv")))
        src = p["pos"]
        n = len(src) // 3
        if n == 0 or len(p["idx"]) == 0:
            continue
        pos = array.array("f", bytes(len(src) * 4))
        xs = src[0::3]
        ys = src[1::3]
        zs = src[2::3]
        pos[0::3] = array.array("f", [v * 0.01 for v in xs])
        pos[1::3] = array.array("f", [v * 0.01 for v in zs])
        pos[2::3] = array.array("f", [v * 0.01 for v in ys])
        mm = ([min(pos[0::3]), min(pos[1::3]), min(pos[2::3])], [max(pos[0::3]), max(pos[1::3]), max(pos[2::3])])
        attrs = {"POSITION": add(pos, 5126, "VEC3", 34962, mm)}
        if p.get("nrm") is not None:
            sn = p["nrm"]
            nrm = array.array("f", sn)
            nrm[1::3] = sn[2::3]
            nrm[2::3] = sn[1::3]
            attrs["NORMAL"] = add(nrm, 5126, "VEC3", 34962)
        if p.get("uv") is not None:
            attrs["TEXCOORD_0"] = add(array.array("f", p["uv"]), 5126, "VEC2", 34962)
        idx = array.array("I", p["idx"])
        out_prims.append({"attributes": attrs, "indices": add(idx, 5125, "SCALAR", 34963),
                          "mode": 4, "material": p["slot"]})
    if not out_prims:
        return False
    bin_name = os.path.splitext(os.path.basename(path))[0] + ".bin"
    doc = {
        "asset": {"version": "2.0", "generator": "OpenUT2k4 ImportUT2004.py",
                  "extras": {"note": "converted from Unreal space: (x, y, z) cm -> (x, z, y) m"}},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": out_prims}],
        "materials": [{"name": s, "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
                      for s in slot_names],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"uri": bin_name, "byteLength": len(bin_data)}],
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(os.path.join(os.path.dirname(path), bin_name), "wb") as f:
        f.write(bin_data)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return True


def _qconv(q):
    """Unreal-space quaternion (x, y, z, w) -> glTF space under the (x, y, z) -> (x, z, y) swap."""
    return (-q[0], -q[2], -q[1], q[3])


def convert_skinned_gltf(src, dst, slot_of_material=None):
    """Skinned/animated glTF written by the exporter (Unreal space, cm) -> standard glTF
    (Y up, metres) with the same axis conversion as write_standard_gltf. Joint translations,
    inverse bind matrices and animation keys are converted too; materials are renamed to slot
    names (slot_of_material: cache material name -> slot name). Returns the converted doc."""
    doc, data = read_gltf(src)
    buf = bytearray(data)
    acc_kind = {}

    def mark(ai, kind):
        if ai is not None:
            acc_kind[ai] = kind

    for mesh in doc.get("meshes", []):
        for p in mesh["primitives"]:
            a = p["attributes"]
            mark(a.get("POSITION"), "pos")
            mark(a.get("NORMAL"), "dir")
    for sk in doc.get("skins", []):
        mark(sk.get("inverseBindMatrices"), "mat4")
    for an in doc.get("animations", []):
        for ch in an["channels"]:
            out = an["samplers"][ch["sampler"]]["output"]
            path = ch["target"]["path"]
            mark(out, {"translation": "pos", "rotation": "quat", "scale": "scl"}.get(path, "other"))
    for ai, kind in acc_kind.items():
        acc = doc["accessors"][ai]
        view = doc["bufferViews"][acc["bufferView"]]
        n = acc["count"] * {"VEC3": 3, "VEC4": 4, "MAT4": 16}.get(acc["type"], 1)
        off = view["byteOffset"] + acc.get("byteOffset", 0)
        a = array.array("f")
        a.frombytes(bytes(buf[off:off + n * 4]))
        if sys.byteorder != "little":
            a.byteswap()
        if kind == "pos":
            x, y, z = a[0::3], a[1::3], a[2::3]
            a[0::3] = array.array("f", [v * 0.01 for v in x])
            a[1::3] = array.array("f", [v * 0.01 for v in z])
            a[2::3] = array.array("f", [v * 0.01 for v in y])
            if "min" in acc:
                acc["min"] = [min(a[0::3]), min(a[1::3]), min(a[2::3])]
                acc["max"] = [max(a[0::3]), max(a[1::3]), max(a[2::3])]
        elif kind == "dir" or kind == "scl":
            y = a[1::3]
            a[1::3] = a[2::3]
            a[2::3] = y
        elif kind == "quat":
            x, y, z = a[0::4], a[1::4], a[2::4]
            a[0::4] = array.array("f", [-v for v in x])
            a[1::4] = array.array("f", [-v for v in z])
            a[2::4] = array.array("f", [-v for v in y])
        elif kind == "mat4":
            perm = (0, 2, 1)
            for k in range(acc["count"]):
                m = a[k * 16:(k + 1) * 16]        # column-major
                o = [0.0] * 16
                for c in range(3):
                    for r in range(3):
                        o[c * 4 + r] = m[perm[c] * 4 + perm[r]]
                for r in range(3):
                    o[12 + r] = m[12 + perm[r]] * 0.01
                o[15] = 1.0
                a[k * 16:(k + 1) * 16] = array.array("f", o)
        if sys.byteorder != "little":
            a.byteswap()
        buf[off:off + n * 4] = a.tobytes()
    for node in doc.get("nodes", []):
        t = node.get("translation")
        if t:
            node["translation"] = [t[0] * 0.01, t[2] * 0.01, t[1] * 0.01]
        q = node.get("rotation")
        if q:
            node["rotation"] = list(_qconv(q))
        s = node.get("scale")
        if s:
            node["scale"] = [s[0], s[2], s[1]]
    if slot_of_material is not None:
        for m in doc.get("materials", []):
            m["name"] = slot_of_material.get(m["name"], m["name"])
    for m in doc.get("materials", []):
        m.pop("extras", None)
    doc["asset"] = {"version": "2.0", "generator": "OpenUT2k4 ImportUT2004.py",
                    "extras": {"note": "converted from Unreal space: (x, y, z) cm -> (x, z, y) m"}}
    bin_name = os.path.splitext(os.path.basename(dst))[0] + ".bin"
    doc["buffers"] = [{"uri": bin_name, "byteLength": len(buf)}]
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(os.path.join(os.path.dirname(dst), bin_name), "wb") as f:
        f.write(buf)
    with open(dst, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return doc


def subset_primitive(p, tri_ids):
    """New primitive holding only the given triangles (vertices compacted)."""
    remap = {}
    pos, nrm, uv, idx = array.array("f"), array.array("f"), array.array("f"), array.array("I")
    src_idx, src_pos, src_nrm, src_uv = p["idx"], p["pos"], p["nrm"], p["uv"]
    for t in tri_ids:
        for k in range(3):
            v = src_idx[t * 3 + k]
            nv = remap.get(v)
            if nv is None:
                nv = len(remap)
                remap[v] = nv
                pos.extend(src_pos[v * 3:v * 3 + 3])
                if src_nrm is not None:
                    nrm.extend(src_nrm[v * 3:v * 3 + 3])
                if src_uv is not None:
                    uv.extend(src_uv[v * 2:v * 2 + 2])
            idx.append(nv)
    return {"pos": pos, "nrm": nrm if src_nrm is not None else None, "uv": uv if src_uv is not None else None,
            "idx": idx, "material": p["material"], "extras": p["extras"]}


# =============================================================================
# Material resolution (cache json -> master + parameters)
# =============================================================================

_MATRESOLVE = None
_RESOLVER = None


def material_resolver():
    """Shared resolver (Tools/ut2k4/matresolve.py, loaded by file: the ut2k4 package imports numpy,
    which the editor's Python may lack)."""
    global _MATRESOLVE, _RESOLVER
    if _RESOLVER is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "openut2k4_matresolve", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ut2k4", "matresolve.py"))
        _MATRESOLVE = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_MATRESOLVE)
        _RESOLVER = _MATRESOLVE.Resolver(_MATRESOLVE.Graph(CACHE_DIR))
    return _RESOLVER


class MaterialInfo:
    """Base texture, blend mode and instance parameters of a UT2004 material (or plain texture).
    Materials go through Tools/ut2k4/matresolve.py, which follows Shader / Combiner / Modifier chains
    to the colour texture (not a mask, lightmap, detail or env map) and collects the UV transform of
    TexScaler / TexPanner / fixed TexRotator, the Combiner Multiply tint, self-illumination and
    specular settings (see its docstring)."""

    def __init__(self, path):
        self.path = path
        self.blend = "opaque"
        self.two_sided = False
        self.unlit = False
        self.texture = None        # UT2004 object path of the base texture
        self.texture_png = None    # cache file
        self.found = False
        self.params = {}           # matresolve Resolution.params(): uv, Tint, Emissive, EmissiveAlpha, ...
        self.rule = None
        self._resolve()

    def _resolve(self):
        if not self.path or self.path == "None":
            return
        mj = cache_file("materials", self.path, ".json")
        tj = cache_file("textures", self.path, ".json")
        if os.path.isfile(mj):
            r = material_resolver().resolve(self.path)
            self.found = True
            self.blend = r.blend or "opaque"
            self.two_sided = bool(r.two_sided)
            self.unlit = bool(r.unlit)
            self.rule = r.rule
            self.params = r.params()
            if r.base and r.base_png:
                self.texture = r.base
                self.texture_png = r.base_png
        elif os.path.isfile(tj):
            d = load_json(tj)
            self.found = True
            h = d.get("material_hints") or {}
            self.blend = h.get("blend_mode") or "opaque"
            if d.get("bMasked"):
                self.blend = "masked"
            self.two_sided = bool(h.get("two_sided") or d.get("bTwoSided"))
            self.texture = self.path
            self.texture_png = os.path.join(os.path.dirname(tj), d["png"])
        if self.texture_png and not os.path.isfile(self.texture_png):
            self.texture_png = None
            self.texture = None


def material_variant_key(path, two_sided=False, unlit=False):
    return (path or "None", bool(two_sided), bool(unlit))


# =============================================================================
# Editor side
# =============================================================================

if unreal:
    EAL = unreal.EditorAssetLibrary
    MEL = unreal.MaterialEditingLibrary
    TOOLS = unreal.AssetToolsHelpers.get_asset_tools()


def asset_exists(path):
    return EAL.does_asset_exist(path)


def resolve_asset(imported):
    """Replacement override: /Game/Replacements/... wins over /Game/Imported/... ."""
    rep = replacement_path(imported)
    if EAL.does_asset_exist(rep):
        return EAL.load_asset(rep), rep
    if EAL.does_asset_exist(imported):
        return EAL.load_asset(imported), imported
    return None, None


def ensure_dir(path):
    if not EAL.does_directory_exist(path):
        EAL.make_directory(path)


def make_pipeline(textures=False):
    """Interchange generic pipeline: no materials/textures from glTF, no Nanite, keep normals."""
    pipe = unreal.InterchangeGenericAssetsPipeline()
    mat = pipe.get_editor_property("material_pipeline")
    mat.set_editor_property("import_materials", False)
    tex = mat.get_editor_property("texture_pipeline")
    tex.set_editor_property("import_textures", textures)
    tex.set_editor_property("detect_normal_map_texture", False)
    for prop, value in (("allow_non_power_of_two", True),
                        ("import_udims", False)):     # names like Foo_1024 are not UDIM tiles
        try:
            tex.set_editor_property(prop, value)
        except Exception:
            pass
    mesh = pipe.get_editor_property("mesh_pipeline")
    mesh.set_editor_property("build_nanite", False)
    mesh.set_editor_property("collision", False)
    mesh.set_editor_property("import_skeletal_meshes", False)
    mesh.set_editor_property("generate_lightmap_u_vs", False)
    common = pipe.get_editor_property("common_meshes_properties")
    common.set_editor_property("recompute_normals", False)
    common.set_editor_property("recompute_tangents", True)
    common.set_editor_property("remove_degenerates", True)
    common.set_editor_property("force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_STATIC_MESH)
    ov = unreal.InterchangePipelineStackOverride()
    ov.add_pipeline(pipe)
    return ov


def run_import_tasks(items, textures):
    """items: [(file, destination_dir, destination_name)] -> {asset path: imported object or None}."""
    tasks = []
    opts = make_pipeline(textures=textures)
    for filename, dest_dir, dest_name in items:
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", filename)
        t.set_editor_property("destination_path", dest_dir)
        t.set_editor_property("destination_name", dest_name)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", False)
        t.set_editor_property("options", opts)
        tasks.append(t)
    out = {}
    batch = 64
    for i in range(0, len(tasks), batch):
        TOOLS.import_asset_tasks(tasks[i:i + batch])
    for (filename, dest_dir, dest_name), t in zip(items, tasks):
        path = "%s/%s" % (dest_dir, dest_name)
        got = [str(p).split(".")[0] for p in (t.get_editor_property("imported_object_paths") or [])]
        if not EAL.does_asset_exist(path) and len(got) == 1 and got[0] != path and EAL.does_asset_exist(got[0]):
            # Interchange sometimes names an asset after the source file (e.g. UDIM-like names)
            if EAL.rename_asset(got[0], path):
                log("renamed %s -> %s" % (got[0], path))
        out[path] = EAL.load_asset(path) if EAL.does_asset_exist(path) else None
        if out[path] is None:
            error("import failed: %s -> %s (imported %s)" % (filename, path, list(t.get_editor_property("imported_object_paths") or [])))
    return out


_UDIM = re.compile(r"[._]1\d{3}$")


def non_udim_file(png):
    """Interchange treats source files named like Foo_1024.png as UDIM tiles (and renames the asset):
    import such files from a copy whose name does not end in a 1001-1999 number."""
    stem = os.path.splitext(os.path.basename(png))[0]
    if not _UDIM.search(stem):
        return png
    import shutil
    dst = os.path.join(WORK_DIR, "textures_non_udim", stem + "_tex.png")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.isfile(dst) or os.path.getmtime(dst) < os.path.getmtime(png):
        shutil.copyfile(png, dst)
    return dst


# ---- textures -----------------------------------------------------------------

class Importer:
    def __init__(self, force=False):
        self.force = force
        self.textures = {}      # UT texture path -> UTexture
        self.materials = {}     # variant key -> UMaterialInterface
        self.meshes = {}        # UT mesh path -> (UStaticMesh, section->slot name list)
        self.masters = {}
        self.stats = {"textures": 0, "materials": 0, "meshes": 0}
        self.new_textures = []  # UT texture paths imported by this run

    # ---- textures
    def import_textures(self, infos):
        todo = []
        for info in infos:
            if not info.texture or info.texture in self.textures:
                continue
            dst = imported_path("Textures", info.texture)
            rep = replacement_path(dst)
            if EAL.does_asset_exist(rep):
                self.textures[info.texture] = EAL.load_asset(rep)
                continue
            if EAL.does_asset_exist(dst) and not self.force:
                self.textures[info.texture] = EAL.load_asset(dst)
                continue
            todo.append((info.texture, info.texture_png, dst))
            self.textures[info.texture] = None
        if not todo:
            return
        log("importing %d textures" % len(todo))
        t0 = time.time()
        items = []
        for _tex, png, dst in todo:
            d, n = dst.rsplit("/", 1)
            items.append((non_udim_file(png), d, n))
        res = run_import_tasks(items, textures=True)
        for tex, _png, dst in todo:
            obj = res.get(dst)
            if isinstance(obj, unreal.Texture2D):
                obj.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
                obj.set_editor_property("srgb", True)
                EAL.save_loaded_asset(obj, only_if_is_dirty=False)
                self.textures[tex] = obj
                self.new_textures.append(tex)
                self.stats["textures"] += 1
        log("textures done in %.1fs" % (time.time() - t0))

    # ---- master materials
    def build_masters(self):
        ensure_dir(MASTER_DIR)
        specs = [
            ("M_ImportedOpaque", unreal.BlendMode.BLEND_OPAQUE, False),
            ("M_ImportedMasked", unreal.BlendMode.BLEND_MASKED, False),
            ("M_ImportedTranslucent", unreal.BlendMode.BLEND_TRANSLUCENT, False),
            ("M_ImportedAdditive", unreal.BlendMode.BLEND_ADDITIVE, True),
            ("M_ImportedModulate", unreal.BlendMode.BLEND_MODULATE, True),
        ]
        for name, blend, unlit in specs:
            path = "%s/%s" % (MASTER_DIR, name)
            current = EAL.does_asset_exist(path) and \
                EAL.get_metadata_tag(EAL.load_asset(path), MASTER_VERSION_TAG) == str(MASTER_VERSION)
            if current and not self.force:
                self.masters[name] = EAL.load_asset(path)
                # masters from older imports may lack newer usage flags (e.g. skeletal mesh)
                _set_usages(self.masters[name])
                EAL.save_loaded_asset(self.masters[name], only_if_is_dirty=True)
                continue
            self.masters[name] = build_master(name, blend, unlit)

    def master_for(self, info):
        return self.masters[{
            "masked": "M_ImportedMasked",
            "translucent": "M_ImportedTranslucent",
            "additive": "M_ImportedAdditive",
            "modulate": "M_ImportedModulate",
            "invisible": "M_ImportedMasked",
        }.get(info.blend, "M_ImportedOpaque")]

    # ---- material instances
    def material(self, key):
        """key = (UT material path, force two sided, force unlit) -> UMaterialInterface."""
        if key in self.materials:
            return self.materials[key]
        path, two_sided, unlit = key
        info = MaterialInfo(path)
        suffix = ("_2S" if two_sided and not info.two_sided else "") + ("_U" if unlit and not info.unlit else "")
        # sections without a material share one default instance (an asset may not be called "None")
        mi_path = (imported_path("Materials", path) if path != "None" else MASTER_DIR + "/MI_ImportedDefault") + suffix
        rep = replacement_path(imported_path("Materials", path)) if path != "None" else None
        if rep and EAL.does_asset_exist(rep):
            self.materials[key] = EAL.load_asset(rep)
            return self.materials[key]
        if EAL.does_asset_exist(mi_path) and not self.force:
            mi = EAL.load_asset(mi_path)
            # keep Base and parameters in sync with the resolver and /Game/Replacements/Textures
            if isinstance(mi, unreal.MaterialInstanceConstant):
                changed = self.apply_material(mi, info, two_sided, unlit)
                if changed:
                    MEL.update_material_instance(mi)
                    EAL.save_loaded_asset(mi, only_if_is_dirty=False)
                    log("%s: updated %s" % (mi_path, ", ".join(changed)))
            self.materials[key] = mi
            return mi
        if not info.found and path != "None":
            warn("material %s not in ImportCache, using a plain default" % path)
        folder, name = mi_path.rsplit("/", 1)
        ensure_dir(folder)
        mi = EAL.load_asset(mi_path) if EAL.does_asset_exist(mi_path) else None
        if mi is None or not isinstance(mi, unreal.MaterialInstanceConstant):
            if mi is not None:
                EAL.delete_asset(mi_path)
            mi = TOOLS.create_asset(name, folder, unreal.MaterialInstanceConstant,
                                    unreal.MaterialInstanceConstantFactoryNew())
        self.apply_material(mi, info, two_sided, unlit)
        MEL.update_material_instance(mi)
        EAL.save_loaded_asset(mi, only_if_is_dirty=False)
        self.materials[key] = mi
        self.stats["materials"] += 1
        return mi

    def instance_values(self, info, two_sided=False, unlit=False):
        """Parent master, Base texture and parameter values an instance of `info` should have."""
        p = info.params or {}
        uv = p.get("uv") or [1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0]
        lit = info.blend in ("opaque", "masked", "translucent")
        emissive = 1.0 if (info.unlit or unlit) and lit else float(p.get("Emissive", 0.0))
        tint = p.get("Tint") or [1.0, 1.0, 1.0]
        scalars = {"Opacity": 0.0 if info.blend == "invisible" else 1.0}
        if lit:
            scalars.update({"Emissive": emissive, "EmissiveAlpha": float(p.get("EmissiveAlpha", 0.0)),
                            "Roughness": float(p.get("Roughness", 0.8)), "Specular": float(p.get("Specular", 0.3))})
        vectors = {"UVTransform": unreal.LinearColor(uv[0], uv[1], uv[2], uv[3]),
                   "UVOffsetPan": unreal.LinearColor(uv[4], uv[5], uv[6], uv[7]),
                   "Tint": unreal.LinearColor(tint[0], tint[1], tint[2], 1.0)}
        tex = self.textures.get(info.texture) if info.texture else None
        return self.master_for(info), tex, scalars, vectors, bool(info.two_sided or two_sided)

    def apply_material(self, mi, info, two_sided=False, unlit=False):
        """Set parent, Base and parameters on `mi` where they differ; returns the names changed.
        Values equal to the master defaults are cleared rather than stored."""
        master, tex, scalars, vectors, sided = self.instance_values(info, two_sided, unlit)
        changed = []
        parent = mi.get_editor_property("parent")
        if parent is None or parent.get_path_name() != master.get_path_name():
            MEL.set_material_instance_parent(mi, master)
            changed.append("parent")
        if tex is not None:
            cur = MEL.get_material_instance_texture_parameter_value(mi, "Base")
            if cur is None or cur.get_path_name() != tex.get_path_name():
                MEL.set_material_instance_texture_parameter_value(mi, "Base", tex)
                changed.append("Base")
        defaults_s = {"Opacity": 1.0, "Emissive": 0.0, "EmissiveAlpha": 0.0, "Roughness": 0.8, "Specular": 0.3}
        have_s = {str(p.get_editor_property("parameter_info").get_editor_property("name")): p
                  for p in mi.get_editor_property("scalar_parameter_values")}
        for name, value in scalars.items():
            cur = have_s.get(name)
            cur_v = cur.get_editor_property("parameter_value") if cur is not None else defaults_s[name]
            if abs(cur_v - value) > 1e-5:
                MEL.set_material_instance_scalar_parameter_value(mi, name, value)
                changed.append(name)
        have_v = {str(p.get_editor_property("parameter_info").get_editor_property("name")): p
                  for p in mi.get_editor_property("vector_parameter_values")}
        defaults_v = {"UVTransform": (1.0, 0.0, 0.0, 1.0), "UVOffsetPan": (0.0, 0.0, 0.0, 0.0),
                      "Tint": (1.0, 1.0, 1.0, 1.0)}
        for name, value in vectors.items():
            cur = have_v.get(name)
            if cur is not None:
                c = cur.get_editor_property("parameter_value")
                cur_t = (c.r, c.g, c.b, c.a)
            else:
                cur_t = defaults_v[name]
            want = (value.r, value.g, value.b, value.a)
            if any(abs(a - b) > 1e-5 for a, b in zip(cur_t, want)):
                MEL.set_material_instance_vector_parameter_value(mi, name, value)
                changed.append(name)
        ov = mi.get_editor_property("base_property_overrides")
        if sided and not (ov.get_editor_property("override_two_sided") and ov.get_editor_property("two_sided")):
            ov.set_editor_property("override_two_sided", True)
            ov.set_editor_property("two_sided", True)
            mi.set_editor_property("base_property_overrides", ov)
            changed.append("two_sided")
        return changed

    # ---- static meshes
    def mesh_slots(self, gltf_path, meta):
        """Slots of an imported mesh: one per (material, collision) pair, in first-use order.
        Returns (slot names, slot -> (material key, collides), section -> slot name)."""
        doc = load_json(gltf_path)
        slots, slot_info, section_slot = [], {}, {}
        for prim in doc["meshes"][0]["primitives"]:
            ex = prim.get("extras") or {}
            mat = doc["materials"][prim["material"]]["name"] if "material" in prim else "None"
            col = bool(ex.get("enable_collision", True))
            key = (mat, col)
            if key not in slot_info:
                slot_info[key] = "S%d" % len(slots)
                slots.append(key)
            section_slot.setdefault(ex.get("section", 0), slot_info[key])
        names = [slot_info[k] for k in slots]
        return names, {slot_info[k]: k for k in slots}, section_slot

    def import_meshes(self, mesh_paths):
        todo = []
        for path in sorted(set(mesh_paths)):
            if path in self.meshes:
                continue
            gltf = cache_file("staticmeshes", path, ".gltf")
            if not os.path.isfile(gltf):
                warn("static mesh %s not in ImportCache" % path)
                self.meshes[path] = (None, {})
                continue
            meta_path = os.path.splitext(gltf)[0] + ".json"
            meta = load_json(meta_path) if os.path.isfile(meta_path) else {}
            names, slot_map, section_slot = self.mesh_slots(gltf, meta)
            dst = imported_path("StaticMeshes", path)
            rep = replacement_path(dst)
            if EAL.does_asset_exist(rep):
                self.meshes[path] = (EAL.load_asset(rep), {})
                continue
            if EAL.does_asset_exist(dst) and not self.force:
                self.meshes[path] = (EAL.load_asset(dst), section_slot)
                continue
            _doc, prims = read_primitives(gltf)
            out_prims = []
            for p in prims:
                key = (p["material"], bool(p["extras"].get("enable_collision", True)))
                p["slot"] = names.index([n for n, k in slot_map.items() if k == key][0])
                out_prims.append(p)
            conv = os.path.join(WORK_DIR, "staticmeshes", asset_folder(split_object_path(path)[0]),
                                asset_name(split_object_path(path)[1]) + ".gltf")
            if not write_standard_gltf(conv, asset_name(split_object_path(path)[1]), out_prims, names):
                warn("static mesh %s has no triangles" % path)
                self.meshes[path] = (None, {})
                continue
            todo.append((path, conv, dst, slot_map, section_slot))
        if not todo:
            return
        log("importing %d static meshes" % len(todo))
        t0 = time.time()
        res = run_import_tasks([(c, d.rsplit("/", 1)[0], d.rsplit("/", 1)[1]) for _p, c, d, _s, _x in todo], textures=False)
        for path, _conv, dst, slot_map, section_slot in todo:
            sm = res.get(dst)
            if not isinstance(sm, unreal.StaticMesh):
                self.meshes[path] = (None, {})
                continue
            self.setup_mesh(sm, {s: (material_variant_key(k[0]), k[1]) for s, k in slot_map.items()})
            self.meshes[path] = (sm, section_slot)
            self.stats["meshes"] += 1
        log("static meshes done in %.1fs" % (time.time() - t0))

    def setup_mesh(self, sm, slot_map):
        """Assign material instances by slot, set complex-as-simple collision on colliding sections."""
        any_collision = False
        mats = sm.get_editor_property("static_materials")
        slot_index = {}
        for i, m in enumerate(mats):
            slot = str(m.get_editor_property("material_slot_name"))
            slot_index[slot] = i
            info = slot_map.get(slot)
            if info is None:
                warn("%s: unexpected material slot %s" % (sm.get_path_name(), slot))
                continue
            sm.set_material(i, self.material(info[0]))
            any_collision = any_collision or info[1]
        body = sm.get_editor_property("body_setup")
        if body is not None:
            body.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE
                                     if any_collision else unreal.CollisionTraceFlag.CTF_USE_DEFAULT)
            # UE2 collides against solid space, not one-sided polygons: double-sided queries keep traces
            # that start behind a surface (e.g. above a ceiling) from skipping the rest of the mesh.
            body.set_editor_property("double_sided_geometry", True)
        # per-section collision (sections map to slots through their material index)
        sms = section_api()
        for sec in range(sm.get_num_sections(0)):
            try:
                mi = sms.get_lod_material_slot(sm, 0, sec) if hasattr(sms, "get_lod_material_slot") else sec
            except Exception:
                mi = sec
            slot = str(mats[mi].get_editor_property("material_slot_name")) if 0 <= mi < len(mats) else None
            col = slot_map.get(slot, (None, True))[1]
            try:
                sms.enable_section_collision(sm, bool(col), 0, sec)
            except Exception as ex:
                warn("%s: could not set section collision: %s" % (sm.get_path_name(), ex))
                break
        try:
            sm.set_editor_property("light_map_resolution", 4)
        except Exception:
            pass
        enable_nanite(sm)
        EAL.save_loaded_asset(sm, only_if_is_dirty=False)

    # ---- BSP
    def import_bsp(self, map_name, map_dir, doc):
        bsp_file = os.path.join(map_dir, "bsp.gltf")
        if not os.path.isfile(bsp_file):
            warn("%s: no bsp.gltf" % map_name)
            return []
        _d, prims = read_primitives(bsp_file)
        for p in prims:
            # material slot per (material, two sided, unlit, collides)
            flags = set(p["extras"].get("poly_flag_names") or [])
            p["key"] = (p["material"], "TwoSided" in flags, "Unlit" in flags, "NotSolid" not in flags)
        return self.import_world_meshes(map_name, "BSP", prims)

    def import_terrains(self, map_name, doc):
        prims = []
        for t in doc["markers"].get("terrains", []):
            actor = next((a for a in doc["actors"] if a["index"] == t["actor_index"]), None)
            try:
                p = terrain_primitive(t, actor)
            except Exception as ex:
                warn("%s: terrain %s skipped: %s" % (map_name, t["actor"], ex))
                continue
            if p:
                prims.append(p)
        if not prims:
            return []
        return self.import_world_meshes(map_name, "Terrain", prims)

    def import_world_meshes(self, map_name, kind, prims):
        """World-space geometry (BSP or terrain) -> chunked static meshes <kind>_<Map>_NN with complex
        collision. prims carry "key" = (material path, two sided, unlit, collides)."""
        folder = "%s/StaticMeshes/%s" % (IMPORTED_ROOT, asset_folder(map_name))
        total = sum(len(p["idx"]) // 3 for p in prims)
        chunks = {}
        if total < BSP_CHUNK_MIN_TRIS:
            for p in prims:
                chunks.setdefault((0, 0), []).append((p, list(range(len(p["idx"]) // 3))))
        else:
            for p in prims:
                pos, idx = p["pos"], p["idx"]
                cells = {}
                for t in range(len(idx) // 3):
                    a, b, c = idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2]
                    cx = (pos[a * 3] + pos[b * 3] + pos[c * 3]) / 3.0
                    cy = (pos[a * 3 + 1] + pos[b * 3 + 1] + pos[c * 3 + 1]) / 3.0
                    cells.setdefault((int(math.floor(cx / BSP_CHUNK_SIZE)), int(math.floor(cy / BSP_CHUNK_SIZE))), []).append(t)
                for cell, tris in cells.items():
                    chunks.setdefault(cell, []).append((p, tris))
        # merge tiny chunks into their largest neighbour to keep the actor count low
        sizes = {c: sum(len(t) for _p, t in v) for c, v in chunks.items()}
        for c in sorted(chunks, key=lambda k: sizes[k]):
            if sizes.get(c, 0) >= 200 or len(chunks) <= 1:
                continue
            nbrs = [(c[0] + dx, c[1] + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if (dx or dy)]
            nbrs = [n for n in nbrs if n in chunks]
            if not nbrs:
                continue
            tgt = max(nbrs, key=lambda n: sizes[n])
            chunks[tgt].extend(chunks.pop(c))
            sizes[tgt] += sizes.pop(c)

        results = []
        todo = []
        for ci, cell in enumerate(sorted(chunks)):
            name = "%s_%s_%02d" % (kind, asset_name(map_name), ci)
            dst = "%s/%s" % (folder, name)
            slots, out_prims = [], []
            for p, tris in chunks[cell]:
                key = p["key"]
                if key not in slots:
                    slots.append(key)
                sub = subset_primitive(p, tris) if len(tris) != len(p["idx"]) // 3 else dict(p)
                sub["slot"] = slots.index(key)
                out_prims.append(sub)
            names = ["S%d" % i for i in range(len(slots))]
            slot_map = {names[i]: (material_variant_key(k[0], k[1], k[2]), k[3]) for i, k in enumerate(slots)}
            rep = replacement_path(dst)
            if EAL.does_asset_exist(rep):
                results.append(EAL.load_asset(rep))
                continue
            if EAL.does_asset_exist(dst) and not self.force:
                results.append(EAL.load_asset(dst))
                continue
            conv = os.path.join(WORK_DIR, "maps", asset_folder(map_name), name + ".gltf")
            if write_standard_gltf(conv, name, out_prims, names):
                todo.append((conv, dst, slot_map))
        # drop stale chunks from an earlier import with a different chunk count
        for stale in range(len(chunks), 1000):
            sp = "%s/%s_%s_%02d" % (folder, kind, asset_name(map_name), stale)
            if not EAL.does_asset_exist(sp):
                break
            EAL.delete_asset(sp)
        if todo:
            log("%s: importing %d %s chunk(s), %d triangles" % (map_name, len(todo), kind, total))
            res = run_import_tasks([(c, d.rsplit("/", 1)[0], d.rsplit("/", 1)[1]) for c, d, _s in todo], textures=False)
            for conv, dst, slot_map in todo:
                sm = res.get(dst)
                if isinstance(sm, unreal.StaticMesh):
                    self.setup_mesh(sm, slot_map)
                    results.append(sm)
        return results


# =============================================================================
# All-assets mode: every texture / material / mesh / sound / skeletal asset / music in ImportCache
# =============================================================================

def cache_entries(kind, packages=None):
    """[(package folder, json path, meta)] for one ImportCache kind, sorted."""
    root = os.path.join(CACHE_DIR, kind)
    out = []
    if not os.path.isdir(root):
        return out
    want = {p.lower() for p in packages} if packages else None
    for pkg in sorted(os.listdir(root)):
        d = os.path.join(root, pkg)
        if not os.path.isdir(d) or (want is not None and pkg.lower() not in want):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".json"):
                continue
            jp = os.path.join(d, fn)
            try:
                meta = load_json(jp)
            except (OSError, ValueError) as ex:
                error("cache file %s unreadable: %s" % (jp, ex))
                continue
            out.append((pkg, jp, meta))
    return out


def cache_packages(kinds):
    names = set()
    for k in kinds:
        root = os.path.join(CACHE_DIR, k)
        if os.path.isdir(root):
            names.update(n for n in os.listdir(root) if os.path.isdir(os.path.join(root, n)))
    return sorted(names, key=str.lower)


class TexRef:
    """Minimal MaterialInfo stand-in for import_textures()."""

    def __init__(self, path, png):
        self.texture = path
        self.texture_png = png


def _norm(name):
    return re.sub(r"[^a-z0-9]", "", str(name).lower())


def update_manifest(kind, entries):
    """Content/ImportedData/Assets/<kind>.json: UT2004 object path -> imported asset info.
    Merged across runs (chunked imports)."""
    if not entries:
        return
    os.makedirs(MANIFEST_DIR, exist_ok=True)
    path = os.path.join(MANIFEST_DIR, kind + ".json")
    doc = {"schema": "openut2k4-imported-assets", "kind": kind, "assets": {}}
    if os.path.isfile(path):
        try:
            doc = load_json(path)
        except (OSError, ValueError):
            pass
    doc.setdefault("assets", {}).update(entries)
    doc["note"] = ("Local placeholder assets converted from the user's own UT2004 install. 'asset' is the "
                   "path to use (a /Game/Replacements override when one exists), 'imported' the generated one.")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, sort_keys=True)


def resolved_path(imported):
    rep = replacement_path(imported)
    return rep if EAL.does_asset_exist(rep) else imported


def stamp_ok(name, source_files):
    """True when a stamp written after a successful import is newer than all source files."""
    sp = os.path.join(STAMP_DIR, name + ".stamp")
    try:
        t = os.path.getmtime(sp)
        return all(os.path.getmtime(f) <= t for f in source_files if f)
    except OSError:
        return False


def write_stamp(name, data=None):
    os.makedirs(STAMP_DIR, exist_ok=True)
    with open(os.path.join(STAMP_DIR, name + ".stamp"), "w", encoding="utf-8") as f:
        json.dump(data or {}, f)


def run_tasks(items, options):
    """items: [(file, destination_dir, destination_name)] -> list of AssetImportTask (executed)."""
    tasks = []
    for filename, dest_dir, dest_name in items:
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", filename)
        t.set_editor_property("destination_path", dest_dir)
        t.set_editor_property("destination_name", dest_name)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", True)
        if options is not None:
            t.set_editor_property("options", options)
        tasks.append(t)
    for i in range(0, len(tasks), 32):
        TOOLS.import_asset_tasks(tasks[i:i + 32])
    return tasks


def make_skeletal_pipeline(only_anims=False, skeleton=None, with_anims=False):
    """Interchange pipeline for the exporter's skinned glTF files: skeletal mesh (+ skeleton)
    or animations only onto an existing skeleton. No materials/textures/physics/Nanite."""
    pipe = unreal.InterchangeGenericAssetsPipeline()
    mat = pipe.get_editor_property("material_pipeline")
    mat.set_editor_property("import_materials", False)
    mat.get_editor_property("texture_pipeline").set_editor_property("import_textures", False)
    mesh = pipe.get_editor_property("mesh_pipeline")
    mesh.set_editor_property("build_nanite", False)
    mesh.set_editor_property("import_static_meshes", False)
    mesh.set_editor_property("import_skeletal_meshes", not only_anims)
    mesh.set_editor_property("create_physics_asset", False)
    mesh.set_editor_property("import_morph_targets", False)
    common = pipe.get_editor_property("common_meshes_properties")
    common.set_editor_property("recompute_normals", True)
    common.set_editor_property("recompute_tangents", True)
    common.set_editor_property("force_all_mesh_as_type", unreal.InterchangeForceMeshType.IFMT_SKELETAL_MESH)
    common.set_editor_property("import_lods", False)
    sk = pipe.get_editor_property("common_skeletal_meshes_and_animations_properties")
    sk.set_editor_property("import_only_animations", only_anims)
    if skeleton is not None:
        sk.set_editor_property("skeleton", skeleton)
    anim = pipe.get_editor_property("animation_pipeline")
    anim.set_editor_property("import_animations", bool(only_anims or with_anims))
    anim.set_editor_property("import_bone_tracks", True)
    ov = unreal.InterchangePipelineStackOverride()
    ov.add_pipeline(pipe)
    return ov


def mesh_material_slots(meta):
    """Unique material names of a skeletal/vertex mesh json in first-use order -> slot names."""
    order = []
    for sec in meta.get("sections") or []:
        m = sec.get("material") or "None"
        if m not in order:
            order.append(m)
    return {m: "S%d" % i for i, m in enumerate(order)}


class SkeletalImporter:
    """SkeletalMesh / MeshAnimation / VertMesh import (see the module docstring for paths)."""

    def __init__(self, imp, force=False):
        self.imp = imp
        self.force = force
        self.meshes = {}        # UT mesh path -> (asset path or None, skeleton asset path or None)
        self.manifest_meshes = {}
        self.manifest_anims = {}
        self.stats = {"skeletal_meshes": 0, "vertex_meshes": 0, "animation_sets": 0, "animations": 0,
                      "skipped_existing": 0}

    # ---- helpers
    def _materials_for(self, metas):
        """Import the textures and material instances that the meshes' sections use."""
        keys = set()
        for meta in metas:
            for m in mesh_material_slots(meta):
                keys.add(material_variant_key(m))
        infos = [MaterialInfo(k[0]) for k in sorted(keys)]
        self.imp.import_textures(infos)
        for k in sorted(keys):
            self.imp.material(k)

    def _assign_materials(self, sk, meta):
        slots = mesh_material_slots(meta)
        by_slot = {v: k for k, v in slots.items()}
        mats = sk.get_editor_property("materials")
        changed = False
        for i, m in enumerate(mats):
            slot = str(m.get_editor_property("material_slot_name"))
            src = by_slot.get(slot)
            if src is None:
                continue
            mi = self.imp.material(material_variant_key(src))
            if mi is not None:
                m.set_editor_property("material_interface", mi)
                mats[i] = m
                changed = True
        if changed:
            sk.set_editor_property("materials", mats)

    def _add_sockets(self, sk, meta):
        """Create/update the mesh's sockets from the json; returns True when something changed."""
        sockets = meta.get("sockets") or []
        changed = False
        if not sockets:
            return changed
        skel = sk.get_editor_property("skeleton")
        names = {}
        try:
            ref = unreal.AnimPoseExtensions.get_reference_pose(skel)
            for n in unreal.AnimPoseExtensions.get_bone_names(ref):
                names.setdefault(_norm(n), str(n))
        except Exception as ex:
            warn("%s: bone names unavailable for sockets: %s" % (sk.get_path_name(), ex))
        for so in sockets:
            bone = names.get(_norm(so.get("bone"))) or names.get(_norm(so.get("bone_original")))
            if bone is None:
                warn("%s: socket %s: bone %s not found" % (sk.get_path_name(), so["name"], so.get("bone")))
                continue
            try:
                # SocketName/BoneName are read-only from Python: add, then rename and re-parent
                sock = sk.find_socket(so["name"])
                if sock is None:
                    sock = unreal.SkeletalMeshSocket(sk)
                    sk.add_socket(sock, False)
                    sk.rename_socket(sock.get_editor_property("socket_name"), so["name"])
                    changed = True
                loc = unreal.Vector(*so["location"])
                q = so["rotation_quat"]
                rot = unreal.Quat(q[0], q[1], q[2], q[3]).rotator()
                cur_loc = sock.get_editor_property("relative_location")
                cur_rot = sock.get_editor_property("relative_rotation")
                if str(sock.get_editor_property("bone_name")) != bone:
                    sock.set_socket_parent(sk, bone)
                    changed = True
                diff = max(abs(cur_loc.x - loc.x), abs(cur_loc.y - loc.y), abs(cur_loc.z - loc.z),
                           abs(cur_rot.pitch - rot.pitch), abs(cur_rot.yaw - rot.yaw), abs(cur_rot.roll - rot.roll))
                if diff > 1e-3:
                    sock.set_editor_property("relative_location", loc)
                    sock.set_editor_property("relative_rotation", rot)
                    changed = True
            except Exception as ex:
                warn("%s: socket %s: %s" % (sk.get_path_name(), so["name"], ex))
        return changed

    def _convert(self, gltf, kind, path, slots):
        pkg, rest = split_object_path(path)
        conv = os.path.join(WORK_DIR, kind, asset_folder(pkg), asset_name(rest) + ".gltf")
        convert_skinned_gltf(gltf, conv, slots)
        return conv

    # ---- meshes
    def import_mesh(self, meta, json_path, skeleton=None, kind="skeletalmeshes"):
        """Import one skeletal/vertex mesh; returns (asset path, skeleton asset) or (None, None)."""
        path = meta["path"]
        dst = imported_path("SkeletalMeshes", path)
        rep = replacement_path(dst)
        if EAL.does_asset_exist(rep):
            sk = EAL.load_asset(rep)
            self.meshes[path] = (rep, sk.get_editor_property("skeleton") if sk else None)
            return self.meshes[path]
        gltf = os.path.join(os.path.dirname(json_path), meta["gltf"])
        if EAL.does_asset_exist(dst) and not self.force and stamp_ok("mesh_" + asset_name(path), [gltf, json_path]):
            sk = EAL.load_asset(dst)
            skel = sk.get_editor_property("skeleton") if sk else None
            if sk is not None and (skeleton is None or skel == skeleton):
                if self._add_sockets(sk, meta):
                    EAL.save_loaded_asset(sk, only_if_is_dirty=False)
                self.meshes[path] = (dst, skel)
                self.stats["skipped_existing"] += 1
                self._manifest_mesh(meta, dst, skel)
                return self.meshes[path]
        slots = mesh_material_slots(meta)
        self._materials_for([meta])
        conv = self._convert(gltf, kind, path, slots)
        folder, name = dst.rsplit("/", 1)
        ensure_dir(folder)
        t0 = time.time()
        tasks = run_tasks([(conv, folder, name)], make_skeletal_pipeline(skeleton=skeleton))
        sk = EAL.load_asset(dst) if EAL.does_asset_exist(dst) else None
        if not isinstance(sk, unreal.SkeletalMesh):
            error("skeletal mesh import failed: %s -> %s (got %s)" % (
                path, dst, list(tasks[0].get_editor_property("imported_object_paths") or [])))
            self.meshes[path] = (None, None)
            return self.meshes[path]
        skel = sk.get_editor_property("skeleton")
        if skeleton is not None and skel != skeleton:
            warn("%s: could not share skeleton %s (got %s)" % (dst, skeleton.get_path_name(),
                                                               skel.get_path_name() if skel else None))
        self._assign_materials(sk, meta)
        self._add_sockets(sk, meta)
        EAL.save_loaded_asset(sk, only_if_is_dirty=False)
        if skel is not None:
            EAL.save_loaded_asset(skel, only_if_is_dirty=False)
        write_stamp("mesh_" + asset_name(path))
        self.stats["vertex_meshes" if kind == "vertmeshes" else "skeletal_meshes"] += 1
        log("skeletal mesh %s -> %s (%.1fs)" % (path, dst, time.time() - t0))
        self.meshes[path] = (dst, skel)
        self._manifest_mesh(meta, dst, skel)
        return self.meshes[path]

    def _manifest_mesh(self, meta, dst, skel):
        self.manifest_meshes[meta["path"]] = {
            "asset": resolved_path(dst), "imported": dst,
            "skeleton": skel.get_path_name().split(".")[0] if skel else None,
            "type": meta.get("type"), "animation": meta.get("animation"),
            "sockets": [s["name"] for s in meta.get("sockets") or []],
            "bounds": meta.get("bounds"), "materials": meta.get("materials"),
        }

    # ---- animations
    def import_anims(self, meta, json_path, skeleton, set_name=None):
        """Every sequence of an animation glTF -> /Game/Imported/Animations/<Pkg>/<Set>_<Seq>."""
        path = meta["path"]
        pkg, rest = split_object_path(path)
        set_name = set_name or asset_name(rest)
        folder = "%s/%s" % (ANIM_DIR, asset_folder(pkg))
        gltf = os.path.join(os.path.dirname(json_path), meta["gltf"])
        seqs = meta.get("sequences") or []
        expected = {s["name"]: "%s/%s_%s" % (folder, set_name, asset_name(s["name"])) for s in seqs}
        stamp = "anim_%s_%s" % (asset_folder(pkg), set_name)
        if not self.force and stamp_ok(stamp, [gltf, json_path]) and all(EAL.does_asset_exist(p) for p in expected.values()):
            self.stats["skipped_existing"] += 1
            self._manifest_anim(meta, expected, skeleton, set_name)
            return expected
        ensure_dir(folder)
        conv = self._convert(gltf, "animations", "%s.%s" % (pkg, set_name), None)
        t0 = time.time()
        # Interchange names the assets <DestName><AnimationName>, except when the file holds a single
        # animation: then the asset is just <DestName>
        dest = set_name + "_" + (asset_name(seqs[0]["name"]) if len(seqs) == 1 else "")
        stale = "%s/%s_" % (folder, set_name)     # left by older imports of single-sequence sets
        if len(seqs) == 1 and EAL.does_asset_exist(stale):
            EAL.delete_asset(stale)
        tasks = run_tasks([(conv, folder, dest)], make_skeletal_pipeline(only_anims=True, skeleton=skeleton))
        got = [str(p).split(".")[0] for p in (tasks[0].get_editor_property("imported_object_paths") or [])]
        by_norm = {_norm(g.rsplit("/", 1)[1]): g for g in got}
        if len(seqs) == 1 and len(got) == 1:
            by_norm.setdefault(_norm(set_name + "_" + seqs[0]["name"]), got[0])
        result = {}
        for s in seqs:
            want = expected[s["name"]]
            hit = want if EAL.does_asset_exist(want) else by_norm.get(_norm(set_name + "_" + s["name"]))
            if hit is None:
                error("animation %s.%s not imported (set %s)" % (path, s["name"], folder))
                continue
            result[s["name"]] = hit
        if result:
            write_stamp(stamp, {"assets": result})
        self.stats["animation_sets"] += 1
        self.stats["animations"] += len(result)
        log("animations %s: %d/%d sequences -> %s (%.1fs)" % (path, len(result), len(seqs), folder, time.time() - t0))
        self._manifest_anim(meta, result, skeleton, set_name)
        return result

    def _manifest_anim(self, meta, assets, skeleton, set_name):
        seqs = {}
        for s in meta.get("sequences") or []:
            a = assets.get(s["name"])
            if not a:
                continue
            seqs[s["name"]] = {"asset": resolved_path(a), "imported": a, "num_frames": s.get("num_frames"),
                               "rate": s.get("rate"), "groups": s.get("groups"),
                               "notifies": s.get("notifies")}
        self.manifest_anims[meta["path"]] = {
            "skeleton": skeleton.get_path_name().split(".")[0] if skeleton else None,
            "representative_mesh": meta.get("representative_mesh"), "set_name": set_name,
            "type": meta.get("type"), "sequences": seqs}

    # ---- driver
    def run(self, packages=None):
        meshes = {m["path"]: (jp, m) for _p, jp, m in cache_entries("skeletalmeshes", packages)
                  if m.get("type") == "SkeletalMesh"}
        anims = {m["path"]: (jp, m) for _p, jp, m in cache_entries("animations", packages)
                 if m.get("type") == "MeshAnimation"}
        verts = [(jp, m) for _p, jp, m in cache_entries("vertmeshes", packages) if m.get("type") == "VertMesh"]
        # meshes outside the selected packages that an animation needs as its skeleton source
        all_meshes = None
        log("skeletal: %d meshes, %d animation sets, %d vertex meshes" % (len(meshes), len(anims), len(verts)))
        done = set()
        for apath, (ajp, ameta) in sorted(anims.items()):
            rep = ameta.get("representative_mesh")
            if not rep or not ameta.get("gltf"):
                warn("animation %s has no skeletal mesh (%s); skipped" % (apath, ameta.get("error")))
                continue
            if rep not in meshes:
                if all_meshes is None:
                    all_meshes = {m["path"]: (jp, m) for _p, jp, m in cache_entries("skeletalmeshes")}
                if rep not in all_meshes:
                    error("animation %s: representative mesh %s not in ImportCache" % (apath, rep))
                    continue
                meshes[rep] = all_meshes[rep]
            try:
                rjp, rmeta = meshes[rep]
                _dst, skel = self.import_mesh(rmeta, rjp)
                done.add(rep)
                if skel is None:
                    error("animation %s: no skeleton from %s" % (apath, rep))
                    continue
                for mpath, (mjp, mmeta) in sorted(meshes.items()):
                    if mpath in done or (mmeta.get("animation") or "").lower() != apath.lower():
                        continue
                    self.import_mesh(mmeta, mjp, skeleton=skel)
                    done.add(mpath)
                self.import_anims(ameta, ajp, skel)
            except Exception as ex:
                error("animation set %s: %s" % (apath, ex))
        for mpath, (mjp, mmeta) in sorted(meshes.items()):
            if mpath in done:
                continue
            try:
                self.import_mesh(mmeta, mjp)
            except Exception as ex:
                error("skeletal mesh %s: %s" % (mpath, ex))
        for jp, meta in verts:
            try:
                _dst, skel = self.import_mesh(meta, jp, kind="vertmeshes")
                if skel is not None and meta.get("sequences"):
                    pkg, rest = split_object_path(meta["path"])
                    self.import_anims(dict(meta, gltf=meta["gltf"]), jp, skel, set_name=asset_name(rest))
            except Exception as ex:
                error("vertex mesh %s: %s" % (meta["path"], ex))
        update_manifest("SkeletalMeshes", self.manifest_meshes)
        update_manifest("Animations", self.manifest_anims)
        log("skeletal done: %s" % self.stats)


def import_sounds(imp, packages=None, force=False):
    """Every exported Sound (wav/ogg) -> SoundWave /Game/Imported/Sounds/<Pkg>/<Group_Name>."""
    todo, manifest, skipped = [], {}, 0
    for _pkg, jp, meta in cache_entries("sounds", packages):
        if meta.get("type") != "Sound" or not meta.get("file"):
            continue
        src = os.path.join(os.path.dirname(jp), meta["file"])
        if not os.path.isfile(src):
            continue
        dst = imported_path("Sounds", meta["path"])
        rep = replacement_path(dst)
        if EAL.does_asset_exist(rep) or (EAL.does_asset_exist(dst) and not force):
            skipped += 1
        else:
            todo.append((src, dst, meta))
        manifest[meta["path"]] = {"asset": dst, "imported": dst, "duration": meta.get("duration"),
                                  "loop": meta.get("loop"), "container": meta.get("container")}
    log("sounds: %d to import, %d up to date" % (len(todo), skipped))
    t0 = time.time()
    for i in range(0, len(todo), 200):
        batch = todo[i:i + 200]
        for _s, dst, _m in batch:
            ensure_dir(dst.rsplit("/", 1)[0])
        tasks = run_tasks([(src, dst.rsplit("/", 1)[0], dst.rsplit("/", 1)[1]) for src, dst, _m in batch], None)
        for (src, dst, meta), t in zip(batch, tasks):
            a = EAL.load_asset(dst) if EAL.does_asset_exist(dst) else None
            if not isinstance(a, unreal.SoundWave):
                error("sound import failed: %s -> %s" % (src, dst))
                manifest.pop(meta["path"], None)
                continue
            if meta.get("loop"):
                a.set_editor_property("looping", True)
                EAL.save_loaded_asset(a, only_if_is_dirty=False)
        log("sounds %d/%d (%.0fs)" % (min(i + 200, len(todo)), len(todo), time.time() - t0))
    for k, v in manifest.items():
        v["asset"] = resolved_path(v["imported"])
    update_manifest("Sounds", manifest)
    return len(todo)


def import_music(force=False):
    """ImportCache/music/<Name>.ogg -> SoundWave /Game/Imported/Music/<Name>."""
    d = os.path.join(CACHE_DIR, "music")
    if not os.path.isdir(d):
        return 0
    ensure_dir(MUSIC_DIR)
    todo, manifest = [], {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".json"):
            continue
        meta = load_json(os.path.join(d, fn))
        src = os.path.join(d, meta["file"])
        dst = "%s/%s" % (MUSIC_DIR, asset_name(meta["name"]))
        if not (EAL.does_asset_exist(replacement_path(dst)) or (EAL.does_asset_exist(dst) and not force)):
            todo.append((src, dst, meta))
        manifest[meta["name"]] = {"asset": dst, "imported": dst, "duration": meta.get("duration"),
                                  "channels": meta.get("channels"), "sample_rate": meta.get("sample_rate"),
                                  "title": (meta.get("comments") or {}).get("TITLE"),
                                  "source_file": meta.get("source_file")}
    log("music: %d to import" % len(todo))
    tasks = run_tasks([(src, MUSIC_DIR, dst.rsplit("/", 1)[1]) for src, dst, _m in todo], None)
    for (src, dst, meta), _t in zip(todo, tasks):
        a = EAL.load_asset(dst) if EAL.does_asset_exist(dst) else None
        if not isinstance(a, unreal.SoundWave):
            error("music import failed: %s -> %s" % (src, dst))
            manifest.pop(meta["name"], None)
            continue
        a.set_editor_property("looping", True)
        EAL.save_loaded_asset(a, only_if_is_dirty=False)
    for k, v in manifest.items():
        v["asset"] = resolved_path(v["imported"])
    update_manifest("Music", manifest)
    return len(todo)


def import_all_assets(imp, kinds, packages=None, force=False):
    """--all-assets: everything in ImportCache of the given kinds (optionally some packages)."""
    t0 = time.time()
    if "textures" in kinds or "materials" in kinds:
        refs = []
        for _pkg, jp, meta in cache_entries("textures", packages):
            if meta.get("type") == "Texture" and meta.get("png"):
                refs.append(TexRef(meta["path"], os.path.join(os.path.dirname(jp), meta["png"])))
        log("textures: %d in cache" % len(refs))
        for i in range(0, len(refs), 500):
            imp.import_textures(refs[i:i + 500])
        # material instances made earlier while a texture was missing pick it up now
        for t in imp.new_textures:
            if EAL.does_asset_exist(imported_path("Materials", t)):
                imp.material(material_variant_key(t))
    if "materials" in kinds:
        mats = [m for _p, _jp, m in cache_entries("materials", packages) if m.get("type") == "Material"]
        infos = [MaterialInfo(m["path"]) for m in mats]
        imp.import_textures(infos)
        for m in mats:
            try:
                imp.material(material_variant_key(m["path"]))
            except Exception as ex:
                error("material %s: %s" % (m["path"], ex))
        log("materials: %d in cache" % len(mats))
    if "staticmeshes" in kinds:
        paths, keys = [], set()
        for _pkg, jp, meta in cache_entries("staticmeshes", packages):
            if meta.get("type") != "StaticMesh":
                continue
            paths.append(meta["path"])
            for sec in meta.get("sections") or []:
                keys.add(material_variant_key(sec.get("material") or "None"))
        imp.import_textures([MaterialInfo(k[0]) for k in sorted(keys)])
        for k in sorted(keys):
            imp.material(k)
        log("static meshes: %d in cache" % len(paths))
        for i in range(0, len(paths), 300):
            imp.import_meshes(paths[i:i + 300])
    if "sounds" in kinds:
        import_sounds(imp, packages, force)
    if "music" in kinds:
        import_music(force)
    if "skeletal" in kinds:
        SkeletalImporter(imp, force).run(packages)
    log("all-assets pass (%s) done in %.1fs" % (",".join(kinds), time.time() - t0))


# ---- --fix-materials ----------------------------------------------------------------

def nanite_recheck(sm):
    """Nanite on / off for a mesh after a slot's master changed (same rule as enable_nanite)."""
    compatible = True
    for slot in sm.get_editor_property("static_materials"):
        mat = slot.get_editor_property("material_interface")
        for _ in range(16):
            if not isinstance(mat, unreal.MaterialInstance) or mat.get_editor_property("parent") is None:
                break
            mat = mat.get_editor_property("parent")
        if mat is not None and mat.get_name() in NON_NANITE_MASTERS:
            compatible = False
    nanite = sm.get_editor_property("nanite_settings")
    if nanite.get_editor_property("enabled") == compatible:
        return False
    if compatible:
        enable_nanite(sm)
    else:
        nanite.set_editor_property("enabled", False)
        sm.set_editor_property("nanite_settings", nanite)
    return True


def fix_materials(imp, list_file=None):
    """--fix-materials: rewrite existing material instances in place (parent, Base, parameters) from the
    current resolver. The list comes from Tools/ut2k4_materials.py (Saved/ImportUT2004/material_fixes.json);
    without it every cached material is checked. Static meshes, skeletal meshes and levels reference the
    instances, so they pick the changes up without being imported again; meshes whose slot changed between
    a Nanite and a non-Nanite master are switched accordingly."""
    t0 = time.time()
    list_file = list_file or os.path.join(LOG_DIR, "material_fixes.json")
    if os.path.isfile(list_file):
        paths = [f["path"] for f in load_json(list_file).get("fixes") or []]
        log("fix-materials: %d materials listed in %s" % (len(paths), list_file))
    else:
        paths = [m["path"] for _p, _jp, m in cache_entries("materials") if m.get("type") == "Material"]
        log("fix-materials: no %s, checking all %d cached materials" % (list_file, len(paths)))
    work = []   # (info, mi path, two_sided, unlit)
    for path in paths:
        base = imported_path("Materials", path)
        if EAL.does_asset_exist(replacement_path(base)):
            continue
        info = None
        for suffix, two_sided, unlit in (("", False, False), ("_2S", True, False), ("_U", False, True),
                                         ("_2S_U", True, True)):
            if EAL.does_asset_exist(base + suffix):
                info = info or MaterialInfo(path)
                work.append((info, base + suffix, two_sided, unlit))
    log("fix-materials: %d existing instances to check" % len(work))
    imp.import_textures(list({id(w[0]): w[0] for w in work}.values()))
    changed_n, reparented, counts = 0, [], {}
    for i, (info, mi_path, two_sided, unlit) in enumerate(work):
        try:
            mi = EAL.load_asset(mi_path)
            if not isinstance(mi, unreal.MaterialInstanceConstant):
                continue
            changed = imp.apply_material(mi, info, two_sided, unlit)
            if changed:
                MEL.update_material_instance(mi)
                EAL.save_loaded_asset(mi, only_if_is_dirty=False)
                changed_n += 1
                for c in changed:
                    counts[c] = counts.get(c, 0) + 1
                if "parent" in changed:
                    reparented.append(mi_path)
                if "Base" in changed or "parent" in changed:
                    log("%s: %s (%s)" % (mi_path, ", ".join(changed), info.rule))
        except Exception as ex:
            error("fix-materials %s: %s" % (mi_path, ex))
        if (i + 1) % 500 == 0:
            log("fix-materials: %d/%d checked, %d changed (%.0fs)" % (i + 1, len(work), changed_n, time.time() - t0))
    meshes = 0
    for mi_path in reparented:
        for pkg in EAL.find_package_referencers_for_asset(mi_path, False) or []:
            pkg = str(pkg)
            if not pkg.startswith(IMPORTED_ROOT + "/StaticMeshes/"):
                continue
            sm = EAL.load_asset(pkg)
            if isinstance(sm, unreal.StaticMesh) and nanite_recheck(sm):
                EAL.save_loaded_asset(sm, only_if_is_dirty=False)
                meshes += 1
                log("%s: Nanite %s" % (pkg, "on" if sm.get_editor_property("nanite_settings").get_editor_property("enabled") else "off"))
    log("fix-materials: %d of %d instances changed (%s), %d textures imported, %d meshes switched Nanite, %.1fs"
        % (changed_n, len(work), ", ".join("%s %d" % kv for kv in sorted(counts.items())), imp.stats["textures"],
           meshes, time.time() - t0))


# ---- terrain ----------------------------------------------------------------------

def read_png_gray(path):
    """Minimal PNG reader for the exporter's own grayscale output (8/16 bit, filter 0)."""
    import struct
    import zlib
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat = 8, bytearray()
    w = h = depth = ctype = 0
    while pos < len(data):
        ln, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + ln
    if ctype != 0 or depth not in (8, 16):
        raise ValueError("unsupported PNG (color type %d, depth %d)" % (ctype, depth))
    raw = zlib.decompress(bytes(idat))
    bpp = depth // 8
    stride = w * bpp + 1
    out = array.array("H" if depth == 16 else "B")
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        if row[0] != 0:
            raise ValueError("PNG row filter %d not supported" % row[0])
        a = array.array("H" if depth == 16 else "B", row[1:])
        if depth == 16 and sys.byteorder == "little":
            a.byteswap()
        out.extend(a)
    return w, h, out


def terrain_primitive(t, actor):
    """UE2 TerrainInfo -> one world-space primitive (Unreal space, UE winding).
    vertex(x, y) = location + ((x - W/2) * sx, (y - H/2) * sy, (h - 32768) * sz / 256)."""
    if not t.get("heightmap_file"):
        return None
    w, h, hm = read_png_gray(os.path.join(CACHE_DIR, t["heightmap_file"]))
    props = (actor or {}).get("properties") or {}
    sx, sy, sz = t["terrain_scale"]
    lx, ly, lz = t["location"]
    inverted = bool(props.get("Inverted"))
    visible_bits = props.get("QuadVisibilityBitmap")   # set bit = visible quad; absent = all visible
    turn_bits = props.get("EdgeTurnBitmap") or []

    def bit(bits, i):
        k = i >> 5
        return k < len(bits) and (int(bits[k]) & 0xFFFFFFFF) >> (i & 31) & 1

    def height(x, y):
        return lz + (hm[y * w + x] - 32768) * sz / 256.0

    layers = t.get("layers") or []
    material = layers[0]["texture"] if layers else "None"
    us = float(layers[0].get("u_scale") or 1.0) if layers else 1.0
    vs = float(layers[0].get("v_scale") or 1.0) if layers else 1.0
    pos, nrm, uv, idx = array.array("f"), array.array("f"), array.array("f"), array.array("I")
    for y in range(h):
        for x in range(w):
            pos.extend((lx + (x - w / 2.0) * sx, ly + (y - h / 2.0) * sy, height(x, y)))
            dx = height(min(x + 1, w - 1), y) - height(max(x - 1, 0), y)
            dy = height(x, min(y + 1, h - 1)) - height(x, max(y - 1, 0))
            n = (-dx / (2.0 * sx), -dy / (2.0 * sy), 1.0)
            ln = math.sqrt(n[0] * n[0] + n[1] * n[1] + 1.0)
            s = -1.0 if inverted else 1.0
            nrm.extend((s * n[0] / ln, s * n[1] / ln, s / ln))
            uv.extend((x / us, y / vs))
    for y in range(h - 1):
        for x in range(w - 1):
            q = y * w + x
            if visible_bits and not bit(visible_bits, q):
                continue
            a, b, c, d = q, q + 1, q + w, q + w + 1
            # cross(b - a, c - a) points up (+Z); Unreal winding wants the opposite
            if bit(turn_bits, q):
                tris = ((a, d, b), (a, c, d))
            else:
                tris = ((a, c, b), (b, c, d))
            for tri in tris:
                idx.extend(tri if not inverted else (tri[0], tri[2], tri[1]))
    if not idx:
        return None
    return {"pos": pos, "nrm": nrm, "uv": uv, "idx": idx, "material": material, "extras": {},
            "key": (material, False, False, True)}


_SECTION_API = None


def section_api():
    """StaticMeshEditorSubsystem when available, else the older EditorStaticMeshLibrary."""
    global _SECTION_API
    if _SECTION_API is None:
        sub = None
        try:
            sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
        except Exception:
            sub = None
        _SECTION_API = sub if sub is not None and hasattr(sub, "enable_section_collision") else unreal.EditorStaticMeshLibrary
    return _SECTION_API


# ---- master material graphs -----------------------------------------------------

def _expr(mat, cls, x, y, **props):
    e = MEL.create_material_expression(mat, cls, x, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def _link(src, dst, dst_input="", src_output=""):
    if not MEL.connect_material_expressions(src, src_output, dst, dst_input):
        raise RuntimeError("failed to connect %s.%s -> %s.%s" % (src.get_name(), src_output, dst.get_name(), dst_input))


def _prop(src, prop, src_output=""):
    if not MEL.connect_material_property(src, src_output, prop):
        raise RuntimeError("failed to connect %s -> %s" % (src.get_name(), prop))


def _set_usages(mat):
    """Usage flags baked in the editor (cooked builds cannot add them later)."""
    # MATUSAGE_SKELETAL_MESH: player models and weapons (Source/OpenUT2k4/ArenaArt.*) use these masters too.
    for n in ("MATUSAGE_STATIC_LIGHTING", "MATUSAGE_INSTANCED_STATIC_MESHES", "MATUSAGE_NANITE", "MATUSAGE_SKELETAL_MESH"):
        usage = getattr(unreal.MaterialUsage, n, None)
        if usage is None:
            continue
        try:
            if hasattr(MEL, "set_base_material_usage"):
                MEL.set_base_material_usage(mat, usage, True)
            else:
                MEL.set_material_usage(mat, usage)
        except Exception as ex:
            warn("could not set usage %s on %s: %s" % (n, mat.get_name(), ex))


NON_NANITE_MASTERS = ("M_ImportedTranslucent", "M_ImportedAdditive", "M_ImportedModulate")


def enable_nanite(sm):
    """Nanite on, unless a slot uses a blend mode Nanite can't draw (same rules as Tools/EnableNanite.py).
    The fallback keeps every triangle: complex-as-simple collision is built from it."""
    for slot in sm.get_editor_property("static_materials"):
        mat = slot.get_editor_property("material_interface")
        for _ in range(16):
            if not isinstance(mat, unreal.MaterialInstance) or mat.get_editor_property("parent") is None:
                break
            mat = mat.get_editor_property("parent")
        if mat is not None and mat.get_name() in NON_NANITE_MASTERS:
            return
    nanite = sm.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", True)
    nanite.set_editor_property("keep_percent_triangles", 1.0)
    nanite.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.PERCENT_TRIANGLES)
    nanite.set_editor_property("fallback_percent_triangles", 1.0)
    sm.set_editor_property("nanite_settings", nanite)


MASTER_VERSION = 2               # 2: UVTransform / UVOffsetPan, EmissiveAlpha
MASTER_VERSION_TAG = "UT26ImportedMasterVersion"


def build_master(name, blend, unlit):
    path = "%s/%s" % (MASTER_DIR, name)
    mat = EAL.load_asset(path) if EAL.does_asset_exist(path) else None
    if mat is None:
        mat = TOOLS.create_asset(name, MASTER_DIR, unreal.Material, unreal.MaterialFactoryNew())
    else:
        MEL.delete_all_material_expressions(mat)
    mat.set_editor_property("blend_mode", blend)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT if unlit
                            else unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property("two_sided", False)
    if blend == unreal.BlendMode.BLEND_MASKED:
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
    _set_usages(mat)

    default_tex = EAL.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    base = _expr(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0, parameter_name="Base")
    if default_tex is not None:
        base.set_editor_property("texture", default_tex)
    # UT2004 TexScaler / TexRotator / TexPanner: uv' = (u a + v c + e, u b + v d + f) + frac(time * pan)
    # UVTransform = (a, b, c, d), UVOffsetPan = (e, f, pan u, pan v); defaults are the identity.
    tc = _expr(mat, unreal.MaterialExpressionTextureCoordinate, -2100, 0)
    xf = _expr(mat, unreal.MaterialExpressionVectorParameter, -2100, 150, parameter_name="UVTransform",
               default_value=unreal.LinearColor(1.0, 0.0, 0.0, 1.0))
    op = _expr(mat, unreal.MaterialExpressionVectorParameter, -2100, 350, parameter_name="UVOffsetPan",
               default_value=unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    u = _expr(mat, unreal.MaterialExpressionComponentMask, -1850, -50, r=True, g=False, b=False, a=False)
    v = _expr(mat, unreal.MaterialExpressionComponentMask, -1850, 50, r=False, g=True, b=False, a=False)
    _link(tc, u)
    _link(tc, v)
    ab = _expr(mat, unreal.MaterialExpressionAppendVector, -1850, 150)
    _link(xf, ab, "A", "R")
    _link(xf, ab, "B", "G")
    cd = _expr(mat, unreal.MaterialExpressionAppendVector, -1850, 250)
    _link(xf, cd, "A", "B")
    _link(xf, cd, "B", "A")
    ef = _expr(mat, unreal.MaterialExpressionAppendVector, -1850, 350)
    _link(op, ef, "A", "R")
    _link(op, ef, "B", "G")
    pan = _expr(mat, unreal.MaterialExpressionAppendVector, -1850, 450)
    _link(op, pan, "A", "B")
    _link(op, pan, "B", "A")
    ua = _expr(mat, unreal.MaterialExpressionMultiply, -1600, 0)
    _link(u, ua, "A")
    _link(ab, ua, "B")
    vc = _expr(mat, unreal.MaterialExpressionMultiply, -1600, 150)
    _link(v, vc, "A")
    _link(cd, vc, "B")
    lin = _expr(mat, unreal.MaterialExpressionAdd, -1450, 50)
    _link(ua, lin, "A")
    _link(vc, lin, "B")
    off = _expr(mat, unreal.MaterialExpressionAdd, -1300, 100)
    _link(lin, off, "A")
    _link(ef, off, "B")
    tm = _expr(mat, unreal.MaterialExpressionTime, -1600, 450)
    pt = _expr(mat, unreal.MaterialExpressionMultiply, -1450, 400)
    _link(tm, pt, "A")
    _link(pan, pt, "B")
    pf = _expr(mat, unreal.MaterialExpressionFrac, -1300, 400)
    _link(pt, pf)
    uv = _expr(mat, unreal.MaterialExpressionAdd, -1150, 150)
    _link(off, uv, "A")
    _link(pf, uv, "B")
    _link(uv, base, "UVs")
    tint = _expr(mat, unreal.MaterialExpressionVectorParameter, -900, -250, parameter_name="Tint",
                 default_value=unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    color = _expr(mat, unreal.MaterialExpressionMultiply, -600, -100)
    _link(base, color, "A", "RGB")
    _link(tint, color, "B", "RGB")

    if unlit:
        intensity = _expr(mat, unreal.MaterialExpressionScalarParameter, -900, 250, parameter_name="Intensity",
                          default_value=1.0)
        em = _expr(mat, unreal.MaterialExpressionMultiply, -400, 0)
        _link(color, em, "A")
        _link(intensity, em, "B")
        _prop(em, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    else:
        _prop(color, unreal.MaterialProperty.MP_BASE_COLOR)
        rough = _expr(mat, unreal.MaterialExpressionScalarParameter, -600, 150, parameter_name="Roughness",
                      default_value=0.8)
        _prop(rough, unreal.MaterialProperty.MP_ROUGHNESS)
        spec = _expr(mat, unreal.MaterialExpressionScalarParameter, -600, 250, parameter_name="Specular",
                     default_value=0.3)
        _prop(spec, unreal.MaterialProperty.MP_SPECULAR)
        emissive = _expr(mat, unreal.MaterialExpressionScalarParameter, -600, 350, parameter_name="Emissive",
                         default_value=0.0)
        # EmissiveAlpha: glow where the Base alpha is set (UT2004 Shader SelfIllumination masked by the
        # diffuse alpha); only set on opaque instances, whose alpha is otherwise unused
        em_alpha = _expr(mat, unreal.MaterialExpressionScalarParameter, -600, 450, parameter_name="EmissiveAlpha",
                         default_value=0.0)
        ea = _expr(mat, unreal.MaterialExpressionMultiply, -500, 450)
        _link(base, ea, "A", "A")
        _link(em_alpha, ea, "B")
        em_sum = _expr(mat, unreal.MaterialExpressionAdd, -450, 380)
        _link(emissive, em_sum, "A")
        _link(ea, em_sum, "B")
        em = _expr(mat, unreal.MaterialExpressionMultiply, -400, 300)
        _link(color, em, "A")
        _link(em_sum, em, "B")
        _prop(em, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if blend in (unreal.BlendMode.BLEND_MASKED, unreal.BlendMode.BLEND_TRANSLUCENT):
        opacity = _expr(mat, unreal.MaterialExpressionScalarParameter, -900, 400, parameter_name="Opacity",
                        default_value=1.0)
        alpha = _expr(mat, unreal.MaterialExpressionMultiply, -400, 450)
        _link(base, alpha, "A", "A")
        _link(opacity, alpha, "B")
        _prop(alpha, unreal.MaterialProperty.MP_OPACITY_MASK if blend == unreal.BlendMode.BLEND_MASKED
              else unreal.MaterialProperty.MP_OPACITY)
    MEL.layout_material_expressions(mat)
    errs = MEL.recompile_material(mat)
    for e in errs or []:
        error("%s compile error: %s" % (name, e))
    EAL.set_metadata_tag(mat, MASTER_VERSION_TAG, str(MASTER_VERSION))
    EAL.save_loaded_asset(mat, only_if_is_dirty=False)
    log("built master material " + path)
    return mat


# =============================================================================
# Level
# =============================================================================

GAMEPLAY_CLASSES = {"pickup", "xpickupbase", "xweaponbase", "weaponlocker", "xbombspawn", "xbombdelivery",
                    "gameobjective", "ctfbase", "navigationpoint", "projectile", "pawn"}


def rot_matrix(pitch, yaw, roll):
    """Rows = X, Y, Z axes of an Unreal rotator in degrees (same layout as FRotationMatrix)."""
    p, y, r = (math.radians(v) for v in (pitch, yaw, roll))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return [[cp * cy, cp * sy, sp],
            [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
            [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]]


def mesh_actor_transform(a, location=None, rotation=None):
    """map.json actor -> (location, rotator dict, scale) such that
    world = location + R * (scale * (vertex - pre_pivot)) (exporter's transform note)."""
    loc = list(location or a["location"])
    rot = rotation or a["rotation"]
    ds = float(a.get("draw_scale") or 1.0)
    ds3 = a.get("draw_scale3d") or [1.0, 1.0, 1.0]
    scale = [ds * float(s) for s in ds3]
    pre = a.get("pre_pivot") or [0.0, 0.0, 0.0]
    if any(abs(v) > 1e-4 for v in pre):
        m = rot_matrix(rot["pitch"], rot["yaw"], rot["roll"])
        sp = [scale[i] * pre[i] for i in range(3)]
        for c in range(3):
            loc[c] -= sp[0] * m[0][c] + sp[1] * m[1][c] + sp[2] * m[2][c]
    return loc, rot, scale


def actor_collides(a):
    p = a.get("properties") or {}
    if p.get("bCollideActors") is False or p.get("bBlockActors") is False:
        return False
    return True


def is_gameplay_actor(a):
    chain = {c.lower() for c in a.get("class_chain") or []}
    return bool(chain & GAMEPLAY_CLASSES)



def spawn(cls, loc, rot=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    r = unreal.Rotator(roll=rot["roll"], pitch=rot["pitch"], yaw=rot["yaw"]) if rot else unreal.Rotator(0, 0, 0)
    return eas.spawn_actor_from_class(cls, unreal.Vector(*loc), r, transient=False)


def build_level(imp, map_name, doc, bsp_meshes):
    path = "%s/%s" % (MAPS_DIR, map_name)
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    ensure_dir(MAPS_DIR)
    if EAL.does_asset_exist(path):
        # rebuild in place: load the level and remove everything placed by an earlier import
        if not les.load_level(path):
            error("could not load existing level " + path)
            return
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        old = [a for a in eas.get_all_level_actors()
               if str(a.get_folder_path()) in ("BSP", "StaticMeshes", "Lights", "Sky")]
        if old:
            eas.destroy_actors(old)
    elif not les.new_level(path, False):
        error("could not create level " + path)
        return
    world = ues.get_editor_world()
    t0 = time.time()

    # BSP
    for sm in bsp_meshes:
        act = spawn(unreal.StaticMeshActor, (0, 0, 0))
        comp = act.static_mesh_component
        comp.set_static_mesh(sm)
        comp.set_mobility(unreal.ComponentMobility.STATIC)
        comp.set_collision_profile_name("BlockAll")
        act.set_actor_label(sm.get_name())
        act.set_folder_path("BSP")

    # static mesh actors
    placed = skipped = 0
    for a in doc["actors"]:
        if not a.get("static_mesh") or a.get("hidden") or is_gameplay_actor(a):
            continue
        sm, section_slot = imp.meshes.get(a["static_mesh"], (None, {}))
        if sm is None:
            skipped += 1
            continue
        loc, rot = a["location"], a["rotation"]
        if "Mover" in (a.get("class_chain") or []):
            loc, rot = mover_pose(doc, a)
        loc, rot, scale = mesh_actor_transform(a, loc, rot)
        act = spawn(unreal.StaticMeshActor, loc, rot)
        act.set_actor_scale3d(unreal.Vector(*scale))
        comp = act.static_mesh_component
        comp.set_static_mesh(sm)
        comp.set_mobility(unreal.ComponentMobility.STATIC)
        if actor_collides(a):
            comp.set_collision_profile_name("BlockAll")
        else:
            comp.set_collision_profile_name("NoCollision")
        for i, skin in enumerate(a.get("skins") or []):
            if not skin or i not in section_slot and str(i) not in section_slot:
                continue
            slot = section_slot.get(i, section_slot.get(str(i)))
            idx = sm.get_material_index(slot)
            if idx >= 0:
                comp.set_material(idx, imp.material(material_variant_key(skin)))
        act.set_actor_label(a["name"])
        act.set_folder_path("StaticMeshes")
        placed += 1

    # lights
    nlights = add_lights(doc)
    add_sky(doc)
    ws = world.get_world_settings()
    try:
        ws.set_editor_property("default_game_mode", unreal.load_class(None, "/Script/OpenUT2k4.ArenaGameMode"))
    except Exception as ex:
        warn("could not set the game mode override: %s" % ex)
    kz = (doc.get("kill_z") or {}).get("z")
    if kz is not None:
        ws.set_editor_property("kill_z", float(kz))
    if not les.save_current_level():
        error("could not save " + path)
        return
    log("%s: level saved (%d BSP chunk(s), %d mesh actors, %d skipped, %d lights) in %.1fs" % (
        path, len(bsp_meshes), placed, skipped, nlights, time.time() - t0))


def mover_pose(doc, a):
    """Lifts stay at their bottom key; other movers (doors...) are placed at their last (open) key."""
    mv = next((m for m in doc["markers"].get("movers", []) if m["actor_index"] == a["index"]), None)
    loc, rot = list(a["location"]), dict(a["rotation"])
    if not mv:
        return loc, rot
    keys = mv.get("key_pos") or []   # KeyPos[1..]
    tag = str(a.get("tag"))
    is_lift = any(l["class"] == "LiftCenter" and str(l.get("lift_tag")) == tag for l in doc["markers"].get("lifts", []))
    if keys and not is_lift:
        k = keys[-1]
        loc = [loc[i] + k[i] for i in range(3)]
        krot = (mv.get("key_rot") or [])
        if len(krot) == len(keys):
            kr = krot[-1]
            rot = {"pitch": rot["pitch"] + kr["pitch"], "yaw": rot["yaw"] + kr["yaw"], "roll": rot["roll"] + kr["roll"]}
    return loc, rot


def add_lights(doc):
    n = 0
    for l in doc["markers"].get("lights", []):
        if l.get("sunlight"):
            continue
        b = float(l.get("brightness") or 0.0)
        if b <= 0.0:
            continue
        radius = max(200.0, min(float(l.get("radius_world") or 1000.0), 12000.0))
        # UE2 lights fall off gently over their whole radius: use UE's non inverse-squared falloff, where the
        # unitless intensity is a plain brightness multiplier (UE2 brightness 64 ~ 1.0).
        intensity = LIGHT_INTENSITY_SCALE * min(b, 512.0) / 64.0
        spot = l.get("light_effect") in ("Spotlight", "StaticSpot") or (l.get("directional") and not l.get("sunlight"))
        if spot:
            act = spawn(unreal.SpotLight, l["location"], l["rotation"])
            comp = act.get_component_by_class(unreal.SpotLightComponent)
            cone = max(5.0, min(80.0, float(l.get("cone") or 128) / 256.0 * 90.0))
            comp.set_editor_property("outer_cone_angle", cone)
            comp.set_editor_property("inner_cone_angle", cone * 0.6)
        else:
            act = spawn(unreal.PointLight, l["location"])
            comp = act.get_component_by_class(unreal.PointLightComponent)
        comp.set_mobility(unreal.ComponentMobility.MOVABLE)
        comp.set_editor_property("intensity_units", unreal.LightUnits.UNITLESS)
        comp.set_editor_property("intensity", intensity)
        comp.set_editor_property("light_color", _to_color(l.get("color_rgb") or [1, 1, 1]))
        comp.set_editor_property("attenuation_radius", radius)
        comp.set_editor_property("source_radius", 10.0)
        comp.set_editor_property("cast_shadows", False)
        comp.set_editor_property("use_inverse_squared_falloff", False)
        comp.set_editor_property("light_falloff_exponent", LIGHT_FALLOFF_EXPONENT)
        act.set_actor_label(l["actor"])
        act.set_folder_path("Lights")
        n += 1
    return n


def _to_color(rgb):
    return unreal.Color(r=int(max(0, min(255, round(rgb[0] * 255)))), g=int(max(0, min(255, round(rgb[1] * 255)))),
                        b=int(max(0, min(255, round(rgb[2] * 255)))), a=255)


def add_sky(doc):
    sun = next((l for l in doc["markers"].get("lights", []) if l.get("sunlight")), None)
    sun_rot = sun["rotation"] if sun else {"pitch": -50.0, "yaw": 35.0, "roll": 0.0}
    if sun_rot["pitch"] > -10.0:
        sun_rot = dict(sun_rot, pitch=-50.0)
    d = spawn(unreal.DirectionalLight, (0, 0, 5000), sun_rot)
    c = d.get_component_by_class(unreal.DirectionalLightComponent)
    c.set_mobility(unreal.ComponentMobility.MOVABLE)
    c.set_editor_property("intensity", 4.0)
    c.set_editor_property("atmosphere_sun_light", True)
    c.set_editor_property("cast_shadows", True)
    if sun:
        c.set_editor_property("light_color", _to_color(sun.get("color_rgb") or [1, 1, 1]))
    d.set_actor_label("Sun")
    d.set_folder_path("Sky")

    s = spawn(unreal.SkyLight, (0, 0, 3000))
    c = s.get_component_by_class(unreal.SkyLightComponent)
    c.set_mobility(unreal.ComponentMobility.MOVABLE)
    c.set_editor_property("source_type", unreal.SkyLightSourceType.SLS_CAPTURED_SCENE)
    c.set_editor_property("real_time_capture", True)
    c.set_editor_property("lower_hemisphere_is_black", False)
    c.set_editor_property("intensity", 1.0)
    s.set_actor_label("SkyLight")
    s.set_folder_path("Sky")

    a = spawn(unreal.SkyAtmosphere, (0, 0, 0))
    a.set_actor_label("SkyAtmosphere")
    a.set_folder_path("Sky")

    f = spawn(unreal.ExponentialHeightFog, (0, 0, -200))
    c = f.get_component_by_class(unreal.ExponentialHeightFogComponent)
    c.set_editor_property("fog_density", 0.004)
    c.set_editor_property("fog_height_falloff", 0.1)
    c.set_editor_property("start_distance", 1500.0)
    f.set_actor_label("HeightFog")
    f.set_folder_path("Sky")

    pp = spawn(unreal.PostProcessVolume, (0, 0, 0))
    pp.set_editor_property("unbound", True)
    pp.set_editor_property("priority", 10.0)
    pp.set_editor_property("blend_weight", 1.0)
    st = pp.get_editor_property("settings")
    for k, v in (("auto_exposure_method", unreal.AutoExposureMethod.AEM_MANUAL),
                 ("auto_exposure_apply_physical_camera_exposure", False),
                 ("auto_exposure_bias", EXPOSURE_BIAS),
                 ("bloom_intensity", 0.6),
                 ("vignette_intensity", 0.25)):
        st.set_editor_property("override_" + k, True)
        st.set_editor_property(k, v)
    pp.set_editor_property("settings", st)
    pp.set_actor_label("PostProcess")
    pp.set_folder_path("Sky")


# =============================================================================
# Main
# =============================================================================

def map_assets(doc):
    """Material keys and mesh paths a map needs."""
    mats, meshes = set(), set()
    for a in doc["actors"]:
        if a.get("static_mesh") and not a.get("hidden") and not is_gameplay_actor(a):
            meshes.add(a["static_mesh"])
            for s in a.get("skins") or []:
                if s:
                    mats.add(material_variant_key(s))
    for p in (doc.get("bsp") or {}).get("materials") or []:
        mats.add(material_variant_key(p))
    for t in doc["markers"].get("terrains", []):
        if t.get("layers"):
            mats.add(material_variant_key(t["layers"][0]["texture"]))
    return mats, meshes


def exported_maps():
    d = os.path.join(CACHE_DIR, "maps")
    if not os.path.isdir(d):
        return []
    return sorted(n for n in os.listdir(d) if os.path.isfile(os.path.join(d, n, "map.json")))


def parse_args():
    argv = sys.argv[1:]
    env = os.environ.get("OPENUT2K4_IMPORT_ARGS")
    if env and not argv:
        argv = shlex.split(env)
    ap = argparse.ArgumentParser(prog="ImportUT2004.py", description="Import UT2004 placeholder content")
    ap.add_argument("maps", nargs="*", help="map names (ImportCache/maps/<Map>)")
    ap.add_argument("--all-exported", action="store_true", help="every map in ImportCache/maps")
    ap.add_argument("--force", action="store_true", help="re-import assets that already exist")
    ap.add_argument("--markers-only", action="store_true", help="only write Content/ImportedData/<Map>.json")
    ap.add_argument("--all-assets", action="store_true",
                    help="import every asset in ImportCache (not only what maps reference)")
    ap.add_argument("--kinds", default=",".join(ASSET_KINDS),
                    help="with --all-assets: comma list of %s" % ",".join(ASSET_KINDS))
    ap.add_argument("--packages", nargs="+", default=None, help="with --all-assets: only these packages")
    ap.add_argument("--chunk", default=None, metavar="I/N",
                    help="with --all-assets: only chunk I (0-based) of N of the cache packages")
    ap.add_argument("--skip-built", action="store_true",
                    help="skip maps whose level was built from the current map.json (resume)")
    ap.add_argument("--list-asset-packages", action="store_true", help="print cache packages and exit")
    ap.add_argument("--fix-materials", action="store_true",
                    help="rewrite existing material instances in place from the current resolver (Base, parent, "
                         "parameters); list from Saved/ImportUT2004/material_fixes.json "
                         "(python3 -I Tools/ut2k4_materials.py), else every cached material")
    ap.add_argument("--fix-list", default=None, help="with --fix-materials: another fix list json")
    ap.add_argument("--report", action="store_true",
                    help="(plain python) summarize ImportCache / Content/Imported / maps and exit")
    ap.add_argument("--game", default="~/UnrealEngine/UT2004/Unreal Tournament 2004",
                    help="with --report: UT2004 folder whose Maps/ are checked")
    return ap.parse_args(argv)


def _du(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def report(game):
    """Counts by kind, sizes, and which maps of the install are missing a level or json (with reasons)."""
    out = []
    cache = {}
    for kind, ext in (("textures", ".png"), ("materials", ".json"), ("staticmeshes", ".gltf"),
                      ("sounds", ".json"), ("skeletalmeshes", ".gltf"), ("animations", ".json"),
                      ("vertmeshes", ".gltf")):
        n = 0
        root = os.path.join(CACHE_DIR, kind)
        for r, _d, files in os.walk(root):
            n += sum(1 for f in files if f.endswith(ext))
        cache[kind] = n
    seqs = 0
    for _p, _jp, m in cache_entries("animations"):
        if m.get("gltf"):
            seqs += len(m.get("sequences") or [])
    cache["animation_sequences"] = seqs
    cache["music"] = len([f for f in os.listdir(os.path.join(CACHE_DIR, "music")) if f.endswith(".ogg")]) \
        if os.path.isdir(os.path.join(CACHE_DIR, "music")) else 0
    cache["maps"] = len(exported_maps())
    out.append("ImportCache: %s" % ", ".join("%s %d" % kv for kv in cache.items()))
    content = os.path.join(PROJECT_DIR, "Content", "Imported")
    imported = {}
    for top in sorted(os.listdir(content)) if os.path.isdir(content) else []:
        n = 0
        for r, _d, files in os.walk(os.path.join(content, top)):
            n += sum(1 for f in files if f.endswith((".uasset", ".umap")))
        imported[top] = n
    out.append("Content/Imported (.uasset/.umap files): %s" % ", ".join("%s %d" % kv for kv in imported.items()))
    for top in ("Animations", "SkeletalMeshes"):
        d = os.path.join(content, top)
        if os.path.isdir(d):
            out.append("  %s packages: %d" % (top, len(os.listdir(d))))
    out.append("Sizes: ImportCache %.2f GB, Content/Imported %.2f GB, Content/ImportedData %.1f MB" % (
        _du(CACHE_DIR) / 1e9, _du(content) / 1e9, _du(DATA_DIR) / 1e6))
    mdir = os.path.join(game, "Maps")
    names = sorted(os.path.splitext(f)[0] for f in os.listdir(mdir) if f.lower().endswith(".ut2")) \
        if os.path.isdir(mdir) else []
    errs = []
    for lf in (os.path.join(CACHE_DIR, "export_errors.log"), os.path.join(LOG_DIR, "import_errors.log")):
        if os.path.isfile(lf):
            with open(lf, "r", encoding="utf-8", errors="replace") as f:
                errs += f.read().splitlines()
    missing = []
    for n in names:
        lvl = os.path.join(content, "Maps", n + ".umap")
        js = os.path.join(DATA_DIR, n + ".json")
        if os.path.isfile(lvl) and os.path.isfile(js):
            continue
        why = [e for e in errs if n.lower() in e.lower() and ("map" in e.lower() or "level" in e.lower())]
        missing.append((n, os.path.isfile(lvl), os.path.isfile(js),
                        why[-1].split("\t")[-1][:200] if why else "no error logged"))
    out.append("Maps in %s: %d, imported (level + json): %d, missing: %d" % (
        mdir, len(names), len(names) - len(missing), len(missing)))
    for n, lvl, js, why in missing:
        out.append("  MISSING %s (level %s, json %s): %s" % (n, lvl, js, why))
    print("\n".join(out))
    return 0


def map_stamp_name(name):
    return "map_" + asset_name(name)


def main():
    args = parse_args()
    maps = list(args.maps)
    if args.all_exported:
        maps += [m for m in exported_maps() if m not in maps]
    kinds = [k.strip() for k in args.kinds.split(",") if k.strip()]
    if args.report:
        return report(args.game)
    if args.list_asset_packages:
        for p in cache_packages(["textures", "materials", "staticmeshes", "sounds"]):
            print(p)
        return 0
    if args.fix_materials:
        if unreal is None:
            print("--fix-materials runs inside the editor", file=sys.stderr)
            return 2
        imp = Importer(force=False)
        imp.build_masters()
        fix_materials(imp, args.fix_list)
        return 1 if _errors else 0
    if not maps and not args.all_assets:
        error("no maps given (use map names, --all-exported or --all-assets)")
        return 1
    if unreal is None and not args.markers_only:
        print("not running inside the editor: only --markers-only is available", file=sys.stderr)
        return 2

    docs = {}
    for name in maps:
        mj = os.path.join(CACHE_DIR, "maps", name, "map.json")
        if not os.path.isfile(mj):
            error("%s: %s missing (run Tools/ut2k4_export.py --map %s first)" % (name, mj, name))
            continue
        try:
            docs[name] = load_json(mj)
            write_markers(docs[name])
        except Exception as ex:
            error("%s: markers: %s" % (name, ex))
            docs.pop(name, None)
    if args.markers_only or unreal is None:
        return 1 if _errors else 0

    t0 = time.time()
    imp = Importer(force=args.force)
    imp.build_masters()
    if args.all_assets:
        packages = args.packages
        if args.chunk:
            i, n = (int(x) for x in args.chunk.split("/"))
            allp = cache_packages(["textures", "materials", "staticmeshes", "sounds"])
            packages = [p for k, p in enumerate(allp) if k * n // len(allp) == i] if allp else []
            log("chunk %d/%d: %d packages (%s .. %s)" % (i, n, len(packages), packages[0] if packages else "-",
                                                         packages[-1] if packages else "-"))
        try:
            import_all_assets(imp, kinds, packages, args.force)
        except Exception as ex:
            import traceback
            error("all-assets pass failed: %s\n%s" % (ex, traceback.format_exc()))
    for name, doc in docs.items():
        tm = time.time()
        mj = os.path.join(CACHE_DIR, "maps", name, "map.json")
        if args.skip_built and not args.force and EAL.does_asset_exist("%s/%s" % (MAPS_DIR, name)) \
                and stamp_ok(map_stamp_name(name), [mj]):
            log("%s: level is up to date" % name)
            continue
        log("=== %s ===" % name)
        try:
            import_map(imp, name, doc)
            write_stamp(map_stamp_name(name))
        except Exception as ex:
            import traceback
            error("%s: map import failed: %s\n%s" % (name, ex, traceback.format_exc()))
        log("%s done in %.1fs" % (name, time.time() - tm))
    log("finished in %.1fs: %d textures, %d material instances, %d static meshes imported; %d warnings, %d errors"
        % (time.time() - t0, imp.stats["textures"], imp.stats["materials"], imp.stats["meshes"],
           len(_warnings), len(_errors)))
    if _errors and not (args.all_assets or args.skip_built):
        raise RuntimeError("ImportUT2004 finished with %d error(s)" % len(_errors))
    return 0


def import_map(imp, name, doc):
    mats, meshes = map_assets(doc)
    # material keys used by the meshes' sections
    for mp in meshes:
        gltf = cache_file("staticmeshes", mp, ".gltf")
        if os.path.isfile(gltf):
            for m in load_json(gltf).get("materials", []):
                mats.add(material_variant_key(m["name"]))
    bsp_doc = os.path.join(CACHE_DIR, "maps", name, "bsp.gltf")
    if os.path.isfile(bsp_doc):
        bd = load_json(bsp_doc)
        for p in bd["meshes"][0]["primitives"]:
            flags = set((p.get("extras") or {}).get("poly_flag_names") or [])
            mats.add(material_variant_key(bd["materials"][p["material"]]["name"], "TwoSided" in flags, "Unlit" in flags))
    infos = [MaterialInfo(p) for p in sorted({k[0] for k in mats})]
    imp.import_textures(infos)
    for k in sorted(mats):
        imp.material(k)
    imp.import_meshes(meshes)
    bsp = imp.import_bsp(name, os.path.join(CACHE_DIR, "maps", name), doc)
    bsp += imp.import_terrains(name, doc)
    build_level(imp, name, doc, bsp)


if __name__ == "__main__" or unreal is not None:
    _rc = main()
    if unreal is None:
        sys.exit(_rc)
