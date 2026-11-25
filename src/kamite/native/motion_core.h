#pragma once

// Rung 4's motion maths (claude/rung4-motion-map.md) on plain Vec4s, portable (SSE, NEON, plain C++), shared by the
// natives (motion.cpp, which adds the guest context) and the offline checks (tools/native). Each op is the recompiled
// code's op in its order under the default float rules (libvu0_core.h): exact for the inputs the natives' entry checks
// let in.

#include "kamite/native/libvu0_core.h"

namespace kamite::native::motion
{
    struct Mat4
    {
        Vec4 r[4];
    };

    // VU0's unit matrix as MtxInitRotVec / MtxInitScaleVec build it (vmove vf4, vf0; vmr32 x3; rows vf7, vf6, vf5,
    // vf4), for the constant vf0 (the natives check ee::vf0Constant).
    inline Mat4 unit()
    {
        return Mat4{{bitsOf(0x3F800000u, 0u, 0u, 0u), bitsOf(0u, 0x3F800000u, 0u, 0u), bitsOf(0u, 0u, 0x3F800000u, 0u),
                     bitsOf(0u, 0u, 0u, 0x3F800000u)}};
    }
    inline Vec4 vf0Constant() { return bitsOf(0u, 0u, 0u, 0x3F800000u); }

