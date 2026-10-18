#!/usr/bin/env python3
"""Light pass: add each imported map's music to Content/ImportedData/<Map>.json.

Reads only the LevelInfo actor of each UT2004 map (its Song property) - no geometry, no assets, no
re-import. When the map's .ut2 cannot be read, the song is taken from ImportCache/maps/<Map>/map.json
(level_info.song, written by Tools/ut2k4/level.py) if that exists.

Fields written into Content/ImportedData/<Map>.json:
  "music"        the UT2004 song name from LevelInfo.Song (e.g. "KR-RobotFactory"), or null
  "music_asset"  the matching SoundWave from Content/ImportedData/Assets/Music.json
                 (e.g. "/Game/Imported/Music/KR-RobotFactory"), or null when no imported track matches.
                 The game checks /Game/Replacements/<same path> first at runtime.

Tools/ImportUT2004.py (slim_markers) writes the same two fields, so future imports keep them.

Usage:
  Tools/ut2k4_map_music.py [--game "/path/to/UT2004"] [--dry-run] [MAP ...]
"""

import argparse
import json
import os
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(PROJECT_ROOT, 'Content', 'ImportedData')
MANIFEST = os.path.join(DATA_DIR, 'Assets', 'Music.json')
CACHE_MAPS = os.path.join(PROJECT_ROOT, 'ImportCache', 'maps')
DEFAULT_GAME = os.path.expanduser('~/UnrealEngine/UT2004/Unreal Tournament 2004')


def norm_song(song):
    """UT2004 Song value -> clean name, or None ("None", "", "Foo.ogg" -> "Foo")."""
    if not isinstance(song, str):
        return None
    s = song.strip()
    if s.lower().endswith('.ogg'):
        s = s[:-4]
    if not s or s.lower() == 'none':
        return None
    return s


def _key(name):
    return re.sub(r'[^a-z0-9]', '', name.lower())


def load_manifest():
    try:
        with open(MANIFEST, 'r', encoding='utf-8') as f:
            assets = json.load(f).get('assets', {})
    except (OSError, ValueError):
        return {}, {}
    exact = {k.lower(): v for k, v in assets.items()}
    loose = {_key(k): v for k, v in assets.items()}
    return exact, loose


def match_asset(song, manifest):
    """Song name -> imported SoundWave path, matching case-insensitively, then ignoring punctuation
    ("KR-SniperTime" -> "KR-Sniper-Time")."""
    if not song:
        return None
    exact, loose = manifest
    e = exact.get(song.lower()) or loose.get(_key(song))
    if not e:
        return None
    return e.get('imported') or e.get('asset')


def song_from_package(mgr, map_name):
    if mgr is None:
        return False, None
    try:
        pkg = mgr.load_package(map_name)
    except Exception:
        return False, None
    if pkg is None:
        return False, None
    try:
        for ei in pkg.exports_of_class('LevelInfo'):
            props = pkg.get_object(ei).props
            return True, props.get('Song')
        return False, None
    except Exception:
        return False, None
    finally:
        try:
            pkg.close()
        except Exception:
            pass
        mgr.packages.pop(map_name.lower(), None)


def song_from_cache(map_name):
    path = os.path.join(CACHE_MAPS, map_name, 'map.json')
    try:
        with open(path, 'r', encoding='utf-8') as f:
            li = json.load(f).get('level_info') or {}
        return True, li.get('song')
    except (OSError, ValueError):
        return False, None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', default=DEFAULT_GAME, help='UT2004 install folder (default: %(default)s)')
    ap.add_argument('--dry-run', action='store_true', help='print, do not write')
    ap.add_argument('maps', nargs='*', help='map names (default: every Content/ImportedData/*.json)')
    args = ap.parse_args(argv)

    mgr = None
    if os.path.isdir(os.path.join(args.game, 'System')):
        try:
            from ut2k4.package import PackageManager
            mgr = PackageManager(args.game)
        except Exception as e:  # noqa: BLE001
            print('warning: cannot open UT2004 install (%s); using ImportCache only' % e)

    manifest = load_manifest()
    maps = args.maps or sorted(os.path.splitext(f)[0] for f in os.listdir(DATA_DIR) if f.endswith('.json'))
    counts = {'matched': 0, 'no_asset': 0, 'no_song': 0, 'unread': 0}
    for name in maps:
        path = os.path.join(DATA_DIR, name + '.json')
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except (OSError, ValueError) as e:
            print('%s: skipped (%s)' % (name, e))
            continue
        ok, raw = song_from_package(mgr, name)
        source = 'ut2'
        if not ok:
            ok, raw = song_from_cache(name)
            source = 'cache'
        if not ok:
            counts['unread'] += 1
            print('%s: LevelInfo not found' % name)
            continue
        song = norm_song(raw)
        asset = match_asset(song, manifest)
        counts['matched' if asset else ('no_asset' if song else 'no_song')] += 1
        print('%-32s %-28s %s (%s)' % (name, song, asset or '-', source))
        if args.dry_run or (data.get('music') == song and data.get('music_asset') == asset and 'music' in data):
            continue
        data['music'] = song
        data['music_asset'] = asset
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=1)
    print('%d maps: %d with a matching track, %d song without an imported track, %d without a song, %d unreadable' % (
        len(maps), counts['matched'], counts['no_asset'], counts['no_song'], counts['unread']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
