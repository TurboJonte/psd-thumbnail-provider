"""Generates synthetic PSD/PSB files covering colour modes and depths the
checked-in corpus does not exercise, plus large files for benchmarking."""

import struct
import os
import sys


def packbits(data):
    """PackBits/RLE encode one row."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        # look for a run of 3+
        run = 1
        while i + run < n and run < 128 and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out.append(257 - run)
            out.append(data[i])
            i += run
        else:
            start = i
            lit = 0
            while i < n and lit < 128:
                if i + 2 < n and data[i] == data[i + 1] == data[i + 2]:
                    break
                i += 1
                lit += 1
            out.append(lit - 1)
            out += data[start:start + lit]
    return bytes(out)


def make_channel(w, h, ci, nch, depth, mode):
    """Procedural content: gradients plus blocks, so it is neither flat nor noise."""
    bpc = depth // 8
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            gx = (x * 255) // max(1, w - 1)
            gy = (y * 255) // max(1, h - 1)
            block = 40 if ((x // 64) + (y // 64)) % 2 else 0
            if mode == 2:  # indexed -> palette index
                v = (gx // 16) + (gy // 16) * 16
            elif ci == 0:
                v = min(255, gx + block)
            elif ci == 1:
                v = min(255, gy + block)
            elif ci == 2:
                v = min(255, (gx + gy) // 2 + block)
            else:
                v = 255 - min(255, (gx + gy) // 2)
            v &= 0xFF
            if bpc == 1:
                row.append(v)
            elif bpc == 2:
                # 16-bit: replicate into the high byte
                row += bytes((v, v))
            else:
                # 32-bit: big-endian linear-light float
                lin = v / 255.0
                lin = lin / 12.92 if lin <= 0.04045 else ((lin + 0.055) / 1.055) ** 2.4
                row += struct.pack('>f', lin)
        rows.append(bytes(row))
    return rows


def write_psd(path, w, h, nch, depth, mode, version=1, compression=1,
              palette=None, thumb=None):
    sig_ver = struct.pack('>4sH6xHIIHH', b'8BPS', version, nch, h, w, depth, mode)

    color_mode_data = palette if palette else b''
    body = struct.pack('>I', len(color_mode_data)) + color_mode_data

    # image resources
    res = b''
    if thumb:
        jpeg, tw, th = thumb
        widthbytes = (tw * 24 + 31) // 32 * 4
        block = struct.pack('>IIIIIIHH', 1, tw, th, widthbytes,
                            widthbytes * th, len(jpeg), 24, 1) + jpeg
        if len(block) % 2:
            block += b'\x00'
        res += b'8BIM' + struct.pack('>H', 1036) + b'\x00\x00' + \
            struct.pack('>I', len(block)) + block
    body += struct.pack('>I', len(res)) + res

    # layer and mask info (empty)
    body += struct.pack('>I', 0) if version == 1 else struct.pack('>Q', 0)

    # image data
    body += struct.pack('>H', compression)
    planes = [make_channel(w, h, c, nch, depth, mode) for c in range(nch)]

    if compression == 0:
        for rows in planes:
            for r in rows:
                body += r
    else:
        counts = []
        payload = bytearray()
        for rows in planes:
            for r in rows:
                enc = packbits(r)
                counts.append(len(enc))
                payload += enc
        fmt = '>%dH' % len(counts) if version == 1 else '>%dI' % len(counts)
        body += struct.pack(fmt, *counts) + bytes(payload)

    with open(path, 'wb') as f:
        f.write(sig_ver + body)
    return os.path.getsize(path)


def main():
    pal = bytearray(768)
    for i in range(256):
        pal[i] = (i * 7) % 256
        pal[256 + i] = (i * 13) % 256
        pal[512 + i] = (i * 29) % 256

    jobs = [
        ('gen_rgb8.psd',    256, 192, 3, 8,  3, 1, 1, None),
        ('gen_rgba8.psd',   256, 192, 4, 8,  3, 1, 1, None),
        ('gen_gray8.psd',   256, 192, 1, 8,  1, 1, 1, None),
        ('gen_cmyk8.psd',   256, 192, 4, 8,  4, 1, 1, None),
        ('gen_indexed.psd', 256, 192, 1, 8,  2, 1, 1, bytes(pal)),
        ('gen_rgb16.psd',   256, 192, 3, 16, 3, 1, 1, None),
        ('gen_raw8.psd',    256, 192, 3, 8,  3, 1, 0, None),
        ('gen_float32.psd', 256, 192, 3, 32, 3, 1, 1, None),
        ('gen_psb16.psb',   300, 200, 3, 16, 3, 2, 1, None),
        # large files for benchmarking the row-skipping path
        ('gen_big_rle.psd', 6000, 4000, 3, 8, 3, 1, 1, None),
        ('gen_big_raw.psd', 4000, 3000, 3, 8, 3, 1, 0, None),
    ]

    only = sys.argv[1] if len(sys.argv) > 1 else None
    for name, w, h, nch, depth, mode, ver, comp, pl in jobs:
        if only and only not in name:
            continue
        size = write_psd(name, w, h, nch, depth, mode, ver, comp, pl)
        print('%-18s %5dx%-5d ch=%d depth=%2d mode=%d comp=%d  %8.1f KB'
              % (name, w, h, nch, depth, mode, comp, size / 1024.0))


if __name__ == '__main__':
    main()
