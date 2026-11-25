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
