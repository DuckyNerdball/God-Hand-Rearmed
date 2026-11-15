"""The chest grab's re-pose (K, Nov 15, step 4; claude/boobplex.md): Gene's relaxed hands with the palm centred on the
fullest point of each breast (pressed 1.5 cm), each hand turned about its side-to-side axis until its deepest fingertip
sinks 1.5 cm into her (K picked 1.5 of 1.0 / 1.5 / 2.0), its fingers toward her middle line and up 30 deg as before,
turned further up only as far as it takes to keep the hand 5 mm off her middle line (K's "A": at Money 100 and below
the breasts sit so close that the two hands' index fingers crossed); the thumb rests along the hand's outer edge (under
the breast it would take a 107-128 deg wrist turn with his arms coming round her from behind). The grip is rigid in her
chest's frame (bone 2) through the hold.

Two busts: the original (Money <= 100%, and the women without the detailed one) and the detailed bust body_detail
builds for Money > 100% (transfer3 + crease; identical within 1.7 mm across the generic women, so one fit).

Her reaction (the fan's idea, K: yes): her left hand from her face onto his left forearm 6 cm behind the wrist over
f21 -> f27, held to f30, back into her motion by f33.

Everything is solved at whole and half frames (60 FPS) and written with 2 decimals.

    GODHAND_EXTRACT=<extract> python3 chest_suplex.py emit     (writes the women's Suplex fits, both busts)
"""
import os, sys, math, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', 'body_detail'))
from chest_suplex import *
from mesh import keep_node
import igl
import xfer

DEPTH = 0.015          # the deepest fingertip this far in her surface (K picked 1.5 cm)
PRESS = 0.015          # the palm centre pressed in (as before)
MARGIN = 0.005         # each hand this far off her middle line at least
UP_MAX = 90.0          # the fingers turned up at most this far (then the palm moves out)
CENTRES = {            # the breast centres in her rest space (+1 her left): the fullest point of each
    'em16': (0.066, 1.356),   # the detailed bust's peak; on the original bust (one ridged block) the same spot
    'em52': (0.082, 1.348),   # Shannon's own
    'em77': (0.065, 1.375),   # her bust sits higher (her bone 29 is not a bust bone)
}
REACT = (21, 27, 30, 33)       # her reach: from, held from, let go from, back in the motion by
GRAB_BACK = 0.06               # her palm this far behind his wrist along his forearm (his size)
FOREARM_R = 0.040              # his forearm's radius there (bone 15's skin)
HER_PRESS = 0.005
HER_POLE = (0.6, -0.7, -0.3)   # her elbow: out, down, back (her chest space, x by side)
HER_BUMP = 0.08
HER_BONES = (11, 12, 13, 14, 16)
SHORT = 0.012         # the hold ends before his arm would fall this far short of her (the palm is pressed 1.5 cm in)
LIFT = 27             # her feet leave the floor (Suplex 333): only a shortfall from here ends the hold
FWN = igl.SIGNED_DISTANCE_TYPE_FAST_WINDING_NUMBER


def sdf(P, V, T):
    return igl.signed_distance(np.ascontiguousarray(P, float), np.ascontiguousarray(V, float),
                               np.ascontiguousarray(T, np.int64), sign_type=FWN)[0]


# ---- the busts ----
_donor = None
def donor():
    global _donor
    if _donor is None:
        ents = dat_read(open(EX + '/el/elf4.dat', 'rb').read())
        md = next(i for i, (t, b, o) in enumerate(ents) if t == 'MD' and b)
        _donor = xfer.load_node('el/elf4.dat', md, ['body'])
    return _donor


