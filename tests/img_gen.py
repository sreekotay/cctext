#!/usr/bin/env python3
"""Write the image fixtures under testdata/img (committed; rerun to regenerate).

Needs Pillow. Every file is small: the loader smoke (tests/img_smoke.ccs)
decodes each one and checks sizes, colours and orientation.
"""
import os
import struct
import sys
import zlib

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "testdata", "img")

# Quadrant colours: top-left red, top-right green, bottom-left blue,
# bottom-right white. Asymmetric, so every EXIF orientation is distinct.
TL, TR, BL, BR = (220, 30, 30), (30, 200, 40), (40, 60, 220), (250, 250, 250)


def quad(w, h, alpha=False):
    im = Image.new("RGBA" if alpha else "RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            c = (TL if x < w // 2 else TR) if y < h // 2 else (BL if x < w // 2 else BR)
            px[x, y] = c + ((255,) if alpha else ())
    if alpha:
        px[0, 0] = (0, 0, 0, 0)
    return im


def exif_bytes(orient):
    # Minimal little-endian TIFF: IFD0 with one SHORT entry 0x0112.
    tiff = b"II*\x00" + struct.pack("<I", 8)
    tiff += struct.pack("<H", 1) + struct.pack("<HHIHH", 0x0112, 3, 1, orient, 0)
    tiff += struct.pack("<I", 0)
    return b"Exif\x00\x00" + tiff


INV = {
    1: None,
    2: Image.Transpose.FLIP_LEFT_RIGHT,
    3: Image.Transpose.ROTATE_180,
    4: Image.Transpose.FLIP_TOP_BOTTOM,
    5: Image.Transpose.TRANSPOSE,
    6: Image.Transpose.ROTATE_90,   # inverse of ROTATE_270
    7: Image.Transpose.TRANSVERSE,
    8: Image.Transpose.ROTATE_270,  # inverse of ROTATE_90
}


def png_header_only(w, h):
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    chunk = struct.pack(">I", len(ihdr)) + b"IHDR" + ihdr
    chunk += struct.pack(">I", zlib.crc32(b"IHDR" + ihdr) & 0xFFFFFFFF)
    # A token IDAT so a decoder reads the header as complete: the size is
    # what the limits must refuse, before any pixel buffer exists.
    idat = zlib.compress(b"\x00" * 64)
    chunk += struct.pack(">I", len(idat)) + b"IDAT" + idat
    chunk += struct.pack(">I", zlib.crc32(b"IDAT" + idat) & 0xFFFFFFFF)
    chunk += struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND") & 0xFFFFFFFF)
    return sig + chunk


def main():
    os.makedirs(OUT, exist_ok=True)
    q = quad(32, 24)
    qa = quad(32, 24, alpha=True)
    qa.save(os.path.join(OUT, "quad.png"))
    q.save(os.path.join(OUT, "quad.jpg"), quality=95)
    q.convert("P", palette=Image.Palette.ADAPTIVE, colors=8).save(os.path.join(OUT, "quad.gif"))
    q.save(os.path.join(OUT, "quad.bmp"))
    qa.save(os.path.join(OUT, "quad.webp"), lossless=True)
    qa.save(os.path.join(OUT, "quad.qoi"))
    # Displayed 40x20; coded so that applying orientation n gives it back.
    disp = quad(40, 20)
    for n in range(1, 9):
        coded = disp if INV[n] is None else disp.transpose(INV[n])
        coded.save(os.path.join(OUT, "exif%d.jpg" % n), quality=95, exif=exif_bytes(n))
    disp.transpose(INV[6]).save(os.path.join(OUT, "exif6.png"), exif=exif_bytes(6))
    # Animation: 3 frames, 100 / 200 / 300 ms.
    frames = [Image.new("RGB", (16, 16), c) for c in (TL, TR, BL)]
    frames[0].save(os.path.join(OUT, "anim.gif"), save_all=True, append_images=frames[1:],
                   duration=[100, 200, 300], loop=0)
    # Big enough that a display-size decode must scale: 1200 x 900.
    big = Image.new("RGB", (1200, 900))
    big.paste(quad(1200, 900))
    big.save(os.path.join(OUT, "big.png"), optimize=True)
    # Broken inputs.
    raw = open(os.path.join(OUT, "quad.png"), "rb").read()
    open(os.path.join(OUT, "trunc.png"), "wb").write(raw[: len(raw) // 2])
    jr = open(os.path.join(OUT, "quad.jpg"), "rb").read()
    open(os.path.join(OUT, "trunc.jpg"), "wb").write(jr[: len(jr) // 2])
    bad = bytearray(raw)
    i = bad.find(b"IDAT") + 8
    for k in range(i, min(i + 24, len(bad) - 12)):
        bad[k] ^= 0x5A
    open(os.path.join(OUT, "corrupt.png"), "wb").write(bytes(bad))
    open(os.path.join(OUT, "garbage.png"), "wb").write(b"not an image at all\n" * 8)
    # Bombs: headers that claim huge canvases (no pixels follow).
    open(os.path.join(OUT, "bomb_dim.png"), "wb").write(png_header_only(100000, 100000))
    open(os.path.join(OUT, "bomb_px.png"), "wb").write(png_header_only(12000, 12000))
    return 0


if __name__ == "__main__":
    sys.exit(main())
