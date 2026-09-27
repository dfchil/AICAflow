"""Versioned host-only AFX metadata; the uploaded image is unchanged."""
import json
import struct
import zlib

MAGIC = 0x314d5841  # AXM1
VERSION = 1
FLAG = 4
MAX_JSON_BYTES = 65536


def resident_size(data):
    """Return the AICA image size from a complete AFX container."""
    if len(data) < 80: raise ValueError('truncated AFX header')
    total, _, image_offset, image_size = struct.unpack_from('<4I', data, 8)
    if total != len(data) or image_offset > total or image_size > total - image_offset:
        raise ValueError('invalid AFX image bounds')
    return image_size


def stream_note_count(data, header):
    """Count NOTE commands in the resident stream, validating its wire sizes."""
    image_offset, image_bytes = header[4], header[5]
    stream_offset, stream_bytes = header[6], header[7]
    if stream_offset + stream_bytes > image_bytes:
        raise ValueError('invalid AFX stream bounds')
    stream = data[image_offset + stream_offset:image_offset + stream_offset + stream_bytes]
    cursor = notes = 0
    while cursor < len(stream):
        opcode = stream[cursor]
        if opcode in (0, 0x13):
            if cursor + 1 != len(stream): raise ValueError('AFX terminal event is not final')
            break
        if opcode == 1: size = 2
        elif opcode == 2: size = 3
        elif opcode == 3: size = 5
        elif opcode == 0x12: size = 2
        elif opcode == 0x14:
            size = 8; notes += 1
        elif opcode == 0x15: size = 4
        elif opcode in (0x10, 0x11):
            prefix = 8 if opcode == 0x10 else 6
            if cursor + prefix > len(stream): raise ValueError('truncated AFX event')
            mask = struct.unpack_from('<I', stream, cursor + prefix - 4)[0]
            if mask >> 18: raise ValueError('invalid AFX event mask')
            size = prefix + 2 * mask.bit_count()
            notes += opcode == 0x10
        else: raise ValueError(f'unsupported AFX opcode {opcode:#x}')
        if cursor + size > len(stream): raise ValueError('truncated AFX event')
        cursor += size
    return notes


def resident_layout(data):
    """Describe the bytes the linked AFX image occupies in AICA RAM."""
    image_bytes = resident_size(data)
    header = struct.unpack_from('<20I', data)
    samples_offset, sample_count = header[10], header[11]
    sample_bytes = sum(struct.unpack_from('<I', data, samples_offset + index * 16 + 4)[0]
                       for index in range(sample_count))
    setup_bytes, stream_bytes = header[9] * 36, header[7]
    if sample_bytes + setup_bytes + stream_bytes > image_bytes:
        raise ValueError('invalid AFX image layout')
    note_count = stream_note_count(data, header)
    command_bytes = stream_bytes + setup_bytes
    return {'image_bytes': image_bytes, 'sample_bytes': sample_bytes,
            'stream_bytes': stream_bytes, 'setup_bytes': setup_bytes,
            'command_bytes': command_bytes,
            'command_baseline_bytes': stream_bytes + note_count * 36,
            'padding_bytes': image_bytes - sample_bytes - command_bytes,
            'sample_count': sample_count, 'setup_count': header[9],
            'channel_count': header[16], 'note_count': note_count}


def read(data):
    if len(data) < 80: raise ValueError('truncated AFX header')
    header = struct.unpack_from('<20I', data)
    offset = header[4]
    present = 96 <= offset <= len(data) and struct.unpack_from("<I", data, offset - 16)[0] == MAGIC
    if not header[3] & FLAG and not present: return None
    if header[2] != len(data) or not 96 <= offset <= len(data): raise ValueError('invalid metadata bounds')
    magic, version, length, checksum = struct.unpack_from('<4I', data, offset - 16)
    if magic != MAGIC or version != VERSION: raise ValueError('unsupported AFX container metadata version')
    size = (length + 16 + 31) // 32 * 32
    start = offset - size
    if not 0 < length <= MAX_JSON_BYTES or start < 80: raise ValueError('invalid metadata size')
    ends = [80, header[10] + header[11] * 16, header[12] + header[13] * 12, header[14] + header[15]]
    if max(ends) > start: raise ValueError('metadata overlaps host tables')
    crc = zlib.crc32(data[:offset - 4])
    crc = zlib.crc32(bytes(4), crc)
    crc = zlib.crc32(data[offset:], crc)
    if crc != checksum: raise ValueError('AFX integrity checksum mismatch')
    value = json.loads(data[start:start + length])
    if not isinstance(value, dict): raise ValueError('metadata must be an object')
    return value


def replace(data, metadata):
    """Replace host-only metadata without changing the uploaded AFX image."""
    return attach(strip(data), metadata)


def strip(data):
    """Remove optional host-only metadata and return the canonical base AFX bytes."""
    header = list(struct.unpack_from('<20I', data))
    offset = header[4]
    if not header[3] & FLAG: return data
    magic, version, length, _ = struct.unpack_from('<4I', data, offset - 16)
    if magic != MAGIC or version != VERSION: raise ValueError('unsupported AFX container metadata version')
    size = (length + 16 + 31) // 32 * 32
    start = offset - size
    result = bytearray(data[:start] + data[offset:])
    header[2] -= size
    header[3] &= ~FLAG
    header[4] -= size
    struct.pack_into('<20I', result, 0, *header)
    return bytes(result)


def attach(data, metadata):
    if not isinstance(metadata, dict): raise ValueError('metadata must be an object')
    header = list(struct.unpack_from('<20I', data))
    if header[3] & FLAG: raise ValueError('AFX already has metadata')
    payload = json.dumps(metadata, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode()
    if not 0 < len(payload) <= MAX_JSON_BYTES: raise ValueError('metadata exceeds 64 KiB')
    size = (len(payload) + 16 + 31) // 32 * 32
    block = payload + bytes(size - 16 - len(payload)) + struct.pack('<4I', MAGIC, VERSION, len(payload), 0)
    offset = header[4]
    result = bytearray(data[:offset] + block + data[offset:])
    header[2] += size; header[4] += size; header[3] |= FLAG
    struct.pack_into('<20I', result, 0, *header)
    struct.pack_into('<I', result, header[4] - 4, zlib.crc32(result))
    return bytes(result)


if __name__ == "__main__":
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    args = parser.parse_args()
    print(json.dumps(read(args.file.read_bytes()), indent=2, ensure_ascii=False))
