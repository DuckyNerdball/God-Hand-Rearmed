"""The women's chest-grab Suplex (K, Nov 15; claude/boobplex.md). Buck Breaker's Suplex pick on women keeps the game's
punish Suplex (Gene 333, victim 329 / 331) and its women's placement, with Gene's arms re-solved so his palms sit on
her bust from the waist grab until she lands. Method after a fan's Blender bake (two-bone IK, clavicle assist, the
hand aimed by finger direction and palm normal), done here on the game's own data: the bust is bone 29 (off the
upper spine, 2) with Money's size and droop (bone_tweaks placeMoney: rows x s, rotX((s - 1) x 30 deg)); the jiggle is
held at 0 while the hands are on (K).

    GODHAND_EXTRACT=<extract> python3 chest_suplex.py <body file> <enemy size> <money> <out.png> [frames]
"""
import sys, os, math, re, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from common import *
from fk import Motion, rot, legik
from scene import Actor, Track, align, roty
from mesh import load_mesh, skin
from punishes import PUNISH
from scipy.spatial.transform import Rotation as Rot

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
PF = os.path.join(REPO, 'resources', 'punish_fit.txt')
GENE_PATH = EX + '/pl/pl00.dat'
D = math.radians

# ---- settings (the fan's, adapted) ----
BREAST_X = 0.075      # breast centre from the middle line (victim rest space); the bust block spans 0.0 .. 0.111
BREAST_Y = 1.350      # its height (the block spans 1.317 .. 1.388)
SINK = 0.015          # the palm pressed into the bust (a grip, K)
FINGERS = (0.90, 0.35, 0.0)   # fingers: toward her middle line, up, forward (victim space), before HAND_UP
ELBOW_POLE = (0.5, -0.85, 0.1)  # elbows: out, down, forward (Gene's chest space, x by side); K picked C: tucked down
HAND_UP, PALM_LOW = 30.0, 0.01  # K (C): fingers turned up 30 deg in the chest's plane, the palm 1 cm under the centre
RAMP_IN, RAMP_OUT = 7, 6
PALM_ALONG, PALM_DEPTH = 0.075, 0.042   # the cup's inner surface in the hand's space (along the fingers, below)
CLAV_MAX = D(30)
VICTIM_RAISE, ARM_LEAD = 15.0, 2   # her right upper arm raised out (deg), leading his hands (frames)
BUMP = 0.12          # the hand's way in and out: a curve through a point this far in front of the breast
LAND_EARLY = 2        # the hold ends this many frames before her chest drops below the threshold (the arms chased it)
EDGE = 0.35          # below this blend the arm's joints also blend back to the motion's (no snap where the IK's twist differs)
TURN_LEAD = 1.0      # the hand turns to its grip ahead of its travel (x the blend)
ARMS = {'R': (5, 7, 8, 9, 10, 6), 'L': (11, 13, 14, 15, 16, 12)}   # clavicle, upper arm, forearm, twist, hand, helper


def read_fit(punish, key, scale):
    """The punish_fit.txt block of a fit: size= and the gene / victim curves (key frame -> values)."""
    out = None
    for line in open(PF):
        line = line.split('#')[0].strip()
        if line.startswith('fit '):
            p = line.split()
            if out is not None:
                break
            if p[1] == punish and key in p[2].split(',') and 'scale=%.2f' % scale in line and ' off' not in line:
                out = dict(size=float(re.search(r'size=([\d.]+)', line).group(1)), gene={}, victim={})
        elif out is not None and line.startswith(('gene ', 'victim ')):
            p = line.split()
            keys = [(int(k.split(':')[0]), [float(x) for x in k.split(':')[1].split(',')]) for k in p[2:]]
            out['gene' if p[0] == 'gene' else 'victim'][int(p[1])] = keys
    return out


def curve(keys, t, n):
    if not keys:
        return np.zeros(n)
    fs = [k for k, _ in keys]
    vals = [np.array((v + [0.0] * n)[:n]) for _, v in keys]
    if t <= fs[0]:
        return vals[0]
    for i in range(len(fs) - 1):
        if fs[i] <= t <= fs[i + 1]:
            a = (t - fs[i]) / (fs[i + 1] - fs[i])
            return vals[i] * (1 - a) + vals[i + 1] * a
    return vals[-1]


def rz(a): return Rot.from_euler('z', a).as_matrix()
def ry(a): return Rot.from_euler('y', a).as_matrix()
def rx(a): return Rot.from_euler('x', a).as_matrix()


