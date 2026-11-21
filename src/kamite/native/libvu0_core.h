#pragma once

// The libvu0 helpers' maths on plain Vec4s (portable: SSE, NEON, plain C++), shared by the natives (libvu0.cpp, which
// adds the guest context) and the offline checks (tools/native). Each op is the recompiled code's op in its order
// under the default float rules (claude/gt3-revisited.md "How ours works"): VMUL = the host product, VMADD = the host
// product rounded, then the host sum (no FMA); results are the recompiled code's bit for bit as long as nothing
// reaches FLT_MAX - 1 ulp, which the natives' entry checks rule out (finiteInputs).

#include "kamite/native/vu_vec.h"

namespace kamite::native::libvu0
{
    // |x| < 2^62 in every lane: products of two such values stay below 2^124 and sums of four below 2^126, far from
    // FLT_MAX, so the recompiled ops never take their exact-lane path (and no exponent-255 operand is let in).
    inline bool finiteInputs(Vec4 v) { return magnitudesBelow(v, 0x5E800000u); } // 2^62

    // vmulax / vmadday / vmaddaz (ACC) then vmaddw (the result): one row of a matrix product, b's lanes weighting
    // a0..a3. acc = ACC after the third step (what the original leaves in ACC).
    struct RowResult
    {
        Vec4 acc;
        Vec4 r;
    };
    inline RowResult mulRow(Vec4 a0, Vec4 a1, Vec4 a2, Vec4 a3, Vec4 b)
    {
        Vec4 acc = mul(a0, splat<0>(b));
        acc = add(acc, mul(a1, splat<1>(b)));
        acc = add(acc, mul(a2, splat<2>(b)));
        const Vec4 r = add(acc, mul(a3, splat<3>(b)));
        return RowResult{acc, r};
    }
}
