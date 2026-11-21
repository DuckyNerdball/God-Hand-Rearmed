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
uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t a)
{
    std::fprintf(stderr, "unexpected Load8 (special address) %08X\n", a);
    std::abort();
}
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *, uint32_t)
{
    std::fprintf(stderr, "unexpected Load32 (special address)\n");
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

// The generated JAL (ps2_runtime.cpp callGuestFunctionAs): the callee's slot (its recompiled function, or the shadow
// harness's entry when the nested checks route shadowed callees through it), the 8-cycle checkpoint first.
using SlotFunction = PS2Runtime::RecompiledFunction;
std::vector<std::pair<uint32_t, SlotFunction>> g_callees; // set in main: every recompiled function in the test
bool g_nestedCalls = false;                               // route callees that have a shadow slot to shadow::entry
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
    fn(rdram, ctx, this);
    return ctx->pc == fallthroughPc;
}

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

    // Two 128 MB backings (lazily committed) like the runtime's; tests use the first 1 MB and the coefficients' block.
    struct Region
    {
        uint32_t begin, size;
    };
    // + batch 2's fixed addresses: the GS table 0x4A5D80, the allocator 0x566E10, the view matrix pointer 0x5CAFF0,
    // 0x7476B0 / 0x747A2C / 0x747AA8, the order table's descriptors 0x755780...
    constexpr Region kRegions[] = {{0u, 1u << 20}, {0x3C3C00u, 0x100u}, {0x4A5D80u, 0x810u}, {0x566E00u, 0x20u},
                                   {0x5CAFF0u, 0x10u}, {0x747600u, 0x500u}, {0x755700u, 0x100u}};
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
            setU32(0x566E10u, 0x506E10u + (half & 1u) * 0x60000u + static_cast<uint32_t>(g_rng() % 0x60010u));
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
        default:
            return;
        }
    }

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
            if (x[at] != y[at] && (!renderPath || kamite::native::lastingContextByte(at)))
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
    void printDiff(const Outcome &a, const Outcome &b)
    {
        const auto *x = reinterpret_cast<const uint8_t *>(&a.ctx), *y = reinterpret_cast<const uint8_t *>(&b.ctx);
        for (size_t at = 0; at < sizeof(R5900Context); at += 4)
            if (std::memcmp(x + at, y + at, 4) != 0)
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
                if (std::memcmp(g_ramA + r.begin + i, g_ramB + r.begin + i, 4) != 0)
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
                    rec.reset(g_ramB, fn.maxBytes);
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
                    fail("a declined call changed something");
                if (!wild)
                    ++declinedOrdinary;
                copyRam(g_ramB, g_ramA); // back in step
                continue;
            }
            ++ran;
            early += original.ctx.pc != 0x00123450u;
            if ((fn.flags & NativeFunction::kChargeAtEnd) && original.ctx.pc != native.ctx.pc)
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
                    printDiff(original, native);
                copyRam(g_ramB, g_ramA);
            }
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
            if (fn.runReal(env, &probe))
                break; // a call the native takes
        } while (true);
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
    kamite::native::packet::registerFunctions();
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
        {0x14D768u, &ForwardNodeUpdate_14D768_0x14d768}, {0x14BE38u, &cModel_nodeLayerScreen_0x14be38}};
    for (const auto &[address, f] : recompiled)
        g_callees.push_back({address, f});
    auto recompiledOf = [&](const NativeFunction &fn) {
        for (const auto &[address, f] : recompiled)
            if (address == fn.address)
                return f;
        std::printf("no recompiled function for %s\n", fn.name);
        std::exit(2);
    };
    if (argc > 2 && std::strcmp(argv[1], "vectors") == 0) // the maths vectors alone
    {
        writeVectors(argv[2], 20000);
        return 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "rerun") == 0)
        return rerun(argv[2], recompiled);
    if (argc > 1 && std::strcmp(argv[1], "bench") == 0)
    {
        for (const NativeFunction &fn : g_natives)
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
    for (const NativeFunction &fn : g_natives)
        checkNative(fn, recompiledOf(fn), calls);

    std::filesystem::remove_all("/tmp/native_shadow_test");
    kamite::native::shadow::Settings settings;
    settings.output = &testOutput;
    settings.header = &testHeader;
    settings.cycles = &testCycles;
    kamite::native::shadow::begin(settings);
    for (const NativeFunction &fn : g_natives)
        kamite::native::shadow::add(fn, recompiledOf(fn));
    for (const NativeFunction &fn : g_natives)
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
                                0x150DC8u, 0x150F68u, 0x151168u, 0x1503B8u, 0x2D8008u, 0x2D7E50u, 0x1F8DF8u})
            if (fn.address == caller)
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
