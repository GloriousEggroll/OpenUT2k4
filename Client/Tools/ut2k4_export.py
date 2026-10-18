#!/usr/bin/env python3
"""Convert assets from a locally installed Unreal Tournament 2004 into intermediate
files (PNG / glTF / WAV / OGG / JSON) for the OpenUT2k4 editor import step.

The output is local-only (ImportCache/ is git-ignored). Nothing from the game is
stored in the source tree.

Examples:
  Tools/ut2k4_export.py --game "/path/to/UT2004" --list-maps
  Tools/ut2k4_export.py --game "/path/to/UT2004" --out ImportCache --map DM-Rankin CTF-Face3
  Tools/ut2k4_export.py --game "/path/to/UT2004" --packages HumanoidArchitecture
  Tools/ut2k4_export.py --game "/path/to/UT2004" --everything      # whole install, resumable
"""

import argparse
import json
import os
import sys
import time

sys.dont_write_bytecode = True  # keep the source tree free of __pycache__
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ut2k4 import SCALE  # noqa: E402
from ut2k4.package import PackageManager, ReadError  # noqa: E402
from ut2k4.exporter import Exporter  # noqa: E402
from ut2k4.gltf import check_gltf  # noqa: E402

DEFAULT_GAME = '~/UnrealEngine/UT2004/Unreal Tournament 2004'
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def list_maps(mgr):
    rows = []
    for key, path in sorted(mgr.package_files('.ut2').items()):
        name = os.path.splitext(os.path.basename(path))[0]
        title = author = ''
        try:
            pkg = mgr.load_package(name)
            si = pkg.exports_of_class('LevelSummary')
            if si:
                p = pkg.get_object(si[0]).props
                title, author = p.get('Title', ''), p.get('Author', '')
            pkg.close()
            mgr.packages.pop(key, None)
        except (ReadError, OSError) as e:
            title = '<error: %s>' % e
        rows.append((name, title, author))
    w = max((len(r[0]) for r in rows), default=10)
    for name, title, author in rows:
        print('%-*s  %-28s  %s' % (w, name, title, author))
    print('%d maps' % len(rows))


ASSET_EXTS = ('.utx', '.usx', '.uax', '.ukx', '.u')