def detailed_mesh(body_file):
    """Her punish_fit mesh with the body node's bust replaced by the detailed one (as body_detail builds it)."""
    sk = skeleton(EX + '/em/' + body_file + '.dat')
    names = [n['name'] for n in sk['nodes'] if keep_node(n['name'])]
    sm, _ = xfer.transfer3(xfer.load_node('em/' + body_file + '.dat', sk['md'], ['body']), donor(), levels=1)
    others = xfer.load_node('em/' + body_file + '.dat', sk['md'], [n for n in names if n != 'body'])
    def part(m):
        W = m['W'].astype(float); return m['P'], m['J'], W / np.maximum(W.sum(1, keepdims=True), 1e-9), m['T']
    P1, J1, W1, T1 = part(sm); P2, J2, W2, T2 = part(others)
    return dict(V=np.concatenate([P1, P2]), J=np.concatenate([J1, J2]), W=np.concatenate([W1, W2]),
                T=np.concatenate([T1, T2 + len(P1)]), rest=load_mesh(sk)['rest'])


# ---- the grip ----
def hand_parts(pair, side):
    """Gene's hand mesh (RH_00 / LH_00) in hand-local space, its triangles and the four fingers' last 6 cm."""
    gm = pair.gmesh; hb = ARMS[side][4]; rest = gm['rest']
    dom = gm['J'][np.arange(len(gm['J'])), gm['W'].argmax(1)]
    sel = dom == hb; idx = np.where(sel)[0]; rm = -np.ones(len(sel), int); rm[idx] = np.arange(len(idx))
    T = rm[gm['T'][sel[gm['T']].all(1)]]; H = gm['V'][idx] - rest[hb]
    sx = -1.0 if side == 'R' else 1.0
    return H, T, (sx * H[:, 0] > 0.115) & (H[:, 2] < 0.05)


def surface(mesh, s):
    """Her rest mesh with Money s on the bust (jiggle 0)."""
    n = len(mesh['rest']); W = [np.eye(3)] * n; W[29] = money_bone(s)
    return skin(mesh, W, list(mesh['rest']))


def point_normal(V, T, x, y, r=0.02):
    """The surface point straight in front at (x, y) and its normal (averaged over 2 cm, as the hit face)."""
    c = V[T].mean(1)
    p, nh = ray_z(V, T[(np.abs(c[:, 0] - x) < 0.05) & (np.abs(c[:, 1] - y) < 0.05)], x, y)
    near = np.linalg.norm(c - p, axis=1) < r
    f = np.cross(V[T[near, 1]] - V[T[near, 0]], V[T[near, 2]] - V[T[near, 0]])
    n = unit((f * np.sign(f @ nh + 1e-12)[:, None]).sum(0))
    return p, (n if n @ nh > 0.3 else nh)


class Grip:
    def __init__(self, pair, centre, mesh=None):
        self.pair, self.centre, self.mesh = pair, centre, mesh or pair.vmesh
        self.hands = {s: hand_parts(pair, s) for s in 'LR'}
        self.arms = {s: Arm(pair.gene.skel, pair.gmesh, s) for s in 'LR'}
        self._c = {}; self._rule = {}

    def place(self, s, side, sd, up=HAND_UP, shift=0.0, depth=DEPTH):
        """(palm point, hand rotation) in her rest model space for Gene's hand `side` on breast sd at Money s."""
        key = (round(s, 4), side, sd, round(up, 2), round(shift, 4), round(depth, 4))
        if key in self._c:
            return self._c[key]
        V = surface(self.mesh, s); T = self.mesh['T']; rest = self.mesh['rest']
        x, y = self.centre[sd]; x += sd * shift
        c0, n0 = point_normal(surface(self.mesh, 1.0), T, x, y)
        c = rest[29] + money_bone(s) @ (c0 - rest[29]); n = unit(rx((s - 1.0) * D(30)) @ n0)
        fv = np.array([-sd * FINGERS[0], FINGERS[1], FINGERS[2]])
        fv = Rot.from_rotvec(unit(np.cross(fv, [0, 1.0, 0])) * D(up)).as_matrix() @ fv
        f = unit(fv - np.dot(fv, n) * n)
        arm = self.arms[side]; R0 = hand_rot(arm, -n, f); P = c - n * PRESS
        H, HT, fing = self.hands[side]; k = unit(np.cross(f, -n))
        def world(phi):
            R = Rot.from_rotvec(k * phi).as_matrix() @ R0
            return R, P + (R @ (H - arm.palm).T).T
        lo, hi = D(-60), D(60)
        for _ in range(40):   # the turn that sinks the deepest fingertip `depth` in
            mid = (lo + hi) / 2
            if -sdf(world(mid)[1][fing], V, T).min() < depth: lo = mid
            else: hi = mid
        R, X = world((lo + hi) / 2); d = sdf(X, V, T)
        out = dict(P=P, R=R, X=X, phi=math.degrees((lo + hi) / 2), sink=-d[fing].min(), up=up, shift=shift)
        self._c[key] = out
        return out

    def rule(self, s, side='L', sd=1):
        """K's "A": the smallest up-turn (from 30 deg) keeping the hand MARGIN off her middle line; past UP_MAX the palm
        moves out. Per hand: em77's bust bone sits off her middle, so Money moves her two sides differently."""
        k = (round(s, 4), side)
        if k not in self._rule:
            up, shift = HAND_UP, 0.0
            while up < UP_MAX and (sd * self.place(s, side, sd, up)['X'][:, 0]).min() < MARGIN:
                up += 1.0
            while (sd * self.place(s, side, sd, up, shift)['X'][:, 0]).min() < MARGIN and shift < 0.05:
                shift += 0.001
            self._rule[k] = (up, shift)
        return self._rule[k]

    def hand(self, s, side, sd):
        up, shift = self.rule(s, side, sd)
        return self.place(s, side, sd, up, shift)


