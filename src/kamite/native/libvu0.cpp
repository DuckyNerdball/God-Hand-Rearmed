// Native libvu0 helpers (Sony's VU0 macro-mode matrix library, linked into the game): the rung 2 pilot
// (claude/native-next.md). Leaf functions, called everywhere (rung 1: libvu0 4.9% of the game thread's busy samples, own
// time), and part of rung 3's packet building. Switch: KAMITE_NATIVE_LIBVU0 (unset = the checked ones, 1 = all, 0 =
// none); a native that is not checked yet runs in the shadow harness during dev replays only.
//
// Each run<Env>() is the recompiled function's effect (SLUS_215.03, generated/sceVu0*.cpp) without the per-op
// bookkeeping: same loads, same ops in the same order (libvu0_core.h), same stores, same registers left behind (the
// VU0 temporaries vf4-vf9 and ACC, the argument registers it steps, a3 as its loop counter), the same checkpoints.

#include "kamite/native/native.h"
#include "kamite/native/libvu0_core.h"
#include "kamite/native/ee_ctx.h"

#include <cstring>

namespace kamite::native::libvu0
{
    namespace
    {
        uint32_t gpr32(const R5900Context *ctx, int reg)
        {
            uint32_t value;
            std::memcpy(&value, &ctx->r[reg], sizeof(value));
            return value;
        }

        // SET_GPR_S32: the low doubleword, sign-extended; the upper doubleword stays.
        void setGprS32(R5900Context *ctx, int reg, uint32_t value)
        {
            const int64_t wide = static_cast<int32_t>(value);
            std::memcpy(&ctx->r[reg], &wide, sizeof(wide));
        }

        // ADDI's overflow trap (SignalException): never taken by a pointer stepped through plain RAM, but a native
        // declines the call rather than differ.
        bool addiOverflows(uint32_t base, uint32_t step)
        {
            const int64_t sum = static_cast<int64_t>(static_cast<int32_t>(base)) + static_cast<int32_t>(step);
            return sum != static_cast<int32_t>(static_cast<uint32_t>(sum));
        }

        bool overlapsUnequal(uint32_t a, uint32_t b, uint32_t size) { return a != b && a < b + size && b < a + size; }

        void setVf(R5900Context *ctx, int reg, Vec4 v) { store(&ctx->vu0_vf[reg], v); }

        // ---- sceVu0MulMatrix 0x30A2E0 (a0 = out, a1 = A, a2 = B): out = B x A (row vectors) -----------------
        // vf4-vf7 = A's rows; per row: vf8 = B row k, ACC / vf9 = mulRow, out row k = vf9, a3 / a2 / a0 stepped, a
        // backward branch (checkpoint, resume entry 0x30A2F4) after rows 0-2.
        constexpr uint32_t kMulMatrix = 0x30A2E0u, kMulMatrixLoop = 0x30A2F4u;

