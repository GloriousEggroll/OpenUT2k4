"""UE2.5 package (.ut2/.utx/.usx/.uax/.ukx/.u) reader with lazy object loading
and cross-package reference resolution."""

import mmap
import os
import sys

from .archive import Reader, ReadError

PACKAGE_TAG = 0x9E2A83C1
RF_HAS_STACK = 0x02000000

# File extensions in the order the stock UT2004.ini lists them.
DEFAULT_SEARCH = [
    ('System', '.u'), ('Maps', '.ut2'), ('Textures', '.utx'), ('Sounds', '.uax'),
    ('Music', '.umx'), ('StaticMeshes', '.usx'), ('Animations', '.ukx'), ('Saves', '.uvx'),
]


class Export:
    __slots__ = ('class_index', 'super_index', 'package_index', 'name', 'flags',
                 'size', 'offset')

    def __init__(self, class_index, super_index, package_index, name, flags, size, offset):
        self.class_index = class_index
        self.super_index = super_index
        self.package_index = package_index
        self.name = name
        self.flags = flags
        self.size = size
        self.offset = offset


class Import:
    __slots__ = ('class_package', 'class_name', 'package_index', 'name')

    def __init__(self, class_package, class_name, package_index, name):
        self.class_package = class_package
        self.class_name = class_name
        self.package_index = package_index
        self.name = name


class ObjRef:
    """A reference to an object as seen from a package (compact object index)."""

    __slots__ = ('pkg', 'index')

    def __init__(self, pkg, index):
        self.pkg = pkg
        self.index = index

    def __bool__(self):
        return self.index != 0

    def __repr__(self):
        return 'ObjRef(%s)' % self.path

    def __eq__(self, other):
        return isinstance(other, ObjRef) and self.path.lower() == other.path.lower()

    def __hash__(self):
        return hash(self.path.lower())

    @property
    def path(self):
        return self.pkg.ref_path(self.index)

    @property
    def name(self):
        return self.pkg.ref_name(self.index)

    @property
    def class_name(self):
        return self.pkg.ref_class_name(self.index)

    def resolve(self):
        """Return (Package, export_index) of the real object, or None."""
        return self.pkg.resolve_ref(self.index)

    def load(self):
        """Return the UObject this reference points at (loading packages as needed)."""
        res = self.resolve()
        if res is None:
            return None
        pkg, ei = res
        return pkg.get_object(ei)