# ---- half frames ----
def pair_at(pair, t):
    """Pair.at at any time t: the motions and curves sampled at t, the placement lerped between whole frames (the
    runtime moves the pair in half steps at 60 FPS)."""
    n = len(pair.frames); i = min(int(math.floor(t + 1e-9)), n - 1); a = min(max(t - i, 0.0), 1.0)
    f0, f1 = pair.frames[i], pair.frames[min(i + 1, n - 1)]
    def ang(y0, y1): return y0 + ((y1 - y0 + math.pi) % (2 * math.pi) - math.pi) * a
    gpos = f0['gpos'] * (1 - a) + f1['gpos'] * a; gyaw = ang(f0['gyaw'], f1['gyaw'])
    vpos = f0['vpos'] * (1 - a) + f1['vpos'] * a; vyaw = ang(f0['vyaw'], f1['vyaw'])
    gv = curve(pair.fit['gene'].get(333), t, 14); vv = curve(pair.fit['victim'].get(pair.vmotion), t, 2)
    Wg, Pg, Lg = pose_pre(pair.gene.skel, pair.gm, t, gene_pre(gv), hip_dy=gv[3] + (gv[13] if len(gv) > 13 else 0))
    vpre = pair.victim_pre(t) if pair.victim_pre else None
    Wv, Pv, Lv = pose_pre(pair.vic.skel, pair.vm, t, vpre, hip_dy=vv[0], bend=D(vv[1]))
    G = dict(R=roty(gyaw + D(gv[2])), s=pair.gscale, p=gpos + roty(gyaw) @ np.array([gv[1], 0, gv[0]]) * pair.pscale)
    V = dict(R=roty(vyaw), s=pair.vscale, p=vpos)
    return dict(t=t, gv=gv, Wg=Wg, Pg=Pg, Lg=Lg, Wv=Wv, Pv=Pv, Lv=Lv, G=G, V=V)


class HalfPair(Pair):
    def at(self, t):
        return pair_at(self, t)


def half(pair):
    pair.__class__ = HalfPair
    return pair


