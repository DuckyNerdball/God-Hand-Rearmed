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

        // ==== M1 batch 2: the batch-1 bodies chained in registers (each step checked as its own native checks it) ====
        // The steps' charges: a composite's jal to a batch-1 function plus everything that function charges.
        template <class Env>
        void chargeMultiply(Env &env)
        {
            env.call(); // jal MtxMultiply
            env.call(); // its jal sceVu0MulMatrix
            for (int k = 0; k < 3; ++k)
                env.checkpoint(32u);
        }
        template <class Env>
        void chargeMulTransVec(Env &env)
        {
            env.call(); // jal MtxMulTransVec
            env.call(); // its jal MtxInitTransVec
            chargeMultiply(env);
        }
        template <class Env>
        void chargeMulScaleVec(Env &env)
        {
            env.call(); // jal MtxMulScaleVec
            env.call(); // its jal MtxInitScaleVec
            chargeMultiply(env);
        }
        template <class Env>
        void chargeMulRotVec(Env &env, uint32_t order)
        {
            env.call(); // jal MtxMulRotVec
            env.call(); // its jal MtxInitRotVec
            chargeRotations(env, order);
            chargeMultiply(env);
        }

        // MtxMultiply(out, A, B) as a step: false = its checks fail (decline the whole call).
        bool multiplyStep(Mat4 &out, const Mat4 &a, const Mat4 &b)
        {
            if (!productInputs(a, b))
                return false;
            out = product(a, b);
            return true;
        }
        // MtxInitRotVec's rotations for a step (angles already in hand): its table entry and coefficients checked as the
        // native does (rotInputs), read from RAM clear of `frame` and `written`.
        template <class Env>
        bool rotationStep(Env &env, Mat4 &m, uint32_t order, const float angles[3], uint32_t frame, uint32_t frameSize,
                          uint32_t written, uint32_t writtenSize)
        {
            if (order > 5u)
                return true;
            uint32_t entry, coefficients;
            if (!plainRam(kRotOrderTable + 4u * order, 4u, entry) || e::read32(env, entry) != kRotOrderCases[order] ||
                !plainRam(kCoefficients, 16u, coefficients) || e::overlaps(coefficients, 16u, frame, frameSize) ||
                e::overlaps(coefficients, 16u, written, writtenSize))
                return false;
            const Vec4 c = e::read128(env, coefficients);
            if (!libvu0::finiteInputs(c))
                return false;
            Mat4 r = unit();
            if (!rotationByOrder(r, order, angles, vf0Constant(), c))
                return false;
            return multiplyStep(m, m, r);
        }
        // A word-aligned 12-byte vector in plain RAM, clear of the frames and of what the call writes before reading it.
        // MtxInitTransVec's t: only out +0x00..+0x2F come before its reads (each later word is written after the read
        // it could alias), so t may sit at out +0x30 (the game's anchor is &mtxLocal[12]; a model's pos &mtx[12]).
        constexpr uint32_t kTransReadAfter = 0x30u;
        template <class Env>
        bool vec3(Env &env, uint32_t address, uint32_t frame, uint32_t frameSize, uint32_t written, uint32_t writtenSize,
                  uint32_t (&w)[3])
        {
            uint32_t at;
            if ((address & 3u) != 0u || !plainRam(address, 12u, at) || e::overlaps(at, 12u, frame, frameSize) ||
                e::overlaps(at, 12u, written, writtenSize))
                return false;
            std::memcpy(w, env.at(at, 12u), 12);
            return true;
        }
        // The EE FPU's add.s / mul.s / div.s (1.0 / x) on the runtime's host path: no exponent-255 operand, no result
        // past FLT_MAX - 1 ulp, an ordinary divisor (ps2FpuAdd / Mul / Divide take their exact path otherwise).
        bool fpuOperand(float x) { return (floatBits(x) & 0x7F800000u) != 0x7F800000u; }
        bool fpuAdd(float a, float b, float &r)
        {
            if (!fpuOperand(a) || !fpuOperand(b))
                return false;
            r = addScalar(a, b);
            return !fpuPastMax(r);
        }
        bool fpuMul(float a, float b, float &r)
        {
            if (!fpuOperand(a) || !fpuOperand(b))
                return false;
            r = mulScalar(a, b);
            return !fpuPastMax(r);
        }
        bool fpuReciprocal(float b, float &r)
        {
            const uint32_t exponent = floatBits(b) & 0x7F800000u;
            if (exponent == 0u || exponent == 0x7F800000u)
                return false;
            r = divScalar(1.0f, b);
            return !fpuPastMax(r);
        }
        // cParts' rot + rotAdd (add.s each, w = 1.0) through Adjust_theta_vec: the wrapped Euler angles.
        template <class Env>
        bool partAngles(Env &env, uint32_t part, float angles[3])
        {
            const uint8_t *p = env.at(part + 0x100u, 12u), *q = env.at(part + 0x130u, 12u);
            float rot[3], add[3], sum[3];
            std::memcpy(rot, p, 12), std::memcpy(add, q, 12);
            for (int i = 0; i < 3; ++i)
                if (!fpuAdd(rot[i], add[i], sum[i]))
                    return false;
            uint32_t table;
            if (!plainRam(kThetaTable, 48u, table))
                return false;
            const uint8_t *t = env.at(table, 48u);
            ThetaTable tt;
            tt.c = load(t);
            std::memcpy(tt.one, t + 16, 16);
            std::memcpy(tt.mask, t + 32, 16);
            const Vec4 v = bitsOf(floatBits(sum[0]), floatBits(sum[1]), floatBits(sum[2]), kOne);
            if (!thetaInputs(v, tt))
                return false;
            const Vec4 wrapped = adjustTheta(v, tt);
            angles[0] = lane<0>(wrapped), angles[1] = lane<1>(wrapped), angles[2] = lane<2>(wrapped);
            return true;
        }

        // ---- MtxMulTransVec 0x147B18 (a0 = out, a1 = A, a2 = t; src/cod/00137aa0.c) ----------------------------------
        // A 0x70 frame, buf at sp: MtxInitTransVec(buf, a2); MtxMultiply(a0, a1, buf) (its 0x10 below).
        constexpr uint32_t kMulTransVec = 0x147B18u, kMulTransFrames = 0x80u;
        template <class Env>
        bool mulTransVec(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0);
            uint32_t frame, out, ma, t[3];
            if ((sp0 & 0xFu) != 0u || !plainRam(sp0 - kMulTransFrames, kMulTransFrames, frame) || !e::vf0Constant(ctx) ||
                !plainRam(a0 & ~0xFu, 64u, out) || !plainRam(e::u32(ctx, e::a1) & ~0xFu, 64u, ma) ||
                e::overlaps(out, 64u, frame, kMulTransFrames) || e::overlaps(ma, 64u, frame, kMulTransFrames) ||
                addiOverflows(a0, 64u) || addiOverflows(sp0 - 0x70u, 64u) ||
                !vec3(env, e::u32(ctx, e::a2), frame, kMulTransFrames, frame, kMulTransFrames, t))
                return false;
            Mat4 m;
            if (!multiplyStep(m, readMat(env, ma), translation(t[0], t[1], t[2])))
                return false;
            env.call(); // jal MtxInitTransVec
            chargeMultiply(env);
            writeMat(env, out, m);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxInitCoord 0x147A48 (a0 = out, a1 = t, a2 = angles, a3 = scale, t0 = rotOrder; src/cod/0012cc00.c) ---
        // A 0x50 frame: MtxInitTransVec(a0, a1); MtxMulRotVec(a0, a0, a2, t0 & 0xFF); MtxMulScaleVec(a0, a0, a3). The
        // angles and scale are read after a0 is written: they may not touch it (t only its first 0x30 bytes).
        constexpr uint32_t kInitCoord = 0x147A48u, kInitCoordFrames = 0x50u + kMulRotFrames;
        template <class Env>
        bool initCoord(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), order = e::u32(ctx, e::t0) & 0xFFu;
            uint32_t frame, out, t[3], s[3];
            float angles[3] = {};
            if ((sp0 & 0xFu) != 0u || (a0 & 0xFu) != 0u || !plainRam(sp0 - kInitCoordFrames, kInitCoordFrames, frame) ||
                !e::vf0Constant(ctx) || !plainRam(a0, 64u, out) || e::overlaps(out, 64u, frame, kInitCoordFrames) ||
                addiOverflows(a0, 64u) || !vec3(env, e::u32(ctx, e::a1), frame, kInitCoordFrames, out, kTransReadAfter, t) ||
                !vec3(env, e::u32(ctx, e::a3), frame, kInitCoordFrames, out, 64u, s))
                return false;
            if (order <= 5u)
            {
                uint32_t a[3];
                if (!vec3(env, e::u32(ctx, e::a2), frame, kInitCoordFrames, out, 64u, a))
                    return false;
                std::memcpy(angles, a, 12);
            }
            Mat4 m = translation(t[0], t[1], t[2]);
            if (!rotationStep(env, m, order, angles, frame, kInitCoordFrames, out, 64u) ||
                !multiplyStep(m, m, scaling(s[0], s[1], s[2])))
                return false;
            env.call(); // jal MtxInitTransVec
            chargeMulRotVec(env, order);
            chargeMulScaleVec(env);
            writeMat(env, out, m);
            returnFrom(ctx);
            return true;
        }

        // ---- cParts_calcLocal 0x150138 (a0 = a cParts; src/cod/00150138.c) -----------------------------------------------
        // rot = rot +0x100 + rotAdd +0x130 (w = 1) in a 0x30 frame, Adjust_theta_vec(rot); MtxInitTransVec(part,
        // anchor *(part +0xD0)); MtxMulRotVec(part, part, rot, 0); MtxMulScaleVec(part, part, scale +0x110). The local
        // matrix is part +0x00..+0x3F; the anchor (normally part +0x30) is read after its first 0x30 bytes are written.
        constexpr uint32_t kCalcLocal = 0x150138u, kCalcLocalFrames = 0x30u + kMulRotFrames;
        template <class Env>
        bool calcLocal(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0);
            uint32_t frame, part, t[3], s[3];
            float angles[3];
            if ((sp0 & 0xFu) != 0u || (a0 & 0xFu) != 0u || !plainRam(sp0 - kCalcLocalFrames, kCalcLocalFrames, frame) ||
                !e::vf0Constant(ctx) || !plainRam(a0, 0x140u, part) || e::overlaps(part, 0x140u, frame, kCalcLocalFrames) ||
                addiOverflows(a0, 64u) || !partAngles(env, part, angles) ||
                !vec3(env, e::read32(env, part + 0xD0u), frame, kCalcLocalFrames, part, kTransReadAfter, t) ||
                !vec3(env, a0 + 0x110u, frame, kCalcLocalFrames, part, 64u, s))
                return false;
            Mat4 m = translation(t[0], t[1], t[2]);
            if (!rotationStep(env, m, 0u, angles, frame, kCalcLocalFrames, part, 64u) ||
                !multiplyStep(m, m, scaling(s[0], s[1], s[2])))
                return false;
            env.call(); // jal Adjust_theta_vec
            env.call(); // jal MtxInitTransVec
            chargeMulRotVec(env, 0u);
            chargeMulScaleVec(env);
            writeMat(env, part, m);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxInverse 0x147568 (a0 = out, a1 = in; asm, no frame, jr ra + nop) -----------------------------------------
        // The FPU reads a1's words, the VU its quadwords (a1 & ~0xF): a1 must be aligned for both to see the same rows.
        // All reads come before the writes (out may be in).
        constexpr uint32_t kInverse = 0x147568u;
        template <class Env>
        bool mtxInverse(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t a1 = e::u32(ctx, e::a1);
            uint32_t in, out;
            if ((a1 & 0xFu) != 0u || !plainRam(a1, 64u, in) || !plainRam(e::u32(ctx, e::a0) & ~0xFu, 64u, out) ||
                !e::vf0Constant(ctx))
                return false;
            const Mat4 a = readMat(env, in);
            Mat4 m;
            if (!inverseInputs(a) || !inverse(a, m))
                return false;
            writeMat(env, out, m);
            ctx->pc = e::u32(ctx, e::ra); // nop delay slots: in_delay_slot as it was
            return true;
        }

        // ---- func_001501D0 (a0 = a cParts, a1 = flags; asm): the part's world matrix +0x80 from its parent's ---------
        // parent = *(part +0x148). a1 & 0x40004 != 0x40000: world = MtxMultiply(parent world, local +0x00). == 0x40000
        // (the parent's scale undone): rot = rot + rotAdd wrapped (Adjust_theta_vec), inv = (1 / parent scaleCalc +0x120..,
        // 1); scaleCalc +0x120 = parent's * scale +0x110 (+0x12C = 1); world = parent world x T(anchor *(part +0xD0)) x
        // S(inv) x R(rot, order 0) x S(scaleCalc). Then, when part +0x154 & 0x800 (read after both paths): the twist
        // (motion_core twist(): about the axis at 0x3BD8B0 by part +0x150; the enemies' skeletons, parts 3, 4, 26, 29-36:
        // rung4-motion-map.md "The twist"). A 0xD0 frame, the deepest callee MtxMulRotVec's 0xD0 below it (the twist's
        // deepest, 0x159FC8 0x40 -> sinf 0x20 -> rem_pio2f 0x50, fits in it).
        constexpr uint32_t kPartWorld = 0x1501D0u, kPartWorldFrames = 0xD0u + kMulRotFrames;
        constexpr uint32_t kTwistAxis = 0x3BD8B0u, kCosTable = 0x3BF140u;
        // The twist's inputs, read after the world matrix and scaleCalc are written (the tables clear of the part and the
        // frames, which the original writes before it reads them), and the twist applied to world. calls: as twist().
        template <class Env>
        bool twistStep(Env &env, Mat4 &world, uint32_t part, uint32_t frame)
        {
            uint32_t axisAt, cosAt;
            if (!plainRam(kTwistAxis, 16u, axisAt) || !plainRam(kCosTable, 16u, cosAt) ||
                e::overlaps(axisAt, 16u, frame, kPartWorldFrames) || e::overlaps(axisAt, 16u, part, 0x160u) ||
                e::overlaps(cosAt, 16u, frame, kPartWorldFrames) || e::overlaps(cosAt, 16u, part, 0x160u))
                return false;
            float axis[4], c[4], angle;
            std::memcpy(axis, env.at(axisAt, 16u), 16);
            std::memcpy(&angle, env.at(part + 0x150u, 4u), 4);
            std::memcpy(c, env.at(cosAt, 16u), 16);
            Exact x;
            int calls = 0;
            if (!twist(x, world, axis, angle, c, calls))
                return false;
            for (int k = 0; k < calls; ++k)
                env.call();
            chargeMultiply(env);
            return true;
        }
        template <class Env>
        bool partWorld(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0);
            const bool unscaled = (e::u32(ctx, e::a1) & 0x40004u) == 0x40000u;
            uint32_t frame, part, parent;
            if ((sp0 & 0xFu) != 0u || (a0 & 0xFu) != 0u || !plainRam(sp0 - kPartWorldFrames, kPartWorldFrames, frame) ||
                !e::vf0Constant(ctx) || !plainRam(a0, 0x160u, part) || e::overlaps(part, 0x160u, frame, kPartWorldFrames) ||
                addiOverflows(a0 + 0x80u, 64u))
                return false;
            const uint32_t parentAddress = e::read32(env, part + 0x148u);
            const bool twisted = (e::read32(env, part + 0x154u) & 0x800u) != 0u;
            if ((parentAddress & 0xFu) != 0u || !plainRam(parentAddress, 0x130u, parent) ||
                e::overlaps(parent, 0x130u, frame, kPartWorldFrames) || e::overlaps(parent, 0x130u, part, 0x160u))
                return false;
            const Mat4 parentWorld = readMat(env, parent + 0x80u);
            Mat4 world;
            if (!unscaled)
            {
                if (!multiplyStep(world, parentWorld, readMat(env, part)))
                    return false;
                if (twisted && !twistStep(env, world, part, frame))
                    return false;
                chargeMultiply(env);
                writeMat(env, part + 0x80u, world);
                returnFrom(ctx);
                return true;
            }
            float angles[3], parentScale[3], scale[3], inv[3], scaleCalc[3];
            if (!partAngles(env, part, angles))
                return false;
            std::memcpy(parentScale, env.at(parent + 0x120u, 12u), 12);
            std::memcpy(scale, env.at(part + 0x110u, 12u), 12);
            for (int i = 0; i < 3; ++i)
                if (!fpuReciprocal(parentScale[i], inv[i]) || !fpuMul(parentScale[i], scale[i], scaleCalc[i]))
                    return false;
            uint32_t t[3];
            // the anchor is read after scaleCalc is written
            if (!vec3(env, e::read32(env, part + 0xD0u), frame, kPartWorldFrames, part + 0x120u, 16u, t) ||
                !multiplyStep(world, parentWorld, translation(t[0], t[1], t[2])) ||
                !multiplyStep(world, world, scaling(floatBits(inv[0]), floatBits(inv[1]), floatBits(inv[2]))) ||
                !rotationStep(env, world, 0u, angles, frame, kPartWorldFrames, part, 0x160u) ||
                !multiplyStep(world, world, scaling(floatBits(scaleCalc[0]), floatBits(scaleCalc[1]), floatBits(scaleCalc[2]))))
                return false;
            if (twisted && !twistStep(env, world, part, frame))
                return false;
            env.call(); // jal Adjust_theta_vec
            chargeMulTransVec(env);
            chargeMulScaleVec(env);
            chargeMulRotVec(env, 0u);
            chargeMulScaleVec(env);
            uint8_t *sc = env.at(part + 0x120u, 16u);
            std::memcpy(sc, scaleCalc, 12);
            std::memcpy(sc + 12, &kOne, 4);
            writeMat(env, part + 0x80u, world);
            returnFrom(ctx);
            return true;
        }

        // ==== M1's blend batch: func_00156D70 fused (its helpers inline), and the quaternion helpers on their own =====
        constexpr uint32_t kAtanTable = 0x42C350u, kNextTable = 0x41EE48u;
        // capVu0Atan2's coefficients (c0..c7, pi/4) and 0x15A190's {1, 2, 0}: read as the originals read them; a different
        // next table declines (it would index past the 3x3).
        template <class Env>
        bool blendTables(Env &env, BlendTables &tb)
        {
            uint32_t atan, next;
            if (!plainRam(kAtanTable, 36u, atan) || !plainRam(kNextTable, 12u, next))
                return false;
            std::memcpy(tb.atan, env.at(atan, 36u), 36);
            std::memcpy(tb.next, env.at(next, 12u), 12);
            return tb.next[0] == 1u && tb.next[1] == 2u && tb.next[2] == 0u;
        }
        template <class Env>
        void writeWords(Env &env, uint32_t offset, const float *w, int count)
        {
            std::memcpy(env.at(offset, 4u * static_cast<uint32_t>(count)), w, 4u * static_cast<uint32_t>(count));
        }

        // ---- func_00156D70 (a0 = out, a1 = A, a2 = B, f12 = t; asm): blendTrs ---------------------------------------------
        // A 0x1E0 frame; the deepest callee chain slerp 0x70 -> sinf 0x20 -> rem_pio2f 0x50 below it. A and B are read
        // whole before out is written (out may be either); all three quadword-aligned (lqc2 / sqc2 and the FPU's swc1 must
        // address the same words), clear of the frames.
        constexpr uint32_t kBlend = 0x156D70u, kBlendFrames = 0x1E0u + 0xE0u;
        template <class Env>
        bool blendNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1), a2 = e::u32(ctx, e::a2);
            uint32_t frame, out, ma, mb;
            BlendTables tb;
            if ((sp0 & 0xFu) != 0u || ((a0 | a1 | a2) & 0xFu) != 0u || !plainRam(sp0 - kBlendFrames, kBlendFrames, frame) ||
                !e::vf0Constant(ctx) || !plainRam(a0, 64u, out) || !plainRam(a1, 64u, ma) || !plainRam(a2, 64u, mb) ||
                e::overlaps(out, 64u, frame, kBlendFrames) || e::overlaps(ma, 64u, frame, kBlendFrames) ||
                e::overlaps(mb, 64u, frame, kBlendFrames) || !blendTables(env, tb))
                return false;
            Exact x;
            int calls = 0;
            const Mat4 m = blendTrs(x, readMat(env, ma), readMat(env, mb), ctx->f[12], tb, calls);
            if (!x.ok)
                return false;
            for (int k = 0; k < calls; ++k)
                env.call();
            writeMat(env, out, m);
            returnFrom(ctx);
            return true;
        }

        // ---- func_0015A6A0 (a0 = out, a1 = q1, a2 = q2, f12 = t; asm): slerp ---------------------------------------------
        // A 0x70 frame, sinf's 0x20 and rem_pio2f's 0x50 below. out[k] is written after q1[k] / q2[k] are read and before
        // q1[k + 1]: out may be q1 or q2, never a partial overlap.
        constexpr uint32_t kSlerp = 0x15A6A0u, kSlerpFrames = 0xE0u;
        bool sameOrApart(uint32_t a, uint32_t b) { return a == b || !e::overlaps(a, 16u, b, 16u); }
        template <class Env>
        bool slerpNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1), a2 = e::u32(ctx, e::a2);
            uint32_t frame, out, p1, p2, atan;
            if ((sp0 & 0xFu) != 0u || ((a0 | a1 | a2) & 3u) != 0u || !e::vf0Constant(ctx) ||
                !plainRam(sp0 - kSlerpFrames, kSlerpFrames, frame) || !plainRam(a0, 16u, out) || !plainRam(a1, 16u, p1) || !plainRam(a2, 16u, p2) || !plainRam(kAtanTable, 36u, atan) ||
                e::overlaps(out, 16u, frame, kSlerpFrames) || e::overlaps(p1, 16u, frame, kSlerpFrames) ||
                e::overlaps(p2, 16u, frame, kSlerpFrames) || !sameOrApart(out, p1) || !sameOrApart(out, p2))
                return false;
            float q1[4], q2[4], c[9], q[4];
            std::memcpy(q1, env.at(p1, 16u), 16);
            std::memcpy(q2, env.at(p2, 16u), 16);
            std::memcpy(c, env.at(atan, 36u), 36);
            Exact x;
            int calls = 0;
            slerp(x, q1, q2, ctx->f[12], c, calls, q);
            if (!x.ok)
                return false;
            for (int k = 0; k < calls; ++k)
                env.call();
            writeWords(env, out, q, 4);
            returnFrom(ctx);
            return true;
        }

        // ---- func_0015A190 (a0 = q, a1 = m; asm): a quaternion from m's 3x3 ----------------------------------------------
        // A 0x80 frame. q is written between m's reads: q may not touch m's 3x3 words (+0x00..+0x2B).
        constexpr uint32_t kMatToQuat = 0x15A190u, kMatToQuatFrame = 0x80u;
        template <class Env>
        bool matToQuatNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1);
            uint32_t frame, out, m;
            BlendTables tb;
            if ((sp0 & 0xFu) != 0u || ((a0 | a1) & 3u) != 0u || !plainRam(sp0 - kMatToQuatFrame, kMatToQuatFrame, frame) ||
                !plainRam(a0, 16u, out) || !plainRam(a1, 0x2Cu, m) || e::overlaps(out, 16u, m, 0x2Cu) ||
                e::overlaps(out, 16u, frame, kMatToQuatFrame) || e::overlaps(m, 0x2Cu, frame, kMatToQuatFrame) ||
                !blendTables(env, tb))
                return false;
            Mat4 mm;
            const uint8_t *p = env.at(m, 0x2Cu);
            for (int r = 0; r < 3; ++r)
            {
                uint32_t w[4] = {0u, 0u, 0u, 0u};
                std::memcpy(w, p + 16 * r, r == 2 ? 12u : 16u);
                mm.r[r] = bitsOf(w[0], w[1], w[2], w[3]);
            }
            Exact x;
            float q[4];
            mat2quat(x, mm, tb.next, q);
            if (!x.ok)
                return false;
            writeWords(env, out, q, 4);
            returnFrom(ctx);
            return true;
        }

        // ---- MtxInitRotQuaternion 0x1478A8 (a0 = out, a1 = q; asm, no frame): q is read whole first ------------------------
        constexpr uint32_t kQuatToMat = 0x1478A8u;
        template <class Env>
        bool quatToMatNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1);
            uint32_t out, in;
            if (((a0 | a1) & 3u) != 0u || !plainRam(a0, 64u, out) || !plainRam(a1, 16u, in))
                return false;
            float q[4];
            std::memcpy(q, env.at(in, 16u), 16);
            Exact x;
            const Mat4 m = quat2mat(x, q);
            if (!x.ok)
                return false;
            uint8_t *dst = env.at(out, 64u);
            for (int k = 0; k < 4; ++k)
                store(dst + 16 * k, m.r[k]);
            returnFrom(ctx);
            return true;
        }
        // ==== M3 (batch 1): the Hermite track (the pose's kind-1 interpolator) and its half-float unpacks ============
        // Called by the pose through its table at 0x3BD910 (jalr) and by each other (jal); caller_scan (ELF + DLLs, the
        // table's 27 jalr sites): no caller reads a scratch register after them.
        constexpr uint32_t kHalf = 0x156508u, kHalf4 = 0x156550u, kHalfTable = 0x41EDF0u, kTrack = 0x158CD0u;
        // ---- func_00156508 (a0 = a 16-bit float; asm, jr ra + nop): f0 = halfBits(a0) -----------------------------------
        template <class Env>
        bool halfNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            e::setFBits(ctx, 0, halfBits(e::u32(ctx, e::a0)));
            ctx->pc = e::u32(ctx, e::ra); // nop delay slot: in_delay_slot as it was
            return true;
        }
        // ---- func_00156550 (a0 = out, a1 = in; asm, MMI, jr ra + nop): four halfBits by the table's lanes --------------
        // lq in (a1 & ~0xF), the table's four quadwords, then sq out (a0 & ~0xF): all read before the write.
        template <class Env>
        bool half4Native(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            uint32_t in, out, table;
            if (!plainRam(e::u32(ctx, e::a1) & ~0xFu, 16u, in) || !plainRam(e::u32(ctx, e::a0) & ~0xFu, 16u, out) ||
                !plainRam(kHalfTable, 64u, table))
                return false;
            uint32_t x[4], t[16], r[4];
            std::memcpy(x, env.at(in, 16u), 16);
            std::memcpy(t, env.at(table, 64u), 64);
            for (uint32_t q = 0; q < 4u; ++q)
                r[q] = halfLane(x[q], t, q);
            std::memcpy(env.at(out, 16u), r, 16);
            ctx->pc = e::u32(ctx, e::ra);
            return true;
        }
        // ---- func_00158CD0 (a0 = the motion, a1 = its record, f12 = t; asm): the track's value at t in f0 ----------------
        // record: +2 the key count n (lhu), +8 the track's offset (lw); track = a0 + it (addu). motion_core trackSample.
        // A 0xA0 frame, its saved registers stored before any read (the Hermite's four words at +0x00..+0x1F): the record,
        // the track with its keys (+0xC + 4n) and 0x41EDF0's table clear of it.
        constexpr uint32_t kTrackFrame = 0xA0u;
        template <class Env>
        bool trackNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a1 = e::u32(ctx, e::a1);
            uint32_t frame, rec, table;
            if ((sp0 & 0xFu) != 0u || (a1 & 3u) != 0u || !plainRam(sp0 - kTrackFrame, kTrackFrame, frame) ||
                !plainRam(a1, 12u, rec) || e::overlaps(rec, 12u, frame, kTrackFrame) || !plainRam(kHalfTable, 64u, table) ||
                e::overlaps(table, 64u, frame, kTrackFrame))
                return false;
            const uint32_t n = u16At(env.at(rec + 2u, 2u));
            const uint32_t trackAddress = e::u32(ctx, e::a0) + e::read32(env, rec + 8u);
            const uint32_t span = 0xCu + 4u * n;
            uint32_t track;
            if ((trackAddress & 1u) != 0u || !plainRam(trackAddress, span, track) || e::overlaps(track, span, frame, kTrackFrame))
                return false;
            uint32_t t[16];
            std::memcpy(t, env.at(table, 64u), 64);
            Exact x;
            int calls = 0, loops = 0;
            const float v = trackSample(x, env.at(track, span), n, bitsFloat(e::fBits(ctx, 12)), t, calls, loops);
            if (!x.ok)
                return false;
            for (int k = 0; k < calls; ++k)
                env.call();
            for (int k = 0; k < loops; ++k)
                env.checkpoint(32u);
            e::setFBits(ctx, 0, floatBits(v));
            returnFrom(ctx);
            return true;
        }

        // ==== M3 (batch 2): the root channels' decode (motion_core RecDecoder) ============================================
        // func_00159AD0 (setMotionInfo x3, the pose x6: the cursors at the motion's +0x1B4 / +0x1D4 / +0x1F4),
        // RecDecode_readHeader (also func_001595F8 x6) and func_00159DA0 (from the two). caller_scan (ELF + 81 DLLs): 9 / 7
        // / 2 sites, no DLL site, no j, no code pointer; none reads a scratch register after the call. Each runs its
        // callees inline (0x156508 / 0x156550's bodies: checked natives) on a copy of the cursor; the rest read in place.
        // A decline (anything read outside plain RAM, on the cursor or in the stack window the original's frames cover,
        // an unaligned access, a divide by a 0 span, a value past FLT_MAX - 1 ulp, a walk that never ends) changes nothing;
        // else the cursor's 0x20 bytes are written once at the end (the original writes only them).
        constexpr uint32_t kReadHeader = 0x1599B0u, kDecode = 0x159AD0u, kDecodeKeys = 0x159DA0u;
        // stack windows: 0x159DA0 0x50; readHeader 0x10 + 0x50; 0x159AD0 0x50 + readHeader's
        constexpr uint32_t kDecodeKeysFrame = 0x50u, kReadHeaderFrame = 0x60u, kDecodeFrame = 0xB0u;
        template <class Env>
        struct DecodeMem
        {
            Env &env;
            uint32_t cursor, frame, frameSize;
            const uint8_t *read(uint32_t address, uint32_t size)
            {
                uint32_t at;
                if (!plainRam(address, size, at) || e::overlaps(at, size, cursor, 0x20u) ||
                    e::overlaps(at, size, frame, frameSize))
                    return nullptr;
                return env.at(at, size);
            }
        };
        // The entry checks: sp aligned (0x156550 reads its quadword at sp & ~0xF), the cursor word-aligned, plain, clear
        // of the window. Runs `body` on a RecDecoder over a copy of the cursor; true = it ran (the cursor written back).
        template <class Env, class Body>
        bool runDecoder(Env &env, R5900Context *ctx, uint32_t frameSize, Body body)
        {
            if (!env.atEnd)
                return false;
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0);
            uint32_t frame, cursor;
            if ((sp0 & 0xFu) != 0u || (a0 & 3u) != 0u || !plainRam(sp0 - frameSize, frameSize, frame) ||
                !plainRam(a0, 0x20u, cursor) || e::overlaps(cursor, 0x20u, frame, frameSize))
                return false;
            DecodeMem<Env> mem{env, cursor, frame, frameSize};
            Exact x;
            RecDecoder<DecodeMem<Env>> d{mem, x};
            uint8_t *c = env.at(cursor, 0x20u);
            std::memcpy(d.o, c, 0x20);
            body(d);
            if (!d.ok || !x.ok)
                return false;
            if (std::memcmp(c, d.o, 0x20) != 0)
                std::memcpy(c, d.o, 0x20);
            for (int k = 0; k < d.calls; ++k)
                env.call();
            for (int k = 0; k < d.loops; ++k)
                env.checkpoint(32u);
            returnFrom(ctx);
            return true;
        }
        // ---- func_00159AD0 (a0 = the cursor, a1 = the block, a2 = the record, f12 = t; asm): f0 -----------------------
        template <class Env>
        bool decodeNative(Env &env, R5900Context *ctx)
        {
            uint32_t v = 0u;
            const uint32_t block = e::u32(ctx, e::a1), record = e::u32(ctx, e::a2);
            const float t = bitsFloat(e::fBits(ctx, 12));
            if (!runDecoder(env, ctx, kDecodeFrame, [&](auto &d) { v = d.decode(block, record, t); }))
                return false;
            e::setFBits(ctx, 0, v);
            return true;
        }
        // ---- RecDecode_readHeader 0x1599B0 (a0 = the cursor, a1 = the block, a2 = the record; src/cod/001599b0.c) ------
        template <class Env>
        bool readHeaderNative(Env &env, R5900Context *ctx)
        {
            const uint32_t block = e::u32(ctx, e::a1), record = e::u32(ctx, e::a2);
            return runDecoder(env, ctx, kReadHeaderFrame, [&](auto &d) { d.header(block, record); });
        }
        // ---- func_00159DA0 (a0 = the cursor; asm) ---------------------------------------------------------------------
        template <class Env>
        bool decodeKeysNative(Env &env, R5900Context *ctx)
        {
            return runDecoder(env, ctx, kDecodeKeysFrame, [&](auto &d) { d.keys(); });
        }

        // ==== M4: the pose's kind-3 interpolator and the pose fused ======================================================
        // ---- func_00158FC8 (a0 = the motion, a1 = its record, f12 = t; asm, a leaf: no frame, no calls): f0 ------------
        // record: +2 the key count n (lhu), +8 the track's offset (lw); track = a0 + it (addu): six f32 then n 8-byte keys
        // (motion_core kind3Sample). Reads only. The record and the track word-aligned (lw / lwc1), plain RAM.
        constexpr uint32_t kKind3 = 0x158FC8u;
        template <class Env>
        bool kind3Native(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            const uint32_t a1 = e::u32(ctx, e::a1);
            uint32_t rec, track;
            if ((a1 & 3u) != 0u || !plainRam(a1, 12u, rec))
                return false;
            const uint32_t n = u16At(env.at(rec + 2u, 2u));
            const uint32_t trackAddress = e::u32(ctx, e::a0) + e::read32(env, rec + 8u);
            const uint32_t span = 0x18u + 8u * n;
            if ((trackAddress & 3u) != 0u || !plainRam(trackAddress, span, track))
                return false;
            Exact x;
            int loops = 0;
            const float v = kind3Sample(x, env.at(track, span), n, bitsFloat(e::fBits(ctx, 12)), loops);
            if (!x.ok)
                return false;
            for (int k = 0; k < loops; ++k)
                env.checkpoint(32u);
            e::setFBits(ctx, 0, floatBits(v));
            returnFrom(ctx);
            return true;
        }

        // ==== M5 (batch 1): matrix to Euler func_00147C88 and its helpers =============================================
        // caller_scan (ELF + 81 DLLs): capVu0Atan2 132 + 2, capVu0Acos 12, MtxGetScaleVec 3, asinf's wrapper 8, 0x147C88 66
        // sites; none reads a scratch register after the call, no j, no code pointer.
        constexpr uint32_t kMatToEuler = 0x147C88u, kScaleVec = 0x148160u, kAtan2 = 0x1F8180u, kAcos = 0x1F8140u,
                           kAsinf = 0x3A1B78u, kLibVersion = 0x45F024u, kEulerCases = 0x41EA20u;
        template <class Env>
        bool atanCoefficients(Env &env, float c[9])
        {
            uint32_t at;
            if (!plainRam(kAtanTable, 36u, at))
                return false;
            std::memcpy(c, env.at(at, 36u), 36);
            return true;
        }
        // ---- capVu0Atan2 0x1F8180 (y = f12, x = f13; asm, a leaf; jr ra + nop: the generated code clears in_delay_slot): f0
        template <class Env>
        bool atan2Native(Env &env, R5900Context *ctx)
        {
            float c[9];
            if (!env.atEnd || !e::vf0Constant(ctx) || !atanCoefficients(env, c))
                return false;
            Exact x;
            const float v = atan2Body(x, bitsFloat(e::fBits(ctx, 12)), bitsFloat(e::fBits(ctx, 13)), c);
            if (!x.ok)
                return false;
            e::setFBits(ctx, 0, floatBits(v));
            returnFrom(ctx);
            return true;
        }
        // ---- capVu0Acos 0x1F8140 (f12; src/cod/001f8140.c): atan2(sqrt.s(1 - x x), x); a 0x10 frame ----------------------
        constexpr uint32_t kAcosFrame = 0x10u;
        template <class Env>
        bool acosNative(Env &env, R5900Context *ctx)
        {
            float c[9];
            uint32_t frame;
            const uint32_t sp0 = e::u32(ctx, e::sp);
            if (!env.atEnd || !e::vf0Constant(ctx) || !plainRam(sp0 - kAcosFrame, kAcosFrame, frame) ||
                !atanCoefficients(env, c))
                return false;
            Exact x;
            const float v = bitsFloat(e::fBits(ctx, 12));
            const float s = x.sqrt(x.sub(one(), x.mul(v, v)));
            const float r = atan2Body(x, s, v, c);
            if (!x.ok)
                return false;
            env.call();
            e::setFBits(ctx, 0, floatBits(r));
            returnFrom(ctx);
            return true;
        }
        // ---- func_003A1B78 (asinf: the libm wrapper; src/cod/003a1b78.c) with __ieee754_asinf / sqrtf inline: f0 ---------
        // frames: 0x60, asinf's 0x50 (sqrtf, isnanf, fabsf: leaves). _LIB_VERSION read in place.
        constexpr uint32_t kAsinfWindow = 0x60u + 0x50u;
        template <class Env>
        bool asinfNative(Env &env, R5900Context *ctx)
        {
            uint32_t frame, lib;
            const uint32_t sp0 = e::u32(ctx, e::sp);
            if (!env.atEnd || !plainRam(sp0 - kAsinfWindow, kAsinfWindow, frame) || !plainRam(kLibVersion, 4u, lib) ||
                e::overlaps(lib, 4u, frame, kAsinfWindow))
                return false;
            Exact x;
            float r;
            int calls = 0, loops = 0;
            if (!asinfWrapper(x, bitsFloat(e::fBits(ctx, 12)), static_cast<int32_t>(e::read32(env, lib)), r, calls, loops) ||
                !x.ok)
                return false;
            for (int k = 0; k < calls; ++k)
                env.call();
            for (int k = 0; k < loops; ++k)
                env.checkpoint(32u);
            e::setFBits(ctx, 0, floatBits(r));
            returnFrom(ctx);
            return true;
        }
        // The 12 words a matrix's rows 0-2 give (lwc1: word-aligned), clear of the stack window
        template <class Env>
        bool readRows(Env &env, uint32_t address, uint32_t frame, uint32_t frameSize, float m[12])
        {
            uint32_t at;
            if ((address & 3u) != 0u || !plainRam(address, 48u, at) || e::overlaps(at, 48u, frame, frameSize))
                return false;
            std::memcpy(m, env.at(at, 48u), 48);
            return true;
        }
        // 0x150710's stores (x, w = 1.0, y, z in that order) into a word-aligned vector clear of the window
        template <class Env>
        bool storeVec(Env &env, uint32_t address, uint32_t frame, uint32_t frameSize, const float v[3])
        {
            uint32_t at;
            if ((address & 3u) != 0u || !plainRam(address, 16u, at) || e::overlaps(at, 16u, frame, frameSize))
                return false;
            uint8_t *p = env.at(at, 16u);
            const uint32_t w[4] = {floatBits(v[0]), floatBits(v[1]), floatBits(v[2]), kOne};
            std::memcpy(p, w, 16);
            return true;
        }
        // ---- MtxGetScaleVec 0x148160 (a0 = out, a1 = a matrix; asm): out = the rows' lengths, 1.0; a 0x10 frame -------------
        constexpr uint32_t kScaleVecFrame = 0x10u;
        template <class Env>
        bool scaleVecNative(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            const uint32_t sp0 = e::u32(ctx, e::sp);
            float m[12], s[3];
            if (!env.atEnd || !plainRam(sp0 - kScaleVecFrame, kScaleVecFrame, frame) ||
                !readRows(env, e::u32(ctx, e::a1), frame, kScaleVecFrame, m))
                return false;
            Exact x;
            rowLengths(x, m, s);
            if (!x.ok || !storeVec(env, e::u32(ctx, e::a0), frame, kScaleVecFrame, s))
                return false;
            env.call(); // jal 0x150710
            returnFrom(ctx);
            return true;
        }
        // ---- func_00147C88 (a0 = out, a1 = a matrix, a2 = its rotation order (byte); asm): out = its Euler angles, 1.0 ------
        // MtxGetScaleVec into the frame (sqc2 vf0 there first: dead), the order's case (the jump table's entry must be the
        // ELF's: a patched one declines), out through 0x150710 at the end (every read before it). A 0x70 frame; the deepest
        // callee the asinf wrapper (0x60 + 0x50).
        constexpr uint32_t kMatToEulerFrame = 0x70u, kMatToEulerWindow = kMatToEulerFrame + kAsinfWindow;
        template <class Env>
        bool matToEulerNative(Env &env, R5900Context *ctx)
        {
            static constexpr uint32_t kCases[6] = {0x147CF8u, 0x147DA0u, 0x147E5Cu, 0x147F14u, 0x147FB4u, 0x148070u};
            uint32_t frame, lib, cases;
            const uint32_t sp0 = e::u32(ctx, e::sp), order = e::u32(ctx, e::a2) & 0xFFu;
            float m[12], s[3], c[9], out[3];
            if (!env.atEnd || (sp0 & 0xFu) != 0u || !e::vf0Constant(ctx) ||
                !plainRam(sp0 - kMatToEulerWindow, kMatToEulerWindow, frame) || !plainRam(kLibVersion, 4u, lib) ||
                !plainRam(kEulerCases, 24u, cases) || !atanCoefficients(env, c) ||
                !readRows(env, e::u32(ctx, e::a1), frame, kMatToEulerWindow, m))
                return false;
            if (order < 6u && e::read32(env, cases + 4u * order) != kCases[order])
                return false;
            Exact x;
            int calls = 2, loops = 0; // MtxGetScaleVec and its 0x150710
            rowLengths(x, m, s);
            if (!matToEuler(x, m, s, order, c, static_cast<int32_t>(e::read32(env, lib)), out, calls, loops) || !x.ok ||
                !storeVec(env, e::u32(ctx, e::a0), frame, kMatToEulerWindow, out))
                return false;
            calls += 1; // the last 0x150710
            for (int k = 0; k < calls; ++k)
                env.call();
            for (int k = 0; k < loops; ++k)
                env.checkpoint(32u);
            returnFrom(ctx);
            return true;
        }

        // ==== M2 (batch 3): cModel_calcParts and cModel_calcWorldParts fused ===========================================
        // The whole per-model pass as one native: the model's own matrix (MtxInitCoord) and its parts' (Adjust_theta_vec,
        // cParts_calcLocal, the blend 0x156D70, the parent join 0x1501D0), each callee run by its own native's body on a
        // scratch context (the same checks, the same charges), each jal charged, each taken back-edge a checkpoint.
        // A part's natives read what earlier ones wrote (its parent's world matrix, its own local one), so they run on a
        // JournalEnv: every access keeps the bytes it touched (the inner natives reach RAM through at() alone, reads and
        // writes alike), and a decline puts them back newest first: RAM as it was, the original runs. Any access inside
        // the stack window (the bytes below the entry sp that the original's frames write) declines.
        struct MotionJournalStore
        {
            struct Entry
            {
                uint32_t at;
                uint32_t size;
                uint32_t old; // offset into bytes
            };
            std::vector<Entry> entries;
            std::vector<uint8_t> bytes;
            bool busy = false;
        };
        inline MotionJournalStore &motionJournalStore()
        {
            thread_local MotionJournalStore store;
            return store;
        }
        template <class Env>
        struct JournalEnv
        {
            Env &env;
            uint32_t low, size; // the stack window (RAM offsets)
            bool atEnd = true;
            bool ok = true;
            bool kept = false;
            MotionJournalStore *store = nullptr;
            JournalEnv(Env &e, uint32_t windowLow, uint32_t windowSize) : env(e), low(windowLow), size(windowSize)
            {
                MotionJournalStore &s = motionJournalStore();
                if (s.busy)
                    ok = false;
                else
                {
                    s.busy = true, store = &s;
                    s.entries.clear(), s.bytes.clear();
                }
            }
            ~JournalEnv()
            {
                if (!store)
                    return;
                if (!kept)
                    for (size_t i = store->entries.size(); i-- > 0u;)
                    {
                        const MotionJournalStore::Entry &w = store->entries[i];
                        std::memcpy(env.at(w.at, w.size), store->bytes.data() + w.old, w.size);
                    }
                store->busy = false;
            }
            JournalEnv(const JournalEnv &) = delete;
            JournalEnv &operator=(const JournalEnv &) = delete;
            void commit() { kept = true; }
            // A read that keeps nothing (M4's pose: its tracks are read in place, only writes need their old bytes).
            const uint8_t *read(uint32_t offset, uint32_t bytes)
            {
                if (!store || e::overlaps(offset, bytes, low, size))
                    ok = false;
                return env.at(offset, bytes);
            }
            uint8_t *at(uint32_t offset, uint32_t bytes)
            {
                uint8_t *p = env.at(offset, bytes);
                if (!store)
                {
                    ok = false;
                    return p;
                }
                if (e::overlaps(offset, bytes, low, size))
                    ok = false; // still kept: the inner native may write it before the decline
                const uint32_t old = static_cast<uint32_t>(store->bytes.size());
                store->entries.push_back({offset, bytes, old});
                store->bytes.insert(store->bytes.end(), p, p + bytes);
                return p;
            }
            bool call() { return env.call(); }
            bool checkpoint(uint32_t cycles) { return env.checkpoint(cycles); }
        };

        // cModel (a cParts with more): mtx +0x80, pos pointer +0xF0, rot +0x100, scale +0x110, scaleCalc +0x120, rotOrder
        // byte +0x140, first part +0x144, objFlags +0x250, blend +0x27C, partNum byte +0x2B4. A part: next +0x144, parent
        // +0x148, partFlags +0x154 (0x8 no local, 0x10 no parent join, 0x100000 no blend). objFlags 0x800 = the owner builds
        // the matrix, 0x20 = near (calcWorldParts' models), bit 2 = blending (calcParts sets it from blend > 0).
        constexpr uint32_t kModelBytes = 0x2B8u;
        struct ModelPass
        {
            uint32_t model;  // RAM offset
            uint32_t self;   // its guest address (the inner natives' arguments are guest addresses)
            uint32_t callSp; // the callees' sp: the entry sp less the pass's frame
        };
        // The scratch context for an inner native: sp and the argument registers set, the rest the caller's (vf0).
        void innerArgs(R5900Context &c, uint32_t sp, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
        {
            e::setS32(&c, e::sp, sp);
            e::setS32(&c, e::a0, a0);
            e::setS32(&c, e::a1, a1);
            e::setS32(&c, e::a2, a2);
            e::setS32(&c, e::a3, a3);
        }
        // MtxInitCoord(mtx, *pos, &rot, &scale, rotOrder byte) and the scale copied to scaleCalc word by word (lwc1 /
        // swc1, x then y then z), unless objFlags & 0x800.
        template <class Env>
        bool modelMatrix(JournalEnv<Env> &j, R5900Context &c, const ModelPass &p)
        {
            if ((e::read32(j, p.model + 0x250u) & 0x800u) != 0u)
                return true;
            innerArgs(c, p.callSp, p.self + 0x80u, e::read32(j, p.model + 0xF0u), p.self + 0x100u, p.self + 0x110u);
            e::setZe32(&c, e::t0, *j.at(p.model + 0x140u, 1u));
            j.call();
            if (!initCoord(j, &c))
                return false;
            for (uint32_t k = 0; k < 12u; k += 4u)
                e::write32(j, p.model + 0x120u + k, e::read32(j, p.model + 0x110u + k));
            return true;
        }
        float readFloat(uint32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        // blend(part, part, part +0x40, f12 = the model's blend as read now)
        template <class Env>
        bool partBlend(JournalEnv<Env> &j, R5900Context &c, const ModelPass &p, uint32_t part)
        {
            innerArgs(c, p.callSp, part, part, part + 0x40u, e::u32(&c, e::a3));
            e::setFBits(&c, 12, e::read32(j, p.model + 0x27Cu));
            j.call();
            return blendNative(j, &c);
        }
        // the parent join: func_001501D0(part, objFlags as read now)
        template <class Env>
        bool partJoin(JournalEnv<Env> &j, R5900Context &c, const ModelPass &p, uint32_t part)
        {
            innerArgs(c, p.callSp, part, e::read32(j, p.model + 0x250u), e::u32(&c, e::a2), e::u32(&c, e::a3));
            j.call();
            return partWorld(j, &c);
        }
        template <class Env>
        bool modelEntry(Env &env, R5900Context *ctx, uint32_t frame, uint32_t window, ModelPass &p, uint32_t &stackLow)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), self = e::u32(ctx, e::a0);
            if (!env.atEnd || (sp0 & 0xFu) != 0u || (self & 0xFu) != 0u || !e::vf0Constant(ctx) ||
                !plainRam(sp0 - window, window, stackLow) || !plainRam(self, kModelBytes, p.model) ||
                e::overlaps(p.model, kModelBytes, stackLow, window))
                return false;
            p.self = self;
            p.callSp = sp0 - frame;
            return true;
        }

        // ---- cModel_calcParts 0x149EB8 (a0 = the model; src/cod/00149eb8.c) ---------------------------------------------
        // A 0x60 frame: the model matrix; objFlags bit 2 = (0 < blend) (c.lt.s); then per part (partNum, re-read each
        // turn; next +0x144 read in the loop's delay slot): Adjust_theta_vec(part +0x100); calcLocal unless 0x8; the blend
        // when 0 < blend (re-read) and not 0x100000 and the model not near (0x20); the join unless 0x10.
        constexpr uint32_t kCalcParts = 0x149EB8u, kCalcPartsFrame = 0x60u;
        constexpr uint32_t kCalcPartsWindow = kCalcPartsFrame + kBlendFrames; // the deepest callee: the blend (0x2C0)
        constexpr int kPartLimit = 255; // partNum is a byte
        template <class Env>
        bool calcPartsNative(Env &env, R5900Context *ctx)
        {
            ModelPass p;
            uint32_t stackLow;
            if (!modelEntry(env, ctx, kCalcPartsFrame, kCalcPartsWindow, p, stackLow))
                return false;
            JournalEnv<Env> j(env, stackLow, kCalcPartsWindow);
            R5900Context c = *ctx;
            if (!j.ok || !modelMatrix(j, c, p))
                return false;
            const float zero = bitsFloat(0u);
            const uint32_t flags = e::read32(j, p.model + 0x250u);
            e::write32(j, p.model + 0x250u,
                       fpuLess(zero, readFloat(e::read32(j, p.model + 0x27Cu))) ? (flags | 4u) : (flags & ~4u));
            const uint32_t count = *j.at(p.model + 0x2B4u, 1u);
            uint32_t part = e::read32(j, p.model + 0x144u);
            for (int i = 0; i < static_cast<int>(count) && j.ok;)
            {
                uint32_t at;
                if ((part & 0xFu) != 0u || !plainRam(part, 0x160u, at))
                    return false;
                innerArgs(c, p.callSp, part + 0x100u, e::u32(&c, e::a1), e::u32(&c, e::a2), e::u32(&c, e::a3));
                j.call();
                if (!adjustThetaVec(j, &c))
                    return false;
                if ((e::read32(j, at + 0x154u) & 0x8u) == 0u)
                {
                    innerArgs(c, p.callSp, part, e::u32(&c, e::a1), e::u32(&c, e::a2), e::u32(&c, e::a3));
                    j.call();
                    if (!calcLocal(j, &c))
                        return false;
                }
                if (fpuLess(zero, readFloat(e::read32(j, p.model + 0x27Cu))) &&
                    (e::read32(j, at + 0x154u) & 0x100000u) == 0u && (e::read32(j, p.model + 0x250u) & 0x20u) == 0u &&
                    !partBlend(j, c, p, part))
                    return false;
                if ((e::read32(j, at + 0x154u) & 0x10u) == 0u && !partJoin(j, c, p, part))
                    return false;
                const uint32_t n = *j.at(p.model + 0x2B4u, 1u);
                ++i;
                part = e::read32(j, at + 0x144u);
                if (i < static_cast<int>(n))
                    j.checkpoint(32u); // bnez back to the loop's top
                else
                    break;
            }
            if (!j.ok)
                return false;
            j.commit();
            returnFrom(ctx);
            return true;
        }

        // ---- cModel_calcWorldParts 0x14A038 (a0 = the model; src/cod/0014a038.c) ----------------------------------------
        // Near models only (objFlags 0x20; else it returns at once): the model matrix; per part (flags read once) unless
        // 0x10: when 0 < blend and not 0x100000, MtxInverse(frame, parent world) and sceVu0MulMatrix(frame, frame, the
        // part's world) into its frame (never read again: dead stack, charged only; their reads must be plain RAM), then
        // the blend; then the join. A 0xA0 frame.
        constexpr uint32_t kCalcWorldParts = 0x14A038u, kCalcWorldFrame = 0xA0u;
        constexpr uint32_t kCalcWorldWindow = kCalcWorldFrame + kBlendFrames;
        template <class Env>
        bool calcWorldPartsNative(Env &env, R5900Context *ctx)
        {
            ModelPass p;
            uint32_t stackLow;
            if (!modelEntry(env, ctx, kCalcWorldFrame, kCalcWorldWindow, p, stackLow))
                return false;
            JournalEnv<Env> j(env, stackLow, kCalcWorldWindow);
            if (!j.ok)
                return false;
            if ((e::read32(j, p.model + 0x250u) & 0x20u) == 0u)
            {
                j.commit();
                returnFrom(ctx);
                return true;
            }
            R5900Context c = *ctx;
            if (!modelMatrix(j, c, p))
                return false;
            const float zero = bitsFloat(0u);
            const uint32_t count = *j.at(p.model + 0x2B4u, 1u);
            uint32_t part = e::read32(j, p.model + 0x144u);
            for (int i = 0; i < static_cast<int>(count) && j.ok;)
            {
                uint32_t at;
                if ((part & 0xFu) != 0u || !plainRam(part, 0x160u, at))
                    return false;
                const uint32_t flags = e::read32(j, at + 0x154u);
                if ((flags & 0x10u) == 0u)
                {
                    if (fpuLess(zero, readFloat(e::read32(j, p.model + 0x27Cu))) && (flags & 0x100000u) == 0u)
                    {
                        const uint32_t parentWorld = e::read32(j, at + 0x148u) + 0x80u;
                        uint32_t in;
                        if (!plainRam(parentWorld & ~0xFu, 0x50u, in))
                            return false;
                        j.call(); // jal MtxInverse (no calls, no loops)
                        j.call(); // jal sceVu0MulMatrix
                        for (int k = 0; k < 3; ++k)
                            j.checkpoint(32u);
                        if (!partBlend(j, c, p, part))
                            return false;
                    }
                    if (!partJoin(j, c, p, part))
                        return false;
                }
                const uint32_t n = *j.at(p.model + 0x2B4u, 1u);
                ++i;
                part = e::read32(j, at + 0x144u);
                if (i < static_cast<int>(n))
                    j.checkpoint(32u);
                else
                    break;
            }
            if (!j.ok)
                return false;
            j.commit();
            returnFrom(ctx);
            return true;
        }

        // ---- the pose func_00158328 (a0 = the object; asm 2464): its motion at +0x428 (an mtb3 block) at the clock -----
        // +0x430 into the root outputs and every part. Records from block + 8, 12 bytes each, in order: word0's low byte the
        // part (lb; 0xFF = the root), & 0xF00 the channel, (>> 12) & 0xF the kind (the table 0x3BD910: kind 0 GetField_2
        // = the record's +8 as f32, 1 the Hermite 0x158CD0, 2 / 6 return 0, 3 0x158FC8; any other entry declines), each
        // present record taken in turn. fp = (+0x434 >> 1) & 1 (a mirrored motion): the root's +0x310 / +0x344 / +0x348
        // negated; per part s4 (fp, +0x14C set, not 0x800000: x on the partner +0x14C, negated) and s6 (fp, +0x14C set,
        // none of 0x800000 / 0x2000000 / 0x1000000: rot y / z negated).
        // Root: 0x0FF / 0x1FF / 0x2FF = the decode 0x159AD0 on the cursors +0x1B4 / +0x1D4 / +0x1F4 into +0x310..+0x318
        // (default 0); 0x3FF..0x5FF the table into +0x340..+0x348 (default 0), Adjust_theta_vec(+0x340); 0x6FF..0x8FF into
        // +0x370..+0x378 (default 1.0). Per part (count +0x2B4 read once for the loop, again for each lookup; parts +0x278):
        // flags +0x154: 0x2000 sets 0x400000, 0x8000 sets 0x200000; 0x80 cleared; 0x1 skips the part's records; channels
        // 0-2 (unless 0x200) the table into the anchor +0xD0's x / y / z (unkeyed with 0 < blend +0x27C and not 0x100000:
        // the bind +0xE0..+0xE8); 3-5 (unless 0x100) the decode on the part's cursors +0x1B4.. into +0x100..+0x108
        // (unkeyed with 0x8000: 0); 6-8 (unless 0x400) the table into +0x110..+0x118; a keyed channel sets 0x80. Skipped
        // channels step over their records. Everything in the original's order on a JournalEnv (writes keep their bytes;
        // tracks read in place); the callees run their checked natives' bodies (decode, Adjust_theta_vec) or cores (the
        // Hermite, kind 3) inline, each jal / jalr charged, each taken back-edge a checkpoint. A 0xD0 frame; the deepest
        // callee 0x159AD0 (0xB0).
        constexpr uint32_t kPose = 0x158328u, kPoseFrame = 0xD0u, kPoseWindow = kPoseFrame + kDecodeFrame;
        constexpr uint32_t kKindTable = 0x3BD910u, kGetField = 0x158CC8u, kZero2 = 0x158FB8u, kZero6 = 0x159490u;
        constexpr uint32_t kSign = 0x80000000u;
        template <class Env>
        struct Pose
        {
            JournalEnv<Env> &j;
            R5900Context &c;
            uint32_t callSp = 0, block = 0, t = 0;
            bool ok = true;
            bool off(uint32_t address, uint32_t size, uint32_t &o)
            {
                if ((address & (size - 1u)) != 0u || !plainRam(address, size, o))
                    ok = false;
                return ok;
            }
            uint32_t lw(uint32_t address)
            {
                uint32_t o, v = 0u;
                if (off(address, 4u, o))
                    std::memcpy(&v, j.read(o, 4u), 4);
                return v;
            }
            int32_t lb(uint32_t address)
            {
                uint32_t o;
                return off(address, 1u, o) ? static_cast<int8_t>(*j.read(o, 1u)) : 0;
            }
            uint32_t lbu(uint32_t address)
            {
                uint32_t o;
                return off(address, 1u, o) ? *j.read(o, 1u) : 0u;
            }
            uint32_t lhu(uint32_t address)
            {
                uint32_t o;
                return off(address, 2u, o) ? u16At(j.read(o, 2u)) : 0u;
            }
            void sw(uint32_t address, uint32_t v)
            {
                uint32_t o;
                if (off(address, 4u, o))
                    e::write32(j, o, v);
            }
            bool fine() const { return ok && j.ok; }
            // jal func_00159AD0(cursor, block, record, f12 = t): f0
            uint32_t decode(uint32_t cursor, uint32_t record)
            {
                if (!fine())
                    return 0u;
                innerArgs(c, callSp, cursor, block, record, e::u32(&c, e::a3));
                e::setFBits(&c, 12, t);
                j.call();
                if (!decodeNative(j, &c))
                    ok = false;
                return e::fBits(&c, 0);
            }
            // jalr the table's entry for the record's kind (a0 = block, a1 = record, f12 = t): f0
            uint32_t interp(uint32_t record, uint32_t word)
            {
                const uint32_t fn = lw(kKindTable + ((word >> 10) & 0x3Cu));
                if (!fine())
                    return 0u;
                j.call();
                switch (fn)
                {
                case kGetField:
                    return lw(record + 8u);
                case kZero2:
                case kZero6:
                    return 0u;
                case kTrack:
                    return hermite(record);
                case kKind3:
                    return kind3(record);
                default: // 4, 5 (not native yet), a zero or patched entry
                    ok = false;
                    return 0u;
                }
            }
            // 0x158CD0's checks and core (trackNative) with the track read in place
            uint32_t hermite(uint32_t record)
            {
                const uint32_t n = lhu(record + 2u), trackAddress = block + lw(record + 8u), span = 0xCu + 4u * n;
                uint32_t track, table;
                if (!fine() || (trackAddress & 1u) != 0u || !plainRam(trackAddress, span, track) ||
                    !plainRam(kHalfTable, 64u, table))
                {
                    ok = false;
                    return 0u;
                }
                uint32_t tb[16];
                std::memcpy(tb, j.read(table, 64u), 64);
                Exact x;
                int calls = 0, loops = 0;
                const float v = trackSample(x, j.read(track, span), n, bitsFloat(t), tb, calls, loops);
                if (!x.ok)
                    ok = false;
                for (int k = 0; k < calls; ++k)
                    j.call();
                for (int k = 0; k < loops; ++k)
                    j.checkpoint(32u);
                return floatBits(v);
            }
            // 0x158FC8's checks and core (kind3Native) with the track read in place
            uint32_t kind3(uint32_t record)
            {
                const uint32_t n = lhu(record + 2u), trackAddress = block + lw(record + 8u), span = 0x18u + 8u * n;
                uint32_t track;
                if (!fine() || (trackAddress & 3u) != 0u || !plainRam(trackAddress, span, track))
                {
                    ok = false;
                    return 0u;
                }
                Exact x;
                int loops = 0;
                const float v = kind3Sample(x, j.read(track, span), n, bitsFloat(t), loops);
                if (!x.ok)
                    ok = false;
                for (int k = 0; k < loops; ++k)
                    j.checkpoint(32u);
                return floatBits(v);
            }
        };
        template <class Env>
        bool poseNative(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), obj = e::u32(ctx, e::a0);
            uint32_t stackLow;
            if (!env.atEnd || (sp0 & 0xFu) != 0u || (obj & 3u) != 0u || !plainRam(sp0 - kPoseWindow, kPoseWindow, stackLow))
                return false;
            JournalEnv<Env> j(env, stackLow, kPoseWindow);
            if (!j.ok)
                return false;
            R5900Context c = *ctx;
            Pose<Env> p{j, c};
            p.callSp = sp0 - kPoseFrame;
            p.block = p.lw(obj + 0x428u);
            const uint32_t half = p.lhu(obj + 0x434u);
            uint32_t word = p.lw(p.block + 8u);
            uint32_t rec = p.block + 8u;
            p.t = p.lw(obj + 0x430u);
            const bool fp = ((half >> 1) & 1u) != 0u;
            // the root's translation: the decode on the object's cursors, default 0
            uint32_t f0 = 0u;
            if ((word & 0xFFFu) == 0x0FFu)
            {
                f0 = p.decode(obj + 0x1B4u, rec);
                rec = p.block + 0x14u;
            }
            p.sw(obj + 0x310u, f0);
            if (fp)
                p.sw(obj + 0x310u, f0 ^ kSign);
            for (uint32_t ch = 1u; ch < 3u; ++ch)
            {
                word = p.lw(rec);
                f0 = 0u;
                if ((word & 0xFFFu) == (ch << 8 | 0xFFu))
                {
                    f0 = p.decode(obj + 0x1B4u + 0x20u * ch, rec);
                    rec += 0xCu;
                }
                p.sw(obj + 0x310u + 4u * ch, f0);
            }
            // the root's rotation (the table, default 0), wrapped; the root's scale (default 1.0)
            for (uint32_t ch = 3u; ch < 9u; ++ch)
            {
                word = p.lw(rec);
                f0 = ch < 6u ? 0u : kOne;
                if ((word & 0xFFFu) == (ch << 8 | 0xFFu))
                {
                    f0 = p.interp(rec, word);
                    rec += 0xCu;
                }
                const uint32_t out = ch < 6u ? 0x340u + 4u * (ch - 3u) : 0x370u + 4u * (ch - 6u);
                if (ch == 8u)
                    break; // its store follows the count's read
                p.sw(obj + out, f0);
                if (ch == 5u && p.fine())
                {
                    innerArgs(c, p.callSp, obj + 0x340u, e::u32(&c, e::a1), e::u32(&c, e::a2), e::u32(&c, e::a3));
                    j.call();
                    if (!adjustThetaVec(j, &c))
                        return false;
                    if (fp)
                    {
                        const uint32_t y = p.lw(obj + 0x344u), z = p.lw(obj + 0x348u);
                        p.sw(obj + 0x344u, y ^ kSign);
                        p.sw(obj + 0x348u, z ^ kSign);
                    }
                }
            }
            const uint32_t count = p.lbu(obj + 0x2B4u);
            p.sw(obj + 0x378u, f0);
            const float zero = bitsFloat(0u);
            auto keyed = [&](uint32_t channel, int32_t s2, uint32_t &w) {
                if (p.lb(rec) != s2)
                    return false;
                w = p.lw(rec);
                return (w & 0xF00u) == channel << 8;
            };
            // a skipped group: each of its three channels' records stepped over while they are the part's (movz)
            auto skip = [&](uint32_t first, int32_t s2) {
                for (uint32_t ch = first; ch < first + 3u; ++ch)
                {
                    if (p.lb(rec) != s2)
                        return;
                    if ((p.lw(rec) & 0xF00u) == ch << 8)
                        rec += 0xCu;
                }
            };
            for (int32_t s2 = 0; s2 < static_cast<int32_t>(count) && p.fine();)
            {
                const uint32_t now = p.lbu(obj + 0x2B4u);
                const uint32_t part = s2 < static_cast<int32_t>(now) ? p.lw(p.lw(obj + 0x278u) + 4u * s2) : 0u;
                uint32_t s1 = part;
                uint32_t flags = p.lw(s1 + 0x154u);
                if ((flags & 0x2000u) != 0u)
                    p.sw(s1 + 0x154u, flags | 0x400000u);
                flags = p.lw(s1 + 0x154u);
                if ((flags & 0x8000u) != 0u)
                    p.sw(s1 + 0x154u, flags | 0x200000u);
                bool s4 = false, s6 = false;
                if (fp && p.lw(s1 + 0x14Cu) != 0u)
                    s4 = (p.lw(s1 + 0x154u) & 0x800000u) == 0u;
                if (fp && p.lw(part + 0x14Cu) != 0u)
                {
                    const uint32_t v1 = p.lw(part + 0x154u);
                    s6 = (v1 & 0x3800000u) == 0u;
                }
                if (s4)
                    s1 = p.lw(part + 0x14Cu);
                flags = p.lw(s1 + 0x154u);
                p.sw(s1 + 0x154u, flags & ~0x80u);
                auto set80 = [&] { p.sw(s1 + 0x154u, p.lw(s1 + 0x154u) | 0x80u); };
                uint32_t w = 0u;
                if ((flags & 1u) != 0u)
                {
                    // every motion channel off: step over the part's records (beql back: a checkpoint each)
                    if (p.lb(rec) == s2)
                    {
                        rec += 0xCu;
                        while (p.fine() && p.lb(rec) == s2)
                        {
                            rec += 0xCu;
                            j.checkpoint(32u);
                        }
                    }
                }
                else
                {
                    // translation: the anchor's x / y / z (x negated with s4); unkeyed while blending: the bind position
                    if ((flags & 0x200u) != 0u)
                        skip(0u, s2);
                    else
                        for (uint32_t ch = 0u; ch < 3u; ++ch)
                        {
                            if (keyed(ch, s2, w))
                            {
                                f0 = p.interp(rec, w);
                                rec += 0xCu;
                                const uint32_t anchor = p.lw(s1 + 0xD0u);
                                p.sw(anchor + 4u * ch, f0);
                                if (ch == 0u && s4)
                                    p.sw(anchor, f0 ^ kSign);
                                set80();
                            }
                            else if (fpuLess(zero, bitsFloat(p.lw(obj + 0x27Cu))) &&
                                     (p.lw(s1 + 0x154u) & 0x100000u) == 0u)
                            {
                                const uint32_t anchor = p.lw(s1 + 0xD0u);
                                p.sw(anchor + 4u * ch, p.lw(s1 + 0xE0u + 4u * ch));
                            }
                        }
                    // rotation: the decode on the part's cursors (y / z negated with s6); unkeyed with 0x8000: 0
                    if ((p.lbu(s1 + 0x155u) & 1u) != 0u)
                        skip(3u, s2);
                    else
                        for (uint32_t ch = 3u; ch < 6u; ++ch)
                        {
                            const uint32_t out = s1 + 0x100u + 4u * (ch - 3u);
                            if (keyed(ch, s2, w))
                            {
                                f0 = p.decode(s1 + 0x1B4u + 0x20u * (ch - 3u), rec);
                                rec += 0xCu;
                                if (ch == 3u)
                                {
                                    const uint32_t fl = p.lw(s1 + 0x154u);
                                    p.sw(out, f0);
                                    p.sw(s1 + 0x154u, fl | 0x80u);
                                    continue;
                                }
                                p.sw(out, f0);
                                if (s6)
                                    p.sw(out, f0 ^ kSign);
                                set80();
                            }
                            else if ((p.lbu(s1 + 0x155u) >> 7) != 0u)
                                p.sw(out, 0u);
                        }
                    // scale (unless 0x400): only keyed channels write
                    if ((p.lw(s1 + 0x154u) & 0x400u) != 0u)
                        skip(6u, s2);
                    else
                        for (uint32_t ch = 6u; ch < 9u; ++ch)
                        {
                            if (p.lb(rec) != s2)
                                break; // the part's records end (bne to the loop's end)
                            w = p.lw(rec);
                            if ((w & 0xF00u) != ch << 8)
                                continue; // another channel: the next test reads the same record
                            f0 = p.interp(rec, w);
                            rec += 0xCu;
                            const uint32_t fl = p.lw(s1 + 0x154u);
                            p.sw(s1 + 0x110u + 4u * (ch - 6u), f0);
                            p.sw(s1 + 0x154u, fl | 0x80u);
                        }
                }
                ++s2;
                if (s2 < static_cast<int32_t>(count))
                    j.checkpoint(32u); // bnel back to the loop's top
            }
            if (!p.fine())
                return false;
            j.commit();
            returnFrom(ctx);
            return true;
        }

        // ==== M5 batch 2: the IK leaves (claude/rung4-motion-map.md "M5 batch 2") =====================================
        // caller_scan (ELF + 81 DLLs): aimIkPart 1, capVu0ApplyMatrixXYZ 14, Quat_rotatePoint 4 (v0 taken as scratch too),
        // 0x15ABA0 1 sites; none reads a scratch register after the call, no j, no code pointer.
        constexpr uint32_t kApplyXyz = 0x1F8350u, kQuatRotate = 0x15ADA0u, kLookQuat = 0x15ABA0u, kAimIkPart = 0x153808u;

        // ---- capVu0ApplyMatrixXYZ 0x1F8350 (a0 = out, a1 = m, a2 = v; src/cod/001f8350.c; a leaf, jr ra + nop) -----------
        // vf4 = v, vf5-vf7 = m's rows 0-2, ACC / vf8 = applyXyz, out = vf8 (lqc2 / sqc2: every address & ~0xF). v and the
        // rows are read before out is written: out may be any of them.
        template <class Env>
        bool applyXyzNative(Env &env, R5900Context *ctx)
        {
            uint32_t out, m, v;
            if (!env.atEnd || !plainRam(e::u32(ctx, e::a0) & ~0xFu, 16u, out) ||
                !plainRam(e::u32(ctx, e::a1) & ~0xFu, 48u, m) || !plainRam(e::u32(ctx, e::a2) & ~0xFu, 16u, v))
                return false;
            const Vec4 vv = e::read128(env, v), r0 = e::read128(env, m), r1 = e::read128(env, m + 16u),
                       r2 = e::read128(env, m + 32u);
            if (!applyXyzInputs(r0, r1, r2, vv))
                return false;
            const libvu0::RowResult r = applyXyz(r0, r1, r2, vv);
            e::write128(env, out, r.r);
            e::setVf(ctx, 4, vv);
            e::setVf(ctx, 5, r0);
            e::setVf(ctx, 6, r1);
            e::setVf(ctx, 7, r2);
            e::setVf(ctx, 8, r.r);
            store(&ctx->vu0_acc, r.acc);
            ctx->pc = e::u32(ctx, e::ra); // a nop delay slot: in_delay_slot as it was
            return true;
        }

        // ---- Quat_rotatePoint 0x15ADA0 (a0 = out, a1 = q, a2 = p; src/cod/0015ada0.c): v0 = out --------------------------
        // A 0x50 frame (p.w = 0 is stored there before most reads: q / p inside it would read the frame), lwc1 / swc1
        // throughout (word-aligned). Every read before the first write: out.w = 1.0, then y, z, x.
        constexpr uint32_t kQuatRotateFrame = 0x50u;
        template <class Env>
        bool quatRotateNative(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1),
                           a2 = e::u32(ctx, e::a2);
            uint32_t frame, out, q, p;
            if (!env.atEnd || (sp0 & 0xFu) != 0u || ((a0 | a1 | a2) & 3u) != 0u ||
                !plainRam(sp0 - kQuatRotateFrame, kQuatRotateFrame, frame) || !plainRam(a0, 16u, out) ||
                !plainRam(a1, 16u, q) || !plainRam(a2, 12u, p) || e::overlaps(out, 16u, frame, kQuatRotateFrame) ||
                e::overlaps(q, 16u, frame, kQuatRotateFrame) || e::overlaps(p, 12u, frame, kQuatRotateFrame))
                return false;
            float qv[4], pv[3], o[3];
            std::memcpy(qv, env.at(q, 16u), 16);
            std::memcpy(pv, env.at(p, 12u), 12);
            Exact x;
            quatRotatePoint(x, qv, pv, o);
            if (!x.ok)
                return false;
            e::write32(env, out + 12u, kOne);
            e::write32(env, out + 4u, floatBits(o[1]));
            e::write32(env, out + 8u, floatBits(o[2]));
            e::write32(env, out, floatBits(o[0]));
            std::memcpy(&ctx->r[e::v0], &ctx->r[e::a0], 8); // daddu v0, a0, zero
            returnFrom(ctx);
            return true;
        }

        // ---- func_0015ABA0 (a0 = q, a1 = a, a2 = b; asm): q = the rotation with z row b and x / y rows from b x a ---------
        // A 0xD0 frame (s0 / s1, ra, f20 / f21 saved before b is copied in by quadword; the vectors and the matrix in it),
        // the thunk 0x101E28's 0x10 and 0x15A190's 0x80 below it. a (lwc1: word-aligned), b (lqc2: & ~0xF) and q (0x15A190's
        // stores, last) clear of the window. calls: the thunk and its capVu0Length twice, UnitMatrix, 0x15A190.
        constexpr uint32_t kLookQuatWindow = 0xD0u + kMatToQuatFrame;
        template <class Env>
        bool lookQuatNative(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), a0 = e::u32(ctx, e::a0), a1 = e::u32(ctx, e::a1);
            uint32_t frame, out, av, bv;
            BlendTables tb;
            if (!env.atEnd || (sp0 & 0xFu) != 0u || ((a0 | a1) & 3u) != 0u || !e::vf0Constant(ctx) ||
                !plainRam(sp0 - kLookQuatWindow, kLookQuatWindow, frame) || !plainRam(a0, 16u, out) ||
                !plainRam(a1, 12u, av) || !plainRam(e::u32(ctx, e::a2) & ~0xFu, 16u, bv) ||
                e::overlaps(out, 16u, frame, kLookQuatWindow) || e::overlaps(av, 12u, frame, kLookQuatWindow) ||
                e::overlaps(bv, 16u, frame, kLookQuatWindow) || !blendTables(env, tb))
                return false;
            float a[3], b[4], q[4];
            std::memcpy(a, env.at(av, 12u), 12);
            std::memcpy(b, env.at(bv, 16u), 16);
            Exact x;
            if (!lookQuat(x, a, b, tb.next, q) || !x.ok)
                return false;
            for (int k = 0; k < 6; ++k)
                env.call();
            writeWords(env, out, q, 4);
            returnFrom(ctx);
            return true;
        }

        // ---- cModel_aimIkPart 0x153808 (a0 = the model, a1 = the part, a2 = the target; src/cod/00153808.c) -------------
        // A 0xC0 frame (s0-s4, ra; the parent's inverse at sp). child = part +0x144 (read first); the part's world +0x80 =
        // MulMatrix(model mtx +0x80, local +0x00); its position *(part +0xF0) = ApplyMatrixXYZ(world, *(child +0xD0)), then
        // target - itself (xyz by swc1; the original skips that copy when the position is its own frame copy: in the window,
        // declined); local = MulMatrix(InversMatrix(*(part +0x148) world), world); the child's world by
        // func_001501D0(child, objFlags +0x250). Each step in the original's order on a JournalEnv (a step reads what the
        // earlier ones wrote); 0x1501D0 by its native on a scratch context, its frames below (the deepest, 0x1A0).
        constexpr uint32_t kAimIkFrame = 0xC0u, kAimIkWindow = kAimIkFrame + kPartWorldFrames;
        template <class Env>
        bool aimIkPartNative(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), model = e::u32(ctx, e::a0), part = e::u32(ctx, e::a1);
            uint32_t stackLow, pm, pp;
            if (!env.atEnd || (sp0 & 0xFu) != 0u || ((model | part) & 0xFu) != 0u || !e::vf0Constant(ctx) ||
                !plainRam(sp0 - kAimIkWindow, kAimIkWindow, stackLow) || !plainRam(model, 0x260u, pm) ||
                !plainRam(part, 0x160u, pp) || addiOverflows(part, 64u) || addiOverflows(part + 0x80u, 64u))
                return false;
            JournalEnv<Env> j(env, stackLow, kAimIkWindow);
            if (!j.ok)
                return false;
            // MulMatrix(out = part +0x80, A = model +0x80, B = part): A whole first, B row k before out row k (apart here)
            const uint32_t child = e::read32(j, pp + 0x144u);
            Mat4 world;
            if (!multiplyStep(world, readMat(j, pm + 0x80u), readMat(j, pp)))
                return false;
            j.call();
            for (int k = 0; k < 3; ++k)
                j.checkpoint(32u);
            writeMat(j, pp + 0x80u, world);
            // ApplyMatrixXYZ(*(part +0xF0), world, *(child +0xD0))
            const uint32_t pos = e::read32(j, pp + 0xF0u);
            uint32_t childAt, out, v;
            if ((child & 3u) != 0u || !plainRam(child + 0xD0u, 4u, childAt))
                return false;
            const uint32_t anchor = e::read32(j, childAt);
            if (!plainRam(pos & ~0xFu, 16u, out) || !plainRam(anchor & ~0xFu, 16u, v))
                return false;
            const Vec4 vv = e::read128(j, v);
            const Mat4 w = readMat(j, pp + 0x80u);
            if (!applyXyzInputs(w.r[0], w.r[1], w.r[2], vv))
                return false;
            j.call();
            e::write128(j, out, applyXyz(w.r[0], w.r[1], w.r[2], vv).r);
            // *pos's xyz = target - *pos (vsub.xyz; lwc1 / swc1 at pos itself: aligned here)
            const uint32_t pos2 = e::read32(j, pp + 0xF0u);
            uint32_t tv, pv;
            if ((pos2 & 0xFu) != 0u || !plainRam(e::u32(ctx, e::a2) & ~0xFu, 16u, tv) || !plainRam(pos2, 16u, pv))
                return false;
            const Vec4 t = e::read128(j, tv), p = e::read128(j, pv);
            if (!libvu0::finiteXyz(t) || !libvu0::finiteXyz(p))
                return false;
            uint32_t d[4];
            store(d, sub(t, p));
            for (uint32_t k = 0; k < 3u; ++k)
                e::write32(j, pv + 4u * k, d[k]);
            // InversMatrix(frame, parent world), then MulMatrix(out = part, A = the inverse, B = part +0x80)
            const uint32_t parent = e::read32(j, pp + 0x148u);
            uint32_t in;
            if (!plainRam((parent + 0x80u) & ~0xFu, 64u, in))
                return false;
            const Mat4 pw = readMat(j, in);
            if (!inversRigidInputs(pw))
                return false;
            j.call();
            Mat4 local;
            if (!multiplyStep(local, inversRigid(pw), readMat(j, pp + 0x80u)))
                return false;
            j.call();
            for (int k = 0; k < 3; ++k)
                j.checkpoint(32u);
            writeMat(j, pp, local);
            // func_001501D0(child, objFlags as read now)
            R5900Context c = *ctx;
            innerArgs(c, sp0 - kAimIkFrame, child, e::read32(j, pm + 0x250u), e::u32(&c, e::a2), e::u32(&c, e::a3));
            j.call();
            if (!partWorld(j, &c) || !j.ok)
                return false;
            j.commit();
            returnFrom(ctx);
            return true;
        }

        // ==== M5 batch 3: func_00153158 fused (claude/rung4-motion-map.md "M5 batch 3") =================================
        // caller_scan (ELF + 81 DLLs): 1 site (IK_InverseKinematics 0x152F24), none reads a scratch register after it, no j,
        // no code pointer.
        // ---- func_00153158 (a0 = A, a1 = B, a2 = C: cParts; asm): IK's two-bone solve -----------------------------------
        // A 0x130 frame; the deepest callee chain 0x15ABA0 0xD0 -> 0x15A190 0x80 below it. Steps 1-7 (ikSolveFirst) read
        // only what no guest write has touched yet; then MtxInitRotQuaternion(A +0x80, qa), *(A +0xF0)'s xyz put back from
        // its copy, the second rotation (capVu0Cos's table read again), MtxInitRotQuaternion(B +0x80, qb) and
        // sceVu0ApplyMatrix(*(B +0xF0), A +0x80, *(B +0xD0)), each reading what the earlier ones wrote (a JournalEnv). The
        // debug bit (0x747A2C & 1: a debug byte scales the reach, the draws are stubs) declines.
        constexpr uint32_t kIkSolve = 0x153158u, kIkSolveFrame = 0x130u, kIkSolveWindow = kIkSolveFrame + kLookQuatWindow;
        constexpr uint32_t kIkDebugWord = 0x747A2Cu, kIkXAxis = 0x3BD8A0u;
        template <class Env>
        bool ikSolveNative(Env &env, R5900Context *ctx)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp), aAt = e::u32(ctx, e::a0), bAt = e::u32(ctx, e::a1),
                           cAt = e::u32(ctx, e::a2);
            uint32_t stackLow, pa, pb, pc, dbg, xAxisAt, cosAt;
            if (!env.atEnd || (sp0 & 0xFu) != 0u || ((aAt | bAt) & 0xFu) != 0u || (cAt & 3u) != 0u ||
                !e::vf0Constant(ctx) || !plainRam(sp0 - kIkSolveWindow, kIkSolveWindow, stackLow) ||
                !plainRam(aAt, 0x170u, pa) || !plainRam(bAt, 0x15Cu, pb) || !plainRam(cAt + 0xF0u, 4u, pc) ||
                !plainRam(kIkDebugWord, 4u, dbg) || !plainRam(kIkXAxis, 16u, xAxisAt) || !plainRam(kCosTable, 16u, cosAt))
                return false;
            JournalEnv<Env> j(env, stackLow, kIkSolveWindow);
            if (!j.ok || (e::read32(j, dbg) & 1u) != 0u)
                return false;
            // steps 1-7: every input as the original reads it (the window's own bytes decline: the original's frames)
            IkSolveIn in;
            uint32_t vA, vC;
            const uint32_t posAAt = e::read32(j, pa + 0xF0u), posCAt = e::read32(j, pc);
            if (!plainRam(posAAt & ~0xFu, 16u, vA) || !plainRam(posCAt & ~0xFu, 16u, vC))
                return false;
            in.posA = e::read128(j, vA);
            in.posC = e::read128(j, vC);
            std::memcpy(&in.la, j.at(pa + 0x158u, 4u), 4);
            std::memcpy(&in.lb, j.at(pb + 0x158u, 4u), 4);
            std::memcpy(in.qA, j.at(pa + 0x160u, 16u), 16);
            std::memcpy(in.xAxis, j.at(xAxisAt, 12u), 12);
            const Mat4 wA = readMat(j, pa + 0x80u);
            for (int k = 0; k < 3; ++k)
                in.wA[k] = wA.r[k];
            BlendTables tb;
            if (!blendTables(j, tb))
                return false;
            std::memcpy(in.atan, tb.atan, 36);
            std::memcpy(in.next, tb.next, 12);
            std::memcpy(in.cos, j.at(cosAt, 16u), 16);
            Exact x;
            IkSolveHalf h;
            int calls = 0;
            if (!ikSolveFirst(x, in, h, calls) || !j.ok)
                return false;
            const Mat4 mA = quat2mat(x, h.qa);
            // posA's copy (lqc2 of *(A +0xF0) & ~0xF): the last read before the first write
            const uint32_t copyAt = e::read32(j, pa + 0xF0u);
            uint32_t vCopy;
            if (!x.ok || !plainRam(copyAt & ~0xFu, 16u, vCopy))
                return false;
            uint32_t copy[4];
            std::memcpy(copy, j.at(vCopy, 16u), 16);
            // step 8: A's matrix, then its position's xyz put back (swc1: word-aligned; the frame copy itself, which
            // the original skips, is in the window: declined above)
            writeMat(j, pa + 0x80u, mA);
            const uint32_t restoreAt = e::read32(j, pa + 0xF0u);
            uint32_t vr;
            if ((restoreAt & 3u) != 0u || !plainRam(restoreAt, 12u, vr))
                return false;
            for (uint32_t k = 0; k < 3u; ++k)
                e::write32(j, vr + 4u * k, copy[k]);
            // step 9: B's rotation (capVu0Cos's table as it is now), its matrix, then its position by A's matrix
            float cos2[4], qb[4];
            std::memcpy(cos2, j.at(cosAt, 16u), 16);
            int calls2 = 0;
            ikSolveSecond(x, h, cos2, qb, calls2);
            const Mat4 mB = quat2mat(x, qb);
            if (!x.ok)
                return false;
            writeMat(j, pb + 0x80u, mB);
            const uint32_t outAt = e::read32(j, pb + 0xF0u), vAt = e::read32(j, pb + 0xD0u);
            uint32_t vo, vm, vv;
            if (!plainRam(outAt & ~0xFu, 16u, vo) || !plainRam(aAt + 0x80u, 64u, vm) || !plainRam(vAt & ~0xFu, 16u, vv))
                return false;
            const Mat4 m = readMat(j, vm);
            const Vec4 v = e::read128(j, vv);
            if (!libvu0::finiteInputs(m.r[0]) || !libvu0::finiteInputs(m.r[1]) || !libvu0::finiteInputs(m.r[2]) ||
                !libvu0::finiteInputs(m.r[3]) || !libvu0::finiteInputs(v))
                return false;
            e::write128(j, vo, libvu0::mulRow(m.r[0], m.r[1], m.r[2], m.r[3], v).r);
            if (!j.ok)
                return false;
            for (int k = 0; k < calls + calls2 + 3; ++k) // + MtxInitRotQuaternion x2, sceVu0ApplyMatrix
                j.call();
            j.commit();
            returnFrom(ctx);
            return true;
        }
    }

    void registerFunctions()
    {
        // M1 batch 1: checked (on by default) since K's 15:56 build of 1db9184 shadowed them in every replay: ~76.8M
        // calls, 0 mismatches, 0 declines, 0 skipped (3,705 interrupted), every .ram.bin the reference's.
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *), bool checked) {
            NativeFunction fn{name, address, "motion", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = 0u;
            registerFunction(fn);
        };
        add("Adjust_theta_vec", kAdjustTheta, &adjustThetaVec<RealEnv>, &adjustThetaVec<ShadowEnv>, true);
        add("MtxInitTransVec", 0x1476D0u, &initTransVec<RealEnv>, &initTransVec<ShadowEnv>, true);
        add("MtxInitRotVec", kInitRotVec, &initRotVec<RealEnv>, &initRotVec<ShadowEnv>, true);
        add("MtxInitScaleVec", 0x147A08u, &initScaleVec<RealEnv>, &initScaleVec<ShadowEnv>, true);
        add("MtxMulRotVec", kMulRotVec, &mulRotVec<RealEnv>, &mulRotVec<ShadowEnv>, true);
        add("MtxMulScaleVec", kMulScaleVec, &mulScaleVec<RealEnv>, &mulScaleVec<ShadowEnv>, true);
        // M1 batch 2: MtxMulTransVec, MtxInverse and func_001501D0 checked since K's 17:31 Twice of d8151b0 (0.18M /
        // 0.74M / 10.77M calls compared, 0 mismatches; 0x1501D0 declined 0.72M: the twist, checked since K's 09:27 Twice
        // of 98add9b, below). MtxInitCoord and
        // cParts_calcLocal checked since K's 19:09 Twice of 91dfbd2 (3.29M / 7.50M calls, 0 mismatches, 0 declines,
        // with the read-after rule kTransReadAfter).
        add("MtxMulTransVec", kMulTransVec, &mulTransVec<RealEnv>, &mulTransVec<ShadowEnv>, true);
        add("MtxInitCoord", kInitCoord, &initCoord<RealEnv>, &initCoord<ShadowEnv>, true);
        add("cParts_calcLocal", kCalcLocal, &calcLocal<RealEnv>, &calcLocal<ShadowEnv>, true);
        add("MtxInverse", kInverse, &mtxInverse<RealEnv>, &mtxInverse<ShadowEnv>, true);
        add("func_001501D0", kPartWorld, &partWorld<RealEnv>, &partWorld<ShadowEnv>, true);
        // M1's blend batch: checked since K's 22:04 Twice of 0e44f7a (0x156D70 0.52M, slerp 1.35M, 0x15A190 1.44M,
        // MtxInitRotQuaternion 2.76M calls compared, 0 mismatches, 0 declines).
        add("func_00156D70", kBlend, &blendNative<RealEnv>, &blendNative<ShadowEnv>, true);
        add("func_0015A6A0", kSlerp, &slerpNative<RealEnv>, &slerpNative<ShadowEnv>, true);
        add("func_0015A190", kMatToQuat, &matToQuatNative<RealEnv>, &matToQuatNative<ShadowEnv>, true);
        add("MtxInitRotQuaternion", kQuatToMat, &quatToMatNative<RealEnv>, &quatToMatNative<ShadowEnv>, true);
        // M2 (batch 3): the per-model passes fused; checked since K's 23:57 Twice of 3581925 (calcParts 247.5k, calcWorldParts
        // 56.6k calls compared, 0 mismatches; declined: one model a frame in each, a twist part), live since 025d04b's
        // builds. Their footprints (a model and all its parts) are recorded with Tag_set_scr's limits.
        // The twist (partFlags 0x800) runs in all three since K's 09:27 Twice of 98add9b shadowed it in each over the live
        // native (func_001501D0 6.27M, calcParts 337k, calcWorldParts 144k calls compared, 0 mismatches, 0 declines).
        auto addPass = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                          bool (*shadow)(ShadowEnv &, R5900Context *), bool checked) {
            NativeFunction fn{name, address, "motion", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = 0u;
            fn.maxBytes = 1u << 20;
            fn.maxRanges = 16384u;
            registerFunction(fn);
        };
        addPass("cModel_calcParts", kCalcParts, &calcPartsNative<RealEnv>, &calcPartsNative<ShadowEnv>, true);
        addPass("cModel_calcWorldParts", kCalcWorldParts, &calcWorldPartsNative<RealEnv>, &calcWorldPartsNative<ShadowEnv>,
                true);
        // M3 (batch 1, Nov 26): the Hermite track and its half-float unpacks (f0 is the result of two of them); checked since
        // K's 12:51 Build and Replay of dc69900 (7.87M / 3.52M / 2.03M calls compared, 0 mismatches, 0 declines).
        auto addF = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                       bool (*shadow)(ShadowEnv &, R5900Context *), bool checked, uint32_t returns) {
            NativeFunction fn{name, address, "motion", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = returns;
            registerFunction(fn);
        };
        addF("func_00156508", kHalf, &halfNative<RealEnv>, &halfNative<ShadowEnv>, true, NativeFunction::kReturnsF0);
        addF("func_00156550", kHalf4, &half4Native<RealEnv>, &half4Native<ShadowEnv>, true, 0u);
        addF("func_00158CD0", kTrack, &trackNative<RealEnv>, &trackNative<ShadowEnv>, true, NativeFunction::kReturnsF0);
        // M3 (batch 2, Nov 26): the root channels' decode (a walk reads one key at a time: up to kMaxSteps ranges); checked
        // since K's 16:08 Build and Replay of e0de538 (0x159AD0 17.93M, 0x159DA0 1.91M, readHeader 0.21M calls compared,
        // 0 mismatches, 0 declines).
        auto addDecode = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                            bool (*shadow)(ShadowEnv &, R5900Context *), bool checked, uint32_t returns) {
            NativeFunction fn{name, address, "motion", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = returns;
            fn.maxRanges = 4096u + 16u;
            registerFunction(fn);
        };
        addDecode("func_00159DA0", kDecodeKeys, &decodeKeysNative<RealEnv>, &decodeKeysNative<ShadowEnv>, true, 0u);
        addDecode("RecDecode_readHeader", kReadHeader, &readHeaderNative<RealEnv>, &readHeaderNative<ShadowEnv>, true, 0u);
        addDecode("func_00159AD0", kDecode, &decodeNative<RealEnv>, &decodeNative<ShadowEnv>, true,
                  NativeFunction::kReturnsF0);
        // M4 (Nov 26): the pose fused (the decode, the Hermite, kind 3, kinds 0 / 2 / 6 and Adjust_theta_vec inline) and
        // kind 3 on its own. The pose checked since K's 19:28 Build and Replay of 7e82f46 (283.6k calls, 283.1k compared,
        // 0 mismatches, 429 declines; its kind-3 path proven in the sandbox only, as M3's decode kinds 3 / 5). Kind 3 alone
        // was never called there (no replay reaches a kind-3 record): it stays shadowed. The pose sits under the features' three wrappers: its slot
        // keeps FunctionHooks' trampoline from boot (native.cpp kUnderHooks), so they call it as their inner function,
        // its observers first. Its footprint (a model, its parts, their tracks) is recorded with a larger limit.
        addF("func_00158FC8", kKind3, &kind3Native<RealEnv>, &kind3Native<ShadowEnv>, false, NativeFunction::kReturnsF0);
        {
            NativeFunction fn{"func_00158328", kPose, "motion", true, &poseNative<RealEnv>, &poseNative<ShadowEnv>, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = 0u;
            fn.maxBytes = 1u << 22;
            fn.maxRanges = 65536u;
            registerFunction(fn);
        }
        // M5 batch 1 (Nov 26): matrix to Euler 0x147C88 (66 callers: IK and the moves) with its helpers inline, and the
        // helpers as natives of their own; checked since K's 23:17 Build and Replay of 368d3b6 (atan2 11.63M, acos 3.96M,
        // the asinf wrapper 1.12M, MtxGetScaleVec 1.20M, 0x147C88 1.12M calls compared, 0 mismatches; acos 2,890 declines).
        addF("capVu0Atan2", kAtan2, &atan2Native<RealEnv>, &atan2Native<ShadowEnv>, true, NativeFunction::kReturnsF0);
        addF("capVu0Acos", kAcos, &acosNative<RealEnv>, &acosNative<ShadowEnv>, true, NativeFunction::kReturnsF0);
        addF("func_003A1B78", kAsinf, &asinfNative<RealEnv>, &asinfNative<ShadowEnv>, true, NativeFunction::kReturnsF0);
        addF("MtxGetScaleVec", kScaleVec, &scaleVecNative<RealEnv>, &scaleVecNative<ShadowEnv>, true, 0u);
        addF("func_00147C88", kMatToEuler, &matToEulerNative<RealEnv>, &matToEulerNative<ShadowEnv>, true, 0u);
        // M5 batch 2 (Nov 27): the IK leaves: cModel_aimIkPart fused (MulMatrix x2, InversMatrix, ApplyMatrixXYZ,
        // func_001501D0 inline), capVu0ApplyMatrixXYZ, Quat_rotatePoint (returns v0 = out), func_0015ABA0; checked since K's
        // 09:56 Build and Replay of f532603 (ApplyMatrixXYZ 2.73M, Quat_rotatePoint 0.32M, aimIkPart 0.26M, 0x15ABA0 0.26M
        // calls compared, 0 mismatches, 0 declines).
        addF("capVu0ApplyMatrixXYZ", kApplyXyz, &applyXyzNative<RealEnv>, &applyXyzNative<ShadowEnv>, true, 0u);
        addF("Quat_rotatePoint", kQuatRotate, &quatRotateNative<RealEnv>, &quatRotateNative<ShadowEnv>, true,
             NativeFunction::kReturnsV0);
        addF("func_0015ABA0", kLookQuat, &lookQuatNative<RealEnv>, &lookQuatNative<ShadowEnv>, true, 0u);
        addF("cModel_aimIkPart", kAimIkPart, &aimIkPartNative<RealEnv>, &aimIkPartNative<ShadowEnv>, true, 0u);
        // M5 batch 3 (Nov 27): func_00153158 fused (IK's two-bone solve: capVu0LengthSq, capVu0Acos x2, Quat_rotatePoint,
        // capVu0ApplyMatrixXYZ, the thunk + capVu0Length x2, func_0015ABA0, SetRotationAxis x2, MtxInitRotQuaternion x2,
        // sceVu0ApplyMatrix inline). Unchecked: shadowed in dev replays.
        addF("func_00153158", kIkSolve, &ikSolveNative<RealEnv>, &ikSolveNative<ShadowEnv>, false, 0u);
    }
}
