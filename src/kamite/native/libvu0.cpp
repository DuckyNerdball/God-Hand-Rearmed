// Native libvu0 helpers (Sony's VU0 macro-mode matrix library, linked into the game): the rung 2 pilot
// (claude/native-next.md). Leaf functions, called everywhere (rung 1: libvu0 4.9% of the game thread's busy samples, own
// time), and part of rung 3's packet building. Switch: KAMITE_NATIVE_LIBVU0 (unset = the checked ones, 1 = all, 0 =
// none); until checked they run in the shadow harness during dev replays only.
//
// Each run<Env>() is the recompiled function's effect (SLUS_215.03, generated/sceVu0*.cpp) without the per-op
// bookkeeping: same loads, same ops in the same order (libvu0_core.h), same stores, same registers left behind (the
// VU0 temporaries vf4-vf9 and ACC, the argument registers it steps, a3 as its loop counter), the same checkpoints.

#include "kamite/native/native.h"
#include "kamite/native/libvu0_core.h"

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

        template <class Env>
        bool mulMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5), a2 = gpr32(ctx, 6);
            uint32_t out, ma, mb;
            if (!plainRam(a0 & ~0xFu, 64u, out) || !plainRam(a1 & ~0xFu, 64u, ma) || !plainRam(a2 & ~0xFu, 64u, mb))
                return false;
            // The original reads B row k after writing out row k - 1: an out that overlaps B (but is not B) would feed
            // its own rows back; it never happens in the game's calls, so decline it rather than mimic it.
            if (overlapsUnequal(out, mb, 64u) || addiOverflows(a0, 64u) || addiOverflows(a2, 64u))
                return false;
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            if (!finiteInputs(m0) || !finiteInputs(m1) || !finiteInputs(m2) || !finiteInputs(m3) ||
                !finiteInputs(load(ram + mb)) || !finiteInputs(load(ram + mb + 16)) || !finiteInputs(load(ram + mb + 32)) ||
                !finiteInputs(load(ram + mb + 48)))
                return false;
            setVf(ctx, 4, m0);
            setVf(ctx, 5, m1);
            setVf(ctx, 6, m2);
            setVf(ctx, 7, m3);
            for (uint32_t k = 0;; ++k)
            {
                const Vec4 b = load(ram + mb + 16u * k); // out == B: row k is still B's (row k - 1 went before it)
                const RowResult row = mulRow(m0, m1, m2, m3, b);
                store(ram + out + 16u * k, row.r);
                if (k == 3u || env.checkpoint(32u))
                {
                    setVf(ctx, 8, b);
                    store(&ctx->vu0_acc, row.acc);
                    setVf(ctx, 9, row.r);
                    setGprS32(ctx, 7, 3u - k);
                    setGprS32(ctx, 6, a2 + 16u * (k + 1u));
                    setGprS32(ctx, 4, a0 + 16u * (k + 1u));
                    ctx->pc = k == 3u ? gpr32(ctx, 31) : kMulMatrixLoop;
                    return true;
                }
            }
        }

        void mulMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 64u);
        }

        // ---- sceVu0ApplyMatrix 0x30A2B0 (a0 = out vector, a1 = matrix, a2 = vector) --------------------------------
        constexpr uint32_t kApplyMatrix = 0x30A2B0u;

        template <class Env>
        bool applyMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5), a2 = gpr32(ctx, 6);
            uint32_t out, ma, vb;
            if (!plainRam(a0 & ~0xFu, 16u, out) || !plainRam(a1 & ~0xFu, 64u, ma) || !plainRam(a2 & ~0xFu, 16u, vb))
                return false;
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            const Vec4 b = load(ram + vb);
            if (!finiteInputs(m0) || !finiteInputs(m1) || !finiteInputs(m2) || !finiteInputs(m3) || !finiteInputs(b))
                return false;
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
            return true;
        }

        void applyMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 16u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 16u);
        }
    }

    void registerFunctions()
    {
        registerFunction(NativeFunction{"sceVu0MulMatrix", kMulMatrix, "libvu0", false, &mulMatrix<RealEnv>,
                                        &mulMatrix<ShadowEnv>, &mulMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0ApplyMatrix", kApplyMatrix, "libvu0", false, &applyMatrix<RealEnv>,
                                        &applyMatrix<ShadowEnv>, &applyMatrixFootprint});
    }
}
