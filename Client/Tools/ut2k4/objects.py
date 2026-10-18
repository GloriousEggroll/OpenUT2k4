"""Lazily loaded UObject wrapper plus class-default lookup."""

import sys

from .archive import Reader, ReadError
from .package import RF_HAS_STACK
from .properties import read_properties, PropertyError


class UObject:
    __slots__ = ('pkg', 'ei', '_props', '_native_pos', '_error', '_chain')

    def __init__(self, pkg, ei):
        self.pkg = pkg
        self.ei = ei
        self._props = None
        self._native_pos = None
        self._error = None
        self._chain = None

    def __repr__(self):
        return '<%s %s>' % (self.class_name, self.path)

    @property
    def export(self):
        return self.pkg.exports[self.ei]

    @property
    def name(self):
        return self.export.name

    @property
    def class_name(self):
        return self.pkg.export_class_name(self.ei)

    @property
    def path(self):
        return self.pkg.export_path(self.ei)

    @property
    def class_chain(self):
        """Class names from the object's class up to Object."""
        if self._chain is None:
            ci = self.export.class_index
            mgr = self.pkg.manager
            if mgr is None or ci == 0:
                self._chain = [self.class_name]
            else:
                self._chain = mgr.class_chain(self.pkg, ci)
        return self._chain

    def is_a(self, *class_names):
        want = {c.lower() for c in class_names}
        return any(c.lower() in want for c in self.class_chain)

    def reader(self):
        return self.pkg.export_reader(self.ei)

    def _load(self):
        r = self.reader()
        try:
            if self.export.flags & RF_HAS_STACK:
                node = r.index()
                r.index()        # StateNode
                r.u64()          # ProbeMask
                r.i32()          # LatentAction
                if node != 0:
                    r.index()    # code offset
            self._props = read_properties(r, self.pkg, hints=self._hints())
            self._native_pos = r.pos
        except (ReadError, IndexError) as e:
            self._props = {}
            self._native_pos = None
            self._error = str(e)

    def _hints(self):
        ci = self.export.class_index
        if ci == 0 or self.pkg.manager is None:
            return None
        try:
            res = self.pkg.resolve_ref(ci)
        except (ReadError, IndexError):
            return None
        if res is None:
            return None
        return array_hints(res[0], res[1])

    @property
    def props(self):
        if self._props is None:
            self._load()
        return self._props

    @property
    def error(self):
        if self._props is None:
            self._load()
        return self._error

    def native_reader(self):
        """Reader positioned right after the tagged properties (native data)."""
        if self._props is None:
            self._load()
        if self._native_pos is None:
            raise ReadError('properties of %s could not be parsed: %s' % (self.path, self._error))
        r = self.reader()
        r.pos = self._native_pos
        return r

    def get(self, name, default=None, with_defaults=True):
        """Property value, falling back to the class defaults."""
        p = self.props
        if name in p:
            return p[name]
        if with_defaults and self.pkg.manager is not None:
            d = class_defaults(self.pkg.manager, self.pkg, self.export.class_index)
            if name in d:
                return d[name]
        return default


# ---------------------------------------------------------------------------
# Class defaults
#
# UClass exports end with the class default object's tagged properties.
# Parsing the preceding UStruct/UState/UClass data requires decoding script
# bytecode, so instead the defaults block is located by scanning for the
# earliest offset from which a complete tagged-property list ends exactly at
# the end of the export.
# ---------------------------------------------------------------------------

_defaults_cache = {}
_declared_cache = {}

# tag type -> UProperty class names it may belong to
_TAG_PROP_CLASSES = {
    1: ('ByteProperty',), 2: ('IntProperty',), 3: ('BoolProperty',), 4: ('FloatProperty',),
    5: ('ObjectProperty', 'ClassProperty'), 6: ('NameProperty',), 7: ('StringProperty',),
    8: ('ClassProperty', 'ObjectProperty'), 9: ('ArrayProperty',), 10: ('StructProperty',),
    11: ('StructProperty',), 12: ('StructProperty',), 13: ('StrProperty',),
    14: ('MapProperty',), 15: ('FixedArrayProperty',),
}


def _children_index(pkg):
    idx = getattr(pkg, '_children_idx', None)
    if idx is None:
        idx = {}
        for i, e in enumerate(pkg.exports):
            if e.package_index > 0:
                idx.setdefault(e.package_index - 1, []).append(i)
        pkg._children_idx = idx
    return idx


def declared_properties(pkg, ei, depth=0):
    """name -> UProperty class name for a class export and all of its superclasses."""
    key = (pkg.path, ei)
    hit = _declared_cache.get(key)
    if hit is not None:
        return hit
    out = {}
    e = pkg.exports[ei]
    if e.super_index != 0 and depth < 64:
        res = pkg.resolve_ref(e.super_index)
        if res is not None:
            out.update(declared_properties(res[0], res[1], depth + 1))
    for ci in _children_index(pkg).get(ei, ()):
        cn = pkg.export_class_name(ci)
        if cn.endswith('Property'):
            out[pkg.exports[ci].name] = cn
    _declared_cache[key] = out
    return out