class Package:
    def __init__(self, path, manager=None):
        self.path = path
        self.manager = manager
        self.name = os.path.splitext(os.path.basename(path))[0]
        self._fh = open(path, 'rb')
        size = os.fstat(self._fh.fileno()).st_size
        self.buf = mmap.mmap(self._fh.fileno(), 0, access=mmap.ACCESS_READ) if size else b''
        self._objects = {}
        self._path_index = None
        self._read_tables()

    def close(self):
        try:
            self.buf.close()
        except Exception:
            pass
        self._fh.close()

    def _read_tables(self):
        r = Reader(self.buf)
        tag = r.u32()
        if tag != PACKAGE_TAG:
            raise ReadError('%s: not an Unreal package (tag %08X)' % (self.path, tag))
        self.version = r.u16()
        self.licensee = r.u16()
        self.flags = r.u32()
        name_count, name_off = r.u32(), r.u32()
        export_count, export_off = r.u32(), r.u32()
        import_count, import_off = r.u32(), r.u32()
        if not (60 <= self.version <= 200):
            raise ReadError('%s: unsupported package version %d' % (self.path, self.version))

        r.pos = name_off
        names = []
        old_names = self.version < 64
        for _ in range(name_count):
            names.append(r.cstring() if old_names else r.fstring())
            r.u32()  # object flags of the name entry
        self.names = names

        r.pos = import_off
        imports = []
        for _ in range(import_count):
            cp = r.index()
            cn = r.index()
            pi = r.i32()
            on = r.index()
            imports.append(Import(names[cp], names[cn], pi, names[on]))
        self.imports = imports

        r.pos = export_off
        exports = []
        for _ in range(export_count):
            ci = r.index()
            si = r.index()
            pi = r.i32()
            on = r.index()
            fl = r.u32()
            sz = r.index()
            off = r.index() if sz > 0 else 0
            exports.append(Export(ci, si, pi, names[on], fl, sz, off))
        self.exports = exports

    def __repr__(self):
        return '<Package %s v%d/%d>' % (self.name, self.version, self.licensee)

    # -- naming -------------------------------------------------------------
    def name_at(self, i):
        if 0 <= i < len(self.names):
            return self.names[i]
        raise ReadError('name index %d out of range' % i)

    def ref(self, index):
        return ObjRef(self, index)

    def ref_name(self, index):
        if index < 0:
            return self.imports[-index - 1].name
        if index > 0:
            return self.exports[index - 1].name
        return 'None'

    def ref_class_name(self, index):
        if index < 0:
            return self.imports[-index - 1].class_name
        if index > 0:
            ci = self.exports[index - 1].class_index
            return 'Class' if ci == 0 else self.ref_name(ci)
        return 'None'

    def export_class_name(self, ei):
        ci = self.exports[ei].class_index
        return 'Class' if ci == 0 else self.ref_name(ci)

    def ref_path(self, index):
        """Full object path, e.g. 'Package.Group.Name'."""
        if index == 0:
            return 'None'
        parts = []
        guard = 0
        if index < 0:
            while index < 0 and guard < 64:
                imp = self.imports[-index - 1]
                parts.append(imp.name)
                index = imp.package_index
                guard += 1
            # the outermost import is the package itself
            return '.'.join(reversed(parts))
        while index > 0 and guard < 64:
            exp = self.exports[index - 1]
            parts.append(exp.name)
            index = exp.package_index
            guard += 1
        if index < 0:  # export whose outer is an import (rare)
            parts.append(self.ref_path(index))
        else:
            parts.append(self.name)
        return '.'.join(reversed(parts))

    def export_path(self, ei):
        return self.ref_path(ei + 1)

    def _build_path_index(self):
        idx = {}
        for i in range(len(self.exports)):
            p = self.export_path(i)
            # strip package name
            rel = p.split('.', 1)[1] if '.' in p else p
            idx.setdefault(rel.lower(), []).append(i)
        self._path_index = idx

    def find_export(self, rel_path, class_name=None):
        """Find an export by path relative to this package ('Group.Name')."""
        if self._path_index is None:
            self._build_path_index()
        cands = self._path_index.get(rel_path.lower(), [])
        if class_name is None:
            return cands[0] if cands else None
        cl = class_name.lower()
        for i in cands:
            if self.export_class_name(i).lower() == cl:
                return i
        # class mismatch (e.g. subclass): fall back to any candidate
        return cands[0] if cands else None

    def exports_of_class(self, *class_names):
        want = {c.lower() for c in class_names}
        return [i for i in range(len(self.exports))
                if self.export_class_name(i).lower() in want]

    # -- resolution -----------------------------------------------------------
    def resolve_ref(self, index):
        if index > 0:
            if index - 1 < len(self.exports):
                return self, index - 1
            return None
        if index == 0:
            return None
        imp = self.imports[-index - 1]
        chain = []
        i = index
        while i < 0:
            im = self.imports[-i - 1]
            chain.append(im.name)
            i = im.package_index
        chain.reverse()
        if len(chain) < 2 or self.manager is None:
            return None
        pkg = self.manager.load_package(chain[0])
        if pkg is None:
            return None
        ei = pkg.find_export('.'.join(chain[1:]), imp.class_name)
        if ei is None:
            return None
        return pkg, ei

    # -- objects ---------------------------------------------------------------
    def get_object(self, ei):
        obj = self._objects.get(ei)
        if obj is None:
            from .objects import UObject
            obj = UObject(self, ei)
            self._objects[ei] = obj
        return obj

    def find_object(self, rel_path, class_name=None):
        ei = self.find_export(rel_path, class_name)
        return None if ei is None else self.get_object(ei)

    def export_reader(self, ei):
        e = self.exports[ei]
        return Reader(self.buf, e.offset, e.offset + e.size)