def asset_packages(mgr):
    """Every non-map package of the install that can hold assets (System/*.u included,
    since resource packages like SkaarjPack_rc.u carry meshes, textures and sounds)."""
    out = []
    for key, path in sorted(mgr.package_files().items()):
        if os.path.splitext(path)[1].lower() in ASSET_EXTS:
            out.append(os.path.splitext(os.path.basename(path))[0])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME,
                    help='UT2004 install folder (contains System/, Maps/, ...). Default: %(default)s')
    ap.add_argument('--out', default=os.path.join(PROJECT_ROOT, 'ImportCache'),
                    help='output folder (default: <project>/ImportCache)')
    ap.add_argument('--map', nargs='+', default=[], metavar='NAME',
                    help='map(s) to export, e.g. DM-Rankin (also exports referenced assets)')
    ap.add_argument('--all-maps', action='store_true', help='export every map in Maps/')
    ap.add_argument('--packages', nargs='+', default=[], metavar='PKG',
                    help='export all textures/materials/meshes/sounds of these packages')
    ap.add_argument('--everything', action='store_true',
                    help='export every asset package (textures, static meshes, sounds, animations, '
                         'System/*.u resources), every Music/*.ogg and every map. Implies --skip-existing')
    ap.add_argument('--all-packages', action='store_true',
                    help='export every non-map package of the install (all asset kinds)')
    ap.add_argument('--music', action='store_true', help='copy every Music/*.ogg with a json sidecar')
    ap.add_argument('--skip-existing', action='store_true',
                    help='resume: keep files an earlier run wrote after the package last changed')
    ap.add_argument('--force', action='store_true', help='with --everything: re-export everything')
    ap.add_argument('--error-log', default=None,
                    help='append per-asset failures here (default: <out>/export_errors.log)')
    ap.add_argument('--list-maps', action='store_true', help='list maps with title/author and exit')
    ap.add_argument('--scale', type=float, default=SCALE,
                    help='uniform position scale UE2 units -> cm (default %(default)s)')
    ap.add_argument('--png-level', type=int, default=6, help='zlib level for PNGs (0-9)')
    ap.add_argument('--maps-only', action='store_true',
                    help='write only map.json + bsp.gltf, skip referenced textures/meshes/sounds')
    ap.add_argument('--check', action='store_true',
                    help='validate every written .gltf with the built-in checker')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args(argv)

    if not os.path.isdir(os.path.join(args.game, 'System')):
        ap.error('--game must point at the UT2004 folder that contains System/ (%s)' % args.game)
    mgr = PackageManager(args.game, verbose=args.verbose)

    if args.list_maps:
        list_maps(mgr)
        return 0

    maps = list(args.map)
    packages = list(args.packages)
    if args.everything:
        args.all_maps = args.all_packages = args.music = True
        args.skip_existing = not args.force
    if args.all_maps:
        maps += [os.path.splitext(os.path.basename(p))[0]
                 for p in sorted(mgr.package_files('.ut2').values())]
    if args.all_packages:
        packages += asset_packages(mgr)
    if not maps and not packages and not args.music:
        ap.error('nothing to do: give --map, --all-maps, --packages, --everything or --list-maps')

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    ex = Exporter(mgr, out, scale=args.scale, png_level=args.png_level, verbose=args.verbose,
                  assets=not args.maps_only)
    ex.skip_existing = args.skip_existing
    err_log = open(args.error_log or os.path.join(out, 'export_errors.log'), 'a', encoding='utf-8')
    err_log.write('# %s run %s: %d packages, %d maps, music=%s\n' % (
        time.strftime('%Y-%m-%d %H:%M:%S'), ' '.join((argv or sys.argv[1:])[:8]), len(packages),
        len(maps), args.music))
    t0 = time.time()
    n_err = [0]

    def flush_errors(unit):
        for e in ex.errors[n_err[0]:]:
            err_log.write('%s\t%s\n' % (unit, e))
        n_err[0] = len(ex.errors)
        err_log.flush()

    def release():
        # drop every open package (mmaps + parsed objects) between units of work
        mgr.close()

    if args.music:
        for path in ex.music_files():
            try:
                ex.export_music_file(path)
            except Exception as e:
                ex.error('music %s' % path, e)
        flush_errors('Music')
    for i, name in enumerate(packages):
        try:
            ex.export_package(name)
        except Exception as e:  # keep going
            ex.error('package %s' % name, e)
        flush_errors(name)
        if args.all_packages:
            release()
            if (i + 1) % 25 == 0:
                print('[progress] %d/%d packages, %.0fs' % (i + 1, len(packages), time.time() - t0), flush=True)
    for i, name in enumerate(maps):
        try:
            ex.export_map(name)
        except Exception as e:
            ex.error('map %s' % name, e)
        flush_errors(name)
        # release map packages between maps to keep memory bounded
        if args.all_maps:
            release()
        else:
            mgr.packages.pop(name.lower(), None)
        if args.all_maps and (i + 1) % 10 == 0:
            print('[progress] %d/%d maps, %.0fs' % (i + 1, len(maps), time.time() - t0), flush=True)

    if args.check:
        bad = 0
        for root, _dirs, files in os.walk(out):
            for fn in files:
                if fn.endswith('.gltf'):
                    try:
                        check_gltf(os.path.join(root, fn))
                    except Exception as e:
                        bad += 1
                        ex.error('check %s' % os.path.join(root, fn), e)
        print('glTF check: %d invalid' % bad)

    c = ex.counts
    print('Done in %.1fs: %d maps (%d up to date), %d textures, %d materials, %d static meshes, %d sounds, '
          '%d skeletal meshes, %d animations, %d vertex meshes, %d music, %d errors'
          % (time.time() - t0, c['maps'], c.get('maps_skipped', 0), c['textures'], c['materials'],
             c['staticmeshes'], c['sounds'], c['skeletalmeshes'], c['animations'], c['vertmeshes'],
             c['music'], len(ex.errors)))
    print('Output: %s' % out)
    err_log.close()
    if args.everything or args.all_packages or args.all_maps:
        summary = {'finished': time.strftime('%Y-%m-%d %H:%M:%S'), 'seconds': round(time.time() - t0, 1),
                   'counts': c, 'errors': ex.errors}
        with open(os.path.join(out, 'export_summary.json'), 'w', encoding='utf-8') as f:
            json.dump(summary, f, indent=1)
    mgr.close()
    if args.everything or args.all_packages or args.all_maps:
        return 0     # per-asset failures are in the error log / export_summary.json
    return 1 if ex.errors and not (c['maps'] or c['textures'] or c['staticmeshes']) else 0


if __name__ == '__main__':
    sys.exit(main())