_PROP_KIND = {
    'NameProperty': 'name', 'ObjectProperty': 'object', 'ClassProperty': 'object',
    'StructProperty': 'struct', 'StrProperty': 'string', 'IntProperty': 'int',
    'FloatProperty': 'float', 'ByteProperty': 'byte', 'BoolProperty': 'bool',
}
_hints_cache = {}


def _struct_of(pkg, prop_ei):
    """Struct referenced by a StructProperty export -> (pkg, export index) or None.

    Layout (UE2): tagged props ('None'), UField SuperField + Next, int32 ArrayDim,
    uint32 PropertyFlags, FName Category, [uint16 RepOffset if CPF_Net], Struct."""
    try:
        r = pkg.export_reader(prop_ei)
        read_properties(r, pkg)
        r.index()
        r.index()
        r.i32()
        flags = r.u32()
        r.index()
        if flags & 0x20:
            r.u16()
        ref = r.index()
        if ref == 0:
            return None
        res = pkg.resolve_ref(ref)
        if res is None or res[0].export_class_name(res[1]) != 'Struct':
            return None
        return res
    except (ReadError, IndexError):
        return None


def array_hints(pkg, ei, depth=0):
    """Dynamic-array property name -> element kind, for a class or struct
    (incl. superclasses and the structs its properties use)."""
    key = (pkg.path, ei)
    hit = _hints_cache.get(key)
    if hit is not None:
        return hit
    out = {}
    _hints_cache[key] = out
    e = pkg.exports[ei]
    if e.super_index != 0 and depth < 64:
        try:
            res = pkg.resolve_ref(e.super_index)
        except (ReadError, IndexError):
            res = None
        if res is not None:
            out.update(array_hints(res[0], res[1], depth + 1))
    kids = _children_index(pkg)
    for ci in kids.get(ei, ()):
        cn = pkg.export_class_name(ci)
        if cn == 'ArrayProperty':
            for ii in kids.get(ci, ()):
                icn = pkg.export_class_name(ii)
                kind = _PROP_KIND.get(icn)
                if kind:
                    out.setdefault(pkg.exports[ci].name, kind)
                    if icn == 'StructProperty' and depth < 64:
                        st = _struct_of(pkg, ii)
                        if st is not None:
                            for k, v in array_hints(st[0], st[1], depth + 1).items():
                                out.setdefault(k, v)
                    break
        elif cn == 'StructProperty' and depth < 64:
            st = _struct_of(pkg, ci)
            if st is not None:
                for k, v in array_hints(st[0], st[1], depth + 1).items():
                    out.setdefault(k, v)
    return out


def _props_match_declared(types, declared):
    for name, (ptype, _sname) in types.items():
        pc = declared.get(name)
        if pc is None or pc not in _TAG_PROP_CLASSES.get(ptype, ()):
            return False
    return True




def _scan_defaults(pkg, ei):
    e = pkg.exports[ei]
    end = e.offset + e.size
    nnames = len(pkg.names)
    none_idx = None
    for i, n in enumerate(pkg.names):
        if n == 'None':
            none_idx = i
            break
    buf = pkg.buf
    declared = declared_properties(pkg, ei)
    hints = array_hints(pkg, ei)
    start = e.offset
    best = None
    pos = start
    while pos < end:
        b0 = buf[pos]
        # candidate tag name must be a valid, plausible name index
        try:
            r = Reader(buf, pos, end)
            ni = r.index()
            if not (0 <= ni < nnames):
                pos += 1
                continue
            if ni == none_idx:
                if r.pos == end:
                    best = {}
                    break
                pos += 1
                continue
            nm = pkg.names[ni]
            if not nm or not (nm[0].isalpha() or nm[0] == '_'):
                pos += 1
                continue
            info = buf[r.pos] if r.pos < end else 0
            if info & 0x0F == 0:
                pos += 1
                continue
            r.pos = pos
            types = {}
            props = read_properties(r, pkg, types=types, hints=hints)
            if r.pos == end and _props_match_declared(types, declared):
                best = props
                break
        except (ReadError, IndexError, PropertyError, UnicodeDecodeError, ValueError):
            pass
        pos += 1
    return best if best is not None else {}


def class_defaults(mgr, pkg, class_index):
    """Merged default properties of a class and its superclasses."""
    if class_index == 0:
        return {}
    key = pkg.ref_path(class_index).lower()
    hit = _defaults_cache.get(key)
    if hit is not None:
        return hit
    _defaults_cache[key] = {}  # recursion guard
    res = pkg.resolve_ref(class_index)
    merged = {}
    if res is not None:
        cpkg, cei = res
        si = cpkg.exports[cei].super_index
        if si != 0:
            merged.update(class_defaults(mgr, cpkg, si))
        try:
            own = _scan_defaults(cpkg, cei)
        except Exception as ex:  # never abort on defaults
            print('  [warn] defaults of %s: %s' % (key, ex), file=sys.stderr)
            own = {}
        merged.update(own)
    _defaults_cache[key] = merged
    return merged
