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
#include "kamite/native/draw_list.h"
#include "kamite/native/shadow.h"
#include "kamite/native/libvu0_core.h"
#include "kamite/texture_slots.h"
#include "kamite/model_detail.h"
#include "ps2_runtime_macros.h" // batch 2 vectors: the runtime's FPU / VU0 ops as the reference

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
void _sceVu0ecossin_0x30a5d0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sceVu0RotMatrixX_0x30a700(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sceVu0RotMatrixY_0x30a7b8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void sceVu0RotMatrixZ_0x30a650(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
// rung 3, step a, batch 1
#define KM_RECOMPILED(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
KM_RECOMPILED(sceVu0CopyMatrix_0x30a548)
KM_RECOMPILED(sceVu0UnitMatrix_0x30a5a0)
KM_RECOMPILED(sceVu0MulVector_0x30a4d0)
KM_RECOMPILED(sceVu0ScaleVector_0x30a4e8)
KM_RECOMPILED(sceVu0OuterProduct_0x30a328)
KM_RECOMPILED(sceVu0Normalize_0x30a370)
KM_RECOMPILED(sceVu0TransMatrix_0x30a508)
KM_RECOMPILED(sceVu0TransposeMatrix_0x30a3b0)
KM_RECOMPILED(sceVu0InversMatrix_0x30a3f8)
KM_RECOMPILED(sceVu0CameraMatrix_0x30a8c8)
KM_RECOMPILED(sceVu0NormalLightMatrix_0x30a978)
KM_RECOMPILED(capVu0Length_0x1f7df8)
KM_RECOMPILED(Tramp_001F7DF8_00101E28_0x101e28)
KM_RECOMPILED(MtxCopy_0x147528)
KM_RECOMPILED(MtxMultiply_0x147548)
KM_RECOMPILED(func_0014FF00_0x14ff00)
KM_RECOMPILED(func_0031A650_0x31a650)
KM_RECOMPILED(func_0031A600_0x31a600)
// rung 3, step a, batch 2
KM_RECOMPILED(func_001503B8_0x1503b8)
KM_RECOMPILED(func_001F8DF8_0x1f8df8)
KM_RECOMPILED(func_002D7E50_0x2d7e50)
KM_RECOMPILED(func_002D8008_0x2d8008)
KM_RECOMPILED(func_002D8070_0x2d8070)
KM_RECOMPILED(func_00150DC8_0x150dc8)
KM_RECOMPILED(func_00150F68_0x150f68)
KM_RECOMPILED(func_00151168_0x151168)
KM_RECOMPILED(func_00155AE8_0x155ae8)
KM_RECOMPILED(func_00155BE8_0x155be8)
KM_RECOMPILED(func_00155C68_0x155c68)
KM_RECOMPILED(func_00155D60_0x155d60)
KM_RECOMPILED(PostInc_D_00566E10_0015B0F0_15B0F0_0x15b0f0)
KM_RECOMPILED(cModel_getMeshPtr_0x14b6f8)
KM_RECOMPILED(ForwardNodeUpdate_14D768_0x14d768)
KM_RECOMPILED(cModel_nodeLayerScreen_0x14be38)
// batches 3-4 (readable natives from the decomp's C)
KM_RECOMPILED(func_0014C5A0_0x14c5a0)
KM_RECOMPILED(func_00151398_0x151398)
KM_RECOMPILED(func_00148BD8_0x148bd8)
KM_RECOMPILED(func_0014B9A0_0x14b9a0)
KM_RECOMPILED(func_00151368_0x151368)
// batches 3-4b (the texture cache's misses)
KM_RECOMPILED(func_00151630_0x151630)
KM_RECOMPILED(func_00151688_0x151688)
KM_RECOMPILED(func_00151990_0x151990)
KM_RECOMPILED(func_00151CC0_0x151cc0)
KM_RECOMPILED(func_00151FA0_0x151fa0)
KM_RECOMPILED(Tim2CalcBufWidth_0x309c30)
KM_RECOMPILED(ps2___muldi3_0x31de10)
KM_RECOMPILED(func_00148D30_0x148d30)
KM_RECOMPILED(func_00148690_0x148690)
KM_RECOMPILED(cModel_setNodeMaterial_0x14b4d0)
KM_RECOMPILED(ForwardAttackByMode_14B5D8_0x14b5d8)
KM_RECOMPILED(func_00154240_0x154240)
KM_RECOMPILED(tanf_0x3a1ae8)
KM_RECOMPILED(ps2___kernel_tanf_0x3a4830)
KM_RECOMPILED(ps2___ieee754_rem_pio2f_0x3a3768)
KM_RECOMPILED(ps2___kernel_rem_pio2f_0x3a3dd8)
KM_RECOMPILED(func_003A19D8_0x3a19d8)
KM_RECOMPILED(scalbnf_0x3a4e28)
KM_RECOMPILED(floorf_0x3a4d18)
KM_RECOMPILED(copysignf_0x3a4f88)
KM_RECOMPILED(func_0030AA40_0x30aa40)
KM_RECOMPILED(func_0014C7E8_0x14c7e8)
KM_RECOMPILED(cModel_Tag_set_rate_0x14ac90)
// rung 3 step b3 B: cModel_Tag_set_scr's subtree
KM_RECOMPILED(func_0014D0E8_0x14d0e8)
KM_RECOMPILED(func_00153B00_0x153b00)
KM_RECOMPILED(func_002D81A0_0x2d81a0)
KM_RECOMPILED(cModel_nodeLayer_0x14bef8)
KM_RECOMPILED(func_0014B810_0x14b810)
KM_RECOMPILED(cModel_queuePacket_0x14c6b8)
KM_RECOMPILED(cModel_Tag_set_scr_0x14aab8)
KM_RECOMPILED(SearchDataByTwoNames_0x31cdc0)
KM_RECOMPILED(cIDBase_getSlotPtr_0x1522c0)
// rung 4, M1 batch 1: the hierarchy leaves (+ sceVu0RotMatrix, MtxInitRotVec's callee for order 5)
KM_RECOMPILED(Adjust_theta_vec_0x1f7da0)
KM_RECOMPILED(MtxInitTransVec_0x1476d0)
KM_RECOMPILED(MtxInitRotVec_0x147728)
KM_RECOMPILED(MtxInitScaleVec_0x147a08)
KM_RECOMPILED(MtxMulRotVec_0x147b60)
KM_RECOMPILED(MtxMulScaleVec_0x147bf8)
KM_RECOMPILED(sceVu0RotMatrix_0x30a870)
// M1 batch 2 (+ func_001501D0's twist path: callees only)
KM_RECOMPILED(MtxMulTransVec_0x147b18)
KM_RECOMPILED(MtxInitCoord_0x147a48)
KM_RECOMPILED(cParts_calcLocal_0x150138)
KM_RECOMPILED(MtxInverse_0x147568)
KM_RECOMPILED(func_001501D0_0x1501d0)
KM_RECOMPILED(MtxInitRotQuaternion_0x1478a8)
KM_RECOMPILED(CopyVec3ToField30_147C40_0x147c40)
KM_RECOMPILED(ForwardVec3At30_147C60_0x147c60)
KM_RECOMPILED(func_00150710_0x150710)
KM_RECOMPILED(Quaternion_SetRotationAxis_0x159fc8)
KM_RECOMPILED(capVu0Cos_0x1f7fb8)
KM_RECOMPILED(sinf_0x3a19f8)
KM_RECOMPILED(ps2___kernel_cosf_0x3a3c80)
KM_RECOMPILED(ps2___kernel_sinf_0x3a4728)
// M1's blend batch (+ its callees: capVu0LengthSq, sceVu0ScaleVectorXYZ, capVu0Atan2)
KM_RECOMPILED(func_00156D70_0x156d70)
KM_RECOMPILED(func_0015A6A0_0x15a6a0)
KM_RECOMPILED(func_0015A190_0x15a190)
KM_RECOMPILED(capVu0LengthSq_0x1f7e28)
KM_RECOMPILED(sceVu0ScaleVectorXYZ_0x30aba0)
KM_RECOMPILED(capVu0Atan2_0x1f8180)
// M2 (batch 3): the per-model passes
KM_RECOMPILED(cModel_calcParts_0x149eb8)
KM_RECOMPILED(cModel_calcWorldParts_0x14a038)
#undef KM_RECOMPILED

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
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *ctx, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load128 (special address) %08X pc %08X\n", a, ctx ? ctx->pc : 0u);
    std::abort();
}
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t, __m128i)
{
    std::fprintf(stderr, "unexpected Store128 (special address)\n");
    std::abort();
}
uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load8 (special address) %08X\n", a);
    std::abort();
}
uint16_t PS2Runtime::Load16(uint8_t *, R5900Context *, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load16 (special address) %08X\n", a);
    std::abort();
}
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *ctx, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load32 (special address) %08X at pc %08X\n", a, ctx ? ctx->pc : 0u);
    std::abort();
}
uint64_t PS2Runtime::Load64(uint8_t *, R5900Context *, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load64 (special address) %08X\n", a);
    std::abort();
}
void PS2Runtime::Store32(uint8_t *, R5900Context *, uint32_t, uint32_t)
{
    std::fprintf(stderr, "unexpected Store32 (special address)\n");
    std::abort();
}
void PS2Runtime::Store8(uint8_t *, R5900Context *, uint32_t, uint8_t)
{
    std::fprintf(stderr, "unexpected Store8 (special address)\n");
    std::abort();
}
void PS2Runtime::Store16(uint8_t *, R5900Context *, uint32_t, uint16_t)
{
    std::fprintf(stderr, "unexpected Store16 (special address)\n");
    std::abort();
}
bool PS2Runtime::dispatchGuestBranch(uint8_t *, R5900Context *, uint32_t target, uint32_t, uint32_t, GuestBranchKind, const char *)
{
    std::fprintf(stderr, "unexpected indirect branch to 0x%08X\n", target);
    std::abort();
}
void PS2Runtime::Store64(uint8_t *, R5900Context *, uint32_t, uint64_t)
{
    std::fprintf(stderr, "unexpected Store64 (special address)\n");
    std::abort();
}
void PS2Runtime::SignalException(R5900Context *, PS2Exception)
{
    std::fprintf(stderr, "unexpected SignalException\n");
    std::abort();
}
void PS2Runtime::requestStop() { g_stopRequested = true; }
// func_00150F68's `break 7` (the natives decline before it): the recompiled function's only.
int g_breaks = 0;
void PS2Runtime::handleBreak(uint8_t *, R5900Context *) { ++g_breaks; }
// native.cpp is not linked: no entry observers here.
void kamite::native::runEntryObservers(uint32_t, uint8_t *, R5900Context *, PS2Runtime *) {}
// The draw list (draw_list.cpp: the walker's registry) is off here: the natives' registration is skipped.
namespace kamite::native::draw_list
{
    bool g_on = false;
    uint32_t g_queueCaller = 0u;
    uint64_t g_uploadsUnlisted = 0u;
    void listPackets(uint32_t, const uint32_t *, const uint32_t *, const uint64_t *, uint32_t, Source) {}
    void rewriting(uint32_t, uint32_t, uint32_t) {}
}

// The patch flags and host hooks the generated code reads (ps2_runtime.cpp's; not linked here): the TIM2 loader's
// texture_slots hook at 0x151C70, registered by texture_slots::install() and switched per call by the setups.
std::atomic<uint32_t> g_ps2xPatchFlags{0u};
namespace
{
    std::vector<std::string> g_flagNames;
    std::vector<std::pair<std::string, PS2Runtime::RecompiledFunction>> g_hostHooks;
}
uint32_t ps2xPatchFlagMask(const char *name)
{
    for (size_t i = 0; i < g_flagNames.size(); ++i)
        if (g_flagNames[i] == name)
            return 1u << i;
    g_flagNames.emplace_back(name);
    return 1u << (g_flagNames.size() - 1u);
}
void ps2xSetPatchFlag(const char *name, bool enabled)
{
    const uint32_t mask = ps2xPatchFlagMask(name);
    if (enabled)
        g_ps2xPatchFlags.fetch_or(mask);
    else
        g_ps2xPatchFlags.fetch_and(~mask);
}
bool ps2xPatchFlag(const char *name) { return (g_ps2xPatchFlags.load() & ps2xPatchFlagMask(name)) != 0u; }
void ps2xRegisterHostHook(const char *name, PS2Runtime::RecompiledFunction hook) { g_hostHooks.emplace_back(name, hook); }
PS2Runtime::RecompiledFunction ps2xHostHook(const char *name)
{
    for (const auto &[n, hook] : g_hostHooks)
        if (n == name)
            return hook;
    return nullptr;
}

// The generated JAL (ps2_runtime.cpp callGuestFunctionAs): the callee's slot (its recompiled function, or the shadow
// harness's entry when the nested checks route shadowed callees through it), the 8-cycle checkpoint first.
using SlotFunction = PS2Runtime::RecompiledFunction;
std::vector<std::pair<uint32_t, SlotFunction>> g_callees; // set in main: every recompiled function in the test
bool g_nestedCalls = false;                               // route callees that have a shadow slot to shadow::entry
bool g_nativeCallees = false; // bench: callees with a checked native run it (as in play), declines the recompiled code
bool runCalleeNative(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc); // below, by the registry
SlotFunction g_ecossinSlot = &_sceVu0ecossin_0x30a5d0;    // the rung-2 nested test's switch
bool PS2Runtime::callGuestFunction(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t, uint32_t fallthroughPc)
{
    SlotFunction fn = nullptr;
    for (const auto &[address, f] : g_callees)
        if (address == targetPc)
            fn = f;
    if (targetPc == 0x30A5D0u)
        fn = g_ecossinSlot;
    else if (g_nestedCalls && kamite::native::shadow::stats(targetPc))
        fn = &kamite::native::shadow::entry;
    if (!fn)
    {
        std::fprintf(stderr, "unexpected call of 0x%08X\n", targetPc);
        std::abort();
    }
    ctx->pc = targetPc;
    if (eeCheckpointDue(8u))
        return false;
    if (!g_nativeCallees || !runCalleeNative(rdram, ctx, targetPc))
        fn(rdram, ctx, this);
    return ctx->pc == fallthroughPc;
}

