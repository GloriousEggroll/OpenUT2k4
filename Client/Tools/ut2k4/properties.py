"""Generic UE1/UE2 tagged-property parsing."""

import struct

from .archive import Reader, ReadError
from .package import ObjRef

PT_BYTE, PT_INT, PT_BOOL, PT_FLOAT, PT_OBJECT, PT_NAME, PT_STRING, PT_CLASS, \
    PT_ARRAY, PT_STRUCT, PT_VECTOR, PT_ROTATOR, PT_STR, PT_MAP, PT_FIXEDARRAY = range(1, 16)

TYPE_NAMES = {
    PT_BYTE: 'byte', PT_INT: 'int', PT_BOOL: 'bool', PT_FLOAT: 'float',
    PT_OBJECT: 'object', PT_NAME: 'name', PT_STRING: 'string', PT_CLASS: 'class',
    PT_ARRAY: 'array', PT_STRUCT: 'struct', PT_VECTOR: 'vector', PT_ROTATOR: 'rotator',
    PT_STR: 'str', PT_MAP: 'map', PT_FIXEDARRAY: 'fixedarray',
}


class Vector(tuple):
    """(X, Y, Z) floats."""
    __slots__ = ()
    kind = 'Vector'


class Rotator(tuple):
    """(Pitch, Yaw, Roll) in Unreal units (65536 = 360 degrees)."""
    __slots__ = ()
    kind = 'Rotator'


# Natively serialized ("immutable") structs: name -> reader function
def _rd_color(r):
    # UE2 struct Color is stored B, G, R, A
    b, g, rr, a = r.u8(), r.u8(), r.u8(), r.u8()
    return {'R': rr, 'G': g, 'B': b, 'A': a}


def _rd_scale(r):
    v = r.vector()
    return {'Scale': Vector(v), 'SheerRate': r.f32(), 'SheerAxis': r.u8()}


def _rd_box(r):
    mn = Vector(r.vector())
    mx = Vector(r.vector())
    return {'Min': mn, 'Max': mx, 'IsValid': r.u8()}


def _rd_plane(r):
    x, y, z, w = r.plane()
    return {'X': x, 'Y': y, 'Z': z, 'W': w}


def _rd_coords(r):
    return {k: Vector(r.vector()) for k in ('Origin', 'XAxis', 'YAxis', 'ZAxis')}


def _rd_range(r):
    return {'Min': r.f32(), 'Max': r.f32()}


def _rd_rangevector(r):
    return {k: _rd_range(r) for k in ('X', 'Y', 'Z')}


def _rd_guid(r):
    return '%08X-%08X-%08X-%08X' % (r.u32(), r.u32(), r.u32(), r.u32())


def _rd_v2d(r):
    return {'X': r.f32(), 'Y': r.f32()}


NATIVE_STRUCTS = {
    'vector': (lambda r: Vector(r.vector()), 12),
    'rotator': (lambda r: Rotator(r.rotator()), 12),
    'color': (_rd_color, 4),
    'plane': (_rd_plane, 16),
    'sphere': (_rd_plane, 16),
    'quat': (_rd_plane, 16),
    'scale': (_rd_scale, 17),
    'box': (_rd_box, 25),
    'coords': (_rd_coords, 48),
    'range': (_rd_range, 8),
    'rangevector': (_rd_rangevector, 24),
    'guid': (_rd_guid, 16),
    'vector2d': (_rd_v2d, 8),
    'intpoint': (lambda r: {'X': r.i32(), 'Y': r.i32()}, 8),
}


class PropertyError(ReadError):
    pass


