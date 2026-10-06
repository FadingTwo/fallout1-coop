#!/usr/bin/env python3
# Side-by-side gameplay clip for the website from a video tour:
#
#   COOP_AUTOTEST_VIDEO=1 COOP_AUTOTEST_TOUR=JUNKENT.MAP,HUBDWNTN.MAP \
#       tools/nettest.sh build/fallout-ce <out> net_tour_host net_tour_client
#   tools/make_video.py <out> <output name> <ffmpeg>
#
# Writes <output name>.mp4 and <output name>-poster.jpg. Needs Pillow.
import glob, os, struct, subprocess, sys, bisect
from PIL import Image, ImageDraw, ImageFont
src = sys.argv[1]; out = sys.argv[2]; ffmpeg = sys.argv[3]
def frames(d):
    fs = sorted(glob.glob(os.path.join(d, 'vid-*.raw')), key=lambda f: int(f.split('-')[-1][:-4]))
    return [(int(f.split('-')[-1][:-4]), f) for f in fs]
def load(f):
    b = open(f, 'rb').read()
    w, h = struct.unpack('<ii', b[:8]); pal = b[8:776]; px = b[776:776 + w * h]
    im = Image.frombytes('P', (w, h), px)
    im.putpalette(bytes(min(255, c * 4) for c in pal))  # 6-bit VGA palette
    return im.convert('RGB')
H, C = frames(src + '/host'), frames(src + '/client')
ht = [t for t, _ in H]; ct = [t for t, _ in C]
start = max(ht[0], ct[0]); end = min(ht[-1], ct[-1])
fb = ImageFont.truetype('/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf', 18)
G = (65, 255, 122); Y = (240, 192, 64); BG = (4, 11, 5)
tmp = out + '.frames'; os.makedirs(tmp, exist_ok=True)
cache = {}
def get(f):
    if f not in cache:
        cache.clear(); cache[f] = load(f)
    return cache[f]
from PIL import ImageChops, ImageStat
def same(a, b):
    # Player 2's screen mirrors player 1's while a map loads: skip those.
    diff = ImageChops.difference(a.crop((0, 0, 640, 378)), b.crop((0, 0, 640, 378)))
    return sum(ImageStat.Stat(diff).mean) < 6
n = 0; t = start; skipped = 0
while t <= end:
    hf = H[bisect.bisect_right(ht, t) - 1][1]; cf = C[bisect.bisect_right(ct, t) - 1][1]
    a, b = load(hf), load(cf)
    if same(a, b):
        skipped += 1; t += 100; continue
    canvas = Image.new('RGB', (1296, 516), BG)
    canvas.paste(a, (8, 30)); canvas.paste(b, (648, 30))
    d = ImageDraw.Draw(canvas)
    d.rectangle([7, 29, 648, 510], outline=G, width=1); d.rectangle([647, 29, 1288, 510], outline=Y, width=1)
    d.text((10, 5), "PLAYER 1'S SCREEN", font=fb, fill=G); d.text((650, 5), "PLAYER 2'S SCREEN", font=fb, fill=Y)
    canvas.save(f'{tmp}/{n:05d}.png'); n += 1; t += 100
print(n, 'frames,', skipped, 'skipped')
subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-framerate', '10', '-i', f'{tmp}/%05d.png', '-vf', 'fps=30,format=yuv420p',
                '-c:v', 'libx264', '-crf', '23', '-preset', 'slow', '-movflags', '+faststart', out + '.mp4'], check=True)
subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-i', f'{tmp}/{n//3:05d}.png', '-q:v', '3', out + '-poster.jpg'], check=True)
