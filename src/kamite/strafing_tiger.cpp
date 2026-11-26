#include "kamite/strafing_tiger.h"

#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace kamite::strafing_tiger
{
    namespace
    {
        // cObjBase_addNullSpeed (obj in a0): position *(+0xF0) += the motion's root delta (+0x330 / +0x334 / +0x338,
        // the model's own space) x scale (+0x110..) x rate (f12), turned by the rotation +0x100 (yaw +0x104). The
        // walking and idle steps run it every frame after moveMotion filled the delta (frame_patches' held vblank too).
        // Under Strafing Tiger the walking steps' delta is turned by the slide angle first and put back after, so only
        // where he goes changes: the motion, its pose and everything else that reads the delta stay the game's.
        constexpr uint32_t kAddNullSpeed = 0x0014F928u;
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj);

        std::atomic<bool> g_setting{false};
        std::atomic<bool> g_slide{false};
        std::atomic<float> g_angle{0.0f};
        PS2Runtime::RecompiledFunction g_inner = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        float rdF(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, sizeof(v)); }
        bool inRam(uint32_t a) { return a >= 0x100000u && a < PS2_RDRAM_SIZE; }

        void addNullSpeedWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (!g_setting.load(std::memory_order_relaxed) || !g_slide.load(std::memory_order_relaxed) || !inRam(obj) ||
                obj != rd32(rdram, kPlayerPointer))
                return g_inner(rdram, ctx, runtime);
            // Moving only: mode 0, phase 1 walk / 2 run / 3 back-pedal (style_pose's isFree phases without standing).
            const uint8_t major = rdram[(obj + 0x2F4u) & PS2_RAM_MASK], sub = rdram[(obj + 0x2F5u) & PS2_RAM_MASK];
            if (major != 0u || sub < 1u || sub > 3u)
                return g_inner(rdram, ctx, runtime);
            // The yaw turns the model's +z (his facing) to (sin yaw, cos yaw) (Ry, frame_patches' eulerMatrix), so
            // turning the delta by Ry(angle) first sends him toward his yaw + angle.
            const float angle = g_angle.load(std::memory_order_relaxed);
            const float c = std::cos(angle), s = std::sin(angle);
            const float x = rdF(rdram, obj + 0x330u), z = rdF(rdram, obj + 0x338u);
            wrF(rdram, obj + 0x330u, x * c + z * s);
            wrF(rdram, obj + 0x338u, -x * s + z * c);
            g_inner(rdram, ctx, runtime);
            wrF(rdram, obj + 0x330u, x);
            wrF(rdram, obj + 0x338u, z);
        }
    }

    std::atomic<bool> &setting() { return g_setting; }
    bool enabled() { return g_setting.load(std::memory_order_relaxed); }

    void setSlide(float angle)
    {
        g_angle.store(angle, std::memory_order_relaxed);
        g_slide.store(true, std::memory_order_relaxed);
    }
    void clearSlide() { g_slide.store(false, std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("KAMITE_STRAFING_TIGER");
        g_setting = env && std::string_view(env) == "1";
        if (!runtime.hasFunction(kAddNullSpeed) || !(g_inner = runtime.lookupFunction(kAddNullSpeed)) ||
            !FunctionHooks::instance().replace(kAddNullSpeed, "cObjBase_addNullSpeed (Strafing Tiger slide)",
                                               &addNullSpeedWrapped, ReplacementStatus::Native,
                                               "Strafing Tiger turns the walk's root delta; off: the original"))
        {
            g_inner = nullptr;
            g_setting = false;
            KM_WARN(Game, "Strafing Tiger: cObjBase_addNullSpeed (0x14F928) is not hookable: off");
        }
    }
}
