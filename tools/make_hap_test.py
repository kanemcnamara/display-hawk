"""Writes small HAP test movies (QuickTime .mov) for Display Hawk's HAP player.

    python tools/make_hap_test.py <out_dir> [--frames 30] [--fps 30]

Produces hap1-snappy.mov (Hap, DXT1), hap5-chunked.mov (Hap Alpha, DXT5, chunked "complex"
frames), hapy-snappy.mov (Hap Q, scaled YCoCg DXT5) and hapm-snappy.mov (Hap Q Alpha), plus
reference-<n>.png source frames. The encoders are simple (quality is not the point); validate the
files with FFmpeg's reference HAP decoder, e.g.  ffmpeg -i hap1-snappy.mov -frames:v 1 check.png
Requires Pillow.
"""
import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw

WIDTH, HEIGHT = 634, 358  # deliberately not multiples of 4: exercises texture padding


# ---------------------------------------------------------------- test frames

def make_frame(i, count):
    img = Image.new("RGBA", (WIDTH, HEIGHT))
    d = ImageDraw.Draw(img)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255)]
    for b, c in enumerate(bars):
        d.rectangle([b * WIDTH // 7, 0, (b + 1) * WIDTH // 7 - 1, HEIGHT // 2], fill=c + (255,))
    for x in range(WIDTH):  # grey ramp
        v = x * 255 // (WIDTH - 1)
        d.line([x, HEIGHT // 2, x, HEIGHT * 3 // 4], fill=(v, v, v, 255))
    d.rectangle([0, HEIGHT * 3 // 4, WIDTH, HEIGHT], fill=(20, 20, 30, 255))
    x = int((WIDTH - 60) * i / max(1, count - 1))  # moving box shows playback
    d.rectangle([x, HEIGHT * 3 // 4 + 10, x + 60, HEIGHT - 10], fill=(255, 140, 0, 255))
    # Alpha: opaque top half, fading bottom half (only matters for the alpha variants).
    alpha = Image.new("L", (WIDTH, HEIGHT), 255)
    ad = ImageDraw.Draw(alpha)
    for y in range(HEIGHT // 2, HEIGHT):
        ad.line([0, y, WIDTH, y], fill=255 - (y - HEIGHT // 2) * 255 // (HEIGHT // 2))
    img.putalpha(alpha)
    return img


def blocks(img):
    """Yields 4x4 blocks (16 RGBA tuples) row by row, edge pixels repeated into the padding."""
    w, h = img.size
    px = img.load()
    for by in range(0, (h + 3) // 4 * 4, 4):
        for bx in range(0, (w + 3) // 4 * 4, 4):
            yield [px[min(bx + i % 4, w - 1), min(by + i // 4, h - 1)] for i in range(16)]


# ---------------------------------------------------------------- block compression

def to565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def from565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def bc1_color(block):
    """DXT1 colour block in 4-colour mode: endpoints at the luminance extremes."""
    lum = [p[0] * 2 + p[1] * 4 + p[2] for p in block]
    c0, c1 = to565(block[lum.index(max(lum))]), to565(block[lum.index(min(lum))])
    if c0 < c1:
        c0, c1 = c1, c0
    if c0 == c1:
        return struct.pack("<HHI", c0, c1, 0)
    e0, e1 = from565(c0), from565(c1)
    pal = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)), tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
    bits = 0
    for i, p in enumerate(block):
        best = min(range(4), key=lambda k: sum((p[j] - pal[k][j]) ** 2 for j in range(3)))
        bits |= best << (2 * i)
    return struct.pack("<HHI", c0, c1, bits)


def bc4_alpha(values):
    """8-value alpha block (as in DXT5 alpha and BC4)."""
    a0, a1 = max(values), min(values)
    if a0 == a1:
        return bytes([a0, a1]) + bytes(6)
    pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    bits = 0
    for i, v in enumerate(values):
        best = min(range(8), key=lambda k: abs(v - pal[k]))
        bits |= best << (3 * i)
    return bytes([a0, a1]) + bits.to_bytes(6, "little")


def encode_bc1(img):
    return b"".join(bc1_color(b) for b in blocks(img))


def encode_bc3(img):
    return b"".join(bc4_alpha([p[3] for p in b]) + bc1_color(b) for b in blocks(img))


def ycocg_image(img):
    """Scaled YCoCg with scale 1: Co, Cg around 0.5 in R, G; scale 0 in B; luma in A."""
    out = Image.new("RGBA", img.size)
    src, dst = img.load(), out.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, _ = src[x, y]
            luma = (r + 2 * g + b) / 4
            co = (r - b) / 2
            cg = (-r + 2 * g - b) / 4
            dst[x, y] = (max(0, min(255, round(co + 128))), max(0, min(255, round(cg + 128))), 0,
                         max(0, min(255, round(luma))))
    return out


def encode_bc4(img):
    return b"".join(bc4_alpha([p[3] for p in b]) for b in blocks(img))


# ---------------------------------------------------------------- Snappy

def snappy_compress(data):
    """Greedy Snappy: 4-byte hash matches become copies, everything else literals."""
    out = bytearray()
    n = len(data)
    v = n
    while True:  # varint length
        if v < 0x80:
            out.append(v)
            break
        out.append((v & 0x7F) | 0x80)
        v >>= 7

    def literal(start, end):
        while start < end:
            length = min(end - start, 1 << 16)
            m = length - 1
            if m < 60:
                out.append(m << 2)
            elif m < 256:
                out.extend(bytes([60 << 2, m]))
            else:
                out.extend(bytes([61 << 2]) + m.to_bytes(2, "little"))
            out.extend(data[start:start + length])
            start += length

    table = {}
    pos = lit_start = 0
    while pos + 4 <= n:
        key = data[pos:pos + 4]
        cand = table.get(key)
        table[key] = pos
        if cand is not None and pos - cand < 65536:
            length = 4
            while pos + length < n and data[cand + length] == data[pos + length] and length < 64:
                length += 1
            literal(lit_start, pos)
            out.append(((length - 1) << 2) | 2)  # copy with 2-byte offset
            out += (pos - cand).to_bytes(2, "little")
            pos += length
            lit_start = pos
        else:
            pos += 1
    literal(lit_start, n)
    return bytes(out)


# ---------------------------------------------------------------- HAP sections

def section(type_byte, payload):
    if len(payload) < (1 << 24):
        return len(payload).to_bytes(3, "little") + bytes([type_byte]) + payload
    return bytes(3) + bytes([type_byte]) + len(payload).to_bytes(4, "little") + payload


def hap_simple(fmt_nibble, texture):
    return section(0xB0 | fmt_nibble, snappy_compress(texture))


def hap_chunked(fmt_nibble, texture, chunks=4):
    size = (len(texture) + chunks - 1) // chunks
    parts = [texture[i:i + size] for i in range(0, len(texture), size)]
    compressed = [snappy_compress(p) if i % 2 == 0 else p for i, p in enumerate(parts)]  # mix snappy / raw
    kinds = bytes(0x0B if i % 2 == 0 else 0x0A for i in range(len(parts)))
    sizes = b"".join(len(c).to_bytes(4, "little") for c in compressed)
    instructions = section(0x01, section(0x02, kinds) + section(0x03, sizes))
    return section(0xC0 | fmt_nibble, instructions + b"".join(compressed))


# ---------------------------------------------------------------- QuickTime writer

def atom(kind, payload):
    return struct.pack(">I", 8 + len(payload)) + kind + payload


MATRIX = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)


def write_mov(path, fourcc, frames, fps, depth):
    timescale, delta = fps * 100, 100
    duration = len(frames) * delta
    ftyp = atom(b"ftyp", b"qt  " + struct.pack(">I", 0x200) + b"qt  ")
    mdat_payload = b"".join(frames)
    first = len(ftyp) + 8
    offsets, pos = [], first
    for f in frames:
        offsets.append(pos)
        pos += len(f)
    mdat = atom(b"mdat", mdat_payload)

    mvhd = atom(b"mvhd", struct.pack(">IIIII", 0, 0, 0, timescale, duration) + struct.pack(">IH", 0x10000, 0x100)
                + bytes(10) + MATRIX + bytes(24) + struct.pack(">I", 2))
    tkhd = atom(b"tkhd", struct.pack(">IIIIII", 0x0000000F, 0, 0, 1, 0, duration) + bytes(8)
                + struct.pack(">HHHH", 0, 0, 0, 0) + MATRIX + struct.pack(">II", WIDTH << 16, HEIGHT << 16))
    mdhd = atom(b"mdhd", struct.pack(">IIIIIHH", 0, 0, 0, timescale, duration, 0, 0))
    hdlr = atom(b"hdlr", struct.pack(">I", 0) + b"mhlr" + b"vide" + bytes(12) + b"\x0cVideoHandler")
    vmhd = atom(b"vmhd", struct.pack(">IHHHH", 1, 0x40, 0x8000, 0x8000, 0x8000))
    dinf = atom(b"dinf", atom(b"dref", struct.pack(">II", 0, 1) + struct.pack(">I", 12) + b"alis" + struct.pack(">I", 1)))
    name = b"Hap"
    entry_body = (bytes(6) + struct.pack(">H", 1) + struct.pack(">HH", 0, 0) + b"DHwk" + struct.pack(">II", 0, 512)
                  + struct.pack(">HH", WIDTH, HEIGHT) + struct.pack(">II", 72 << 16, 72 << 16) + struct.pack(">IH", 0, 1)
                  + bytes([len(name)]) + name + bytes(31 - len(name)) + struct.pack(">Hh", depth, -1))
    stsd = atom(b"stsd", struct.pack(">II", 0, 1) + struct.pack(">I", 8 + len(entry_body)) + fourcc + entry_body)
    stts = atom(b"stts", struct.pack(">IIII", 0, 1, len(frames), delta))
    stsc = atom(b"stsc", struct.pack(">IIIII", 0, 1, 1, 1, 1))
    stsz = atom(b"stsz", struct.pack(">III", 0, 0, len(frames)) + b"".join(struct.pack(">I", len(f)) for f in frames))
    stco = atom(b"stco", struct.pack(">II", 0, len(frames)) + b"".join(struct.pack(">I", o) for o in offsets))
    stbl = atom(b"stbl", stsd + stts + stsc + stsz + stco)
    minf = atom(b"minf", vmhd + hdlr + dinf + stbl)
    mdia = atom(b"mdia", mdhd + hdlr + minf)
    moov = atom(b"moov", mvhd + atom(b"trak", tkhd + mdia))
    Path(path).write_bytes(ftyp + mdat + moov)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--fps", type=int, default=30)
    args = ap.parse_args()
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)

    sources = [make_frame(i, args.frames) for i in range(args.frames)]
    for i in (0, args.frames // 2):
        sources[i].save(out / f"reference-{i}.png")

    variants = {
        "hap1-snappy.mov": (b"Hap1", 24, lambda im: hap_simple(0x0B, encode_bc1(im.convert("RGB").convert("RGBA")))),
        "hap5-chunked.mov": (b"Hap5", 32, lambda im: hap_chunked(0x0E, encode_bc3(im))),
        "hapy-snappy.mov": (b"HapY", 24, lambda im: hap_simple(0x0F, encode_bc3(ycocg_image(im)))),
        "hapm-snappy.mov": (b"HapM", 32, lambda im: section(0x0D, hap_simple(0x0F, encode_bc3(ycocg_image(im)))
                                                            + hap_simple(0x01, encode_bc4(im)))),
    }
    for name, (fourcc, depth, encode) in variants.items():
        frames = [encode(im) for im in sources]
        write_mov(out / name, fourcc, frames, args.fps, depth)
        print(f"{name}: {len(frames)} frames, {sum(map(len, frames)) // len(frames)} bytes/frame")


if __name__ == "__main__":
    main()
