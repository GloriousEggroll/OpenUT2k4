"""Minimal PNG writer (zlib only)."""

import struct
import zlib

import numpy as np


def _chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, img, level=6):
    """img: numpy array HxW (gray), HxWx3 (RGB) or HxWx4 (RGBA); uint8 or uint16."""
    img = np.ascontiguousarray(img)
    if img.ndim == 2:
        h, w = img.shape
        ch = 1
    else:
        h, w, ch = img.shape
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}[ch]
    if img.dtype == np.uint16:
        depth = 16
        raw = img.astype('>u2').reshape(h, w * ch).view(np.uint8)
    else:
        depth = 8
        raw = img.astype(np.uint8).reshape(h, w * ch)
    rows = np.zeros((h, raw.shape[1] + 1), dtype=np.uint8)  # filter type 0 per row
    rows[:, 1:] = raw
    ihdr = struct.pack('>IIBBBBB', w, h, depth, color_type, 0, 0, 0)
    data = (b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', ihdr)
            + _chunk(b'IDAT', zlib.compress(rows.tobytes(), level)) + _chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(data)
    return len(data)


def read_png_info(path):
    """(width, height, bit_depth, color_type) from the IHDR chunk."""
    with open(path, 'rb') as f:
        head = f.read(33)
    if head[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    w, h, depth, ct = struct.unpack('>IIBB', head[16:26])
    return w, h, depth, ct
