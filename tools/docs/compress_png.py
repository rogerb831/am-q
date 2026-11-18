"""Recompresses PNG screenshots in place (no third-party modules).

The editor's screenshot path writes large, barely compressed RGBA PNGs. This decodes each file,
drops the alpha channel when every pixel is opaque, applies the PNG "Sub" or "Up" row filter
(whichever deflates smaller for the row, measured on the row alone), and writes one zlib stream at
level 9. The pixels are unchanged.

    python tools/docs/compress_png.py <file.png> [...]
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        n = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat.append(body)
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f'{path}: only 8-bit RGB or RGBA, not interlaced')
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b, c = prev[i], prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def write_png(path, w, h, bpp, rows):
    opaque = bpp == 4 and all(all(v == 255 for v in r[3::4]) for r in rows)
    if opaque:
        rows = [bytearray(b for i, b in enumerate(r) if i % 4 != 3) for r in rows]
        bpp = 3
    out = bytearray()
    prev = bytearray(len(rows[0]))
    for line in rows:
        sub = bytearray(line)
        for i in range(len(line) - 1, bpp - 1, -1):
            sub[i] = (line[i] - line[i - bpp]) & 0xFF
        up = bytearray((a - b) & 0xFF for a, b in zip(line, prev))
        best = min(((0, line), (1, sub), (2, up)), key=lambda t: len(zlib.compress(bytes(t[1]), 1)))
        out.append(best[0])
        out += best[1]
        prev = line

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6 if bpp == 4 else 2, 0, 0, 0)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(bytes(out), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


for p in sys.argv[1:]:
    before = len(open(p, 'rb').read())
    write_png(p, *read_png(p))
    after = len(open(p, 'rb').read())
    print(f'{p}: {before // 1024} KB -> {after // 1024} KB')