def _read_tag(r, pkg):
    """Returns (name, type, struct_name, size, array_index, bool_value) or None at 'None'."""
    ni = r.index()
    name = pkg.name_at(ni)
    if name == 'None':
        return None
    info = r.u8()
    ptype = info & 0x0F
    if ptype == 0:
        raise PropertyError('invalid property type 0 for %s' % name)
    is_array = bool(info & 0x80)
    struct_name = None
    if ptype == PT_STRUCT:
        struct_name = pkg.name_at(r.index())
    sc = (info >> 4) & 7
    if sc < 5:
        size = (1, 2, 4, 12, 16)[sc]
    elif sc == 5:
        size = r.u8()
    elif sc == 6:
        size = r.u16()
    else:
        size = r.i32()
    array_index = 0
    bool_value = False
    if ptype == PT_BOOL:
        bool_value = is_array
    elif is_array:
        b = r.u8()
        if b < 128:
            array_index = b
        else:
            b2 = r.u8()
            if b & 0x40:
                b3 = r.u8()
                b4 = r.u8()
                array_index = ((b << 24) | (b2 << 16) | (b3 << 8) | b4) & 0x3FFFFF
            else:
                array_index = ((b << 8) | b2) & 0x3FFF
    return name, ptype, struct_name, size, array_index, bool_value


def read_properties(r, pkg, limit=None, types=None, hints=None):
    """Read a tagged property list from reader r up to the 'None' terminator.

    Returns a dict. Static-array elements other than index 0 are stored as
    'Name[i]'. Unknown data is kept as {'_raw': size}. If `types` is a dict it
    receives property name -> (tag type, struct name). `hints` maps dynamic
    array property names to their element kind ('name', 'object', 'struct', ...)."""
    props = {}
    end_limit = r.end if limit is None else limit
    while True:
        tag = _read_tag(r, pkg)
        if tag is None:
            break
        name, ptype, sname, size, aidx, bval = tag
        start = r.pos
        if size < 0 or start + size > end_limit:
            raise PropertyError('property %s size %d overruns object' % (name, size))
        if ptype == PT_BOOL:
            value = bval
        else:
            end = start + size
            sub = Reader(r.buf, start, end)
            try:
                value = _read_value(sub, pkg, ptype, sname, size, name, hints, False)
            except (ReadError, IndexError, UnicodeDecodeError, struct.error):
                value = None
            if value is None or (isinstance(value, dict) and '_raw' in value):
                value = None
                if ptype in (PT_ARRAY, PT_STRUCT):
                    # UE2 quirk: the stored size of struct/array data is sometimes a
                    # little smaller than the data actually written.
                    sub = Reader(r.buf, start, min(end_limit, end + 64))
                    try:
                        value = _read_value(sub, pkg, ptype, sname, size, name, hints, True)
                        end = max(end, sub.pos)
                    except (ReadError, IndexError, UnicodeDecodeError, struct.error):
                        value = None
                if value is None:
                    value = {'_raw': size, '_type': TYPE_NAMES.get(ptype, ptype)}
            r.pos = end
        key = name if aidx == 0 else '%s[%d]' % (name, aidx)
        props[key] = value
        if types is not None:
            types[name] = (ptype, sname)
    return props


def _read_value(r, pkg, ptype, sname, size, pname, hints=None, relaxed=False):
    if ptype == PT_BYTE:
        return r.u8()
    if ptype == PT_INT:
        return r.i32()
    if ptype == PT_FLOAT:
        return r.f32()
    if ptype in (PT_OBJECT, PT_CLASS):
        return ObjRef(pkg, r.index())
    if ptype == PT_NAME:
        return pkg.name_at(r.index())
    if ptype in (PT_STR, PT_STRING):
        return r.fstring()
    if ptype == PT_VECTOR:
        return Vector(r.vector())
    if ptype == PT_ROTATOR:
        return Rotator(r.rotator())
    if ptype == PT_STRUCT:
        return read_struct(r, pkg, sname, size, hints, relaxed)
    if ptype == PT_ARRAY:
        return read_array(r, pkg, pname, size, hints, relaxed)
    return {'_raw': size, '_type': TYPE_NAMES.get(ptype, ptype)}