def gene_pre(v):
    """punish_fit.cpp rewrite(): Gene's pre-rotations from a gene curve's values (degrees)."""
    a = [D(x) for x in v[4:12]]
    return {1: rz(a[1]) @ rx(a[0]), 13: rz(a[2]) @ ry(a[3]), 12: rz(a[2] * .5) @ ry(a[3] * .5),
            7: rz(-a[5]) @ ry(-a[6]), 6: rz(-a[5] * .5) @ ry(-a[6] * .5), 14: ry(a[4]), 8: ry(-a[7])}


def pose_pre(skel, mot, t, pre=None, hip_dy=0.0, bend=0.0, local=None):
    """fk.pose with pre-rotations (local = pre . own) or whole local rotations given per bone."""
    bones = skel['bones']; n = len(bones); pre = pre or {}; local = local or {}
    W = [None] * n; P = [None] * n; L = [None] * n
    for i, b in enumerate(bones):
        e = [mot.get(i, 3 + c, t) for c in range(3)]
        if i == 1:
            e[0] += bend
        R = local[i] if i in local else (pre[i] @ rot(e) if i in pre else rot(e))
        L[i] = R
        if i == 0:
            tr = np.array([mot.get(0, c, t, b['t'][c]) for c in range(3)]); tr[1] += hip_dy
        else:
            tr = np.array(b['t'], float)
        p = b['parent']
        if p < 0 or p >= n or p == i or W[p] is None:
            W[i] = R; P[i] = tr if i == 0 else np.array(b['t'], float)
        else:
            W[i] = W[p] @ R; P[i] = P[p] + W[p] @ tr
    for th, kn, an in ((18, 19, 20), (22, 23, 24)):
        if an < n and mot.has(an, 0):
            legik(skel, W, P, th, kn, an, np.array([mot.get(an, c, t) for c in range(3)]), mot, t)
    return W, P, L


class Pair:
    """The fitted pair frame by frame, as the runtime plays it (placement curve, heights, bend, Gene's pose)."""

    def __init__(self, body, enemy_size=1.0, fit_scale=1.0, vmotion=329, fit=None):
        self.gene = Actor(GENE_PATH); self.vic = Actor(body)
        key = '%08x' % runtime_key(self.vic.skel['bones'])
        self.fit = fit or read_fit('Suplex', key, fit_scale)
        assert self.fit, 'no Suplex fit for %s at %.2f' % (key, fit_scale)
        k = math.sqrt(enemy_size / fit_scale)            # the pair scaling (both meet halfway)
        self.gscale = k
        self.pscale = fit_scale * k                       # the placement curve's scale (the enemy at fit size x k)
        self.fit_scale = fit_scale
        self.vscale = fit_scale * k * self.fit['size']
        self.gm = Motion(333); self.vm = Motion(vmotion); self.vmotion = vmotion
        st = PUNISH['Suplex'][0]
        g, v = self.gene, self.vic
        g.scale = self.gscale; v.scale = self.vscale
        g.pos = np.array([0, 0, 1.2]); g.yaw = math.pi; v.pos = np.zeros(3); v.yaw = 0.0
        g0 = curve(self.fit['gene'].get(333), 0, 14)
        align(g, v, st['align'][:2], st['align'][2], st['align'][3], dict(unscaled=True))
        self.gt = Track(g); self.vt = Track(v); self.gt.start([self.gm]); self.vt.start([self.vm])
        self.frames = []
        for t in range(self.gm.frames):
            if t:
                self.gt.advance(); self.vt.advance()
            self.frames.append(dict(t=t, gpos=g.pos.copy(), gyaw=g.yaw, vpos=v.pos.copy(), vyaw=v.yaw))
        self.gmesh = self.gene.mesh; self.vmesh = self.vic.mesh
        self.victim_pre = None     # t -> {bone: pre-rotation} for her (the arm raise)

    def at(self, t):
        """Gene's and the victim's bone matrices (world) at frame t with the fit; Gene's model -> world too."""
        f = self.frames[t]
        gv = curve(self.fit['gene'].get(333), t, 14)
        vv = curve(self.fit['victim'].get(self.vmotion), t, 2)
        Wg, Pg, Lg = pose_pre(self.gene.skel, self.gm, t, gene_pre(gv), hip_dy=gv[3] + (gv[13] if len(gv) > 13 else 0))
        vpre = self.victim_pre(t) if self.victim_pre else None
        Wv, Pv, Lv = pose_pre(self.vic.skel, self.vm, t, vpre, hip_dy=vv[0], bend=D(vv[1]))
        yaw = f['gyaw'] + D(gv[2])
        gpos = f['gpos'] + roty(f['gyaw']) @ np.array([gv[1], 0, gv[0]]) * self.pscale
        G = dict(R=roty(yaw), s=self.gscale, p=gpos)
        V = dict(R=roty(f['vyaw']), s=self.vscale, p=f['vpos'])
        return dict(t=t, gv=gv, Wg=Wg, Pg=Pg, Lg=Lg, Wv=Wv, Pv=Pv, Lv=Lv, G=G, V=V)


