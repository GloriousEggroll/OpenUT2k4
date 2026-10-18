"""UE2 Sound objects: the payload is the original file (normally RIFF/WAVE or Ogg)."""

import struct


class SoundError(Exception):
    pass


def read_sound(obj):
    """Returns (format_name, data bytes)."""
    r = obj.native_reader()
    fmt = obj.pkg.name_at(r.index())
    if obj.pkg.licensee >= 2:
        r.f32()
    data = r.lazy_bytes(obj.pkg.version > 61)
    return fmt, data


def sound_kind(data):
    if data[:4] == b'RIFF' and data[8:12] == b'WAVE':
        return 'wav'
    if data[:4] == b'OggS':
        return 'ogg'
    if data[:3] == b'ID3' or data[:2] in (b'\xff\xfb', b'\xff\xf3'):
        return 'mp3'
    return None


WAVE_FORMATS = {1: 'PCM', 2: 'MS-ADPCM', 3: 'IEEE_FLOAT', 0x11: 'IMA-ADPCM', 0x55: 'MP3'}


def wav_info(data):
    """Parse the fmt/data chunks of a RIFF WAVE file."""
    info = {}
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        if cid == b'fmt ' and size >= 16:
            tag, ch, rate, _bps, _align, bits = struct.unpack_from('<HHIIHH', data, body)
            info.update({'codec': WAVE_FORMATS.get(tag, '0x%04X' % tag), 'format_tag': tag,
                         'channels': ch, 'sample_rate': rate, 'bits_per_sample': bits})
        elif cid == b'data':
            info['data_bytes'] = min(size, len(data) - body)
        elif cid == b'smpl' and size >= 36:
            nloops = struct.unpack_from('<I', data, body + 28)[0]
            if nloops and size >= 36 + 24:
                start, end = struct.unpack_from('<II', data, body + 36 + 8)
                info['loop'] = {'start_sample': start, 'end_sample': end}
        pos = body + size + (size & 1)
    if info.get('codec') == 'PCM' and info.get('data_bytes') and info.get('channels') \
            and info.get('bits_per_sample') and info.get('sample_rate'):
        frames = info['data_bytes'] // (info['channels'] * info['bits_per_sample'] // 8)
        info['duration'] = round(frames / float(info['sample_rate']), 4)
    return info