        struct MulMatrixArgs
        {
            uint32_t a0, a2, out, ma, mb;
        };
        // MulMatrix's entry checks (also MtxMultiply's: the same arguments). Touches the three matrices through env.at
        // first, so a caller with a recorded footprint has them filled.
        template <class Env>
        bool mulMatrixCheck(Env &env, const R5900Context *ctx, MulMatrixArgs &m)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5), a2 = gpr32(ctx, 6);
            if (!plainRam(a0 & ~0xFu, 64u, m.out) || !plainRam(a1 & ~0xFu, 64u, m.ma) || !plainRam(a2 & ~0xFu, 64u, m.mb))
                return false;
            // The original reads B row k after writing out row k - 1: an out that overlaps B (but is not B) would feed
            // its own rows back; it never happens in the game's calls, so decline it rather than mimic it.
            if (overlapsUnequal(m.out, m.mb, 64u) || addiOverflows(a0, 64u) || addiOverflows(a2, 64u))
                return false;
            m.a0 = a0;
            m.a2 = a2;
            env.at(m.ma, 64u);
            env.at(m.mb, 64u);
            env.at(m.out, 64u);
            const uint8_t *const ram = env.ram;
            for (uint32_t row = 0; row < 64u; row += 16u)
                if (!finiteInputs(load(ram + m.ma + row)) || !finiteInputs(load(ram + m.mb + row)))
                    return false;
            return true;
        }

        // vf4-vf7 = A's rows; per row: vf8 = B row k, ACC / vf9 = mulRow, out row k = vf9, a3 / a2 / a0 stepped, a
        // backward branch (checkpoint, resume entry 0x30A2F4) after rows 0-2. Leaves pc = ra or the resume entry.
        template <class Env>
        void mulMatrixBody(Env &env, R5900Context *ctx, const MulMatrixArgs &m)
        {
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + m.ma), m1 = load(ram + m.ma + 16), m2 = load(ram + m.ma + 32), m3 = load(ram + m.ma + 48);
            setVf(ctx, 4, m0);
            setVf(ctx, 5, m1);
            setVf(ctx, 6, m2);
            setVf(ctx, 7, m3);
            for (uint32_t k = 0;; ++k)
            {
                const Vec4 b = load(ram + m.mb + 16u * k); // out == B: row k is still B's (row k - 1 went before it)
                const RowResult row = mulRow(m0, m1, m2, m3, b);
                store(ram + m.out + 16u * k, row.r);
                if (k == 3u || env.checkpoint(32u))
                {
                    setVf(ctx, 8, b);
                    store(&ctx->vu0_acc, row.acc);
                    setVf(ctx, 9, row.r);
                    setGprS32(ctx, 7, 3u - k);
                    setGprS32(ctx, 6, m.a2 + 16u * (k + 1u));
                    setGprS32(ctx, 4, m.a0 + 16u * (k + 1u));
                    ctx->pc = k == 3u ? gpr32(ctx, 31) : kMulMatrixLoop;
                    return;
                }
            }
        }

        template <class Env>
        bool mulMatrix(Env &env, R5900Context *ctx)
        {
            MulMatrixArgs m;
            if (!mulMatrixCheck(env, ctx, m))
                return false;
            mulMatrixBody(env, ctx, m);
            return true;
        }

        void mulMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 64u);
        }

        // ---- sceVu0ApplyMatrix 0x30A2B0 (a0 = out vector, a1 = matrix, a2 = vector) --------------------------------
        constexpr uint32_t kApplyMatrix = 0x30A2B0u;

        // Its checks from the argument values (a caller running it inline predicts them); offsets out. Touches the
        // three through env.at (a caller's recorded footprint).
        template <class Env>
        bool applyCheck(Env &env, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t &out, uint32_t &ma, uint32_t &vb)
        {
            if (!plainRam(a0 & ~0xFu, 16u, out) || !plainRam(a1 & ~0xFu, 64u, ma) || !plainRam(a2 & ~0xFu, 16u, vb))
                return false;
            env.at(ma, 64u);
            env.at(vb, 16u);
            env.at(out, 16u);
            uint8_t *const ram = env.ram;
            return finiteInputs(load(ram + ma)) && finiteInputs(load(ram + ma + 16)) && finiteInputs(load(ram + ma + 32)) &&
                   finiteInputs(load(ram + ma + 48)) && finiteInputs(load(ram + vb));
        }
        template <class Env>
        void applyOn(Env &env, R5900Context *ctx, uint32_t out, uint32_t ma, uint32_t vb)
        {
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            const Vec4 b = load(ram + vb);
            const RowResult row = mulRow(m0, m1, m2, m3, b);
            store(ram + out, row.r);
            setVf(ctx, 4, m0);
            setVf(ctx, 5, m1);
            setVf(ctx, 6, m2);
            setVf(ctx, 7, m3);
            setVf(ctx, 8, b);
            store(&ctx->vu0_acc, row.acc);
            setVf(ctx, 9, row.r);
            ctx->pc = gpr32(ctx, 31);
        }
        template <class Env>
        bool applyMatrix(Env &env, R5900Context *ctx)
        {
            uint32_t out, ma, vb;
            if (!applyCheck(env, gpr32(ctx, 4), gpr32(ctx, 5), gpr32(ctx, 6), out, ma, vb))
                return false;
            applyOn(env, ctx, out, ma, vb);
            return true;
        }

        void applyMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 16u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 16u);
        }

        // ---- _sceVu0ecossin 0x30A5D0 and sceVu0RotMatrixX / Y / Z 0x30A700 / 0x30A7B8 / 0x30A650 -----------------
        // RotMatrix*(a0 = out, a1 = matrix, f12 = angle): f12 = pi/2 -+ angle (r7 = which), vf6 = all of r8 (its low
        // word the angle's bits), ra kept in a2 and the call to ecossin (a guest call: 8 cycles, may come due with
        // ctx->pc = 0x30A5D0), then the rotation rows vf6-vf9 and out = matrix x rotation row by row (a1 stepped, the
        // backward-branch checkpoints as MulMatrix's).
        constexpr uint32_t kEcossin = 0x30A5D0u, kCoefficients = 0x3C3C90u;
        constexpr uint32_t kPiOver2 = 0x3FC90FDBu;

        void setGprLow64(R5900Context *ctx, int reg, uint64_t value) { std::memcpy(&ctx->r[reg], &value, sizeof(value)); }
        uint64_t gpr64(const R5900Context *ctx, int reg)
        {
            uint64_t value;
            std::memcpy(&value, &ctx->r[reg], sizeof(value));
            return value;
        }
        Vec4 vf(const R5900Context *ctx, int reg) { return load(&ctx->vu0_vf[reg]); }

        // A garbage lane multiplied by 0 (vf4.yzw in ecossin) must not be the host's Inf / NaN (the original redoes such
        // a lane exactly).
        bool noExponent255(Vec4 v) { return magnitudesBelow(v, 0x7F800000u); }

        // ecossin on the context: what the original leaves (vf4-vf8, Q, the status flags, r8, pc = ra).
        void ecossinOn(R5900Context *ctx, const uint8_t *ram, uint32_t coefficients)
        {
            VuRegs v{vf(ctx, 4), vf(ctx, 5), vf(ctx, 6), vf(ctx, 7), vf(ctx, 8), ctx->vu0_q, ctx->vu0_status};
            ecossin(vf(ctx, 0), load(ram + coefficients), gpr64(ctx, 7) != 0u, v);
            setVf(ctx, 4, v.vf4);
            setVf(ctx, 5, v.vf5);
            setVf(ctx, 6, v.vf6);
            setVf(ctx, 7, v.vf7);
            setVf(ctx, 8, v.vf8);
            ctx->vu0_q = v.q;
            ctx->vu0_status = v.status;
            setGprS32(ctx, 8, kCoefficients); // lui / addiu
            ctx->pc = gpr32(ctx, 31);
        }

        template <class Env>
        bool ecossinNative(Env &env, R5900Context *ctx)
        {
            uint32_t coefficients;
            if (!plainRam(kCoefficients, 16u, coefficients))
                return false;
            // |angle| < 32: the polynomial's powers stay far below FLT_MAX.
            if ((laneBits<0>(vf(ctx, 6)) & 0x7FFFFFFFu) >= 0x42000000u || !noExponent255(vf(ctx, 4)) ||
                !finiteInputs(load(env.ram + coefficients)))
                return false;
            ecossinOn(ctx, env.ram, coefficients);
            return true;
        }

        void ecossinFootprint(const R5900Context *, Footprint &fp) { fp.add(kCoefficients, 16u); }

        struct RotEntry
        {
            uint32_t entry, back, loop; // the function, the return from ecossin, the loop head (resume entries)
        };
        constexpr RotEntry kRot[3] = {{0x30A700u, 0x30A74Cu, 0x30A780u}, {0x30A7B8u, 0x30A804u, 0x30A838u},
                                      {0x30A650u, 0x30A69Cu, 0x30A6CCu}};

        template <int Axis, class Env>
        bool rotMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5);
            uint32_t out, in, coefficients;
            if (!plainRam(a0 & ~0xFu, 64u, out) || !plainRam(a1 & ~0xFu, 64u, in) || !plainRam(kCoefficients, 16u, coefficients))
                return false;
            if (overlapsUnequal(out, in, 64u) || addiOverflows(a0, 64u) || addiOverflows(a1, 64u))
                return false;
            uint8_t *const ram = env.ram;
            const float angle = ctx->f[12];
            // |angle| < 16 (pi/2 -+ it is ecossin's input), a plain-sized matrix, a vf4 that * 0 stays 0.
            if ((floatBits(angle) & 0x7FFFFFFFu) >= 0x41800000u || !noExponent255(vf(ctx, 4)) ||
                !finiteInputs(load(ram + coefficients)) || !finiteInputs(load(ram + in)) || !finiteInputs(load(ram + in + 16)) ||
                !finiteInputs(load(ram + in + 32)) || !finiteInputs(load(ram + in + 48)))
                return false;
            const RotEntry &at = kRot[Axis];

            // c.olt.s f12, f0 (f0 = 0): a negative angle with a non-zero exponent sets C.
            const uint32_t angleBits = floatBits(angle);
            const bool negativeAngle = (angleBits & 0x80000000u) && (angleBits & 0x7F800000u);
            ctx->fcr31 = negativeAngle ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
            setGprLow64(ctx, 1, kPiOver2);
            const float piOver2 = bitsFloat(kPiOver2);
            ctx->f[0] = piOver2;
            ctx->f[12] = negativeAngle ? piOver2 + angle : piOver2 - angle;
            setGprLow64(ctx, 7, negativeAngle ? 1u : 0u);
            setGprS32(ctx, 8, floatBits(ctx->f[12]));
            std::memcpy(&ctx->vu0_vf[6], &ctx->r[8], 16); // qmtc2: the whole register, its upper half included
            setGprLow64(ctx, 6, gpr64(ctx, 31));
            setGprS32(ctx, 31, at.back);
            ctx->pc = kEcossin;
            if (env.call())
                return true; // the scheduler resumes at ecossin, which returns to the original's resume entry
            ecossinOn(ctx, ram, coefficients);
            setGprLow64(ctx, 31, gpr64(ctx, 6));

            Vec4 vf4 = vf(ctx, 4);
            const RotationRows m = rotationRows<Axis>(vf(ctx, 0), vf(ctx, 5), vf4);
            setVf(ctx, 6, m.r0);
            setVf(ctx, 7, m.r1);
            setVf(ctx, 8, m.r2);
            setVf(ctx, 9, m.r3);
            for (uint32_t k = 0;; ++k)
            {
                const Vec4 row = load(ram + in + 16u * k); // out == in: row k is still the input's
                const RowResult r = mulRow(m.r0, m.r1, m.r2, m.r3, row);
                store(ram + out + 16u * k, r.r);
                if (k == 3u || env.checkpoint(32u))
                {
                    setVf(ctx, 4, row);
                    store(&ctx->vu0_acc, r.acc);
                    setVf(ctx, 5, r.r);
                    setGprS32(ctx, 7, 3u - k);
                    setGprS32(ctx, 5, a1 + 16u * (k + 1u));
                    setGprS32(ctx, 4, a0 + 16u * (k + 1u));
                    ctx->pc = k == 3u ? gpr32(ctx, 31) : at.loop;
                    return true;
                }
            }
        }

        void rotMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 64u);
            fp.add(kCoefficients, 16u);
        }

        // ==== rung 3, step a, batch 1: the libvu0 helpers the character path calls (recorded footprints) ============
        // Each leaf is split in two: <name>Check (the entry checks, reading through env.at) and <name>On (the effect on
        // the context and RAM, from the argument registers, ending at pc = ra). A composite (CameraMatrix ...) checks
        // every leaf it will run before it changes anything, then runs them in order with the guest calls between.
        namespace e = kamite::native::ee;

        bool plain16(uint32_t address, uint32_t &offset) { return plainRam(address & ~0xFu, 16u, offset); }
        bool plain64(uint32_t address, uint32_t &offset) { return plainRam(address & ~0xFu, 64u, offset); }
        uint32_t off16(uint32_t address) { return (address & ~0xFu) & PS2_RAM_MASK; } // after the checks

        // ---- sceVu0CopyMatrix 0x30A548 (a0 = out, a1 = in): lq a2, a3, t0, t1; sq them ---------------------------
        constexpr uint32_t kCopyMatrix = 0x30A548u;
        template <class Env>
        void copyMatrixOn(Env &env, R5900Context *ctx)
        {
            const uint32_t in = off16(e::u32(ctx, e::a1)), out = off16(e::u32(ctx, e::a0));
            e::lq(env, ctx, e::a2, in);
            e::lq(env, ctx, e::a3, in + 16u);
            e::lq(env, ctx, e::t0, in + 32u);
            e::lq(env, ctx, e::t1, in + 48u);
            e::sq(env, ctx, e::a2, out);
            e::sq(env, ctx, e::a3, out + 16u);
            e::sq(env, ctx, e::t0, out + 32u);
            e::sq(env, ctx, e::t1, out + 48u);
            ctx->pc = e::u32(ctx, e::ra);
        }
        bool copyMatrixCheck(const R5900Context *ctx)
        {
            uint32_t o;
            return plain64(e::u32(ctx, e::a0), o) && plain64(e::u32(ctx, e::a1), o);
        }
        template <class Env>
        bool copyMatrix(Env &env, R5900Context *ctx)
        {
            if (!copyMatrixCheck(ctx))
                return false;
            copyMatrixOn(env, ctx);
            return true;
        }

        // ---- sceVu0UnitMatrix 0x30A5A0 (a0 = out): vf4 = (0,0,0,1), vf5-vf7 its rotations; rows 3..0 ------------
        constexpr uint32_t kUnitMatrix = 0x30A5A0u;
        template <class Env>
        void unitMatrixOn(Env &env, R5900Context *ctx)
        {
            const Vec4 vf0 = e::vf(ctx, 0);
            Vec4 v4 = sub(vf0, vf0);
            v4 = blend<8>(v4, add(v4, vf0));
            const Vec4 v5 = rotateLanes(v4), v6 = rotateLanes(v5), v7 = rotateLanes(v6);
            e::setVf(ctx, 4, v4);
            e::setVf(ctx, 5, v5);
            e::setVf(ctx, 6, v6);
            e::setVf(ctx, 7, v7);
            const uint32_t out = off16(e::u32(ctx, e::a0));
            e::write128(env, out + 48u, v4);
            e::write128(env, out + 32u, v5);
            e::write128(env, out + 16u, v6);
            e::write128(env, out, v7);
            ctx->pc = e::u32(ctx, e::ra);
        }
        bool unitMatrixCheck(const R5900Context *ctx)
        {
            uint32_t o;
            return e::vf0Constant(ctx) && plain64(e::u32(ctx, e::a0), o);
        }
        template <class Env>
        bool unitMatrix(Env &env, R5900Context *ctx)
        {
            if (!unitMatrixCheck(ctx))
                return false;
            unitMatrixOn(env, ctx);
            return true;
        }

        // ---- sceVu0MulVector 0x30A4D0 (a0 = out, a1, a2): vf6 = vf4 * vf5 ----------------------------------------
        constexpr uint32_t kMulVector = 0x30A4D0u;
        template <class Env>
        bool mulVectorCheck(Env &env, uint32_t a0, uint32_t a1, uint32_t a2)
        {
            uint32_t out, va, vb;
            if (!plain16(a0, out) || !plain16(a1, va) || !plain16(a2, vb))
                return false;
            return finiteInputs(e::read128(env, va)) && finiteInputs(e::read128(env, vb));
        }
        template <class Env>
        void mulVectorOn(Env &env, R5900Context *ctx)
        {
            const Vec4 a = e::read128(env, off16(e::u32(ctx, e::a1))), b = e::read128(env, off16(e::u32(ctx, e::a2)));
            const Vec4 r = mul(a, b);
            e::setVf(ctx, 4, a);
            e::setVf(ctx, 5, b);
            e::setVf(ctx, 6, r);
            e::write128(env, off16(e::u32(ctx, e::a0)), r);
            ctx->pc = e::u32(ctx, e::ra);
        }
        template <class Env>
        bool mulVector(Env &env, R5900Context *ctx)
        {
            if (!mulVectorCheck(env, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2)))
                return false;
            mulVectorOn(env, ctx);
            return true;
        }

        // ---- sceVu0ScaleVector 0x30A4E8 (a0 = out, a1 = v, f12 = s): t0 = mfc1 f12, vf5 = qmtc2 t0, vf6 = v * s ----
        constexpr uint32_t kScaleVector = 0x30A4E8u;
        template <class Env>
        void scaleVectorOn(Env &env, R5900Context *ctx)
        {
            const Vec4 v = e::read128(env, off16(e::u32(ctx, e::a1)));
            e::setVf(ctx, 4, v);
            e::setS32(ctx, e::t0, floatBits(ctx->f[12]));
            std::memcpy(&ctx->vu0_vf[5], &ctx->r[e::t0], 16);
            const Vec4 r = mul(v, splat<0>(e::vf(ctx, 5)));
            e::setVf(ctx, 6, r);
            e::write128(env, off16(e::u32(ctx, e::a0)), r);
            ctx->pc = e::u32(ctx, e::ra);
        }
        // Every lane of v (all four are multiplied and stored) and s below 2^62.
        template <class Env>
        bool scaleVectorCheck(Env &env, const R5900Context *ctx)
        {
            uint32_t out, in;
            if (!plain16(e::u32(ctx, e::a0), out) || !plain16(e::u32(ctx, e::a1), in))
                return false;
            return finiteInputs(e::read128(env, in)) && (floatBits(ctx->f[12]) & 0x7FFFFFFFu) < 0x5E800000u;
        }
        template <class Env>
        bool scaleVector(Env &env, R5900Context *ctx)
        {
            if (!scaleVectorCheck(env, ctx))
                return false;
            scaleVectorOn(env, ctx);
            return true;
        }

        // ---- sceVu0OuterProduct 0x30A328 (a0 = out, a1, a2): vf6.xyz = a x b, vf6.w = vf6.w - vf6.w (= +0) --------
        constexpr uint32_t kOuterProduct = 0x30A328u;
        template <class Env>
        void outerProductOn(Env &env, R5900Context *ctx)
        {
            const Vec4 a = e::read128(env, off16(e::u32(ctx, e::a1))), b = e::read128(env, off16(e::u32(ctx, e::a2)));
            e::setVf(ctx, 4, a);
            e::setVf(ctx, 5, b);
            const OuterResult o = outerProduct(a, b);
            store(&ctx->vu0_acc, blend<7>(load(&ctx->vu0_acc), o.acc));
            const Vec4 old6 = e::vf(ctx, 6);
            const Vec4 r = blend<8>(o.r, sub(old6, old6));
            e::setVf(ctx, 6, r);
            e::write128(env, off16(e::u32(ctx, e::a0)), r);
            ctx->pc = e::u32(ctx, e::ra);
        }
        // a, b: xyz below 2^62; vf6.w (subtracted from itself) not exponent 255.
        bool outerInputs(Vec4 a, Vec4 b, const R5900Context *ctx)
        {
            return finiteXyz(a) && finiteXyz(b) && !exponent255(laneBits<3>(e::vf(ctx, 6)));
        }
        template <class Env>
        bool outerProductNative(Env &env, R5900Context *ctx)
        {
            uint32_t out, va, vb;
            if (!plain16(e::u32(ctx, e::a0), out) || !plain16(e::u32(ctx, e::a1), va) || !plain16(e::u32(ctx, e::a2), vb))
                return false;
            if (!outerInputs(e::read128(env, va), e::read128(env, vb), ctx))
                return false;
            outerProductOn(env, ctx);
            return true;
        }

        // ---- sceVu0Normalize 0x30A370 (a0 = out, a1 = v) ---------------------------------------------------------
        constexpr uint32_t kNormalize = 0x30A370u;
        template <class Env>
        void normalizeOn(Env &env, R5900Context *ctx)
        {
            const Vec4 v = e::read128(env, off16(e::u32(ctx, e::a1)));
            const NormalizeResult n = normalize(e::vf(ctx, 0), v, e::vf(ctx, 5), ctx->vu0_status);
            e::setVf(ctx, 4, v);
            e::setVf(ctx, 5, n.vf5);
            e::setVf(ctx, 6, n.vf6);
            ctx->vu0_q = n.q;
            ctx->vu0_status = n.status;
            e::write128(env, off16(e::u32(ctx, e::a0)), n.vf6);
            ctx->pc = e::u32(ctx, e::ra);
        }
        // v.xyz below 2^62 and a sum of squares with exponent 1-254 (a zero vector divides by 0: ps2ee's path).
        bool normalizeInput(Vec4 v) { return finiteXyz(v) && ordinaryLength(squaresSummed(v, v)); }
        template <class Env>
        bool normalizeNative(Env &env, R5900Context *ctx)
        {
            uint32_t out, in;
            if (!e::vf0Constant(ctx) || !plain16(e::u32(ctx, e::a0), out) || !plain16(e::u32(ctx, e::a1), in) ||
                !normalizeInput(e::read128(env, in)))
                return false;
            normalizeOn(env, ctx);
            return true;
        }

        // ---- sceVu0TransMatrix 0x30A508 (a0 = out, a1 = m, a2 = v): out = m with row 3's xyz + v.xyz ------------
        constexpr uint32_t kTransMatrix = 0x30A508u;
        template <class Env>
        void transMatrixOn(Env &env, R5900Context *ctx)
        {
            const uint32_t m = off16(e::u32(ctx, e::a1)), out = off16(e::u32(ctx, e::a0));
            const Vec4 v = e::read128(env, off16(e::u32(ctx, e::a2)));
            Vec4 row3 = e::read128(env, m + 48u);
            e::setVf(ctx, 4, v);
            e::lq(env, ctx, e::a3, m);
            e::lq(env, ctx, e::t0, m + 16u);
            e::lq(env, ctx, e::t1, m + 32u);
            row3 = blend<7>(row3, add(row3, v));
            e::setVf(ctx, 5, row3);
            e::sq(env, ctx, e::a3, out);
            e::sq(env, ctx, e::t0, out + 16u);
            e::sq(env, ctx, e::t1, out + 32u);
            e::write128(env, out + 48u, row3);
            ctx->pc = e::u32(ctx, e::ra);
        }
        bool transInputs(Vec4 row3, Vec4 v) { return finiteXyz(row3) && finiteXyz(v); }
        template <class Env>
        bool transMatrix(Env &env, R5900Context *ctx)
        {
            uint32_t out, m, v;
            if (!plain64(e::u32(ctx, e::a0), out) || !plain64(e::u32(ctx, e::a1), m) || !plain16(e::u32(ctx, e::a2), v))
                return false;
            if (!transInputs(e::read128(env, m + 48u), e::read128(env, v)))
                return false;
            transMatrixOn(env, ctx);
            return true;
        }

        // ---- sceVu0TransposeMatrix 0x30A3B0 (a0 = out, a1 = in): integer word shuffles in t0-t7 ------------------
        constexpr uint32_t kTransposeMatrix = 0x30A3B0u;
        Words gprWords(const R5900Context *ctx, int reg)
        {
            Words w;
            std::memcpy(w.w, &ctx->r[reg], 16);
            return w;
        }
        void setWords(R5900Context *ctx, int reg, Words w) { e::setVec(ctx, reg, w.w); }
        template <class Env>
        void transposeOn(Env &env, R5900Context *ctx)
        {
            const uint32_t in = off16(e::u32(ctx, e::a1)), out = off16(e::u32(ctx, e::a0));
            e::lq(env, ctx, e::t0, in);
            e::lq(env, ctx, e::t1, in + 16u);
            e::lq(env, ctx, e::t2, in + 32u);
            e::lq(env, ctx, e::t3, in + 48u);
            const Words r0 = gprWords(ctx, e::t0), r1 = gprWords(ctx, e::t1), r2 = gprWords(ctx, e::t2), r3 = gprWords(ctx, e::t3);
            const Words t4 = pextlw(r1, r0), t5 = pextuw(r1, r0), t6 = pextlw(r3, r2), t7 = pextuw(r3, r2);
            setWords(ctx, e::t4, t4);
            setWords(ctx, e::t5, t5);
            setWords(ctx, e::t6, t6);
            setWords(ctx, e::t7, t7);
            setWords(ctx, e::t0, pcpyld(t6, t4));
            setWords(ctx, e::t1, pcpyud(t4, t6));
            setWords(ctx, e::t2, pcpyld(t7, t5));
            setWords(ctx, e::t3, pcpyud(t5, t7));
            e::sq(env, ctx, e::t0, out);
            e::sq(env, ctx, e::t1, out + 16u);
            e::sq(env, ctx, e::t2, out + 32u);
            e::sq(env, ctx, e::t3, out + 48u);
            ctx->pc = e::u32(ctx, e::ra);
        }
        template <class Env>
        bool transposeMatrix(Env &env, R5900Context *ctx)
        {
            uint32_t o;
            if (!plain64(e::u32(ctx, e::a0), o) || !plain64(e::u32(ctx, e::a1), o))
                return false;
            transposeOn(env, ctx);
            return true;
        }

        // ---- sceVu0InversMatrix 0x30A3F8 (a0 = out, a1 = in): rigid inverse (transposed 3x3, -t * R) ------------
        constexpr uint32_t kInversMatrix = 0x30A3F8u;
        template <class Env>
        void inversOn(Env &env, R5900Context *ctx)
        {
            const uint32_t in = off16(e::u32(ctx, e::a1)), out = off16(e::u32(ctx, e::a0));
            e::lq(env, ctx, e::t0, in);
            e::lq(env, ctx, e::t1, in + 16u);
            e::lq(env, ctx, e::t2, in + 32u);
            Vec4 v4 = e::read128(env, in + 48u);
            const Vec4 v5 = v4;
            v4 = blend<7>(v4, sub(v4, v4));
            const Vec4 v9 = v4;
            std::memcpy(&ctx->r[e::t3], &v4, 16); // qmfc2 t3, vf4
            const Words r0 = gprWords(ctx, e::t0), r1 = gprWords(ctx, e::t1), r2 = gprWords(ctx, e::t2), r3 = gprWords(ctx, e::t3);
            const Words t4 = pextlw(r1, r0), t5 = pextuw(r1, r0), t6 = pextlw(r3, r2), t7 = pextuw(r3, r2);
            setWords(ctx, e::t4, t4);
            setWords(ctx, e::t5, t5);
            setWords(ctx, e::t6, t6);
            setWords(ctx, e::t7, t7);
            const Words c0 = pcpyld(t6, t4), c1 = pcpyud(t4, t6), c2 = pcpyld(t7, t5);
            setWords(ctx, e::t0, c0);
            setWords(ctx, e::t1, c1);
            setWords(ctx, e::t2, c2);
            const Vec4 v6 = load(c0.w), v7 = load(c1.w), v8 = load(c2.w);
            const InversResult r = inversRows(v6, v7, v8, v5, v9, load(&ctx->vu0_acc));
            const Vec4 acc = r.acc;
            v4 = r.vf4;
            e::setVf(ctx, 4, v4);
            e::setVf(ctx, 5, v5);
            e::setVf(ctx, 6, v6);
            e::setVf(ctx, 7, v7);
            e::setVf(ctx, 8, v8);
            e::setVf(ctx, 9, v9);
            store(&ctx->vu0_acc, acc);
            e::sq(env, ctx, e::t0, out);
            e::sq(env, ctx, e::t1, out + 16u);
            e::sq(env, ctx, e::t2, out + 32u);
            e::write128(env, out + 48u, v4);
            ctx->pc = e::u32(ctx, e::ra);
        }
        // Every row's xyz below 2^62 (row 3's w is carried, never in a sum that reaches a result).
        template <class Env>
        bool inversInputs(Env &env, uint32_t in)
        {
            for (uint32_t row = 0; row < 64u; row += 16u)
                if (!finiteXyz(e::read128(env, in + row)))
                    return false;
            return true;
        }
        template <class Env>
        bool inversMatrix(Env &env, R5900Context *ctx)
        {
            uint32_t out, in;
            if (!plain64(e::u32(ctx, e::a0), out) || !plain64(e::u32(ctx, e::a1), in) || !inversInputs(env, in))
                return false;
            inversOn(env, ctx);
            return true;
        }

        // ---- capVu0Length 0x1F7DF8 (the game's own; a0 = v): f0 = |v.xyz| (v0 = vf4 whole) ----------------------
        constexpr uint32_t kCapVu0Length = 0x1F7DF8u;
        template <class Env>
        void lengthOn(Env &env, R5900Context *ctx)
        {
            const Vec4 v = e::read128(env, off16(e::u32(ctx, e::a0)));
            const LengthResult l = length(e::vf(ctx, 0), v, ctx->vu0_status);
            e::setVf(ctx, 4, l.vf4);
            ctx->vu0_q = l.q;
            ctx->vu0_status = l.status;
            std::memcpy(&ctx->r[e::v0], &ctx->vu0_vf[4], 16); // qmfc2 v0, vf4
            std::memcpy(&ctx->f[0], &ctx->r[e::v0], 4);       // mtc1 v0, f0
            ctx->pc = e::u32(ctx, e::ra);
        }
        template <class Env>
        bool lengthCheck(Env &env, const R5900Context *ctx)
        {
            uint32_t in;
            return e::vf0Constant(ctx) && plain16(e::u32(ctx, e::a0), in) && finiteXyz(e::read128(env, in));
        }
        template <class Env>
        bool capVu0Length(Env &env, R5900Context *ctx)
        {
            if (!lengthCheck(env, ctx))
                return false;
            lengthOn(env, ctx);
            return true;
        }

        // ---- the one-call wrappers: addiu sp, -16; sd ra; jal <leaf> (nop); ld ra; jr ra (addiu sp, 16) -----------
        // MtxCopy 0x147528 -> CopyMatrix, MtxMultiply 0x147548 -> MulMatrix (with its checkpoints), the game's thunk
        // 0x101E28 -> capVu0Length. The leaf runs inline after the guest call's 8 cycles.
        constexpr uint32_t kMtxCopy = 0x147528u, kMtxMultiply = 0x147548u, kLengthThunk = 0x101E28u;
        // The frame [sp - 16, sp): plain and clear of the leaf's data (offsets).
        bool wrapperFrame(const R5900Context *ctx, uint32_t &frame)
        {
            const uint32_t sp = e::u32(ctx, e::sp);
            return (sp & 0xFu) == 0u && plainRam(sp - 16u, 16u, frame);
        }
        template <class Env>
        void wrapperEnter(Env &env, R5900Context *ctx, uint32_t frame, uint32_t site, uint32_t leaf)
        {
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) - 16u);
            e::write64(env, frame, e::u64(ctx, e::ra));
            e::jal(ctx, site + 8u, leaf, false);
        }
        template <class Env>
        void wrapperLeave(Env &env, R5900Context *ctx, uint32_t frame, uint32_t site)
        {
            e::setLow64(ctx, e::ra, e::read64(env, frame));
            const uint32_t target = e::u32(ctx, e::ra);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) + 16u);
            e::jrRa(ctx, site + 0x14u, target, true);
        }

        template <class Env>
        bool mtxCopy(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            if (!wrapperFrame(ctx, frame) || !copyMatrixCheck(ctx))
                return false;
            const uint32_t in = off16(e::u32(ctx, e::a1));
            if (e::overlaps(frame, 16u, in, 64u)) // the saved ra would be in the copied matrix
                return false;
            wrapperEnter(env, ctx, frame, kMtxCopy, kCopyMatrix);
            if (env.call())
                return true;
            copyMatrixOn(env, ctx);
            wrapperLeave(env, ctx, frame, kMtxCopy);
            return true;
        }

        template <class Env>
        bool mtxMultiply(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            MulMatrixArgs m;
            if (!wrapperFrame(ctx, frame) || !mulMatrixCheck(env, ctx, m))
                return false;
            if (e::overlaps(frame, 16u, m.ma, 64u) || e::overlaps(frame, 16u, m.mb, 64u) || e::overlaps(frame, 16u, m.out, 64u))
                return false;
            wrapperEnter(env, ctx, frame, kMtxMultiply, kMulMatrix);
            if (env.call())
                return true;
            mulMatrixBody(env, ctx, m);
            if (ctx->pc != kMtxMultiply + 0x10u)
                return true; // MulMatrix stopped at a checkpoint (its resume entry carries on)
            wrapperLeave(env, ctx, frame, kMtxMultiply);
            return true;
        }

        template <class Env>
        bool lengthThunk(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            if (!wrapperFrame(ctx, frame) || !lengthCheck(env, ctx))
                return false;
            if (e::overlaps(frame, 16u, off16(e::u32(ctx, e::a0)), 16u))
                return false;
            wrapperEnter(env, ctx, frame, kLengthThunk, kCapVu0Length);
            if (env.call())
                return true;
            lengthOn(env, ctx);
            wrapperLeave(env, ctx, frame, kLengthThunk);
            return true;
        }

        // ---- sceVu0CameraMatrix 0x30A8C8 (a0 = out, a1 = position, a2 = z axis, a3 = y axis) -----------------------
        // m = Unit; m[0] = norm(y x z); m[2] = norm(z); m[1] = m[2] x m[0]; m[3].xyz += position; out = Invers(m).
        // A 0xB0 frame: s0-s4 saved (sq), ra (sd), the matrix at sp, a temporary at sp + 0x40.
        constexpr uint32_t kCameraMatrix = 0x30A8C8u;
        template <class Env>
        bool cameraMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp);
            uint32_t frame, out, pos, zAxis, yAxis;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - 0xB0u, 0xB0u, frame) || !e::vf0Constant(ctx) ||
                !plain64(e::u32(ctx, e::a0), out) || !plain16(e::u32(ctx, e::a1), pos) ||
                !plain16(e::u32(ctx, e::a2), zAxis) || !plain16(e::u32(ctx, e::a3), yAxis))
                return false;
            // The inputs are read after the frame is written: keep them out of it.
            if (e::overlaps(frame, 0xB0u, pos, 16u) || e::overlaps(frame, 0xB0u, zAxis, 16u) || e::overlaps(frame, 0xB0u, yAxis, 16u))
                return false;
            const Vec4 p = e::read128(env, pos), z = e::read128(env, zAxis), y = e::read128(env, yAxis);
            if (!finiteXyz(p) || !finiteXyz(z) || !finiteXyz(y))
                return false;
            const Vec4 c = outerProduct(y, z).r;
            if (!finiteXyz(c) || !ordinaryLength(squaresSummed(c, c)) || !ordinaryLength(squaresSummed(z, z)))
                return false;

            const uint32_t sp = sp0 - 0xB0u;
            e::setS32(ctx, e::sp, sp);
            e::sq(env, ctx, e::s4, frame + 0x60u);
            e::sq(env, ctx, e::s1, frame + 0x90u);
            e::move(ctx, e::s4, e::a0);
            e::sq(env, ctx, e::s2, frame + 0x80u);
            e::move(ctx, e::s1, e::a3);
            e::sq(env, ctx, e::s3, frame + 0x70u);
            e::move(ctx, e::a0, e::sp);
            e::sq(env, ctx, e::s0, frame + 0xA0u);
            e::move(ctx, e::s3, e::a1);
            e::write64(env, frame + 0x50u, e::u64(ctx, e::ra));
            e::jal(ctx, 0x30A8F4u, kUnitMatrix, true);
            e::move(ctx, e::s2, e::a2);
            if (env.call())
                return true;
            unitMatrixOn(env, ctx);

            e::setS32(ctx, e::s0, sp + 0x40u);
            e::move(ctx, e::a2, e::s2);
            e::move(ctx, e::a1, e::s1);
            e::jal(ctx, 0x30A908u, kOuterProduct, true);
            e::move(ctx, e::a0, e::s0);
            if (env.call())
                return true;
            outerProductOn(env, ctx);

            e::move(ctx, e::a0, e::sp);
            e::jal(ctx, 0x30A914u, kNormalize, true);
            e::move(ctx, e::a1, e::s0);
            if (env.call())
                return true;
            normalizeOn(env, ctx);

            e::setS32(ctx, e::s0, sp + 0x20u);
            e::move(ctx, e::a1, e::s2);
            e::jal(ctx, 0x30A924u, kNormalize, true);
            e::move(ctx, e::a0, e::s0);
            if (env.call())
                return true;
            normalizeOn(env, ctx);

            e::setS32(ctx, e::a0, sp + 0x10u);
            e::move(ctx, e::a2, e::sp);
            e::jal(ctx, 0x30A934u, kOuterProduct, true);
            e::move(ctx, e::a1, e::s0);
            if (env.call())
                return true;
            outerProductOn(env, ctx);

            e::move(ctx, e::a2, e::s3);
            e::move(ctx, e::a0, e::sp);
            e::jal(ctx, 0x30A944u, kTransMatrix, true);
            e::move(ctx, e::a1, e::sp);
            if (env.call())
                return true;
            transMatrixOn(env, ctx);

            e::move(ctx, e::a0, e::s4);
            e::jal(ctx, 0x30A950u, kInversMatrix, true);
            e::move(ctx, e::a1, e::sp);
            if (env.call())
                return true;
            inversOn(env, ctx);

            e::lq(env, ctx, e::s0, frame + 0xA0u);
            e::lq(env, ctx, e::s1, frame + 0x90u);
            e::lq(env, ctx, e::s2, frame + 0x80u);
            e::lq(env, ctx, e::s3, frame + 0x70u);
            e::lq(env, ctx, e::s4, frame + 0x60u);
            e::setLow64(ctx, e::ra, e::read64(env, frame + 0x50u));
            const uint32_t target = e::u32(ctx, e::ra);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) + 0xB0u);
            e::jrRa(ctx, 0x30A970u, target, true);
            return true;
        }

        // ---- sceVu0NormalLightMatrix 0x30A978 (a0 = out, a1 / a2 / a3 = the three light directions) ---------------
        // out[k] = norm(-light k) for k = 0..2, row 3 = (0, 0, 0, 1), then out transposed in place. A 0x60 frame:
        // f20 (swc1), s0-s2 (sq), ra (sd), the scaled vector at sp.
        constexpr uint32_t kNormalLightMatrix = 0x30A978u;
        // Its checks from the values it will be called with (sp, a0-a3; vf0 from ctx); the frame's offset out.
        template <class Env>
        bool normalLightCheck(Env &env, const R5900Context *ctx, uint32_t sp0, uint32_t a0, uint32_t a1, uint32_t a2,
                              uint32_t a3, uint32_t &frame, uint32_t l[3])
        {
            uint32_t out;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - 0x60u, 0x60u, frame) || !e::vf0Constant(ctx) || (a0 & 0xFu) != 0u ||
                !plain64(a0, out) || !plain16(a1, l[0]) || !plain16(a2, l[1]) || !plain16(a3, l[2]))
                return false;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t light = l[k];
                // read after the frame and the earlier rows are written: keep them clear of both
                if (e::overlaps(frame, 0x60u, light, 16u) || e::overlaps(out, 64u, light, 16u))
                    return false;
                const Vec4 v = e::read128(env, light);
                if (!finiteInputs(v) || !normalizeInput(v)) // -v: the same squares
                    return false;
            }
            return true;
        }
        template <class Env>
        bool normalLightBody(Env &env, R5900Context *ctx, uint32_t frame);
        template <class Env>
        bool normalLightMatrix(Env &env, R5900Context *ctx)
        {
            uint32_t frame, l[3];
            if (!normalLightCheck(env, ctx, e::u32(ctx, e::sp), e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2),
                                  e::u32(ctx, e::a3), frame, l))
                return false;
            normalLightBody(env, ctx, frame);
            return true;
        }
        // From its entry, the checks passed; false = stopped at a guest call (ctx left there).
        template <class Env>
        bool normalLightBody(Env &env, R5900Context *ctx, uint32_t frame)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp);
            const uint32_t sp = sp0 - 0x60u;
            e::setS32(ctx, e::sp, sp);
            e::write32(env, frame + 0x50u, floatBits(ctx->f[20]));
            e::sq(env, ctx, e::s0, frame + 0x40u);
            e::setS32(ctx, e::at, 0xBF800000u);
            std::memcpy(&ctx->f[20], &ctx->r[e::at], 4); // mtc1 at, f20 (-1.0)
            e::move(ctx, e::s0, e::a0);
            e::sq(env, ctx, e::s1, frame + 0x30u);
            e::move(ctx, e::a0, e::sp);
            e::sq(env, ctx, e::s2, frame + 0x20u);
            e::move(ctx, e::s1, e::a2);
            e::move(ctx, e::s2, e::a3);
            e::write64(env, frame + 0x10u, e::u64(ctx, e::ra));
            e::jal(ctx, 0x30A9A8u, kScaleVector, true);
            ctx->f[12] = ctx->f[20];
            if (env.call())
                return false;
            scaleVectorOn(env, ctx);

            e::move(ctx, e::a1, e::sp);
            e::jal(ctx, 0x30A9B4u, kNormalize, true);
            e::move(ctx, e::a0, e::s0);
            if (env.call())
                return false;
            normalizeOn(env, ctx);

            e::move(ctx, e::a0, e::sp);
            ctx->f[12] = ctx->f[20];
            e::jal(ctx, 0x30A9C4u, kScaleVector, true);
            e::move(ctx, e::a1, e::s1);
            if (env.call())
                return false;
            scaleVectorOn(env, ctx);

            e::move(ctx, e::a1, e::sp);
            e::jal(ctx, 0x30A9D0u, kNormalize, true);
            e::setS32(ctx, e::a0, e::u32(ctx, e::s0) + 0x10u);
            if (env.call())
                return false;
            normalizeOn(env, ctx);

            ctx->f[12] = ctx->f[20];
            e::move(ctx, e::a0, e::sp);
            e::jal(ctx, 0x30A9E0u, kScaleVector, true);
            e::move(ctx, e::a1, e::s2);
            if (env.call())
                return false;
            scaleVectorOn(env, ctx);

            e::setS32(ctx, e::a0, e::u32(ctx, e::s0) + 0x20u);
            e::jal(ctx, 0x30A9ECu, kNormalize, true);
            e::move(ctx, e::a1, e::sp);
            if (env.call())
                return false;
            normalizeOn(env, ctx);

            std::memset(&ctx->f[0], 0, 4); // mtc1 zero, f0
            e::move(ctx, e::a0, e::s0);
            e::setS32(ctx, e::at, 0x3F800000u);
            std::memcpy(&ctx->f[1], &ctx->r[e::at], 4);
            e::move(ctx, e::a1, e::a0);
            const uint32_t s0 = e::u32(ctx, e::s0);
            e::write32(env, (s0 + 0x30u) & PS2_RAM_MASK, floatBits(ctx->f[0]));
            e::write32(env, (s0 + 0x3Cu) & PS2_RAM_MASK, floatBits(ctx->f[1]));
            e::write32(env, (s0 + 0x38u) & PS2_RAM_MASK, floatBits(ctx->f[0]));
            e::jal(ctx, 0x30AA14u, kTransposeMatrix, true);
            e::write32(env, (s0 + 0x34u) & PS2_RAM_MASK, floatBits(ctx->f[0]));
            if (env.call())
                return false;
            transposeOn(env, ctx);

            e::lq(env, ctx, e::s0, frame + 0x40u);
            e::lq(env, ctx, e::s1, frame + 0x30u);
            e::lq(env, ctx, e::s2, frame + 0x20u);
            e::setLow64(ctx, e::ra, e::read64(env, frame + 0x10u));
            const uint32_t f20 = e::read32(env, frame + 0x50u);
            std::memcpy(&ctx->f[20], &f20, 4);
            const uint32_t target = e::u32(ctx, e::ra);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) + 0x60u);
            e::jrRa(ctx, 0x30AA30u, target, true);
            return true;
        }

        // ==== rung 3, step a, batch 2 (the batch-2 chat, Nov 21): the mid builders that call these helpers (switch
        // KAMITE_NATIVE_PACKET). Each body is its disassembly, one op:: helper per instruction (ee_ctx.h), its callees
        // run inline after their guest call's cycles; the entry checks predict every callee's inputs (intermediates by
        // the same core maths) and decline anything the callees would decline, or an overlap that would change them.
        using namespace e::op;
        bool plainSpan(uint32_t address, uint32_t size, uint32_t &offset) { return plainRam(address, size, offset); }
        bool vfBelow(Vec4 v, uint32_t limit) { return magnitudesBelow(v, limit); }

        // ---- func_001503B8 (a0 = model, a1 = out): out = MtxMultiply(model's matrix +0x80, a unit matrix with row 3's
        // xyz = the model's +0xC0..+0xC8). A 0x50 frame: the matrix at sp, ra at +0x40.
        constexpr uint32_t kModelMatrix = 0x1503B8u;
        template <class Env>
        bool modelMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), model = e::u32(ctx, e::a0);
            uint32_t frame, wrap, fields;
            if ((sp0 & 0xFu) != 0u || !plainSpan(sp0 - 0x50u, 0x50u, frame) || !plainSpan(sp0 - 0x60u, 16u, wrap) ||
                !plainSpan(model + 0xC0u, 12u, fields) || !e::vf0Constant(ctx) || e::overlaps(fields, 12u, frame, 0x50u))
                return false;
            // MulMatrix's arguments: out = a1, A = model + 0x80, B = the frame's matrix (rows: vf7, vf6, vf5, vf4 with
            // the model's three floats in row 3).
            MulMatrixArgs m;
            const uint32_t outAddr = e::u32(ctx, e::a1), maAddr = model + 0x80u;
            if (!plainRam(outAddr & ~0xFu, 64u, m.out) || !plainRam(maAddr & ~0xFu, 64u, m.ma))
                return false;
            m.mb = frame;
            m.a0 = outAddr;
            m.a2 = sp0 - 0x50u;
            if (addiOverflows(outAddr, 64u) || addiOverflows(m.a2, 64u) || overlapsUnequal(m.out, m.mb, 64u) ||
                e::overlaps(m.ma, 64u, frame, 0x50u) || e::overlaps(m.ma, 64u, wrap, 16u) ||
                e::overlaps(m.out, 64u, frame, 0x50u) || e::overlaps(m.out, 64u, wrap, 16u))
                return false;
            env.at(m.ma, 64u);
            env.at(m.out, 64u);
            const uint8_t *const ram = env.ram;
            for (uint32_t row = 0; row < 64u; row += 16u)
                if (!finiteInputs(load(ram + m.ma + row)))
                    return false;
            const uint32_t t0 = e::read32(env, fields), t1 = e::read32(env, fields + 4u), t2 = e::read32(env, fields + 8u);
            if (!finiteInputs(bitsOf(t0, t1, t2, 0u)))
                return false; // the unit rows are 0 / 1

            daddu(ctx, e::v0, e::a0, e::zero);
            addiu(ctx, e::sp, e::sp, -0x50);
            daddu(ctx, e::a0, e::a1, e::zero);
            sd(env, ctx, e::ra, e::sp, 0x40);
            const Vec4 v4 = e::vf(ctx, 0), v5 = rotateLanes(v4), v6 = rotateLanes(v5), v7 = rotateLanes(v6);
            e::setVf(ctx, 4, v4);
            e::setVf(ctx, 5, v5);
            e::setVf(ctx, 6, v6);
            e::setVf(ctx, 7, v7);
            sqc2(env, ctx, 4, e::sp, 0x30);
            sqc2(env, ctx, 5, e::sp, 0x20);
            sqc2(env, ctx, 6, e::sp, 0x10);
            sqc2(env, ctx, 7, e::sp, 0);
            lwc1(env, ctx, 2, e::v0, 0xC0);
            addiu(ctx, e::a1, e::v0, 0x80);
            lwc1(env, ctx, 1, e::v0, 0xC4);
            daddu(ctx, e::a2, e::sp, e::zero);
            lwc1(env, ctx, 0, e::v0, 0xC8);
            swc1(env, ctx, 2, e::sp, 0x30);
            swc1(env, ctx, 1, e::sp, 0x34);
            e::jal(ctx, 0x150404u, kMtxMultiply, true);
            swc1(env, ctx, 0, e::sp, 0x38);
            if (env.call())
                return true;
            wrapperEnter(env, ctx, wrap, kMtxMultiply, kMulMatrix);
            if (env.call())
                return true;
            mulMatrixBody(env, ctx, m);
            if (ctx->pc != kMtxMultiply + 0x10u)
                return true;
            wrapperLeave(env, ctx, wrap, kMtxMultiply);
            ld(env, ctx, e::ra, e::sp, 0x40);
            const uint32_t target = e::u32(ctx, e::ra);
            addiu(ctx, e::sp, e::sp, 0x50);
            e::jrRa(ctx, 0x150410u, target, true);
            return true;
        }

        // The four-float copy loops (lwc1 f0, 0(src); addiu n, -1; addiu src, 4; swc1 f0, 0(dst); bne n, end (addiu dst,
        // 4)) from n = 3 to -1: four words, three backward branches. false = stopped at a checkpoint.
        template <class Env>
        bool copyFour(Env &env, R5900Context *ctx, int src, int n, int dst, int end, uint32_t bneSite, uint32_t head)
        {
            for (;;)
            {
                lwc1(env, ctx, 0, src, 0);
                addiu(ctx, n, n, -1);
                addiu(ctx, src, src, 4);
                swc1(env, ctx, 0, dst, 0);
                const bool again = e::u64(ctx, n) != e::u64(ctx, end);
                addiu(ctx, dst, dst, 4);
                e::delaySlot(ctx, bneSite);
                if (!again)
                    return true;
                ctx->pc = head;
                if (env.checkpoint(32u))
                    return false;
            }
        }

        // ---- func_002D8008 (a0, a1, a2 = out, t0): a1 +0x3C = 1.0; out +0x30 = MulVector(a0 + 0x30, t0) (through the
        // frame). A 0x30 frame: the product at sp, ra at +0x10, s0 at +0x20. The native: lightColourReadable (R3).
        constexpr uint32_t kLightColour = 0x2D8008u;

        // ==== the REWRITE natives, R3 (Nov 23): func_001503B8 and func_002D8008 from the decomp's C (4607c68) ==========
        // kRenderPath | kChargeAtEnd (their only callers ignore v0: 0x14C7E8 for 0x1503B8; 0x153B00 / 0x154240 reload it
        // after 0x2D8008): the plan proves
        // every access plain and clear of the frames the original writes before it reads, then the writes.

        // void *func_001503B8(void *a0, void *a1) (src/cod/001503b8.c): buf = a unit matrix (vf0 rotated) with row 3's xyz
        // = a0 +0xC0..+0xC8; MtxMultiply(a1, a0 + 0x80, buf) = MulMatrix: a1 row k = (a0 + 0x80)'s rows by buf row k. Its
        // frame 0x50 (buf at sp), MtxMultiply's 0x10 below it. Calls: MtxMultiply, its MulMatrix; MulMatrix's 3 back-edges.
        template <class Env>
        bool modelMatrixReadable(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), model = e::u32(ctx, e::a0), outAddr = e::u32(ctx, e::a1);
            uint32_t frame, wrap, fields, out, ma;
            if ((sp0 & 0xFu) != 0u || !plainSpan(sp0 - 0x50u, 0x50u, frame) || !plainSpan(sp0 - 0x60u, 16u, wrap) ||
                !plainSpan(model + 0xC0u, 12u, fields) || !e::vf0Constant(ctx) || e::overlaps(fields, 12u, frame, 0x50u) ||
                !plainRam(outAddr & ~0xFu, 64u, out) || !plainRam((model + 0x80u) & ~0xFu, 64u, ma))
                return false;
            // MulMatrix's addi on a0 / a2 would trap past 2^31; A is read after both frames are written, out written
            // while buf (the frame) is still read
            if (addiOverflows(outAddr, 64u) || addiOverflows(sp0 - 0x50u, 64u) || e::overlaps(ma, 64u, frame, 0x50u) ||
                e::overlaps(ma, 64u, wrap, 16u) || e::overlaps(out, 64u, frame, 0x50u) || e::overlaps(out, 64u, wrap, 16u))
                return false;
            const uint8_t *const a = env.at(ma, 64u);
            const Vec4 a0 = load(a), a1 = load(a + 16), a2 = load(a + 32), a3 = load(a + 48);
            const uint32_t x = e::read32(env, fields), y = e::read32(env, fields + 4u), z = e::read32(env, fields + 8u);
            if (!finiteInputs(a0) || !finiteInputs(a1) || !finiteInputs(a2) || !finiteInputs(a3) ||
                !finiteInputs(bitsOf(x, y, z, 0u)))
                return false; // the unit rows are 0 / 1
            const Vec4 v4 = e::vf(ctx, 0), v5 = rotateLanes(v4), v6 = rotateLanes(v5), v7 = rotateLanes(v6);
            const Vec4 buf[4] = {v7, v6, v5, bitsOf(x, y, z, 0x3F800000u)}; // vf0.w = 1 (vf0Constant)
            Vec4 rows[4];
            for (int k = 0; k < 4; ++k)
                rows[k] = mulRow(a0, a1, a2, a3, buf[k]).r;
            env.call(); // jal MtxMultiply
            env.call(); // its jal sceVu0MulMatrix
            for (int k = 0; k < 3; ++k)
                env.checkpoint(32u); // MulMatrix's back-edges
            uint8_t *const dst = env.at(out, 64u);
            for (int k = 0; k < 4; ++k)
                store(dst + 16 * k, rows[k]);
            ctx->pc = gpr32(ctx, 31);
            ctx->in_delay_slot = false;
            return true;
        }

        // void func_002D8008(void *a0, float *a1, float *a2, int a3, float *a4) (src/cod/00156488.c): a1[15] = 1.0f;
        // tmp = sceVu0MulVector(a0 + 0x30, a4) (a4 = t0); a2 + 0x30 = tmp, word by word (3 back-edges). A 0x30 frame (tmp
        // at sp).
        template <class Env>
        bool lightColourReadable(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp);
            const uint32_t aAddr = e::u32(ctx, e::a0) + 0x30u, bAddr = e::u32(ctx, e::t0);
            uint32_t frame, one, va, vb, dst;
            if ((sp0 & 0xFu) != 0u || !plainSpan(sp0 - 0x30u, 0x30u, frame) || !plainSpan(e::u32(ctx, e::a1) + 0x3Cu, 4u, one) ||
                !plain16(aAddr, va) || !plain16(bAddr, vb) || !plainSpan(e::u32(ctx, e::a2) + 0x30u, 16u, dst))
                return false;
            // MulVector reads after the frame's stores and the 1.0; the copy reads tmp while writing dst
            if (e::overlaps(va, 16u, frame, 0x30u) || e::overlaps(vb, 16u, frame, 0x30u) || e::overlaps(va, 16u, one, 4u) ||
                e::overlaps(vb, 16u, one, 4u) || e::overlaps(dst, 16u, frame, 0x30u) ||
                !mulVectorCheck(env, sp0 - 0x30u, aAddr, bAddr))
                return false;
            e::write32(env, one, 0x3F800000u);
            alignas(16) uint8_t product[16];
            store(product, mul(e::read128(env, va), e::read128(env, vb)));
            env.call(); // jal sceVu0MulVector
            for (int k = 0; k < 3; ++k)
                env.checkpoint(32u); // the copy's back-edges
            for (uint32_t i = 0; i < 16u; i += 4u)
            {
                uint32_t word;
                std::memcpy(&word, product + i, 4);
                e::write32(env, dst + i, word);
            }
            ctx->pc = gpr32(ctx, 31);
            ctx->in_delay_slot = false;
            return true;
        }

        // ---- func_002D7E50 (a0 = light set, a2 = out, a3 = colours, t1 = ambient): out = NormalLightMatrix(the set's
        // three directions +0x161A0 / B0 / C0), out row 3 = the ambient; colours rows 0-2 = the set's +0x161D0 / E0 / F0,
        // row 3 = +0x40 (xyz * 1.0); every lane clamped to 0..1 (VMAX vf0.x, VMINI vf0.w). A 0x50 frame: ra at sp,
        // s3 / s2 / s1 / s0 at +0x10 / 20 / 30 / 40.
        constexpr uint32_t kLightMatrix = 0x2D7E50u;
        template <class Env>
        bool lightMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), set = e::u32(ctx, e::a0), out = e::u32(ctx, e::a2);
            const uint32_t colours = e::u32(ctx, e::a3), ambient = e::u32(ctx, e::t1);
            uint32_t frame, nlFrame, l[3], src[3], amb, row3, dst3, cols, outRows;
            if ((sp0 & 0xFu) != 0u || (colours & 0xFu) != 0u || !plainSpan(sp0 - 0x50u, 0x50u, frame) ||
                !plainSpan(set + 0x161D0u, 0x30u, src[0]) || !plainSpan(set + 0x40u, 16u, row3) ||
                !plainSpan(ambient, 16u, amb) || !plainSpan(out + 0x30u, 16u, dst3) || !plainSpan(colours, 0x40u, cols) ||
                !plainSpan(out, 64u, outRows))
                return false;
            if (!normalLightCheck(env, ctx, sp0 - 0x50u, out, set + 0x161A0u, set + 0x161B0u, set + 0x161C0u, nlFrame, l))
                return false;
            for (uint32_t light : l)
                if (e::overlaps(light, 16u, frame, 0x50u))
                    return false;
            // row 3's lanes go through VMUL x 1.0: the host's product unless at / past FLT_MAX (the recompiled
            // function's ps2ee). Read them as they will be (clear of every write before the lqc2).
            if (e::overlaps(row3, 16u, frame, 0x50u) || e::overlaps(row3, 16u, nlFrame, 0x60u) ||
                e::overlaps(row3, 16u, outRows, 64u) || e::overlaps(row3, 16u, cols, 0x40u))
                return false;
            if (!vfBelow(blend<8>(e::read128(env, row3), splat(0.0f)), 0x7F7FFFFFu))
                return false;
            (void)src;

            addiu(ctx, e::sp, e::sp, -0x50);
            lui(ctx, e::a1, 0x1);
            sqOp(env, ctx, e::s0, e::sp, 0x40);
            lui(ctx, e::v0, 0x1);
            sqOp(env, ctx, e::s2, e::sp, 0x20);
            lui(ctx, e::v1, 0x1);
            sqOp(env, ctx, e::s3, e::sp, 0x10);
            daddu(ctx, e::s2, e::a0, e::zero);
            daddu(ctx, e::s0, e::a2, e::zero);
            ori(ctx, e::v0, e::v0, 0x61B0);
            ori(ctx, e::v1, e::v1, 0x61C0);
            sqOp(env, ctx, e::s1, e::sp, 0x30);
            ori(ctx, e::a1, e::a1, 0x61A0);
            daddu(ctx, e::s3, e::a3, e::zero);
            addu(ctx, e::a2, e::s2, e::v0);
            addu(ctx, e::a3, e::s2, e::v1);
            daddu(ctx, e::a0, e::s0, e::zero);
            sd(env, ctx, e::ra, e::sp, 0);
            daddu(ctx, e::s1, e::t1, e::zero);
            e::jal(ctx, 0x2D7E9Cu, kNormalLightMatrix, true);
            addu(ctx, e::a1, e::s2, e::a1);
            if (env.call())
                return true;
            if (!normalLightBody(env, ctx, nlFrame))
                return true;
            addiu(ctx, e::v1, e::zero, -1);
            addiu(ctx, e::s0, e::s0, 0x30);
            addiu(ctx, e::v0, e::zero, 0x3);
            if (!copyFour(env, ctx, e::s1, e::v0, e::s0, e::v1, 0x2D7EC0u, 0x2D7EB0u))
                return true;
            lui(ctx, e::v0, 0x1);
            daddu(ctx, e::v1, e::s3, e::zero);
            ori(ctx, e::v0, e::v0, 0x61D0);
            addiu(ctx, e::a0, e::zero, 0x3);
            addu(ctx, e::v0, e::s2, e::v0);
            addiu(ctx, e::a2, e::s3, 0x10);
            addiu(ctx, e::a3, e::s3, 0x20);
            addiu(ctx, e::t0, e::s3, 0x30);
            addiu(ctx, e::t1, e::s2, 0x40);
            addiu(ctx, e::a1, e::zero, -1);
            if (!copyFour(env, ctx, e::v0, e::a0, e::v1, e::a1, 0x2D7F00u, 0x2D7EF0u))
                return true;
            lui(ctx, e::v0, 0x1);
            daddu(ctx, e::v1, e::a2, e::zero);
            ori(ctx, e::v0, e::v0, 0x61E0);
            addiu(ctx, e::a0, e::zero, 0x3);
            addu(ctx, e::v0, e::s2, e::v0);
            addiu(ctx, e::a1, e::zero, -1);
            if (!copyFour(env, ctx, e::v0, e::a0, e::v1, e::a1, 0x2D7F30u, 0x2D7F20u))
                return true;
            lui(ctx, e::v0, 0x1);
            daddu(ctx, e::v1, e::a3, e::zero);
            ori(ctx, e::v0, e::v0, 0x61F0);
            addiu(ctx, e::a1, e::zero, 0x3);
            addu(ctx, e::a0, e::s2, e::v0);
            addiu(ctx, e::v0, e::zero, -1);
            if (!copyFour(env, ctx, e::a0, e::a1, e::v1, e::v0, 0x2D7F60u, 0x2D7F50u))
                return true;
            daddu(ctx, e::v1, e::t0, e::zero);
            daddu(ctx, e::v0, e::t1, e::zero);
            addiu(ctx, e::a0, e::zero, 0x3);
            addiu(ctx, e::a1, e::zero, -1);
            if (!copyFour(env, ctx, e::v0, e::a0, e::v1, e::a1, 0x2D7F88u, 0x2D7F78u))
                return true;
            lui(ctx, e::at, 0x3F80);
            e::mtc1(ctx, e::at, 0);
            e::mfc1(ctx, e::t0, 0);
            std::memcpy(&ctx->vu0_vf[1], &ctx->r[e::t0], 16); // qmtc2.ni: the whole register
            lqc2(env, ctx, 5, e::s3, 0x30);
            lqc2(env, ctx, 2, e::s3, 0);
            lqc2(env, ctx, 3, e::s3, 0x10);
            lqc2(env, ctx, 4, e::s3, 0x20);
            e::setVf(ctx, 5, blend<7>(e::vf(ctx, 5), mul(e::vf(ctx, 5), splat<0>(e::vf(ctx, 1)))));
            const Vec4 lo = splat<0>(e::vf(ctx, 0)), hi = splat<3>(e::vf(ctx, 0));
            for (int r = 2; r <= 5; ++r)
                e::setVf(ctx, r, vuMax(e::vf(ctx, r), lo));
            for (int r = 2; r <= 5; ++r)
                e::setVf(ctx, r, vuMin(e::vf(ctx, r), hi));
            sqc2(env, ctx, 2, e::s3, 0);
            sqc2(env, ctx, 3, e::s3, 0x10);
            sqc2(env, ctx, 4, e::s3, 0x20);
            sqc2(env, ctx, 5, e::s3, 0x30);
            lqOp(env, ctx, e::s0, e::sp, 0x40);
            lqOp(env, ctx, e::s1, e::sp, 0x30);
            lqOp(env, ctx, e::s2, e::sp, 0x20);
            lqOp(env, ctx, e::s3, e::sp, 0x10);
            ld(env, ctx, e::ra, e::sp, 0);
            const uint32_t target = e::u32(ctx, e::ra);
            addiu(ctx, e::sp, e::sp, 0x50);
            e::jrRa(ctx, 0x2D7FFCu, target, true);
            return true;
        }

        // ---- func_001F8DF8 (a0 = box: centre +0, half sizes +0xC.., a1 = matrix, f12 = threshold): the centre and the
        // eight corners (box matrix, then + the centre's xyz, then the matrix at *0x5CAFF0); v0 = 1 unless the smallest
        // corner w is below f12 by the compare key. A 0x140 frame: the centre row at sp, its transform at +0x10 / +0x20,
        // the matrix copy at +0x30, the corners at +0x70..; ra +0xF0, s2 / s1 / s0 +0x100 / 110 / 120, f20 / f21 +0x130 /
        // 138. Each guest call (ApplyMatrix 17 times, MtxCopy) and the corner loop's branch can stop it.
        constexpr uint32_t kBoxTest = 0x1F8DF8u, kViewMatrix = 0x5CAFF0u;
        constexpr uint32_t kOne = 0x3F800000u;
        template <class Env>
        bool boxTest(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), box = e::u32(ctx, e::a0), mAddr = e::u32(ctx, e::a1);
            uint32_t frame, copyFrame, boxAt, viewAt, ma, view;
            if ((sp0 & 0xFu) != 0u || !plainSpan(sp0 - 0x140u, 0x140u, frame) || !plainSpan(sp0 - 0x150u, 16u, copyFrame) ||
                !plainSpan(box, 0x18u, boxAt) || !plainSpan(kViewMatrix, 4u, viewAt) || !plain64(mAddr, ma))
                return false;
            const uint32_t viewAddr = e::read32(env, viewAt);
            if (!plain64(viewAddr, view))
                return false;
            // Everything read after the frame's writes stays clear of them (the frame, MtxCopy's frame).
            const uint32_t reads[4][2] = {{boxAt, 0x18u}, {viewAt, 4u}, {ma, 64u}, {view, 64u}};
            for (const auto &r : reads)
                if (e::overlaps(r[0], r[1], frame, 0x140u) || e::overlaps(r[0], r[1], copyFrame, 16u))
                    return false;
            env.at(ma, 64u);
            env.at(view, 64u);
            const uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            const Vec4 g0 = load(ram + view), g1 = load(ram + view + 16), g2 = load(ram + view + 32), g3 = load(ram + view + 48);
            if (!finiteInputs(m0) || !finiteInputs(m1) || !finiteInputs(m2) || !finiteInputs(m3) || !finiteInputs(g0) ||
                !finiteInputs(g1) || !finiteInputs(g2) || !finiteInputs(g3))
                return false;
            // The maths ahead, as it will run: the centre, each corner's two transforms and the add between them.
            const uint32_t cx = e::read32(env, boxAt), cy = e::read32(env, boxAt + 4u), cz = e::read32(env, boxAt + 8u);
            const uint32_t hx = e::read32(env, boxAt + 0xCu), hy = e::read32(env, boxAt + 0x10u), hz = e::read32(env, boxAt + 0x14u);
            const Vec4 centre = bitsOf(cx, cy, cz, kOne);
            if (!finiteInputs(centre) || !finiteInputs(bitsOf(hx, hy, hz, kOne)))
                return false;
            const Vec4 c = mulRow(m0, m1, m2, m3, centre).r;
            const Vec4 shift = blend<8>(c, splat(1.0f)); // the centre row at sp: its xyz, w still 1.0
            for (int k = 0; k < 8; ++k)
            {
                const uint32_t sx = (k & 1) ? hx : hx ^ 0x80000000u, sy = (k & 4) ? hy ^ 0x80000000u : hy,
                               sz = (k & 2) ? hz : hz ^ 0x80000000u;
                const Vec4 p = mulRow(m0, m1, m2, m3, bitsOf(sx, sy, sz, kOne)).r;
                const Vec4 q = blend<7>(p, add(p, shift));
                if (!finiteInputs(q))
                    return false; // the second transform's input (and no sum past FLT_MAX)
            }

            addiu(ctx, e::sp, e::sp, -0x140);
            sqOp(env, ctx, e::s0, e::sp, 0x120);
            sqOp(env, ctx, e::s1, e::sp, 0x110);
            addiu(ctx, e::s0, e::sp, 0x20);
            sqOp(env, ctx, e::s2, e::sp, 0x100);
            daddu(ctx, e::s1, e::a0, e::zero);
            swc1(env, ctx, 21, e::sp, 0x138);
            daddu(ctx, e::s2, e::a1, e::zero);
            swc1(env, ctx, 20, e::sp, 0x130);
            e::setFBits(ctx, 21, e::fBits(ctx, 12)); // mov.s
            sd(env, ctx, e::ra, e::sp, 0xF0);
            lui(ctx, e::at, 0x3F80);
            e::mtc1(ctx, e::at, 20);
            lwc1(env, ctx, 2, e::s1, 0);
            lwc1(env, ctx, 1, e::s1, 4);
            lwc1(env, ctx, 0, e::s1, 8);
            swc1(env, ctx, 2, e::sp, 0);
            swc1(env, ctx, 1, e::sp, 4);
            swc1(env, ctx, 0, e::sp, 8);
            swc1(env, ctx, 20, e::sp, 0xC);
            lqc2(env, ctx, 4, e::sp, 0);
            sqc2(env, ctx, 4, e::sp, 0x20);
            daddu(ctx, e::a0, e::s0, e::zero);
            daddu(ctx, e::a1, e::s2, e::zero);
            e::jal(ctx, 0x1F8E58u, kApplyMatrix, true);
            daddu(ctx, e::a2, e::s0, e::zero);
            if (env.call())
                return true;
            uint32_t out, mat, vec;
            applyCheck(env, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2), out, mat, vec); // offsets
            applyOn(env, ctx, out, mat, vec);
            lqc2(env, ctx, 4, e::s0, 0);
            sqc2(env, ctx, 4, e::sp, 0x10);
            addiu(ctx, e::v0, e::sp, 0x10);
            lwc1(env, ctx, 0, e::sp, 0x10); // beq sp, v0's delay slot (never taken)
            e::delaySlot(ctx, 0x1F8E6Cu);
            lwc1(env, ctx, 1, e::sp, 0x14);
            lwc1(env, ctx, 2, e::sp, 0x18);
            swc1(env, ctx, 0, e::sp, 0);
            swc1(env, ctx, 1, e::sp, 4);
            swc1(env, ctx, 2, e::sp, 8);
            daddu(ctx, e::a1, e::s2, e::zero);
            e::jal(ctx, 0x1F8E8Cu, kMtxCopy, true);
            addiu(ctx, e::a0, e::sp, 0x30);
            if (env.call())
                return true;
            wrapperEnter(env, ctx, copyFrame, kMtxCopy, kCopyMatrix);
            if (env.call())
                return true;
            copyMatrixOn(env, ctx);
            wrapperLeave(env, ctx, copyFrame, kMtxCopy);
            daddu(ctx, e::s2, e::zero, e::zero);
            lwc1(env, ctx, 5, e::s1, 0xC);
            addiu(ctx, e::v0, e::sp, 0x70);
            lwc1(env, ctx, 6, e::s1, 0x14);
            addiu(ctx, e::v1, e::sp, 0x80);
            e::setFBits(ctx, 3, e::fBits(ctx, 5) ^ 0x80000000u); // neg.s
            lwc1(env, ctx, 1, e::s1, 0x10);
            e::setFBits(ctx, 2, e::fBits(ctx, 6) ^ 0x80000000u);
            daddu(ctx, e::s0, e::v0, e::zero);
            e::mtc1(ctx, e::zero, 0);
            lui(ctx, e::s1, 0x5D);
            swc1(env, ctx, 3, e::sp, 0x70);
            e::setFBits(ctx, 4, e::fBits(ctx, 1) ^ 0x80000000u);
            swc1(env, ctx, 0, e::sp, 0x60);
            swc1(env, ctx, 0, e::sp, 0x68);
            swc1(env, ctx, 0, e::sp, 0x64);
            swc1(env, ctx, 1, e::v0, 4);
            swc1(env, ctx, 2, e::v0, 8);
            swc1(env, ctx, 20, e::v0, 0xC);
            swc1(env, ctx, 5, e::sp, 0x80);
            swc1(env, ctx, 1, e::v1, 4);
            swc1(env, ctx, 2, e::v1, 8);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 3, e::sp, 0x90);
            addiu(ctx, e::v1, e::sp, 0x90);
            swc1(env, ctx, 1, e::v1, 4);
            swc1(env, ctx, 6, e::v1, 8);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 5, e::sp, 0xA0);
            addiu(ctx, e::v1, e::sp, 0xA0);
            swc1(env, ctx, 1, e::v1, 4);
            swc1(env, ctx, 6, e::v1, 8);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 3, e::sp, 0xB0);
            addiu(ctx, e::v1, e::sp, 0xB0);
            swc1(env, ctx, 4, e::v1, 4);
            swc1(env, ctx, 2, e::v1, 8);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 5, e::sp, 0xC0);
            addiu(ctx, e::v1, e::sp, 0xC0);
            swc1(env, ctx, 2, e::v1, 8);
            swc1(env, ctx, 4, e::v1, 4);
            swc1(env, ctx, 20, e::v1, 0xC);
            addiu(ctx, e::v1, e::sp, 0xD0);
            swc1(env, ctx, 3, e::sp, 0xD0);
            swc1(env, ctx, 4, e::v1, 4);
            swc1(env, ctx, 6, e::v1, 8);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 5, e::sp, 0xE0);
            addiu(ctx, e::v1, e::sp, 0xE0);
            swc1(env, ctx, 20, e::v1, 0xC);
            swc1(env, ctx, 4, e::v1, 4);
            swc1(env, ctx, 6, e::v1, 8);
            for (;;)
            {
                daddu(ctx, e::a0, e::s0, e::zero);
                addiu(ctx, e::a1, e::sp, 0x30);
                e::jal(ctx, 0x1F8F70u, kApplyMatrix, true);
                daddu(ctx, e::a2, e::s0, e::zero);
                if (env.call())
                    return true;
                applyCheck(env, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2), out, mat, vec);
                applyOn(env, ctx, out, mat, vec);
                lqc2(env, ctx, 4, e::s0, 0);
                lqc2(env, ctx, 5, e::sp, 0);
                e::setVf(ctx, 4, blend<7>(e::vf(ctx, 4), add(e::vf(ctx, 4), e::vf(ctx, 5))));
                sqc2(env, ctx, 4, e::s0, 0);
                lw(env, ctx, e::a1, e::s1, -0x5010);
                daddu(ctx, e::a0, e::s0, e::zero);
                daddu(ctx, e::a2, e::s0, e::zero);
                e::jal(ctx, 0x1F8F94u, kApplyMatrix, true);
                addiu(ctx, e::s2, e::s2, 1);
                if (env.call())
                    return true;
                applyCheck(env, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2), out, mat, vec);
                applyOn(env, ctx, out, mat, vec);
                slti(ctx, e::v0, e::s2, 0x8);
                const bool again = e::u64(ctx, e::v0) != 0u;
                addiu(ctx, e::s0, e::s0, 0x10);
                e::delaySlot(ctx, 0x1F8FA0u);
                if (!again)
                    break;
                ctx->pc = 0x1F8F68u;
                if (env.checkpoint(32u))
                    return true;
            }
            // the smallest w by the compare key (c.lt.s f1, f0; bc1tl mov.s f0, f1)
            lwc1(env, ctx, 0, e::sp, 0x7C);
            const uint32_t minSites[7] = {0x1F8FB8u, 0x1F8FCCu, 0x1F8FE0u, 0x1F8FF4u, 0x1F9008u, 0x1F901Cu, 0x1F9030u};
            for (int k = 0; k < 7; ++k)
            {
                lwc1(env, ctx, 1, e::sp, 0x8C + 0x10 * k);
                e::setC(ctx, e::cLt(ctx, 1, 0));
                if (e::cBit(ctx))
                {
                    e::setFBits(ctx, 0, e::fBits(ctx, 1));
                    e::delaySlot(ctx, minSites[k]);
                }
            }
            e::setC(ctx, e::cLt(ctx, 21, 0));
            daddu(ctx, e::v0, e::zero, e::zero); // bc1t's delay slot
            e::delaySlot(ctx, 0x1F9040u);
            if (!e::cBit(ctx))
                addiu(ctx, e::v0, e::zero, 0x1);
            lqOp(env, ctx, e::s0, e::sp, 0x120);
            lqOp(env, ctx, e::s1, e::sp, 0x110);
            lqOp(env, ctx, e::s2, e::sp, 0x100);
            ld(env, ctx, e::ra, e::sp, 0xF0);
            lwc1(env, ctx, 21, e::sp, 0x138);
            lwc1(env, ctx, 20, e::sp, 0x130);
            const uint32_t target = e::u32(ctx, e::ra);
            addiu(ctx, e::sp, e::sp, 0x140);
            e::jrRa(ctx, 0x1F9064u, target, true);
            return true;
        }
    }

    void registerFunctions()
    {
        // All checked (on by default; claude/native-next.md "Rung 2 as built"): 0 mismatches and 0 declines in K's
        // shadowed replays (MulMatrix / ApplyMatrix ~75M calls on f733c30; ecossin 31.8M and the RotMatrix trio 31.8M on
        // 21f25ea, both rates), every .ram.bin identical, the first two measured ~3x cheaper in the sampler.
        registerFunction(NativeFunction{"sceVu0MulMatrix", kMulMatrix, "libvu0", true, &mulMatrix<RealEnv>,
                                        &mulMatrix<ShadowEnv>, &mulMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0ApplyMatrix", kApplyMatrix, "libvu0", true, &applyMatrix<RealEnv>,
                                        &applyMatrix<ShadowEnv>, &applyMatrixFootprint});
        registerFunction(NativeFunction{"_sceVu0ecossin", kEcossin, "libvu0", true, &ecossinNative<RealEnv>,
                                        &ecossinNative<ShadowEnv>, &ecossinFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixX", kRot[0].entry, "libvu0", true, &rotMatrix<0, RealEnv>,
                                        &rotMatrix<0, ShadowEnv>, &rotMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixY", kRot[1].entry, "libvu0", true, &rotMatrix<1, RealEnv>,
                                        &rotMatrix<1, ShadowEnv>, &rotMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixZ", kRot[2].entry, "libvu0", true, &rotMatrix<2, RealEnv>,
                                        &rotMatrix<2, ShadowEnv>, &rotMatrixFootprint});

        // Rung 3, step a, batch 1 (claude/native-next.md): checked (on by default) since K's 16:44 build of e881eeb
        // shadowed them in every replay: ~155M calls, 0 mismatches, 0 skipped, every .ram.bin the reference's. Recorded
        // footprints.
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *)) {
            registerFunction(NativeFunction{name, address, "libvu0", true, real, shadow, nullptr});
        };
        add("sceVu0CopyMatrix", kCopyMatrix, &copyMatrix<RealEnv>, &copyMatrix<ShadowEnv>);
        add("sceVu0UnitMatrix", kUnitMatrix, &unitMatrix<RealEnv>, &unitMatrix<ShadowEnv>);
        add("sceVu0MulVector", kMulVector, &mulVector<RealEnv>, &mulVector<ShadowEnv>);
        add("sceVu0ScaleVector", kScaleVector, &scaleVector<RealEnv>, &scaleVector<ShadowEnv>);
        add("sceVu0OuterProduct", kOuterProduct, &outerProductNative<RealEnv>, &outerProductNative<ShadowEnv>);
        add("sceVu0Normalize", kNormalize, &normalizeNative<RealEnv>, &normalizeNative<ShadowEnv>);
        add("sceVu0TransMatrix", kTransMatrix, &transMatrix<RealEnv>, &transMatrix<ShadowEnv>);
        add("sceVu0TransposeMatrix", kTransposeMatrix, &transposeMatrix<RealEnv>, &transposeMatrix<ShadowEnv>);
        add("sceVu0InversMatrix", kInversMatrix, &inversMatrix<RealEnv>, &inversMatrix<ShadowEnv>);
        add("sceVu0CameraMatrix", kCameraMatrix, &cameraMatrix<RealEnv>, &cameraMatrix<ShadowEnv>);
        add("sceVu0NormalLightMatrix", kNormalLightMatrix, &normalLightMatrix<RealEnv>, &normalLightMatrix<ShadowEnv>);
        add("capVu0Length", kCapVu0Length, &capVu0Length<RealEnv>, &capVu0Length<ShadowEnv>);
        add("Tramp_001F7DF8_00101E28", kLengthThunk, &lengthThunk<RealEnv>, &lengthThunk<ShadowEnv>);
        add("MtxCopy", kMtxCopy, &mtxCopy<RealEnv>, &mtxCopy<ShadowEnv>);
        add("MtxMultiply", kMtxMultiply, &mtxMultiply<RealEnv>, &mtxMultiply<ShadowEnv>);

        // Rung 3, step a, batch 2: the mid builders calling these (switch KAMITE_NATIVE_PACKET), checked since K's
        // 21:42 build of 99e32b8 (0 mismatches in every replay).
        auto addPacket = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                            bool (*shadow)(ShadowEnv &, R5900Context *)) {
            registerFunction(NativeFunction{name, address, "packet", true, real, shadow, nullptr});
        };
        addPacket("func_001F8DF8", kBoxTest, &boxTest<RealEnv>, &boxTest<ShadowEnv>);
        addPacket("func_002D7E50", kLightMatrix, &lightMatrix<RealEnv>, &lightMatrix<ShadowEnv>);
        // The REWRITE natives, R3 (Nov 23): rewritten from the decomp's C, kRenderPath | kChargeAtEnd (the asm versions
        // above were checked). 0x2D8008 checked on K's 20:57 build of 7fbf4c9 (1.08M calls shadowed, 0 mismatches, 0
        // declines); 0x1503B8 had no standalone call (0x14C7E8's native runs it inline): stays shadowed.
        auto addReadable = [](const char *name, uint32_t address, bool checked, bool (*real)(RealEnv &, R5900Context *),
                              bool (*shadow)(ShadowEnv &, R5900Context *)) {
            NativeFunction fn{name, address, "packet", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = 0u; // void for their callers (see R3 above)
            registerFunction(fn);
        };
        addReadable("func_001503B8", kModelMatrix, false, &modelMatrixReadable<RealEnv>, &modelMatrixReadable<ShadowEnv>);
        addReadable("func_002D8008", kLightColour, true, &lightColourReadable<RealEnv>, &lightColourReadable<ShadowEnv>);
    }
}
