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

# ---- models kept as they are with clothing layers (growOnly; Olivia's skirt and belt, Nov 16) ----------------------
# A skirt hangs from the fullest point: below it the skirt takes the growth there, straight back (no crease drop: K saw
# a sharp edge). The body under it is kept inside it (pulled in where the grown body would poke out between the skirt's
# coarse triangles: K saw her panties through the skirt), as deep as it sat at 100% or MARGIN. A belt plate over the
# skirt is pushed out as one piece so what is under it stays under it (K: the strap through its silver plate).
APEX, MARGIN = 0.965, 0.002

def axis_point(P):
    return np.stack([np.clip(P[:, 0], -AXIS_X, AXIS_X), np.full(len(P), AXIS_Y), np.full(len(P), AXIS_Z)], 1)

def skirt_field(P):
    Q = P.copy()
    low = Q[:, 1] < APEX
    Q[low, 1] = APEX
    D = field(Q)
    D[low, 1] = 0.0
    return D

def ray_hits(orig, dirs, V, T):
    """Distance along each unit ray to the nearest triangle of (V, T) in front of it (inf: none)."""
    a, b, c = V[T[:, 0]], V[T[:, 1]], V[T[:, 2]]
    e1, e2 = b - a, c - a
    out = np.full(len(orig), np.inf)
    for i, (o, d) in enumerate(zip(orig, dirs)):
        p = np.cross(d, e2); det = (e1 * p).sum(1); ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1 / np.where(ok, det, 1), 0); s = o - a; u = (s * p).sum(1) * inv
        q = np.cross(s, e1); v = (d * q).sum(1) * inv; t = (e2 * q).sum(1) * inv
        m = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-6)
        if m.any():
            out[i] = t[m].min()
    return out

def _rays(P):
    c = axis_point(P); r = P - c; d = np.linalg.norm(r, axis=1)
    return c, r / np.maximum(d, 1e-12)[:, None], d

def grow_layers(nodes, skirt=None, under=None, plate=None):
    """nodes: {name: (P, T, N)} of one model. Returns {name: (D, N130)} for the nodes the growth reaches."""
    D = {n: (skirt_field(P) if n == skirt else field(P)) for n, (P, T, N) in nodes.items()}
    if skirt in nodes and under in nodes:
        Ps, Ts, _ = nodes[skirt]; Pu = nodes[under][0]
        idx = np.where(D[under][:, 2] < -1e-5)[0]
        c0, u0, d0 = _rays(Pu[idx]); h0 = ray_hits(c0, u0, Ps, Ts)
        G = Pu[idx] + D[under][idx]
        c1, u1, d1 = _rays(G); h1 = ray_hits(c1, u1, Ps + D[skirt], Ts)
        allowed = h1 - np.minimum(MARGIN, h0 - d0)
        over = np.isfinite(h0) & np.isfinite(h1) & (d1 > allowed)
        D[under][idx[over]] = c1[over] + u1[over] * allowed[over][:, None] - Pu[idx[over]]
    if plate in nodes:
        Pp, Tp, _ = nodes[plate]
        lo, hi = Pp.min(0), Pp.max(0)
        need = 0.0
        for n in (skirt, under):
            if n not in nodes:
                continue
            P = nodes[n][0]
            idx = np.where((P[:, 0] >= lo[0] - 0.012) & (P[:, 0] <= hi[0] + 0.012) & (P[:, 1] >= lo[1] - 0.012) &
                           (P[:, 1] <= hi[1] + 0.012) & (P[:, 2] < -0.05))[0]
            if not len(idx):
                continue
            c0, u0, d0 = _rays(P[idx]); h0 = ray_hits(c0, u0, Pp, Tp)
            c1, u1, d1 = _rays(P[idx] + D[n][idx]); h1 = ray_hits(c1, u1, Pp + D[plate], Tp)
            ok = np.isfinite(h0) & np.isfinite(h1)
            if ok.any():
                need = max(need, float(np.max(np.minimum(h0 - d0, MARGIN)[ok] - (h1 - d1)[ok])))
        if need > 0:
            _, up, _ = _rays(Pp + D[plate])
            D[plate] = D[plate] + up * need
    out = {}
    for n, (P, T, N) in nodes.items():
        if np.abs(D[n]).max() < 1e-3:
            continue
        sn, _ = xfer.smooth_normals(P, T); sn2, _ = xfer.smooth_normals(P + D[n], T)
        N2 = N + (sn2 - sn)
        out[n] = (D[n], N2 / (np.linalg.norm(N2, axis=1)[:, None] + 1e-12))
    return out
