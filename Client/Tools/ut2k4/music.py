"""Music/*.ogg passthrough: Ogg Vorbis header parsing for the json sidecar."""

import struct


def _pages(data):
    pos = 0
    n = len(data)
    while pos + 27 <= n:
        if data[pos:pos + 4] != b'OggS':
            nxt = data.find(b'OggS', pos + 1)
            if nxt < 0:
                return
            pos = nxt
            continue
        _ver, htype, granule, serial, seq, _crc, nseg = struct.unpack_from('<BBqIIIB', data, pos + 4)
        segs = data[pos + 27:pos + 27 + nseg]
        body = pos + 27 + nseg
        size = sum(segs)
        yield htype, granule, serial, data[body:body + size], segs
        pos = body + size


def ogg_info(data):
    """-> dict(codec, channels, sample_rate, bitrate_nominal, duration, vendor, comments)."""
    info = {'codec': None}
    packets = []
    cur = b''
    last_granule = 0
    for htype, granule, _serial, body, segs in _pages(data):
        if granule >= 0:
            last_granule = max(last_granule, granule)
        if len(packets) < 2:
            off = 0
            for s in segs:
                cur += body[off:off + s]
                off += s
                if s < 255:
                    packets.append(cur)
                    cur = b''
                    if len(packets) >= 2:
                        break
    if packets and packets[0][:7] == b'\x01vorbis':
        p = packets[0]
        _v, ch, rate, bmax, bnom, bmin = struct.unpack_from('<IBIiii', p, 7)
        info.update({'codec': 'vorbis', 'channels': ch, 'sample_rate': rate,
                     'bitrate_nominal': bnom if bnom > 0 else None})
        if rate:
            info['duration'] = round(last_granule / float(rate), 3)
    elif packets and packets[0][:8] == b'OpusHead':
        info.update({'codec': 'opus', 'channels': packets[0][9], 'sample_rate': 48000})
        info['duration'] = round(last_granule / 48000.0, 3)
    if len(packets) > 1 and packets[1][:7] == b'\x03vorbis':
        p = packets[1]
        try:
            vl = struct.unpack_from('<I', p, 7)[0]
            info['vendor'] = p[11:11 + vl].decode('utf-8', 'replace')
            pos = 11 + vl
            n = struct.unpack_from('<I', p, pos)[0]
            pos += 4
            comments = {}
            for _ in range(min(n, 256)):
                ln = struct.unpack_from('<I', p, pos)[0]
                pos += 4
                kv = p[pos:pos + ln].decode('utf-8', 'replace')
                pos += ln
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    comments.setdefault(k.upper(), v)
            info['comments'] = comments
        except struct.error:
            pass
    return info
