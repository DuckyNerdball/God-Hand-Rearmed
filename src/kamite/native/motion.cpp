// Native motion functions (rung 4, claude/rung4-motion-map.md): the bone hierarchy first (M1). Switch:
// KAMITE_NATIVE_MOTION (unset = the checked ones, 1 = all, 0 = none); a native that is not checked yet runs in the
// shadow harness during dev replays only.
//
// Every native here is kRenderPath | kChargeAtEnd (K, Nov 25, rung 4 round 2: answers 8 and 9): the harness compares
// what lasts (RAM outside the dead stack, the callee-saved registers, pc, in_delay_slot, the EE cycles); temporaries are
// don't-care. Allowed per batch by the caller scan (tools/native/caller_scan.py over the ELF and the 81 room DLLs: no
// caller reads a scratch register after any of them). All void for their callers: returns = 0. Each reads and checks
// everything first and computes the result, then writes it (a decline changes nothing), in the original's order where
// an argument may alias what it writes.

#include "kamite/native/native.h"
#include "kamite/native/motion_core.h"
#include "kamite/native/ee_ctx.h"

namespace kamite::native::motion
{
    namespace
    {
        namespace e = kamite::native::ee;

        constexpr uint32_t kOne = 0x3F800000u;
        constexpr uint32_t kCoefficients = 0x3C3C90u; // _sceVu0ecossin's polynomial (the ELF's .data)

        // ADDI's overflow trap (the libvu0 loops step their pointers with addi): never in plain RAM; declined.
        bool addiOverflows(uint32_t base, uint32_t step)
        {
            const int64_t sum = static_cast<int64_t>(static_cast<int32_t>(base)) + static_cast<int32_t>(step);
            return sum != static_cast<int32_t>(static_cast<uint32_t>(sum));
        }

        template <class Env>
        Mat4 readMat(Env &env, uint32_t offset)
        {
            const uint8_t *p = env.at(offset, 64u);
            return Mat4{{load(p), load(p + 16), load(p + 32), load(p + 48)}};
        }
        template <class Env>
        void writeMat(Env &env, uint32_t offset, const Mat4 &m)
        {
            uint8_t *p = env.at(offset, 64u);
            for (int k = 0; k < 4; ++k)
                store(p + 16 * k, m.r[k]);
        }
        // jr ra with a real delay slot (the generated code clears in_delay_slot after it)
        void returnFrom(R5900Context *ctx)
        {
            ctx->pc = e::u32(ctx, e::ra);
            ctx->in_delay_slot = false;
        }

        // ---- Adjust_theta_vec 0x1F7DA0 (a0 = an Euler vector, in place; asm, jr ra + nop) ------------------------------
        constexpr uint32_t kAdjustTheta = 0x1F7DA0u, kThetaTable = 0x3BF110u;
        template <class Env>
        bool adjustThetaVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            uint32_t v, table;
            if (!plainRam(e::u32(ctx, e::a0) & ~0xFu, 16u, v) || !plainRam(kThetaTable, 48u, table))
                return false;
            const Vec4 x = e::read128(env, v);
            const uint8_t *t = env.at(table, 48u);
            ThetaTable tt;
            tt.c = load(t);
            std::memcpy(tt.one, t + 16, 16);
            std::memcpy(tt.mask, t + 32, 16);
            if (!thetaInputs(x, tt))
                return false;
            e::write128(env, v, adjustTheta(x, tt));
            ctx->pc = e::u32(ctx, e::ra); // a nop delay slot: in_delay_slot as it was
            return true;
        }

