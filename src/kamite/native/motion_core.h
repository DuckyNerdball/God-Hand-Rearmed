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

    // ==== M1's blend (func_00156D70) and its helpers: the FPU / VU scalar ops checked as they go ======================
    // Exact: each op is the host's (the runtime's path) while no result reaches FLT_MAX - 1 ulp; div.s / VDIV only by an
    // ordinary divisor (exponent 1-254) and a dividend not 255; sqrt.s = sqrt(|a|) for exponent 1-254 (ps2EeSqrt; the
    // game's sqrtf fallback runs only on a NaN root, which that never gives). ok = false: decline. (packet.cpp's
    // ExactOps, rung 3, is the same idea.)
    struct Exact
    {
        bool ok = true;
        float fpu(float r)
        {
            if (fpuPastMax(r))
                ok = false;
            return r;
        }
        float mul(float a, float b) { return fpu(mulScalar(a, b)); }
        float add(float a, float b) { return fpu(addScalar(a, b)); }
        float sub(float a, float b) { return fpu(subScalar(a, b)); }
        float div(float a, float b)
        {
            const uint32_t ea = (floatBits(a) >> 23) & 0xFFu, eb = (floatBits(b) >> 23) & 0xFFu;
            if (eb - 1u >= 254u || ea == 255u)
                ok = false;
            return fpu(divScalar(a, b));
        }
        float sqrt(float a)
        {
            if (((floatBits(a) >> 23) & 0xFFu) - 1u >= 254u)
                ok = false;
            return sqrtScalar(bitsFloat(floatBits(a) & 0x7FFFFFFFu));
        }
        Vec4 xyz(Vec4 r) // a VU result whose xyz lanes reach the result
        {
            if (fpuPastMax(lane<0>(r)) || fpuPastMax(lane<1>(r)) || fpuPastMax(lane<2>(r)))
                ok = false;
            return r;
        }
    };
    // c.lt.s: the runtime's compare key (sign-magnitude, exponent-0 values as 0)
    inline uint32_t fpuKey(uint32_t bits)
    {
        if ((bits & 0x7F800000u) == 0u)
            bits = 0u;
        return (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
    }
    inline bool fpuLess(float a, float b) { return fpuKey(floatBits(a)) < fpuKey(floatBits(b)); }
    inline float absBits(float f) { return bitsFloat(floatBits(f) & 0x7FFFFFFFu); } // abs.s
    inline float negBits(float f) { return bitsFloat(floatBits(f) ^ 0x80000000u); } // neg.s
    // A multiplier of +-1.0 as the game has it in a register: not folded away (x * 1.0 still flushes a denormal).
    inline float registerConstant(float f)
    {
        volatile float v = f;
        return v;
    }
    inline float one() { return bitsFloat(0x3F800000u); }

    // ---- libm (fdlibm float, linked into the game): __kernel_sinf 0x3A4728 / __kernel_cosf 0x3A3C80 ---------------
    inline float kernelSinf(Exact &x, float xf, float yf, int iy)
    {
        const uint32_t ix = floatBits(xf) & 0x7FFFFFFFu;
        if (ix <= 0x31FFFFFFu && fpuToWord(xf) == 0)
            return xf;
        const float z = x.mul(xf, xf);
        float r = x.mul(z, bitsFloat(0x2F2EC9D3u));
        const float v = x.mul(z, xf);
        r = x.mul(z, x.add(r, bitsFloat(0xB2D72F34u)));
        r = x.mul(z, x.add(r, bitsFloat(0x3638EF1Bu)));
        r = x.mul(z, x.add(r, bitsFloat(0xB9500D01u)));
        r = x.add(r, bitsFloat(0x3C088889u));
        if (iy == 0)
            return x.add(xf, x.mul(v, x.add(x.mul(z, r), bitsFloat(0xBE2AAAABu))));
        const float vr = x.mul(v, r);
        float f = x.mul(yf, bitsFloat(0x3F000000u));
        const float vs1 = x.mul(v, bitsFloat(0xBE2AAAABu));
        f = x.sub(x.sub(x.mul(z, x.sub(f, vr)), yf), vs1);
        return x.sub(xf, f);
    }
    inline float kernelCosf(Exact &x, float xf, float yf)
    {
        const uint32_t ix = floatBits(xf) & 0x7FFFFFFFu;
        if (ix <= 0x31FFFFFFu && fpuToWord(xf) == 0)
            return one();
        const float z = x.mul(xf, xf);
        float p = x.mul(z, bitsFloat(0xAD47D74Eu));
        p = x.mul(z, x.add(p, bitsFloat(0x310F74F6u)));
        p = x.mul(z, x.add(p, bitsFloat(0xB493F27Cu)));
        p = x.mul(z, x.add(p, bitsFloat(0x37D00D01u)));
        p = x.mul(z, x.add(p, bitsFloat(0xBAB60B61u)));
        p = x.add(p, bitsFloat(0x3D2AAAABu));
        const float r = x.mul(z, p);
        const float half = bitsFloat(0x3F000000u);
        if (ix <= 0x3E999999u)
        {
            const float zr = x.sub(x.mul(z, r), x.mul(xf, yf));
            return x.sub(one(), x.sub(x.mul(z, half), zr));
        }
        const float qx = bitsFloat(ix > 0x3F480000u ? 0x3E900000u : ix - 0x01000000u);
        const float zr = x.sub(x.mul(z, r), x.mul(xf, yf));
        const float hz = x.sub(x.mul(z, half), qx);
        const float a = x.sub(one(), qx);
        return x.sub(a, x.sub(hz, zr));
    }
    // sinf 0x3A19F8: |x| <= pi/4: __kernel_sinf(x, 0, 0) (1 guest call); up to 3pi/4: __ieee754_rem_pio2f's n = +-1 path
    // (pio2_1 / pio2_1t, or pio2_2 / pio2_2t near pi/2; as packet.cpp's tanf), then +-__kernel_cosf(y0, y1) (2 calls).
    // Past that (rem_pio2f's medium / huge paths, x - x) the call declines (ok = false).
    inline float sinfBody(Exact &x, float a, int &calls)
    {
        const uint32_t hx = floatBits(a), ix = hx & 0x7FFFFFFFu;
        calls = 1;
        if (ix <= 0x3F490FD8u)
            return kernelSinf(x, a, bitsFloat(0u), 0);
        if (ix > 0x4016CBE3u)
        {
            x.ok = false;
            return a;
        }
        calls = 2;
        const bool positive = static_cast<int32_t>(hx) > 0;
        const bool nearHalfPi = (ix & 0xFFFFFFF0u) == 0x3FC90FD0u;
        const float pio2_1 = bitsFloat(0x3FC90F80u);
        float z = positive ? x.sub(a, pio2_1) : x.add(a, pio2_1);
        float tail = bitsFloat(0x37354443u);
        if (nearHalfPi)
        {
            const float pio2_2 = bitsFloat(0x37354400u);
            z = positive ? x.sub(z, pio2_2) : x.add(z, pio2_2);
            tail = bitsFloat(0x2E85A308u);
        }
        const float y0 = positive ? x.sub(z, tail) : x.add(z, tail);
        const float d = x.sub(z, y0);
        const float y1 = positive ? x.sub(d, tail) : x.add(d, tail);
        const float c = kernelCosf(x, y0, y1);
        return positive ? c : negBits(c); // n & 3 = 1: cos, 3: -cos
    }

    // ---- capVu0Atan2 0x1F8180 (asm; y = f12, x = f13): the table 0x42C350 = c0..c7 (an odd series in z), pi/4 ----------
    // |x| or |y| below 1e-5 (0x3727C5AC): 0, +-pi/2 or +-pi; else z = (|y| - |x|) / (|y| + |x|) (VDIV), a = pi/4 +
    // c0 z + c1 z^3 + ... + c7 z^15 (vmulax / vmadda in that order, then vmaddw with vf0.w), by quadrant.
    inline float atan2Body(Exact &x, float y, float xv, const float c[9])
    {
        const float ax = absBits(xv), ay = absBits(y), eps = bitsFloat(0x3727C5ACu), zero = bitsFloat(0u);
        if (fpuLess(ax, eps))
        {
            if (fpuLess(ay, eps))
                return zero;
            return fpuLess(y, zero) ? bitsFloat(0xBFC90FDBu) : bitsFloat(0x3FC90FDBu);
        }
        if (fpuLess(ay, eps))
        {
            if (!fpuLess(xv, zero))
                return zero;
            return fpuLess(y, zero) ? bitsFloat(0xC0490FDBu) : bitsFloat(0x40490FDBu);
        }
        const float sum = x.add(ax, ay), diff = x.sub(ay, ax);
        const uint32_t es = (floatBits(sum) >> 23) & 0xFFu, ed = (floatBits(diff) >> 23) & 0xFFu;
        if (es - 1u >= 254u || ed == 255u)
            x.ok = false;
        uint16_t status = 0;
        const float z = x.add(zero, x.fpu(libvu0::vuDiv(status, diff, sum))); // vaddq.x vf7, vf0, Q
        const float z2 = x.mul(z, z);
        float acc = x.mul(z, c[0]);
        float p = x.mul(z, z2);
        acc = x.add(acc, x.mul(p, c[1]));
        for (int k = 2; k < 8; ++k)
        {
            p = x.mul(p, z2);
            acc = x.add(acc, x.mul(p, c[k]));
        }
        const float a = x.add(acc, x.mul(c[8], registerConstant(1.0f)));
        const float pi = bitsFloat(0x40490FDBu);
        if (fpuLess(xv, zero))
            return fpuLess(y, zero) ? x.sub(a, pi) : x.sub(pi, a);
        return fpuLess(y, zero) ? negBits(a) : a;
    }

    // ---- func_0015A6A0: slerp(out, q1, q2, t) (asm) -------------------------------------------------------------------
    // dot (x, y, z, w in that order), its sign taken out (* -1.0); below 0x3F7FFF58: theta = capVu0Atan2(sqrt(1 - dot^2),
    // dot), s0 = sinf((1 - t) theta) / sin, s1 = sinf(t theta) / sin (sin's reciprocal first); else s0 = 1 - t, s1 = t;
    // s1 *= the sign; out[k] = q1[k] s0 + q2[k] s1. calls: capVu0Atan2 + 2 sinf and theirs (the lerp: none).
    inline void slerp(Exact &x, const float q1[4], const float q2[4], float t, const float c[9], int &calls, float out[4])
    {
        calls = 0;
        float sign = one();
        float dot = x.mul(q1[0], q2[0]);
        const float dy = x.mul(q1[1], q2[1]), dz = x.mul(q1[2], q2[2]);
        dot = x.add(dot, dy);
        const float dw = x.mul(q1[3], q2[3]);
        dot = x.add(dot, dz);
        dot = x.add(dot, dw);
        if (fpuLess(dot, bitsFloat(0u)))
            sign = bitsFloat(0xBF800000u);
        dot = x.mul(dot, registerConstant(sign));
        float s0, s1;
        if (fpuLess(dot, bitsFloat(0x3F7FFF58u)))
        {
            const float sinTheta = x.sqrt(x.sub(one(), x.mul(dot, dot)));
            const float theta = atan2Body(x, sinTheta, dot, c);
            const float inv = x.div(one(), sinTheta);
            int c1 = 0, c2 = 0;
            const float a1 = sinfBody(x, x.mul(t, theta), c1);
            const float omt = x.sub(one(), t);
            s1 = x.mul(a1, inv);
            const float a0 = sinfBody(x, x.mul(omt, theta), c2);
            s0 = x.mul(a0, inv);
            calls = 3 + c1 + c2;
        }
        else
        {
            s0 = x.sub(one(), t);
            s1 = t;
        }
        s1 = x.mul(s1, registerConstant(sign));
        for (int k = 0; k < 4; ++k)
            out[k] = x.add(x.mul(q1[k], s0), x.mul(q2[k], s1));
    }

    // ---- func_0015A190: a quaternion from a matrix's 3x3 (asm; Shoemake) -----------------------------------------------
    // trace > 0 (c.lt.s 0, trace): s = sqrt(trace + 1), w = s 0.5, f = 0.5 / s, xyz from the off-diagonal differences;
    // else the largest diagonal i (m00 < m11 -> 1, then m_ii < m22 -> 2), j = next[i], k = next[j] (the ELF's {1, 2, 0} at
    // 0x41EE48), s = sqrt(m_ii - m_jj - m_kk + 1). Writes q in the original's order (callers' q is clear of m).
    inline void mat2quat(Exact &x, const Mat4 &mm, const uint32_t next[3], float q[4])
    {
        float m[3][4];
        for (int r = 0; r < 3; ++r)
            store(m[r], mm.r[r]);
        const float half = bitsFloat(0x3F000000u);
        const float tr = x.add(x.add(m[0][0], m[1][1]), m[2][2]);
        if (fpuLess(bitsFloat(0u), tr))
        {
            const float s = x.sqrt(x.add(tr, one()));
            const float w = x.mul(s, half), f = x.div(half, s);
            q[3] = w;
            q[0] = x.mul(x.sub(m[1][2], m[2][1]), f);
            q[1] = x.mul(x.sub(m[2][0], m[0][2]), f);
            q[2] = x.mul(x.sub(m[0][1], m[1][0]), f);
            return;
        }
        uint32_t i = fpuLess(m[0][0], m[1][1]) ? 1u : 0u;
        if (fpuLess(m[i][i], m[2][2]))
            i = 2u;
        const uint32_t j = next[i], k = next[j];
        const float s = x.sqrt(x.add(x.sub(x.sub(m[i][i], m[j][j]), m[k][k]), one()));
        const float qi = x.mul(s, half), f = x.div(half, s);
        q[i] = qi;
        q[3] = x.mul(x.sub(m[j][k], m[k][j]), f);
        q[j] = x.mul(x.add(m[i][j], m[j][i]), f);
        q[k] = x.mul(x.add(m[i][k], m[k][i]), f);
    }

    // ---- MtxInitRotQuaternion 0x1478A8: the rotation matrix of q (asm; the FPU in the asm's order) ----------------------
    inline Mat4 quat2mat(Exact &x, const float q[4])
    {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        float xx = x.mul(qx, qx), zz = x.mul(qz, qz), zw = x.mul(qz, qw), xw = x.mul(qx, qw), xy = x.mul(qx, qy);
        float yy = x.mul(qy, qy), yw = x.mul(qy, qw), yz = x.mul(qy, qz);
        xx = x.add(xx, xx), zz = x.add(zz, zz);
        float zx = x.mul(qz, qx);
        yy = x.add(yy, yy), yw = x.add(yw, yw), xy = x.add(xy, xy), yz = x.add(yz, yz), zx = x.add(zx, zx);
        xw = x.add(xw, xw), zw = x.add(zw, zw);
        const float m11 = x.sub(x.sub(one(), zz), xx), m22 = x.sub(x.sub(one(), xx), yy), m00 = x.sub(x.sub(one(), yy), zz);
        const float m20 = x.add(zx, yw), m10 = x.sub(xy, zw), m21 = x.sub(yz, xw), m02 = x.sub(zx, yw);
        const float m01 = x.add(xy, zw), m12 = x.add(yz, xw);
        Mat4 m;
        m.r[0] = bitsOf(floatBits(m00), floatBits(m01), floatBits(m02), 0u);
        m.r[1] = bitsOf(floatBits(m10), floatBits(m11), floatBits(m12), 0u);
        m.r[2] = bitsOf(floatBits(m20), floatBits(m21), floatBits(m22), 0u);
        m.r[3] = bitsOf(0u, 0u, 0u, 0x3F800000u);
        return m;
    }

    // ==== func_001501D0's twist path (partFlags +0x154 & 0x800; rung 4, Nov 26) ======================================
    // ---- capVu0Cos 0x1F7FB8 (asm; f12 = x): u = pi/2 + x when x < 0 (c.lt.s; then a3 = 1), else pi/2 - x; on VU0 in the
    // asm's order s = u + c3 u^3 + c2 u^5 + c1 u^7 + c0 u^9 (the table 0x3BF140 = c0..c3, lqc2); returns 0 + (0 + s)
    // (vaddx.xy vf4, vf5 = 0, vf4x; vaddy.x vf4, vf5, vf4y). Its Q = sqrt(1 - s^2) and the a3 branch write vf4.x, which the
    // last vaddy overwrites: dead (Q / vf7 / the VU flags are scratch to every caller). No calls, no loops.
    inline float cosBody(Exact &x, float a, const float c[4])
    {
        const float zero = bitsFloat(0u), piOver2 = bitsFloat(0x3FC90FDBu);
        const float u = fpuLess(a, zero) ? x.add(piOver2, a) : x.sub(piOver2, a);
        float s = x.add(zero, u);     // vaddx.x vf4, vf0, vf6x
        const float u2 = x.mul(u, u); // vmul.x vf6, vf6, vf6
        float p[4];
        for (int k = 0; k < 4; ++k)
            p[k] = x.mul(x.mul(c[k], u), u2); // vmulw.xyzw vf8, vf5, vf6w; vmulx.xyzw vf8, vf8, vf6x
        for (int k = 0; k < 3; ++k)
            p[k] = x.mul(p[k], u2); // vmulx.xyz
        s = x.add(s, p[3]);         // vaddw.x
        for (int k = 0; k < 2; ++k)
            p[k] = x.mul(p[k], u2); // vmulx.xy
        s = x.add(s, p[2]);         // vaddz.x
        p[0] = x.mul(p[0], u2);     // vmulx.x
        s = x.add(s, p[1]);         // vaddy.x
        s = x.add(s, p[0]);         // vaddx.x
        return x.add(zero, x.add(zero, s));
    }
    // ---- Quaternion_SetRotationAxis 0x159FC8 (a0 = q, a1 = axis, f12 = angle; asm): h = angle 0.5 (mul.s), s = sinf(h),
    // q.xyz = axis.xyz s (mul.s each, written as computed), q.w = capVu0Cos(h). sinfCalls: sinf's own callees (1-2).
    inline void axisQuat(Exact &x, const float axis[3], float angle, const float c[4], int &sinfCalls, float q[4])
    {
        const float h = x.mul(angle, bitsFloat(0x3F000000u));
        const float s = sinfBody(x, h, sinfCalls);
        for (int k = 0; k < 3; ++k)
            q[k] = x.mul(axis[k], s);
        q[3] = cosBody(x, h, c);
    }

    // ---- func_00156D70: blend(out, A, B, t) (asm): two TRS matrices interpolated --------------------------------------
    // T = A.t + (B.t - A.t) t (row 3, vsub / vmulx / vadd .xyz); s = sqrt.s(capVu0LengthSq(row k)) per row of A and B;
    // the rows over s (div.s 1 / s, sceVu0ScaleVectorXYZ) to quaternions (0x15A190), slerp by t, back to a matrix
    // (MtxInitRotQuaternion); rows k *= sA.k + (sB.k - sA.k) t; row 3 = (T, 1). calls: every guest call it makes.
    struct BlendTables
    {
        float atan[9];
        uint32_t next[3];
    };
    inline float lengthSq(Exact &x, Vec4 v) // capVu0LengthSq: vmul.xyz, vaddy.x, vaddz.x
    {
        const Vec4 s = x.xyz(mul(v, v));
        return x.add(x.add(lane<0>(s), lane<1>(s)), lane<2>(s));
    }
    inline Mat4 blendTrs(Exact &x, const Mat4 &a, const Mat4 &b, float t, const BlendTables &tb, int &calls)
    {
        const Vec4 tt = splat(t);
        const Vec4 trans = x.xyz(add(a.r[3], x.xyz(mul(x.xyz(sub(b.r[3], a.r[3])), tt))));
        float sa[3], sb[3];
        for (int k = 0; k < 3; ++k)
            sa[k] = x.sqrt(lengthSq(x, a.r[k]));
        for (int k = 0; k < 3; ++k)
            sb[k] = x.sqrt(lengthSq(x, b.r[k]));
        Mat4 na = a, nb = b;
        for (int k = 0; k < 3; ++k)
        {
            na.r[k] = x.xyz(mul(a.r[k], splat(x.div(one(), sa[k]))));
            nb.r[k] = x.xyz(mul(b.r[k], splat(x.div(one(), sb[k]))));
        }
        float qa[4], qb[4], q[4];
        mat2quat(x, na, tb.next, qa);
        mat2quat(x, nb, tb.next, qb);
        int slerpCalls = 0;
        slerp(x, qa, qb, t, tb.atan, slerpCalls, q);
        Mat4 out = quat2mat(x, q);
        const Vec4 va = bitsOf(floatBits(sa[0]), floatBits(sa[1]), floatBits(sa[2]), 0x3F800000u);
        const Vec4 vb = bitsOf(floatBits(sb[0]), floatBits(sb[1]), floatBits(sb[2]), 0x3F800000u);
        const Vec4 sc = x.xyz(add(va, x.xyz(mul(x.xyz(sub(vb, va)), tt))));
        out.r[0] = blend<7>(out.r[0], x.xyz(mul(out.r[0], splat<0>(sc))));
        out.r[1] = blend<7>(out.r[1], x.xyz(mul(out.r[1], splat<1>(sc))));
        out.r[2] = blend<7>(out.r[2], x.xyz(mul(out.r[2], splat<2>(sc))));
        out.r[3] = blend<7>(out.r[3], trans);
        calls = 6 + 6 + 2 + 1 + slerpCalls + 1;
        return out;
    }

    // ==== M3: the pose's leaves (rung 4, Nov 26; rung4-motion-map.md "M3") ============================================
    // ---- func_00156508 (a0 = a PlatinumGames 16-bit float: sign 1, exponent 6 with bias 47, mantissa 9; asm): its f32
    // bits by integer ops alone: s = a0 << 14; ((s & 0x1F800000) + 0x28000000) | (s & 0x7FFF00) | ((s << 2) & 0x80000000)
    // (so 0 comes out as 2^-47, not 0).
    inline uint32_t halfBits(uint32_t a0)
    {
        const uint32_t s = a0 << 14;
        return (((s & 0x7FFF00u) | ((s & 0x1F800000u) + 0x28000000u)) | ((s << 2) & 0x80000000u));
    }
    // ---- func_00156550 (a0 = out, a1 = in: four words; asm, MMI): the same per 32-bit lane with the four quadwords at
    // 0x41EDF0 (t = 16 words; lane q: T0[q] & s + T1[q] (paddw), | T2[q] & s, | T3[q] & (s << 2); the ELF's lanes are all
    // 0x1F800000, 0x28000000, 0x7FFFFF, 0x80000000).
    inline uint32_t halfLane(uint32_t x, const uint32_t t[16], uint32_t q)
    {
        const uint32_t s = x << 14;
        return ((s & t[8 + q]) | ((s & t[q]) + t[4 + q])) | ((s << 2) & t[12 + q]);
    }
    inline uint16_t u16At(const uint8_t *p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    // c.eq.s: the runtime's compare key (exponent-0 values as 0)
    inline bool fpuEqual(float a, float b) { return fpuKey(floatBits(a)) == fpuKey(floatBits(b)); }
    // ---- func_00158CD0: a Hermite track at time t (the pose's kind-1 interpolator; asm) ----------------------------
    // track (12-byte header: half floats pos, dpos, tan0, dtan0, tan1, dtan1) then n 4-byte keys (frames since the last
    // key, value step, incoming and outgoing tangent steps). k = track + 0xC (key -1 = the header's last 4 bytes). In the
    // asm's order and ops: frames summed (add.s of cvt.s.w) until the sum equals t (key value) or passes it; past it the
    // sum steps back one key (sub.s) and sums again from that key (a second loop: the re-sum may land differently), then
    // the segment's Hermite between key s3 (= the first loop's key - 1, kept) and the key the second loop stopped at; past
    // the last key, the last key's value (key[n - 1], or the header's byte 9 when n = 0). value = byte dpos + pos (mul.s,
    // add.s). calls: 2 (0x156508) + 1 (0x156550) on the Hermite path; loops: the taken back-edges.
    inline float trackSample(Exact &x, const uint8_t *track, uint32_t n, float t, const uint32_t table[16], int &calls,
                             int &loops)
    {
        const float pos = bitsFloat(halfBits(u16At(track))), dpos = bitsFloat(halfBits(u16At(track + 2)));
        const uint8_t *k = track + 0xC;
        calls = 2, loops = 0;
        auto value = [&](int64_t key) { // key -1: the header's last 4 bytes
            return x.add(x.mul(static_cast<float>(k[4 * key + 1]), dpos), pos);
        };
        float sum = bitsFloat(0u);
        uint32_t v1 = 0;
        if (n == 0u)
            return value(-1);
        for (;;)
        {
            const float dt = static_cast<float>(k[4u * v1]);
            sum = x.add(sum, dt);
            if (fpuEqual(sum, t))
                return value(v1);
            if (!fpuLess(sum, t))
            {
                // 0x158DA4: step back one key and sum again from it
                const float before = x.sub(sum, dt);
                const int32_t s3 = static_cast<int32_t>(v1) - 1;
                sum = before;
                uint32_t j = v1;
                if (j < n)
                    for (;;)
                    {
                        sum = x.add(sum, static_cast<float>(k[4u * j]));
                        if (fpuEqual(sum, t))
                            return value(j);
                        ++v1;
                        if (!fpuLess(sum, t))
                        {
                            // 0x158E58: the Hermite between key s3 and key j
                            const uint8_t *a = k + 4 * s3, *b = k + 4u * j;
                            uint32_t c[4];
                            for (uint32_t q = 0; q < 4u; ++q)
                                c[q] = halfLane(u16At(track + 4u + 2u * q), table, q);
                            calls = 3;
                            const float seg = x.sub(sum, before);
                            const float u = x.div(x.sub(t, before), seg);
                            const float tan0 = bitsFloat(c[0]), dtan0 = bitsFloat(c[1]), tan1 = bitsFloat(c[2]),
                                        dtan1 = bitsFloat(c[3]);
                            float f5 = x.mul(static_cast<float>(a[3]), dtan1);
                            float f7 = x.mul(static_cast<float>(a[1]), dpos);
                            float f2 = x.mul(u, u);
                            float f4 = x.mul(static_cast<float>(b[1]), dpos);
                            f7 = x.add(f7, pos);
                            float f6 = x.mul(f2, u);
                            const float f8 = x.mul(f2, bitsFloat(0x40400000u));
                            f4 = x.add(f4, pos);
                            float f0 = x.add(f6, f6);
                            f6 = x.sub(f6, f2);
                            float f1 = static_cast<float>(b[2]);
                            float f3 = negBits(f0);
                            f0 = x.sub(f0, f8);
                            f2 = x.sub(f6, f2);
                            f3 = x.add(f3, f8);
                            f0 = x.mul(f0, f7);
                            f1 = x.mul(f1, dtan0);
                            f5 = x.add(f5, tan1);
                            f2 = x.add(f2, u);
                            f0 = x.add(f0, f7);
                            f4 = x.mul(f4, f3);
                            f1 = x.add(f1, tan0);
                            f5 = x.mul(f5, f2);
                            f0 = x.add(f0, f4);
                            f1 = x.mul(f1, f6);
                            f0 = x.add(f0, f5);
                            return x.add(f0, f1);
                        }
                        ++j;
                        if (v1 < n)
                        {
                            ++loops;
                            continue;
                        }
                        break;
                    }
                return value(s3); // 0x158E14: past the last key on the second sum
            }
            ++v1;
            if (v1 < n)
            {
                ++loops;
                continue;
            }
            return value(static_cast<int64_t>(n) - 1); // 0x158D84
        }
    }

    // ---- func_00158FC8: a kind-3 track at time t (the pose's interpolator for kind 3; asm, a leaf) ----------------------
    // track: six f32 (pos, dpos, tan0, dtan0, tan1, dtan1) then n 8-byte keys {u16 frame (absolute), value, in, out};
    // key -1 = the header's last 8 bytes (+0x10). value(k) = cvt(key k's value) * dpos + pos (mul.s, add.s). Loop 1 walks
    // the keys while frame < t (c.eq.s: that key's value; past the last: key n - 1's; n = 0: key -1's); at the first key
    // past t (s), loop 2 starts again at s (equal: its value; past t: the Hermite between key s - 1 and it; run out: key
    // s - 1's value). Hermite in the asm's order: u = (t - fA) / (fB - fA), A / B = the keys' values, A's tangent = out *
    // dtan1 + tan1, B's = in * dtan0 + tan0. loops: every taken backward branch (the two loops' and the three b's back to
    // the shared return).
    inline float kind3Sample(Exact &x, const uint8_t *track, uint32_t n, float t, int &loops)
    {
        auto f32 = [&](uint32_t at) {
            uint32_t v;
            std::memcpy(&v, track + at, 4);
            return bitsFloat(v);
        };
        const float pos = f32(0u), dpos = f32(4u);
        const uint8_t *k = track + 0x18;
        auto field = [&](int64_t key, uint32_t at) { return static_cast<float>(u16At(k + 8 * key + at)); };
        auto value = [&](int64_t key) { return x.add(x.mul(field(key, 2u), dpos), pos); };
        loops = 0;
        if (n == 0u)
            return value(-1); // 0x159030
        uint32_t v1 = 0;
        for (;;)
        {
            const float f = field(v1, 0u);
            if (fpuEqual(f, t))
            {
                ++loops; // 0x1590C8, b back to the return
                return value(v1);
            }
            if (!fpuLess(f, t))
                break;
            if (++v1 < n)
            {
                ++loops;
                continue;
            }
            return value(static_cast<int64_t>(n) - 1); // 0x159030
        }
        const int64_t before = static_cast<int64_t>(v1) - 1; // t4
        for (uint32_t j = v1;;)
        {
            const float f = field(j, 0u);
            if (fpuEqual(f, t))
            {
                ++loops; // 0x1590E0, b back
                return value(j);
            }
            v1 = j + 1u;
            if (!fpuLess(f, t))
            {
                // 0x1590F8: the Hermite between key t4 and key j
                const float fB = field(j, 0u), fA = field(before, 0u), vA = field(before, 2u), vB = field(j, 2u);
                const float span = x.sub(fB, fA);
                const float dtan0 = f32(0xCu);
                const float d = x.sub(t, fA);
                const float dtan1 = f32(0x14u);
                float f8 = x.mul(vA, dpos);
                float f7 = x.mul(vB, dpos);
                const float tan1 = f32(0x10u);
                const float u = x.div(d, span);
                const float tan0 = f32(0x8u);
                f8 = x.add(f8, pos);
                f7 = x.add(f7, pos);
                float f3 = field(j, 4u);
                float f4 = field(before, 6u);
                float f1 = x.mul(u, u);
                f3 = x.mul(f3, dtan0);
                f4 = x.mul(f4, dtan1);
                float f5 = x.mul(f1, u);
                const float f9 = x.mul(f1, bitsFloat(0x40400000u));
                f4 = x.add(f4, tan1);
                float f0 = x.add(f5, f5);
                f5 = x.sub(f5, f1);
                f3 = x.add(f3, tan0);
                float f2 = negBits(f0);
                f0 = x.sub(f0, f9);
                f1 = x.sub(f5, f1);
                f2 = x.add(f2, f9);
                f0 = x.mul(f0, f8);
                f1 = x.add(f1, u);
                f7 = x.mul(f7, f2);
                f0 = x.add(f0, f8);
                f4 = x.mul(f4, f1);
                f3 = x.mul(f3, f5);
                f0 = x.add(f0, f7);
                f0 = x.add(f0, f4);
                return x.add(f0, f3);
            }
            ++j;
            if (v1 < n)
            {
                ++loops;
                continue;
            }
            ++loops; // 0x1590C0, b back
            return value(before);
        }
    }

    // ==== M5 batch 1: matrix to Euler (0x147C88) and its helpers ======================================================
    // c.le.s: the runtime's compare key
    inline bool fpuLessEqual(float a, float b) { return fpuKey(floatBits(a)) <= fpuKey(floatBits(b)); }
    // __ieee754_sqrtf 0x3A3B48 (decomp C, newlib e_sqrtf: bit by bit) for a normal positive x (else false: never reached
    // from asinf); loops: the bit loop's taken back-edge (24).
    inline bool sqrtfBits(uint32_t bits, uint32_t &out, int &loops)
    {
        int32_t ix = static_cast<int32_t>(bits);
        if ((ix & 0x7f800000) == 0x7f800000 || ix <= 0 || (ix >> 23) == 0)
            return false;
        int32_t m = (ix >> 23) - 127;
        ix = (ix & 0x007fffff) | 0x00800000;
        if (m & 1)
            ix += ix;
        m >>= 1;
        ix += ix;
        int32_t q = 0, s = 0;
        for (uint32_t r = 0x01000000u; r != 0u;)
        {
            const int32_t t = s + static_cast<int32_t>(r);
            if (t <= ix)
            {
                s = t + static_cast<int32_t>(r);
                ix -= t;
                q += static_cast<int32_t>(r);
            }
            ix += ix;
            r >>= 1;
            if (r != 0u)
                ++loops;
        }
        if (ix != 0)
            q += (q & 1);
        out = static_cast<uint32_t>((q >> 1) + 0x3f000000 + static_cast<int32_t>(static_cast<uint32_t>(m) << 23));
        return true;
    }
    // __ieee754_asinf 0x3A2558 (decomp C, newlib ef_asin.c; no fused ops in its asm) on Exact; |x| > 1 (the 0 / 0 NaN)
    // false. calls: fabsf and __ieee754_sqrtf on |x| >= 0.5; loops: sqrtf's.
    inline bool ieeeAsinf(Exact &x, float xf, float &out, int &calls, int &loops)
    {
        const uint32_t hx = floatBits(xf), ix = hx & 0x7fffffffu;
        const float one = bitsFloat(0x3F800000u), huge = bitsFloat(0x7149F2CAu), two = bitsFloat(0x40000000u),
                    half = bitsFloat(0x3F000000u), pio2_hi = bitsFloat(0x3FC90FDAu), pio2_lo = bitsFloat(0x33A22168u),
                    pio4_hi = bitsFloat(0x3F490FDBu), pS0 = bitsFloat(0x3E2AAAABu), pS1 = bitsFloat(0xBEA6B090u),
                    pS2 = bitsFloat(0x3E4E0AA8u), pS3 = bitsFloat(0xBD241146u), pS4 = bitsFloat(0x3A4F7F04u),
                    pS5 = bitsFloat(0x3811EF08u), qS1 = bitsFloat(0xC019D139u), qS2 = bitsFloat(0x4001572Du),
                    qS3 = bitsFloat(0xBF303361u), qS4 = bitsFloat(0x3D9DC62Eu);
        auto pq = [&](float t, float &p, float &q) {
            p = x.mul(t, x.add(pS0, x.mul(t, x.add(pS1, x.mul(t, x.add(pS2, x.mul(t, x.add(pS3, x.mul(t, x.add(pS4, x.mul(t, pS5)))))))))));
            q = x.add(one, x.mul(t, x.add(qS1, x.mul(t, x.add(qS2, x.mul(t, x.add(qS3, x.mul(t, qS4))))))));
        };
        if (ix == 0x3f800000u)
        {
            out = x.add(x.mul(xf, pio2_hi), x.mul(xf, pio2_lo));
            return true;
        }
        if (ix > 0x3f800000u)
            return false;
        float p, q;
        if (ix < 0x3f000000u)
        {
            if (ix < 0x32000000u)
            {
                if (fpuLess(one, x.add(huge, xf)))
                {
                    out = xf;
                    return true;
                }
                return false; // (t unset in the C: never, huge + x > 1)
            }
            const float t = x.mul(xf, xf);
            pq(t, p, q);
            out = x.add(xf, x.mul(xf, x.div(p, q)));
            return true;
        }
        calls += 1; // fabsf
        const float t = x.mul(x.sub(one, absBits(xf)), half);
        pq(t, p, q);
        calls += 1; // __ieee754_sqrtf
        uint32_t sb;
        if (!sqrtfBits(floatBits(t), sb, loops))
            return false;
        const float s = bitsFloat(sb);
        float r;
        if (ix >= 0x3F79999Au)
            r = x.sub(pio2_hi, x.sub(x.mul(two, x.add(s, x.mul(s, x.div(p, q)))), pio2_lo));
        else
        {
            const float w = bitsFloat(sb & 0xfffff000u);
            const float c = x.div(x.sub(t, x.mul(w, w)), x.add(s, w));
            const float rr = x.div(p, q);
            const float pp = x.sub(x.mul(x.mul(two, s), rr), x.sub(pio2_lo, x.mul(two, c)));
            const float qq = x.sub(pio4_hi, x.mul(two, w));
            r = x.sub(pio4_hi, x.sub(pp, qq));
        }
        out = static_cast<int32_t>(hx) > 0 ? r : negBits(r);
        return true;
    }
    // func_003A1B78 (asinf, decomp C): __ieee754_asinf, then unless _LIB_VERSION (0x45F024) is _IEEE_ (-1): isnanf (a
    // NaN: done), fabsf, and |x| > 1 the DOMAIN path (matherr, errno: false). calls: every jal it makes.
    inline bool asinfWrapper(Exact &x, float xf, int32_t libVersion, float &out, int &calls, int &loops)
    {
        calls += 1; // __ieee754_asinf
        if (!ieeeAsinf(x, xf, out, calls, loops))
            return false;
        if (libVersion == -1)
            return true;
        calls += 1; // isnanf
        if ((floatBits(xf) & 0x7fffffffu) > 0x7f800000u)
            return true;
        calls += 1; // fabsf
        return !fpuLess(bitsFloat(0x3F800000u), absBits(xf));
    }
    // MtxGetScaleVec 0x148160's lengths: each row's (x x + y y) + z z, sqrt.s
    inline void rowLengths(Exact &x, const float m[12], float s[3])
    {
        for (int r = 0; r < 3; ++r)
            s[r] = x.sqrt(x.add(x.add(x.mul(m[4 * r], m[4 * r]), x.mul(m[4 * r + 1], m[4 * r + 1])), x.mul(m[4 * r + 2], m[4 * r + 2])));
    }
    // func_00147C88's six rotation orders (its jump table 0x41EA20; 6 and up: order 0's code). m = the matrix's first 12
    // words, s = rowLengths; atan2(y, x) = capVu0Atan2 (vf0 constant), asin(v) = -pi/2 when v <= -1, pi/2 when 1 <= v
    // (c.le.s), else the asinf wrapper. calls: the atan2s and the asinf wrapper's; loops: order 3's taken bc1t back to
    // the shared pi/2 tail, asinf's. false = decline (a domain error, Exact's limits).
    inline bool matToEuler(Exact &x, const float m[12], const float s[3], uint32_t order, const float c[9], int32_t libVersion,
                           float out[3], int &calls, int &loops)
    {
        const float sx = s[0], sy = s[1], sz = s[2];
        bool ok = true;
        auto atan2 = [&](float y, float xv) {
            calls += 1;
            return atan2Body(x, y, xv, c);
        };
        auto asinClamp = [&](float v, bool backEdge) {
            if (fpuLessEqual(v, bitsFloat(0xBF800000u)))
                return bitsFloat(0xBFC90FDBu);
            if (fpuLessEqual(bitsFloat(0x3F800000u), v))
            {
                if (backEdge)
                    ++loops; // order 3: bc1t back to 0x147E38
                return bitsFloat(0x3FC90FDBu);
            }
            float r = 0.0f;
            calls += 1;
            if (!asinfWrapper(x, v, libVersion, r, calls, loops))
                ok = false;
            return r;
        };
        auto d = [&](float a, float b) { return x.div(a, b); };
        switch (order < 6u ? order : 0u)
        {
        case 0:
            out[0] = atan2(d(m[6], sy), d(m[10], sz));
            out[1] = asinClamp(d(negBits(m[2]), sx), false);
            out[2] = atan2(d(m[1], sx), d(m[0], sx));
            break;
        case 1:
            out[0] = atan2(d(negBits(m[9]), sz), d(m[5], sy));
            out[1] = atan2(d(negBits(m[2]), sx), d(m[0], sx));
            out[2] = asinClamp(d(m[1], sx), false);
            break;
        case 2:
            out[0] = asinClamp(d(m[6], sy), false);
            out[1] = atan2(d(negBits(m[2]), sx), d(m[10], sz));
            out[2] = atan2(d(negBits(m[4]), sy), d(m[5], sy));
            break;
        case 3:
            out[0] = atan2(d(m[6], sy), d(m[5], sy));
            out[1] = atan2(d(m[8], sz), d(m[0], sx));
            out[2] = asinClamp(d(negBits(m[4]), sy), true);
            break;
        case 4:
            out[0] = asinClamp(d(negBits(m[9]), sz), false);
            out[1] = atan2(d(m[8], sz), d(m[10], sz));
            out[2] = atan2(d(m[1], sx), d(m[5], sy));
            break;
        default:
            out[0] = atan2(d(negBits(m[9]), sz), d(m[10], sz));
            out[1] = asinClamp(d(m[8], sz), false);
            out[2] = atan2(d(negBits(m[4]), sy), d(m[0], sx));
            break;
        }
        return ok;
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

    // The twist on a part's world matrix w (after either join path): t = w's translation (0x147C60 -> func_00150710 into
    // the frame), q about the axis by the angle, r = MtxInitRotQuaternion(q), w = MtxMultiply(out = w, A = r, B = w) (row k
    // of B read before row k of out is written: out = B is safe), then w's xyz translation put back (0x147C40; the
    // product's w lane stays). calls: every guest call (0x147C60 and its 0x150710, 0x159FC8, sinf and its 1-2, capVu0Cos,
    // MtxInitRotQuaternion, 0x147C40) less MtxMultiply's (the caller charges it with its loops). false = decline.
    inline bool twist(Exact &x, Mat4 &w, const float axis[3], float angle, const float c[4], int &calls)
    {
        float q[4];
        int sinfCalls = 0;
        axisQuat(x, axis, angle, c, sinfCalls, q);
        if (!x.ok)
            return false;
        const Mat4 r = quat2mat(x, q);
        if (!x.ok || !productInputs(r, w))
            return false;
        Mat4 out = product(r, w);
        out.r[3] = blend<7>(out.r[3], w.r[3]);
        w = out;
        calls = 2 + 1 + 1 + sinfCalls + 1 + 1 + 1;
        return true;
    }

    // ==== M3 batch 2: the root channels' decode (func_00159AD0, RecDecode_readHeader 0x1599B0, func_00159DA0) =========
    // A cursor (0x20 bytes: the motion's +0x1B4 / +0x1D4 / +0x1F4; decomp RecTrack.h) walks one record's keys forward:
    // +0 kind, +4 src (the record's block), +8 cur (the key at or after t), +0xC t0 / +0xE t1 (the bracketing keys'
    // frames), +0x10 / +0x14 their values, +0x18 / +0x1C the tangents. Kinds: 1 = the Hermite track's bytes (src: halves
    // pos, dpos, tan0, dtan0, tan1, dtan1; 4-byte keys from src + 0xC: frames since the last, value, out, in), 3 = shorts
    // (src: six floats; 8-byte keys from src + 0x18: frame, value, out, in), 5 = floats (16-byte keys from the record's
    // data: frame, value, out, in); else the record's +8 word is the value. The cursor is worked on in o (the bytes the
    // original leaves); everything else is read through mem.read(address, size), nullptr = decline (not plain RAM,
    // overlapping the cursor or the call's stack window). Each access is checked for its width's alignment (the PS2
    // faults on an unaligned one: declined). calls / loops: the guest calls and taken back-edges the original charges.
    template <class Mem>
    struct RecDecoder
    {
        static constexpr uint32_t kHalfTable = 0x41EDF0u; // func_00156550's four quadwords
        static constexpr uint32_t kMaxSteps = 4096u;      // a longer walk (garbage past a track) declines
        Mem &mem;
        Exact &x;
        uint8_t o[0x20] = {};
        int calls = 0, loops = 0;
        bool ok = true;

        uint32_t word(uint32_t at) const
        {
            uint32_t v;
            std::memcpy(&v, o + at, 4);
            return v;
        }
        void setWord(uint32_t at, uint32_t v) { std::memcpy(o + at, &v, 4); }
        uint32_t half(uint32_t at) const { return u16At(o + at); }
        void setHalf(uint32_t at, uint32_t v)
        {
            o[at] = static_cast<uint8_t>(v);
            o[at + 1u] = static_cast<uint8_t>(v >> 8);
        }
        void setFloat(uint32_t at, float f) { setWord(at, floatBits(f)); }
        const uint8_t *read(uint32_t address, uint32_t size, uint32_t align)
        {
            static const uint8_t zeros[64] = {}; // a declined read's stand-in (never used: callers stop on !ok)
            const uint8_t *p = ok && (address & (align - 1u)) == 0u ? mem.read(address, size) : nullptr;
            if (!p)
                ok = false;
            return p ? p : zeros;
        }
        static uint32_t at32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        // cvt.s.w of a byte / halfword, mul.s, add.s
        float value(uint32_t q, float scale, float base) { return x.add(x.mul(static_cast<float>(q), scale), base); }

        // ---- func_00159DA0 (a0 = the cursor; asm): the bracketing keys' values and tangents ----------------------------
        // kind 1: pos / dpos by 0x156508 (twice), the tangents' four by 0x156550 (on the stack, the table at 0x41EDF0);
        // +0x14 = cur's value, +0x1C = cur's out tangent, +0x10 = the key before's value, +0x18 its in tangent. 3: the
        // same from src's floats and the keys' shorts. 5: the keys' floats copied. Else nothing.
        void keys()
        {
            const uint32_t kind = o[0];
            if (kind == 1u)
            {
                calls += 3;
                const uint8_t *s = read(word(4), 12u, 2u);
                const uint8_t *table = read(kHalfTable, 64u, 16u);
                const uint8_t *c = read(word(8) - 4u, 8u, 1u);
                if (!ok)
                    return;
                const float pos = bitsFloat(halfBits(u16At(s))), dpos = bitsFloat(halfBits(u16At(s + 2)));
                uint32_t t[16];
                std::memcpy(t, table, 64);
                float lane[4];
                for (uint32_t q = 0; q < 4u; ++q)
                    lane[q] = bitsFloat(halfLane(u16At(s + 4u + 2u * q), t, q));
                setFloat(0x14u, value(c[5], dpos, pos));
                setFloat(0x1Cu, value(c[6], lane[1], lane[0]));
                setFloat(0x10u, value(c[1], dpos, pos));
                setFloat(0x18u, value(c[3], lane[3], lane[2]));
            }
            else if (kind == 3u)
            {
                const uint8_t *s = read(word(4), 0x18u, 4u);
                const uint8_t *c = read(word(8) - 8u, 0x10u, 2u);
                if (!ok)
                    return;
                float f[6];
                std::memcpy(f, s, sizeof(f));
                setFloat(0x14u, value(u16At(c + 0xA), f[1], f[0]));
                setFloat(0x1Cu, value(u16At(c + 0xC), f[3], f[2]));
                setFloat(0x10u, value(u16At(c + 2), f[1], f[0]));
                setFloat(0x18u, value(u16At(c + 6), f[5], f[4]));
            }
            else if (kind == 5u)
            {
                const uint8_t *c = read(word(8) - 0x10u, 0x1Cu, 4u);
                if (!ok)
                    return;
                setWord(0x10u, at32(c + 4));
                setWord(0x14u, at32(c + 0x14));
                setWord(0x18u, at32(c + 0xC));
                setWord(0x1Cu, at32(c + 0x18));
            }
        }

        // ---- RecDecode_readHeader 0x1599B0 (a0 = the cursor, a1 = the block, a2 = the record; src/cod/001599b0.c) -----
        // kind = (record word >> 12) & 0xF; kinds 1 / 3 / 5 point the cursor at the keys (block + the record's +8), reset
        // it when the record (+2) has fewer than 2 keys (kind 0, +0x10 = 0), else t0 = 0, t1 = the first key's frame and
        // func_00159DA0; any other kind keeps the record's +8 word as the value.
        void header(uint32_t block, uint32_t record)
        {
            const uint8_t *r = read(record, 12u, 4u);
            if (!ok)
                return;
            const uint32_t kind = (at32(r) >> 12) & 0xFu, count = u16At(r + 2), off = at32(r + 8);
            o[0] = static_cast<uint8_t>(kind);
            if (kind != 1u && kind != 3u && kind != 5u)
            {
                setWord(0x10u, off);
                return;
            }
            const uint32_t p = block + off;
            const uint32_t head = kind == 1u ? 0xCu : kind == 3u ? 0x18u : 0u;
            if (kind != 5u)
                setWord(4u, p);
            setWord(8u, p + head);
            if (count < 2u)
            {
                o[0] = 0u;
                setWord(0x10u, 0u);
                return;
            }
            setHalf(0xCu, 0u);
            setWord(8u, p + head + (kind == 1u ? 4u : kind == 3u ? 8u : 0x10u));
            const uint8_t *first = read(word(8), kind == 1u ? 1u : 2u, kind == 1u ? 1u : 2u);
            if (!ok)
                return;
            setHalf(0xEu, kind == 1u ? first[0] : u16At(first));
            calls += 1;
            keys();
        }

        // ---- func_00159AD0 (a0 = the cursor, a1 = the block, a2 = the record, f12 = t; asm): the value at t in f0 (bits)
        // kind 0: +0x10. s1 = t's frame rounded up (0 below 0: cvt.w.s, cvt.s.w, c.eq). t before t0: RecDecode_readHeader
        // (start over). kinds 1 / 3 / 5 while t1 < s1 (sltu): step a key (t0 = t1, cur += 4 / 8 / 16, t1 = t0 + the key's
        // byte (16 bits) or the key's frame), then func_00159DA0. t on t0: +0x10; on t1: +0x14; kinds 1 / 3 / 5 the Hermite
        // between them; else +0x10. s1 past 0xFFFF on a walk never ends (t1 is 16 bits): declined.
        uint32_t decode(uint32_t block, uint32_t record, float t)
        {
            if (o[0] == 0u)
                return word(0x10u);
            uint32_t s1 = 0u;
            if (!fpuLess(t, bitsFloat(0u)))
            {
                const int32_t i = fpuToWord(t);
                s1 = static_cast<uint32_t>(i);
                if (i >= 0 && i <= 0xFFFF && !fpuEqual(static_cast<float>(i), t))
                    ++s1;
                else if (i < 0 || i > 0xFFFF)
                    s1 = 0x10000u; // past 0xFFFF (cvt.s.w's chop not needed: a walk declines, else s1 isn't read)
            }
            if (fpuLess(t, static_cast<float>(half(0xCu))))
            {
                calls += 1;
                header(block, record);
                if (!ok)
                    return 0u;
            }
            uint32_t kind = o[0];
            if ((kind == 1u || kind == 3u || kind == 5u) && half(0xEu) < s1)
            {
                if (s1 > 0xFFFFu)
                {
                    ok = false;
                    return 0u;
                }
                const uint32_t stride = kind == 1u ? 4u : kind == 3u ? 8u : 0x10u;
                uint32_t cur = word(8u), t0 = 0u, t1 = half(0xEu);
                for (uint32_t step = 0;; ++step)
                {
                    if (step == kMaxSteps)
                    {
                        ok = false;
                        return 0u;
                    }
                    t0 = t1;
                    cur += stride;
                    const uint8_t *k = read(cur, kind == 1u ? 1u : 2u, kind == 1u ? 1u : 2u);
                    if (!ok)
                        return 0u;
                    t1 = kind == 1u ? (t0 + k[0]) & 0xFFFFu : u16At(k);
                    if (t1 >= s1)
                        break;
                    ++loops;
                }
                setHalf(0xCu, t0);
                setWord(8u, cur);
                setHalf(0xEu, t1);
                calls += 1;
                keys();
                if (!ok)
                    return 0u;
            }
            const uint32_t t0 = half(0xCu), t1 = half(0xEu);
            if (fpuEqual(static_cast<float>(t0), t))
                return word(0x10u);
            if (fpuEqual(static_cast<float>(t1), t))
                return word(0x14u);
            kind = o[0];
            if (kind != 1u && kind != 3u && kind != 5u)
                return word(0x10u);
            return floatBits(hermite(t, t0, t1));
        }
        // 0x159CE4: s = (t - t0) / (t1 - t0) (subu, cvt.s.w); h00 = 2s^3 - 3s^2 + 1, h01 = -2s^3 + 3s^2, h10 = s^3 - 2s^2
        // + s, h11 = s^3 - s^2 on +0x10 / +0x14 / +0x18 / +0x1C, in the asm's order.
        float hermite(float t, uint32_t t0, uint32_t t1)
        {
            const float a = static_cast<float>(t0);
            const float span = static_cast<float>(static_cast<int32_t>(t1 - t0));
            const float s = x.div(x.sub(t, a), span);
            float f1 = x.mul(s, s);
            float f5 = x.mul(f1, s);
            const float f6 = x.mul(f1, bitsFloat(0x40400000u));
            float f2 = x.add(f5, f5);
            f5 = x.sub(f5, f1);
            float f3 = negBits(f2);
            f2 = x.sub(f2, f6);
            f1 = x.sub(f5, f1);
            f3 = x.add(f3, f6);
            f2 = x.add(f2, one());
            f1 = x.add(f1, s);
            const float f8 = x.mul(bitsFloat(word(0x14u)), f3);
            float f0 = x.mul(bitsFloat(word(0x10u)), f2);
            const float f9 = x.mul(bitsFloat(word(0x18u)), f1);
            const float f7 = x.mul(bitsFloat(word(0x1Cu)), f5);
            f0 = x.add(f0, f8);
            f0 = x.add(f0, f9);
            return x.add(f0, f7);
        }
    };

    // ==== M5 batch 2: the IK leaves (claude/rung4-motion-map.md "M5 batch 2") ========================================
    // capVu0ApplyMatrixXYZ 0x1F8350 (src/cod/001f8350.c): vmulax / vmadday of rows 0 and 1, vmaddz of row 2, by v.xyz (all
    // four lanes; row 3 never). acc = ACC after vmadday (what the original leaves there). Rows below 2^62 in every lane,
    // v's xyz too (v.w is never read).
    inline bool applyXyzInputs(Vec4 r0, Vec4 r1, Vec4 r2, Vec4 v)
    {
        return libvu0::finiteInputs(r0) && libvu0::finiteInputs(r1) && libvu0::finiteInputs(r2) && libvu0::finiteXyz(v);
    }
    inline libvu0::RowResult applyXyz(Vec4 r0, Vec4 r1, Vec4 r2, Vec4 v)
    {
        Vec4 acc = mul(r0, splat<0>(v));
        acc = add(acc, mul(r1, splat<1>(v)));
        return libvu0::RowResult{acc, add(acc, mul(r2, splat<2>(v)))};
    }

    // sceVu0InversMatrix 0x30A3F8 (libvu0.cpp inversOn) on rows: rows 0-2 = in's 3x3 transposed by word (their w lanes
    // row 3's xyz cleared: x - x), row 3 = inversRows' (-(t R^T), in row 3's w). Every row's xyz below 2^62 (inversInputs).
    inline bool inversRigidInputs(const Mat4 &in)
    {
        for (int k = 0; k < 4; ++k)
            if (!libvu0::finiteXyz(in.r[k]))
                return false;
        return true;
    }
    inline Mat4 inversRigid(const Mat4 &in)
    {
        const Vec4 v5 = in.r[3];
        const Vec4 v9 = blend<7>(v5, sub(v5, v5));
        uint32_t r0[4], r1[4], r2[4], r3[4];
        store(r0, in.r[0]), store(r1, in.r[1]), store(r2, in.r[2]), store(r3, v9);
        const Vec4 c0 = bitsOf(r0[0], r1[0], r2[0], r3[0]), c1 = bitsOf(r0[1], r1[1], r2[1], r3[1]),
                   c2 = bitsOf(r0[2], r1[2], r2[2], r3[2]);
        return Mat4{{c0, c1, c2, libvu0::inversRows(c0, c1, c2, v5, v9, splat(0.0f)).vf4}};
    }

    // Quat_rotatePoint 0x15ADA0 (src/cod/0015ada0.c: Quat_mul(t, q, (p, 0)), Quat_mul(r, t, conj q)): out.xyz = r.xyz in
    // the asm's FPU order (p.w = 0 comes from the frame: each x * 0 is a real product). r.w only reaches the frame (dead):
    // not computed.
    inline void quatRotatePoint(Exact &x, const float q[4], const float p[3], float out[3])
    {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3], px = p[0], py = p[1], pz = p[2];
        const float zero = registerConstant(bitsFloat(0u));
        const float f17 = x.mul(qw, pz);
        float f7 = x.mul(qy, pz);
        float f3 = x.mul(qz, px);
        float f13 = x.mul(qz, py);
        float f6 = x.mul(qx, py);
        float f4 = x.mul(qy, px);
        const float f14 = x.mul(qx, pz);
        const float f16 = x.mul(qz, pz);
        const float f2 = x.mul(qx, zero);
        f6 = x.sub(f6, f4);
        f4 = x.mul(qy, zero);
        f7 = x.sub(f7, f13);
        f3 = x.sub(f3, f14);
        const float f15 = x.mul(qx, px);
        float f8 = x.mul(qw, zero);
        f13 = x.mul(qy, py);
        const float f5 = x.mul(qz, zero);
        const float f1 = x.mul(qw, px);
        const float f0 = x.mul(qw, py);
        f7 = x.add(f7, f2);
        f3 = x.add(f3, f4);
        f8 = x.sub(f8, f15);
        const float tx = x.add(f7, f1);
        const float ty = x.add(f3, f0);
        const float nx = negBits(qx), ny = negBits(qy), nz = negBits(qz);
        f6 = x.add(f6, f5);
        f8 = x.sub(f8, f13);
        const float tz = x.add(f6, f17);
        const float g5 = x.mul(ty, nx);
        float g0 = x.mul(tx, ny);
        const float tw = x.sub(f8, f16);
        float g1 = x.mul(tz, nx);
        const float g13 = x.mul(tx, nz);
        g0 = x.sub(g0, g5);
        const float g14 = x.mul(tz, ny);
        float g2 = x.mul(ty, nz);
        const float g7 = x.mul(tx, qw), g3 = x.mul(ty, qw), g11 = x.mul(tz, qw);
        g1 = x.sub(g1, g13);
        g2 = x.sub(g2, g14);
        const float g9 = x.mul(tw, nx), g10 = x.mul(tw, ny);
        g1 = x.add(g1, g3);
        g0 = x.add(g0, g11);
        const float g8 = x.mul(tw, nz);
        g2 = x.add(g2, g7);
        out[1] = x.add(g1, g10);
        out[2] = x.add(g0, g8);
        out[0] = x.add(g2, g9);
    }

    // func_0015ABA0 (asm): a quaternion for the basis with z row b (as given), x / y from b x a. c = b x a (FPU, the asm's
    // order), n1 = c normalized by capVu0Length (vf4 = c with w 1.0; 0 < length: xyz * (1 / length), w 1.0; else (0, 0,
    // 0, 1)), d = n1 x b, n2 likewise; rows n2, n1, b over the unit matrix (w 0; row 3 (0, 0, 0, 1)); 0x15A190 into q.
    // false = a length's input past 2^62 (capVu0Length's check); x.ok = the FPU's.
    inline bool normalizeByLength(Exact &x, const float c[3], float n[3])
    {
        const Vec4 v = bitsOf(floatBits(c[0]), floatBits(c[1]), floatBits(c[2]), 0x3F800000u);
        if (!libvu0::finiteXyz(v))
            return false;
        const float len = lane<0>(libvu0::length(vf0Constant(), v, 0).vf4);
        if (!fpuLess(bitsFloat(0u), len))
        {
            n[0] = n[1] = n[2] = bitsFloat(0u);
            return true;
        }
        const float r = x.div(one(), len);
        n[2] = x.mul(c[2], r);
        n[0] = x.mul(c[0], r);
        n[1] = x.mul(c[1], r);
        return true;
    }
    inline bool lookQuat(Exact &x, const float a[3], const float b[3], const uint32_t next[3], float q[4])
    {
        const float z0 = b[0], z1 = b[1], z2 = b[2];
        float c[3], n1[3], d[3], n2[3];
        {
            const float f6 = x.mul(z1, a[0]), f4 = x.mul(z2, a[0]), f2 = x.mul(z2, a[1]), f1 = x.mul(z0, a[1]);
            const float f3 = x.mul(z1, a[2]), f0 = x.mul(z0, a[2]);
            c[2] = x.sub(f1, f6);
            c[0] = x.sub(f3, f2);
            c[1] = x.sub(f4, f0);
        }
        if (!normalizeByLength(x, c, n1))
            return false;
        {
            const float f6 = x.mul(n1[2], z1), f4 = x.mul(n1[1], z2), f1 = x.mul(n1[0], z1), f3 = x.mul(n1[1], z0);
            const float f2 = x.mul(n1[2], z0), f0 = x.mul(n1[0], z2);
            d[0] = x.sub(f4, f6);
            d[2] = x.sub(f1, f3);
            d[1] = x.sub(f2, f0);
        }
        if (!normalizeByLength(x, d, n2))
            return false;
        Mat4 m = unit();
        m.r[0] = bitsOf(floatBits(n2[0]), floatBits(n2[1]), floatBits(n2[2]), 0u);
        m.r[1] = bitsOf(floatBits(n1[0]), floatBits(n1[1]), floatBits(n1[2]), 0u);
        m.r[2] = bitsOf(floatBits(z0), floatBits(z1), floatBits(z2), 0u);
        mat2quat(x, m, next, q);
        return true;
    }

    // ==== M5 batch 3: func_00153158, IK's two-bone solve (claude/rung4-motion-map.md "M5 batch 3") ===================
    // The quaternion product both of its products use: o = a * b, each lane in the asm's order.
    inline void ikQuatMul(Exact &x, const float a[4], const float b[4], float o[4])
    {
        const float ax = a[0], ay = a[1], az = a[2], aw = a[3], bx = b[0], by = b[1], bz = b[2], bw = b[3];
        o[0] = x.add(x.add(x.sub(x.mul(ay, bz), x.mul(az, by)), x.mul(ax, bw)), x.mul(aw, bx));
        o[1] = x.add(x.add(x.sub(x.mul(az, bx), x.mul(ax, bz)), x.mul(ay, bw)), x.mul(aw, by));
        o[2] = x.add(x.add(x.sub(x.mul(ax, by), x.mul(ay, bx)), x.mul(az, bw)), x.mul(aw, bz));
        o[3] = x.sub(x.sub(x.sub(x.mul(aw, bw), x.mul(ax, bx)), x.mul(ay, by)), x.mul(az, bz));
    }
    // capVu0Acos's body (its native's: sqrt.s(1 - x x), then atan2): its own declines (sqrt.s of 0 at +-1) via x.
    inline float ikAcos(Exact &x, float v, const float atan[9])
    {
        return atan2Body(x, x.sqrt(x.sub(one(), x.mul(v, v))), v, atan);
    }
    // The clamp before each capVu0Acos: c.lt.s c, -1.0 -> -1.0; then c.lt.s c, 1.0 ? c : 1.0.
    inline float ikClamp(float c)
    {
        const float minusOne = bitsFloat(0xBF800000u);
        if (fpuLess(c, minusOne))
            c = minusOne;
        return fpuLess(c, one()) ? c : one();
    }
    // The normalize after each capVu0Length in 0x153158: v as lqc2 has it (any w); 0 < length (c.lt.s): xyz * (1 /
    // length) (div.s, mul.s), w = v's; else vf0 (0, 0, 0, 1). false = capVu0Length's check (xyz past 2^62).
    inline bool ikNormalize(Exact &x, Vec4 v, Vec4 &n)
    {
        if (!libvu0::finiteXyz(v))
            return false;
        const float len = lane<0>(libvu0::length(vf0Constant(), v, 0).vf4);
        if (!fpuLess(bitsFloat(0u), len))
        {
            n = vf0Constant();
            return true;
        }
        const float r = x.div(one(), len);
        uint32_t w[4];
        store(w, v);
        n = bitsOf(floatBits(x.mul(lane<0>(v), r)), floatBits(x.mul(lane<1>(v), r)), floatBits(x.mul(lane<2>(v), r)), w[3]);
        return true;
    }
    // Everything 0x153158 computes before its first guest write (steps 1-7 of the map): the inputs as the original reads
    // them (posA / posC by lqc2, la / lb at +0x158, qA at A +0x160, 0x3BD8A0's xyz, A's world rows 0-2, capVu0Atan2's
    // table, 0x15A190's next table, capVu0Cos's table at the first SetRotationAxis). Out: P = q0 * qA, the local axis,
    // ang2, qa = P * r1 (A's new rotation). calls: the guest calls so far (LengthSq, the two capVu0Acos and their atan2,
    // Quat_rotatePoint, ApplyMatrixXYZ, the thunks and capVu0Length, 0x15ABA0 and its six, SetRotationAxis with sinf's and
    // capVu0Cos). false or x.ok = false: decline.
    struct IkSolveIn
    {
        Vec4 posA, posC, wA[3];
        float la, lb, qA[4], xAxis[3], atan[9], cos[4];
        uint32_t next[3];
    };
    struct IkSolveHalf
    {
        float P[4], axis[3], ang2, qa[4];
        bool bent;
    };
    inline bool ikSolveFirst(Exact &x, const IkSolveIn &in, IkSolveHalf &h, int &calls)
    {
        if (!libvu0::finiteXyz(in.posA) || !libvu0::finiteXyz(in.posC))
            return false;
        // 1: dist = sqrt.s(capVu0LengthSq((A - C).xyz)) (sqrtf never: the runtime's c.eq.s of a value with itself)
        const float dist = x.sqrt(lengthSq(x, x.xyz(sub(in.posA, in.posC))));
        calls = 1;
        // 3: the bend angles (law of cosines, the asm's order), else both +0
        float ang1 = bitsFloat(0u), ang2 = bitsFloat(0u);
        const float la = in.la, lb = in.lb;
        h.bent = fpuLess(dist, x.add(la, lb));
        if (h.bent)
        {
            const float f5 = x.mul(la, la), f2 = x.mul(dist, dist), f4 = x.mul(lb, lb);
            const float c1 = x.div(x.sub(x.add(f5, f2), f4), x.mul(x.add(la, la), dist));
            const float c2 = x.div(x.sub(x.add(f2, f4), f5), x.mul(x.add(dist, dist), lb));
            ang1 = ikAcos(x, ikClamp(c1), in.atan);
            ang2 = ikAcos(x, ikClamp(c2), in.atan);
            calls += 4;
        }
        // 4: the local axis (Quat_rotatePoint of (1, 0, 0) by qA), its world direction by A's rows 0-2, normalized
        quatRotatePoint(x, in.qA, in.xAxis, h.axis);
        const Vec4 axis4 = bitsOf(floatBits(h.axis[0]), floatBits(h.axis[1]), floatBits(h.axis[2]), 0x3F800000u);
        if (!applyXyzInputs(in.wA[0], in.wA[1], in.wA[2], axis4))
            return false;
        Vec4 n, u;
        if (!ikNormalize(x, applyXyz(in.wA[0], in.wA[1], in.wA[2], axis4).r, n))
            return false;
        // 5: the reach direction (C - A).xyz, w = C's, normalized
        if (!ikNormalize(x, blend<7>(in.posC, x.xyz(sub(in.posC, in.posA))), u))
            return false;
        calls += 1 + 1 + 2 + 2;
        // 6: q0 = func_0015ABA0(n, u)
        float a[3] = {lane<0>(n), lane<1>(n), lane<2>(n)}, b[3] = {lane<0>(u), lane<1>(u), lane<2>(u)}, q0[4];
        if (!lookQuat(x, a, b, in.next, q0))
            return false;
        calls += 7;
        // 7: P = q0 * qA, r1 about the axis by -ang1, qa = P * r1
        ikQuatMul(x, q0, in.qA, h.P);
        float r1[4];
        int sinfCalls = 0;
        axisQuat(x, h.axis, negBits(ang1), in.cos, sinfCalls, r1);
        ikQuatMul(x, h.P, r1, h.qa);
        calls += 3 + sinfCalls;
        h.ang2 = ang2;
        return x.ok;
    }
    // Step 9's rotation: qb = P * (the axis by ang2), with capVu0Cos's table as read then. calls: SetRotationAxis's.
    inline void ikSolveSecond(Exact &x, const IkSolveHalf &h, const float cos[4], float qb[4], int &calls)
    {
        float r2[4];
        int sinfCalls = 0;
        axisQuat(x, h.axis, h.ang2, cos, sinfCalls, r2);
        ikQuatMul(x, h.P, r2, qb);
        calls = 3 + sinfCalls;
    }
}
