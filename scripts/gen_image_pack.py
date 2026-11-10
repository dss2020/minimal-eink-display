#!/usr/bin/env python3
"""Generate an e-ink image pack (EIPK) from a directory of photos.

Pack format (all little-endian):
  Header, 12 bytes: magic b"EIPK", version u8 = 1, bpp u8 = 1,
                    width u16, height u16, count u16
  `count` entries, 12 bytes each: offset u32 (from start of pack to payload),
                    length u32 (stored payload bytes), method u8, 3 zero pad bytes
  Payloads back to back.
  method 0 = raw frame bytes, method 1 = LZSS (only if strictly smaller).

Raw frame: row-major, rows padded to whole bytes (stride = (W+7)//8),
MSB-first within a byte, bit 1 = white, bit 0 = black. Row padding bits
are set to 1 (white).

LZSS bitstream: groups of one flag byte + up to 8 items; flag bits consumed
LSB first. Bit 1 = literal (1 byte). Bit 0 = match: u16 LE v, offset =
(v >> 4) + 1 (1..4096), length = (v & 0xF) + 3 (3..18); copied byte by byte
so overlapping matches are valid. No end marker: decoding stops when the raw
frame size has been produced. Matches never run past the raw size. Unused
flag bits in the last group are 0 and have no item bytes.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"EIPK"
VERSION = 1
HEADER_SIZE = 12
ENTRY_SIZE = 12
METHOD_RAW = 0
METHOD_LZSS = 1

MIN_MATCH = 3
MAX_MATCH = 18
MAX_OFFSET = 4096
MAX_CHAIN = 32


def frame_stride(width: int) -> int:
    return (width + 7) // 8


def lzss_encode(data: bytes) -> bytes:
    """Compress data with the LZSS variant described in the module docstring."""
    n = len(data)
    out = bytearray()
    chains: dict[bytes, list[int]] = {}
    flags = 0
    nbits = 0
    items = bytearray()

    def insert(p: int) -> None:
        if p + MIN_MATCH <= n:
            chains.setdefault(data[p:p + MIN_MATCH], []).append(p)

    i = 0
    while i < n:
        best_len = 0
        best_off = 0
        if i + MIN_MATCH <= n:
            cand = chains.get(data[i:i + MIN_MATCH])
            if cand:
                limit = min(MAX_MATCH, n - i)
                tries = 0
                for j in range(len(cand) - 1, -1, -1):
                    p = cand[j]
                    off = i - p
                    if off > MAX_OFFSET:
                        break
                    tries += 1
                    if tries > MAX_CHAIN:
                        break
                    ln = MIN_MATCH
                    while ln < limit and data[p + ln] == data[i + ln]:
                        ln += 1
                    if ln > best_len:
                        best_len, best_off = ln, off
                        if ln == limit:
                            break
        if best_len >= MIN_MATCH:
            v = ((best_off - 1) << 4) | (best_len - MIN_MATCH)
            items += struct.pack("<H", v)
            for k in range(best_len):
                insert(i + k)
            i += best_len
        else:
            flags |= 1 << nbits
            items.append(data[i])
            insert(i)
            i += 1
        nbits += 1
        if nbits == 8:
            out.append(flags)
            out += items
            flags = 0
            nbits = 0
            items = bytearray()
    if nbits:
        out.append(flags)
        out += items
    return bytes(out)


def lzss_decode(data: bytes, raw_size: int) -> bytes:
    """Reference decoder. Raises ValueError on malformed input."""
    out = bytearray()
    pos = 0
    while len(out) < raw_size:
        if pos >= len(data):
            raise ValueError("LZSS stream truncated")
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= raw_size:
                break
            if flags & (1 << bit):
                if pos >= len(data):
                    raise ValueError("LZSS stream truncated (literal)")
                out.append(data[pos])
                pos += 1
            else:
                if pos + 2 > len(data):
                    raise ValueError("LZSS stream truncated (match)")
                v = data[pos] | (data[pos + 1] << 8)
                pos += 2
                off = (v >> 4) + 1
                ln = (v & 0xF) + MIN_MATCH
                if off > len(out):
                    raise ValueError("LZSS match offset before start")
                if len(out) + ln > raw_size:
                    raise ValueError("LZSS match runs past raw size")
                for _ in range(ln):
                    out.append(out[-off])
    return bytes(out)


def build_pack(frames: list[bytes], width: int, height: int) -> tuple[bytes, list[tuple[int, int]]]:
    """Build a pack. Returns (pack bytes, [(raw_size, stored_size), ...])."""
    raw_size = frame_stride(width) * height
    payloads: list[tuple[int, bytes]] = []
    for f in frames:
        if len(f) != raw_size:
            raise ValueError(f"frame size {len(f)} != expected {raw_size}")
        comp = lzss_encode(f)
        if len(comp) < len(f):
            payloads.append((METHOD_LZSS, comp))
        else:
            payloads.append((METHOD_RAW, f))
    count = len(frames)
    out = bytearray(MAGIC)
    out += struct.pack("<BBHHH", VERSION, 1, width, height, count)
    offset = HEADER_SIZE + ENTRY_SIZE * count
    for method, p in payloads:
        out += struct.pack("<IIB3x", offset, len(p), method)
        offset += len(p)
    for _, p in payloads:
        out += p
    return bytes(out), [(raw_size, len(p)) for _, p in payloads]


def parse_pack(pack: bytes) -> tuple[dict, list[tuple[int, int, int]]]:
    """Parse header and entries -> (info dict, [(offset, length, method)])."""
    if len(pack) < HEADER_SIZE or pack[:4] != MAGIC:
        raise ValueError("bad magic")
    ver, bpp, w, h, count = struct.unpack_from("<BBHHH", pack, 4)
    entries = [struct.unpack_from("<IIB", pack, HEADER_SIZE + ENTRY_SIZE * i)
               for i in range(count)]
    return {"version": ver, "bpp": bpp, "width": w, "height": h, "count": count}, entries


def decode_pack_frame(pack: bytes, index: int) -> bytes:
    info, entries = parse_pack(pack)
    off, ln, method = entries[index]
    payload = pack[off:off + ln]
    raw_size = frame_stride(info["width"]) * info["height"]
    if method == METHOD_RAW:
        if ln != raw_size:
            raise ValueError("raw payload size mismatch")
        return payload
    return lzss_decode(payload, raw_size)


# ---------------------------------------------------------------- imaging

def _import_pil():
    try:
        from PIL import Image, ImageDraw, ImageFont, ImageOps
    except ImportError:
        sys.exit("error: Pillow is required (pip install -r scripts/requirements.txt)")
    return Image, ImageDraw, ImageFont, ImageOps


def list_images(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    exts = {".jpg", ".jpeg", ".png"}
    files = [p for p in directory.iterdir()
             if p.is_file() and not p.name.startswith(".") and p.suffix.lower() in exts]
    return sorted(files, key=lambda p: p.name.lower())


def image_to_frame(img, width: int, height: int, fit: str, dither: str):
    """Return a 1-bit PIL image of exactly width x height."""
    Image, _, _, ImageOps = _import_pil()
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "P") or "transparency" in img.info:
        rgba = img.convert("RGBA")
        bg = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        img = Image.alpha_composite(bg, rgba)
    img = img.convert("RGB")
    if fit == "cover":
        img = ImageOps.fit(img, (width, height), Image.LANCZOS, centering=(0.5, 0.5))
    else:
        img = ImageOps.contain(img, (width, height), Image.LANCZOS)
        canvas = Image.new("RGB", (width, height), (255, 255, 255))
        canvas.paste(img, ((width - img.width) // 2, (height - img.height) // 2))
        img = canvas
    gray = img.convert("L")
    if dither == "fs":
        return gray.convert("1", dither=Image.FLOYDSTEINBERG)
    return gray.point(lambda p: 255 if p >= 128 else 0).convert("1", dither=Image.NONE)


def frame_bytes(bw) -> bytes:
    """Pack a 1-bit image: row padded, MSB first, 1 = white."""
    data = bytearray(bw.tobytes())
    w, h = bw.size
    pad = frame_stride(w) * 8 - w
    if pad:
        stride = frame_stride(w)
        mask = (1 << pad) - 1  # padding bits (low bits of last byte) = 1 (white)
        for y in range(h):
            data[y * stride + stride - 1] |= mask
    return bytes(data)


def make_placeholder(width: int, height: int):
    Image, ImageDraw, ImageFont, _ = _import_pil()
    im = Image.new("L", (width, height), 255)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, width - 1, height - 1], outline=0, width=2)
    d.line([0, 0, width - 1, height - 1], fill=0, width=1)
    d.line([0, height - 1, width - 1, 0], fill=0, width=1)
    try:
        font = ImageFont.load_default()
        text = "No pictures found"
        box = d.textbbox((0, 0), text, font=font)
        tw, th = box[2] - box[0], box[3] - box[1]
        x, y = (width - tw) // 2, (height - th) // 2
        d.rectangle([x - 4, y - 4, x + tw + 4, y + th + 4], fill=255, outline=0)
        d.text((x - box[0], y - box[1]), text, fill=0, font=font)
    except Exception:
        pass
    return im.point(lambda p: 255 if p >= 128 else 0).convert("1", dither=Image.NONE)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate an EIPK e-ink image pack.")
    ap.add_argument("--input-dir", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--width", required=True, type=int)
    ap.add_argument("--height", required=True, type=int)
    ap.add_argument("--fit", choices=("cover", "contain"), default="cover")
    ap.add_argument("--dither", choices=("fs", "none"), default="fs")
    ap.add_argument("--preview-dir", type=Path)
    a = ap.parse_args()
    if not (0 < a.width <= 0xFFFF and 0 < a.height <= 0xFFFF):
        print("error: width/height must be in 1..65535", file=sys.stderr)
        return 1

    Image, _, _, _ = _import_pil()
    files = list_images(a.input_dir)
    names: list[str] = []
    bws = []
    if not files:
        print(f"warning: no images found in {a.input_dir}; emitting placeholder frame",
              file=sys.stderr)
        bws.append(make_placeholder(a.width, a.height))
        names.append("placeholder")
    for p in files:
        try:
            with Image.open(p) as im:
                im.load()
                bws.append(image_to_frame(im, a.width, a.height, a.fit, a.dither))
        except Exception as e:
            print(f"error: cannot process image {p}: {e}", file=sys.stderr)
            return 1
        names.append(p.stem)

    frames = [frame_bytes(b) for b in bws]
    pack, sizes = build_pack(frames, a.width, a.height)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(pack)

    if a.preview_dir:
        a.preview_dir.mkdir(parents=True, exist_ok=True)
        for i, (b, nm) in enumerate(zip(bws, names)):
            b.save(a.preview_dir / f"{i:03d}_{nm}.png")

    print(f"image pack: {len(frames)} image(s), {a.width}x{a.height}, 1 bpp -> {a.output}")
    for i, (nm, (raw, stored)) in enumerate(zip(names, sizes)):
        print(f"  {i:03d} {nm}: raw {raw} B, stored {stored} B ({100 * stored / raw:.0f}%)")
    print(f"total pack size: {len(pack)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