def read_struct(r, pkg, sname, size, hints=None, relaxed=False):
    nat = NATIVE_STRUCTS.get((sname or '').lower())
    if nat is not None and nat[1] == size and not relaxed:
        return nat[0](r)
    # non-native structs are stored as nested tagged property lists
    start = r.pos
    try:
        d = read_properties(r, pkg, hints=hints)
        if relaxed or r.pos == r.end or r.pos + 1 == r.end:
            d['_struct'] = sname
            return d
    except (ReadError, IndexError):
        if relaxed:
            raise
    r.pos = start
    if nat is not None:
        try:
            return nat[0](r)
        except ReadError:
            pass
    return {'_struct': sname, '_raw': size}


def _looks_like_float(u):
    if u == 0:
        return True
    e = (u >> 23) & 0xFF
    return 90 < e < 170


def read_array(r, pkg, pname, size, hints=None, relaxed=False):
    """Dynamic array. The element type is not stored in UE2 tags; it comes from
    the class declarations (hints) when available, otherwise it is inferred:
    tagged structs, compact object indices, 4-byte scalars, strings."""
    count = r.index()
    if count == 0:
        return []
    data_start = r.pos
    data_len = r.end - data_start
    if count < 0 or count > data_len:
        raise PropertyError('bad array count')
    hint = (hints or {}).get(pname) or ARRAY_HINTS.get(pname.lower())

    def try_structs():
        r.pos = data_start
        out = []
        for _ in range(count):
            out.append(read_properties(r, pkg, hints=hints))
        if relaxed or r.pos in (r.end, r.end - 1):
            return out
        raise PropertyError('struct array size mismatch')

    if relaxed:
        return try_structs()

    def try_indices(kind):
        r.pos = data_start
        out = []
        for _ in range(count):
            out.append(r.index())
        if r.pos != r.end:
            raise PropertyError('index array size mismatch')
        if kind == 'name':
            return [pkg.name_at(i) for i in out]
        nimp, nexp = len(pkg.imports), len(pkg.exports)
        for i in out:
            if not (-nimp <= i <= nexp):
                raise PropertyError('object index out of range')
        return [ObjRef(pkg, i) for i in out]

    def try_strings():
        r.pos = data_start
        out = [r.fstring() for _ in range(count)]
        if r.pos != r.end:
            raise PropertyError('string array size mismatch')
        return out

    def try_scalars(kind=None):
        if data_len != 4 * count:
            raise PropertyError('not 4-byte scalars')
        r.pos = data_start
        raw = [r.u32() for _ in range(count)]
        if kind == 'float' or (kind is None and all(_looks_like_float(u) for u in raw)):
            return [struct.unpack('<f', struct.pack('<I', u))[0] for u in raw]
        return [struct.unpack('<i', struct.pack('<I', u))[0] for u in raw]

    def try_bytes():
        if data_len != count:
            raise PropertyError('not bytes')
        r.pos = data_start
        return list(r.bytes(count))

    def try_vectors():
        if data_len != 12 * count:
            raise PropertyError('not vectors')
        r.pos = data_start
        return [Vector(r.vector()) for _ in range(count)]

    order = {
        'struct': (try_structs,),
        'object': (lambda: try_indices('object'),),
        'name': (lambda: try_indices('name'),),
        'string': (try_strings,),
        'float': (lambda: try_scalars('float'),),
        'int': (lambda: try_scalars('int'),),
        'byte': (try_bytes,),
        'bool': (try_bytes,),
        'vector': (try_vectors, try_structs),
    }.get(hint, ())
    if data_len <= 3 * count:
        order = order + (lambda: try_indices('object'), try_bytes, try_structs)
    else:
        order = order + (try_structs, lambda: try_indices('object'), try_strings,
                         try_vectors, try_scalars)
    for fn in order:
        try:
            return fn()
        except (ReadError, IndexError, UnicodeDecodeError, struct.error):
            continue
    return {'_raw': size, '_type': 'array', '_count': count}


# Element-type hints for well-known dynamic array properties.
ARRAY_HINTS = {
    'materials': None,  # StaticMesh: struct; MaterialSwitch/Sequence: object/struct -> infer
    'skins': 'object',
    'sounds': 'object',
    'decolayers': 'struct',
    'alternatepaths': 'object',
    'excludetag': 'name',
    'pathlist': 'object',
}
