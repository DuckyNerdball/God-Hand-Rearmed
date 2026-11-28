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
            return r;
        }
        if (!lessEqual(0.0f, t) || !lessEqual(t, motion::one()))
            return r;
        r.point = x.xyz(blend<7>(start, add(start, blend<7>(d, mul(d, splat(t))))));
        if (!x.ok)
        {
            r.ok = false;
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
            return r;
        }
        const Vec4 n = libvu0::outerProduct(e1, e3).r;
        for (int k = 0; k < 4; ++k)
            if (!normalizeStep(vf0, subXyz(v[k], p), r.status, u[k]))
            {
                r.ok = false;
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

    // ==== C2: the grid (claude/rung4-collision-map.md "C2") ==========================================================

    // ---- func_00136030 (asm): is the point p inside the triangle v[0..2]? --------------------------------------------
    // n_k = normalize(v_k - p) (k = 0..2); A = n1 x n2, B = n2 x n0, s = A.B; s < 0 (c.lt.s s, 0) -> |s| < 1e-6
    // (0x358637BD); else C = n0 x n1, s = A.C, s < 0 -> |s| < 1e-6; else 1. Guest calls: Normalize x3, OuterProduct x2,
    // InnerProduct, then OuterProduct + InnerProduct when the first s >= 0 (`calls`); no loop. `ok` false: a normalize
    // declined (p on a vertex, or a difference past 2^62).
    struct TriInside
    {
        int inside = 0;
        bool ok = true;
        int calls = 0;
        uint16_t status;
    };
    inline TriInside triangleInside(Vec4 vf0, const Vec4 v[3], Vec4 p, uint16_t status)
    {
        TriInside r;
        r.status = status;
        Vec4 n[3];
        for (int k = 0; k < 3; ++k)
            if (!normalizeStep(vf0, subXyz(v[k], p), r.status, n[k]))
            {
                r.ok = false;
                return r;
            }
        const Vec4 a = libvu0::outerProduct(n[1], n[2]).r;
        float s = inner(a, libvu0::outerProduct(n[2], n[0]).r);
        r.calls = 6;
        if (!lessThan(s, 0.0f))
        {
            r.calls = 8;
            s = inner(a, libvu0::outerProduct(n[0], n[1]).r);
            if (!lessThan(s, 0.0f))
            {
                r.inside = 1;
                return r;
            }
        }
        r.inside = lessThan(motion::absBits(s), bitsFloat(0x358637BDu)) ? 1 : 0;
        return r;
    }

    // ---- func_00136458 (asm): the segment start..end against the triangle v[0..2] -----------------------------------
    // N = (v1 - v0) x (v2 - v0) (OuterProduct; N.w reaches only the frame), d = (end - start).xyz; 0 <= N.d (c.le.s:
    // facing away, parallel or degenerate) -> 0; func_00135EC8(start, end, N, v0, hit = vf0) (planeHit) 0 -> 0;
    // func_00136030(v, hit) 0 -> 0; else 1 and `point` = the hit buffer (vf0 with xyz from 0x135EC8). Guest calls in
    // `calls` (OuterProduct, InnerProduct, then 1 + 4 and 1 + 0x136030's); no loop. `ok` false: a step declined (the
    // OuterProduct / InnerProduct inputs past 2^62, planeHit's or triangleInside's).
    struct TriProbe
    {
        int hit = 0;
        bool ok = true;
        int calls = 0;
        Vec4 point;
        uint16_t status;
    };
    inline TriProbe triangleProbe(Vec4 vf0, Vec4 start, Vec4 end, const Vec4 v[3], uint16_t status)
    {
        TriProbe r;
        r.status = status;
        const Vec4 e1 = subXyz(v[1], v[0]), e2 = subXyz(v[2], v[0]);
        if (!libvu0::finiteXyz(e1) || !libvu0::finiteXyz(e2)) // sceVu0OuterProduct's own check
        {
            r.ok = false;
            return r;
        }
        const Vec4 n = blend<8>(libvu0::outerProduct(e1, e2).r, splat(0.0f));
        const Vec4 d = subXyz(end, start);
        if (!libvu0::finiteXyz(n) || !libvu0::finiteXyz(d)) // sceVu0InnerProduct's
        {
            r.ok = false;
            return r;
        }
        r.calls = 2;
        if (lessEqual(0.0f, inner(n, d)))
            return r;
        const PlaneHit h = planeHit(vf0, start, end, n, v[0], r.status);
        r.calls += 5;
        if (!h.ok)
        {
            r.ok = false;
            return r;
        }
        r.status = h.status;
        if (!h.hit)
            return r;
        const Vec4 hit = blend<7>(vf0, h.point);
        const TriInside t = triangleInside(vf0, v, hit, r.status);
        r.calls += 1 + t.calls;
        if (!t.ok)
        {
            r.ok = false;
            return r;
        }
        r.status = t.status;
        r.hit = t.inside;
        r.point = hit;
        return r;
    }

    // ---- The grid functions on guest memory ---------------------------------------------------------------------------
    // M: the guest memory of one call, in the asm's order of accesses (collision.cpp: a journal over the env, declining
    // an access outside plain RAM, misaligned or in the frames' window; libvu0_check: a record's image):
    //   bool ok; uint32_t lw(a), lhu(a), lbu(a); Vec4 lq(a) (a & ~0xF); void sw(a, v), sb(a, v), sq(a, v) (a & ~0xF);
    //   void call() (a jal: 8 cycles), loop() (a taken backward branch: 32).
    // A function stops at the first access that fails (m.ok false): the native declines, its writes put back.
    inline uint64_t signExtend(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }
    template <class M>
    inline float lwc1(M &m, uint32_t a) { return bitsFloat(m.lw(a)); }

    // func_00133948 as a step: a = vmini(a, b), b = vmax(a, b) (xyzw; b written last)
    template <class M>
    inline void boxSortStep(M &m, uint32_t a, uint32_t b)
    {
        const Vec4 va = m.lq(a), vb = m.lq(b);
        m.sq(a, vuMin(va, vb));
        m.sq(b, vuMax(va, vb));
    }
    // func_00133408 as a step on four word-aligned 12-byte vectors (lwc1)
    template <class M>
    inline bool overlapStep(M &m, const float amin[3], const float amax[3], uint32_t bmin, uint32_t bmax)
    {
        float lo[3], hi[3];
        for (uint32_t i = 0; i < 3u; ++i)
            lo[i] = lwc1(m, bmin + 4u * i), hi[i] = lwc1(m, bmax + 4u * i);
        int loops = 0;
        const bool hit = boxesOverlap(amin, amax, lo, hi, loops);
        if (loops)
            m.loop();
        return hit;
    }
    // three words copied word by word (lwc1 / swc1: x, y, z)
    template <class M>
    inline void copyXyz(M &m, uint32_t to, uint32_t from)
    {
        for (uint32_t i = 0; i < 12u; i += 4u)
            m.sw(to + i, m.lw(from + i));
    }

    // ---- func_0012F288 (src/cod/0012f288.c, the asm's order): the query box and the cells it covers; v0 ---------------
    // a1 / a2: the argument registers (64 bits: the beq guards compare them with scroll + 0x90 / + 0xA0). Calls
    // 0x133948 and 0x133408 (its z / y exit: a loop). `x` declines a division (ExactOps).
    template <class M>
    inline uint32_t gridRange(M &m, uint32_t s, uint64_t a1, uint64_t a2)
    {
        if (a1 != signExtend(s + 0x90u))
            copyXyz(m, s + 0x90u, static_cast<uint32_t>(a1));
        if (a2 != signExtend(s + 0xA0u))
            copyXyz(m, s + 0xA0u, static_cast<uint32_t>(a2));
        m.call();
        boxSortStep(m, s + 0x90u, s + 0xA0u);
        m.call();
        float gmin[3], gmax[3];
        for (uint32_t i = 0; i < 3u; ++i)
            gmin[i] = lwc1(m, s + 0x30u + 4u * i), gmax[i] = lwc1(m, s + 0x40u + 4u * i);
        if (!m.ok || !overlapStep(m, gmin, gmax, s + 0x90u, s + 0xA0u) || !m.ok)
            return 0u;
        // (box - min) / cell per axis, the runtime's cvt.w.s; the stores, then each re-read
        auto cells = [&](uint32_t box, uint32_t at) {
            Exact x;
            int32_t q[3];
            for (uint32_t i = 0; i < 3u; ++i)
            {
                const float d = x.sub(lwc1(m, box + 4u * i), lwc1(m, s + 0x30u + 4u * i));
                q[i] = fpuToWord(x.div(d, lwc1(m, s + 0x50u + 4u * i)));
            }
            if (!x.ok)
                m.ok = false;
            for (uint32_t i = 0; i < 3u; ++i)
                m.sw(at + 4u * i, static_cast<uint32_t>(q[i]));
        };
        cells(s + 0x90u, s + 0x6Cu);
        for (uint32_t i = 0; i < 3u; ++i)
            if (static_cast<int32_t>(m.lw(s + 0x6Cu + 4u * i)) < 0)
                m.sw(s + 0x6Cu + 4u * i, 0u);
        uint32_t last[3];
        for (uint32_t i = 0; i < 3u; ++i)
        {
            const uint32_t lo = m.lw(s + 0x6Cu + 4u * i), num = m.lw(s + 0x60u + 4u * i);
            if (i == 0u)
                last[0] = num - 1u; // a0 = num x - 1, from this read
            if (!m.ok || !(lo < num))
                return 0u;
        }
        cells(s + 0xA0u, s + 0x78u);
        const uint32_t hi0 = m.lw(s + 0x78u);
        if (last[0] < hi0)
            m.sw(s + 0x78u, last[0]);
        for (uint32_t i = 1; i < 3u; ++i)
        {
            last[i] = m.lw(s + 0x60u + 4u * i) - 1u;
            if (last[i] < m.lw(s + 0x78u + 4u * i))
                m.sw(s + 0x78u + 4u * i, last[i]);
        }
        for (uint32_t i = 0; i < 2u; ++i)
            if (static_cast<int32_t>(m.lw(s + 0x78u + 4u * i)) < 0)
                return 0u;
        return static_cast<int32_t>(m.lw(s + 0x80u)) >= 0 ? 1u : 0u;
    }

    // ---- func_0012F600 (src/cod/0012f600.c): clear the seen bit (flags &= ~4) of every polygon a cell lists ------------
    // per class bit of the mask (its count byte re-read per class, +0x14 per polygon); n - 1 loops per class.
    template <class M>
    inline void clearClass(M &m, uint32_t s, uint32_t q, uint32_t n)
    {
        for (uint32_t k = 0; k < n && m.ok; ++k)
        {
            const uint32_t id = m.lhu(q);
            const uint32_t at = m.lw(s + 0x14u) + id * 0x14u + 0x10u;
            q += 2u;
            m.sw(at, m.lw(at) & ~4u);
            if (k + 1u < n)
                m.loop();
        }
    }
    template <class M>
    inline void clearSeen(M &m, uint32_t s, uint32_t cell, uint32_t mask)
    {
        const uint32_t start = m.lw(cell);
        const uint32_t base = m.lw(s + 0x1Cu) + start * 2u;
        if (mask & 1u)
            clearClass(m, s, base, m.lbu(cell + 4u));
        if (mask & 2u)
        {
            const uint32_t n0 = m.lbu(cell + 4u);
            clearClass(m, s, base + n0 * 2u, m.lbu(cell + 5u));
        }
        if (mask & 4u)
        {
            const uint32_t n0 = m.lbu(cell + 4u), n1 = m.lbu(cell + 5u);
            clearClass(m, s, base + n0 * 2u + n1 * 2u, m.lbu(cell + 6u));
        }
    }

    // ---- func_0012F4C8 (src/cod/0012f4c8.c, the asm's axes): clearSeen over the cells x0..x1, y0..y1, z0..z1 ----------
    // (unsigned ranges, inclusive; cell ((x ny + y) nz + z) * 8 from +0x18, a start of -1 empty); a call per listed cell,
    // a loop per taken back-edge of the three loops. kMaxGridCells bounds the cells (and rows) a native walks: past it the
    // native declines (the caller passes a clamped range; an unbounded one would never end in the original either).
    constexpr uint64_t kMaxGridCells = 65536u; // the rooms seen: 68 x 1 x 68 = 4,624 cells at most
    inline bool gridWalkBounded(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, uint32_t z0, uint32_t z1)
    {
        if (x1 < x0)
            return true;
        // an end of 0xFFFFFFFF never ends (i + 1 wraps to 0, never past it)
        if (x1 == 0xFFFFFFFFu || (y0 <= y1 && y1 == 0xFFFFFFFFu) || (z0 <= z1 && z1 == 0xFFFFFFFFu))
            return false;
        const uint64_t nx = uint64_t{x1 - x0} + 1u, ny = y1 < y0 ? 0u : uint64_t{y1 - y0} + 1u,
                       nz = z1 < z0 ? 0u : uint64_t{z1 - z0} + 1u;
        return nx <= kMaxGridCells && nx * ny <= kMaxGridCells && nx * ny * nz <= kMaxGridCells;
    }
    template <class M>
    inline void clearRange(M &m, uint32_t s, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, uint32_t z0, uint32_t z1,
                           uint32_t mask)
    {
        if (x1 < x0)
            return;
        for (uint32_t x = x0;; ++x)
        {
            const uint32_t ny = m.lw(s + 0x64u);
            const uint32_t b0 = x * ny * m.lw(s + 0x68u);
            if (!(y1 < y0))
                for (uint32_t y = y0;; ++y)
                {
                    const uint32_t b1 = y * m.lw(s + 0x68u) + b0;
                    if (!(z1 < z0))
                        for (uint32_t z = z0;; ++z)
                        {
                            const uint32_t cell = m.lw(s + 0x18u) + (b1 + z) * 8u;
                            const uint32_t start = m.lw(cell);
                            if (!m.ok)
                                return;
                            if (start != 0xFFFFFFFFu)
                            {
                                m.call();
                                clearSeen(m, s, cell, mask);
                                if (!m.ok)
                                    return;
                            }
                            if (z1 < z + 1u)
                                break;
                            m.loop();
                        }
                    if (y1 < y + 1u)
                        break;
                    m.loop();
                }
            if (!m.ok || x1 < x + 1u)
                return;
            m.loop();
        }
    }

    // ---- func_0012F750 (asm): one polygon of the query (claude/rung4-collision-map.md "C2", steps 1-5) ----------------
    // The scroll s holds the query (+0xD0 dir, +0xE0 start, +0xF0 end, +0x174 / +0x178 masks, +0x18C list, +0x181
    // stepped, +0x184 / +0x188 the distances); `status` VU0's (threaded through the probes' normalizes).
    template <class M>
    inline void polygonTest(M &m, Vec4 vf0, uint16_t status, uint32_t s, uint32_t id)
    {
        const uint32_t polys = m.lw(s + 0x14u), groups = m.lw(s + 0x8u);
        const uint32_t p = polys + id * 0x14u;
        const uint32_t g = groups + (m.lhu(p + 0xCu) << 4);
        const uint32_t flags = m.lw(p + 0x10u);
        if (!m.ok || (flags & 6u) != 0u)
            return;
        m.sw(p + 0x10u, flags | 4u);
        if ((m.lw(g + 4u) & m.lw(s + 0x178u)) != 0u || !m.ok)
            return;
        // 2. the normal (w 1.0: the buffer was vf0) and the facing test
        const uint32_t nb = m.lhu(p + 8u) * 12u + m.lw(s + 0x10u);
        const uint32_t nx = m.lw(nb), ny = m.lw(nb + 4u), nz = m.lw(nb + 8u);
        const Vec4 n = bitsOf(nx, ny, nz, laneBits<3>(vf0));
        m.call();
        const Vec4 dir = m.lq(s + 0xD0u);
        if (!m.ok)
            return;
        if (!libvu0::finiteXyz(dir) || !libvu0::finiteXyz(n)) // sceVu0InnerProduct's own check
        {
            m.ok = false;
            return;
        }
        if (lessEqual(0.0f, inner(dir, n)))
            return;
        // 3. the clear loop, the polygon's box, the segment's box, the overlap
        m.loop(), m.loop(), m.loop();
        uint32_t ids[4];
        ids[1] = m.lhu(p + 2u), ids[0] = m.lhu(p), ids[2] = m.lhu(p + 4u), ids[3] = m.lhu(p + 6u);
        const uint32_t sel = m.lhu(p + 0xAu);
        const uint32_t verts = m.lw(s + 0xCu);
        float bmin[3], bmax[3];
        for (uint32_t i = 0; i < 3u; ++i)
        {
            bmin[i] = lwc1(m, ids[(sel >> (4u * i)) & 3u] * 12u + verts + 4u * i);
            bmax[i] = lwc1(m, ids[(sel >> (4u * i + 2u)) & 3u] * 12u + verts + 4u * i);
        }
        copyXyz(m, s + 0x90u, s + 0xE0u);
        copyXyz(m, s + 0xA0u, s + 0xF0u);
        m.call();
        boxSortStep(m, s + 0x90u, s + 0xA0u);
        m.call();
        if (!m.ok || !overlapStep(m, bmin, bmax, s + 0x90u, s + 0xA0u) || !m.ok)
            return;
        // 4. the vertices (w 1.0) and the probe
        const uint32_t verts2 = m.lw(s + 0xCu);
        Vec4 v[4];
        auto vertex = [&](uint32_t vid) {
            const uint32_t at = vid * 12u + verts2;
            const uint32_t x = m.lw(at), y = m.lw(at + 4u), z = m.lw(at + 8u);
            return bitsOf(x, y, z, laneBits<3>(vf0));
        };
        for (int k = 0; k < 3; ++k)
            v[k] = vertex(ids[k]);
        const bool quad = (m.lbu(p + 0x10u) & 1u) != 0u;
        Vec4 hit = vf0;
        int inside = 0;
        if (!quad)
        {
            m.call();
            const Vec4 start = m.lq(s + 0xE0u), end = m.lq(s + 0xF0u);
            if (!m.ok)
                return;
            const TriProbe t = triangleProbe(vf0, start, end, v, status);
            if (!t.ok)
            {
                m.ok = false;
                return;
            }
            for (int k = 0; k < t.calls; ++k)
                m.call();
            inside = t.hit;
            if (inside)
                hit = t.point;
        }
        else
        {
            v[3] = vertex(ids[3]);
            m.call();
            const Vec4 start = m.lq(s + 0xE0u), end = m.lq(s + 0xF0u);
            if (!m.ok)
                return;
            // HitProbe_probeHitPoint inline (collision.cpp hitProbeNative's body)
            const PlaneHit h = planeHit(vf0, start, end, n, v[0], status);
            if (!h.ok)
            {
                m.ok = false;
                return;
            }
            int calls = 1 + 4;
            if (h.hit)
            {
                const QuadInside q = quadInside(vf0, v, blend<7>(vf0, h.point), h.status);
                if (!q.ok)
                {
                    m.ok = false;
                    return;
                }
                m.loop();
                calls += 1 + q.calls;
                inside = q.inside;
                if (inside)
                    hit = blend<7>(vf0, h.point);
            }
            for (int k = 0; k < calls; ++k)
                m.call();
        }
        if (!inside)
            return;
        // 5. the hit
        const uint32_t list = m.lw(s + 0x18Cu);
        if (list != 0u)
        {
            if (m.lbu(s + 0x181u) != 0u)
            {
                m.sw(list, g);
                m.sw(s + 0x17Cu, 1u);
            }
            else if (m.lw(s + 0x140u) != 0u)
            {
                m.sw(m.lw(s + 0x17Cu) * 4u + list, g);
                const uint32_t count = m.lw(s + 0x17Cu), capacity = m.lw(s + 0x140u);
                m.sw(s + 0x17Cu, count + 1u);
                m.sw(s + 0x140u, capacity - 1u);
            }
        }
        Exact x;
        const float dy = x.sub(lwc1(m, s + 0xE4u), lane<1>(hit)), dx = x.sub(lwc1(m, s + 0xE0u), lane<0>(hit));
        const float dz = x.sub(lwc1(m, s + 0xE8u), lane<2>(hit));
        const float best = lwc1(m, s + 0x188u);
        const float d2 = x.add(x.add(x.mul(dx, dx), x.mul(dy, dy)), x.mul(dz, dz));
        if (!x.ok)
            m.ok = false;
        m.sw(s + 0x184u, floatBits(d2));
        if (lessEqual(d2, best))
        {
            m.sw(s + 0x130u, nx), m.sw(s + 0x134u, ny), m.sw(s + 0x138u, nz);
            m.sw(s + 0x188u, m.lw(s + 0x184u));
            m.sw(s + 0x120u, laneBits<0>(hit)), m.sw(s + 0x124u, laneBits<1>(hit)), m.sw(s + 0x128u, laneBits<2>(hit));
            if (m.lbu(s + 0x181u) != 0u)
                copyXyz(m, s + 0xF0u, s + 0x120u);
        }
        m.sb(s + 0x180u, 1u);
    }
}