# ---- the solve ----
class Grab(Chest):
    """The chest grab with the grip rigid in her chest frame and her reaction; any time t (half frames)."""

    def __init__(self, pair, grip, react=True, monies=None, **kw):
        self.grip = grip
        self.monies = monies or MONEY_ORIG
        self.react = react; self._her = {}
        super().__init__(half(pair), **kw)
        self.her = Arm(pair.vic.skel, pair.vmesh, 'L')
        self.hold = self.reach_hold(self.hold)

    def reach_hold(self, hold):
        """The hold ends before the first frame his arms would fall short of her by more than SHORT (the palm, pressed
        1.5 cm in, would come off her): at size 1.20 she flips over his shoulder out of his reach at f32-34 (up to 5 cm
        short), so there his hands let go from f31. Only from the lift on (her feet leave the floor, f27): before it a
        shortfall is a dip of a frame or so (around f23 at 1.20, up to 1.2 cm) that only presses the palm less."""
        h0, end = hold
        for m in self.monies:
            clav = self.clavicles(m)
            for t in window(max(h0 + 1, LIFT), hold[1]):
                if any(i['short'] > SHORT for i in self._solve(t, m, clav)[3].values()):
                    end = min(end, int(math.floor(t)) - 1)
                    break
        self._clav.clear()
        return h0, end

    def targets2(self, q, money, side, sd):
        """Palm point and hand rotation in Gene's model space."""
        r = self.grip.hand(money, side, sd)
        rest = self.grip.mesh['rest']; Wv, Pv = q['Wv'], q['Pv']
        pw = to_world(q['V'], Pv[2] + Wv[2] @ (r['P'] - rest[2])); Rw = q['V']['R'] @ Wv[2] @ r['R']
        return to_model(q['G'], pw), q['G']['R'].T @ Rw

    def targets(self, q, money, sd):
        side = 'L' if sd == 1 else 'R'
        pt, Rt = self.targets2(q, money, side, sd)
        return pt, Rt @ self.arms[side].n_loc, Rt @ self.arms[side].f_loc

    def _solve_hold(self, t, money, clavs):
        q = self.pair.at(t); w = self.weight(t); loc = {}; info = {}
        if w > 0:
            Wg, Pg, Lg = q['Wg'], q['Pg'], q['Lg']
            for s, sd in self.assign(q).items():
                arm = self.arms[s]
                pt, Rt = self.targets2(q, money, s, sd)
                pn = Rt @ arm.n_loc
                pole = unit(Wg[2] @ np.array([arm.sx * self.pole[0], self.pole[1], self.pole[2]]))
                if w < 1:
                    p0 = Pg[arm.h] + Wg[arm.h] @ arm.palm
                    S, E, H = Pg[arm.a], Pg[arm.e], Pg[arm.h]
                    u = unit(H - S); pole0 = unit((E - S) - u * np.dot(E - S, u))
                    ctrl = pt - pn * BUMP
                    pt = p0 * (1 - w) ** 2 + ctrl * 2 * w * (1 - w) + pt * w * w
                    Rt = slerp(Wg[arm.h], Rt, min(1.0, w * TURN_LEAD))
                    pole = unit(pole0 * (1 - w) + pole * w)
                clav = None
                if clavs is not None:
                    c = clavs[s]; i = int(math.floor(t)); a = t - i
                    clav = c[min(i, len(c) - 1)] * (1 - a) + c[min(i + 1, len(c) - 1)] * a
                l, inf = solve_arm(self.pair.gene.skel, Wg, Pg, Lg, arm, pt, Rt, pole, clav)
                info[s] = inf
                if w < EDGE:
                    l = {b: slerp(Lg[b], R, w / EDGE) for b, R in l.items()}
                loc.update(l)
                if arm.a in l:
                    dR = l[arm.a] @ Lg[arm.a].T
                    loc[arm.helper] = slerp(np.eye(3), dR, 0.5) @ Lg[arm.helper]
        return q, loc, w, info

    # her reaction
    def react_w(self, t):
        a, b, c, d = REACT
        if t <= a or t >= d: return 0.0
        if t < b: return smooth((t - a) / (b - a))
        if t <= c: return 1.0
        return 1.0 - smooth((t - c) / (d - c))

    def her_target(self, t, money):
        """Her palm point, hand rotation and elbow pole in her model space, at full weight."""
        q, loc, w, info = self.solve(t, money)
        gv = q['gv']
        Wg, Pg, _ = pose_pre(self.pair.gene.skel, self.pair.gm, t, gene_pre(gv), hip_dy=gv[3] + (gv[13] if len(gv) > 13 else 0), local=loc)
        e, wr = to_world(q['G'], Pg[14]), to_world(q['G'], Pg[16])
        u = unit(wr - e); A = wr - u * GRAB_BACK * self.pair.gscale
        C = to_world(q['V'], q['Pv'][2]); rad = A - C; rad[1] = 0; rad = unit(rad)
        a0 = unit(rad + np.array([0, 0.6, 0])); a = unit(a0 - np.dot(a0, u) * u)
        Pw = A + a * (FOREARM_R * self.pair.gscale - HER_PRESS)
        Vr = q['V']['R']; p = to_model(q['V'], Pw); n_in = Vr.T @ (-a); ax = Vr.T @ u
        arm = self.her; Wv = q['Wv']; best = None
        for sg in (1.0, -1.0):   # fingers round his forearm: the way nearest her motion's own hand
            R = hand_rot(arm, n_in, unit(np.cross(ax, -n_in)) * sg)
            ang = np.linalg.norm(Rot.from_matrix(R @ Wv[arm.h].T).as_rotvec())
            if best is None or ang < best[0]: best = (ang, R)
        pole = unit(Wv[2] @ np.array([arm.sx * HER_POLE[0], HER_POLE[1], HER_POLE[2]]))
        return q, p, best[1], pole, n_in

    def her_locals(self, t, money):
        key = (round(t, 3), round(money, 4))
        if key in self._her: return self._her[key]
        w = self.react_w(t); out = {}
        if self.react and w > 0:
            a, b, c, d = REACT
            if t > c:   # letting go: from the held pose back to the motion's, joint by joint
                held = self.her_locals(c, money); q = self.pair.at(t)
                out = {k: slerp(q['Lv'][k], R, w) for k, R in held.items()}
            else:
                q, p, Rt, pole, n_in = self.her_target(t, money)
                arm = self.her; Wv, Pv, Lv = q['Wv'], q['Pv'], q['Lv']
                if w < 1:
                    p0 = Pv[arm.h] + Wv[arm.h] @ arm.palm
                    S, E, H = Pv[arm.a], Pv[arm.e], Pv[arm.h]
                    uu = unit(H - S); pole0 = unit((E - S) - uu * np.dot(E - S, uu))
                    ctrl = p - n_in * HER_BUMP
                    p = p0 * (1 - w) ** 2 + ctrl * 2 * w * (1 - w) + p * w * w
                    Rt = slerp(Wv[arm.h], Rt, min(1.0, w * TURN_LEAD)); pole = unit(pole0 * (1 - w) + pole * w)
                l, _ = solve_arm(self.pair.vic.skel, Wv, Pv, Lv, arm, p, Rt, pole)
                if w < EDGE: l = {k: slerp(Lv[k], R, w / EDGE) for k, R in l.items()}
                if arm.a in l:
                    l[arm.helper] = slerp(np.eye(3), l[arm.a] @ Lv[arm.a].T, 0.5) @ Lv[arm.helper]
                out = l
        self._her[key] = out
        return out

    def victim_full(self, t, money):
        """Her pre-rotations: the right arm's raise and the left arm's reach."""
        pre = dict(self.victim_arm(t) or {})
        for b, R in self.her_locals(t, money).items():
            pre[b] = R @ own_local(self.pair.vm, b, t).T
        return pre

    def at_full(self, t, money):
        keep = self.pair.victim_pre; pre = self.victim_full(t, money)
        self.pair.victim_pre = lambda tt: pre
        try: return self.pair.at(t)
        finally: self.pair.victim_pre = keep


