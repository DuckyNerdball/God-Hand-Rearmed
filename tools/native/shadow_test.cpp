// Host-side checks of the native EE functions and the shadow harness (claude/native-next.md rung 2), against the
// recompiled functions generated from K's SLUS_215.03 (generated/ is not in the repo: build it first, README.md).
//
//   1. natives:    each native against its recompiled function on random game-like inputs, every checkpoint pattern
//                  (none due, due at each), aliasing (out = an input); context, RAM and EE cycles must be identical, and
//                  the natives may only decline the inputs they are meant to (special values, huge values).
//   2. footprints: every byte the recompiled function changes lies in the native's declared footprint.
//   3. invisible:  a call through the shadow harness leaves context, RAM, cycles and checkpoint calls exactly as the
//                  recompiled function alone does, early exits included; every call compared, 0 mismatches.
//   4. detection:  a wrong native (one bit of one result, a register, the cycles) is caught and saved; a loose native
//                  passes within its ulps and fails with 0.
//   5. vectors:    writes libvu0 test vectors for libvu0_check (plain C++ and NEON under qemu).
//   "bench": ns per call, recompiled vs native (host compiler, not MSVC: a hint, K's replays are the measure).
//   "rerun <case>.<rate>.native.bin": each saved mismatch's input through the native and the recompiled function
//   again, here; prints where they differ (and whether this host agrees with what the game saw).
#include "kamite/native/native.h"
#include "kamite/native/shadow.h"
#include "kamite/native/libvu0_core.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <type_traits>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <xmmintrin.h>

// The recompiled functions (generated/*.cpp, built alongside).
void sceVu0MulMatrix_0x30a2e0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sceVu0ApplyMatrix_0x30a2b0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

// ---- the runtime pieces the generated code and the harness call ----------------------------------------------------
namespace
{
    int g_checkpointCalls = 0; // eeCheckpointDue calls so far
    int g_dueAt = 0;           // the call that comes due (1-based), 0 = none
    uint64_t g_charged = 0;    // EE cycles charged
    bool g_stopRequested = false;
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    g_charged += cycles == 32u ? 32u : cycles;
    return ++g_checkpointCalls == g_dueAt;
}
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t)
{
    std::fprintf(stderr, "unexpected Load128 (special address)\n");
    std::abort();
}
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t, __m128i)
{
    std::fprintf(stderr, "unexpected Store128 (special address)\n");
    std::abort();
}
void PS2Runtime::SignalException(R5900Context *, PS2Exception)
{
    std::fprintf(stderr, "unexpected SignalException\n");
    std::abort();
}
void PS2Runtime::requestStop() { g_stopRequested = true; }

// native.cpp is not linked: collect the registrations here.
static std::vector<kamite::native::NativeFunction> g_natives;
void kamite::native::registerFunction(const NativeFunction &function) { g_natives.push_back(function); }

namespace
{
    using kamite::native::NativeFunction;
    using Recompiled = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    int g_failures = 0;
    void fail(const char *what)
    {
        if (++g_failures <= 20)
            std::printf("FAIL: %s\n", what);
    }

