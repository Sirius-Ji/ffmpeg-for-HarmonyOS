import zlib, struct
w = h = 96
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        dx, dy = x - 48, y - 48
        d = (dx * dx + dy * dy) ** 0.5
        if d < 40:
            row += bytes((220, 40, 40, 210))
        elif d < 46:
            row += bytes((255, 255, 255, 255))
        else:
            row += bytes((0, 0, 0, 0))
    rows.append(bytes(row))
raw = b''.join(rows)
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(r'harmony/entry/src/main/resources/rawfile/logo.png', 'wb').write(png)
print('logo png', len(png))
