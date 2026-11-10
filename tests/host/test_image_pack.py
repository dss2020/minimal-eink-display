#!/usr/bin/env python3
"""Tests for scripts/gen_image_pack.py (and the C codec if present)."""
import importlib.util
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "gen_image_pack.py"

spec = importlib.util.spec_from_file_location("gen_image_pack", SCRIPT)
gip = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gip)

failures = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global failures
    print(("PASS: " if ok else "FAIL: ") + name + (f" ({detail})" if detail else ""))
    if not ok:
        failures += 1


def roundtrip(name: str, data: bytes, expect_raw: bool = False) -> None:
    enc = gip.lzss_encode(data)
    try:
        dec = gip.lzss_decode(enc, len(data))
    except ValueError as e:
        check(f"lzss {name}", False, str(e))
        return
    check(f"lzss {name} roundtrip ({len(data)} -> {len(enc)})", dec == data)
    if expect_raw:
        pack, sizes = gip.build_pack([data], len(data) * 8, 1)
        _, entries = gip.parse_pack(pack)
        check(f"lzss {name} falls back to raw", entries[0][2] == 0)


def test_lzss() -> None:
    roundtrip("zeros", bytes(15000))
    roundtrip("ones", b"\xff" * 15000)
    rnd = random.Random(1)
    roundtrip("random", bytes(rnd.randrange(256) for _ in range(4000)), expect_raw=True)
    roundtrip("pattern", b"\xa5\x3c\x0f" * 3000 + b"\x01\x02" * 500)
    roundtrip("tiny", b"ab")
    roundtrip("empty", b"")
    grad = Image.linear_gradient("L").resize((400, 300)).rotate(90)
    bw = grad.convert("1", dither=Image.FLOYDSTEINBERG)
    data = bw.tobytes()
    roundtrip("dithered gradient", data)
    ratio = len(gip.lzss_encode(data)) / len(data)
    print(f"INFO: dithered gradient compression ratio {ratio:.2f}")


def make_images(d: Path) -> None:
    g = Image.linear_gradient("L").resize((640, 480)).convert("RGB")
    g.save(d / "a_gradient.jpg", quality=85)
    rgba = Image.new("RGBA", (200, 200), (0, 0, 0, 0))
    for x in range(200):
        for y in range(100):
            rgba.putpixel((x, y), (200, 30, 30, 255))
    rgba.save(d / "b_alpha.PNG")
    port = Image.linear_gradient("L").resize((300, 500)).convert("RGB")
    port.save(d / "c_portrait.jpeg")
    Image.new("RGB", (320, 240), "white").save(d / "d_white.png")
    (d / ".hidden.png").write_bytes(b"junk")
    (d / "notes.txt").write_text("ignore me")


def run_gen(indir: Path, out: Path, w: int, h: int, *extra: str):
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--input-dir", str(indir), "--output", str(out),
         "--width", str(w), "--height", str(h), *extra],
        capture_output=True, text=True)


def test_packs(tmp: Path) -> list[tuple[Path, int, int]]:
    imgs = tmp / "imgs"
    imgs.mkdir()
    make_images(imgs)
    packs = []
    for w, h in ((400, 300), (122, 250)):
        out = tmp / f"pack_{w}x{h}.bin"
        prev = tmp / f"prev_{w}x{h}"
        r = run_gen(imgs, out, w, h, "--preview-dir", str(prev))
        check(f"gen {w}x{h} exit 0", r.returncode == 0, r.stderr.strip())
        if r.returncode != 0:
            continue
        pack = out.read_bytes()
        info, entries = gip.parse_pack(pack)
        check(f"{w}x{h} header", info == {"version": 1, "bpp": 1, "width": w,
                                         "height": h, "count": 4}, str(info))
        stride, raw = (w + 7) // 8, ((w + 7) // 8) * h
        pos = 12 + 12 * 4
        ok = True
        for off, ln, m in entries:
            ok &= off == pos and m in (0, 1) and (m == 1 or ln == raw) and ln <= raw
            pos += ln
        ok &= pos == len(pack)
        check(f"{w}x{h} entries contiguous", ok)
        ok = all(len(gip.decode_pack_frame(pack, i)) == raw for i in range(4))
        check(f"{w}x{h} frames decode to {raw} bytes (stride {stride})", ok)
        previews = sorted(p.name for p in prev.iterdir())
        check(f"{w}x{h} previews", previews == ["000_a_gradient.png", "001_b_alpha.png",
                                                "002_c_portrait.png", "003_d_white.png"],
              str(previews))
        white = gip.decode_pack_frame(pack, 3)
        check(f"{w}x{h} white image is all 0xFF", set(white) == {0xFF})
        # contain/none options run
        r2 = run_gen(imgs, tmp / "opt.bin", w, h, "--fit", "contain", "--dither", "none")
        check(f"{w}x{h} contain/none exit 0", r2.returncode == 0, r2.stderr.strip())
        packs.append((out, w, h))
    return packs


def test_empty(tmp: Path) -> None:
    for name, d in (("empty dir", tmp / "empty"), ("missing dir", tmp / "nope")):
        if name == "empty dir":
            d.mkdir()
        out = tmp / "empty.bin"
        r = run_gen(d, out, 400, 300)
        check(f"{name}: exit 0", r.returncode == 0, r.stderr.strip())
        info, _ = gip.parse_pack(out.read_bytes())
        check(f"{name}: one frame", info["count"] == 1)
        check(f"{name}: warning on stderr", "warning" in r.stderr.lower())
    bad = tmp / "bad"
    bad.mkdir()
    (bad / "broken.jpg").write_bytes(b"not a jpeg")
    r = run_gen(bad, tmp / "bad.bin", 100, 100)
    check("corrupt image fails non-zero", r.returncode != 0 and "broken.jpg" in r.stderr)


def test_c(tmp: Path, packs) -> None:
    csrc = ROOT / "lib" / "image_pack" / "image_pack_codec.c"
    if not csrc.exists():
        print("SKIP: C codec not present")
        return
    exe = tmp / "decode_main"
    r = subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "lib" / "image_pack"),
                        str(ROOT / "tests" / "host" / "decode_main.c"), str(csrc),
                        "-o", str(exe)], capture_output=True, text=True)
    check("C codec compiles", r.returncode == 0, r.stderr.strip())
    if r.returncode != 0:
        return
    packs = list(packs) + [(tmp / "empty.bin", 400, 300)]
    for out, w, h in packs:
        od = tmp / f"c_{out.stem}"
        od.mkdir()
        r = subprocess.run([str(exe), str(out), str(od)], capture_output=True, text=True)
        check(f"C decode {out.name} exit 0", r.returncode == 0, r.stderr.strip())
        if r.returncode != 0:
            continue
        pack = out.read_bytes()
        info, _ = gip.parse_pack(pack)
        same = all((od / f"frame_{i:03d}.bin").read_bytes() == gip.decode_pack_frame(pack, i)
                   for i in range(info["count"]))
        check(f"C frames identical to Python for {out.name}", same)


def main() -> int:
    test_lzss()
    tmp = Path(tempfile.mkdtemp(prefix="eipk_test_"))
    try:
        packs = test_packs(tmp)
        test_empty(tmp)
        test_c(tmp, packs)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print(f"{failures} CHECK(S) FAILED")
        return 1
    print("ALL PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