    // Two 128 MB backings (lazily committed) like the runtime's; tests use the first 1 MB.
    constexpr uint32_t kUsed = 1u << 20;
    uint8_t *backing()
    {
        void *p = mmap(nullptr, PS2_RDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return static_cast<uint8_t *>(p);
    }
    uint8_t *g_ramA = backing(), *g_ramB = backing();
    alignas(64) unsigned char g_runtimeStorage[64]; // never dereferenced by the stubs
    PS2Runtime *const g_rt = reinterpret_cast<PS2Runtime *>(g_runtimeStorage);

    std::mt19937_64 g_rng(0x6748524E61746976ull);

    uint32_t gameFloat()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 100u);
        const uint32_t sign = (g_rng() & 1u) << 31;
        float f;
        if (pick < 55)
            f = std::ldexp(static_cast<float>(g_rng() % 1000000u) / 1000000.0f + 0.5f, static_cast<int>(g_rng() % 24u) - 12);
        else if (pick < 70)
            f = static_cast<float>(static_cast<int>(g_rng() % 2001u) - 1000) / 8.0f; // round numbers, 0
        else if (pick < 78)
            f = (pick & 1) ? 1.0f : 0.0f;
        else if (pick < 85)
        {
            const uint32_t bits = static_cast<uint32_t>(g_rng()) & 0x007FFFFFu; // a denormal (or 0)
            return sign | bits;
        }
        else if (pick < 92)
            f = std::ldexp(1.0f + static_cast<float>(g_rng() % 1000u) / 1000.0f, static_cast<int>(g_rng() % 40u) + 20);
        else if (pick < 96)
            return sign | (0x5E7FFFF0u + static_cast<uint32_t>(g_rng() % 0x20u)); // either side of the 2^62 limit
        else if (pick < 98)
            return sign | 0x7F7FFFFFu; // FLT_MAX
        else
            return sign | (0x7F800000u | (static_cast<uint32_t>(g_rng()) & 0x7FFFFFu)); // exponent 255
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits | sign;
    }
    // Mostly ordinary values (what the game passes), sometimes the specials above.
    uint32_t inputFloat(bool wild)
    {
        if (wild)
            return gameFloat();
        float f = std::ldexp(static_cast<float>(g_rng() % 2000001u) / 1000000.0f - 1.0f, static_cast<int>(g_rng() % 12u) - 4);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return bits;
    }

    void randomContext(R5900Context &ctx)
    {
        auto *bytes = reinterpret_cast<uint8_t *>(&ctx);
        for (size_t i = 0; i < sizeof(ctx); ++i)
            bytes[i] = static_cast<uint8_t>(g_rng());
        std::memset(&ctx.r[0], 0, 16);
    }
    void setGpr(R5900Context &ctx, int reg, uint32_t value)
    {
        const int64_t wide = static_cast<int32_t>(value);
        std::memcpy(&ctx.r[reg], &wide, 8); // upper doubleword: random, as a real register may hold
    }
    void fillFloats(uint8_t *ram, uint32_t address, int count, bool wild)
    {
        for (int i = 0; i < count; ++i)
        {
            const uint32_t v = inputFloat(wild);
            std::memcpy(ram + address + 4u * i, &v, 4);
        }
    }

    // One call's setup: argument registers and RAM contents, the same in both backings.
    struct Call
    {
        R5900Context ctx;
        int dueAt;
    };

    // Picks the argument addresses for a function: a0 = out, a1 = matrix, a2 = matrix / vector; sometimes aliased.
    void setupCall(const NativeFunction &fn, Call &call, bool wild)
    {
        randomContext(call.ctx);
        call.ctx.pc = fn.address;
        const uint32_t base = 0x10000u + 0x400u * static_cast<uint32_t>(g_rng() % 64u);
        uint32_t a0 = base, a1 = base + 0x100u, a2 = base + 0x200u;
        const uint32_t alias = static_cast<uint32_t>(g_rng() % 10u);
        if (alias == 0)
            a0 = a2; // out = B (MulMatrix in place on its right operand)
        else if (alias == 1)
            a0 = a1; // out = A
        else if (alias == 2)
            a0 = a2 + 0x10u; // out overlapping B unequally: the native declines MulMatrix
        // KSEG0 / uncached mirrors of the same RAM, as game pointers may be.
        const uint32_t mirror = static_cast<uint32_t>(g_rng() % 4u);
        const uint32_t seg = mirror == 0 ? 0x80000000u : mirror == 1 ? 0x20000000u : 0u;
        setGpr(call.ctx, 4, a0 | seg);
        setGpr(call.ctx, 5, a1 | seg);
        setGpr(call.ctx, 6, a2 | seg);
        setGpr(call.ctx, 31, 0x00123450u);
        std::memset(g_ramA + base, 0, 0x400);
        fillFloats(g_ramA, base, 256, wild);
        std::memcpy(g_ramB + base, g_ramA + base, 0x400);
        call.dueAt = static_cast<int>(g_rng() % 5u); // 0 = none, else the checkpoint that comes due
    }

    // Runs `run` from the call's state on `ram`; returns the context after, the cycles and checkpoint calls.
    struct Outcome
    {
        R5900Context ctx;
        uint64_t charged;
        int checkpointCalls;
    };
    template <class Run>
    Outcome runFrom(const Call &call, Run run)
    {
        Outcome out;
        std::memcpy(static_cast<void *>(&out.ctx), &call.ctx, sizeof(R5900Context));
        g_checkpointCalls = 0;
        g_dueAt = call.dueAt;
        g_charged = 0;
        run(&out.ctx);
        out.charged = g_charged;
        out.checkpointCalls = g_checkpointCalls;
        return out;
    }

    bool sameOutcome(const Outcome &a, const Outcome &b)
    {
        return std::memcmp(&a.ctx, &b.ctx, sizeof(R5900Context)) == 0 && a.charged == b.charged &&
               a.checkpointCalls == b.checkpointCalls && std::memcmp(g_ramA, g_ramB, kUsed) == 0;
    }

    void resetRam() { std::memset(g_ramA, 0, kUsed), std::memset(g_ramB, 0, kUsed); }

    // ---- 1 + 2: natives against the recompiled functions; footprints ---------------------------------------------
    void checkNative(const NativeFunction &fn, Recompiled recompiled, int calls)
    {
        int ran = 0, declined = 0, declinedOrdinary = 0, early = 0;
        std::vector<uint8_t> before(kUsed);
        for (int i = 0; i < calls; ++i)
        {
            Call call;
            const bool wild = (i % 4) == 3;
            setupCall(fn, call, wild);
            std::memcpy(before.data(), g_ramA, kUsed);
            const Outcome original = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            bool accepted = false;
            const Outcome native = runFrom(call, [&](R5900Context *ctx) {
                kamite::native::RealEnv env{g_ramB, g_rt};
                accepted = fn.runReal(env, ctx);
            });
            // footprint: everything the original changed is declared
            kamite::native::Footprint fp;
            fn.footprint(&call.ctx, fp);
            for (uint32_t at = 0; at < kUsed; ++at)
                if (g_ramA[at] != before[at])
                {
                    bool inside = false;
                    for (int r = 0; r < fp.count; ++r)
                        inside |= at - (fp.ranges[r].address & PS2_RAM_MASK) < fp.ranges[r].size;
                    if (!inside)
                    {
                        fail("the original wrote outside the footprint");
                        break;
                    }
                }
            if (!accepted)
            {
                ++declined;
                // a declined call changed nothing
                if (std::memcmp(&native.ctx, &call.ctx, sizeof(R5900Context)) != 0 || std::memcmp(before.data(), g_ramB, kUsed) != 0 ||
                    native.checkpointCalls != 0)
                    fail("a declined call changed something");
                if (!wild)
                    ++declinedOrdinary;
                std::memcpy(g_ramB, g_ramA, kUsed); // back in step
                continue;
            }
            ++ran;
            early += original.ctx.pc != 0x00123450u;
            if (!sameOutcome(original, native))
            {
                char text[160];
                std::snprintf(text, sizeof(text), "%s call %d: native differs from the recompiled function (due at %d)",
                              fn.name, i, call.dueAt);
                fail(text);
                std::memcpy(g_ramB, g_ramA, kUsed);
            }
        }
        std::printf("%-20s %d calls: %d native (%d stopped at a checkpoint), %d declined (%d with ordinary inputs: "
                    "aliasing)\n", fn.name, calls, ran, early, declined, declinedOrdinary);
        resetRam();
    }

    // ---- 3: the harness is invisible -------------------------------------------------------------------------------
    uint64_t testCycles() { return g_charged; }
    std::string g_output = "/tmp/native_shadow_test/run";
    std::string testOutput() { return g_output; }
    std::string testHeader() { return "test"; }

    void checkInvisible(const NativeFunction &fn, Recompiled recompiled, int calls)
    {
        const kamite::native::shadow::Stats *stats = kamite::native::shadow::stats(fn.address);
        const uint64_t mismatchesBefore = stats->mismatches, checkedBefore = stats->checked;
        for (int i = 0; i < calls; ++i)
        {
            Call call;
            setupCall(fn, call, (i % 4) == 3);
            const Outcome alone = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            const Outcome shadowed = runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
            if (!sameOutcome(alone, shadowed))
            {
                fail("the shadow harness changed the call's outcome");
                std::memcpy(g_ramB, g_ramA, kUsed);
            }
        }
        std::printf("%-20s shadowed %d calls: checked %llu, matched %llu, declined %llu, skipped %llu, early exits %llu, "
                    "mismatches %llu\n", fn.name, calls, static_cast<unsigned long long>(stats->checked - checkedBefore),
                    static_cast<unsigned long long>(stats->matched), static_cast<unsigned long long>(stats->declined),
                    static_cast<unsigned long long>(stats->skipped), static_cast<unsigned long long>(stats->earlyExits),
                    static_cast<unsigned long long>(stats->mismatches - mismatchesBefore));
        if (stats->mismatches != mismatchesBefore)
            fail("mismatches reported for a correct native");
        resetRam();
    }

    // ---- 4: wrong natives are caught ---------------------------------------------------------------------------------
    // ApplyMatrix with a fault: 1 = the result's last bit, 2 = a3 written, 3 = a checkpoint too many, 4 = 1 ulp (loose).
    int g_fault = 0;
    template <class Env>
    bool faultyApply(Env &env, R5900Context *ctx)
    {
        const NativeFunction &good = g_natives[1];
        kamite::native::RealEnv *real = nullptr;
        (void)real;
        bool ran;
        if constexpr (std::is_same_v<Env, kamite::native::ShadowEnv>)
            ran = good.runShadow(env, ctx);
        else
            ran = good.runReal(env, ctx);
        if (!ran)
            return false;
        uint32_t out;
        std::memcpy(&out, &ctx->r[4], 4);
        uint8_t *p = env.ram + ((out & ~0xFu) & PS2_RAM_MASK);
        if (g_fault == 1 || g_fault == 4)
        {
            uint32_t w;
            std::memcpy(&w, p, 4);
            w += (w & 0x7F800000u) ? 1u : 0u; // one ulp (not on a zero: that would be a denormal)
            std::memcpy(p, &w, 4);
        }
        if (g_fault == 2)
            std::memset(&ctx->r[7], 0x5A, 4);
        if (g_fault == 3)
            env.checkpoint(32u);
        return true;
    }

    void checkDetection(const NativeFunction &apply)
    {
        const char *names[] = {"", "result bit", "register", "cycles", "1 ulp, loose"};
        for (int fault = 1; fault <= 4; ++fault)
        {
            g_fault = fault;
            for (uint32_t ulps : {0u, 1u})
            {
                if (fault != 4 && ulps)
                    continue;
                NativeFunction wrong = apply;
                wrong.address = 0x30A2B0u;
                wrong.runReal = &faultyApply<kamite::native::RealEnv>;
                wrong.runShadow = &faultyApply<kamite::native::ShadowEnv>;
                wrong.ulps = ulps;
                // a fresh harness slot per variant: a different (unused) address
                wrong.address = 0x00900000u + 0x10u * static_cast<uint32_t>(fault * 2 + ulps);
                kamite::native::shadow::add(wrong, &sceVu0ApplyMatrix_0x30a2b0);
                const kamite::native::shadow::Stats *stats = kamite::native::shadow::stats(wrong.address);
                for (int i = 0; i < 200; ++i)
                {
                    Call call;
                    setupCall(apply, call, false);
                    call.ctx.pc = wrong.address;
                    call.dueAt = 0;
                    runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
                }
                const bool expectCaught = !(fault == 4 && ulps == 1u);
                std::printf("fault %-14s ulps %u: checked %llu, mismatches %llu (context %llu ram %llu cycles %llu), saved %u, "
                            "worst ulps %u\n", names[fault], ulps, static_cast<unsigned long long>(stats->checked),
                            static_cast<unsigned long long>(stats->mismatches), static_cast<unsigned long long>(stats->mismatchCtx),
                            static_cast<unsigned long long>(stats->mismatchRam),
                            static_cast<unsigned long long>(stats->mismatchCycles), stats->saved, stats->worstUlps);
                // a zero result has no ulp to add: a few calls of fault 1 / 4 may match
                if (expectCaught && stats->mismatches < stats->checked / 2)
                    fail("a wrong native went unnoticed");
                if (!expectCaught && stats->mismatches != 0)
                    fail("a loose native within its ulps was reported");
                if (expectCaught && stats->saved != std::min<uint64_t>(16u, stats->mismatches))
                    fail("mismatches not saved");
            }
        }
        g_fault = 0;
        resetRam();
    }

    // ---- 5: test vectors for libvu0_check ----------------------------------------------------------------------------
    void writeVectors(const char *path, int count)
    {
        std::FILE *f = std::fopen(path, "wb");
        int written = 0;
        for (int i = 0; i < count; ++i)
        {
            R5900Context ctx;
            randomContext(ctx);
            ctx.pc = 0x30A2B0u;
            const uint32_t base = 0x20000u;
            setGpr(ctx, 4, base);
            setGpr(ctx, 5, base + 0x100u);
            setGpr(ctx, 6, base + 0x200u);
            setGpr(ctx, 31, 0x00123450u);
            fillFloats(g_ramA, base + 0x100u, 16, false);
            fillFloats(g_ramA, base + 0x200u, 4, false);
            g_dueAt = 0;
            sceVu0ApplyMatrix_0x30a2b0(g_ramA, &ctx, g_rt);
            // matrix, vector, then the original's ACC and result
            std::fwrite(g_ramA + base + 0x100u, 1, 64, f);
            std::fwrite(g_ramA + base + 0x200u, 1, 16, f);
            std::fwrite(&ctx.vu0_acc, 1, 16, f);
            std::fwrite(&ctx.vu0_vf[9], 1, 16, f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d libvu0 vectors to %s\n", written, path);
        resetRam();
    }
}

namespace
{
    template <class Run>
    double nsPerCall(Run run)
    {
        const int n = 2000000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
            run();
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    }

    void bench(const NativeFunction &fn, Recompiled recompiled)
    {
        Call call;
        setupCall(fn, call, false);
        setGpr(call.ctx, 4, 0x30000u), setGpr(call.ctx, 5, 0x30100u), setGpr(call.ctx, 6, 0x30200u);
        fillFloats(g_ramA, 0x30100u, 32, false);
        g_dueAt = 0;
        R5900Context ctx;
        const double original = nsPerCall([&] {
            std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx));
            recompiled(g_ramA, &ctx, g_rt);
        });
        const double native = nsPerCall([&] {
            std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx));
            kamite::native::RealEnv env{g_ramA, g_rt};
            fn.runReal(env, &ctx);
        });
        const double copy = nsPerCall([&] { std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx)); });
        std::printf("%-20s recompiled %.1f ns, native %.1f ns (context copy %.1f ns subtracted)\n", fn.name,
                    original - copy, native - copy, copy);
    }
}