// native.cpp is not linked: collect the registrations here.
static std::vector<kamite::native::NativeFunction> g_natives;
void kamite::native::registerFunction(const NativeFunction &function) { g_natives.push_back(function); }
bool runCalleeNative(uint8_t *rdram, R5900Context *ctx, uint32_t targetPc)
{
    for (const kamite::native::NativeFunction &fn : g_natives)
        if (fn.address == targetPc && fn.checked)
        {
            alignas(64) static unsigned char storage[64]; // never dereferenced (eeCheckpointDue: the stub above)
            kamite::native::RealEnv env{rdram, reinterpret_cast<PS2Runtime *>(storage)};
            return kamite::native::runNative(fn, fn.runReal, env, ctx);
        }
    return false;
}

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

    // Two 128 MB backings (lazily committed) like the runtime's; tests use the first 1 MB and the coefficients' block.
    struct Region
    {
        uint32_t begin, size;
    };
    // + batch 2's fixed addresses: the texture manager D_004664A0 (its tables up to +0x3F8AB) and the GS table 0x4A5D80, the allocator 0x566E10, the view matrix pointer 0x5CAFF0,
    // 0x7476B0 / 0x747A2C / 0x747AA8, the order table's descriptors 0x755780...
    constexpr Region kRegions[] = {{0u, 1u << 20}, {0x3C3C00u, 0x100u}, {0x466000u, 0x40590u}, {0x566E00u, 0x20u},
                                   {0x5CAFF0u, 0x10u}, {0x747600u, 0x500u}, {0x755700u, 0x100u},
                                   {0x4F8000u, 0x18000u}, // batch 3: the frame allocator's areas around 0x506E10
                                   // step a: func_00154240's constants (D_003BD870..89F, D_004A6940) and the light set
                                   {0x3BD800u, 0x200u}, {0x4A6900u, 0x100u}, {0x604700u, 0x80u}, {0x61A800u, 0x200u},
                                   // the 0x14C7E8 batch: tanf's T table, the screen values D_003C1180 / 1190, D_00586B88
                                   {0x45EF00u, 0x100u}, {0x3C1100u, 0x100u}, {0x586B00u, 0x100u},
                                   // rung 4: Adjust_theta_vec's table (0x3BF110)
                                   {0x3BF100u, 0x60u}, {0x41EA00u, 0x20u}, // + MtxInitRotVec's switch, capVu0Cos's table (0x3BF140)
                                   {0x42C340u, 0x40u}, {0x41EE40u, 0x20u}}; // + capVu0Atan2's table, 0x15A190's next
    constexpr uint32_t usedBytes()
    {
        uint32_t total = 0;
        for (const Region &r : kRegions)
            total += r.size;
        return total;
    }
    constexpr uint32_t kUsed = usedBytes();
    uint8_t *backing()
    {
        void *p = mmap(nullptr, PS2_RDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return static_cast<uint8_t *>(p);
    }
    uint8_t *g_ramA = backing(), *g_ramB = backing(), *g_ramC = backing(); // C: recorded footprints' scratch
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
        const float vf0[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // VU0's constant register
        std::memcpy(&ctx.vu0_vf[0], vf0, 16);
    }
    // An angle as the game passes them (radians), sometimes -0, a denormal, or past the natives' limits.
    float angle()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 100u);
        float f;
        if (pick < 80)
            f = (static_cast<float>(g_rng() % 2000001u) / 1000000.0f - 1.0f) * 7.0f;
        else if (pick < 84)
            f = 0.0f;
        else if (pick < 88)
            return -0.0f;
        else if (pick < 91)
        {
            uint32_t bits = (static_cast<uint32_t>(g_rng()) & 0x807FFFFFu); // a denormal
            std::memcpy(&f, &bits, 4);
        }
        else if (pick < 96)
            f = (static_cast<float>(g_rng() % 2000001u) / 1000000.0f - 1.0f) * 40.0f; // some past 16 / 32
        else
        {
            const uint32_t bits = gameFloat();
            std::memcpy(&f, &bits, 4);
        }
        return f;
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

    void setU32(uint32_t at, uint32_t v)
    {
        std::memcpy(g_ramA + at, &v, 4);
        std::memcpy(g_ramB + at, &v, 4);
    }
    void randomBytes(uint32_t at, uint32_t size)
    {
        for (uint32_t i = 0; i < size; ++i)
            g_ramA[at + i] = static_cast<uint8_t>(g_rng());
        std::memcpy(g_ramB + at, g_ramA + at, size);
    }

    // Rung 3's batch 1 on top of the libvu0 setup: a stack (sp, its frame's old bytes random), a3, sometimes a zero
    // vector, a broken vf0, inputs in the frame; the queue's tables (descriptors, slots, tails, the frame half word).
    void setupBatch1(const NativeFunction &fn, Call &call, bool wild, uint32_t base, uint32_t seg)
    {
        const uint32_t sp = 0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
        randomBytes(sp - 0x100u, 0x100u);
        setGpr(call.ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
        setGpr(call.ctx, 7, (base + 0x300u) | seg);
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
        if (pick == 0)
            std::memset(g_ramA + base + 0x100u, 0, 16), std::memcpy(g_ramB + base + 0x100u, g_ramA + base + 0x100u, 16);
        else if (pick == 1)
            std::memset(g_ramA + base + 0x200u, 0, 16), std::memcpy(g_ramB + base + 0x200u, g_ramA + base + 0x200u, 16);
        else if (pick == 2 && wild)
        {
            const float two = 2.0f;
            std::memcpy(reinterpret_cast<uint8_t *>(&call.ctx.vu0_vf[0]) + 12, &two, 4);
        }
        else if (pick == 3)
            setGpr(call.ctx, 5 + static_cast<int>(g_rng() % 3u), (sp - 0x40u) | seg); // an input inside the frame
        else if (pick == 4)
            setGpr(call.ctx, 4, (sp - 0x20u) | seg); // the output inside the frame
        else if (pick == 5)
            setGpr(call.ctx, 4, ((base + 0x104u) | seg)); // unaligned out
        if (fn.address == 0x30A978u && g_rng() % 2u) // NormalLightMatrix: light w lanes as the game has them
            for (uint32_t v = 0x100u; v <= 0x300u; v += 0x100u)
                setU32(base + v + 12u, 0u);

        if (fn.address != 0x14FF00u && fn.address != 0x31A650u && fn.address != 0x31A600u)
            return;
        // The queue: the frame half word, a table at base (descriptors at +0xB00 + 8 * index + 4 * half, indexes -2..11),
        // descriptors at 0x40000 + 0x40 * k (+0 slots, +0x20 the first index), slots at 0x50000, tails at 0x60000.
        setU32(0x7476B0u, static_cast<uint32_t>(g_rng()));
        const uint32_t mirror = (g_rng() % 4u == 0u) ? 0x20000000u : 0u;
        for (int i = -4; i < 24; ++i)
            setU32(base + 0xB00u + static_cast<uint32_t>(4 * i), (0x40000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u)) | mirror);
        for (uint32_t k = 0; k < 16; ++k)
        {
            setU32(0x40000u + 0x40u * k, (0x50000u + 0x100u * static_cast<uint32_t>(g_rng() % 8u)) | mirror);
            setU32(0x40000u + 0x40u * k + 0x20u, static_cast<uint32_t>(g_rng() % 24u));
        }
        randomBytes(0x50000u, 0x1000u);
        randomBytes(0x60000u, 0x200u);
        const uint32_t index = static_cast<uint32_t>(static_cast<int>(g_rng() % 14u) - 2); // some >= 10, some negative
        const uint32_t tail = 0x60000u + 8u * static_cast<uint32_t>(g_rng() % 32u);
        const uint32_t tail2 = (g_rng() % 2u) ? 0u : 0x60100u + 8u * static_cast<uint32_t>(g_rng() % 32u);
        const uint64_t order = (g_rng() % 8u == 0u) ? g_rng() : g_rng() % 32u; // an index (sometimes a huge one)
        if (fn.address == 0x14FF00u)
        {
            setGpr(call.ctx, 4, (0x40000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u)) | mirror);
            std::memcpy(&call.ctx.r[5], &order, 8);
            setGpr(call.ctx, 6, 0x00ABC000u + static_cast<uint32_t>(g_rng() % 0x1000u) * 16u);
            setGpr(call.ctx, 7, (g_rng() % 8u == 0u) ? 0x50000u + 0x100u * static_cast<uint32_t>(g_rng() % 8u) : tail);
            return;
        }
        setGpr(call.ctx, 4, base | mirror);
        setGpr(call.ctx, 5, index);
        std::memcpy(&call.ctx.r[6], &order, 8);
        setGpr(call.ctx, 7, tail);
        setGpr(call.ctx, 8, tail2); // 0 half the time: the last tag is then a3's
    }

    // ---- rung 3, step a, batch 2: each function's tables, lists and arguments --------------------------------------
    void setU64(uint32_t at, uint64_t v)
    {
        std::memcpy(g_ramA + at, &v, 8);
        std::memcpy(g_ramB + at, &v, 8);
    }
    void setByte(uint32_t at, uint8_t v) { g_ramA[at] = g_ramB[at] = v; }
    void floatsAt(uint32_t at, int count, bool wild)
    {
        fillFloats(g_ramA, at, count, wild);
        std::memcpy(g_ramB + at, g_ramA + at, 4u * static_cast<uint32_t>(count));
    }
    void setFloat(uint32_t at, float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        setU32(at, bits);
    }
    void setGpr64(R5900Context &ctx, int reg, uint64_t value) { std::memcpy(&ctx.r[reg], &value, 8); }
    // A GS table index as the code sees it (andi 0xFF, bltz): mostly small, sometimes negative or with upper bits.
    uint64_t gsIndex()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
        if (pick == 0)
            return static_cast<uint64_t>(-static_cast<int64_t>(1 + g_rng() % 4u));
        if (pick == 1)
            return g_rng(); // anything
        return g_rng() % 300u;
    }
    // The order table at 0x754C80 the texture builders queue into (descriptors at +0xB00, as setupBatch1's).
    void orderTable(uint32_t mirror)
    {
        setU32(0x7476B0u, static_cast<uint32_t>(g_rng()));
        for (int i = -4; i < 24; ++i)
            setU32(0x755780u + static_cast<uint32_t>(4 * i), (0x40000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u)) | mirror);
        for (uint32_t k = 0; k < 16; ++k)
        {
            setU32(0x40000u + 0x40u * k, (0x50000u + 0x100u * static_cast<uint32_t>(g_rng() % 8u)) | mirror);
            setU32(0x40000u + 0x40u * k + 0x20u, static_cast<uint32_t>(g_rng() % 24u));
        }
        randomBytes(0x50000u, 0x1000u);
    }
    // A mesh list from model + 0x25C (next at +0x404, the countdown byte at +0x385); its nodes.
    uint32_t g_meshSpacing = 0;
    int meshList(uint32_t model, uint32_t seg, bool cycles, bool zeroBytes)
    {
        const int n = static_cast<int>(g_rng() % 7u);
        const uint32_t spacing = g_meshSpacing = (g_rng() % 3u == 0u) ? 0x420u : (g_rng() % 4u == 0u) ? 0x100u : 0x800u;
        const uint32_t first = 0x30000u + 0x40u * static_cast<uint32_t>(g_rng() % 8u);
        for (int i = 0; i < n; ++i)
        {
            const uint32_t node = first + spacing * static_cast<uint32_t>(i);
            setU32(node + 0x404u, i + 1 < n ? (node + spacing) | seg : 0u);
            setByte(node + 0x385u, (zeroBytes && g_rng() % 30u == 0u) ? 0u : static_cast<uint8_t>(1u + g_rng() % 4u));
        }
        if (cycles && n > 0 && g_rng() % 4u == 0u)
            setU32(first + spacing * static_cast<uint32_t>(n - 1) + 0x404u, (first + spacing * static_cast<uint32_t>(g_rng() % n)) | seg);
        setU32(model + 0x25Cu, n ? first | seg : 0u);
        return n;
    }

    // A texture manager's cache table at w (func_00151398's layout): entries {name, b, c, v} from the kind's start
    // down; a hit (v < maxBlock) or, 1 in 6, a full table with no hit (the original returns 0 then; a miss in a table
    // that isn't full makes it insert and call the upload builders, which the test doesn't have).
    void textureTable(uint32_t w, uint64_t &name, uint64_t &type, uint32_t maxBlock)
    {
        const uint32_t kindPick = static_cast<uint32_t>(g_rng() % 6u);
        type = kindPick < 2 ? 2u : kindPick < 4 ? 1u : kindPick == 4 ? 0u : g_rng();
        const uint32_t start = w + (type == 2u ? 0x3D2F0u : type == 1u ? 0x3DC50u : 0x3C990u);
        const uint32_t boundary = w + (type == 2u ? 0x3C9A0u : type == 1u ? 0x3D300u : 0x3C040u);
        const bool full = g_rng() % 6u == 0u;
        const uint32_t n = full ? (start - boundary) / 0x10u : 1u + static_cast<uint32_t>(g_rng() % 24u);
        std::vector<uint32_t> names(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            names[i] = g_rng() % 8u == 0u ? 0x80000000u | static_cast<uint32_t>(g_rng()) : static_cast<uint32_t>(g_rng() % 0x7FFFFFu);
            const uint32_t at = start - 0x10u * i;
            setU32(at, names[i]);
            setU32(at + 4u, static_cast<uint32_t>(g_rng()));
            setU32(at + 8u, static_cast<uint32_t>(g_rng()));
            setU32(at + 12u, static_cast<uint32_t>(g_rng() % maxBlock));
        }
        setU32(start - 0x10u * n, 0xFFFFFFFFu); // the end (at the boundary when full)
        name = names[g_rng() % n];
        if (full)
            name = 0xFFFFFFF0u; // in no entry (the random names stay below 0x7FFFFF or carry bit 31 with others)
        name = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(name))); // as the callers pass it
        if (full && g_rng() % 2u == 0u)
            name = names[g_rng() % n] ^ (1ull << 40); // upper bits no lw gives: never equal (a miss)
    }

    // ---- batches 3-4b: the texture cache's misses ------------------------------------------------------------------
    void setU16(uint32_t at, uint16_t v)
    {
        std::memcpy(g_ramA + at, &v, 2);
        std::memcpy(g_ramB + at, &v, 2);
    }
    // The fixed tables the builders read: the TIM / TM formats -> PSM (0x41ED68 / 80 / 98, the ELF's: 2 1 0 0x14 0x13
    // 0; a picture type of 0 or 7 reads either neighbour) and Tim2CalcBufWidth's (0x309C30) jump table (0x4502E0).
    void textureTables()
    {
        static const uint32_t jump[59] = {
            0x309C54u,0x309C54u,0x309C54u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C54u,
            0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C70u,0x309C70u,0x309C94u,
            0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C54u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,
            0x309C94u,0x309C94u,0x309C94u,0x309C54u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,
            0x309C54u,0x309C94u,0x309C94u,0x309C94u,0x309C54u,0x309C54u,0x309C54u,0x309C94u,0x309C94u,0x309C94u,0x309C94u,
            0x309C94u,0x309C94u,0x309C94u,0x309C54u};
        for (uint32_t i = 0; i < 59u; ++i)
            setU32(0x4502E0u + 4u * i, jump[i]);
        static const uint32_t formats[6] = {2u, 1u, 0u, 0x14u, 0x13u, 0u};
        for (uint32_t table : {0x41ED68u, 0x41ED80u, 0x41ED98u})
            for (uint32_t i = 0; i < 6u; ++i)
                setU32(table + 4u * i, formats[i]);
        setU32(0x41ED64u, g_rng() % 2u ? 0x13u : 0x3Bu + static_cast<uint32_t>(g_rng() % 4u)); // type 0's PSM
        setU32(0x41EDB0u, g_rng() % 2u ? 0x2Cu : 0x80000000u);                                // type 7 of the TM table
        setU32(0x41EA50u, 0x00323354u), setU32(0x41EA54u, 0u), setU32(0x41EA58u, 0x00324D54u), setU32(0x41EA5Cu, 0u); // "T32", "TM2"
    }
    // A texture manager at w: the uploads' object (its record counter at +0x3C, the next records' old bytes), the VRAM
    // cursors (+0x3DC60..), the register cursor and the upload counts (+0x3F8A8 / AA / AB), the GS flag, the order table.
    void textureManager(uint32_t w)
    {
        textureTables();
        orderTable((g_rng() % 4u == 0u) ? 0x20000000u : 0u);
        setU32(0x747A2Cu, g_rng() % 8u == 0u ? 0x800u : static_cast<uint32_t>(g_rng()) & ~0x800u);
        const uint32_t records = g_rng() % 16u == 0u ? 0x280u - static_cast<uint32_t>(g_rng() % 3u)
                                                    : static_cast<uint32_t>(g_rng() % 0x270u);
        setU32(w + 0x3Cu, records);
        const uint32_t from = w + 0x40u + std::min(records, 0x27Fu) * 0x180u;
        randomBytes(from, std::min(0x180u * 8u, w + 0x3C040u - from));
        for (uint32_t k = 0; k < 9u; ++k)
            setU32(w + 0x3DC60u + 4u * k, (k % 3u == 1u ? 0x1C20u : 0x2800u) + static_cast<uint32_t>(g_rng() % 0x400u));
        setU32(w + 0x3F8A8u, static_cast<uint32_t>(g_rng() % 0x100u) | (static_cast<uint32_t>(g_rng()) & 0xFFFF0000u));
        ps2xSetPatchFlag("texture_slots", g_rng() % 4u != 0u);
    }
    // A TIM picture header (0x30 bytes) at p: its type (1-5, sometimes 0 / 6 / 7), size, header size, CLUT size,
    // GsTex0 (random).
    void timPicture(uint32_t p)
    {
        randomBytes(p, 0x30u);
        setByte(p + 0x13u, g_rng() % 16u == 0u ? static_cast<uint8_t>(g_rng() % 8u) : static_cast<uint8_t>(1u + g_rng() % 5u));
        const uint32_t sizes = static_cast<uint32_t>(g_rng() % 16u);
        setU16(p + 0x14u, sizes == 0 ? static_cast<uint16_t>(g_rng() % 2048u) : static_cast<uint16_t>(1u << (g_rng() % 11u)));
        setU16(p + 0x16u, sizes == 1 ? static_cast<uint16_t>(g_rng() % 2048u) : static_cast<uint16_t>(1u << (g_rng() % 11u)));
        setU16(p + 0xCu, g_rng() % 8u == 0u ? static_cast<uint16_t>(g_rng() % 0x100u) : 0x30u);
        setU32(p + 4u, g_rng() % 3u == 0u ? 0x40u : g_rng() % 2u ? 0x400u : static_cast<uint32_t>(g_rng() % 0x1000u));
        setU32(p + 8u, g_rng() % 8u == 0u ? static_cast<uint32_t>(g_rng()) : static_cast<uint32_t>(g_rng() % 0x80000u));
    }
    // A texture file of class `kind` at f (0 TIM2, 1 TIM3, 2 the game's pack, 3 TM), as func_00151630 classifies them.
    void textureFile(int kind, uint32_t f)
    {
        randomBytes(f, 0x20u);
        switch (kind)
        {
        case 0:
        case 1:
        {
            const bool tim3 = kind == 1 || g_rng() % 4u == 0u;
            setByte(f, 'T'), setByte(f + 1u, 'I'), setByte(f + 2u, 'M'), setByte(f + 3u, tim3 ? '3' : '2');
            if (kind == 1)
            {
                setByte(f + 5u, 6u);
                timPicture(f + 0x10u);
                return;
            }
            const uint8_t format = g_rng() % 2u ? 0u : static_cast<uint8_t>(1u + g_rng() % 5u); // a TIM3 here: not 6
            setByte(f + 5u, format);
            const uint32_t n = g_rng() % 10u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 4u);
            setU16(f + 6u, static_cast<uint16_t>(n));
            for (uint32_t i = 0; i < n; ++i)
                timPicture(f + (format ? 0x80u : 0x10u) + 0x30u * i);
            return;
        }
        case 3:
        {
            setByte(f, 'T'), setByte(f + 1u, 'M');
            setByte(f + 3u, static_cast<uint8_t>(0x34u + g_rng() % 0x40u)); // not '2' / '3'
            const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
            setU32(f + 4u, pick == 0 ? 0x100u : (1u + static_cast<uint32_t>(g_rng() % 4u)) |
                                                    (pick == 1 ? static_cast<uint32_t>(g_rng()) & 0xFFFFFF00u : 0u));
            setU32(f + 0xCu, static_cast<uint32_t>(g_rng() % 0x200000u));
            for (uint32_t i = 0; i < 256u; ++i)
                setU32(f + 0x10u + 4u * i, 0x400u + 0x40u * (i % 8u));
            for (uint32_t i = 0; i < 8u; ++i)
                timPicture(f + 0x410u + 0x40u * i);
            return;
        }
        default:
        {
            // the pack: +0 the entry count (bit 31: relocated), +4 / +6 its size, +8 / +0xC the holders' sections,
            // +0x10 / +0x12 their counts, entries of 0x18 from +0x18, packets of 0x60 from +0x200
            const bool relocated = g_rng() % 2u == 0u;
            const uint32_t entries = g_rng() % 8u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 5u);
            setU32(f, entries | (relocated ? 0x80000000u : 0u));
            setU16(f + 4u, static_cast<uint16_t>(g_rng() % 2u ? 1u << (g_rng() % 10u) : g_rng()));
            setU16(f + 6u, static_cast<uint16_t>(g_rng() % 2u ? 1u << (g_rng() % 10u) : g_rng()));
            setU32(f + 8u, 0x100u), setU32(f + 0xCu, 0x180u);
            const uint32_t n1 = static_cast<uint32_t>(g_rng() % 5u), n2 = static_cast<uint32_t>(g_rng() % 4u);
            setU16(f + 0x10u, static_cast<uint16_t>(n1)), setU16(f + 0x12u, static_cast<uint16_t>(n2));
            randomBytes(f + 0x18u, 0x18u * 5u);
            randomBytes(f + 0x100u, 0x100u);
            randomBytes(f + 0x200u, 0x60u * 7u);
            for (uint32_t j = 0; j < 7u; ++j)
            {
                const bool second = j >= 4u;
                const uint32_t holder = second ? f + 0x180u + 0x10u * (j - 4u) : f + 0x100u + 0x10u * j;
                const uint32_t packet = f + 0x200u + 0x60u * static_cast<uint32_t>(g_rng() % 7u);
                setU32(holder + 4u, relocated ? packet : packet - (second ? f + 0x180u : f + 0x100u));
            }
            return;
        }
        }
    }
    // The table of type `type` in the manager at w with room left and no entry named `name` (a miss that inserts).
    void textureMissTable(uint32_t w, uint64_t type, uint32_t name)
    {
        const uint32_t start = w + (type == 2u ? 0x3D2F0u : type == 1u ? 0x3DC50u : 0x3C990u);
        const uint32_t n = static_cast<uint32_t>(g_rng() % 24u);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t at = start - 0x10u * i;
            uint32_t v = static_cast<uint32_t>(g_rng() % 0x7FFFFFu);
            if (v == name)
                ++v;
            setU32(at, v), setU32(at + 4u, static_cast<uint32_t>(g_rng())), setU32(at + 8u, static_cast<uint32_t>(g_rng()));
            setU32(at + 12u, static_cast<uint32_t>(g_rng() % 0x10000u));
        }
        setU32(start - 0x10u * n, 0xFFFFFFFFu);
        randomBytes(start - 0x10u * n + 4u, 12u); // the new entry's old bytes
    }
    uint32_t textureFileAt() { return 0xD0000u + 0x800u * static_cast<uint32_t>(g_rng() % 16u); }
    uint64_t textureType()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 8u);
        return pick < 3 ? 2u : pick < 5 ? 1u : pick < 7 ? 0u : g_rng();
    }

    void setupBatch2(const NativeFunction &fn, Call &call, bool wild, uint32_t base, uint32_t seg)
    {
        R5900Context &ctx = call.ctx;
        uint32_t sp;
        std::memcpy(&sp, &ctx.r[29], 4);
        sp &= 0x1FFFFFFu;
        const uint32_t mirror = (g_rng() % 4u == 0u) ? 0x20000000u : 0u;
        switch (fn.address)
        {
        case 0x155D60u:
        {
            const uint32_t pick = static_cast<uint32_t>(g_rng() % 20u);
            // a 0 byte spins the original ~4G times: only when a checkpoint stops it early
            setByte(base + 0x385u, pick == 0 && call.dueAt ? 0u : pick == 1 ? 255u : static_cast<uint8_t>(1u + g_rng() % 6u));
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, g_rng());
            setGpr(ctx, 6, (g_rng() % 8u == 0u) ? 0xFFFFFFFFu : static_cast<uint32_t>(g_rng() % 4u));
            return;
        }
        case 0x14B6F8u:
        {
            const int n = meshList(base, seg, true, false);
            setGpr(ctx, 4, base | seg);
            if (g_rng() % 16u == 0u)
                setGpr64(ctx, 5, (g_rng() << 32) | static_cast<uint32_t>(g_rng() % 10u)); // upper bits: no cycle wanted
            else
                setGpr64(ctx, 5, g_rng() % static_cast<uint64_t>(n + 3));
            uint64_t index;
            std::memcpy(&index, &ctx.r[5], 8);
            if (((index >> 32) != 0u || static_cast<uint32_t>(index) == 0u) && n > 0)
                meshList(base, seg, false, false); // rebuilt without a cycle
            return;
        }
        case 0x14D768u:
        {
            meshList(base, seg, false, call.dueAt != 0);
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, g_rng());
            // nodes 0x100 apart: index 7 writes over the next node's link (the native declines; the original would
            // follow a garbage pointer): a 0 then, which ends the list
            const uint32_t index = static_cast<uint32_t>(g_rng() % 12u);
            if (g_meshSpacing == 0x100u && index >= 7u)
                setGpr64(ctx, 5, 0u);
            setGpr(ctx, 6, index);
            return;
        }
        case 0x15B0F0u:
        {
            const uint32_t half = static_cast<uint32_t>(g_rng());
            setU32(0x7476B0u, half);
            // the pointer below this half's limit (0x506E10 / 0x566E10) by up to 0x60000: room for most sizes, full for
            // some (Nov 23: it sat at or past the limit, so nearly every call was "full" and a wrong size went unseen)
            const uint32_t limit = 0x506E10u + (half & 1u) * 0x60000u;
            setU32(0x566E10u, limit - static_cast<uint32_t>(g_rng() % 0x60010u));
            setGpr(ctx, 4, (g_rng() % 8u == 0u) ? static_cast<uint32_t>(g_rng()) : static_cast<uint32_t>(g_rng() % 0x2000u));
            return;
        }
        case 0x155AE8u:
        case 0x155BE8u:
        case 0x155C68u:
            randomBytes(0x4A5D80u, 0x808u);
            setGpr64(ctx, fn.address == 0x155C68u ? 6 : 5, gsIndex());
            if (fn.address == 0x155AE8u)
            {
                setGpr(ctx, 4, base | seg);
                setGpr(ctx, 6, static_cast<uint32_t>(g_rng() % 4u));
            }
            else if (fn.address == 0x155BE8u)
            {
                setGpr(ctx, 4, base | seg);
                setGpr(ctx, 6, static_cast<uint32_t>(g_rng() % 8u));
                for (uint32_t k = 0; k < 8; ++k)
                    setU32(base + 0x220u + 4u * k, (0x60000u + 0x80u * static_cast<uint32_t>(g_rng() % 16u)) | seg);
            }
            else
            {
                setGpr(ctx, 4, (base + 8u * static_cast<uint32_t>(g_rng() % 32u)) | seg);
                setGpr(ctx, 5, (base + 8u * static_cast<uint32_t>(g_rng() % 32u)) | seg);
            }
            return;
        case 0x150DC8u:
        case 0x150F68u:
        case 0x151168u:
        {
            orderTable(mirror);
            const uint32_t pick = static_cast<uint32_t>(g_rng() % 12u);
            setU32(base + 0x3Cu, pick == 0 ? 0x280u : pick == 1 ? 0x27Fu : static_cast<uint32_t>(g_rng() % 12u));
            setU32(0x747A2Cu, static_cast<uint32_t>(g_rng()) & ((g_rng() % 4u == 0u) ? ~0u : ~0x800u));
            const uint64_t kind = g_rng() % 6u;
            const uint32_t sign = (g_rng() % 4u == 0u) ? 0xFFFFFFFFu : static_cast<uint32_t>(g_rng() % 2u);
            setGpr(ctx, 4, base | seg);
            if (fn.address == 0x150DC8u)
            {
                const uint32_t tex = 0x70000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
                randomBytes(0x70000u, 0x1000u);
                setU32(base + 0x304u, tex | seg);
                setGpr(ctx, 5, (base + 0x300u) | seg);
                setGpr(ctx, 6, static_cast<uint32_t>(g_rng()));
                setGpr(ctx, 7, sign);
                setGpr64(ctx, 8, kind);
                return;
            }
            setU32(sp, static_cast<uint32_t>(kind)); // the kind, on the caller's stack
            const uint32_t formats[6] = {0u, 0x13u, 0x14u, static_cast<uint32_t>(g_rng() % 30u), 0xFFFFFFFFu, 0x13u};
            setGpr(ctx, 5, static_cast<uint32_t>(g_rng()));
            setGpr(ctx, 6, formats[g_rng() % 6u]);
            setGpr(ctx, 7, static_cast<uint32_t>(g_rng()));
            setGpr64(ctx, 8, g_rng());
            setGpr(ctx, 9, (g_rng() % 10u == 0u) ? 0u : static_cast<uint32_t>(g_rng() % 1024u) - (g_rng() % 8u == 0u ? 512u : 0u));
            setGpr(ctx, 10, static_cast<uint32_t>(g_rng() % 1024u) - (g_rng() % 8u == 0u ? 512u : 0u));
            setGpr(ctx, 11, sign);
            return;
        }
        case 0x2D8070u:
        {
            floatsAt(base + 0x16150u, 5, wild);
            if (g_rng() % 8u == 0u) // near == far: a zero divisor
                setU32(base + 0x16154u, 0u), setU32(base + 0x16150u, 0u);
            for (uint32_t k = 0; k < 3; ++k)
                if (g_rng() % 2u)
                    setFloat(base + 0x16158u + 4u * k, static_cast<float>(static_cast<int>(g_rng() % 600u) - 100) / 2.0f);
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, (base + 0x200u) | seg);
            setGpr(ctx, 6, (base + 0x300u + 8u * static_cast<uint32_t>(g_rng() % 8u)) | seg);
            const uint32_t f = inputFloat(wild);
            std::memcpy(&ctx.f[12], &f, 4);
            return;
        }
        case 0x14BE38u:
        {
            const uint32_t node = base + 0x100u;
            setU32(node + 0x380u, static_cast<uint32_t>(g_rng()));
            setU32(base + 0x250u, static_cast<uint32_t>(g_rng()));
            if (g_rng() % 2u)
            {
                setFloat(base + 0x24Cu, 1.0f);
                setByte(base + 0x2B6u, 255u), setByte(base + 0x2B7u, 255u), setByte(base + 0x2BCu, 255u);
            }
            else
            {
                setU32(base + 0x24Cu, inputFloat(wild));
                setByte(base + 0x2B6u, static_cast<uint8_t>(g_rng())), setByte(base + 0x2B7u, static_cast<uint8_t>(g_rng()));
                setByte(base + 0x2BCu, static_cast<uint8_t>(g_rng()));
            }
            setU32(0x747AA8u, (g_rng() % 2u) ? 0u : static_cast<uint32_t>(g_rng()));
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, node | seg);
            return;
        }
        case 0x1503B8u:
            setGpr(ctx, 4, base | seg);
            if (g_rng() % 16u != 0u)
                setGpr(ctx, 5, (base + 0x200u) | seg);
            return;
        case 0x2D8008u:
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, (base + 0x100u) | seg);
            setGpr(ctx, 6, (base + 0x200u + 4u * static_cast<uint32_t>(g_rng() % 8u)) | seg);
            setGpr(ctx, 8, (base + 0x300u) | seg);
            return;
        case 0x2D7E50u:
            floatsAt(base + 0x161A0u, 24, wild);
            for (uint32_t v = 0x161A0u; v <= 0x161C0u && g_rng() % 2u; v += 0x10u)
                setU32(base + v + 12u, 0u); // light w lanes as the game has them
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 6, (0x80000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | seg);
            setGpr(ctx, 7, (0x90000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | seg);
            setGpr(ctx, 9, (base + 0x380u + 4u * static_cast<uint32_t>(g_rng() % 4u)) | seg);
            return;
        case 0x1F8DF8u:
        {
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, (base + 0x100u) | seg);
            setU32(0x5CAFF0u, (g_rng() % 8u == 0u) ? (base + 0x140u) | seg : (base + 0x200u) | seg);
            const uint32_t f = inputFloat(wild);
            std::memcpy(&ctx.f[12], &f, 4);
            return;
        }
        case 0x14C5A0u: // batch 3: queue the model's packet (cModel at base; decomp field names in packet.cpp)
        case 0x14C6B8u: // b3 B: cModel_queuePacket, the same function
        {
            orderTable(mirror);
            const uint32_t node = 0x30000u + 0x40u * static_cast<uint32_t>(g_rng() % 64u);
            setU32(base + 0x25Cu, g_rng() % 16u == 0u ? 0u : node | seg); // meshHead (0: the flags come from 0x380)
            const uint32_t flags = static_cast<uint32_t>(g_rng());
            const uint32_t pick = static_cast<uint32_t>(g_rng() % 4u);
            setU32(node + 0x380u, pick == 0 ? flags & ~0x4080u : pick == 1 ? flags | 0x4080u : flags);
            setU32(0x380u, static_cast<uint32_t>(g_rng()));
            for (uint32_t half = 0; half < 2; ++half)
            {
                const uint32_t packet = 0x60000u + 0x80u * static_cast<uint32_t>(g_rng() % 256u);
                setU32(base + 0x220u + 4u * half, packet | mirror); // (a packet at -0x70 would send the original to 0xFFFFFF90)
                randomBytes(packet + 0x70u, 0x10u); // the packet's last tag (0x31A650's t0 = start + 0x70)
            }
            const uint32_t kinds = static_cast<uint32_t>(g_rng() % 8u);
            setU32(base + 0x2C0u, kinds < 4 ? 0xAu : kinds == 4 ? static_cast<uint32_t>(g_rng()) & 0x7FFFFFFFu :
                                  static_cast<uint32_t>(g_rng() % 12u)); // per-node, any (non-negative: a negative kind
                                                                         // indexes the table backwards in the original
                                                                         // too), or 0..11 (10+ out of range)
            setU32(base + 0x2ACu, static_cast<uint32_t>(g_rng()));
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, g_rng()); // short id: only the low 16 bits count
            // the stack clear of the data, or (1 in 16) its frame over the model's id: the original's sq s1 lands on it
            // (s1's upper half 0: an index of 0, harmless) and the native declines when it reads the id
            const bool over = g_rng() % 16u == 0u;
            const uint32_t stack = over ? base + 0x2C0u : 0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
            if (over)
                std::memset(reinterpret_cast<uint8_t *>(&ctx.r[17]) + 8, 0, 8);
            setGpr(ctx, 29, stack | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x151398u: // batch 3: the texture cache's lookup (manager w at 0x80000 + ...; tables below w + 0x3DC50)
        {
            uint64_t name, type;
            const uint32_t w = 0x80000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u);
            if (g_rng() % 2u == 0u) // batches 3-4b: a miss with room (the name is the file: insert + builder)
            {
                const uint32_t f = textureFileAt();
                textureFile(static_cast<int>(g_rng() % 4u), f);
                textureManager(w);
                type = textureType();
                textureMissTable(w, type, f);
                name = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(f | (g_rng() % 8u == 0u ? 0x20000000u : 0u))));
            }
            else
                textureTable(w, name, type, 0x10000u);
            setGpr(ctx, 4, w | seg);
            setGpr64(ctx, 5, name);
            setGpr64(ctx, 6, type);
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x14B9A0u: // batch 3: a node's UV-scroll packet (cModel at base, its node at 0x30000 + ...)
        {
            orderTable(mirror);
            const uint32_t node = 0x30000u + 0x800u * static_cast<uint32_t>(g_rng() % 8u);
            const uint32_t half = [] { uint32_t v; std::memcpy(&v, g_ramA + 0x7476B0u, 4); return v & 1u; }();
            setU32(base + 0x250u, static_cast<uint32_t>(g_rng())); // objFlags (CMODEL_F_LATE 0x2000: kind 0x14, else 2)
            for (uint32_t h = 0; h < 2u; ++h)
                setU32(base + 0x220u + 4u * h, (0x68000u + 0x80u * static_cast<uint32_t>(g_rng() % 64u)) | mirror);
            for (uint32_t k = 0; k < 2u * 8u + 16u; k += 4u) // GS table entries 2, 3, 0x14, 0x15
                setU32(0x4A5D80u + 2u * 8u + k, static_cast<uint32_t>(g_rng())), setU32(0x4A5D80u + 0x14u * 8u + k, static_cast<uint32_t>(g_rng()));
            for (uint32_t k = 0; k < 0x40u; k += 4u)
                setU32(0x3BD860u + k, inputFloat(wild)), setU32(0x4A6940u + k, static_cast<uint32_t>(g_rng()));
            floatsAt(node + 0x38Cu, 2, wild);
            randomBytes(node + 0x90u, 8u), randomBytes(node + 0xB0u + 0x90u, 8u);
            const uint32_t flags = static_cast<uint32_t>(g_rng());
            setU32(node + 0x380u, g_rng() % 2u ? flags & ~0x4080u : flags | 0x4080u);
            const uint32_t kinds = static_cast<uint32_t>(g_rng() % 8u);
            setU32(base + 0x2C0u, kinds < 4 ? 0xAu : static_cast<uint32_t>(g_rng() % 12u));
            setU32(base + 0x2ACu, static_cast<uint32_t>(g_rng()));
            // the allocator: room in this half's area (it ends at 0x506E10 / 0x566E10), or (1 in 8) full
            const uint32_t limit = 0x506E10u + half * 0x60000u;
            const uint32_t room = g_rng() % 8u == 0u ? 0x100u : 0x120u + 0x10u * static_cast<uint32_t>(g_rng() % 0x400u);
            const uint32_t cur = half ? 0x506E10u + 0x10u * static_cast<uint32_t>(g_rng() % 0x900u) : limit - room;
            setU32(0x566E10u, cur);
            for (uint32_t k = 0; k < 0x120u && cur + k + 4u <= 0x510000u && cur >= 0x4F8000u; k += 4u)
                setU32(cur + k, static_cast<uint32_t>(g_rng())); // old bytes (the packet's tail tag is read back)
            if (g_rng() % 16u == 0u) // the slot tags over the node's uvScroll (read after them): the native declines
                setU32(base + 0x220u + 4u * half, node + 0x38Cu - 0x50u);
            if (g_rng() % 32u == 0u) // a broken vf0: UnitMatrix's result would follow it; the native declines
                ctx.vu0_vf[0] = _mm_set_ps(2.0f, 0.0f, 0.0f, 0.0f);
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, node | seg);
            setGpr64(ctx, 6, g_rng());
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x148BD8u: // batch 3: a mesh node's texture registers (node at base; the manager D_004664A0)
        {
            uint64_t name, type;
            const bool miss = g_rng() % 2u == 0u; // batches 3-4b: the texture's first lookup (insert + builder)
            uint32_t f = 0;
            if (miss)
            {
                f = textureFileAt();
                textureFile(static_cast<int>(g_rng() % 4u), f);
                textureManager(0x4664A0u);
                setU16(0x4664A0u + 0x3F8A8u, static_cast<uint16_t>(g_rng() % 64u)); // the rows below stay random
                type = textureType();
                textureMissTable(0x4664A0u, type, f);
                name = f;
            }
            else
                textureTable(0x4664A0u, name, type, 64u); // VRAM blocks 0..63: register rows at 0x4A4128 + 16 * block
            for (uint32_t row = 0; row < 64u * 16u + 8u; row += 4u)
                setU32(0x4A4128u + row, static_cast<uint32_t>(g_rng()));
            setU32(0x7476B0u, static_cast<uint32_t>(g_rng()));
            const uint32_t data = 0x70000u + 0x100u * static_cast<uint32_t>(g_rng() % 32u);
            const uint32_t n = g_rng() % 8u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 16u);
            setU32(base + 0x388u, data | seg);
            setU32(data + 8u, (n << 16) | static_cast<uint32_t>(g_rng() % 0x10000u)); // +0xA: the mesh count
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t entry = 0x100u + 0x20u * static_cast<uint32_t>(g_rng() % 64u); // from data
                setU32(data + 0x20u + 4u * i, entry);
                setByte(data + entry + 0x16u, g_rng() % 4u == 0u ? 0xFFu : static_cast<uint8_t>(g_rng()));
                for (uint32_t h = 0; h < 2u; ++h) // the packets' words, both halves
                    setU32(base + 0x300u + 0x40u * h + 4u * i, (0x60000u + 0x40u * static_cast<uint32_t>(g_rng() % 256u)) | mirror);
            }
            setByte(base + 0x387u, static_cast<uint8_t>(g_rng()));
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, type);
            setGpr64(ctx, 6, name);
            setGpr64(ctx, 7, g_rng());
            // the stack clear of the data, or (1 in 16) one packet word pointing into another mesh's table entry
            // read later (a write onto a read: the native declines)
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            if (n > 1u && g_rng() % 16u == 0u)
            {
                uint32_t later;
                std::memcpy(&later, g_ramA + data + 0x20u + 4u * (n - 1u), 4);
                for (uint32_t h = 0; h < 2u; ++h)
                    setU32(base + 0x300u + 0x40u * h, data + later + 0x16u - 0x20u - 4u); // its sd covers the byte
            }
            return;
        }
        case 0x151630u: // batches 3-4b: the classification (a file's header)
        {
            const uint32_t f = textureFileAt();
            textureFile(static_cast<int>(g_rng() % 4u), f);
            if (g_rng() % 4u == 0u)
                randomBytes(f, 8u);
            setGpr(ctx, 4, f | seg);
            return;
        }
        case 0x151990u: // batches 3-4b: the builders (w, the file, pa / pb, the kind)
        case 0x151CC0u:
        case 0x151688u:
        case 0x151FA0u:
        {
            const int kind = fn.address == 0x151990u ? 0 : fn.address == 0x151CC0u ? 1 : fn.address == 0x151688u ? 2 : 3;
            const uint32_t w = 0x80000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u);
            const uint32_t f = textureFileAt();
            textureFile(kind, f);
            textureManager(w);
            const uint32_t pa = w + 0x3DC60u + 0xCu * static_cast<uint32_t>(g_rng() % 3u);
            setGpr(ctx, 4, w | seg);
            setGpr(ctx, 5, f | (g_rng() % 8u == 0u ? 0x20000000u : 0u));
            setGpr(ctx, 6, pa | seg);
            setGpr(ctx, 7, (pa + 4u) | seg);
            setGpr64(ctx, 8, textureType());
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x148D30u: // batches 3-4b: a node's textures by mesh (node at base, the room's pack, the manager D_004664A0)
        {
            const uint32_t w = 0x4664A0u;
            textureManager(w);
            setU16(w + 0x3F8A8u, static_cast<uint16_t>(g_rng() % 64u));
            for (uint32_t row = 0; row < 128u * 16u + 8u; row += 4u)
                setU32(w + 0x3DC88u + row, static_cast<uint32_t>(g_rng()));
            const uint64_t kind = textureType();
            // the cache: a few random names, then the files' names (hits) unless they should miss
            const uint32_t start = w + (kind == 2u ? 0x3D2F0u : kind == 1u ? 0x3DC50u : 0x3C990u);
            uint32_t entries = 0;
            auto cache = [&](uint32_t name) {
                setU32(start - 0x10u * entries, name), setU32(start - 0x10u * entries + 12u, static_cast<uint32_t>(g_rng() % 64u));
                ++entries;
            };
            for (uint32_t i = g_rng() % 6u; i > 0; --i)
                cache(static_cast<uint32_t>(g_rng() % 0x7FFFFu) | 0x100000u);
            const bool named = g_rng() % 2u == 0u;
            const bool misses = g_rng() % 2u == 0u;
            uint32_t pack;
            if (named)
            {
                // a pack at 0xC8000: count, offsets, names ("T32" / "TM2" / others), its files (TIM2 / TIM3 / the pack)
                pack = 0xC8000u + 0x100u * static_cast<uint32_t>(g_rng() % 8u);
                const uint32_t count = g_rng() % 16u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 6u);
                setU32(pack, count);
                for (uint32_t i = 1; i <= count; ++i)
                {
                    const uint32_t f = 0xD0000u + 0x800u * ((i - 1u) % 16u);
                    textureFile(static_cast<int>(g_rng() % 3u), f);
                    setU32(pack + 4u * i, f - pack);
                    const uint32_t pick = static_cast<uint32_t>(g_rng() % 3u);
                    setU32(pack + 4u * (count + i), pick == 0 ? 0x00323354u : pick == 1 ? 0x00324D54u : static_cast<uint32_t>(g_rng()));
                    if (!misses || g_rng() % 2u)
                        cache(f);
                }
                cache(0u); // a slot with no such entry looks up name 0
            }
            else
            {
                pack = textureFileAt();
                const int fileKind = static_cast<int>(g_rng() % 4u);
                textureFile(fileKind, pack);
                if (fileKind == 3) // a TM pack: its pictures' names (all cached: they aren't files of their own)
                    for (uint32_t i = 0; i < 8u; ++i)
                        cache(pack + 0x400u + 0x40u * i);
                else if (!misses)
                    cache(pack);
            }
            setU32(start - 0x10u * entries, 0xFFFFFFFFu);
            setU32(0x747A9Cu, g_rng() % 16u == 0u ? 0u : pack); // D_00747A9C
            setU32(base + 0x380u, (static_cast<uint32_t>(g_rng()) & ~0x02000000u) | (named ? 0x02000000u : 0u));
            const uint32_t data = 0x70000u + 0x100u * static_cast<uint32_t>(g_rng() % 32u);
            const uint32_t n = g_rng() % 8u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 8u);
            setU32(base + 0x388u, data | seg);
            setU32(data + 8u, (n << 16) | static_cast<uint32_t>(g_rng() % 0x10000u));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t entry = 0x100u + 0x20u * static_cast<uint32_t>(g_rng() % 64u);
                setU32(data + 0x20u + 4u * i, entry);
                setByte(data + entry + 0x16u, g_rng() % 4u == 0u ? 0xFFu : static_cast<uint8_t>(g_rng() % 8u));
                for (uint32_t h = 0; h < 2u; ++h)
                    setU32(base + 0x300u + 0x40u * h + 4u * i, (0x60000u + 0x40u * static_cast<uint32_t>(g_rng() % 256u)) | mirror);
            }
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, kind);
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x148690u: // batches 3-4b: a node's material textures (node at base; +0x400 a name / file / pack by variant)
        {
            const uint32_t w = 0x4664A0u;
            textureManager(w);
            setU16(w + 0x3F8A8u, static_cast<uint16_t>(g_rng() % 64u));
            for (uint32_t row = 0; row < 128u * 16u + 8u; row += 4u)
                setU32(w + 0x3DC88u + row, static_cast<uint32_t>(g_rng()));
            const uint64_t kind = textureType();
            const uint32_t start = w + (kind == 2u ? 0x3D2F0u : kind == 1u ? 0x3DC50u : 0x3C990u);
            uint32_t entries = 0;
            auto cache = [&](uint32_t name) {
                setU32(start - 0x10u * entries, name), setU32(start - 0x10u * entries + 12u, static_cast<uint32_t>(g_rng() % 64u));
                ++entries;
            };
            for (uint32_t i = g_rng() % 6u; i > 0; --i)
                cache(static_cast<uint32_t>(g_rng() % 0x7FFFFu) | 0x100000u);
            const uint32_t how = static_cast<uint32_t>(g_rng() % 4u); // 0 / 3: one lookup, 1: getSlotPtr, 2: SearchDataByTwoNames
            const bool misses = g_rng() % 2u == 0u;
            uint32_t named;
            if (how == 2u)
            {
                named = 0xC8000u + 0x100u * static_cast<uint32_t>(g_rng() % 8u);
                const uint32_t count = g_rng() % 16u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 6u);
                setU32(named, count);
                for (uint32_t i = 1; i <= count; ++i)
                {
                    const uint32_t f = 0xD0000u + 0x800u * ((i - 1u) % 16u);
                    textureFile(static_cast<int>(g_rng() % 3u), f);
                    setU32(named + 4u * i, f - named);
                    const uint32_t pick = static_cast<uint32_t>(g_rng() % 3u);
                    setU32(named + 4u * (count + i), pick == 0 ? 0x00323354u : pick == 1 ? 0x00324D54u : static_cast<uint32_t>(g_rng()));
                    if (!misses || g_rng() % 2u)
                        cache(f);
                }
                cache(0u);
            }
            else
            {
                named = textureFileAt();
                const int fileKind = how == 1u ? static_cast<int>(g_rng() % 4u) : static_cast<int>(g_rng() % 3u);
                textureFile(fileKind, named);
                if (fileKind == 3)
                    for (uint32_t i = 0; i < 8u; ++i)
                        cache(named + 0x400u + 0x40u * i);
                else if (!misses)
                    cache(named);
            }
            setU32(start - 0x10u * entries, 0xFFFFFFFFu);
            const uint32_t variant = how == 0u ? 0x04000000u : how == 1u ? 0x08000000u : how == 2u ? 0x02000000u : 0u;
            setU32(base + 0x380u, (static_cast<uint32_t>(g_rng()) & ~0x0E000004u) | variant | (g_rng() % 8u == 0u ? 4u : 0u));
            setU32(base + 0x400u, named);
            setU32(base + 0x410u, g_rng() % 4u == 0u ? 0x40000000u : static_cast<uint32_t>(g_rng()) & ~0x40000000u);
            setByte(base + 0x387u, static_cast<uint8_t>(g_rng()));
            const uint32_t remap = g_rng() % 2u ? 0u : 0xC9000u + 0x100u * static_cast<uint32_t>(g_rng() % 4u);
            if (remap)
                for (uint32_t i = 0; i < 256u; ++i)
                    setByte(remap + i, g_rng() % 6u == 0u ? 0xFFu : static_cast<uint8_t>(g_rng() % 8u));
            const uint32_t data = 0x70000u + 0x100u * static_cast<uint32_t>(g_rng() % 32u);
            const uint32_t n = g_rng() % 8u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 8u);
            setU32(base + 0x388u, data | seg);
            setU32(data + 8u, (n << 16) | static_cast<uint32_t>(g_rng() % 0x10000u));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t entry = 0x100u + 0x20u * static_cast<uint32_t>(g_rng() % 64u);
                setU32(data + 0x20u + 4u * i, entry);
                setByte(data + entry + 0x16u, g_rng() % 5u == 0u ? 0xFFu : static_cast<uint8_t>(g_rng() % 8u));
                for (uint32_t h = 0; h < 2u; ++h)
                    setU32(base + 0x300u + 0x40u * h + 4u * i, (0x60000u + 0x40u * static_cast<uint32_t>(g_rng() % 256u)) | mirror);
            }
            setGpr(ctx, 4, base | seg);
            setGpr64(ctx, 5, kind);
            setGpr64(ctx, 6, remap ? remap | (g_rng() % 4u == 0u ? 0x20000000u : 0u) : (g_rng() % 4u == 0u ? 0u : 0u));
            setGpr64(ctx, 7, g_rng());
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x14B4D0u: // step a: cModel_setNodeMaterial (model at 0x38000.., node at base; its callee's setup below)
        case 0x14B5D8u: // step a: ForwardAttackByMode (the same)
        {
            const bool material = fn.address == 0x14B4D0u;
            // the callee's path: the special pass (0x148BD8, 1 in 3) or the other (0x148690 / 0x148D30); its own setup
            // fills the node, the manager and the files, and its registers say what the model must hold
            const bool special = g_rng() % 3u == 0u;
            NativeFunction callee = fn;
            callee.address = special ? 0x148BD8u : material ? 0x148690u : 0x148D30u;
            setupBatch2(callee, call, wild, base, seg);
            uint64_t a1, a2;
            std::memcpy(&a1, &ctx.r[5], 8);
            std::memcpy(&a2, &ctx.r[6], 8);
            const uint32_t model = 0x38000u + 0x400u * static_cast<uint32_t>(g_rng() % 8u);
            randomBytes(model, 0x2E4u);
            setU32(model + 0x2C4u, static_cast<uint32_t>(a1)); // drawPrio: the callee's kind
            // the special pass: the node's flag and D_00747AA8 (the pass's name) both set; the other path: one of them
            // (or both) clear, so a random name never reaches the cache (its miss would read a file at a random address)
            const uint32_t clear = special ? 0u : 1u + static_cast<uint32_t>(g_rng() % 3u); // 1: the flag, 2: the pass, 3: both
            setU32(0x747AA8u, special ? static_cast<uint32_t>(a2) : (clear & 2u) ? 0u : static_cast<uint32_t>(g_rng()) | 1u);
            const bool flagOn = special || !(clear & 1u);
            uint32_t flags;
            std::memcpy(&flags, g_ramA + base + 0x380u, 4);
            if (material)
            {
                // the node: CMODEL_NODE_SPECIAL as above, CMODEL_NODE_ALPHA half the time
                flags = (flags & ~0x60000000u) | (flagOn ? 0x40000000u : 0u) |
                        (g_rng() % 2u ? 0x20000000u : 0u);
                setU32(base + 0x380u, flags);
                // the alpha: 1.0 x 255 x 255 x 255 (mode 3), near it, random, or past the host's exact range (declines)
                const uint32_t pick = static_cast<uint32_t>(g_rng() % 8u);
                const uint32_t tint = pick < 3 ? 0x3F800000u : pick == 3 ? 0x3F800001u : pick == 4 ? 0x3F7FFFFFu
                                      : pick == 5 ? inputFloat(wild) : pick == 6 ? 0x7F000000u | static_cast<uint32_t>(g_rng() % 0x800000u)
                                      : static_cast<uint32_t>(g_rng() % 0x800000u); // a denormal
                setU32(model + 0x24Cu, tint);
                const bool full = pick < 4 || g_rng() % 2u;
                setByte(model + 0x2B6u, full ? 255u : static_cast<uint8_t>(g_rng()));
                setByte(model + 0x2B7u, full ? 255u : static_cast<uint8_t>(g_rng()));
                setByte(model + 0x2BCu, full ? 255u : static_cast<uint8_t>(g_rng()));
                // the texture exchange table: the callee's remap (its a2) copied to texSlot when it had one
                const bool exchange = !special && a2 != 0u;
                uint32_t texFlags;
                std::memcpy(&texFlags, g_ramA + model + 0x254u, 4);
                setU32(model + 0x254u, (texFlags & ~0x10000000u) | (exchange ? 0x10000000u : 0u));
                if (exchange)
                    for (uint32_t i = 0; i < 256u; ++i)
                        setByte(model + 0x2D4u + i, g_ramA[(static_cast<uint32_t>(a2) & 0x1FFFFFFu) + i]);
            }
            else
            {
                // node->info->flags: CMODEL_MESH_SPECIAL as the path wants
                const uint32_t info = 0x3A000u + 0x40u * static_cast<uint32_t>(g_rng() % 16u);
                setU32(base + 0x414u, info | seg);
                uint32_t mesh = static_cast<uint32_t>(g_rng()) & ~0x200000u;
                if (flagOn)
                    mesh |= 0x200000u;
                setU32(info + 0x34u, mesh);
            }
            setGpr(ctx, 4, model | seg);
            setGpr(ctx, 5, base | seg);
            return;
        }
        case 0x154240u: // step a: a node's lighting block (model at 0x38000.., node at base; the paths picked evenly)
        {
            const uint32_t model = 0x38000u + 0x400u * static_cast<uint32_t>(g_rng() % 8u);
            randomBytes(model, 0x2E4u);
            randomBytes(base + 0x380u, 0x98u);
            const uint32_t path = static_cast<uint32_t>(g_rng() % 4u); // 0x800000, 0x410's bit, 2D8008, 2D7E50
            uint32_t flags = static_cast<uint32_t>(g_rng()) & ~0x800001u;
            if (g_rng() % 16u == 0u)
                flags |= 1u; // hidden
            if (path == 0u)
                flags |= 0x800000u;
            if (g_rng() % 4u == 0u)
                flags |= 0x200000u;
            setU32(base + 0x380u, flags);
            uint32_t n410 = static_cast<uint32_t>(g_rng()) & ~0x20000000u;
            setU32(base + 0x410u, path == 1u || (path == 0u && g_rng() % 2u) ? n410 | 0x20000000u : n410);
            uint32_t m250 = static_cast<uint32_t>(g_rng()) & ~0x882000u;
            if (path == 2u || (path < 2u && g_rng() % 2u))
                m250 |= 0x800000u;
            if (g_rng() % 4u == 0u)
                m250 |= 0x80000u;
            if (g_rng() % 4u == 0u)
                m250 |= 0x2000u;
            setU32(model + 0x250u, m250);
            // the colour: sometimes w exactly 1.0 (the env pass)
            for (uint32_t o : {0x240u, 0x244u, 0x248u, 0x24Cu})
                setU32(model + o, inputFloat(wild));
            for (uint32_t o : {0x3A0u, 0x3A4u, 0x3A8u, 0x3ACu, 0x38Cu, 0x390u})
                setU32(base + o, inputFloat(wild));
            if (g_rng() % 2u)
            {
                setU32(model + 0x24Cu, 0x3F800000u);
                setU32(base + 0x3ACu, 0x3F800000u);
                setByte(model + 0x2B6u, 255u);
                setByte(model + 0x2B7u, 255u);
                setByte(model + 0x2BCu, 255u);
            }
            // pointers: the model's position, the camera, the mesh info, this frame's packets
            const uint32_t pos = 0x3C000u + 0x10u * static_cast<uint32_t>(g_rng() % 64u);
            floatsAt(pos, 4, wild);
            if (g_rng() % 8u == 0u)
                floatsAt(0x3C800u + (pos & 0x3F0u), 4, wild); // unused here
            setU32(model + 0xF0u, (g_rng() % 16u == 0u ? 0x3D210u : pos) | seg); // sometimes the eye itself
            const uint32_t view = 0x3D000u + 0x10u * static_cast<uint32_t>(g_rng() % 16u);
            floatsAt(view + 0x210u, 4, wild);
            setU32(0x5CAFF0u, view | seg);
            const uint32_t info = 0x3A800u + 0x40u * static_cast<uint32_t>(g_rng() % 16u);
            randomBytes(info + 0x58u, 2u);
            setU32(base + 0x414u, info | seg);
            for (uint32_t h = 0; h < 2u; ++h)
                setU32(model + 0x220u + 4u * h, (0x3E000u + 0x40u * static_cast<uint32_t>(g_rng() % 32u)) | seg);
            const uint32_t idPick = static_cast<uint32_t>(g_rng() % 4u);
            const uint16_t id = idPick == 0 ? 0u : idPick == 1 ? static_cast<uint16_t>(g_rng() % 200u)
                                : idPick == 2 ? static_cast<uint16_t>(-static_cast<int>(g_rng() % 200u)) : static_cast<uint16_t>(g_rng());
            setByte(model + 0x2ACu, static_cast<uint8_t>(id));
            setByte(model + 0x2ADu, static_cast<uint8_t>(id >> 8));
            setU32(0x7476B0u, static_cast<uint32_t>(g_rng()));
            setU32(0x747A2Cu, g_rng() % 3u == 0u ? 0x1000u : static_cast<uint32_t>(g_rng()) & ~0x1000u);
            setU32(0x747AA8u, g_rng() % 2u ? 0u : static_cast<uint32_t>(g_rng()) | 1u);
            // the light set and the constants
            floatsAt(0x604730u, 8, wild);
            floatsAt(0x61A850u, 0xD0u / 4u, wild);
            const uint32_t near = inputFloat(false), far = inputFloat(false);
            setU32(0x61A850u, near);
            setU32(0x61A854u, g_rng() % 8u == 0u ? near : far); // a zero range sometimes (declines)
            for (uint32_t k = 0; k < 3u; ++k)
            {
                const float c = static_cast<float>(static_cast<int>(g_rng() % 400u) - 60) * 0.75f;
                uint32_t bits;
                std::memcpy(&bits, &c, 4);
                setU32(0x61A858u + 4u * k, wild ? inputFloat(true) : bits);
            }
            for (uint32_t v = 0x61A8A0u; v <= 0x61A8C0u && g_rng() % 2u; v += 0x10u)
                setU32(v + 12u, 0u); // light w lanes as the game has them
            floatsAt(0x3BD870u, 12, wild);
            floatsAt(0x4A6940u, 16, wild);
            if (g_rng() % 32u == 0u)
                std::memset(&ctx.vu0_vf[0], 0, 16); // a broken vf0: declines
            setGpr(ctx, 4, model | seg);
            setGpr(ctx, 5, base | seg);
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x153B00u: // b3 B: a scenery node's lighting block (func_00154240's setup, then this function's paths)
        {
            NativeFunction sub = fn;
            sub.address = 0x154240u;
            setupBatch2(sub, call, wild, base, seg);
            uint32_t model;
            std::memcpy(&model, &ctx.r[4], 4);
            model &= 0x1FFFFFFu;
            uint32_t flags;
            std::memcpy(&flags, g_ramA + base + 0x380u, 4);
            // the colour row: dispFlags 0x800000 / D_00747A38 0x800, else 0x2D7E50 (model 0x800000), else 0x2D8008
            const uint32_t path = static_cast<uint32_t>(g_rng() % 4u);
            flags &= ~(0x800000u | 0x8000u | 0x200000u | 0x20000u | 2u | 0x40000u | 0x10000000u);
            if (path == 0u)
                flags |= 0x800000u;
            setU32(0x747A38u, path == 1u ? static_cast<uint32_t>(g_rng()) | 0x800u : static_cast<uint32_t>(g_rng()) & ~0x800u);
            uint32_t m250;
            std::memcpy(&m250, g_ramA + model + 0x250u, 4);
            m250 = (path == 2u || g_rng() % 4u == 0u) ? m250 | 0x800000u : m250 & ~0x800000u;
            setU32(model + 0x250u, g_rng() % 4u == 0u ? m250 | 0x80000u : m250 & ~0x80000u);
            if (g_rng() % 4u == 0u)
                flags |= 0x8000u; // +0x10 = Unit
            // the fog: one of the dispFlags paths (sometimes several: the first wins)
            static constexpr uint32_t kFogBits[5] = {0x200000u, 0x20000u, 2u, 0x40000u, 0x10000000u};
            const uint32_t fogPick = static_cast<uint32_t>(g_rng() % 7u);
            if (fogPick < 5u)
                flags |= kFogBits[fogPick];
            else if (fogPick == 5u)
                for (uint32_t b : kFogBits)
                    if (g_rng() % 2u)
                        flags |= b;
            setU32(base + 0x380u, flags);
            uint32_t info;
            std::memcpy(&info, g_ramA + base + 0x414u, 4);
            info &= 0x1FFFFFFu;
            const uint32_t bits = static_cast<uint32_t>(g_rng());
            setU32(info + 0x34u, g_rng() % 3u == 0u ? bits | 0x200000u : bits & ~0x200000u); // view space
            const uint32_t forcePick = static_cast<uint32_t>(g_rng() % 4u);
            setByte(model + 0x2BEu, forcePick == 0u ? 0x18u : forcePick == 1u ? 0x1Cu : static_cast<uint8_t>(g_rng()));
            const uint32_t fogByte = static_cast<uint32_t>(g_rng() % 8u);
            setByte(model + 0x2BDu, fogByte < 4u ? 0u : fogByte == 4u ? 1u : fogByte == 5u ? 2u : static_cast<uint8_t>(g_rng()));
            // the light set's ranges and colours (+0x08..+0x27, +0x60..+0x77) and the fixed fog (D_003C3D20..3D33)
            floatsAt(0x604708u, 8, wild);
            floatsAt(0x604760u, 6, wild);
            floatsAt(0x3C3D20u, 5, wild);
            if (g_rng() % 8u == 0u) // a zero range (declines)
            {
                setU32(0x604768u, 0u), setU32(0x604764u, 0u);
                setU32(0x3C3D24u, 0u), setU32(0x3C3D20u, 0u);
            }
            for (uint32_t pair : {0x604718u, 0x604720u, 0x604764u, 0x604770u, 0x3C3D20u})
                if (g_rng() % 2u) // ordinary ranges: near < far
                {
                    const float n = static_cast<float>(g_rng() % 2000u), f = n + 1.0f + static_cast<float>(g_rng() % 30000u);
                    setFloat(pair, n), setFloat(pair + 4u, f);
                }
            if (g_rng() % 64u == 0u) // this half's packet over the block's fog words: the dead stores alias (declines)
            {
                uint32_t parity;
                std::memcpy(&parity, g_ramA + 0x7476B0u, 4);
                setU32(model + 0x220u + 4u * (parity & 1u), (base + 0x160u + 0xD0u * (parity & 1u)) | seg);
            }
            return;
        }
        case 0x2D81A0u: // b3 B: the light set's fog colour and range (set at base)
        {
            floatsAt(base + 0x16150u, 5, wild);
            if (g_rng() % 8u == 0u) // near == far: a zero divisor
                setU32(base + 0x16154u, 0u), setU32(base + 0x16150u, 0u);
            for (uint32_t k = 0; k < 3; ++k)
                if (g_rng() % 2u)
                    setFloat(base + 0x16158u + 4u * k, static_cast<float>(static_cast<int>(g_rng() % 600u) - 100) / 2.0f);
            setGpr(ctx, 4, base | seg);
            const uint32_t alias = static_cast<uint32_t>(g_rng() % 16u);
            setGpr(ctx, 5, (alias == 0u ? base + 0x16150u - 0xCu : base + 0x200u) | seg);  // out +0xC over near
            setGpr(ctx, 6, (alias == 1u ? base + 0x16150u : base + 0x300u + 8u * static_cast<uint32_t>(g_rng() % 8u)) | seg);
            return;
        }
        case 0x14BEF8u: // b3 B: cModel_nodeLayer (model at base, node at base + 0x100, its info at base + 0x300)
        {
            const uint32_t node = base + 0x100u, info = base + 0x300u;
            setU32(node + 0x380u, static_cast<uint32_t>(g_rng()));
            setU32(base + 0x250u, static_cast<uint32_t>(g_rng()));
            if (g_rng() % 2u)
            {
                setFloat(base + 0x24Cu, 1.0f);
                setByte(base + 0x2B6u, 255u), setByte(base + 0x2B7u, 255u), setByte(base + 0x2BCu, 255u);
            }
            else
            {
                setU32(base + 0x24Cu, inputFloat(wild));
                setByte(base + 0x2B6u, static_cast<uint8_t>(g_rng())), setByte(base + 0x2B7u, static_cast<uint8_t>(g_rng()));
                setByte(base + 0x2BCu, static_cast<uint8_t>(g_rng()));
            }
            setByte(base + 0x2BEu, g_rng() % 4u ? 0xFEu : static_cast<uint8_t>(g_rng()));
            setU32(node + 0x414u, info | seg);
            setU32(info + 0x34u, static_cast<uint32_t>(g_rng()));
            setU32(0x747AA8u, (g_rng() % 2u) ? 0u : static_cast<uint32_t>(g_rng()));
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, node | seg);
            return;
        }
        case 0x14B810u: // b3 B: queue a scenery node's packet (cModel at base, the node at 0x30000 + ...)
        {
            orderTable(mirror);
            const uint32_t node = 0x30000u + 0x200u * static_cast<uint32_t>(g_rng() % 32u);
            const uint32_t flags = static_cast<uint32_t>(g_rng());
            const uint32_t pick = static_cast<uint32_t>(g_rng() % 4u);
            setU32(node + 0x380u, pick == 0 ? flags & ~0x4080u : pick == 1 ? flags | 0x4080u : flags);
            for (uint32_t half = 0; half < 2; ++half)
                randomBytes(node + 0xB0u * half + 0xA0u, 0x10u); // the packet's last tag (0x31A650's t0)
            setU32(0x7476B0u, static_cast<uint32_t>(g_rng()));
            const uint32_t kinds = static_cast<uint32_t>(g_rng() % 8u);
            setU32(base + 0x2C0u, kinds < 4 ? 0xAu : kinds == 4 ? static_cast<uint32_t>(g_rng()) & 0x7FFFFFFFu :
                                  static_cast<uint32_t>(g_rng() % 12u));
            setU32(base + 0x2ACu, static_cast<uint32_t>(g_rng()));
            setGpr(ctx, 4, base | seg);
            setGpr(ctx, 5, node | seg);
            setGpr64(ctx, 6, g_rng()); // short id: only the low 16 bits count
            setGpr(ctx, 29, (0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u)) | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            return;
        }
        case 0x3A1AE8u: // the 0x14C7E8 batch: tanf (f12; the ELF's T table, sometimes wild)
        case 0x30AA40u: // func_0030AA40 = sceVu0ViewScreenMatrix (a0 = m, f12-f19, farz at the caller's 0(sp))
        case 0x14C7E8u: // func_0014C7E8: a model's per-frame matrices (model at 0x38000.., its node list at 0x48000..)
        case 0x14D0E8u: // b3 B: func_0014D0E8, the scenery variant (the same model, buffer and camera; a mesh head)
        {
            static constexpr uint32_t kT[13] = {0x3EAAAAABu, 0x3E088889u, 0x3D5D0DD1u, 0x3CB327A4u, 0x3C11371Fu,
                                                0x3B6B6916u, 0x3ABEDE48u, 0x3A1A26C8u, 0x398137B9u, 0x38A3F445u,
                                                0x3895C07Au, 0xB79BAE5Fu, 0x37D95384u};
            for (uint32_t k = 0; k < 13u; ++k)
                setU32(0x45EFF0u + 4u * k, g_rng() % 16u == 0u ? inputFloat(true) : kT[k]);
            const uint32_t spAt = 0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
            setGpr(ctx, 29, spAt | (g_rng() % 4u == 0u ? 0x80000000u : 0u));
            auto floatBits = [](float f) {
                uint32_t b;
                std::memcpy(&b, &f, 4);
                return b;
            };
            auto halfAngle = [&]() -> uint32_t { // tanf's argument: its paths and their edges
                const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
                const uint32_t sign = (g_rng() % 4u == 0u) ? 0x80000000u : 0u;
                if (pick < 6u)
                    return floatBits(static_cast<float>(g_rng() % 785398u) / 1000000.0f) | sign; // under pi/4
                if (pick < 9u)
                    return floatBits(0.7853982f + static_cast<float>(g_rng() % 1570796u) / 1000000.0f) | sign; // pi/4..3pi/4
                if (pick == 9u)
                    return (0x3F490FD8u + static_cast<uint32_t>(g_rng() % 6u)) | sign; // pi/4 either side
                if (pick == 10u)
                    return (0x3FC90FC8u + static_cast<uint32_t>(g_rng() % 0x20u)) | sign; // near pi/2 (pio2_2)
                if (pick == 11u)
                    return (0x3F2CA138u + static_cast<uint32_t>(g_rng() % 0x10u)) | sign; // kernel_tanf's 0.6744 edge
                if (pick == 12u)
                    return (static_cast<uint32_t>(g_rng()) % 0x31900000u) | sign; // tiny, denormal, 0
                if (pick == 13u)
                    return (0x4016CBE0u + static_cast<uint32_t>(g_rng() % 8u)) | sign; // 3pi/4 either side
                return inputFloat(true);
            };
            if (fn.address == 0x3A1AE8u)
            {
                const uint32_t a = halfAngle();
                std::memcpy(&ctx.f[12], &a, 4);
                return;
            }
            if (fn.address == 0x30AA40u)
            {
                uint32_t m = base + 0x40u * static_cast<uint32_t>(g_rng() % 8u);
                if (g_rng() % 32u == 0u)
                    m += 4u; // unaligned: declines
                if (g_rng() % 32u == 0u)
                    m = spAt - 0x60u; // in its own frame: declines
                setGpr(ctx, 4, m | mirror);
                for (int r = 12; r <= 19; ++r)
                {
                    const uint32_t v = inputFloat(wild && g_rng() % 4u == 0u);
                    std::memcpy(&ctx.f[r], &v, 4);
                }
                if (g_rng() % 2u) // the game's own arguments (zmin 1, zmax 65535, nearz 0.1, farz 600)
                {
                    ctx.f[17] = 1.0f;
                    ctx.f[18] = 65535.0f;
                    ctx.f[19] = 0.1f;
                }
                setU32(spAt, g_rng() % 2u ? floatBits(600.0f) : inputFloat(wild));
                return;
            }
            // func_0014C7E8
            const uint32_t self = 0x38000u + 0x400u * static_cast<uint32_t>(g_rng() % 8u);
            randomBytes(self, 0x2B8u);
            floatsAt(self, 16, wild);
            floatsAt(self + 128u, 16, wild);
            floatsAt(self + 304u, 2, wild);
            const uint32_t idx = static_cast<uint32_t>(g_rng() % 2u);
            setU32(0x7476B0u, static_cast<uint32_t>(g_rng() << 1) | idx);
            uint32_t buf = 0x3E000u + 0x200u * static_cast<uint32_t>(g_rng() % 8u);
            const uint32_t outs = 0x70000u + 0x10u * static_cast<uint32_t>(g_rng() % 64u); // clear of the nodes (0x48000..0x68000)
            const uint32_t view = 0x3D000u + 0x10u * static_cast<uint32_t>(g_rng() % 16u);
            floatsAt(view, 16, wild);
            floatsAt(view + 192u, 16, wild);
            floatsAt(0x4A6900u, 16, wild);
            uint32_t fov = floatBits(static_cast<float>(10u + g_rng() % 110u) + static_cast<float>(g_rng() % 1000u) / 1000.0f);
            const uint32_t fovPick = static_cast<uint32_t>(g_rng() % 16u);
            if (fovPick == 0u)
                fov = floatBits(90.0f);
            else if (fovPick == 1u)
                fov = floatBits(static_cast<float>(120u + g_rng() % 200u)); // past 270: tanf declines
            else if (fovPick == 2u)
                fov = inputFloat(true);
            setU32(view + 592u, fov);
            setU32(0x5CAFF0u, (view | (g_rng() % 64u == 0u ? 4u : 0u)) | seg); // sometimes unaligned: declines
            setU32(0x3C1190u, g_rng() % 4u ? floatBits(static_cast<float>(224u + g_rng() % 300u)) : inputFloat(wild));
            setU32(0x586B88u, g_rng() % 4u ? floatBits(0.75f + static_cast<float>(g_rng() % 1000u) / 1000.0f) : inputFloat(wild));
            const uint32_t widthPick = static_cast<uint32_t>(g_rng() % 16u);
            setU32(0x3C1180u, widthPick == 0u ? static_cast<uint32_t>(g_rng()) : widthPick < 8u ? 640u : static_cast<uint32_t>(g_rng() % 2048u));
            uint32_t flags = static_cast<uint32_t>(g_rng()) & ~0x1082000u;
            for (uint32_t bit : {0x2000u, 0x80000u, 0x1000000u})
                if (g_rng() % 2u)
                    flags |= bit;
            setU32(self + 592u, flags);
            // the node list: n nodes at 0x48000 + 0x200 i in a shuffled order
            const uint32_t nPick = static_cast<uint32_t>(g_rng() % 32u);
            uint32_t n = nPick == 0u ? 255u : nPick < 4u ? 1u : 1u + static_cast<uint32_t>(g_rng() % 24u);
            if (nPick == 31u && call.dueAt)
                n = 0u; // the do-while would spin 2^32 times: only when a checkpoint stops the original early
            setByte(self + 692u, static_cast<uint8_t>(n));
            uint32_t order[255];
            for (uint32_t i = 0; i < 255u; ++i)
                order[i] = i;
            for (uint32_t i = n ? n - 1u : 0u; i > 0u; --i)
                std::swap(order[i], order[g_rng() % (i + 1u)]);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t node = 0x48000u + 0x200u * order[i];
                floatsAt(node + 0x80u, 16, wild && g_rng() % 4u == 0u);
                floatsAt(node + 0xC0u, 3, wild && g_rng() % 4u == 0u);
                setU32(node + 324u, (i + 1u < n ? 0x48000u + 0x200u * order[i + 1u] : static_cast<uint32_t>(g_rng())) | seg);
            }
            const uint32_t head = 0x3F000u + 4u * static_cast<uint32_t>(g_rng() % 16u);
            setU32(head, (0x48000u + 0x200u * order[0]) | seg);
            setU32(self + 632u, head | seg);
            // aliasing: the outputs over an input (each declines)
            uint32_t outsAt = outs;
            const uint32_t alias = static_cast<uint32_t>(g_rng() % 48u);
            if (alias == 0u && n == 1u)
                outsAt = 0x48000u + 0x200u * order[0] + 0x70u; // the output over the node's own matrix
            else if (alias == 1u)
                buf = view - 0x10u; // buf+16 = the camera's matrix
            else if (alias == 2u)
                outsAt = buf + 0x100u; // the node outputs over buf+272
            else if (alias == 3u)
                buf |= 8u; // unaligned
            else if (alias == 4u)
                buf = spAt - 0x200u; // in the stack window
            setU32(self + 552u + 4u * idx, buf | seg);
            setU32(self + 560u + 4u * idx, outsAt | seg);
            setU32(self + 552u + 4u * (idx ^ 1u), static_cast<uint32_t>(g_rng()));
            setU32(self + 560u + 4u * (idx ^ 1u), static_cast<uint32_t>(g_rng()));
            if (g_rng() % 32u == 0u)
                std::memset(&ctx.vu0_vf[0], 0, 16); // a broken vf0 (the original's UnitMatrix reads it)
            if (fn.address == 0x14D0E8u)
            {
                // the mesh head (getMeshPtr(self, 0)) and its info's view-space bit; D_004A6940; sometimes no mesh
                // (the original reads address 0x414 then: the native declines)
                const uint32_t meshHead = 0x6C000u + 0x400u * static_cast<uint32_t>(g_rng() % 8u);
                const uint32_t info = 0x3A800u + 0x40u * static_cast<uint32_t>(g_rng() % 16u);
                setU32(self + 0x25Cu, g_rng() % 32u == 0u ? 0u : meshHead | seg);
                setU32(meshHead + 0x414u, info | seg);
                const uint32_t bits = static_cast<uint32_t>(g_rng());
                setU32(info + 0x34u, g_rng() % 3u == 0u ? bits | 0x200000u : bits & ~0x200000u);
                floatsAt(0x4A6940u, 16, wild);
                if (g_rng() % 48u == 0u) // the buffer over the mesh's info: a write over a read (declines)
                    setU32(self + 552u + 4u * idx, (info - 0x30u) | seg);
            }
            setGpr(ctx, 4, self | seg);
            return;
        }
        case 0x14AC90u: // step a's last item: cModel_Tag_set_rate fused (node 0 from the material callee's setup, the
                        // model from func_0014C7E8's, the other nodes cloned from node 0; each pass's flags at random)
        case 0x14AAB8u: // b3 B: cModel_Tag_set_scr fused (the same model and nodes; its own paths added at the end)
        {
            if (call.dueAt && g_rng() % 4u != 0u)
                call.dueAt = 0; // mostly whole runs: its checkpoints are many (decided first: the setups below spin
                                // the original 2^32 times only when a checkpoint stops it)
            const uint32_t node0 = 0x80000u;
            // 1. the material callee on node 0: the texture manager, its files, D_00747AA8, a model's material fields
            NativeFunction sub = fn;
            const bool forwardPath = g_rng() % 2u == 0u;
            sub.address = forwardPath ? 0x14B5D8u : 0x14B4D0u;
            setupBatch2(sub, call, wild, node0, seg);
            uint32_t matModel;
            std::memcpy(&matModel, &ctx.r[4], 4);
            matModel &= 0x1FFFFFFu;
            uint8_t material[0x2E4 + 0x100];
            std::memcpy(material, g_ramA + matModel, sizeof(material));
            orderTable(mirror); // (not every material path sets one; before the parts list, which may reach its slots)
            // 2. the matrices' setup: the model, its camera, its parts, the buffers, the stack
            sub.address = 0x14C7E8u;
            setupBatch2(sub, call, wild, base, seg);
            uint32_t self;
            std::memcpy(&self, &ctx.r[4], 4);
            self &= 0x1FFFFFFu;
            // the node matrices' outputs in an area of their own (away from the material's mesh data at 0x70000.. and
            // the order table's descriptors), and the buffer not in the stack (that setup's own-frame aliases land on
            // Tag_set_rate's saved pointers here: the original would follow garbage)
            {
                uint32_t idx;
                std::memcpy(&idx, g_ramA + 0x7476B0u, 4);
                idx &= 1u;
                uint32_t buf, outs;
                std::memcpy(&buf, g_ramA + self + 552u + 4u * idx, 4);
                std::memcpy(&outs, g_ramA + self + 560u + 4u * idx, 4);
                if ((buf & 0x1FFFFFFu) >= 0xE0000u && (buf & 0x1FFFFFFu) < 0x100000u)
                    setU32(self + 552u + 4u * idx, (0x3E000u + 0x200u * static_cast<uint32_t>(g_rng() % 8u)) | seg);
                (void)outs; // (its aliases there would run the parts' outputs over the order table's descriptors)
                setU32(self + 560u + 4u * idx, (0xA0000u + 0x10u * static_cast<uint32_t>(g_rng() % 64u)) | seg);
            }
            if (!wild) // that setup's screen values and FOV, wild a quarter of the time each: ordinary here (its own test
                       // covers them; most calls here should reach the passes after it)
            {
                uint32_t view;
                std::memcpy(&view, g_ramA + 0x5CAFF0u, 4);
                setFloat((view & 0x1FFFFF0u) + 592u, static_cast<float>(10u + g_rng() % 110u) + static_cast<float>(g_rng() % 1000u) / 1000.0f);
                setFloat(0x3C1190u, static_cast<float>(224u + g_rng() % 300u));
                setFloat(0x586B88u, 0.75f + static_cast<float>(g_rng() % 1000u) / 1000.0f);
                setU32(0x3C1180u, g_rng() % 2u ? 640u : static_cast<uint32_t>(g_rng() % 2048u));
                static constexpr uint32_t kT[13] = {0x3EAAAAABu, 0x3E088889u, 0x3D5D0DD1u, 0x3CB327A4u, 0x3C11371Fu,
                                                    0x3B6B6916u, 0x3ABEDE48u, 0x3A1A26C8u, 0x398137B9u, 0x38A3F445u,
                                                    0x3895C07Au, 0xB79BAE5Fu, 0x37D95384u}; // tanf's T, the ELF's
                for (uint32_t k = 0; k < 13u; ++k)
                    setU32(0x45EFF0u + 4u * k, kT[k]);
            }
            // the material callee's model fields: tint.w, the alpha bytes, texFlags, drawPrio, the remap table
            for (uint32_t o : {0x24Cu, 0x254u, 0x2C4u})
            {
                uint32_t v;
                std::memcpy(&v, material + o, 4);
                setU32(self + o, v);
            }
            for (uint32_t o : {0x2B6u, 0x2B7u, 0x2BCu})
                setByte(self + o, material[o]);
            for (uint32_t i = 0; i < 0x100u; ++i)
                setByte(self + 0x2D4u + i, material[0x2D4u + i]);
            // 3. the model's own fields
            uint32_t objFlags;
            std::memcpy(&objFlags, g_ramA + self + 0x250u, 4);
            for (uint32_t bit : {0x10u, 0x400u, 0x800000u})
                objFlags = g_rng() % 2u ? objFlags | bit : objFlags & ~bit;
            // the material callees as node 0 was built for (another's node fields would name files at random addresses):
            // setNodeMaterial (0x80 clear) or ForwardAttackByMode (0x400000), or (1 in 4) neither
            objFlags = (objFlags | 0x80u) & ~0x400000u;
            if (g_rng() % 4u != 0u)
                objFlags = forwardPath ? objFlags | 0x400000u : objFlags & ~0x80u;
            setU32(self + 0x250u, objFlags);
            for (uint32_t o : {0x240u, 0x244u, 0x248u})
                setU32(self + o, inputFloat(wild));
            if (g_rng() % 4u == 0u) // alpha exactly 1.0 (the env pass, mode 3, layer 0x1E)
            {
                setU32(self + 0x24Cu, 0x3F800000u);
                setByte(self + 0x2B6u, 255u), setByte(self + 0x2B7u, 255u), setByte(self + 0x2BCu, 255u);
            }
            for (uint32_t k = 0; k < 6u; ++k) // the box: centre, half sizes
                setU32(self + 0x260u + 4u * k, inputFloat(wild && g_rng() % 4u == 0u));
            const uint16_t id = static_cast<uint16_t>(g_rng() % 4u == 0u ? g_rng() : g_rng() % 200u);
            setU16(self + 0x2ACu, id);
            const uint32_t kinds = static_cast<uint32_t>(g_rng() % 8u);
            setU32(self + 0x2C0u, kinds < 4 ? 0xAu : static_cast<uint32_t>(g_rng() % 12u));
            for (uint32_t h = 0; h < 2u; ++h)
            {
                const uint32_t packet = 0x9A000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
                setU32(self + 0x220u + 4u * h, packet | mirror);
                randomBytes(packet + 0x30u, 0x50u); // the light word, the slot tags, the queue's tail tag
            }
            const uint32_t pos = 0x3C000u + 0x10u * static_cast<uint32_t>(g_rng() % 64u);
            floatsAt(pos, 4, wild);
            setU32(self + 0xF0u, pos | seg);
            setByte(self + 0x2CCu, static_cast<uint8_t>(g_rng()));
            // 4. the mesh nodes: node 0 as built, the others its clones (same textures), each its own flags and colours
            const uint32_t nodes = g_rng() % 16u == 0u ? 0u : 1u + static_cast<uint32_t>(g_rng() % 5u);
            setU32(self + 0x25Cu, nodes ? node0 | seg : 0u);
            uint32_t flags0;
            std::memcpy(&flags0, g_ramA + node0 + 0x380u, 4);
            for (uint32_t i = 0; i < nodes; ++i)
            {
                const uint32_t node = node0 + 0x800u * i;
                if (i > 0u)
                {
                    std::memcpy(g_ramA + node + 0x300u, g_ramA + node0 + 0x300u, 0x118u);
                    std::memcpy(g_ramB + node + 0x300u, g_ramA + node0 + 0x300u, 0x118u);
                    randomBytes(node, 0x300u); // its packets and lighting blocks' old bytes
                }
                uint32_t flags = (flags0 & 0x6E000004u) | (static_cast<uint32_t>(g_rng()) & ~0x6EE7C087u);
                for (uint32_t bit : {0x1u, 0x2u, 0x80u, 0x4000u, 0x10000u, 0x20000u, 0x40000u, 0x200000u, 0x800000u})
                    if (g_rng() % (bit == 1u ? 8u : bit == 0x200000u ? 4u : 2u) == 0u)
                        flags |= bit;
                setU32(node + 0x380u, flags);
                setByte(node + 0x385u, (call.dueAt && g_rng() % 40u == 0u) ? 0u : static_cast<uint8_t>(1u + g_rng() % 4u));
                setU32(node + 0x404u, i + 1u < nodes ? (node + 0x800u) | seg : 0u);
                for (uint32_t o : {0x3A0u, 0x3A4u, 0x3A8u, 0x3ACu, 0x38Cu, 0x390u})
                    setU32(node + o, inputFloat(wild));
                if (g_rng() % 2u)
                    setU32(node + 0x3ACu, 0x3F800000u);
                setU32(node + 0x410u, static_cast<uint32_t>(g_rng()));
                // its mesh info: node 0's flags (the forward path's special bit), its own far bit
                uint32_t info0, mesh;
                std::memcpy(&info0, g_ramA + node0 + 0x414u, 4);
                std::memcpy(&mesh, g_ramA + (info0 & 0x1FFFFFFu) + 0x34u, 4);
                const uint32_t info = 0x3A800u + 0x40u * i;
                setU32(node + 0x414u, info | seg);
                setU32(info + 0x34u, g_rng() % 2u ? mesh | 0x01000000u : mesh & ~0x01000000u);
                randomBytes(info + 0x58u, 2u);
            }
            // 5. the light set, the box test's thresholds, the camera's eye, the constants
            uint32_t view;
            std::memcpy(&view, g_ramA + 0x5CAFF0u, 4);
            view &= 0x1FFFFF0u;
            floatsAt(view + 0x210u, 4, wild);
            for (uint32_t o : {0x14u, 0x1Cu, 0x24u, 0x68u})
                setU32(0x604700u + o, inputFloat(wild));
            floatsAt(0x604730u, 8, wild);
            floatsAt(0x61A850u, 0xD0u / 4u, wild);
            const uint32_t nearV = inputFloat(false), farV = inputFloat(false);
            setU32(0x61A850u, nearV);
            setU32(0x61A854u, g_rng() % 8u == 0u ? nearV : farV);
            for (uint32_t k = 0; k < 3u; ++k)
                setFloat(0x61A858u + 4u * k, static_cast<float>(static_cast<int>(g_rng() % 400u) - 60) * 0.75f);
            for (uint32_t v = 0x61A8A0u; v <= 0x61A8C0u && g_rng() % 2u; v += 0x10u)
                setU32(v + 12u, 0u);
            floatsAt(0x3BD860u, 16, wild);
            floatsAt(0x4A6940u, 16, wild);
            uint32_t gsFlag;
            std::memcpy(&gsFlag, g_ramA + 0x747A2Cu, 4);
            setU32(0x747A2Cu, g_rng() % 3u == 0u ? gsFlag | 0x1000u : gsFlag & ~0x1000u);
            randomBytes(0x4A5D80u, 0x808u); // the GS table
            // 6. the frame allocator: room in this half's area, or (1 in 4) about full (a packet then lands at 0..0x8F)
            uint32_t half;
            std::memcpy(&half, g_ramA + 0x7476B0u, 4);
            half &= 1u;
            const uint32_t limit = 0x506E10u + half * 0x60000u;
            // (the edges: the first packet just fits or not, the last one just fits after it)
            static constexpr uint32_t kEdges[6] = {0x20u, 0x30u, 0x40u, 0xB0u, 0xC0u, 0xD0u};
            const uint32_t room = g_rng() % 4u == 0u ? kEdges[g_rng() % 6u] : 0x300u + 0x10u * static_cast<uint32_t>(g_rng() % 0x200u);
            const uint32_t cur = half ? 0x506E10u + 0x10u * static_cast<uint32_t>(g_rng() % 0x100u) : limit - room;
            setU32(0x566E10u, cur);
            for (uint32_t k = 0; k < 0x300u && cur + k + 4u <= 0x510000u && cur >= 0x4F8000u; k += 4u)
                setU32(cur + k, static_cast<uint32_t>(g_rng()));
            randomBytes(0u, 0x100u); // where a full allocator's packets go
            // 7. the arguments: id (only its low 16 bits count), the far flag, no_lod
            setGpr64(ctx, 5, (g_rng() & ~0xFFFFull) | id);
            setGpr64(ctx, 6, g_rng() % 8u == 0u ? g_rng() : g_rng() % 2u);
            ps2xSetPatchFlag("no_lod", g_rng() % 2u == 0u);
            if (fn.address == 0x14AAB8u)
            {
                // func_0014D0E8's fixed pass, a late model; func_00153B00's paths (view-space meshes, the colour row,
                // the layer force, the fog), cModel_nodeLayer's (back nodes, the layer force)
                uint32_t of;
                std::memcpy(&of, g_ramA + self + 0x250u, 4);
                for (uint32_t bit : {0x80000u, 0x2000u})
                    of = g_rng() % 3u == 0u ? of | bit : of & ~bit;
                setU32(self + 0x250u, of);
                setU32(0x747A38u, g_rng() % 4u == 0u ? 0x800u : 0u);
                if (g_rng() % 4u == 0u) // the far flag's upper word set (the original compares all 64 bits)
                    setGpr64(ctx, 6, (g_rng() << 32) | (g_rng() % 2u));
                const uint32_t forcePick = static_cast<uint32_t>(g_rng() % 6u);
                setByte(self + 0x2BEu, forcePick < 3u ? 0xFEu : forcePick == 3u ? 0x18u : forcePick == 4u ? 0x1Cu :
                                       static_cast<uint8_t>(g_rng() % 0x30u));
                const uint32_t fogByte = static_cast<uint32_t>(g_rng() % 8u);
                setByte(self + 0x2BDu, fogByte < 5u ? 0u : fogByte == 5u ? 1u : fogByte == 6u ? 2u : static_cast<uint8_t>(g_rng()));
                floatsAt(0x604708u, 8, wild);
                floatsAt(0x604760u, 6, wild);
                floatsAt(0x3C3D20u, 5, wild);
                for (uint32_t pair : {0x604718u, 0x604720u, 0x604764u, 0x604770u, 0x3C3D20u})
                    if (g_rng() % 2u)
                    {
                        const float n = static_cast<float>(g_rng() % 2000u), f = n + 1.0f + static_cast<float>(g_rng() % 30000u);
                        setFloat(pair, n), setFloat(pair + 4u, f);
                    }
                for (uint32_t i = 0; i < nodes; ++i) // the nodes' view-space bits and the scenery dispFlags bits
                {
                    const uint32_t info = 0x3A800u + 0x40u * i, node = node0 + 0x800u * i;
                    uint32_t m, f;
                    std::memcpy(&m, g_ramA + info + 0x34u, 4);
                    if (!(of & 0x400000u)) // (ForwardAttackByMode's material keys on it: its setup's bit stays)
                        setU32(info + 0x34u, g_rng() % 4u == 0u ? m | 0x200000u : m & ~0x200000u);
                    std::memcpy(&f, g_ramA + node + 0x380u, 4);
                    for (uint32_t bit : {0x8000u, 0x10000000u, 0x1000000u})
                        f = g_rng() % 4u == 0u ? f | bit : f & ~bit;
                    setU32(node + 0x380u, f);
                }
            }
            return;
        }
        default:
            return;
        }
    }

    // Picks the argument addresses for a function: a0 = out, a1 = matrix, a2 = matrix / vector; sometimes aliased.

    // ---- rung 4, M1 batch 1: the hierarchy leaves (motion.cpp) -------------------------------------------------------
    // Angles as the game passes them (angle(): radians, -0, denormals, some past the limits), the tables mostly the
    // ELF's (Adjust_theta_vec's 0x3BF110, ecossin's sin polynomial 0x3C3C90), rotOrders 0-5 mostly (some above, upper
    // bits random), aliasing (an input inside the output or the frames), misaligned pointers, KSEG mirrors.
    constexpr uint32_t kThetaTable[12] = {0x3EA2F983u, 0x40490FDBu, 0u, 0u, 1u, 1u, 1u, 1u,
                                          0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFEu};
    constexpr uint32_t kSinPolynomial[4] = {0x362E9C14u, 0xB94FB21Fu, 0x3C08873Eu, 0xBE2AAAA4u};
    // A rotation (three random angles, the textbook way) times a scale, with a translation: MtxInverse's usual input.
    struct Mat4f
    {
        float v[16];
    };
    void randomRigid(Mat4f &m)
    {
        const double a = angle(), b = angle(), c = angle();
        const double s = 0.25 + static_cast<double>(g_rng() % 1000u) / 250.0;
        const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
        const double r[9] = {cb * cc, cb * sc, -sb, sa * sb * cc - ca * sc, sa * sb * sc + ca * cc, sa * cb,
                             ca * sb * cc + sa * sc, ca * sb * sc - sa * cc, ca * cb};
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                m.v[4 * i + j] = static_cast<float>(r[3 * i + j] * s);
            m.v[4 * i + 3] = 0.0f;
            m.v[12 + i] = static_cast<float>((static_cast<double>(g_rng() % 2001u) - 1000.0) / 8.0);
        }
        m.v[15] = 1.0f;
    }
    // MtxInitRotVec's jump table (the ELF's .rodata); sometimes two cases swapped (the natives decline)
    void setRotOrderTable()
    {
        static constexpr uint32_t kCases[6] = {0x147788u, 0x1477B0u, 0x1477D8u, 0x147810u, 0x147848u, 0x147880u};
        const uint32_t swap = g_rng() % 32u == 0u ? static_cast<uint32_t>(g_rng() % 6u) : 6u;
        for (uint32_t k = 0; k < 6u; ++k)
            setU32(0x41EA00u + 4u * k, kCases[k == swap ? (k + 1u) % 6u : k]);
    }
    void setThetaTable()
    {
        if (g_rng() % 8u)
            for (uint32_t i = 0; i < 12u; ++i)
                setU32(0x3BF110u + 4u * i, kThetaTable[i]);
        else
            floatsAt(0x3BF110u, 4, false), randomBytes(0x3BF120u, 32u);
    }
    // The blend batch's tables (the ELF's): capVu0Atan2's c0..c7, pi/4 at 0x42C350; 0x15A190's next index {1, 2, 0}.
    constexpr uint32_t kAtanTable[9] = {0x3F7FFFF5u, 0xBEAAA61Cu, 0x3E4C40A6u, 0xBE0E6C63u, 0x3DC577DFu,
                                        0xBD6501C4u, 0x3CB31652u, 0xBB84D7E7u, 0x3F490FDBu};
    void setBlendTables(bool sometimesWrong)
    {
        for (uint32_t k = 0; k < 9u; ++k)
            setU32(0x42C350u + 4u * k, kAtanTable[k]);
        const bool wrong = sometimesWrong && g_rng() % 32u == 0u;
        setU32(0x41EE48u, wrong ? 2u : 1u), setU32(0x41EE4Cu, 2u), setU32(0x41EE50u, 0u);
    }
    // A TRS matrix: a rotation, each row scaled on its own (0.25-4.25 x 0.5-2), a translation.
    void randomTrs(Mat4f &m)
    {
        randomRigid(m);
        for (int i = 0; i < 3; ++i)
        {
            const float f = 0.5f + static_cast<float>(g_rng() % 1000u) / 666.0f;
            for (int j = 0; j < 3; ++j)
                m.v[4 * i + j] *= f;
        }
    }
    void setMat4f(uint32_t at, const Mat4f &m)
    {
        for (uint32_t k = 0; k < 16u; ++k)
            setFloat(at + 4u * k, m.v[k]);
    }
    // A blend / slerp weight: mostly 0-1, sometimes exactly 0 or 1, sometimes outside.
    float blendWeight()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
        if (pick == 0)
            return 0.0f;
        if (pick == 1)
            return 1.0f;
        if (pick == 2)
            return static_cast<float>(g_rng() % 2001u) / 1000.0f - 0.5f;
        return static_cast<float>(g_rng() % 100001u) / 100000.0f;
    }
    void setQuat(uint32_t at)
    {
        double q[4], n = 0.0;
        for (double &c : q)
            c = static_cast<double>(static_cast<int>(g_rng() % 2001u) - 1000) / 1000.0, n += c * c;
        n = n > 0.0 ? std::sqrt(n) : 1.0;
        for (uint32_t k = 0; k < 4u; ++k)
            setFloat(at + 4u * k, static_cast<float>(q[k] / n));
    }
    void setAngles(uint32_t at)
    {
        for (uint32_t i = 0; i < 3u; ++i)
            setFloat(at + 4u * i, angle());
    }
    // func_001501D0's twist (partFlags 0x800; the *_twist natives only, so the other natives' setups keep their draws):
    // the axis at 0x3BD8B0 (the ELF's (0, 1, 0, 1); sometimes a unit 4-vector or wild words) and capVu0Cos's polynomial
    // at 0x3BF140 (the ELF's: the same words as ecossin's kSinPolynomial; sometimes other values).
    void setTwistTables()
    {
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 8u);
        if (pick < 6)
            for (uint32_t k = 0; k < 4u; ++k)
                setU32(0x3BD8B0u + 4u * k, (k & 1u) ? 0x3F800000u : 0u);
        else if (pick == 6)
            setQuat(0x3BD8B0u);
        else
            floatsAt(0x3BD8B0u, 4, true);
        if (g_rng() % 8u)
            for (uint32_t k = 0; k < 4u; ++k)
                setU32(0x3BF140u + 4u * k, kSinPolynomial[k]);
        else
            floatsAt(0x3BF140u, 4, false);
    }
    // A twist angle (part +0x150): 0 as the game mostly has it, -0, the game's other values (+-pi/6, -0.0683), angles up
    // to +-3pi/2, sinf's edges (h = angle / 2 at pi/4, the near-pi/2 path, 3pi/4) and past them (declined), denormals,
    // wild words.
    void setTwistAngle(uint32_t at)
    {
        static constexpr uint32_t kEdges[] = {0x3FC90FD8u, 0x3FC90FD9u, 0x3FC90FDBu, 0x40490FD0u, 0x40490FDBu,
                                              0x40490FDFu, 0x40490FE0u, 0x4096CBE3u, 0x4096CBE4u, 0x40C90FDBu};
        const uint32_t sign = (g_rng() & 1u) << 31;
        switch (g_rng() % 10u)
        {
        case 0:
        case 1: setU32(at, 0u); break;
        case 2: setU32(at, 0x80000000u); break;
        case 3: setFloat(at, (g_rng() % 3u == 0u) ? -0.0683f : (sign ? -0.5235988f : 0.5235988f)); break;
        case 4:
        case 5: setFloat(at, (static_cast<float>(g_rng() % 2000001u) / 1000000.0f - 1.0f) * 4.712389f); break;
        case 6: setU32(at, sign | kEdges[g_rng() % (sizeof(kEdges) / 4u)]); break;
        case 7: setU32(at, sign | (static_cast<uint32_t>(g_rng()) & 0x7FFFFFu)); break;
        case 8: setFloat(at, angle()); break;
        default: setU32(at, gameFloat()); break;
        }
    }
    // M2: a cModel and its parts as the game lays them out (cModel.h): the model's pos pointer +0xF0 = &mtx[12]
    // (+0xB0), each part's anchor +0xD0 = &mtxLocal[12] (+0x30), parents the model or an earlier part (sometimes a
    // later one), parts +0x2C0 + 0x160 i after the model; flags, blend, rotOrder and partNum varied; now and then a
    // twist part (declined), a list shorter than partNum, a part in the frames or unaligned, NO_CALC with a set matrix.
    void setupModel(uint32_t address, R5900Context &ctx, uint32_t sp, uint32_t pick, uint32_t seg, uint32_t base,
                    bool twistSetup)
    {
        setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
        setRotOrderTable();
        setThetaTable();
        setBlendTables(false);
        const uint32_t model = base + (pick == 0 ? 4u : 0u);
        const uint32_t count = static_cast<uint32_t>(g_rng() % 9u); // 0-8 parts
        const uint32_t end = model + 0x2C0u + 0x160u * 8u;
        for (uint32_t a = base; a < end; a += 4u)
            setFloat(a, static_cast<float>(static_cast<int>(g_rng() % 2001u) - 1000) / 500.0f);
        auto rigidAt = [](uint32_t at) {
            Mat4f m;
            randomRigid(m);
            setMat4f(at, m);
        };
        auto trsAt = [](uint32_t at) {
            Mat4f m;
            randomTrs(m);
            setMat4f(at, m);
        };
        auto scaleAt = [](uint32_t at) {
            for (uint32_t i = 0; i < 3u; ++i)
                setFloat(at + 4u * i, 0.5f + static_cast<float>(g_rng() % 1000u) / 500.0f);
            setFloat(at + 12u, 1.0f);
        };
        rigidAt(model + 0x80u);
        setU32(model + 0xF0u, ((g_rng() % 8u) ? model + 0xB0u : model + 0x30u) | seg);
        setAngles(model + 0x100u);
        scaleAt(model + 0x110u);
        scaleAt(model + 0x120u);
        setU32(model + 0x140u, static_cast<uint32_t>(g_rng() & ~0xFFull) | (pick < 14 ? g_rng() % 6u : 6u + g_rng() % 250u));
        uint32_t objFlags = static_cast<uint32_t>(g_rng()) & ~0x40824u;
        if (g_rng() % 4u == 0u)
            objFlags |= 0x800u; // the owner builds the matrix
        if (address == 0x14A038u ? g_rng() % 8u != 0u : g_rng() % 4u == 0u)
            objFlags |= 0x20u;  // near
        if (g_rng() % 2u)
            objFlags |= 0x40000u; // 0x1501D0's unscaled path (with bit 2 clear)
        setU32(model + 0x250u, objFlags);
        setFloat(model + 0x27Cu, (g_rng() % 2u) ? 0.0f : blendWeight());
        const uint32_t listed = (g_rng() % 16u == 0u && count != 0u) ? count - 1u : count; // a short list: declined
        setU32(model + 0x2B4u, (static_cast<uint32_t>(g_rng()) & ~0xFFu) | count);
        uint32_t parts[8];
        for (uint32_t i = 0; i < 8u; ++i)
            parts[i] = model + 0x2C0u + 0x160u * i;
        setU32(model + 0x144u, (listed ? parts[0] : 0u) | seg);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t part = parts[i];
            rigidAt(part);
            trsAt(part + 0x40u);
            rigidAt(part + 0x80u);
            uint32_t anchor = part + 0x30u;
            const uint32_t a = static_cast<uint32_t>(g_rng() % 32u);
            if (a == 0)
                anchor = part + 0x20u;   // inside the rows written first: declined
            else if (a == 1)
                anchor = model + 0x30u;  // another struct's words
            else if (a == 2)
                anchor = sp - 0x200u;    // in the pass's window (below the callee's own frames): declined
            setU32(part + 0xD0u, anchor | seg);
            for (uint32_t k = 0; k < 3u; ++k)
            {
                setFloat(part + 0x100u + 4u * k, angle());
                setFloat(part + 0x130u + 4u * k, (g_rng() % 4u) ? angle() * 0.0625f : 0.0f);
            }
            scaleAt(part + 0x110u);
            scaleAt(part + 0x120u);
            // past the last part: read, never followed (random); a short list's end: 0 (the pass walks on into
            // address 0, zeroed here, as the original would)
            uint32_t next = i + 1u < listed ? parts[i + 1u] : i + 1u < count ? 0u : static_cast<uint32_t>(g_rng());
            setU32(part + 0x144u, next | seg);
            uint32_t parent = (i == 0u || g_rng() % 3u == 0u) ? model : parts[g_rng() % i];
            if (g_rng() % 16u == 0u && i + 1u < count)
                parent = parts[i + 1u]; // a later part: its world not built yet this pass
            setU32(part + 0x148u, parent | seg);
            uint32_t flags = static_cast<uint32_t>(g_rng()) & ~0x100818u;
            if (g_rng() % 4u == 0u)
                flags |= 0x8u;
            if (g_rng() % 6u == 0u)
                flags |= 0x10u;
            if (g_rng() % 4u == 0u)
                flags |= 0x100000u;
            if (g_rng() % 24u == 0u)
                flags |= 0x800u; // the twist: 0x1501D0 declines, so does the pass (the *_twist natives run it)
            if (twistSetup && g_rng() % 3u == 0u)
                flags |= 0x800u;
            setU32(part + 0x154u, flags);
            if (twistSetup)
                setTwistAngle(part + 0x150u);
        }
        if (twistSetup)
            setTwistTables();
        for (uint32_t a = 0; a < 0x200u; a += 4u)
            setU32(a, 0u);
        setGpr(ctx, 4, model | seg);
    }
    void setupMotion(const NativeFunction &fn, Call &call, bool wild, uint32_t base, uint32_t seg)
    {
        // Other natives: untouched, and no draws from g_rng (their setups stay the sequence they were proven on).
        if (fn.address != 0x1F7DA0u && fn.address != 0x1476D0u && fn.address != 0x147A08u && fn.address != 0x147728u &&
            fn.address != 0x147B60u && fn.address != 0x147BF8u && fn.address != 0x147B18u && fn.address != 0x147A48u &&
            fn.address != 0x150138u && fn.address != 0x147568u && fn.address != 0x1501D0u && fn.address != 0x156D70u &&
            fn.address != 0x15A6A0u && fn.address != 0x15A190u && fn.address != 0x1478A8u && fn.address != 0x149EB8u &&
            fn.address != 0x14A038u)
            return;
        R5900Context &ctx = call.ctx;
        const bool twistSetup = std::strstr(fn.name, "_twist") != nullptr; // the twist path's natives (rung 4, Nov 26)
        const uint32_t sp = 0xF0000u + 0x100u * static_cast<uint32_t>(g_rng() % 16u);
        const uint32_t pick = static_cast<uint32_t>(g_rng() % 16u);
        if (g_rng() % 8u) // ecossin's own polynomial (else setupCall's ordinary values)
            for (uint32_t i = 0; i < 4u; ++i)
                setU32(0x3C3C90u + 4u * i, kSinPolynomial[i]);
        switch (fn.address)
        {
        case 0x1F7DA0u: // Adjust_theta_vec(v)
        {
            setThetaTable();
            const uint32_t v = base + 0x10u * static_cast<uint32_t>(g_rng() % 8u);
            const float scale = (g_rng() % 4u) ? 1.0f : 16.0f; // several turns
            for (uint32_t i = 0; i < 3u; ++i)
                if (wild && g_rng() % 2u)
                    setU32(v + 4u * i, gameFloat());
                else
                    setFloat(v + 4u * i, angle() * scale);
            setU32(v + 12u, (g_rng() % 4u) ? gameFloat() : static_cast<uint32_t>(g_rng())); // w: carried as is
            setGpr(ctx, 4, (v + (pick == 0 ? 4u : 0u)) | seg); // unaligned: lqc2 / sqc2 take it masked
            return;
        }
        case 0x1476D0u: // MtxInitTransVec(out, t)
        case 0x147A08u: // MtxInitScaleVec(out, s)
        {
            uint32_t out = base, src = base + 0x100u;
            if (pick == 0)
                src = out + 0x30u; // t inside out: read after the writes before it
            else if (pick == 1)
                src = out + 0x34u;
            else if (pick == 2)
                src = out + 4u * static_cast<uint32_t>(g_rng() % 14u);
            else if (pick == 3)
                out += 4u * (1u + static_cast<uint32_t>(g_rng() % 3u)); // word-aligned only (Scale: rows at out & ~0xF)
            else if (pick == 4)
                out += 2u; // declined
            else if (pick == 5)
                src += 1u; // declined
            setGpr(ctx, 4, out | seg);
            setGpr(ctx, 5, src | seg);
            return;
        }
        case 0x147728u: // MtxInitRotVec(out, angles, order)
        case 0x147B60u: // MtxMulRotVec(out, A, angles, order)
        case 0x147BF8u: // MtxMulScaleVec(out, A, scale)
        case 0x147B18u: // MtxMulTransVec(out, A, t)
        {
            const bool rot = fn.address == 0x147728u || fn.address == 0x147B60u, mul = fn.address != 0x147728u;
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setRotOrderTable();
            uint32_t out = base, a = base + 0x100u, in = base + 0x200u;
            if (rot)
                setAngles(in);
            uint64_t order = pick < 11 ? g_rng() % 6u : pick < 13 ? 6u + g_rng() % 250u : g_rng() % 6u;
            if (g_rng() % 4u == 0u)
                order |= g_rng() & ~0xFFull; // andi 0xFF
            switch (g_rng() % 16u)
            {
            case 0: in = out + 4u; break;               // angles / scale inside out (InitRotVec declines)
            case 1: in = sp - 0x20u; break;             // in the frames: declined
            case 2: out = sp - 0x40u; break;            // out in the frames: declined
            case 3: out = a; break;                     // out = A
            case 4: out = a + 0x10u; break;             // out overlapping A
            case 5: a = sp - 0x80u; break;              // A in the frames: declined
            case 6: out = base + 4u; break;             // unaligned out: masked
            case 7: in = base + 0x202u; break;          // unaligned angles / scale: declined
            case 8: setU32(0x3C3C90u, 0x7F800001u); break; // a NaN coefficient: declined
            default: break;
            }
            setGpr(ctx, 4, out | seg);
            if (mul)
                setGpr(ctx, 5, a | seg);
            setGpr(ctx, mul ? 6 : 5, in | seg);
            if (rot)
                setGpr64(ctx, mul ? 7 : 6, order);
            return;
        }
        case 0x156D70u: // func_00156D70(out, A, B; f12 = t): the blend
        {
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setBlendTables(false);
            uint32_t out = base, a = base + 0x100u, b = base + 0x200u;
            Mat4f ma, mb;
            randomTrs(ma);
            const uint32_t shape = static_cast<uint32_t>(g_rng() % 16u);
            if (shape == 0)
                mb = ma;                              // the same pose: dot 1, the lerp path
            else
            {
                randomTrs(mb);
                if (shape < 4)                        // a pose close to A
                    for (int k = 0; k < 16; ++k)
                        mb.v[k] = ma.v[k] + (mb.v[k] - ma.v[k]) * 0.01f;
            }
            if (g_rng() % 4u) // TRS matrices (else setupCall's random words: declines and odd quaternions)
                setMat4f(a, ma), setMat4f(b, mb);
            if (shape == 4)
                for (uint32_t k = 0; k < 4u; ++k)
                    setU32(a + 0x10u + 4u * k, 0u);   // a zero row: sqrt(0), declined
            switch (g_rng() % 12u)
            {
            case 0: out = a; break;
            case 1: out = b; break;
            case 2: out = sp - 0x40u; break;          // in the frames: declined
            case 3: a = sp - 0x100u; break;           // declined
            case 4: b = base + 0x208u; break;         // unaligned: declined
            case 5: a = sp - 0x1E0u; break;           // its own frame's bottom (the saved ra): declined
            default: break;
            }
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, a | seg), setGpr(ctx, 6, b | seg);
            ctx.f[12] = blendWeight();
            return;
        }
        case 0x15A6A0u: // func_0015A6A0(out, q1, q2; f12 = t): slerp
        {
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setBlendTables(false);
            uint32_t out = base, q1 = base + 0x100u, q2 = base + 0x200u;
            setQuat(q1);
            const uint32_t shape = static_cast<uint32_t>(g_rng() % 8u);
            if (shape <= 1) // q2 = q1 (the lerp path) or -q1 (the sign flip)
                for (uint32_t k = 0; k < 4u; ++k)
                {
                    uint32_t w;
                    std::memcpy(&w, g_ramA + q1 + 4u * k, 4);
                    setU32(q2 + 4u * k, shape == 0 ? w : w ^ 0x80000000u);
                }
            else
                setQuat(q2);
            switch (g_rng() % 10u)
            {
            case 0: out = q1; break;
            case 1: out = q2; break;
            case 2: out = q1 + 8u; break;             // a partial overlap: declined
            case 3: out = sp - 0x30u; break;          // declined
            case 4: q1 = sp - 0x70u; break;           // its own frame's bottom (the saved ra): declined
            default: break;
            }
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, q1 | seg), setGpr(ctx, 6, q2 | seg);
            ctx.f[12] = blendWeight();
            return;
        }
        case 0x15A190u: // func_0015A190(q, m): a quaternion from m's 3x3
        {
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setBlendTables(true);
            uint32_t out = base, m = base + 0x100u;
            if (g_rng() % 4u)
            {
                Mat4f r;
                randomRigid(r);
                if (g_rng() % 2u) // unit scale: a pure rotation (all four branches show up)
                    for (int i = 0; i < 3; ++i)
                    {
                        double n = 0.0;
                        for (int j = 0; j < 3; ++j)
                            n += static_cast<double>(r.v[4 * i + j]) * r.v[4 * i + j];
                        for (int j = 0; j < 3; ++j)
                            r.v[4 * i + j] = static_cast<float>(r.v[4 * i + j] / std::sqrt(n));
                    }
                setMat4f(m, r);
            }
            switch (g_rng() % 10u)
            {
            case 0: out = m + 0x20u; break;           // q inside m's 3x3: declined
            case 1: out = m + 0x30u; break;           // row 3: not read
            case 2: out = sp - 0x20u; break;          // declined
            case 3: m = sp - 0x80u; break;            // its own frame's bottom: declined
            default: break;
            }
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, m | seg);
            return;
        }
        case 0x1478A8u: // MtxInitRotQuaternion(out, q)
        {
            uint32_t out = base, q = base + 0x100u;
            if (g_rng() % 2u)
                setQuat(q);
            const uint32_t pick2 = static_cast<uint32_t>(g_rng() % 8u);
            if (pick2 == 0)
                q = out + 0x14u;                      // q inside out: read whole first
            else if (pick2 == 1)
                out = base + 2u;                      // declined
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, q | seg);
            return;
        }
        case 0x147A48u: // MtxInitCoord(out, t, angles, scale, t0 = order)
        {
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setRotOrderTable();
            uint32_t out = base, t = base + 0x100u, angles = base + 0x200u, scale = base + 0x300u;
            setAngles(angles);
            uint64_t order = pick < 13 ? g_rng() % 6u : 6u + g_rng() % 250u;
            if (g_rng() % 4u == 0u)
                order |= g_rng() & ~0xFFull;
            switch (g_rng() % 12u)
            {
            case 0: t = out + 0x30u; break;        // &out[12], as cModel's pos: read before its words are written
            case 6: t = out + 0x2Cu; break;        // its first word is out[11], written first: declined
            case 7: t = out + 0x34u; break;
            case 1: angles = out + 0x10u; break;   // declined
            case 2: scale = out + 0x20u; break;    // declined
            case 3: scale = sp - 0x30u; break;     // in the frames: declined
            case 4: out = base + 8u; break;        // unaligned out: declined
            case 5: t = base + 0x101u; break;      // declined
            default: break;
            }
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, t | seg), setGpr(ctx, 6, angles | seg), setGpr(ctx, 7, scale | seg);
            setGpr64(ctx, 8, order);
            return;
        }
        case 0x150138u: // cParts_calcLocal(part)
        case 0x1501D0u: // func_001501D0(part, flags): the world matrix from the parent's
        {
            setGpr(ctx, 29, sp | ((g_rng() % 4u == 0u) ? 0x80000000u : 0u));
            setRotOrderTable();
            setThetaTable();
            const uint32_t part = base + (pick == 0 ? 4u : 0u); // unaligned: declined (the fields follow a0)
            uint32_t anchor = base + 0x380u, parent = base + 0x200u;
            for (uint32_t i = 0; i < 3u; ++i)
            {
                setFloat(part + 0x100u + 4u * i, angle());
                setFloat(part + 0x130u + 4u * i, (g_rng() % 4u) ? angle() * 0.0625f : 0.0f);
            }
            if (g_rng() % 4u) // ordinary scales (some 0: declined reciprocals)
                for (uint32_t i = 0; i < 3u; ++i)
                {
                    setFloat(part + 0x110u + 4u * i, 0.5f + static_cast<float>(g_rng() % 1000u) / 500.0f);
                    setFloat(parent + 0x120u + 4u * i, (g_rng() % 64u) ? 0.5f + static_cast<float>(g_rng() % 1000u) / 500.0f : 0.0f);
                }
            setU32(part + 0x154u, (g_rng() % 8u == 0u) ? 0x800u : static_cast<uint32_t>(g_rng()) & ~0x800u);
            switch (g_rng() % 12u)
            {
            case 0: anchor = part + 0x20u; break;   // calcLocal: inside the local rows written first (declined)
            case 6: case 7: case 8: case 9: anchor = part + 0x30u; break; // &mtxLocal[12]: the game's own anchor
            case 10: anchor = part + 0x2Cu; break;  // calcLocal: overlaps mtxLocal[11] (declined)
            case 1: anchor = part + 0x124u; break;  // 0x1501D0: inside scaleCalc written first (declined)
            case 2: anchor = sp - 0x40u; break;     // in the frames: declined
            case 3: parent = part; break;           // declined
            case 4: parent = sp - 0x100u; break;    // declined
            case 5: parent = base + 0x208u; break;  // unaligned: declined
            default: break;
            }
            setU32(part + 0xD0u, anchor | seg);
            setU32(part + 0x148u, parent | seg);
            setGpr(ctx, 4, part | seg);
            const uint32_t flags = (g_rng() % 2u) ? 0x40000u : static_cast<uint32_t>(g_rng() % 4u == 0u ? g_rng() : 0x40004u);
            setGpr64(ctx, 5, flags | (g_rng() & 0xFFFFFFFF00000000ull));
            if (twistSetup)
            {
                // mostly a twist part (other flag bits random half the time), its angle, the tables; now and then the
                // part moved onto the axis (0x3BD800: the world matrix the original writes covers 0x3BD8B0; declined)
                if (g_rng() % 4u)
                    setU32(part + 0x154u, 0x800u | ((g_rng() % 2u) ? static_cast<uint32_t>(g_rng()) : 0u));
                setTwistAngle(part + 0x150u);
                setTwistTables();
                if (pick != 0 && g_rng() % 32u == 0u)
                {
                    std::memcpy(g_ramA + 0x3BD800u, g_ramA + part, 0x160u);
                    std::memcpy(g_ramB + 0x3BD800u, g_ramA + part, 0x160u);
                    setGpr(ctx, 4, 0x3BD800u | seg);
                }
            }
            return;
        }
        case 0x149EB8u: // cModel_calcParts(model)
        case 0x14A038u: // cModel_calcWorldParts(model): near models only
            setupModel(fn.address, ctx, sp, pick, seg, base, twistSetup);
            return;
        case 0x147568u: // MtxInverse(out, in)
        {
            uint32_t out = base, in = base + 0x100u;
            if (g_rng() % 2u) // a rotation x scale with a translation, as the game inverts
            {
                Mat4f m;
                randomRigid(m);
                for (uint32_t k = 0; k < 16u; ++k)
                    setFloat(in + 4u * k, m.v[k]);
            }
            const uint32_t pick2 = static_cast<uint32_t>(g_rng() % 16u);
            if (pick2 == 0)
                for (uint32_t k = 0; k < 12u; ++k)
                    setU32(in + 4u * k, 0u); // det 0: the unit matrix
            else if (pick2 == 1)
                for (uint32_t k = 0; k < 4u; ++k)
                {
                    uint32_t w;
                    std::memcpy(&w, g_ramA + in + 4u * k, 4);
                    setU32(in + 0x20u + 4u * k, w); // row 2 = row 0
                }
            else if (pick2 == 2)
                out = in;                                // in place
            else if (pick2 == 3)
                in += 4u;                                // unaligned: declined
            else if (pick2 == 4)
                setFloat(in + 4u * static_cast<uint32_t>(g_rng() % 11u), 1.0e13f); // past 2^41: declined
            setGpr(ctx, 4, out | seg), setGpr(ctx, 5, in | seg);
            return;
        }
        default:
            return;
        }
    }

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
        // ecossin's coefficients (the ELF's .data at 0x3C3C90; any values must give the original's bits)
        fillFloats(g_ramA, 0x3C3C90u, 4, false);
        std::memcpy(g_ramB + 0x3C3C90u, g_ramA + 0x3C3C90u, 16);
        call.ctx.f[12] = angle();
        const float x = angle() + 1.5707964f; // ecossin's own input: pi/2 -+ an angle
        std::memcpy(&call.ctx.vu0_vf[6], &x, 4);
        if (g_rng() % 2u)
            std::memset(&call.ctx.r[7], 0, 8); // r7: ecossin's sign choice (else random, mostly non-zero)
        call.dueAt = static_cast<int>(g_rng() % 5u); // 0 = none, else the checkpoint that comes due
        setupBatch1(fn, call, wild, base, seg);
        setupBatch2(fn, call, wild, base, seg);
        setupMotion(fn, call, wild, base, seg);
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

    bool ramEqual(const uint8_t *a, const uint8_t *b)
    {
        for (const Region &r : kRegions)
            if (std::memcmp(a + r.begin, b + r.begin, r.size) != 0)
                return false;
        return true;
    }
    void copyRam(uint8_t *to, const uint8_t *from)
    {
        for (const Region &r : kRegions)
            std::memcpy(to + r.begin, from + r.begin, r.size);
    }
    void saveRam(std::vector<uint8_t> &out, const uint8_t *from)
    {
        out.resize(kUsed);
        size_t at = 0;
        for (const Region &r : kRegions)
            std::memcpy(out.data() + at, from + r.begin, r.size), at += r.size;
    }
    bool sameAsSaved(const std::vector<uint8_t> &saved, const uint8_t *ram)
    {
        size_t at = 0;
        for (const Region &r : kRegions)
        {
            if (std::memcmp(saved.data() + at, ram + r.begin, r.size) != 0)
                return false;
            at += r.size;
        }
        return true;
    }

    bool sameOutcome(const Outcome &a, const Outcome &b)
    {
        return std::memcmp(&a.ctx, &b.ctx, sizeof(R5900Context)) == 0 && a.charged == b.charged &&
               a.checkpointCalls == b.checkpointCalls && ramEqual(g_ramA, g_ramB);
    }

    // K's answer 2 (the relaxed bar): a kRenderPath native is held to what lasts (lastingContextByte, the RAM outside
    // the call's dead stack, the cycles); kChargeAtEnd ones charge fewer, larger checkpoints (their count isn't compared).
    bool sameOutcomeFor(const NativeFunction &fn, const Call &call, const Outcome &a, const Outcome &b)
    {
        if (fn.flags == 0u)
            return sameOutcome(a, b);
        if (a.charged != b.charged)
            return false;
        if (!(fn.flags & NativeFunction::kChargeAtEnd) && a.checkpointCalls != b.checkpointCalls)
            return false;
        const bool renderPath = (fn.flags & NativeFunction::kRenderPath) != 0u;
        const auto *x = reinterpret_cast<const uint8_t *>(&a.ctx), *y = reinterpret_cast<const uint8_t *>(&b.ctx);
        for (size_t at = 0; at < sizeof(R5900Context); ++at)
            if (x[at] != y[at] && (!renderPath || kamite::native::lastingContextByte(fn, at)))
                return false;
        for (const Region &r : kRegions)
            for (uint32_t i = 0; i < r.size; ++i)
                if (g_ramA[r.begin + i] != g_ramB[r.begin + i] &&
                    (!renderPath || !kamite::native::deadStackByte(fn, call.ctx, r.begin + i)))
                    return false;
        return true;
    }

    void resetRam()
    {
        for (const Region &r : kRegions)
            std::memset(g_ramA + r.begin, 0, r.size), std::memset(g_ramB + r.begin, 0, r.size);
    }

    // KM_DIFF=1: where the first mismatches differ (context words by field, RAM words).
    // (a kRenderPath native: only what its comparison looks at)
    const NativeFunction *g_diffFn = nullptr;
    const Call *g_diffCall = nullptr;
    bool diffCounts(size_t ctxAt, int64_t ramAt)
    {
        if (!g_diffFn || !(g_diffFn->flags & NativeFunction::kRenderPath))
            return true;
        if (ramAt >= 0)
            return !kamite::native::deadStackByte(*g_diffFn, g_diffCall->ctx, static_cast<uint32_t>(ramAt));
        for (size_t k = ctxAt; k < ctxAt + 4; ++k)
            if (kamite::native::lastingContextByte(*g_diffFn, k))
                return true;
        return false;
    }
    void printDiff(const Outcome &a, const Outcome &b)
    {
        const auto *x = reinterpret_cast<const uint8_t *>(&a.ctx), *y = reinterpret_cast<const uint8_t *>(&b.ctx);
        for (size_t at = 0; at < sizeof(R5900Context); at += 4)
            if (std::memcmp(x + at, y + at, 4) != 0 && diffCounts(at, -1))
            {
                uint32_t u, v;
                std::memcpy(&u, x + at, 4), std::memcpy(&v, y + at, 4);
                const char *what = "?";
                size_t base = 0;
                if (at >= offsetof(R5900Context, r) && at < offsetof(R5900Context, r) + sizeof(a.ctx.r))
                    what = "r", base = offsetof(R5900Context, r);
                else if (at >= offsetof(R5900Context, vu0_vf) && at < offsetof(R5900Context, vu0_vf) + sizeof(a.ctx.vu0_vf))
                    what = "vf", base = offsetof(R5900Context, vu0_vf);
                else if (at >= offsetof(R5900Context, f) && at < offsetof(R5900Context, f) + sizeof(a.ctx.f))
                    what = "f", base = offsetof(R5900Context, f);
                std::printf("    ctx +0x%zX (%s %zu.%zu): recompiled %08X native %08X\n", at, what,
                            (at - base) / (std::strcmp(what, "f") == 0 ? 4 : 16), ((at - base) % 16) / 4, u, v);
            }
        if (a.charged != b.charged || a.checkpointCalls != b.checkpointCalls)
            std::printf("    cycles %llu / %llu, checkpoints %d / %d\n", (unsigned long long)a.charged,
                        (unsigned long long)b.charged, a.checkpointCalls, b.checkpointCalls);
        for (const Region &r : kRegions)
            for (uint32_t i = 0; i < r.size; i += 4)
                if (std::memcmp(g_ramA + r.begin + i, g_ramB + r.begin + i, 4) != 0 && diffCounts(0, r.begin + i))
                {
                    uint32_t u, v;
                    std::memcpy(&u, g_ramA + r.begin + i, 4), std::memcpy(&v, g_ramB + r.begin + i, 4);
                    std::printf("    ram %08X: recompiled %08X native %08X\n", r.begin + i, u, v);
                }
    }

    // ---- 1 + 2: natives against the recompiled functions; footprints ---------------------------------------------
    void checkNative(const NativeFunction &fn, Recompiled recompiled, int calls)
    {
        int ran = 0, declined = 0, declinedOrdinary = 0, early = 0, interrupted = 0;
        std::vector<uint8_t> before;
        for (int i = 0; i < calls; ++i)
        {
            Call call;
            const bool wild = (i % 4) == 3;
            setupCall(fn, call, wild);
            saveRam(before, g_ramA);
            // A recorded footprint: the native's shadow run on C, filled from B (still the input), records it; its
            // outcome must be the real run's (checked below).
            kamite::native::Recording rec;
            Outcome recorded{};
            bool recordedRan = false;
            if (!fn.footprint)
            {
                recorded = runFrom(call, [&](R5900Context *ctx) {
                    kamite::native::ShadowEnv env{g_ramC};
                    rec.reset(g_ramB, fn.maxBytes, fn.maxRanges);
                    env.record = &rec;
                    env.dueAt = call.dueAt;
                    recordedRan = kamite::native::runNative(fn, fn.runShadow, env, ctx);
                });
            }
            const Outcome original = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            bool accepted = false;
            const Outcome native = runFrom(call, [&](R5900Context *ctx) {
                kamite::native::RealEnv env{g_ramB, g_rt};
                accepted = kamite::native::runNative(fn, fn.runReal, env, ctx);
            });
            // footprint: everything the original changed is declared (a recorded one: every range it recorded)
            kamite::native::Footprint fp;
            std::vector<kamite::native::Footprint::Range> ranges;
            if (fn.footprint)
            {
                fn.footprint(&call.ctx, fp);
                ranges.assign(fp.ranges, fp.ranges + fp.count);
            }
            else
                ranges = rec.ranges;
            size_t savedAt = 0;
            for (const Region &region : kRegions)
            {
                if (!fn.footprint && !accepted) // a declined recorded call recorded nothing
                    break;
                for (uint32_t i = 0; i < region.size; ++i, ++savedAt)
                {
                    const uint32_t at = region.begin + i;
                    if (g_ramA[at] == before[savedAt])
                        continue;
                    if ((fn.flags & NativeFunction::kRenderPath) && kamite::native::deadStackByte(fn, call.ctx, at))
                        continue; // the dead stack: the original's frames, not compared

                    bool inside = false;
                    for (const auto &range : ranges)
                        inside |= at - (range.address & PS2_RAM_MASK) < range.size;
                    if (!inside)
                    {
                        fail("the original wrote outside the footprint");
                        break;
                    }
                }
            }
            if (!fn.footprint)
            {
                // the recording run agrees with the real run: same decision, context, and bytes where it recorded
                if (recordedRan != accepted)
                    fail("the recorded run decided otherwise than the real run");
                else if (accepted)
                {
                    if (std::memcmp(&recorded.ctx, &native.ctx, sizeof(R5900Context)) != 0)
                        fail("the recorded run's context differs from the real run's");
                    for (const auto &r : rec.ranges)
                        if (std::memcmp(g_ramC + r.address, g_ramB + r.address, r.size) != 0)
                        {
                            fail("the recorded run's bytes differ from the real run's");
                            break;
                        }
                }
                for (const auto &r : rec.ranges)
                    std::memset(g_ramC + r.address, 0, r.size);
                rec.unmark();
            }
            if (!accepted)
            {
                ++declined;
                // a declined call changed nothing
                if (std::memcmp(&native.ctx, &call.ctx, sizeof(R5900Context)) != 0 || !sameAsSaved(before, g_ramB) ||
                    native.checkpointCalls != 0)
                {
                    fail("a declined call changed something");
                    if (std::getenv("KM_DIFF") && g_failures <= 3)
                        std::printf("    context %s, ram %s, checkpoints %d\n",
                                    std::memcmp(&native.ctx, &call.ctx, sizeof(R5900Context)) ? "changed" : "same",
                                    sameAsSaved(before, g_ramB) ? "same" : "changed", native.checkpointCalls);
                }
                if (!wild)
                    ++declinedOrdinary;
                copyRam(g_ramB, g_ramA); // back in step
                continue;
            }
            ++ran;
            early += original.ctx.pc != 0x00123450u;
            // Interrupted = the original's own checkpoint came due (it stops on the dueAt-th call), not merely a pc
            // that differs: a native that leaves a different pc (e.g. the original restored a frame it had overwritten,
            // so its ra differs) must be compared and fail (rung 4, Nov 25; planted: MtxMulScaleVec's out in its frame).
            const bool cameDue = call.dueAt != 0 && original.checkpointCalls == call.dueAt;
            if ((fn.flags & NativeFunction::kChargeAtEnd) && cameDue)
            {
                // the original stopped at one of its own checkpoints; the native ran through (the harness's
                // "interrupted"): the native must still have run to the return, its whole sum charged
                ++interrupted;
                if (native.ctx.pc != 0x00123450u)
                    fail("a charged-at-end native stopped before its return");
                copyRam(g_ramB, g_ramA);
                continue;
            }
            if (!sameOutcomeFor(fn, call, original, native))
            {
                char text[160];
                std::snprintf(text, sizeof(text), "%s call %d: native differs from the recompiled function (due at %d)",
                              fn.name, i, call.dueAt);
                fail(text);
                if (std::getenv("KM_DIFF") && g_failures <= 3)
                {
                    g_diffFn = &fn, g_diffCall = &call;
                    uint32_t sp0;
                    std::memcpy(&sp0, &call.ctx.r[29], 4);
                    std::printf("    (entry sp %08X)\n", sp0);
                    printDiff(original, native);
                    g_diffFn = nullptr;
                }
                copyRam(g_ramB, g_ramA);
            }
            else if (fn.flags & NativeFunction::kRenderPath)
                copyRam(g_ramB, g_ramA); // the dead stack the original wrote and the native didn't: back in step
        }
        std::printf("%-20s %d calls: %d native (%d stopped at a checkpoint), %d declined (%d with ordinary values: "
                    "aliasing, angles past the limit, Inf / NaN garbage in vf4)%s\n", fn.name, calls, ran, early, declined,
                    declinedOrdinary, fn.flags ? (fn.flags & NativeFunction::kChargeAtEnd ? " [render path, at end]"
                                                                                           : " [render path]") : "");
        if (fn.flags & NativeFunction::kChargeAtEnd)
        {
            std::printf("%-20s   interrupted (the original stopped at its own checkpoint) %d\n", "", interrupted);
            if (interrupted == ran)
                fail("every call interrupted: nothing compared");
        }
        resetRam();
    }

    // ---- 3: the harness is invisible -------------------------------------------------------------------------------
    uint64_t testCycles() { return g_charged; }
    bool g_nested = false; // checkInvisible: the shadowed run's ecossin calls go through the harness too
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
            g_ecossinSlot = &_sceVu0ecossin_0x30a5d0;
            const Outcome alone = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            if (g_nested)
                g_ecossinSlot = &kamite::native::shadow::entry;
            const Outcome shadowed = runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
            g_ecossinSlot = &_sceVu0ecossin_0x30a5d0;
            if (!sameOutcome(alone, shadowed))
            {
                fail("the shadow harness changed the call's outcome");
                copyRam(g_ramB, g_ramA);
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
        if (g_fault == 5)
            std::memset(&ctx->r[16], 0x5A, 4); // s0: callee-saved, lasts
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

    // ---- 4b: the relaxed bar (K's answer 2): scratch and the dead stack don't-care, what lasts still caught ------------
    void checkRelaxedDetection(const NativeFunction &apply)
    {
        struct Case
        {
            const char *name;
            int fault;
            int sp;           // 0 = random, 1 = just above the result (the result in the dead stack), 2 = below it
            bool caught;
        };
        const Case cases[] = {
            {"a3 (scratch)", 2, 0, false},       {"s0 (saved)", 5, 0, true},
            {"result, dead stack", 1, 1, false}, {"result, live", 1, 2, true},
            {"cycles", 3, 0, true},
        };
        int n = 0;
        for (const Case &c : cases)
        {
            g_fault = c.fault;
            NativeFunction wrong = apply;
            wrong.runReal = &faultyApply<kamite::native::RealEnv>;
            wrong.runShadow = &faultyApply<kamite::native::ShadowEnv>;
            wrong.flags = NativeFunction::kRenderPath;
            wrong.address = 0x00920000u + 0x10u * static_cast<uint32_t>(n++);
            kamite::native::shadow::add(wrong, &sceVu0ApplyMatrix_0x30a2b0);
            const kamite::native::shadow::Stats *stats = kamite::native::shadow::stats(wrong.address);
            for (int i = 0; i < 200; ++i)
            {
                Call call;
                setupCall(apply, call, false);
                call.ctx.pc = wrong.address;
                call.dueAt = 0;
                uint32_t out;
                std::memcpy(&out, &call.ctx.r[4], 4);
                out = (out & ~0xFu) & PS2_RAM_MASK;
                if (c.sp == 1)
                    setGpr(call.ctx, 29, out + 0x40u);
                else if (c.sp == 2)
                    setGpr(call.ctx, 29, out - 0x100u);
                runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
            }
            std::printf("relaxed %-20s checked %llu, mismatches %llu (context %llu ram %llu cycles %llu)\n", c.name,
                        static_cast<unsigned long long>(stats->checked), static_cast<unsigned long long>(stats->mismatches),
                        static_cast<unsigned long long>(stats->mismatchCtx), static_cast<unsigned long long>(stats->mismatchRam),
                        static_cast<unsigned long long>(stats->mismatchCycles));
            if (c.caught && stats->mismatches < stats->checked / 2)
                fail("the relaxed bar missed what lasts");
            if (!c.caught && stats->mismatches != 0)
                fail("the relaxed bar reported scratch / dead stack");
        }
        g_fault = 0;
        resetRam();
    }

    // The relaxed bar on real natives: twins of composites flagged kRenderPath | kChargeAtEnd (same bodies), shadowed
    // at a spare address: no mismatches, the harness invisible, interrupted calls counted and not compared.
    void checkTwinShadow(const NativeFunction &twin, Recompiled recompiled, int calls, uint32_t address)
    {
        NativeFunction slot = twin;
        slot.address = address;
        kamite::native::shadow::add(slot, recompiled);
        const kamite::native::shadow::Stats *stats = kamite::native::shadow::stats(address);
        for (int i = 0; i < calls; ++i)
        {
            Call call;
            setupCall(twin, call, (i % 4) == 3);
            call.ctx.pc = address;
            const Outcome alone = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            const Outcome shadowed = runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
            if (!sameOutcome(alone, shadowed))
            {
                fail("the shadow harness changed a relaxed call's outcome");
                copyRam(g_ramB, g_ramA);
            }
        }
        std::printf("%-20s twin shadowed %d: checked %llu, matched %llu, declined %llu, skipped %llu, interrupted %llu, "
                    "mismatches %llu\n", twin.name, calls, static_cast<unsigned long long>(stats->checked),
                    static_cast<unsigned long long>(stats->matched), static_cast<unsigned long long>(stats->declined),
                    static_cast<unsigned long long>(stats->skipped), static_cast<unsigned long long>(stats->interrupted),
                    static_cast<unsigned long long>(stats->mismatches));
        if (stats->mismatches != 0)
            fail("mismatches reported for a relaxed twin");
        if (stats->checked == 0)
            fail("no relaxed twin call compared");
        resetRam();
    }

    // A replay's layering (native.cpp, rung 4 Nov 26): a native registered again at a checked native's address (the
    // *_twist variants) is shadowed with the live checked native as the function it checks against, whose declines run
    // the recompiled code. Here at a spare address: the shadowed call's outcome must be the recompiled alone's, and the
    // harness must report no mismatch.
    const NativeFunction *g_layerBase = nullptr;
    Recompiled g_layerRecompiled = nullptr;
    void layeredHeld(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        kamite::native::RealEnv env{rdram, rt};
        if (!kamite::native::runNative(*g_layerBase, g_layerBase->runReal, env, ctx))
            g_layerRecompiled(rdram, ctx, rt);
    }
    void checkLayered(const NativeFunction &variant, const NativeFunction &base, Recompiled recompiled, int calls,
                      uint32_t address)
    {
        g_layerBase = &base;
        g_layerRecompiled = recompiled;
        NativeFunction slot = variant;
        slot.address = address;
        kamite::native::shadow::add(slot, &layeredHeld);
        const kamite::native::shadow::Stats *stats = kamite::native::shadow::stats(address);
        for (int i = 0; i < calls; ++i)
        {
            Call call;
            setupCall(variant, call, (i % 4) == 3);
            call.ctx.pc = address;
            call.dueAt = 0; // the live native charges at its end: whole runs only (relaxed bar, as the replays compare)
            const Outcome alone = runFrom(call, [&](R5900Context *ctx) { recompiled(g_ramA, ctx, g_rt); });
            const Outcome shadowed = runFrom(call, [&](R5900Context *ctx) { kamite::native::shadow::entry(g_ramB, ctx, g_rt); });
            if (!sameOutcomeFor(variant, call, alone, shadowed))
            {
                if (std::getenv("KM_DIFF"))
                {
                    std::printf("  charged %llu vs %llu\n", static_cast<unsigned long long>(alone.charged),
                                static_cast<unsigned long long>(shadowed.charged));
                    const auto *x = reinterpret_cast<const uint8_t *>(&alone.ctx), *y = reinterpret_cast<const uint8_t *>(&shadowed.ctx);
                    for (size_t at = 0; at < sizeof(R5900Context); at += 4)
                        if (std::memcmp(x + at, y + at, 4) != 0 && kamite::native::lastingContextByte(variant, at))
                            std::printf("  ctx +0x%zX\n", at);
                    int shown = 0;
                    for (const Region &r : kRegions)
                        for (uint32_t i = 0; i < r.size && shown < 8; i += 4)
                            if (std::memcmp(g_ramA + r.begin + i, g_ramB + r.begin + i, 4) != 0 &&
                                !kamite::native::deadStackByte(variant, call.ctx, r.begin + i))
                                std::printf("  ram 0x%X\n", r.begin + i), ++shown;
                }
                fail("the layered shadow changed a call's outcome");
            }
            copyRam(g_ramB, g_ramA); // the live native leaves no frames: B's dead stack differs from A's after each call
        }
        std::printf("%-20s layered over %s %d: checked %llu, matched %llu, declined %llu, skipped %llu, interrupted %llu, "
                    "mismatches %llu\n", variant.name, base.name, calls, static_cast<unsigned long long>(stats->checked),
                    static_cast<unsigned long long>(stats->matched), static_cast<unsigned long long>(stats->declined),
                    static_cast<unsigned long long>(stats->skipped), static_cast<unsigned long long>(stats->interrupted),
                    static_cast<unsigned long long>(stats->mismatches));
        if (stats->mismatches != 0)
            fail("mismatches reported for a layered variant");
        if (stats->checked == 0)
            fail("no layered call compared");
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

        // RotMatrixX / Y / Z whole (ecossin + the rows + the product): <path>.rot
        const std::string rotPath = std::string(path) + ".rot";
        f = std::fopen(rotPath.c_str(), "wb");
        Recompiled rot[3] = {&sceVu0RotMatrixX_0x30a700, &sceVu0RotMatrixY_0x30a7b8, &sceVu0RotMatrixZ_0x30a650};
        written = 0;
        for (int i = 0; i < count; ++i)
        {
            const uint32_t axis = static_cast<uint32_t>(i % 3);
            R5900Context ctx;
            randomContext(ctx);
            const uint32_t base = 0x20000u;
            setGpr(ctx, 4, base);
            setGpr(ctx, 5, base + 0x100u);
            setGpr(ctx, 31, 0x00123450u);
            fillFloats(g_ramA, base + 0x100u, 16, false);
            fillFloats(g_ramA, 0x3C3C90u, 4, false);
            for (int lane = 1; lane < 4; ++lane) // vf4's garbage lanes: ordinary values (the natives decline Inf / NaN)
            {
                const uint32_t v = inputFloat(false);
                std::memcpy(reinterpret_cast<uint8_t *>(&ctx.vu0_vf[4]) + 4 * lane, &v, 4);
            }
            float a = (static_cast<float>(g_rng() % 2000001u) / 1000000.0f - 1.0f) * 7.0f;
            ctx.f[12] = a;
            const uint32_t abits = kamite::native::floatBits(a);
            const bool negative = (abits & 0x80000000u) && (abits & 0x7F800000u);
            const float piOver2 = kamite::native::bitsFloat(0x3FC90FDBu);
            volatile float reduced = negative ? piOver2 + a : piOver2 - a;
            const uint32_t rbits = kamite::native::floatBits(reduced);
            uint8_t vf6[16];
            const uint32_t sext = (rbits & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            std::memcpy(vf6, &rbits, 4), std::memcpy(vf6 + 4, &sext, 4), std::memcpy(vf6 + 8, reinterpret_cast<uint8_t *>(&ctx.r[8]) + 8, 8);
            const uint32_t negativeWord = negative ? 1u : 0u, status = ctx.vu0_status;
            std::fwrite(&axis, 4, 1, f);
            std::fwrite(&ctx.vu0_vf[0], 16, 1, f);
            std::fwrite(g_ramA + 0x3C3C90u, 16, 1, f);
            std::fwrite(&ctx.vu0_vf[4], 16, 1, f);
            std::fwrite(&ctx.vu0_vf[5], 16, 1, f);
            std::fwrite(vf6, 16, 1, f);
            std::fwrite(&ctx.vu0_vf[7], 16, 1, f);
            std::fwrite(&ctx.vu0_vf[8], 16, 1, f);
            std::fwrite(&ctx.vu0_q, 4, 1, f);
            std::fwrite(&status, 4, 1, f);
            std::fwrite(&negativeWord, 4, 1, f);
            std::fwrite(g_ramA + base + 0x100u, 64, 1, f);
            g_dueAt = 0;
            g_checkpointCalls = 0;
            rot[axis](g_ramA, &ctx, g_rt);
            const uint32_t statusAfter = ctx.vu0_status;
            std::fwrite(g_ramA + base, 64, 1, f);
            std::fwrite(&ctx.vu0_vf[4], 16 * 6, 1, f); // vf4-vf9
            std::fwrite(&ctx.vu0_acc, 16, 1, f);
            std::fwrite(&ctx.vu0_q, 4, 1, f);
            std::fwrite(&statusAfter, 4, 1, f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d RotMatrix vectors to %s\n", written, rotPath.c_str());

        // Rung 3's batch 1 maths (<path>.ops, 256-byte records: u32 kind, then inputs and the recompiled function's
        // results): 1 OuterProduct, 2 Normalize, 3 capVu0Length, 4 InversMatrix, 5 ScaleVector, 6 MulVector, 7 TransMatrix.
        const std::string opsPath = std::string(path) + ".ops";
        f = std::fopen(opsPath.c_str(), "wb");
        written = 0;
        for (int i = 0; i < count; ++i)
        {
            const uint32_t kind = 1u + static_cast<uint32_t>(i % 7);
            R5900Context ctx;
            randomContext(ctx);
            const uint32_t base = 0x20000u;
            setGpr(ctx, 4, base);
            setGpr(ctx, 5, base + 0x100u);
            setGpr(ctx, 6, base + 0x200u);
            setGpr(ctx, 31, 0x00123450u);
            fillFloats(g_ramA, base + 0x100u, 16, false);
            fillFloats(g_ramA, base + 0x200u, 4, false);
            ctx.f[12] = static_cast<float>(static_cast<int>(g_rng() % 2001u) - 1000) / 37.0f;
            for (int lane = 0; lane < 4; ++lane) // vf5 / vf6 / ACC: ordinary values (their kept lanes are carried)
            {
                const uint32_t v5 = inputFloat(false), v6 = inputFloat(false), acc = inputFloat(false);
                std::memcpy(reinterpret_cast<uint8_t *>(&ctx.vu0_vf[5]) + 4 * lane, &v5, 4);
                std::memcpy(reinterpret_cast<uint8_t *>(&ctx.vu0_vf[6]) + 4 * lane, &v6, 4);
                std::memcpy(reinterpret_cast<uint8_t *>(&ctx.vu0_acc) + 4 * lane, &acc, 4);
            }
            uint8_t record[256] = {};
            std::memcpy(record, &kind, 4);
            std::memcpy(record + 4, g_ramA + base + 0x100u, 64);  // a1's 64 bytes
            std::memcpy(record + 68, g_ramA + base + 0x200u, 16); // a2's vector
            std::memcpy(record + 84, &ctx.vu0_vf[5], 16);
            std::memcpy(record + 100, &ctx.vu0_vf[6], 16);
            std::memcpy(record + 116, &ctx.vu0_acc, 16);
            std::memcpy(record + 132, &ctx.f[12], 4);
            std::memcpy(record + 136, &ctx.vu0_status, 2);
            g_dueAt = 0;
            switch (kind)
            {
            case 1: sceVu0OuterProduct_0x30a328(g_ramA, &ctx, g_rt); break;
            case 2: sceVu0Normalize_0x30a370(g_ramA, &ctx, g_rt); break;
            case 3: setGpr(ctx, 4, base + 0x100u), capVu0Length_0x1f7df8(g_ramA, &ctx, g_rt); break;
            case 4: sceVu0InversMatrix_0x30a3f8(g_ramA, &ctx, g_rt); break;
            case 5: sceVu0ScaleVector_0x30a4e8(g_ramA, &ctx, g_rt); break;
            case 6: sceVu0MulVector_0x30a4d0(g_ramA, &ctx, g_rt); break;
            default: sceVu0TransMatrix_0x30a508(g_ramA, &ctx, g_rt); break;
            }
            // results: out's 64 bytes (base), vf4-vf9, ACC, Q, status
            std::memcpy(record + 140, g_ramA + base, 64);
            std::memcpy(record + 204, &ctx.vu0_vf[4], 16);
            std::memcpy(record + 220, &ctx.vu0_acc, 16);
            std::memcpy(record + 236, &ctx.vu0_q, 4);
            std::memcpy(record + 240, &ctx.vu0_status, 2);
            std::memcpy(record + 242, &ctx.vu0_vf[5], 14); // vf5's first 14 bytes (Normalize's sum and root)
            std::fwrite(record, 1, sizeof(record), f);
            std::memset(g_ramA + base, 0, 0x300);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d batch-1 maths vectors to %s\n", written, opsPath.c_str());

        // Rung 3's batch 2 maths (<path>.b2, 64-byte records: u32 kind, inputs, the runtime's results (the recompiled
        // functions' ops)): 1 the light fade (near, far, f12 -> d, q0, q1, s0, s1, m), 2 VU0 max / min (a, b -> max,
        // min), 3 cvt.w.s (f -> word), 4 VU0 mul by 1.0 / add (a, b -> a * 1, a + b), 5 the node layer's product (alpha,
        // three bytes -> b0 / 255, b1 / 255, b2 / 255, the product).
        const std::string b2Path = std::string(path) + ".b2";
        f = std::fopen(b2Path.c_str(), "wb");
        written = 0;
        auto bitsOfF = [](float x) { uint32_t b; std::memcpy(&b, &x, 4); return b; };
        auto fOfBits = [](uint32_t b) { float x; std::memcpy(&x, &b, 4); return x; };
        for (int i = 0; i < count; ++i)
        {
            const uint32_t kind = 1u + static_cast<uint32_t>(i % 5);
            uint32_t rec[16] = {kind};
            switch (kind)
            {
            case 1:
            {
                const float n = fOfBits(gameFloat()), fa = (g_rng() % 4u) ? fOfBits(inputFloat(false)) : fOfBits(gameFloat());
                const float f12 = fOfBits(inputFloat(i % 3 == 0));
                uint32_t fcr31 = 0;
                const float d = ps2FpuSub(fa, n), q0 = ps2FpuDivide(fcr31, n, d), q1 = ps2FpuDivide(fcr31, f12, d);
                const float s0 = ps2FpuAdd(q0, 1.0f), s1 = ps2FpuSub(s0, q1), m = ps2FpuMul(s1, 255.0f);
                const float in[3] = {n, fa, f12}, out[6] = {d, q0, q1, s0, s1, m};
                for (int k = 0; k < 3; ++k)
                    rec[1 + k] = bitsOfF(in[k]);
                for (int k = 0; k < 6; ++k)
                    rec[4 + k] = bitsOfF(out[k]);
                break;
            }
            case 2:
            {
                alignas(16) uint32_t a[4], b[4];
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t pick = static_cast<uint32_t>(g_rng() % 8u);
                    a[k] = pick == 0 ? 0x80000000u : pick == 1 ? static_cast<uint32_t>(g_rng()) & 0x807FFFFFu : gameFloat();
                    b[k] = (g_rng() % 4u == 0u) ? a[k] ^ 0x80000000u : gameFloat();
                }
                const __m128 va = _mm_load_ps(reinterpret_cast<float *>(a)), vb = _mm_load_ps(reinterpret_cast<float *>(b));
                std::memcpy(rec + 1, a, 16), std::memcpy(rec + 5, b, 16);
                const __m128 mx = ps2VuMax(va, vb), mn = ps2VuMin(va, vb);
                std::memcpy(rec + 9, &mx, 16);
                std::memcpy(rec + 13, &mn, 12); // max, then min's xyz (the record's room)
                break;
            }
            case 3:
            {
                const uint32_t x = (g_rng() % 2u) ? gameFloat() : bitsOfF(static_cast<float>(static_cast<int>(g_rng() % 100000u) - 20000) / 7.0f);
                rec[1] = x;
                rec[2] = static_cast<uint32_t>(ps2FpuConvertToWord(fOfBits(x)));
                break;
            }
            case 4:
            {
                alignas(16) uint32_t a[4], b[4];
                for (int k = 0; k < 4; ++k)
                    a[k] = (g_rng() % 3u) ? inputFloat(false) : gameFloat(), b[k] = inputFloat(i % 2 == 0);
                const __m128 va = _mm_load_ps(reinterpret_cast<float *>(a)), vb = _mm_load_ps(reinterpret_cast<float *>(b));
                volatile float oneValue = 1.0f; // a constant 1.0 lets the compiler fold x * 1 to x (no flush)
                const __m128 one = _mm_set1_ps(oneValue);
                const __m128 mu = ps2VuMul(va, one), ad = ps2VuAdd(va, vb);
                std::memcpy(rec + 1, a, 16), std::memcpy(rec + 5, b, 16);
                std::memcpy(rec + 9, &mu, 12), std::memcpy(rec + 12, &ad, 16); // mul's xyz, add's xyzw
                break;
            }
            default:
            {
                const uint32_t alpha = (g_rng() % 3u) ? bitsOfF(1.0f) : gameFloat();
                const int b0 = static_cast<int>(g_rng() % 256u), b1 = (g_rng() % 2u) ? 255 : static_cast<int>(g_rng() % 256u);
                const int b2 = (g_rng() % 2u) ? 255 : static_cast<int>(g_rng() % 256u);
                uint32_t fcr31 = 0;
                const float k = fOfBits(0x437F0000u);
                const float q0 = ps2FpuDivide(fcr31, FPU_CVT_S_W(b0), k), q1 = ps2FpuDivide(fcr31, FPU_CVT_S_W(b1), k);
                const float q2 = ps2FpuDivide(fcr31, FPU_CVT_S_W(b2), k);
                const float pr = ps2FpuMul(ps2FpuMul(ps2FpuMul(fOfBits(alpha), q0), q1), q2);
                rec[1] = alpha, rec[2] = static_cast<uint32_t>(b0), rec[3] = static_cast<uint32_t>(b1), rec[4] = static_cast<uint32_t>(b2);
                rec[5] = bitsOfF(q0), rec[6] = bitsOfF(q1), rec[7] = bitsOfF(q2), rec[8] = bitsOfF(pr);
                break;
            }
            }
            std::fwrite(rec, 4, 16, f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d batch-2 maths vectors to %s\n", written, b2Path.c_str());

        // Rung 4 (<path>.motion, libvu0_check replays them on motion_core.h): the recompiled Adjust_theta_vec,
        // MtxInitRotVec, MtxMulRotVec, MtxMulScaleVec and MtxInverse (kind 4: A -> out) on clean contexts. A record: kind, order, in (v / angles /
        // scale), the theta table, the coefficients, A, then the original's 64 result bytes (theta: 16).
        const std::string motionPath = std::string(path) + ".motion";
        f = std::fopen(motionPath.c_str(), "wb");
        written = 0;
        const Recompiled motionFns[5] = {&Adjust_theta_vec_0x1f7da0, &MtxInitRotVec_0x147728, &MtxMulRotVec_0x147b60,
                                         &MtxMulScaleVec_0x147bf8, &MtxInverse_0x147568};
        for (int i = 0; i < count; ++i)
        {
            const uint32_t kind = static_cast<uint32_t>(i % 5);
            R5900Context ctx;
            randomContext(ctx);
            const uint32_t out = 0x20000u, a = 0x20100u, in = 0x20200u;
            for (uint32_t k = 0; k < 12u; ++k)
                setU32(0x3BF110u + 4u * k, kThetaTable[k]);
            for (uint32_t k = 0; k < 4u; ++k)
                setU32(0x3C3C90u + 4u * k, kSinPolynomial[k]);
            for (uint32_t k = 0; k < 6u; ++k)
                setU32(0x41EA00u + 4u * k, 0x147788u + (k == 0u ? 0u : k == 1u ? 0x28u : k == 2u ? 0x50u : k == 3u ? 0x88u : k == 4u ? 0xC0u : 0xF8u));
            fillFloats(g_ramA, a, 16, false);
            if (kind == 4u && g_rng() % 2u)
            {
                Mat4f m;
                randomRigid(m);
                for (uint32_t k = 0; k < 16u; ++k)
                    setFloat(a + 4u * k, m.v[k]);
            }
            const uint32_t order = static_cast<uint32_t>(g_rng() % 7u);
            if (kind == 0u)
                for (uint32_t k = 0; k < 4u; ++k)
                    setFloat(out + 4u * k, angle() * ((g_rng() % 4u) ? 1.0f : 16.0f));
            else if (kind == 3u)
                fillFloats(g_ramA, in, 3, false);
            else
                for (uint32_t k = 0; k < 3u; ++k)
                    setFloat(in + 4u * k, angle());
            uint8_t record[4 + 4 + 16 + 48 + 16 + 64 + 64] = {};
            std::memcpy(record, &kind, 4);
            std::memcpy(record + 4, &order, 4);
            std::memcpy(record + 8, g_ramA + (kind == 0u ? out : in), 16);
            std::memcpy(record + 24, g_ramA + 0x3BF110u, 48);
            std::memcpy(record + 72, g_ramA + 0x3C3C90u, 16);
            std::memcpy(record + 88, g_ramA + a, 64);
            setGpr(ctx, 29, 0xF0000u);
            setGpr(ctx, 31, 0x00123450u);
            setGpr(ctx, 4, kind == 1u ? in + 0x100u : out); // InitRotVec: its own out, clear of its angles
            setGpr(ctx, 5, kind == 1u ? in : a);
            setGpr(ctx, 6, kind == 1u ? order : in);
            setGpr(ctx, 7, order);
            ctx.pc = kind == 0u ? 0x1F7DA0u : kind == 1u ? 0x147728u : kind == 2u ? 0x147B60u : kind == 3u ? 0x147BF8u : 0x147568u;
            g_dueAt = 0;
            motionFns[kind](g_ramA, &ctx, g_rt);
            std::memcpy(record + 152, g_ramA + (kind == 1u ? in + 0x100u : out), kind == 0u ? 16u : 64u);
            std::fwrite(record, 1, sizeof(record), f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d motion vectors to %s\n", written, motionPath.c_str());

        // The blend (<path>.blend): A, B, t, capVu0Atan2's table, 0x15A190's next -> the recompiled func_00156D70's out.
        const std::string blendPath = std::string(path) + ".blend";
        f = std::fopen(blendPath.c_str(), "wb");
        written = 0;
        for (int i = 0; i < count; ++i)
        {
            R5900Context ctx;
            randomContext(ctx);
            setBlendTables(false);
            const uint32_t out = 0x20000u, a = 0x20100u, b = 0x20200u;
            Mat4f ma, mb;
            randomTrs(ma);
            randomTrs(mb);
            if (i % 8 == 0)
                mb = ma;
            setMat4f(a, ma), setMat4f(b, mb);
            setGpr(ctx, 29, 0xF0000u), setGpr(ctx, 31, 0x00123450u);
            setGpr(ctx, 4, out), setGpr(ctx, 5, a), setGpr(ctx, 6, b);
            const float t = blendWeight();
            ctx.f[12] = t;
            ctx.pc = 0x156D70u;
            uint8_t record[64 + 64 + 4 + 36 + 12 + 64];
            std::memcpy(record, g_ramA + a, 64), std::memcpy(record + 64, g_ramA + b, 64), std::memcpy(record + 128, &t, 4);
            std::memcpy(record + 132, g_ramA + 0x42C350u, 36), std::memcpy(record + 168, g_ramA + 0x41EE48u, 12);
            g_dueAt = 0;
            func_00156D70_0x156d70(g_ramA, &ctx, g_rt);
            std::memcpy(record + 180, g_ramA + out, 64);
            std::fwrite(record, 1, sizeof(record), f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d blend vectors to %s\n", written, blendPath.c_str());

        // The twist (<path>.twist): parent world, local, axis, angle, capVu0Cos's table -> the recompiled func_001501D0's
        // world matrix (the scaled path with partFlags 0x800: MtxMultiply, then the twist).
        const std::string twistPath = std::string(path) + ".twist";
        f = std::fopen(twistPath.c_str(), "wb");
        written = 0;
        for (int i = 0; i < count; ++i)
        {
            R5900Context ctx;
            randomContext(ctx);
            const uint32_t part = 0x20000u, parent = 0x20200u;
            Mat4f pw, local;
            randomRigid(pw);
            randomTrs(local);
            setMat4f(parent + 0x80u, pw), setMat4f(part, local);
            setU32(part + 0x148u, parent), setU32(part + 0x154u, 0x800u);
            setTwistAngle(part + 0x150u);
            setTwistTables();
            setGpr(ctx, 29, 0xF0000u), setGpr(ctx, 31, 0x00123450u);
            setGpr(ctx, 4, part), setGpr(ctx, 5, 0u);
            ctx.pc = 0x1501D0u;
            uint8_t record[64 + 64 + 16 + 4 + 16 + 64];
            std::memcpy(record, g_ramA + parent + 0x80u, 64), std::memcpy(record + 64, g_ramA + part, 64);
            std::memcpy(record + 128, g_ramA + 0x3BD8B0u, 16), std::memcpy(record + 144, g_ramA + part + 0x150u, 4);
            std::memcpy(record + 148, g_ramA + 0x3BF140u, 16);
            g_dueAt = 0;
            func_001501D0_0x1501d0(g_ramA, &ctx, g_rt);
            std::memcpy(record + 164, g_ramA + part + 0x80u, 64);
            std::fwrite(record, 1, sizeof(record), f);
            ++written;
        }
        std::fclose(f);
        std::printf("wrote %d twist vectors to %s\n", written, twistPath.c_str());
        resetRam();
    }
}

namespace
{
    template <class Run>
    double nsPerCall(Run run)
    {
        static const int n = [] { // KM_BENCH_CALLS: fewer runs for the big ones (cModel_Tag_set_rate: microseconds each)
            const char *v = std::getenv("KM_BENCH_CALLS");
            return v && *v ? std::max(1, std::atoi(v)) : 2000000;
        }();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
            run();
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
    }

    // KM_BENCH_SETUPS=n: the mean over n setups the native takes (its paths mixed as the setup picks them), else one.
    void bench(const NativeFunction &fn, Recompiled recompiled)
    {
        const char *setups = std::getenv("KM_BENCH_SETUPS");
        const int n = setups ? std::max(1, std::atoi(setups)) : 1;
        double original = 0.0, native = 0.0, copy = 0.0, withNatives = 0.0;
        for (int i = 0; i < n; ++i)
        {
            Call call;
            do
            {
                setupCall(fn, call, false);
                if (fn.footprint) // the rung-2 six: fixed, ordinary arguments
                {
                    setGpr(call.ctx, 4, 0x30000u), setGpr(call.ctx, 5, 0x30100u), setGpr(call.ctx, 6, 0x30200u);
                    fillFloats(g_ramA, 0x30100u, 32, false);
                }
                copyRam(g_ramB, g_ramA);
                g_dueAt = 0;
                R5900Context probe;
                std::memcpy(static_cast<void *>(&probe), &call.ctx, sizeof(probe));
                kamite::native::RealEnv env{g_ramB, g_rt};
                if (kamite::native::runNative(fn, fn.runReal, env, &probe))
                    break; // a call the native takes
            } while (true);
            g_dueAt = 0;
            R5900Context ctx;
            original += nsPerCall([&] {
                std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx));
                recompiled(g_ramA, &ctx, g_rt);
            });
            native += nsPerCall([&] {
                std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx));
                kamite::native::RealEnv env{g_ramA, g_rt};
                kamite::native::runNative(fn, fn.runReal, env, &ctx);
            });
            copy += nsPerCall([&] { std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx)); });
            if (std::getenv("KM_BENCH_NATIVE_CALLEES")) // the recompiled function with its callees' checked natives (play)
            {
                g_nativeCallees = true;
                withNatives += nsPerCall([&] {
                    std::memcpy(static_cast<void *>(&ctx), &call.ctx, sizeof(ctx));
                    recompiled(g_ramA, &ctx, g_rt);
                });
                g_nativeCallees = false;
            }
        }
        std::printf("%-20s recompiled %.1f ns, native %.1f ns (context copy %.1f ns subtracted)\n", fn.name,
                    (original - copy) / n, (native - copy) / n, copy / n);
        if (std::getenv("KM_BENCH_NATIVE_CALLEES"))
            std::printf("%-20s recompiled with its callees' checked natives %.1f ns\n", "", (withNatives - copy) / n);
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
                kamite::native::runNative(*fn, fn->runReal, env, ctx);
            });
            std::printf("  here: native %s the recompiled function; the recompiled function %s the game's real run\n",
                        sameOutcomeFor(*fn, input, original, native) ? "matches" : "DIFFERS from",
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
    kamite::native::packet::registerFunctions();
    kamite::native::motion::registerFunctions();
    const NativeFunction &apply = g_natives[1];
    // Every native with its recompiled function.
    const std::vector<std::pair<uint32_t, Recompiled>> recompiled = {
        {0x30A2E0u, &sceVu0MulMatrix_0x30a2e0},  {0x30A2B0u, &sceVu0ApplyMatrix_0x30a2b0},
        {0x30A5D0u, &_sceVu0ecossin_0x30a5d0},   {0x30A700u, &sceVu0RotMatrixX_0x30a700},
        {0x30A7B8u, &sceVu0RotMatrixY_0x30a7b8}, {0x30A650u, &sceVu0RotMatrixZ_0x30a650},
        {0x30A548u, &sceVu0CopyMatrix_0x30a548}, {0x30A5A0u, &sceVu0UnitMatrix_0x30a5a0},
        {0x30A4D0u, &sceVu0MulVector_0x30a4d0},  {0x30A4E8u, &sceVu0ScaleVector_0x30a4e8},
        {0x30A328u, &sceVu0OuterProduct_0x30a328}, {0x30A370u, &sceVu0Normalize_0x30a370},
        {0x30A508u, &sceVu0TransMatrix_0x30a508}, {0x30A3B0u, &sceVu0TransposeMatrix_0x30a3b0},
        {0x30A3F8u, &sceVu0InversMatrix_0x30a3f8}, {0x30A8C8u, &sceVu0CameraMatrix_0x30a8c8},
        {0x30A978u, &sceVu0NormalLightMatrix_0x30a978}, {0x1F7DF8u, &capVu0Length_0x1f7df8},
        {0x101E28u, &Tramp_001F7DF8_00101E28_0x101e28}, {0x147528u, &MtxCopy_0x147528},
        {0x147548u, &MtxMultiply_0x147548},      {0x14FF00u, &func_0014FF00_0x14ff00},
        {0x31A650u, &func_0031A650_0x31a650},    {0x31A600u, &func_0031A600_0x31a600},
        {0x1503B8u, &func_001503B8_0x1503b8},    {0x1F8DF8u, &func_001F8DF8_0x1f8df8},
        {0x2D7E50u, &func_002D7E50_0x2d7e50},    {0x2D8008u, &func_002D8008_0x2d8008},
        {0x2D8070u, &func_002D8070_0x2d8070},    {0x150DC8u, &func_00150DC8_0x150dc8},
        {0x150F68u, &func_00150F68_0x150f68},    {0x151168u, &func_00151168_0x151168},
        {0x155AE8u, &func_00155AE8_0x155ae8},    {0x155BE8u, &func_00155BE8_0x155be8},
        {0x155C68u, &func_00155C68_0x155c68},    {0x155D60u, &func_00155D60_0x155d60},
        {0x15B0F0u, &PostInc_D_00566E10_0015B0F0_15B0F0_0x15b0f0}, {0x14B6F8u, &cModel_getMeshPtr_0x14b6f8},
        {0x14D768u, &ForwardNodeUpdate_14D768_0x14d768}, {0x14BE38u, &cModel_nodeLayerScreen_0x14be38},
        {0x14C5A0u, &func_0014C5A0_0x14c5a0},    {0x151398u, &func_00151398_0x151398},
        {0x148BD8u, &func_00148BD8_0x148bd8},    {0x14B9A0u, &func_0014B9A0_0x14b9a0},
        {0x151630u, &func_00151630_0x151630},    {0x151990u, &func_00151990_0x151990},
        {0x151CC0u, &func_00151CC0_0x151cc0},    {0x151688u, &func_00151688_0x151688},
        {0x151FA0u, &func_00151FA0_0x151fa0},    {0x148D30u, &func_00148D30_0x148d30},
        {0x148690u, &func_00148690_0x148690},    {0x14B4D0u, &cModel_setNodeMaterial_0x14b4d0},
        {0x14B5D8u, &ForwardAttackByMode_14B5D8_0x14b5d8}, {0x154240u, &func_00154240_0x154240},
        {0x3A1AE8u, &tanf_0x3a1ae8},             {0x30AA40u, &func_0030AA40_0x30aa40},
        {0x14C7E8u, &func_0014C7E8_0x14c7e8},    {0x14AC90u, &cModel_Tag_set_rate_0x14ac90},
        {0x14D0E8u, &func_0014D0E8_0x14d0e8},    {0x153B00u, &func_00153B00_0x153b00},
        {0x2D81A0u, &func_002D81A0_0x2d81a0},    {0x14BEF8u, &cModel_nodeLayer_0x14bef8},
        {0x14B810u, &func_0014B810_0x14b810},    {0x14C6B8u, &cModel_queuePacket_0x14c6b8},
        {0x14AAB8u, &cModel_Tag_set_scr_0x14aab8},
        {0x1F7DA0u, &Adjust_theta_vec_0x1f7da0}, {0x1476D0u, &MtxInitTransVec_0x1476d0},
        {0x147728u, &MtxInitRotVec_0x147728},    {0x147A08u, &MtxInitScaleVec_0x147a08},
        {0x147B60u, &MtxMulRotVec_0x147b60},     {0x147BF8u, &MtxMulScaleVec_0x147bf8},
        {0x147B18u, &MtxMulTransVec_0x147b18},   {0x147A48u, &MtxInitCoord_0x147a48},
        {0x150138u, &cParts_calcLocal_0x150138}, {0x147568u, &MtxInverse_0x147568},
        {0x1501D0u, &func_001501D0_0x1501d0},    {0x156D70u, &func_00156D70_0x156d70},
        {0x15A6A0u, &func_0015A6A0_0x15a6a0},    {0x15A190u, &func_0015A190_0x15a190},
        {0x1478A8u, &MtxInitRotQuaternion_0x1478a8},
        {0x149EB8u, &cModel_calcParts_0x149eb8}, {0x14A038u, &cModel_calcWorldParts_0x14a038}};
    g_callees.push_back({0x151368u, &func_00151368_0x151368}); // callees only (no native)
    g_callees.push_back({0x309C30u, &Tim2CalcBufWidth_0x309c30});
    g_callees.push_back({0x31DE10u, &ps2___muldi3_0x31de10});
    g_callees.push_back({0x31CDC0u, &SearchDataByTwoNames_0x31cdc0});
    g_callees.push_back({0x1522C0u, &cIDBase_getSlotPtr_0x1522c0});
    g_callees.push_back({0x30A870u, &sceVu0RotMatrix_0x30a870}); // MtxInitRotVec's order 5
    for (const auto &[address, f] : std::initializer_list<std::pair<uint32_t, void (*)(uint8_t *, R5900Context *, PS2Runtime *)>>{
             {0x1478A8u, &MtxInitRotQuaternion_0x1478a8}, {0x147C40u, &CopyVec3ToField30_147C40_0x147c40},
             {0x147C60u, &ForwardVec3At30_147C60_0x147c60}, {0x150710u, &func_00150710_0x150710},
             {0x159FC8u, &Quaternion_SetRotationAxis_0x159fc8}, {0x1F7FB8u, &capVu0Cos_0x1f7fb8},
             {0x3A19F8u, &sinf_0x3a19f8}, {0x3A3C80u, &ps2___kernel_cosf_0x3a3c80},
             {0x3A4728u, &ps2___kernel_sinf_0x3a4728}})
        g_callees.push_back({address, f}); // func_001501D0's twist path (no natives: the native declines it)
    for (const auto &[address, f] : std::initializer_list<std::pair<uint32_t, void (*)(uint8_t *, R5900Context *, PS2Runtime *)>>{
             {0x1F7E28u, &capVu0LengthSq_0x1f7e28}, {0x30ABA0u, &sceVu0ScaleVectorXYZ_0x30aba0},
             {0x1F8180u, &capVu0Atan2_0x1f8180}})
        g_callees.push_back({address, f}); // the blend's inline helpers (no natives of their own yet)
    for (const auto &[address, f] : std::initializer_list<std::pair<uint32_t, void (*)(uint8_t *, R5900Context *, PS2Runtime *)>>{
             {0x3A4830u, &ps2___kernel_tanf_0x3a4830}, {0x3A3768u, &ps2___ieee754_rem_pio2f_0x3a3768},
             {0x3A3DD8u, &ps2___kernel_rem_pio2f_0x3a3dd8}, {0x3A19D8u, &func_003A19D8_0x3a19d8},
             {0x3A4E28u, &scalbnf_0x3a4e28}, {0x3A4D18u, &floorf_0x3a4d18}, {0x3A4F88u, &copysignf_0x3a4f88}})
        g_callees.push_back({address, f}); // tanf's libm callees (no natives)
    kamite::texture_slots::install(); // its hook in the TIM2 loader (the setups switch the flag per call)
    kamite::model_detail::install();  // no_lod's hook at cModel_Tag_set_rate's entry (switched per call too)
    for (const auto &[address, f] : recompiled)
        g_callees.push_back({address, f});
    auto recompiledOf = [&](const NativeFunction &fn) {
        for (const auto &[address, f] : recompiled)
            if (address == fn.address)
                return f;
        std::printf("no recompiled function for %s\n", fn.name);
        std::exit(2);
    };
    if (argc > 1 && std::strcmp(argv[1], "camera") == 0) // func_0014C7E8's fixed cameras, from the recompiled CameraMatrix
    {
        for (float z : {50.0f, 145.0f})
        {
            R5900Context ctx;
            randomContext(ctx);
            const float in[12] = {0.0f, 0.0f, z, 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
            std::memcpy(g_ramA + 0x3C010u, in, sizeof(in));
            setGpr(ctx, 4, 0x3C100u);
            setGpr(ctx, 5, 0x3C010u);
            setGpr(ctx, 6, 0x3C020u);
            setGpr(ctx, 7, 0x3C030u);
            setGpr(ctx, 29, 0xF0000u);
            setGpr(ctx, 31, 0x00123450u);
            ctx.pc = 0x30A8C8u;
            sceVu0CameraMatrix_0x30a8c8(g_ramA, &ctx, g_rt);
            for (uint32_t k = 0; k < 16u; ++k)
            {
                uint32_t w;
                std::memcpy(&w, g_ramA + 0x3C100u + 4u * k, 4);
                std::printf("0x%08Xu,%s", w, k % 8u == 7u ? "\n" : " ");
            }
        }
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "vectors") == 0) // the maths vectors alone
    {
        writeVectors(argv[2], 20000);
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "rerun") == 0)
        return rerun(argv[2], recompiled);
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0)
    {
        const char *only = std::getenv("KM_ONLY"); // one native's bench only
        for (const NativeFunction &fn : g_natives)
            if (!only || std::strcmp(fn.name, only) == 0)
                bench(fn, recompiledOf(fn));
        return 0;
    }
    const int calls = argc > 1 ? std::atoi(argv[1]) : 20000;

    // KM_ONLY=<name>: the native checks of that function only (a quick loop while one is being fixed).
    if (const char *only = std::getenv("KM_ONLY"))
    {
        for (const NativeFunction &fn : g_natives)
            if (std::strcmp(fn.name, only) == 0)
                checkNative(fn, recompiledOf(fn), calls);
        std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
        return g_failures ? 1 : 0;
    }
    std::filesystem::remove_all("/tmp/native_shadow_test");
    kamite::native::shadow::Settings settings;
    settings.output = &testOutput;
    settings.header = &testHeader;
    settings.cycles = &testCycles;
    kamite::native::shadow::begin(settings);
    // One slot per address, as native.cpp keeps it: a native registered again at an address (the *_twist variants)
    // takes its slot in these checks (in a replay the checked one is live and the variant is the shadowed one).
    auto overridden = [&](const NativeFunction &fn) {
        bool later = false, seen = false;
        for (const NativeFunction &other : g_natives)
        {
            if (&other == &fn)
                seen = true;
            else if (seen && other.address == fn.address)
                later = true;
        }
        return later;
    };
    auto layered = [&](int n, uint32_t spare) {
        for (const NativeFunction &variant : g_natives)
            for (const NativeFunction &base : g_natives)
                if (&base != &variant && base.address == variant.address && overridden(base) && base.checked)
                {
                    checkLayered(variant, base, recompiledOf(variant), n, spare);
                    spare += 0x10u;
                }
    };
    if (std::getenv("KM_LAYERED")) // the replay's layering checks alone
    {
        layered(calls, 0x00931000u);
        std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
        return g_failures ? 1 : 0;
    }
    for (const NativeFunction &fn : g_natives)
        checkNative(fn, recompiledOf(fn), calls);
    for (const NativeFunction &fn : g_natives)
        if (!overridden(fn))
            kamite::native::shadow::add(fn, recompiledOf(fn));
    for (const NativeFunction &fn : g_natives)
        if (!overridden(fn))
            checkInvisible(fn, recompiledOf(fn), calls);
    // Nested: RotMatrix shadowed while the ecossin it calls is shadowed too (its own frame); every native that calls
    // another (CameraMatrix, NormalLightMatrix, the wrappers, the queue) with its callees shadowed too.
    g_nested = true;
    for (const NativeFunction &fn : g_natives)
        if (std::strncmp(fn.name, "sceVu0RotMatrix", 15) == 0)
            checkInvisible(fn, recompiledOf(fn), calls / 4);
    g_nested = false;
    g_nestedCalls = true;
    for (const NativeFunction &fn : g_natives)
        for (uint32_t caller : {0x30A8C8u, 0x30A978u, 0x147528u, 0x147548u, 0x101E28u, 0x31A650u, 0x31A600u, 0x14D768u,
                                0x150DC8u, 0x150F68u, 0x151168u, 0x1503B8u, 0x2D8008u, 0x2D7E50u, 0x1F8DF8u,
                                0x154240u, 0x30AA40u, 0x14C7E8u, 0x14AC90u, 0x14D0E8u, 0x153B00u, 0x14B810u,
                                0x14C6B8u, 0x14AAB8u, 0x147728u, 0x147B60u, 0x147BF8u, 0x147B18u, 0x147A48u,
                                0x150138u, 0x1501D0u, 0x156D70u, 0x15A6A0u, 0x149EB8u, 0x14A038u})
            if (fn.address == caller && !overridden(fn))
                checkInvisible(fn, recompiledOf(fn), calls / 4);
    g_nestedCalls = false;

    checkDetection(apply);
    checkRelaxedDetection(apply);
    // Twins: the composites with loops and guest calls, run charged-at-end and compared on what lasts.
    {
        uint32_t spare = 0x00930000u;
        for (const NativeFunction &fn : g_natives)
            for (uint32_t a : {0x1F8DF8u, 0x1503B8u, 0x14D768u, 0x30A8C8u, 0x2D7E50u, 0x150F68u, 0x31A650u})
                if (fn.address == a)
                {
                    NativeFunction twin = fn;
                    twin.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
                    checkNative(twin, recompiledOf(fn), calls / 2);
                    checkTwinShadow(twin, recompiledOf(fn), calls / 4, spare);
                    spare += 0x10u;
                }
        layered(calls / 2, spare); // the replay's layering: each variant shadowed over its live checked native
    }
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