def to_world(X, p): return X['p'] + (X['R'] @ (X['s'] * np.asarray(p).T)).T
def to_model(X, w): return (X['R'].T @ (np.asarray(w) - X['p']).T).T / X['s']


# ---- the bust ----
def ray_z(V, T, x, y):
    """Front-most hit of the ray (x, y, +inf -> -inf) on the mesh: point and outward normal."""
    best = None
    for a, b, c in T:
        A, B, C = V[a], V[b], V[c]
        e1, e2 = B - A, C - A
        det = e1[0] * e2[1] - e1[1] * e2[0]
        if abs(det) < 1e-12:
            continue
        u = ((x - A[0]) * e2[1] - (y - A[1]) * e2[0]) / det
        w = (e1[0] * (y - A[1]) - e1[1] * (x - A[0])) / det
        if u < 0 or w < 0 or u + w > 1:
            continue
        z = A[2] + u * e1[2] + w * e2[2]
        if best is None or z > best[0]:
            n = np.cross(e1, e2); n = n / np.linalg.norm(n)
            if n[2] < 0:
                n = -n
            best = (z, n)
    return np.array([x, y, best[0]]), best[1]


def bust_points(vic):
    """Per side (+1 = her left, x > 0): the breast centre on her rest surface and its normal."""
    m = vic.mesh
    return {sd: ray_z(m['V'], m['T'], sd * BREAST_X, BREAST_Y) for sd in (1, -1)}


def money_bone(s):
    """Bone 29's rotation and scale in bone 2's frame under Money s (jiggle held at 0)."""
    return rx((s - 1.0) * D(30)) * s


# ---- the arm ----
def unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def rot_between(a, b):
    a, b = unit(a), unit(b)
    v = np.cross(a, b); c = float(np.dot(a, b)); s = np.linalg.norm(v)
    if s < 1e-10:
        return np.eye(3)
    return Rot.from_rotvec(v / s * math.atan2(s, c)).as_matrix()


def frame_of(f, n):
    e1 = unit(f); e2 = unit(n - np.dot(n, e1) * e1)
    return np.column_stack([e1, e2, np.cross(e1, e2)])


class Arm:
    def __init__(self, skel, mesh, side):
        self.side = side
        self.c, self.a, self.e, self.t, self.h, self.helper = ARMS[side]
        rest = mesh['rest']
        sx = -1.0 if side == 'R' else 1.0
        self.sx = sx
        self.f_loc = np.array([sx, 0, 0.0])          # fingers along the arm (rest)
        self.n_loc = np.array([0, -1.0, 0])          # palm faces down in the T-pose
        # RH_00 / LH_00 is a half-curled hand: the cup's inner surface (palm under the knuckles) goes on the breast
        self.palm = np.array([sx * PALM_ALONG, -PALM_DEPTH, 0.015])
        self.L1 = np.linalg.norm(skel['bones'][self.e]['t'])
        self.L2 = np.linalg.norm(skel['bones'][self.t]['t']) + np.linalg.norm(skel['bones'][self.h]['t'])


def fk_arm(skel, W, P, L, arm, local):
    """World matrices / positions of the arm chain from its parent's, with new local rotations."""
    bones = skel['bones']; W = list(W); P = list(P)
    for b in (arm.c, arm.helper, arm.a, arm.e, arm.t, arm.h):
        p = bones[b]['parent']
        R = local.get(b, L[b])
        W[b] = W[p] @ R; P[b] = P[p] + W[p] @ np.array(bones[b]['t'], float)
    return W, P


def hand_rot(arm, palm_n, fingers):
    """The hand's model-space rotation with its fingers along `fingers` and its palm facing `palm_n`."""
    return frame_of(fingers, palm_n) @ frame_of(arm.f_loc, arm.n_loc).T


