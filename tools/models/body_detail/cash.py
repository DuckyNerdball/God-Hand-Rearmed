"""Cash's growth (K, Nov 15): the seat grown above 101% Cash, on top of the detailed seat (transplant.build2).

The growth at 130% per height comes from a side-profile reference K picked (the body's depth before / after, measured
per row and laid onto the model's landmarks: small of the back y 1.18, fullest point 1.00, crease 0.925, thigh 0.795;
0.23 m of body depth = 222 px). Nothing at the waist, the most in the cheeks' lower half (a teardrop that rounds over
into a deeper fold), the thighs at THIGH of the reference (K: a faint fill). The move is a function of position only
(a flap, a hem or a skirt over the body moves with it): away from the cheeks' centre line (sideways part x 0.6, the
slope capped at UP / -DOWN) by the profile x a lateral weight (the cleft half, the hip sides HIP) x a facing weight
(back 1, sides ~0.4, front 0) x less between the thighs.
Cash c grows it by (c - 1) / 0.3 (linear; the normals blended to the 130% ones). The C++ (body_detail_build.cpp
cashGrowth) follows this.
"""
import numpy as np
import xfer

# growth at 130% Cash (mm) at y = 0.79, 0.80, ... 1.23 (K's reference profile, Nov 15)
PROFILE_MM = [11.4, 20.3, 20.4, 20.7, 20.7, 20.7, 20.9, 19.9, 20.7, 20.0, 18.5, 17.7, 17.2, 17.6, 21.1, 30.9, 37.8, 40.8,
              41.2, 39.3, 37.2, 36.0, 35.6, 35.0, 34.2, 33.4, 32.4, 30.7, 28.7, 25.0, 23.0, 18.7, 15.1, 11.0, 7.0, 4.6,
              3.5, 1.9, 0.3, 0.3, 0.7, 0.0, 0.0, 0.0, 0.0]
PROFILE_Y0, PROFILE_DY = 0.79, 0.01
THIGH, CREASE, HIP, DOWN, UP = 0.35, 0.925, 0.35, 0.45, 0.3

def sstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)

def profile(y):
    ys = PROFILE_Y0 + PROFILE_DY * np.arange(len(PROFILE_MM))
    return np.interp(y, ys, np.array(PROFILE_MM) / 1000.0, left=PROFILE_MM[0] / 1000.0, right=0.0)

AXIS_X, AXIS_Y, AXIS_Z = 0.06, 1.00, -0.03   # the cheeks' centre line (x -0.06 .. 0.06)

def direction(P):
    """Away from the cheeks' centre line: the sideways part x 0.6, the slope (rise per horizontal) capped at UP above
    and -DOWN below (a move mostly back, as the surface normals there)."""
    c = np.stack([np.clip(P[:, 0], -AXIS_X, AXIS_X), np.full(len(P), AXIS_Y), np.full(len(P), AXIS_Z)], 1)
    r = P - c
    r = r / (np.linalg.norm(r, axis=1)[:, None] + 1e-12)
    h = np.hypot(0.6 * r[:, 0], r[:, 2])
    slope = np.clip(r[:, 1] / np.maximum(h, 1e-6), -DOWN, UP)
    d = np.stack([0.6 * r[:, 0], slope * h, r[:, 2]], 1)
    return r, d / (np.linalg.norm(d, axis=1)[:, None] + 1e-12)

def field(P):
    """The move at 130% at each position (a function of position only, so a flap or a hem over the body moves with it)."""
    y = P[:, 1]
    g = profile(y)
    g = g * np.where(y < CREASE, THIGH + (1 - THIGH) * sstep(CREASE - 0.03, CREASE, y), 1.0)
    g = g * sstep(0.72, 0.80, y)
    ax = np.abs(P[:, 0])
    lat = np.where(ax < 0.02, 0.5 + 0.5 * sstep(0.0, 0.02, ax), 1.0) * (1 - (1 - HIP) * sstep(0.10, 0.165, ax))
    r, d = direction(P)
    face = np.clip((-r[:, 2] + 0.2) / 0.5, 0, 1)
    below = 1 - sstep(CREASE - 0.03, CREASE, y)   # between the thighs: little
    inner = 1 - below * (1 - (0.3 + 0.7 * sstep(0.02, 0.05, ax)))
    return (g * lat * face * inner)[:, None] * d

def growth(P, T, N):
    """(D, N130): each record's move at 130% and its normal there. P / N: the detailed state's positions / normals."""
    D = field(P)
    sn, _ = xfer.smooth_normals(P, T)
    sn2, _ = xfer.smooth_normals(P + D, T)
    N2 = N + (sn2 - sn)
    return D, N2 / (np.linalg.norm(N2, axis=1)[:, None] + 1e-12)

def at(P, N, D, N130, cash):
    """Positions / normals at Cash c (> 1)."""
    k = np.clip((cash - 1.0) / 0.3, 0.0, 1.0)
    n = N * (1 - k) + N130 * k
    return P + k * D, n / (np.linalg.norm(n, axis=1)[:, None] + 1e-12)