class PackageManager:
    """Finds and caches packages using the game's UT2004.ini Paths."""

    def __init__(self, game_dir, verbose=False):
        self.game_dir = os.path.abspath(game_dir)
        self.verbose = verbose
        self.packages = {}
        self.missing = set()
        self.search = self._read_search_paths()
        self._file_index = self._index_files()
        self._class_cache = {}

    def _read_search_paths(self):
        system = os.path.join(self.game_dir, 'System')
        ini = None
        for cand in ('UT2004.ini', 'Default.ini'):
            p = os.path.join(system, cand)
            if os.path.isfile(p):
                ini = p
                break
        paths = []
        if ini:
            try:
                with open(ini, 'r', encoding='latin-1') as f:
                    section = None
                    for line in f:
                        line = line.strip()
                        if line.startswith('['):
                            section = line.lower()
                            continue
                        if section == '[core.system]' and line.lower().startswith('paths='):
                            spec = line.split('=', 1)[1].strip()
                            d, pat = os.path.split(spec.replace('\\', '/'))
                            ext = os.path.splitext(pat)[1].lower()
                            full = os.path.normpath(os.path.join(system, d))
                            paths.append((full, ext))
            except OSError:
                pass
        if not paths:
            paths = [(os.path.join(self.game_dir, d), e) for d, e in DEFAULT_SEARCH]
        return paths

    def _index_files(self):
        idx = {}
        for d, ext in self.search:
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                stem, e = os.path.splitext(fn)
                if e.lower() != ext:
                    continue
                idx.setdefault(stem.lower(), os.path.join(d, fn))
        return idx

    def package_files(self, ext=None):
        return {k: v for k, v in self._file_index.items()
                if ext is None or v.lower().endswith(ext)}

    def find_file(self, name):
        return self._file_index.get(name.lower())

    def load_package(self, name):
        key = name.lower()
        pkg = self.packages.get(key)
        if pkg is not None:
            return pkg
        if key in self.missing:
            return None
        path = self._file_index.get(key)
        if path is None:
            self.missing.add(key)
            if self.verbose:
                print('  [warn] package not found: %s' % name, file=sys.stderr)
            return None
        try:
            pkg = Package(path, self)
        except (ReadError, OSError) as e:
            print('  [error] cannot open %s: %s' % (path, e), file=sys.stderr)
            self.missing.add(key)
            return None
        self.packages[key] = pkg
        return pkg

    def open_file(self, path):
        """Open a package by file path and register it under its name."""
        name = os.path.splitext(os.path.basename(path))[0].lower()
        pkg = self.packages.get(name)
        if pkg is None:
            pkg = Package(path, self)
            self.packages[name] = pkg
        return pkg

    def resolve_path(self, full_path, class_name=None):
        """Resolve 'Package.Group.Name' to a UObject."""
        parts = full_path.split('.')
        if len(parts) < 2:
            return None
        pkg = self.load_package(parts[0])
        if pkg is None:
            return None
        return pkg.find_object('.'.join(parts[1:]), class_name)

    # -- class hierarchy -----------------------------------------------------
    def class_chain(self, pkg, class_index):
        """List of class names from the given class up to Object, e.g.
        ['xWeaponBase', 'xPickUpBase', 'Actor', 'Object']."""
        if class_index == 0:
            return ['Class']
        key = pkg.ref_path(class_index).lower()
        hit = self._class_cache.get(key)
        if hit is not None:
            return hit
        chain = []
        cur = (pkg, class_index)
        seen = 0
        while cur is not None and seen < 64:
            p, ci = cur
            chain.append(p.ref_name(ci))
            res = p.resolve_ref(ci)
            if res is None:
                break
            rp, ei = res
            si = rp.exports[ei].super_index
            cur = (rp, si) if si != 0 else None
            seen += 1
        self._class_cache[key] = chain
        return chain

    def close(self):
        for p in self.packages.values():
            p.close()
        self.packages.clear()
