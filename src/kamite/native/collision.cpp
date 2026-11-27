// Native collision functions (rung 4, claude/rung4-collision-map.md): ChkLine's tree from the leaves up (K, Nov 27:
// C1 the probes' leaves, C2 the grid, C3 the solids and func_0012ED60, C4 ChkLine). Switch: KAMITE_NATIVE_COLLISION
// (unset = the checked ones, 1 = all, 0 = none); a native that is not checked yet runs in the shadow harness during dev
// replays only.
//
// Every native here is kRenderPath | kChargeAtEnd (rung 4's rules, rung4-motion-map.md "Grilled" 8 and 9): the harness
// compares what lasts (RAM outside the dead stack, the return value, the callee-saved registers, pc, in_delay_slot, the
// EE cycles); temporaries are don't-care. Allowed per batch by the caller scan (tools/native/caller_scan.py over the ELF
// and the 81 room DLLs: no caller reads a scratch register after any of them). Each reads and checks everything first,
// computes, then writes (a decline changes nothing). A function with a frame declines any pointer into its frames' window
// (the original's saves would be read or overwritten).

#include "kamite/native/native.h"
#include "kamite/native/collision_core.h"
#include "kamite/native/ee_ctx.h"

#include <cstdio>
#include <type_traits>

namespace kamite::native::collision
{
    namespace
    {
        namespace e = kamite::native::ee;