def solve_arm(skel, W, P, L, arm, palm_pt, Rt, pole, clav=None):
    """New local rotations (clavicle, upper arm, forearm, hand) putting the palm at palm_pt with the hand at Rt (all in
    Gene's model space). Returns (locals, info)."""
    loc = {}
    wrist = palm_pt - Rt @ arm.palm
    W1, P1 = fk_arm(skel, W, P, L, arm, loc)
    S = P1[arm.a]; reach = arm.L1 + arm.L2
    info = dict(clav=0.0, short=0.0)
    C = P1[arm.c]; v0 = S - C; ax = np.cross(unit(v0), unit(wrist - C))
    lim = min(math.acos(np.clip(np.dot(unit(v0), unit(wrist - C)), -1, 1)), CLAV_MAX)
    need = 0.0   # the clavicle turn (toward the wrist) the reach needs
    if np.linalg.norm(wrist - S) > reach * 0.985 and np.linalg.norm(ax) > 1e-9:
        for need in np.linspace(0, lim, 61):
            if np.linalg.norm(wrist - (C + Rot.from_rotvec(unit(ax) * need).as_matrix() @ v0)) <= reach * 0.985:
                break
    info['need'] = need
    th = need if clav is None else min(clav, lim)
    if th > 1e-6 and np.linalg.norm(ax) > 1e-9:
        ax = unit(ax)
        Wp = W1[skel['bones'][arm.c]['parent']]
        loc[arm.c] = Wp.T @ Rot.from_rotvec(ax * th).as_matrix() @ Wp @ L[arm.c]
        info['clav'] = math.degrees(th)
        W1, P1 = fk_arm(skel, W, P, L, arm, loc)
        S = P1[arm.a]
    d = np.linalg.norm(wrist - S); u = (wrist - S) / d
    dc = float(np.clip(d, abs(arm.L1 - arm.L2) + 1e-4, reach * 0.9995))
    info['short'] = max(0.0, d - reach * 0.9995)
    Wc = S + u * dc
    aa = (arm.L1 ** 2 - arm.L2 ** 2 + dc ** 2) / (2 * dc); hh = math.sqrt(max(arm.L1 ** 2 - aa ** 2, 0.0))
    pp = pole - u * np.dot(pole, u)
    if np.linalg.norm(pp) < 1e-6:
        pp = (P1[arm.e] - S) - u * np.dot(P1[arm.e] - S, u)
    pp = unit(pp)
    E = S + u * aa + pp * hh
    # The elbow as a hinge (Gene's motions bend it about the forearm's y only): the upper arm's x along the upper arm,
    # its y the hinge (normal to the arm's plane, signed so the bend has the sign the motion's own elbow has), the
    # forearm a pure y turn. A free forearm took a twist and sat near Euler gimbal lock (y -90), which the motion
    # format can't interpolate at half frames (60 FPS wobbled 13 deg).
    u1 = unit(E - S); u2 = unit(Wc - E)
    best = None
    for hs in (1.0, -1.0):
        h = unit(np.cross(u, pp)) * hs
        Wa = np.column_stack([arm.sx * u1, h, np.cross(arm.sx * u1, h)])
        bend = math.acos(np.clip(np.dot(u1, u2), -1.0, 1.0))
        for beta in (bend, -bend):
            err = 1.0 - np.dot(Wa @ ry(beta) @ np.array([arm.sx, 0, 0]), u2)
            # Gene's elbows bend one way: the left about -y, the right about +y (rotY(L elbow) / rotY(-R elbow))
            good = beta * -arm.sx >= -1e-6
            if err < 1e-4 and good and (best is None or err < best[0]):
                best = (err, Wa, beta)
    _, Wa, beta = best
    loc[arm.a] = W1[arm.c].T @ Wa
    loc[arm.e] = ry(beta)
    W1, P1 = fk_arm(skel, W, P, L, arm, loc)
    loc[arm.h] = W1[arm.t].T @ Rt
    W1, P1 = fk_arm(skel, W, P, L, arm, loc)
    info['palm_err'] = float(np.linalg.norm(P1[arm.h] + W1[arm.h] @ arm.palm - palm_pt))
    return loc, info


def smooth(x):
    x = min(max(float(x), 0.0), 1.0)
    return x * x * (3 - 2 * x)


def slerp(A, B, w):
    if w <= 0:
        return A
    if w >= 1:
        return B
    r = Rot.from_matrix(np.stack([A, B]))
    from scipy.spatial.transform import Slerp
    return Slerp([0, 1], r)([w]).as_matrix()[0]


