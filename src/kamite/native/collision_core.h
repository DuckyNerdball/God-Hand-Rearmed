#pragma once

// Rung 4's collision maths (claude/rung4-collision-map.md) on plain Vec4s, portable (SSE, NEON, plain C++), shared by the
// natives (collision.cpp, which adds the guest context) and the offline checks (tools/native). Each op is the recompiled
// code's op in its order under the default float rules (libvu0_core.h, motion_core.h's Exact): exact for the inputs the
// natives' entry checks let in.

#include "kamite/native/motion_core.h"

#include <initializer_list>

namespace kamite::native::collision
{
    using motion::Exact;

    // c.lt.s / c.le.s / c.eq.s: the runtime's compare key (motion_core fpuKey: sign-magnitude, exponent-0 values as 0)
    inline bool lessThan(float a, float b) { return motion::fpuKey(floatBits(a)) < motion::fpuKey(floatBits(b)); }
    inline bool lessEqual(float a, float b) { return motion::fpuKey(floatBits(a)) <= motion::fpuKey(floatBits(b)); }
    inline bool equal(float a, float b) { return motion::fpuKey(floatBits(a)) == motion::fpuKey(floatBits(b)); }

    // ---- sceVu0InnerProduct 0x30A348 as a step (libvu0_core innerSummed): f0 = lane x --------------------------------
    inline float inner(Vec4 a, Vec4 b) { return lane<0>(libvu0::innerSummed(a, b)); }

    // ---- func_00133408 (src/cod/00133408.c): do the boxes amin..amax and bmin..bmax overlap? x, then z, then y ---------
    // The x tests fall through to the shared `return 0`; the z / y tests branch back to it (a taken backward branch: one
    // checkpoint, `loops`).
    inline bool boxesOverlap(const float amin[3], const float amax[3], const float bmin[3], const float bmax[3], int &loops)
    {
        loops = 0;
        if (lessThan(amax[0], bmin[0]) || lessThan(bmax[0], amin[0]))
            return false;
        for (int k : {2, 1})
            if (lessThan(amax[k], bmin[k]) || lessThan(bmax[k], amin[k]))
            {
                loops = 1;
                return false;
            }
        return true;
    }

    // ---- sceVu0Normalize as a step (libvu0_core normalize: vf6 = (v.xyz / |v|, 0)) -----------------------------------
    // The libvu0 native's own checks: v.xyz below 2^62, a sum of squares with exponent 1-254 (else a decline).
    inline bool normalizeStep(Vec4 vf0, Vec4 v, uint16_t &status, Vec4 &out)
    {
        if (!libvu0::finiteXyz(v) || !libvu0::ordinaryLength(libvu0::squaresSummed(v, v))) // libvu0.cpp normalizeInput
            return false;
        const libvu0::NormalizeResult n = libvu0::normalize(vf0, v, v, status);
        status = n.status;
        out = n.vf6;
        return true;
    }
    // vsub.xyz vf4, vf4 (a), vf5 (b): w = a's
    inline Vec4 subXyz(Vec4 a, Vec4 b) { return blend<7>(a, sub(a, b)); }

    // ---- func_00135EC8 (src/cod/00135ec8.c): the segment start..end against the plane (normal, a point of it) ------------
    // d = (end - start).xyz (w = end's); n = sceVu0Normalize(normal); t = (n.p - n.start) / n.d (three InnerProducts, in
    // that order); n.d == 0 (c.eq.s) -> 0; t outside [0, 1] (c.le.s 0, t; c.le.s t, 1) -> 0; else hit = start + d t
    // (vmulx.xyz, vadd.xyz: w = start's) and 1. Four guest calls, always. `ok` false: declined (a normalize of a zero /
    // huge normal, a quotient past FLT_MAX, a divisor with exponent 255).
    struct PlaneHit
    {
        int hit = 0;
        bool ok = true;
        int why = 0; // a decline's reason: 1 start / end / the point past 2^62, 2 the normal's normalize, 3 the quotient
        Vec4 point;
        uint16_t status;
    };
    inline PlaneHit planeHit(Vec4 vf0, Vec4 start, Vec4 end, Vec4 normal, Vec4 p, uint16_t status)
    {
        PlaneHit r;
        r.status = status;
        Vec4 n;
        if (!libvu0::finiteXyz(start) || !libvu0::finiteXyz(end) || !libvu0::finiteXyz(p) ||
            !normalizeStep(vf0, normal, r.status, n))
        {
            r.ok = false;
            r.why = libvu0::finiteXyz(start) && libvu0::finiteXyz(end) && libvu0::finiteXyz(p) ? 2 : 1;
            return r;
        }
        const Vec4 d = subXyz(end, start);
        Exact x;
        float t = inner(n, p);
        t = x.sub(t, inner(n, start));
        const float den = inner(n, d);
        if (equal(den, 0.0f))
            return r;
        t = x.div(t, den);
        if (!x.ok)
        {
            r.ok = false;
            r.why = 3;
            return r;
        }
        if (!lessEqual(0.0f, t) || !lessEqual(t, motion::one()))
            return r;
        r.point = x.xyz(blend<7>(start, add(start, blend<7>(d, mul(d, splat(t))))));
        if (!x.ok)
        {
            r.ok = false;
            r.why = 3;
            return r;
        }
        r.hit = 1;
        return r;
    }

    // ---- func_001361A0 (asm): is the point p inside the quad v[0..3]? -------------------------------------------------
    // e1 = normalize(v1 - v0), e3 = normalize(v3 - v0), N = e1 x e3; u_k = normalize(v_k - p) (k = 0..3); per edge
    // (u0 u1, u1 u2, u2 u3, u3 u0): s = N . (u_k x u_k+1) (OuterProduct, InnerProduct); the first s < 0 (c.lt.s s, 0)
    // returns |s| < 1e-6 (0x358637BD); all four >= 0 return 1. Guest calls: Normalize x6, OuterProduct, then 2 per edge
    // tested (`calls`); one backward branch (the vf0 clear loop). `ok` false: a normalize declined.
    struct QuadInside
    {
        int inside = 0;
        bool ok = true;
        int why = 0; // a decline's reason: 1 the edges' normalize (v1 - v0 / v3 - v0), 2 a corner's (v_k - p)
        int calls = 0;
        uint16_t status;
    };
    inline QuadInside quadInside(Vec4 vf0, const Vec4 v[4], Vec4 p, uint16_t status)
    {
        QuadInside r;
        r.status = status;
        Vec4 e1, e3, u[4];
        if (!normalizeStep(vf0, subXyz(v[1], v[0]), r.status, e1) || !normalizeStep(vf0, subXyz(v[3], v[0]), r.status, e3))
        {
            r.ok = false;
            r.why = 1;
            return r;
        }
        const Vec4 n = libvu0::outerProduct(e1, e3).r;
        for (int k = 0; k < 4; ++k)
            if (!normalizeStep(vf0, subXyz(v[k], p), r.status, u[k]))
            {
                r.ok = false;
                r.why = 2;
                return r;
            }
        r.calls = 7;
        for (int k = 0; k < 4; ++k)
        {
            r.calls += 2;
            const float s = inner(n, libvu0::outerProduct(u[k], u[(k + 1) & 3]).r);
            if (lessThan(s, 0.0f))
            {
                r.inside = lessThan(motion::absBits(s), bitsFloat(0x358637BDu)) ? 1 : 0;
                return r;
            }
        }
        r.inside = 1;
        return r;
    }
}
