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
}
