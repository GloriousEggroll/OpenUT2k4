"""
Enable Nanite on the imported UT2004 static meshes (editor Python, run by Scripts/enable-nanite.sh).

Large non-Nanite meshes (BSP and terrain chunks especially) overflow the virtual shadow map's
non-Nanite marking queue and make shadows expensive. Nanite fixes both.

- Meshes whose materials include translucent / additive / modulate slots stay non-Nanite (Nanite
  does not render those blend modes).
- The fallback mesh keeps 100% of the triangles: complex-as-simple collision is built from the
  render LOD (the fallback for a Nanite mesh), so a reduced fallback would change the collision.
- Resumable: meshes already set up are skipped. Chunk with OPENUT2K4_NANITE_CHUNK="<index>/<count>".

Usage (inside the editor): UnrealEditor-Cmd OpenUT2k4.uproject -run=pythonscript -script=Tools/EnableNanite.py
"""

import os
import time

import unreal

ROOT = "/Game/Imported/StaticMeshes"
NON_NANITE_MASTERS = ("M_ImportedTranslucent", "M_ImportedAdditive", "M_ImportedModulate")
EAL = unreal.EditorAssetLibrary


def log(msg):
    unreal.log("[EnableNanite] " + msg)


def base_material_name(mat):
    """Name of the root material of a material instance chain."""
    seen = 0
    while isinstance(mat, unreal.MaterialInstance) and seen < 16:
        parent = mat.get_editor_property("parent")
        if parent is None:
            break
        mat = parent
        seen += 1
    return mat.get_name() if mat else ""


def nanite_compatible(sm):
    for m in sm.get_editor_property("static_materials"):
        mat = m.get_editor_property("material_interface")
        if mat and base_material_name(mat) in NON_NANITE_MASTERS:
            return False
    return True


def configure(sm):
    """Returns 'enabled', 'skipped' (incompatible) or 'unchanged' (already done)."""
    settings = sm.get_editor_property("nanite_settings")
    want = nanite_compatible(sm)
    full_fallback = (settings.get_editor_property("fallback_target") == unreal.NaniteFallbackTarget.PERCENT_TRIANGLES
                     and settings.get_editor_property("fallback_percent_triangles") >= 1.0)
    if settings.get_editor_property("enabled") == want and (not want or full_fallback):
        return "unchanged"
    settings.set_editor_property("enabled", want)
    if want:
        settings.set_editor_property("keep_percent_triangles", 1.0)
        settings.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.PERCENT_TRIANGLES)
        settings.set_editor_property("fallback_percent_triangles", 1.0)
    # set_editor_property on the mesh runs Pre/PostEditChange, which rebuilds the render data
    sm.set_editor_property("nanite_settings", settings)
    EAL.save_loaded_asset(sm, only_if_is_dirty=False)
    return "enabled" if want else "skipped"


def main():
    index, count = 0, 1
    chunk = os.environ.get("OPENUT2K4_NANITE_CHUNK", "")
    if "/" in chunk:
        index, count = (int(v) for v in chunk.split("/", 1))
    paths = sorted(p.split(".")[0] for p in EAL.list_assets(ROOT, recursive=True, include_folder=False))
    paths = [p for i, p in enumerate(paths) if i % count == index]
    log("chunk %d/%d: %d assets" % (index, count, len(paths)))
    stats = {"enabled": 0, "skipped": 0, "unchanged": 0, "failed": 0}
    t0 = time.time()
    for n, path in enumerate(paths):
        try:
            sm = EAL.load_asset(path)
            if not isinstance(sm, unreal.StaticMesh):
                continue
            stats[configure(sm)] += 1
        except Exception as ex:  # keep going: one bad asset must not stop the pass
            stats["failed"] += 1
            unreal.log_warning("[EnableNanite] %s: %s" % (path, ex))
        if n % 200 == 199:
            log("%d/%d (%.0fs) %s" % (n + 1, len(paths), time.time() - t0, stats))
            unreal.SystemLibrary.collect_garbage()
    log("finished chunk %d/%d in %.0fs: %s" % (index, count, time.time() - t0, stats))


main()