    // ---- Adjust_theta_vec 0x1F7DA0 (asm): an Euler vector's xyz wrapped by even multiples of pi -----------------------
    // The table at 0x3BF110: c = (1/pi, pi, 0, 0), then the words {1, 1, 1, 1} and {~1 x4}. vf7.xyz = ftoi0(v * c.x)
    // (truncated); + 1 where v's bits are a positive int (pcgtw against 0: v > 0, +0 excluded), & ~1 (paddw / pand on
    // all 128 bits); vf4.xyz = v - itof0(n) * c.y. w stays (sqc2 stores the vector as loaded).
    struct ThetaTable
    {
        Vec4 c;
        uint32_t one[4], mask[4];
    };
    // xyz and c.xy below 2^62: no product or difference reaches FLT_MAX (ftoi saturates as the original's).
    inline bool thetaInputs(Vec4 v, const ThetaTable &t)
    {
        return libvu0::finiteXyz(v) && magnitudesBelow(blend<3>(splat(0.0f), t.c), 0x5E800000u);
    }
    inline Vec4 adjustTheta(Vec4 v, const ThetaTable &t)
    {
        const Vec4 n = ftoi(mul(v, splat<0>(t.c))); // vmulx.xyz, vftoi0.xyz (w: don't-care, blended out below)
        uint32_t vb[4], nb[4];
        store(vb, v);
        store(nb, n);
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t positive = static_cast<int32_t>(vb[i]) > 0 ? 0xFFFFFFFFu : 0u; // pcgtw t0, t0, zero
            nb[i] = (nb[i] + (positive & t.one[i])) & t.mask[i];                         // pand, paddw, pand
        }
        const Vec4 k = itof(load(nb));                              // vitof0.xyz
        return blend<7>(v, sub(v, mul(k, splat<1>(t.c))));          // vmuly.xyz, vsub.xyz
    }

    // ---- sceVu0RotMatrixX / Y / Z in place (a0 == a1; Axis 0 = X, 1 = Y, 2 = Z): m = rows of m times R(angle) --------
    // f12 = pi/2 -+ angle (c.olt.s against 0: a negative angle with a non-zero exponent), ecossin, the rotation rows,
    // then each row through mulRow (libvu0.cpp rotMatrix's ops; only vf4.xy and vf5 = 0 reach the rows, so the VU
    // registers' old lanes don't matter).
    // The rotMatrix native's checks per call: |angle| < 16 and every row below 2^62 (the coefficients: once, by the
    // caller); false = decline.
    inline bool rotationInputs(float angle, const Mat4 &m)
    {
        return (floatBits(angle) & 0x7FFFFFFFu) < 0x41800000u && libvu0::finiteInputs(m.r[0]) &&
               libvu0::finiteInputs(m.r[1]) && libvu0::finiteInputs(m.r[2]) && libvu0::finiteInputs(m.r[3]);
    }
    template <int Axis>
    inline void rotate(Mat4 &m, float angle, Vec4 vf0, Vec4 coefficients)
    {
        const uint32_t bits = floatBits(angle);
        const bool negative = (bits & 0x80000000u) && (bits & 0x7F800000u);
        const float piOver2 = bitsFloat(0x3FC90FDBu);
        const float reduced = negative ? addScalar(piOver2, angle) : subScalar(piOver2, angle);
        libvu0::VuRegs v{splat(0.0f), splat(0.0f), bitsOf(floatBits(reduced), 0u, 0u, 0u), splat(0.0f), splat(0.0f), 0.0f, 0};
        libvu0::ecossin(vf0, coefficients, negative, v);
        Vec4 vf4 = v.vf4;
        const libvu0::RotationRows r = libvu0::rotationRows<Axis>(vf0, v.vf5, vf4);
        for (int k = 0; k < 4; ++k)
            m.r[k] = libvu0::mulRow(r.r0, r.r1, r.r2, r.r3, m.r[k]).r;
    }
    inline void rotateAxis(Mat4 &m, int axis, float angle, Vec4 vf0, Vec4 coefficients)
    {
        if (axis == 0)
            rotate<0>(m, angle, vf0, coefficients);
        else if (axis == 1)
            rotate<1>(m, angle, vf0, coefficients);
        else
            rotate<2>(m, angle, vf0, coefficients);
    }

    // ---- MtxInitRotVec 0x147728 (src/cod/00147728.c): the unit matrix, then three rotations by rotOrder -------------
    // Orders 0-4: X / Y / Z single-axis calls (a1[axis]); 5: sceVu0RotMatrix 0x30A870 = Z, Y, X; above 5: the unit only.
    constexpr int kRotOrder[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    // m = unit() rotated; false = a rotation's checks failed (nothing to keep).
    inline bool rotationByOrder(Mat4 &m, uint32_t order, const float angles[3], Vec4 vf0, Vec4 coefficients)
    {
        if (order > 5u)
            return true;
        for (int i = 0; i < 3; ++i)
        {
            const int axis = kRotOrder[order][i];
            if (!rotationInputs(angles[axis], m))
                return false;
            rotateAxis(m, axis, angles[axis], vf0, coefficients);
        }
        return true;
    }

    // MtxInitTransVec's matrix: the unit rows with row 3 = (t.xyz, 1.0), the words as copied.
    inline Mat4 translation(uint32_t x, uint32_t y, uint32_t z)
    {
        Mat4 m = unit();
        m.r[3] = bitsOf(x, y, z, 0x3F800000u);
        return m;
    }
    // MtxInitScaleVec's: the unit rows with the diagonal's xyz = s.
    inline Mat4 scaling(uint32_t x, uint32_t y, uint32_t z)
    {
        Mat4 m = unit();
        m.r[0] = bitsOf(x, 0u, 0u, 0u);
        m.r[1] = bitsOf(0u, y, 0u, 0u);
        m.r[2] = bitsOf(0u, 0u, z, 0u);
        return m;
    }

    // ---- MtxInverse 0x147568 (asm; a rigid-ish 4x4 with row 3 the translation) --------------------------------------
    // det by the FPU in the asm's order (mul.s / add.s / sub.s); det with exponent 0 (c.eq.s against 0) -> the unit
    // matrix; else Q = 1 / det (vdiv), rows' cofactors by vopmula / vopmsub (r1 x r2, r2 x r0, r0 x r1) times Q, their
    // dots with row 3 (x + z, then + y) negated into w (0 - dot), and the four transposed (pextlw / pcpyld: row k =
    // (c0[k], c1[k], c2[k], vf0[k])). Inputs: the nine rotation entries below 2^41 (det's products below 2^123);
    // false = a result at 2^126 or more (the runtime's exact path could differ there): decline.
    inline bool inverseInputs(const Mat4 &a)
    {
        return magnitudesBelow(blend<7>(splat(0.0f), a.r[0]), 0x54000000u) &&
               magnitudesBelow(blend<7>(splat(0.0f), a.r[1]), 0x54000000u) &&
               magnitudesBelow(blend<7>(splat(0.0f), a.r[2]), 0x54000000u);
    }
    inline bool below126(Vec4 v) { return magnitudesBelow(v, 0x7E800000u); }
    inline bool inverse(const Mat4 &a, Mat4 &out)
    {
        const float m00 = lane<0>(a.r[0]), m01 = lane<1>(a.r[0]), m02 = lane<2>(a.r[0]);
        const float m10 = lane<0>(a.r[1]), m11 = lane<1>(a.r[1]), m12 = lane<2>(a.r[1]);
        const float m20 = lane<0>(a.r[2]), m21 = lane<1>(a.r[2]), m22 = lane<2>(a.r[2]);
        float d = addScalar(mulScalar(mulScalar(m00, m11), m22), mulScalar(mulScalar(m10, m21), m02));
        d = addScalar(d, mulScalar(mulScalar(m20, m01), m12));
        d = subScalar(d, mulScalar(mulScalar(m02, m11), m20));
        d = subScalar(d, mulScalar(mulScalar(m01, m10), m22));
        d = subScalar(d, mulScalar(mulScalar(m00, m12), m21));
        if ((floatBits(d) & 0x7F800000u) == 0u)
        {
            out = unit();
            return true;
        }
        uint16_t status = 0;
        const float q = libvu0::vuDiv(status, 1.0f, d);
        const Vec4 c0 = libvu0::outerProduct(a.r[1], a.r[2]).r, c1 = libvu0::outerProduct(a.r[2], a.r[0]).r,
                   c2 = libvu0::outerProduct(a.r[0], a.r[1]).r;
        const Vec4 v4 = mul(c0, splat(q)), v5 = mul(c1, splat(q)), v6 = mul(c2, splat(q));
        const Vec4 p4 = mul(v4, a.r[3]), p5 = mul(v5, a.r[3]), p6 = mul(v6, a.r[3]);
        const Vec4 zero = splat(0.0f);
        if (!below126(blend<7>(zero, v4)) || !below126(blend<7>(zero, v5)) || !below126(blend<7>(zero, v6)) ||
            !below126(blend<7>(zero, p4)) || !below126(blend<7>(zero, p5)) || !below126(blend<7>(zero, p6)))
            return false;
        const float d4 = addScalar(addScalar(lane<0>(p4), lane<2>(p4)), lane<1>(p4));
        const float d5 = addScalar(addScalar(lane<0>(p5), lane<2>(p5)), lane<1>(p5));
        const float d6 = addScalar(addScalar(lane<0>(p6), lane<2>(p6)), lane<1>(p6));
        const float w4 = subScalar(0.0f, d4), w5 = subScalar(0.0f, d5), w6 = subScalar(0.0f, d6);
        uint32_t x[4], y[4], z[4];
        store(x, v4), store(y, v5), store(z, v6);
        out.r[0] = bitsOf(x[0], y[0], z[0], 0u);
        out.r[1] = bitsOf(x[1], y[1], z[1], 0u);
        out.r[2] = bitsOf(x[2], y[2], z[2], 0u);
        out.r[3] = bitsOf(floatBits(w4), floatBits(w5), floatBits(w6), 0x3F800000u);
        return true;
    }

    // MtxMultiply(out, A, B) = sceVu0MulMatrix: out row k = A's rows weighted by B row k (mulRow). Both below 2^62.
    inline bool productInputs(const Mat4 &a, const Mat4 &b)
    {
        for (int k = 0; k < 4; ++k)
            if (!libvu0::finiteInputs(a.r[k]) || !libvu0::finiteInputs(b.r[k]))
                return false;
        return true;
    }
    inline Mat4 product(const Mat4 &a, const Mat4 &b)
    {
        Mat4 out;
        for (int k = 0; k < 4; ++k)
            out.r[k] = libvu0::mulRow(a.r[0], a.r[1], a.r[2], a.r[3], b.r[k]).r;
        return out;
    }
}