        // ---- MtxInitTransVec 0x1476D0 (a0 = out, a1 = a translation; src/cod/0013c930.c) -------------------------------
        // The unit rows' first three by word, then out[12..14] = a1[0..2] word by word (out[15] = 1 between the last
        // read and its store): an a1 inside out reads what the original has written by then.
        template <class Env>
        bool initTransVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1);
            uint32_t out, src;
            if (((a0 | a1) & 3u) != 0u || !plainRam(a0, 64u, out) || !plainRam(a1, 12u, src))
                return false;
            for (uint32_t k = 0; k < 12u; ++k)
                e::write32(env, out + 4u * k, k % 5u == 0u ? kOne : 0u);
            e::write32(env, out + 0x30u, e::read32(env, src));
            e::write32(env, out + 0x34u, e::read32(env, src + 4u));
            const uint32_t z = e::read32(env, src + 8u);
            e::write32(env, out + 0x3Cu, kOne);
            e::write32(env, out + 0x38u, z);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxInitScaleVec 0x147A08 (a0 = out, a1 = a scale; src/cod/00147a08.c) ------------------------------------
        // The unit matrix by quadword (sqc2: out & ~0xF), then out +0 / +0x14 / +0x28 = a1[0..2] word by word.
        template <class Env>
        bool initScaleVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1);
            uint32_t rows, src;
            if (((a0 | a1) & 3u) != 0u || !plainRam(a0 & ~0xFu, 64u, rows) || !plainRam(a1, 12u, src) ||
                !e::vf0Constant(ctx))
                return false;
            const uint32_t out = a0 & PS2_RAM_MASK; // inside the rows (a0 - (a0 & ~0xF) < 16)
            writeMat(env, rows, unit());
            e::write32(env, out, e::read32(env, src));
            e::write32(env, out + 0x14u, e::read32(env, src + 4u));
            e::write32(env, out + 0x28u, e::read32(env, src + 8u));
            returnFrom(ctx);
            return true;
        }

        // The rotations' inputs: the angles (read once: they may not alias anything written before their read), the
        // coefficients (once: no write reaches them). Charges each rotation's guest calls and back-edges.
        struct RotInputs
        {
            uint32_t order;
            float angles[3];
            Vec4 coefficients;
        };
        // MtxInitRotVec's switch: lw a0, 0x41EA00 + 4 * order; jr a0 (the ELF's .rodata; a different entry declines).
        constexpr uint32_t kRotOrderTable = 0x41EA00u;
        constexpr uint32_t kRotOrderCases[6] = {0x147788u, 0x1477B0u, 0x1477D8u, 0x147810u, 0x147848u, 0x147880u};
        // order <= 5: a1 plain, word-aligned, clear of `clear` (the frames, the matrix the rotations build) and of
        // the coefficients' block; the coefficients below 2^62 (the rotMatrix native's checks).
        template <class Env>
        bool rotInputs(Env &env, RotInputs &in, uint32_t a1, uint32_t frame, uint32_t frameSize, uint32_t matrix)
        {
            if (in.order > 5u)
                return true;
            uint32_t angles, coefficients, entry;
            if (!plainRam(kRotOrderTable + 4u * in.order, 4u, entry) || e::read32(env, entry) != kRotOrderCases[in.order])
                return false;
            if ((a1 & 3u) != 0u || !plainRam(a1, 12u, angles) || !plainRam(kCoefficients, 16u, coefficients) ||
                e::overlaps(angles, 12u, frame, frameSize) || e::overlaps(angles, 12u, matrix, 64u) ||
                e::overlaps(coefficients, 16u, frame, frameSize) || e::overlaps(coefficients, 16u, matrix, 64u))
                return false;
            std::memcpy(in.angles, env.at(angles, 12u), 12);
            in.coefficients = e::read128(env, coefficients);
            return libvu0::finiteInputs(in.coefficients);
        }
        // MtxInitRotVec's guest calls: order 5 calls sceVu0RotMatrix, which calls the three; each RotMatrix calls
        // ecossin and loops three times.
        template <class Env>
        void chargeRotations(Env &env, uint32_t order)
        {
            if (order > 5u)
                return;
            if (order == 5u)
                env.call(); // jal sceVu0RotMatrix
            for (int i = 0; i < 3; ++i)
            {
                env.call(); // jal sceVu0RotMatrixX / Y / Z
                env.call(); // its jal _sceVu0ecossin
                for (int k = 0; k < 3; ++k)
                    env.checkpoint(32u); // its back-edges
            }
        }

        // ---- MtxInitRotVec 0x147728 (a0 = out, a1 = Euler angles, a2 = rotOrder; src/cod/00147728.c) ------------------
        // A 0x30 frame (s0, s1, ra), sceVu0RotMatrix's 0x30 below it (order 5). out & ~0xF throughout (sqc2 / lqc2).
        constexpr uint32_t kInitRotVec = 0x147728u, kInitRotFrames = 0x60u;
        template <class Env>
        bool initRotVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0);
            RotInputs in;
            in.order = e::u32(ctx, e::a2) & 0xFFu;
            uint32_t frame, out;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - kInitRotFrames, kInitRotFrames, frame) || !e::vf0Constant(ctx) ||
                !plainRam(a0 & ~0xFu, 64u, out) || e::overlaps(out, 64u, frame, kInitRotFrames) ||
                (in.order <= 5u && addiOverflows(a0, 64u)) ||
                !rotInputs(env, in, e::u32(ctx, e::a1), frame, kInitRotFrames, out))
                return false;
            Mat4 m = unit();
            if (!rotationByOrder(m, in.order, in.angles, vf0Constant(), in.coefficients))
                return false;
            chargeRotations(env, in.order);
            writeMat(env, out, m);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxMulRotVec 0x147B60 (a0 = out, a1 = A, a2 = Euler angles, a3 = rotOrder; src/cod/00134880.c) ----------
        // A 0x70 frame, buf at sp: MtxInitRotVec(buf, a2, a3 & 0xFF); MtxMultiply(a0, a1, buf). The frames below sp0:
        // its own, MtxInitRotVec's 0x30 and sceVu0RotMatrix's 0x30 (MtxMultiply's 0x10 lies inside them). buf is dead
        // stack: the native keeps it in registers.
        constexpr uint32_t kMulRotVec = 0x147B60u, kMulRotFrames = 0xD0u;
        template <class Env>
        bool mulRotVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), buf = sp0 - 0x70u;
            RotInputs in;
            in.order = e::u32(ctx, e::a3) & 0xFFu;
            uint32_t frame, out, ma;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - kMulRotFrames, kMulRotFrames, frame) || !e::vf0Constant(ctx) ||
                !plainRam(a0 & ~0xFu, 64u, out) || !plainRam(e::u32(ctx, e::a1) & ~0xFu, 64u, ma) ||
                e::overlaps(out, 64u, frame, kMulRotFrames) || e::overlaps(ma, 64u, frame, kMulRotFrames) ||
                addiOverflows(a0, 64u) || addiOverflows(buf, 64u) ||
                !rotInputs(env, in, e::u32(ctx, e::a2), frame, kMulRotFrames, frame))
                return false;
            Mat4 b = unit();
            if (!rotationByOrder(b, in.order, in.angles, vf0Constant(), in.coefficients))
                return false;
            const Mat4 a = readMat(env, ma);
            if (!productInputs(a, b))
                return false;
            const Mat4 result = product(a, b);
            env.call(); // jal MtxInitRotVec
            chargeRotations(env, in.order);
            env.call(); // jal MtxMultiply
            env.call(); // its jal sceVu0MulMatrix
            for (int k = 0; k < 3; ++k)
                env.checkpoint(32u); // MulMatrix's back-edges
            writeMat(env, out, result);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxMulScaleVec 0x147BF8 (a0 = out, a1 = A, a2 = a scale; src/cod/00147bb0.c) -----------------------------
        // A 0x70 frame, buf at sp: MtxInitScaleVec(buf, a2); MtxMultiply(a0, a1, buf) (its 0x10 frame below).
        constexpr uint32_t kMulScaleVec = 0x147BF8u, kMulScaleFrames = 0x80u;
        template <class Env>
        bool mulScaleVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a2 = e::u32(ctx, e::a2);
            uint32_t frame, out, ma, scale;
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - kMulScaleFrames, kMulScaleFrames, frame) || !e::vf0Constant(ctx) ||
                !plainRam(a0 & ~0xFu, 64u, out) || !plainRam(e::u32(ctx, e::a1) & ~0xFu, 64u, ma) ||
                (a2 & 3u) != 0u || !plainRam(a2, 12u, scale) || e::overlaps(out, 64u, frame, kMulScaleFrames) ||
                e::overlaps(ma, 64u, frame, kMulScaleFrames) || e::overlaps(scale, 12u, frame, kMulScaleFrames) ||
                addiOverflows(a0, 64u) || addiOverflows(sp0 - 0x70u, 64u))
                return false;
            Mat4 b = unit();
            const uint8_t *s = env.at(scale, 12u);
            uint32_t sx, sy, sz;
            std::memcpy(&sx, s, 4), std::memcpy(&sy, s + 4, 4), std::memcpy(&sz, s + 8, 4);
            b.r[0] = bitsOf(sx, 0u, 0u, 0u);
            b.r[1] = bitsOf(0u, sy, 0u, 0u);
            b.r[2] = bitsOf(0u, 0u, sz, 0u);
            const Mat4 a = readMat(env, ma);
            if (!productInputs(a, b))
                return false;
            const Mat4 result = product(a, b);
            env.call(); // jal MtxInitScaleVec
            env.call(); // jal MtxMultiply
            env.call(); // its jal sceVu0MulMatrix
            for (int k = 0; k < 3; ++k)
                env.checkpoint(32u);
            writeMat(env, out, result);
            returnFrom(ctx);
            return true;
        }
    }

    void registerFunctions()
    {
        // M1 batch 1 (Nov 25): shadowed (unchecked) until a K build shows 0 mismatches over the replays.
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *)) {
            NativeFunction fn{name, address, "motion", false, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = 0u;
            registerFunction(fn);
        };
        add("Adjust_theta_vec", kAdjustTheta, &adjustThetaVec<RealEnv>, &adjustThetaVec<ShadowEnv>);
        add("MtxInitTransVec", 0x1476D0u, &initTransVec<RealEnv>, &initTransVec<ShadowEnv>);
        add("MtxInitRotVec", kInitRotVec, &initRotVec<RealEnv>, &initRotVec<ShadowEnv>);
        add("MtxInitScaleVec", 0x147A08u, &initScaleVec<RealEnv>, &initScaleVec<ShadowEnv>);
        add("MtxMulRotVec", kMulRotVec, &mulRotVec<RealEnv>, &mulRotVec<ShadowEnv>);
        add("MtxMulScaleVec", kMulScaleVec, &mulScaleVec<RealEnv>, &mulScaleVec<ShadowEnv>);
    }
}