namespace
{
    template <class T>
    bool take(std::FILE *f, T *out, size_t size = sizeof(T)) { return std::fread(out, 1, size, f) == size; }

    int rerun(const char *path, const std::vector<std::pair<uint32_t, Recompiled>> &recompiledByAddress)
    {
        std::FILE *f = std::fopen(path, "rb");
        if (!f)
            return 2;
        char magic[8];
        int records = 0;
        while (take(f, magic, 8) && std::memcmp(magic, "GHRSHDW1", 8) == 0)
        {
            uint32_t ctxSize, address, kinds, count;
            char name[32];
            uint64_t call, nativeCycles, realCycles;
            take(f, &ctxSize), take(f, &address), take(f, name, 32), take(f, &call), take(f, &kinds);
            take(f, &nativeCycles), take(f, &realCycles), take(f, &count);
            if (ctxSize != sizeof(R5900Context))
            {
                std::printf("context size %u here %zu: the game's build lays it out differently\n", ctxSize, sizeof(R5900Context));
                return 2;
            }
            std::vector<kamite::native::Footprint::Range> ranges(count);
            uint32_t total = 0;
            for (auto &range : ranges)
                take(f, &range.address), take(f, &range.size), total += range.size;
            Call input;
            R5900Context gameNative, gameReal;
            take(f, &input.ctx, ctxSize), take(f, &gameNative, ctxSize), take(f, &gameReal, ctxSize);
            std::vector<uint8_t> in(total), gameNativeBytes(total), gameRealBytes(total);
            take(f, in.data(), total), take(f, gameNativeBytes.data(), total), take(f, gameRealBytes.data(), total);
            ++records;
            Recompiled recompiled = nullptr;
            const NativeFunction *fn = nullptr;
            for (const auto &[a, r] : recompiledByAddress)
                if (a == address)
                    recompiled = r;
            for (const NativeFunction &n : g_natives)
                if (n.address == address)
                    fn = &n;
            std::printf("record %d: %s call %llu, kinds %u, cycles native %llu / real %llu\n", records, name,
                        static_cast<unsigned long long>(call), kinds, static_cast<unsigned long long>(nativeCycles),
                        static_cast<unsigned long long>(realCycles));
            if (!recompiled || !fn)
            {
                std::printf("  (not built into this test)\n");
                continue;
            }
            resetRam();
            for (uint32_t i = 0, at = 0; i < count; at += ranges[i].size, ++i)
            {
                std::memcpy(g_ramA + (ranges[i].address & PS2_RAM_MASK), in.data() + at, ranges[i].size);
                std::memcpy(g_ramB + (ranges[i].address & PS2_RAM_MASK), in.data() + at, ranges[i].size);
            }
            input.dueAt = 0;
            const Outcome original = runFrom(input, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            const Outcome native = runFrom(input, [&](R5900Context *ctx) {
                kamite::native::RealEnv env{g_ramB, g_rt};
                fn->runReal(env, ctx);
            });
            std::printf("  here: native %s the recompiled function; the recompiled function %s the game's real run\n",
                        sameOutcome(original, native) ? "matches" : "DIFFERS from",
                        std::memcmp(&original.ctx, &gameReal, ctxSize) == 0 ? "matches" : "differs from");
        }
        std::fclose(f);
        return 0;
    }
}

int main(int argc, char **argv)
{
    // The game thread's float environment: round toward zero, denormals flushed (main.cpp applyEeFloatingPointMode).
    _mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x6000u | 0x8040u);
    kamite::native::libvu0::registerFunctions();
    const NativeFunction &mul = g_natives[0], &apply = g_natives[1];
    if (argc > 2 && std::strcmp(argv[1], "rerun") == 0)
        return rerun(argv[2], {{0x30A2E0u, &sceVu0MulMatrix_0x30a2e0}, {0x30A2B0u, &sceVu0ApplyMatrix_0x30a2b0}});
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0)
    {
        bench(mul, &sceVu0MulMatrix_0x30a2e0);
        bench(apply, &sceVu0ApplyMatrix_0x30a2b0);
        return 0;
    }
    const int calls = argc > 1 ? std::atoi(argv[1]) : 20000;

    checkNative(mul, &sceVu0MulMatrix_0x30a2e0, calls);
    checkNative(apply, &sceVu0ApplyMatrix_0x30a2b0, calls);

    std::filesystem::remove_all("/tmp/native_shadow_test");
    kamite::native::shadow::Settings settings;
    settings.output = &testOutput;
    settings.header = &testHeader;
    settings.cycles = &testCycles;
    kamite::native::shadow::begin(settings);
    kamite::native::shadow::add(mul, &sceVu0MulMatrix_0x30a2e0);
    kamite::native::shadow::add(apply, &sceVu0ApplyMatrix_0x30a2b0);
    checkInvisible(mul, &sceVu0MulMatrix_0x30a2e0, calls);
    checkInvisible(apply, &sceVu0ApplyMatrix_0x30a2b0, calls);

    checkDetection(apply);
    kamite::native::shadow::writeSummary();
    std::printf("summary: %s.native.txt (%ju bytes), records: %s.native.bin (%ju bytes)\n", g_output.c_str(),
                static_cast<uintmax_t>(std::filesystem::file_size(g_output + ".native.txt")), g_output.c_str(),
                static_cast<uintmax_t>(std::filesystem::file_size(g_output + ".native.bin")));

    if (argc > 2)
        writeVectors(argv[2], 20000);
    if (g_stopRequested)
        fail("the harness asked the runtime to stop");
    std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