class Chest:
    """The solve for one pair: hold window, per-frame arm locals at a Money value."""

    def __init__(self, pair, hold=None, hand_up=HAND_UP, pole=None, sink=None, low=PALM_LOW):
        self.pair = pair
        self.hand_up = D(hand_up)                 # fingers turned up in the chest's plane (deg; K: a grab, not a cup)
        self.pole = pole or ELBOW_POLE
        self.sink = SINK if sink is None else sink
        self.low = low                            # the palm's spot this far below the breast centre (m)
        self._clav = {}
        self.arms = {s: Arm(pair.gene.skel, pair.gmesh, s) for s in 'LR'}
        self.bust = bust_points(pair.vic)
        self.hold = hold or self.find_hold()
        pair.victim_pre = self.victim_arm

    def victim_arm(self, t):
        """Her right arm (the one hanging by her side, Gene's right hand comes in under it) raised out from her body,
        a little ahead of his hands, so his forearm passes between her arm and her side (K's elbow fix)."""
        w = self.weight(min(t + ARM_LEAD, self.hold[1])) if t < self.hold[1] else self.weight(t)
        r = D(VICTIM_RAISE) * w
        return {7: rz(-r), 6: rz(-r * 0.5)} if r else None

    def find_hold(self):
        """The waist grab: both wrists within 30 cm of her hips; ends when she lands (the slam, her chest low)."""
        near = []; low = []
        for t in range(len(self.pair.frames)):
            q = self.pair.at(t)
            hips = to_world(q['V'], q['Pv'][0]); chest = to_world(q['V'], q['Pv'][2])
            d = max(np.linalg.norm(to_world(q['G'], q['Pg'][ARMS[s][4]]) - hips) for s in 'LR')
            near.append(d < 0.30); low.append(chest[1] < 0.45)
        h0 = next(t for t, v in enumerate(near) if v)
        land = next(t for t, v in enumerate(low) if v and t > h0)
        return h0, land - LAND_EARLY

    def weight(self, t):
        h0, h1 = self.hold
        if t < h0:
            return smooth((t - (h0 - RAMP_IN)) / RAMP_IN)
        if t <= h1:
            return 1.0
        return 1.0 - smooth((t - h1) / RAMP_OUT)

    def assign(self, q):
        """Gene's hand -> her breast: the side whose wrists are nearer over the hold."""
        return {'L': 1, 'R': -1}

    def targets(self, q, money, sd):
        """Palm point, palm normal (into her), finger direction for breast side sd, in Gene's model space."""
        c, n = self.bust[sd]
        rest = self.pair.vmesh['rest']
        Wv, Pv = q['Wv'], q['Pv']
        B = money_bone(money)
        p_model = Pv[29] + Wv[2] @ (B @ (c - np.array([0, self.low, 0]) - rest[29]))
        n_model = unit(Wv[2] @ rx((money - 1.0) * D(30)) @ n)
        fv = np.array([-sd * FINGERS[0], FINGERS[1], FINGERS[2]])
        if self.hand_up:
            fv = Rot.from_rotvec(unit(np.cross(fv, [0, 1.0, 0])) * self.hand_up).as_matrix() @ fv
        fing = unit(Wv[2] @ fv)
        pw = to_world(q['V'], p_model - n_model * self.sink)
        nw = q['V']['R'] @ n_model; fw = q['V']['R'] @ fing
        pg = to_model(q['G'], pw); ng = q['G']['R'].T @ nw; fg = q['G']['R'].T @ fw
        return pg, -ng, unit(fg - np.dot(fg, ng) * ng)

    def clavicles(self, money):
        """The clavicle turn per side and frame: what the reach needs, spread over the frames round it (a tent of 4 each
        way, then a 3-frame average) so the shoulders shrug in and out instead of popping (they did, 0 -> 30 deg in a
        frame)."""
        key = round(money, 3)
        if key in self._clav:
            return self._clav[key]
        n = len(self.pair.frames)
        need = {s_: np.zeros(n) for s_ in 'LR'}
        for t in range(n):
            if self.weight(t) > 0:
                for s_, inf in self._solve(t, money, None)[3].items():
                    need[s_][t] = inf['need']
        out = {}
        for s_, v in need.items():
            tent = np.array([max(v[max(0, min(n - 1, t + k))] * (1 - abs(k) / 5.0) for k in range(-4, 5)) for t in range(n)])
            out[s_] = np.convolve(np.pad(tent, 1, mode='edge'), np.ones(3) / 3, mode='valid')
        self._clav[key] = out
        return out

    def solve(self, t, money):
        return self._solve(t, money, self.clavicles(money))

    def _solve(self, t, money, clavs):
        """After she lands (t > h1) the arms let go joint by joint: from their pose at the landing (as locals) back to the
        motion's, by the blend; chasing her chest as she bounces away stretched them and snapped them back."""
        h1 = self.hold[1]
        if t > h1 and self.weight(t) > 0:
            q = self.pair.at(t); w = self.weight(t)
            _, last, _, info = self._solve_hold(h1, money, clavs)
            loc = {b: slerp(q['Lg'][b], R, w) for b, R in last.items()}
            return q, loc, w, info
        return self._solve_hold(t, money, clavs)

    def _solve_hold(self, t, money, clavs):
        """Gene's new arm locals at frame t. In the hold the palms sit on the bust; on the way in and out the palm
        travels from where the game's motion has it to the breast along a path bowed out from her body (BUMP), the
        hand turning on the way, so the hand never sweeps through her (a blend of joint angles did)."""
        q = self.pair.at(t)
        w = self.weight(t)
        loc = {}
        info = {}
        if w > 0:
            Wg, Pg, Lg = q['Wg'], q['Pg'], q['Lg']
            for s, sd in self.assign(q).items():
                arm = self.arms[s]
                pt, pn, fg = self.targets(q, money, sd)
                Rt = hand_rot(arm, pn, fg)
                pole = unit(Wg[2] @ np.array([arm.sx * self.pole[0], self.pole[1], self.pole[2]]))
                if w < 1:
                    p0 = Pg[arm.h] + Wg[arm.h] @ arm.palm
                    S, E, H = Pg[arm.a], Pg[arm.e], Pg[arm.h]
                    u = unit(H - S); pole0 = unit((E - S) - u * np.dot(E - S, u))
                    ctrl = pt - pn * BUMP   # in front of the breast: the hand comes onto it from the front
                    pt = p0 * (1 - w) ** 2 + ctrl * 2 * w * (1 - w) + pt * w * w
                    Rt = slerp(Wg[arm.h], Rt, min(1.0, w * TURN_LEAD))
                    pole = unit(pole0 * (1 - w) + pole * w)
                l, inf = solve_arm(self.pair.gene.skel, Wg, Pg, Lg, arm, pt, Rt, pole, None if clavs is None else clavs[s][t])
                info[s] = inf
                if w < EDGE:   # near the ends: the IK's own twist of the arm handed back to the motion's, joint by joint
                    l = {b: slerp(Lg[b], R, w / EDGE) for b, R in l.items()}
                loc.update(l)
                if arm.a in l:   # the shoulder helper follows the upper arm's change at half
                    dR = l[arm.a] @ Lg[arm.a].T
                    loc[arm.helper] = slerp(np.eye(3), dR, 0.5) @ Lg[arm.helper]
        return q, loc, w, info


