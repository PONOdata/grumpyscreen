"""Decode a glassrun.py capture into a labelled contact sheet.

Runs on the desktop (needs Pillow).

usage: python sheet.py <capture dir> <out.png> [every N] [first M]

Each tile is labelled with its ms since the action and the Klipper state
index.txt recorded for that frame.
"""
import os
import sys
import zlib

from PIL import Image, ImageDraw

src, dst = sys.argv[1], sys.argv[2]
every = int(sys.argv[3]) if len(sys.argv) > 3 else 1
first = int(sys.argv[4]) if len(sys.argv) > 4 else 10**9
state = {}
idx = os.path.join(src, "index.txt")
if os.path.exists(idx):
    for ln in open(idx):
        p = ln.split()
        if len(p) == 2 and p[0].endswith(".z"):
            state[p[0]] = p[1]
frames = sorted(f for f in os.listdir(src) if f.endswith(".z"))[:first][::every]
if not frames:
    sys.exit("no frames in " + src)
cols = 4
w, h = 240, 136
rows = (len(frames) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * (h + 14)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for k, f in enumerate(frames):
    raw = zlib.decompress(open(os.path.join(src, f), "rb").read())
    if len(raw) != 480 * 272 * 4:
        sys.exit("%s decodes to %d bytes, not one 480x272 BGRA frame" % (f, len(raw)))
    im = Image.frombytes("RGBA", (480, 272), raw, "raw", "BGRA", 1920, 1).convert("RGB")
    x, y = (k % cols) * w, (k // cols) * (h + 14)
    sheet.paste(im.resize((w, h)), (x, y + 14))
    d.text((x + 2, y + 1), "%s %s" % (f[4:9], state.get(f, "")), fill=(255, 220, 120))
sheet.save(dst)
print(dst, len(frames), "frames")
