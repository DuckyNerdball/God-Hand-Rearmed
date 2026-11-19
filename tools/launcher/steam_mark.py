"""The bill's Add to Steam mark (K, Nov 18): a sketched Steam arm from Flaticon (a 512 px black-on-transparent PNG;
free with attribution, README credits) inside a hand-inked ring, the whole logo. Writes resources/images/steam_mark.png (256 px, white, the ink in alpha).

python3 -I tools/launcher/steam_mark.py <sketch.png> resources/images/steam_mark.png [a|b]

a (used): the small wheel sits in the ring's edge, as in Steam's own mark; b: the ring round the whole arm. Prints the
ring's radius against the picture's half width: kSteamMarkRing in src/kamite/launcher_saloon.cpp. Fixed seed.
"""
import sys, numpy as np
from PIL import Image
from scipy import ndimage as nd
src, out = sys.argv[1], sys.argv[2]
variant = sys.argv[3] if len(sys.argv) > 3 else 'a'
a = np.array(Image.open(src).convert('RGBA'))[..., 3].astype(np.float32) / 255.0
H, W = a.shape
fill = nd.binary_fill_holes(nd.binary_closing(a > 0.4, iterations=6))
if variant == 'a':
    C = np.array([306.0, 200.0]); R = 236.0
else:
    ys, xs = np.where(fill); C = np.array([(xs.min() + xs.max()) / 2, (ys.min() + ys.max()) / 2])
    R = np.sqrt((xs - C[0]) ** 2 + (ys - C[1]) ** 2).max() + 40
T = 30.0
ys, xs = np.where(fill)
reach = max(R + T, np.sqrt((xs - C[0]) ** 2 + (ys - C[1]) ** 2).max())
half = int(reach + 14)
size = 2 * half
x0, y0 = int(round(half - C[0])), int(round(half - C[1]))
arm = np.zeros((size, size), np.float32); armfill = np.zeros((size, size), bool)
arm[y0:y0 + H, x0:x0 + W] = a; armfill[y0:y0 + H, x0:x0 + W] = fill
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
ang = np.arctan2(yy - half, xx - half); dist = np.hypot(xx - half, yy - half)
def wobble(n, amp):
    k = np.arange(1, n + 1); ph = rng.uniform(0, 2 * np.pi, n); am = amp * rng.uniform(0.3, 1.0, n) / k
    return lambda t: sum(am[i] * np.sin(k[i] * t + ph[i]) for i in range(n))
radius = R + wobble(5, 4.0)(ang)
width = np.maximum(T * (1.0 + 0.22 * wobble(6, 1.0)(ang)), T * 0.55)
rough = nd.gaussian_filter(rng.standard_normal((size, size)), 3.5); rough /= rough.std()
d = np.abs(dist - radius) - width / 2 + 1.0 * rough
ring = np.clip(0.5 - d / 1.4, 0, 1)
streak = np.zeros_like(ring)
for _ in range(9):
    t0 = rng.uniform(-np.pi, np.pi); span = rng.uniform(0.12, 0.45); off = rng.uniform(-0.3, 0.3); th = rng.uniform(1.5, 3.5)
    dt = np.angle(np.exp(1j * (ang - t0)))
    along = np.clip(1 - np.abs(dt) / (span / 2), 0, 1)
    across = np.clip(1 - np.abs(dist - radius - off * width) / th, 0, 1)
    streak = np.maximum(streak, along ** 0.5 * across)
ring = ring * (1 - 0.9 * streak)
ring[nd.binary_dilation(armfill, iterations=9)] = 0  # a gap where the arm meets the ring
alpha = np.maximum(arm, ring)
img = np.zeros((size, size, 4), np.uint8); img[..., :3] = 255; img[..., 3] = (alpha * 255 + 0.5).astype(np.uint8)
Image.fromarray(img).resize((256, 256), Image.LANCZOS).save(out)
print(variant, 'ring radius in the image (of half):', round(R / half, 4))