# ---- the data (punish_fit.txt) ----
STEP = 0.5
MONEY_ORIG = (0.85, 0.925, 1.0, 1.15, 1.3)
MONEY_DETAIL = (1.01, 1.15, 1.3)


def window(lo, hi):
    return [round(x, 1) for x in np.arange(lo, hi + 1e-6, STEP)]


def gene_lines(ch, money):
    pair = ch.pair; h0, h1 = ch.hold; out = {b: [] for b in GENE_BONES}
    for t in window(max(0, h0 - RAMP_IN), min(len(pair.frames) - 1, h1 + RAMP_OUT)):
        q, loc, w, info = ch.solve(t, money)
        for b in GENE_BONES:
            out[b].append((t, Rot.from_matrix(loc.get(b, q['Lg'][b]) @ own_local(pair.gm, b, t).T).as_rotvec(degrees=True)))
    return out


def victim_lines(ch, money):
    """Her left arm's reach per Money (it follows Gene's hand)."""
    a, b, c, d = REACT; out = {k: [] for k in HER_BONES}
    for t in window(a, d):
        loc = ch.her_locals(t, money)
        for k in HER_BONES:
            R = loc.get(k, own_local(ch.pair.vm, k, t))
            out[k].append((t, Rot.from_matrix(R @ own_local(ch.pair.vm, k, t).T).as_rotvec(degrees=True)))
    return out