def held_fit(body, scale=1.2, size=0.95, keys=None, ground=0.45):
    """A held fit at a larger size (K, Nov 15: keep her size, at most 5% off; her knees take up the height): the 1.00
    fit's curves, with Gene's placement (forward, side) and her hip height solved per key so her chest (bone 2) sits
    where it does against Gene at 1.00. Linear: her hips move her whole upper body straight up, Gene's placement moves
    him flat. Her height eases back to the 1.00 fit's once her chest is near the floor (she lands)."""
    base = Pair(body, 1.0)
    f10 = base.fit
    keys = keys or list(range(0, 96, 4)) + [95]
    fit = dict(size=size, gene={333: [list(x) for x in []]}, victim={})
    trial = Pair(body, scale, scale, fit=dict(size=size, gene=f10['gene'], victim=f10['victim']))
    gk, vk = [], []
    for t in keys:
        a = base.at(t); b = trial.at(t)
        c1 = a['G']['R'].T @ (to_world(a['V'], a['Pv'][2]) - a['G']['p'])        # her chest in Gene's frame at 1.00
        cw = to_world(b['V'], b['Pv'][2])
        landed = cw[1] < ground * scale or t > 44
        gv = list(curve(f10['gene'].get(333), t, 14)); vv = list(curve(f10['victim'].get(329), t, 2))
        if not landed:
            R = b['G']['R']; want = b['G']['p'] + R @ c1                        # where her chest should be
            dh = (want[1] - cw[1]) / trial.vscale
            vv[0] = float(np.clip(vv[0] + dh, -0.15, 0.25))
            cw = cw + np.array([0, (vv[0] - curve(f10['victim'].get(329), t, 2)[0]) * trial.vscale, 0])
            miss = cw - want                                                    # Gene moves by the rest, flat
            Rg = roty(trial.frames[t]['gyaw'])
            loc = Rg.T @ miss / trial.pscale
            gv[1] += loc[0]; gv[0] += loc[2]
        gk.append((t, gv)); vk.append((t, vv))
    fit['gene'] = {333: gk}
    fit['victim'] = {329: vk, 331: vk}
    return Pair(body, scale, scale, fit=fit)