        // jr ra with a real delay slot, or a path through one (the generated code clears in_delay_slot after it)
        void returnFrom(R5900Context *ctx)
        {
            ctx->pc = e::u32(ctx, e::ra);
            ctx->in_delay_slot = false;
        }
        // lqc2 / sqc2: the quadword at address & ~0xF, plain RAM
        bool plain16(uint32_t address, uint32_t &offset) { return plainRam(address & ~0xFu, 16u, offset); }
        // A function's frames: sp 16-aligned, [sp - window, sp) plain RAM (offset `low`)
        bool frameWindow(const R5900Context *ctx, uint32_t window, uint32_t &low)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp);
            return (sp0 & 0xFu) == 0u && plainRam(sp0 - window, window, low);
        }
        // An output written by swc1 x, z, y (words): 0 (none), or word-aligned plain RAM clear of the window
        bool outWords(uint32_t address, uint32_t low, uint32_t window, uint32_t &offset)
        {
            if (address == 0u)
                return true;
            return (address & 3u) == 0u && plainRam(address, 12u, offset) && !e::overlaps(offset, 12u, low, window);
        }
        // Dev builds: why a C1 native declined in play, the first 12 of each reason per function and run, inputs included
        // (claude/strip-list.md: K's 21:48 Build and Replay of 3151fdc had 564 HitProbe / 0x135EC8 declines, all in
        // the two roulette cases). why: 0 an entry check, 1-3 PlaneHit::why, 11-12 QuadInside::why + 10.
        enum DeclineFn
        {
            kNotePlane,
            kNoteQuad,
            kNoteProbe
        };
        template <class Env>
        void noteDecline(DeclineFn fn, int why, const Vec4 *v, int n)
        {
#if !defined(KAMITE_RELEASE)
            if constexpr (std::is_same_v<Env, RealEnv>)
            {
                static int seen[3][16];
                if (why < 0 || why > 15 || ++seen[fn][why] > 12)
                    return;
                static const char *const names[3] = {"func_00135EC8", "func_001361A0", "HitProbe_probeHitPoint"};
                std::fprintf(stderr, "[collision] %s declined, why %d:", names[fn], why);
                for (int k = 0; k < n; ++k)
                    std::fprintf(stderr, " (%08X %08X %08X %08X)", laneBits<0>(v[k]), laneBits<1>(v[k]), laneBits<2>(v[k]),
                                 laneBits<3>(v[k]));
                std::fprintf(stderr, "\n");
            }
#else
            (void)fn, (void)why, (void)v, (void)n;
#endif
        }
        template <class Env>
        void writeXzy(Env &env, uint32_t offset, Vec4 p)
        {
            e::write32(env, offset, laneBits<0>(p));
            e::write32(env, offset + 8u, laneBits<2>(p));
            e::write32(env, offset + 4u, laneBits<1>(p));
        }

        // ==== C1: the probes' leaves (claude/rung4-collision-map.md "C1") =================================================
        // caller_scan (ELF + 81 DLLs): 0x133948 2, 0x133408 5, 0x135EC8 3, 0x1361A0 1, HitProbe 4 sites; none reads a
        // scratch register after the call, no j, no code pointer. sceVu0InnerProduct is libvu0.cpp's (whole context).

        // ---- func_00133948 (a0 unused, a1, a2; src/cod/00133948.c; a leaf, jr ra + nop): a1 = vmini, a2 = vmax (xyzw) ----
        constexpr uint32_t kBoxSort = 0x133948u;
        template <class Env>
        bool boxSortNative(Env &env, R5900Context *ctx)
        {
            uint32_t a, b;
            if (!env.atEnd || !plain16(e::u32(ctx, e::a1), a) || !plain16(e::u32(ctx, e::a2), b))
                return false;
            const Vec4 va = e::read128(env, a), vb = e::read128(env, b);
            e::write128(env, a, vuMin(va, vb));
            e::write128(env, b, vuMax(va, vb)); // a1 == a2: the max, written last
            ctx->pc = e::u32(ctx, e::ra);       // a nop delay slot: in_delay_slot as it was
            return true;
        }

        // ---- func_00133408 (a0 unused, a1 = amin, a2 = amax, a3 = bmin, t0 = bmax; src/cod/00133408.c): v0 = overlap -----
        // lwc1 throughout (word-aligned, 12 bytes each). Every exit passes a real delay slot.
        constexpr uint32_t kOverlap = 0x133408u;
        template <class Env>
        bool overlapNative(Env &env, R5900Context *ctx)
        {
            const uint32_t p[4] = {e::u32(ctx, e::a1), e::u32(ctx, e::a2), e::u32(ctx, e::a3), e::u32(ctx, e::t0)};
            float v[4][3];
            if (!env.atEnd)
                return false;
            for (int k = 0; k < 4; ++k)
            {
                uint32_t o;
                if ((p[k] & 3u) != 0u || !plainRam(p[k], 12u, o))
                    return false;
                for (int i = 0; i < 3; ++i)
                    v[k][i] = bitsFloat(e::read32(env, o + 4u * i));
            }
            int loops = 0;
            const bool hit = boxesOverlap(v[0], v[1], v[2], v[3], loops);
            if (loops)
                env.checkpoint(32u);
            e::setS32(ctx, e::v0, hit ? 1u : 0u);
            returnFrom(ctx);
            return true;
        }

        // ---- func_00135EC8 (a0 = start, a1 = end, a2 = normal, a3 = a point of the plane, t0 = out; src/cod/00135ec8.c) ----
        // A 0xB0 frame (s0-s3, ra, f20 saved first); start / end / normal / point by lqc2 (& ~0xF), out by swc1 x, z, y.
        // Calls: Normalize, InnerProduct x3 (leaves: no frames of their own).
        constexpr uint32_t kPlaneHit = 0x135EC8u, kPlaneHitFrame = 0xB0u;
        template <class Env>
        bool planeHitNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, s, en, n, pt, out = 0;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kPlaneHitFrame, low) ||
                !plain16(e::u32(ctx, e::a0), s) || !plain16(e::u32(ctx, e::a1), en) || !plain16(e::u32(ctx, e::a2), n) ||
                !plain16(e::u32(ctx, e::a3), pt) || !outWords(e::u32(ctx, e::t0), low, kPlaneHitFrame, out))
            {
                noteDecline<Env>(kNotePlane, 0, nullptr, 0);
                return false;
            }
            for (uint32_t o : {s, en, n, pt})
                if (e::overlaps(o, 16u, low, kPlaneHitFrame))
                {
                    noteDecline<Env>(kNotePlane, 0, nullptr, 0);
                    return false;
                }
            const Vec4 in[4] = {e::read128(env, s), e::read128(env, en), e::read128(env, n), e::read128(env, pt)};
            const PlaneHit h = planeHit(e::vf(ctx, 0), in[0], in[1], in[2], in[3], ctx->vu0_status);
            if (!h.ok)
            {
                noteDecline<Env>(kNotePlane, h.why, in, 4);
                return false;
            }
            for (int k = 0; k < 4; ++k)
                env.call();
            if (h.hit && e::u32(ctx, e::t0) != 0u)
                writeXzy(env, out, h.point);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(h.hit));
            returnFrom(ctx);
            return true;
        }

        // ---- func_001361A0 (a0 = the quad: four quadwords, a1 = the point; asm): v0 = inside ------------------------------
        // A 0x150 frame (s0-s7, fp, ra, f20 saved first; the vf0 clear loop: one backward branch). The quad's rows and the
        // point by lqc2. Calls: Normalize x6, OuterProduct, then OuterProduct + InnerProduct per edge tested.
        constexpr uint32_t kQuadInside = 0x1361A0u, kQuadInsideFrame = 0x150u;
        template <class Env>
        bool quadInsideNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, quad, pt;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kQuadInsideFrame, low) ||
                !plainRam(e::u32(ctx, e::a0) & ~0xFu, 64u, quad) || !plain16(e::u32(ctx, e::a1), pt) ||
                e::overlaps(quad, 64u, low, kQuadInsideFrame) || e::overlaps(pt, 16u, low, kQuadInsideFrame))
            {
                noteDecline<Env>(kNoteQuad, 0, nullptr, 0);
                return false;
            }
            Vec4 v[5];
            for (uint32_t k = 0; k < 4u; ++k)
                v[k] = e::read128(env, quad + 16u * k);
            v[4] = e::read128(env, pt);
            const QuadInside q = quadInside(e::vf(ctx, 0), v, v[4], ctx->vu0_status);
            if (!q.ok)
            {
                noteDecline<Env>(kNoteQuad, 10 + q.why, v, 5);
                return false;
            }
            env.checkpoint(32u);
            for (int k = 0; k < q.calls; ++k)
                env.call();
            e::setS32(ctx, e::v0, static_cast<uint32_t>(q.inside));
            returnFrom(ctx);
            return true;
        }

        // ---- HitProbe_probeHitPoint (a0 = start, a1 = end, a2 = the quad, a3 = its normal, t0 = out; src/cod/001365b8.c) --
        // A 0x40 frame: hit = vf0 at sp, func_00135EC8(start, end, normal, quad row 0, hit), then func_001361A0(quad, hit)
        // (the deeper frame: 0x150), out = hit (x, z, y) when both say so. v0 = 1 / 0.
        constexpr uint32_t kHitProbe = 0x1365B8u, kHitProbeWindow = 0x40u + kQuadInsideFrame;
        template <class Env>
        bool hitProbeNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, s, en, n, quad, out = 0;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kHitProbeWindow, low) ||
                !plain16(e::u32(ctx, e::a0), s) || !plain16(e::u32(ctx, e::a1), en) || !plain16(e::u32(ctx, e::a3), n) ||
                !plainRam(e::u32(ctx, e::a2) & ~0xFu, 64u, quad) ||
                !outWords(e::u32(ctx, e::t0), low, kHitProbeWindow, out) || e::overlaps(quad, 64u, low, kHitProbeWindow))
            {
                noteDecline<Env>(kNoteProbe, 0, nullptr, 0);
                return false;
            }
            for (uint32_t o : {s, en, n})
                if (e::overlaps(o, 16u, low, kHitProbeWindow))
                {
                    noteDecline<Env>(kNoteProbe, 0, nullptr, 0);
                    return false;
                }
            const Vec4 vf0 = e::vf(ctx, 0);
            Vec4 v[4];
            for (uint32_t k = 0; k < 4u; ++k)
                v[k] = e::read128(env, quad + 16u * k);
            const Vec4 in[7] = {e::read128(env, s), e::read128(env, en), e::read128(env, n), v[0], v[1], v[2], v[3]};
            const PlaneHit h = planeHit(vf0, in[0], in[1], in[2], v[0], ctx->vu0_status);
            if (!h.ok)
            {
                noteDecline<Env>(kNoteProbe, h.why, in, 7);
                return false;
            }
            int inside = 0, calls = 1 + 4;
            if (h.hit)
            {
                // the hit buffer: vf0 (w = 1.0), x / z / y from 0x135EC8
                const QuadInside q = quadInside(vf0, v, blend<7>(vf0, h.point), h.status);
                if (!q.ok)
                {
                    noteDecline<Env>(kNoteProbe, 10 + q.why, in, 7);
                    return false;
                }
                env.checkpoint(32u);
                calls += 1 + q.calls;
                inside = q.inside;
            }
            for (int k = 0; k < calls; ++k)
                env.call();
            if (inside && e::u32(ctx, e::t0) != 0u)
                writeXzy(env, out, h.point);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(inside));
            returnFrom(ctx);
            return true;
        }
    }

    void registerFunctions()
    {
        // C1 (Nov 27): the probes' leaves; checked since K's 21:48 Build and Replay of 3151fdc shadowed them (0x133408
        // 15.22M, 0x133948 10.96M, 0x135EC8 2.56M, HitProbe 2.29M, 0x1361A0 1.61M calls compared, 0 mismatches;
        // 0x135EC8 / HitProbe 564 declines each, the roulette cases: noteDecline logs their reasons).
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *), bool checked, uint32_t returns) {
            NativeFunction fn{name, address, "collision", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = returns;
            registerFunction(fn);
        };
        add("func_00133948", kBoxSort, &boxSortNative<RealEnv>, &boxSortNative<ShadowEnv>, true, 0u);
        add("func_00133408", kOverlap, &overlapNative<RealEnv>, &overlapNative<ShadowEnv>, true, NativeFunction::kReturnsV0);
        add("func_00135EC8", kPlaneHit, &planeHitNative<RealEnv>, &planeHitNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("func_001361A0", kQuadInside, &quadInsideNative<RealEnv>, &quadInsideNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("HitProbe_probeHitPoint", kHitProbe, &hitProbeNative<RealEnv>, &hitProbeNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
    }
}
