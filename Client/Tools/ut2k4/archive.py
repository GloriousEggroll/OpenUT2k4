"""Low-level binary reader for Unreal Engine 2.x package data."""

import struct

_U8 = struct.Struct('<B')
_I16 = struct.Struct('<h')
_U16 = struct.Struct('<H')
_I32 = struct.Struct('<i')
_U32 = struct.Struct('<I')
_U64 = struct.Struct('<Q')
_F32 = struct.Struct('<f')
_VEC = struct.Struct('<3f')
_ROT = struct.Struct('<3i')
_PLANE = struct.Struct('<4f')


class ReadError(Exception):
    pass


class Reader:
    """Cursor over a bytes-like buffer (bytes, memoryview or mmap)."""

    __slots__ = ('buf', 'pos', 'end')

    def __init__(self, buf, pos=0, end=None):
        self.buf = buf
        self.pos = pos
        self.end = len(buf) if end is None else end

    # -- primitives -------------------------------------------------------
    def _check(self, n):
        if self.pos + n > self.end:
            raise ReadError('read past end (pos=%d need=%d end=%d)' % (self.pos, n, self.end))

    def remaining(self):
        return self.end - self.pos

    def bytes(self, n):
        if n < 0:
            raise ReadError('negative length %d' % n)
        self._check(n)
        p = self.pos
        self.pos = p + n
        return bytes(self.buf[p:p + n])

    def skip(self, n):
        self._check(n)
        self.pos += n

    def u8(self):
        self._check(1)
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def _unpack(self, st):
        self._check(st.size)
        v = st.unpack_from(self.buf, self.pos)
        self.pos += st.size
        return v

    def i16(self):
        return self._unpack(_I16)[0]

    def u16(self):
        return self._unpack(_U16)[0]

    def i32(self):
        return self._unpack(_I32)[0]

    def u32(self):
        return self._unpack(_U32)[0]

    def u64(self):
        return self._unpack(_U64)[0]

    def f32(self):
        return self._unpack(_F32)[0]

    def vector(self):
        return self._unpack(_VEC)

    def rotator(self):
        return self._unpack(_ROT)

    def plane(self):
        return self._unpack(_PLANE)

    def index(self):
        """FCompactIndex: sign + continuation in the first byte, then 7-bit groups."""
        buf = self.buf
        p = self.pos
        if p >= self.end:
            raise ReadError('compact index past end')
        b0 = buf[p]
        p += 1
        v = b0 & 0x3F
        if b0 & 0x40:
            shift = 6
            while True:
                if p >= self.end:
                    raise ReadError('compact index past end')
                b = buf[p]
                p += 1
                v |= (b & 0x7F) << shift
                shift += 7
                if not (b & 0x80) or shift > 27:
                    break
        self.pos = p
        return -v if b0 & 0x80 else v

    def fstring(self):
        n = self.index()
        if n == 0:
            return ''
        if n > 0:
            if n > 1 << 20:
                raise ReadError('string too long')
            raw = self.bytes(n)
            return raw.rstrip(b'\0').decode('latin-1')
        n = -n
        if n > 1 << 20:
            raise ReadError('string too long')
        raw = self.bytes(n * 2)
        return raw.decode('utf-16-le', 'replace').rstrip('\0')

    def box(self):
        mn = self.vector()
        mx = self.vector()
        valid = self.u8()
        return {'Min': mn, 'Max': mx, 'IsValid': valid}

    def sphere(self):
        return self.plane()

    def cstring(self):
        """Zero-terminated string (name table of packages older than version 64)."""
        p = self.pos
        e = self.buf.find(b'\0', p, self.end)  # bytes and mmap both support find
        if e < 0:
            raise ReadError('unterminated string')
        self.pos = e + 1
        return bytes(self.buf[p:e]).decode('latin-1')

    def lazy_bytes(self, has_skip=True):
        """TLazyArray<BYTE>: int32 skip position (package version > 61), then TArray<BYTE>."""
        if has_skip:
            self.i32()
        n = self.index()
        return self.bytes(n)

    def lazy_bytes_span(self, has_skip=True):
        """Like lazy_bytes but returns (offset, length) without copying."""
        if has_skip:
            self.i32()
        n = self.index()
        self._check(n)
        p = self.pos
        self.pos += n
        return p, n