def posed_meshes(pair, q, loc, money):
    """Skinned world vertices: Gene with the new arm locals, the victim with Money's bust."""
    gv = q['gv']
    Wg, Pg, _ = pose_pre(pair.gene.skel, pair.gm, q['t'], gene_pre(gv), hip_dy=gv[3] + (gv[13] if len(gv) > 13 else 0), local=loc)
    G = to_world(q['G'], skin(pair.gmesh, Wg, Pg))
    Wv = list(q['Wv']); Pv = list(q['Pv'])
    Wv[29] = Wv[2] @ money_bone(money)       # the bust: Money's scale + droop (skin() takes the matrix as is)
    V = to_world(q['V'], skin(pair.vmesh, Wv, Pv))
    return G, V, Wg, Pg


def strip(pair, ch, frames, money, view, out, size=(240, 300), zoom=None, label=None):
    """A row of frames (Gene blue, the victim orange); zoom = span in metres round her chest, else the whole pair."""
    from raster import render
    from PIL import Image, ImageDraw
    rows = []
    for t in frames:
        q, loc, w, info = ch.solve(t, money)
        G, V, _, _ = posed_meshes(pair, q, loc, money)
        rows.append((t, w, info, G, V, to_world(q['V'], q['Pv'][2])))
    if zoom:
        spans = []
    allv = np.concatenate([np.concatenate([r[3], r[4]]) for r in rows]) @ view.T
    lo = allv[:, :2].min(0); hi = allv[:, :2].max(0); c = (lo + hi) / 2; span = (hi - lo).max() * 1.05
    ims = []
    for t, w, info, G, V, chest in rows:
        cc, sp = ((view @ chest)[:2], zoom) if zoom else (c, span)
        im, _ = render([(G, pair.gmesh['T'], (90, 120, 200)), (V, pair.vmesh['T'], (220, 140, 80))], view, size=size, extent=(cc, sp))
        pim = Image.fromarray(im)
        ImageDraw.Draw(pim).text((6, 4), (label + ' ' if label else '') + 'f%d' % t, fill=(0, 0, 0))
        ims.append(pim)
    sh = Image.new('RGB', (sum(i.width for i in ims), ims[0].height), 'white'); x = 0
    for i in ims:
        sh.paste(i, (x, 0)); x += i.width
    sh.save(out)
    return out


def view_of(yaw, pitch):
    return rx(D(pitch)) @ ry(D(yaw))


# ---- the data the runtime plays (punish_fit.txt "pose" / "bust" lines) ----
GENE_BONES = (5, 6, 7, 8, 10, 11, 12, 13, 14, 16)
MONEY = (0.85, 1.0, 1.3)


def own_local(mot, b, t):
    return rot([mot.get(b, 3 + c, t) for c in range(3)])


def gene_pre_frames(ch, money):
    """Per frame of the window: bone -> the whole pre-rotation (new local . the motion's own^-1), as rotation vectors
    (degrees). Outside the window the fit's own pose applies (the runtime)."""
    pair = ch.pair; out = {}
    h0, h1 = ch.hold
    for t in range(max(0, h0 - RAMP_IN), min(len(pair.frames), h1 + RAMP_OUT + 1)):
        q, loc, w, info = ch.solve(t, money)
        row = {}
        for b in GENE_BONES:
            L = loc.get(b, q['Lg'][b])
            row[b] = Rot.from_matrix(L @ own_local(pair.gm, b, t).T).as_rotvec(degrees=True)
        out[t] = row
    return out


def victim_pre_frames(ch, vmotion):
    out = {}
    h0, h1 = ch.hold
    for t in range(max(0, h0 - RAMP_IN - ARM_LEAD), min(len(ch.pair.frames), h1 + RAMP_OUT + 1)):
        pre = ch.victim_arm(t) or {}
        out[t] = {b: Rot.from_matrix(pre.get(b, np.eye(3))).as_rotvec(degrees=True) for b in (7, 6)}
    return out