def raise_lines(ch):
    h0, h1 = ch.hold; out = {7: [], 6: []}
    for t in window(max(0, h0 - RAMP_IN - ARM_LEAD), min(len(ch.pair.frames) - 1, h1 + RAMP_OUT)):
        pre = ch.victim_arm(t) or {}
        for k in (7, 6):
            out[k].append((t, Rot.from_matrix(pre.get(k, np.eye(3))).as_rotvec(degrees=True)))
    return out


def fmt(keys):
    return ' '.join('%s:%.2f,%.2f,%.2f' % (('%g' % t), v[0], v[1], v[2]) for t, v in keys)


def grab(file, scale, size, detail):
    body = EX + '/em/' + file + '.dat'
    pair = Pair(body, 1.0) if size is None else held_fit(body, scale, size)
    C = {1: CENTRES[file], -1: (-CENTRES[file][0], CENTRES[file][1])}
    grip = Grip(pair, C, detailed_mesh(file) if detail else None)
    return Grab(pair, grip, monies=MONEY_DETAIL if detail else MONEY_ORIG)


def block(keys, file, scale, size, detail=False):
    ch = grab(file, scale, size, detail); pair = ch.pair
    gk = pair.fit['gene'][333]
    gp = (3, 3, 2, 3) + (1,) * 10
    note = ', her size held, her knees bent' if size is not None else ''
    lines = ['fit Suplex %s%s scale=%.2f size=%.3f frontcam   # %s (women): the chest grab, %s bust (chest_grip.py, K Nov 15)%s'
             % (keys, ' bust=detail' if detail else '', scale, pair.fit['size'], file, 'the detailed' if detail else 'the original', note)]
    lines.append('gene 333 ' + fmt_curve([(t, list(v)) for t, v in gk], 14 if len(gk[0][1]) > 4 else 4, gp))
    for vm in (329, 331):
        vk = pair.fit['victim'].get(vm) or pair.fit['victim'][329]
        lines.append('victim %d ' % vm + fmt_curve([(t, list(v)) for t, v in vk], 2, (3, 2)))
    for m in (MONEY_DETAIL if detail else MONEY_ORIG):
        S = gene_lines(ch, m)
        for b in GENE_BONES:
            lines.append('pose gene 333 %.2f %d %s' % (m, b, fmt(S[b])))
    R = raise_lines(ch)
    for vm in (329, 331):
        for b in (7, 6):
            lines.append('pose victim %d 0 %d %s' % (vm, b, fmt(R[b])))
    for m in (MONEY_DETAIL if detail else MONEY_ORIG):
        H = victim_lines(ch, m)
        for vm in (329, 331):
            for b in HER_BONES:
                lines.append('pose victim %d %.2f %d %s' % (vm, m, b, fmt(H[b])))
    for v, kw in enumerate(CAMERAS):
        cam = Camera(ch, **kw); keys_ = []
        for t in range(0, cam.turn[1] + 1):
            e, g = cam.at(t)
            keys_.append('%d:%s' % (t, ','.join('%.3f' % x for x in list(e) + list(g))))
        lines.append('cam 333 %d ' % v + ' '.join(keys_))
    h0, h1 = ch.hold
    lines.append('bust 333 ' + ' '.join('%d:%.2f' % (t, ch.weight(t)) for t in range(h0 - RAMP_IN, h1 + RAMP_OUT + 1)))
    return lines