WOMEN = [('2ed83e81,ed288b2c,51dc32d6', 'em16'), ('3fed1598', 'em52'), ('43f1d30c', 'em77')]
SIZES = ((1.0, None), (1.2, 0.95))   # enemy size, size= (None: the 1.00 fit's own)


def fmt_curve(keys, n, prec):
    return ' '.join('%d:%s' % (t, ','.join(('%.' + str(prec[i]) + 'f') % v[i] for i in range(min(n, len(v))))) for t, v in keys)


def block(keys, file, scale, size):
    body = EX + '/em/' + file + '.dat'
    pair = Pair(body, 1.0) if size is None else held_fit(body, scale, size)
    ch = Chest(pair)
    gk = pair.fit['gene'][333]
    gp = (3, 3, 2, 3) + (1,) * 10
    lines = ['fit Suplex %s scale=%.2f size=%.3f frontcam   # %s (women): the chest grab (chest_suplex.py, K Nov 15)%s'
             % (keys, scale, pair.fit['size'], file, '' if size is None else ', her size held, her knees bent')]
    lines.append('gene 333 ' + fmt_curve([(t, list(v)) for t, v in gk], 14 if len(gk[0][1]) > 4 else 4, gp))
    for vm in (329, 331):
        vk = pair.fit['victim'].get(vm) or pair.fit['victim'][329]
        lines.append('victim %d ' % vm + fmt_curve([(t, list(v)) for t, v in vk], 2, (3, 2)))
    for m in MONEY:
        S = gene_pre_frames(ch, m)
        for b in GENE_BONES:
            lines.append('pose gene 333 %.2f %d ' % (m, b) + ' '.join('%d:%.1f,%.1f,%.1f' % ((t,) + tuple(S[t][b])) for t in sorted(S)))
    for vm in (329, 331):
        V = victim_pre_frames(ch, vm)
        for b in (7, 6):
            lines.append('pose victim %d 0 %d ' % (vm, b) + ' '.join('%d:%.1f,%.1f,%.1f' % ((t,) + tuple(V[t][b])) for t in sorted(V)))
    # the camera turned to the pair's front: half a turn round the pair's centre (the two chests) in Gene's model space,
    # frame by frame, so it keeps the original's distance (round its look-at point it ran up to 4 m further; K)
    piv = []
    for t in range(0, len(pair.frames), 2):
        q = pair.at(t)
        m_ = (q['Pg'][2] + to_model(q['G'], to_world(q['V'], q['Pv'][2]))) / 2
        piv.append('%d:%.3f,%.3f' % (t, m_[0], m_[2]))
    lines.append('cam 333 ' + ' '.join(piv))
    h0, h1 = ch.hold
    lines.append('bust 333 ' + ' '.join('%d:%.2f' % (t, ch.weight(t)) for t in range(h0 - RAMP_IN, h1 + RAMP_OUT + 1)))
    return lines


def emit(path=PF):
    """Replace the women's Suplex fits in punish_fit.txt (idempotent; run after emit7)."""
    src = open(path).read().split('\n')
    women = {k for keys, _ in WOMEN for k in keys.split(',')}
    out, skip, placed = [], False, False
    for line in src:
        if line.startswith('fit '):
            p = line.split()
            skip = p[1] == 'Suplex' and bool(women & set(p[2].split(',')))
            if skip and not placed:
                for keys, file in WOMEN:
                    for scale, size in SIZES:
                        out += block(keys, file, scale, size)
                        print('emitted', file, scale)
                placed = True
        elif skip and not line.startswith(('gene ', 'victim ', 'pose ', 'bust ', 'cam ')):
            skip = False
        if not skip:
            out.append(line)
    open(path, 'w', newline='\n').write('\n'.join(out))


if __name__ == '__main__' and sys.argv[1:2] == ['emit']:
    emit()
elif __name__ == '__main__':
    body, esize, money, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    frames = [int(x) for x in (sys.argv[5] if len(sys.argv) > 5 else '8,12,15,17,20,26,32,36,40').split(',')]
    yaw = float(sys.argv[6]) if len(sys.argv) > 6 else 20.0
    zoom = float(sys.argv[7]) if len(sys.argv) > 7 else None
    pair = Pair(EX + '/em/' + body + '.dat', esize)
    ch = Chest(pair)
    print('hold', ch.hold)
    strip(pair, ch, frames, money, view_of(yaw, 6), out, zoom=zoom)
