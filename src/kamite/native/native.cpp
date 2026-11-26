// Native EE functions: the registry and their installation (native.h); the shadow harness is shadow.cpp.

#include "kamite/native/native.h"
#include "kamite/native/shadow.h"
#include "kamite/native/chain_digest.h"
#include "kamite/native/draw_list.h"

#include "kamite/frame_patches.h"
#include "kamite/function_hooks.h"
#include "kamite/git_commit.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define KM_GETPID _getpid
#else
#include <unistd.h>
#define KM_GETPID getpid
#endif

extern uint64_t g_ps2xEeDeferred;   // EeScheduler.cpp: checkpoint cycles not yet accounted

namespace kamite::native
{
    namespace
    {
        std::vector<NativeFunction> &registry()
        {
            static std::vector<NativeFunction> functions;
            return functions;
        }

        // The installed natives: one trampoline per slot (a template instance per index), no lookup on the call.
        constexpr size_t kMaxNatives = 128;
        struct Installed
        {
            NativeFunction fn;
            PS2Runtime::RecompiledFunction held = nullptr;
        };
        std::array<Installed, kMaxNatives> g_installed;
        size_t g_installedCount = 0;

        template <size_t I>
        void nativeEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Installed &slot = g_installed[I];
            RealEnv env{rdram, runtime};
            if (ctx->pc != slot.fn.address || !runNative(slot.fn, slot.fn.runReal, env, ctx))
                slot.held(rdram, ctx, runtime); // declined: the slot's previous function
        }
        template <size_t... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, kMaxNatives> entries(std::index_sequence<I...>)
        {
            return {&nativeEntry<I>...};
        }
        constexpr std::array<PS2Runtime::RecompiledFunction, kMaxNatives> kEntries = entries(std::make_index_sequence<kMaxNatives>{});

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        // KAMITE_NATIVE_<SUBSYSTEM>: unset = the checked functions, 0 = none, anything else = all.
        bool switchedOn(const NativeFunction &fn, std::string &how)
        {
            const std::string variable = "KAMITE_NATIVE_" + upper(fn.subsystem);
            const char *value = std::getenv(variable.c_str());
            how = variable + "=" + (value && *value ? value : "unset");
            if (!value || !*value)
                return fn.checked;
            return std::strcmp(value, "0") != 0;
        }

        bool listed(const std::string &list, const NativeFunction &fn)
        {
            size_t at = 0;
            while (at <= list.size())
            {
                size_t end = list.find_first_of(", ;", at);
                if (end == std::string::npos)
                    end = list.size();
                const std::string token = lower(list.substr(at, end - at));
                at = end + 1;
                if (token.empty())
                    continue;
                if (token == "all" || token == lower(fn.name) || token == lower(fn.subsystem))
                    return true;
                if (token.rfind("0x", 0) == 0 && std::strtoul(token.c_str(), nullptr, 16) == fn.address)
                    return true;
            }
            return false;
        }

#if !defined(KAMITE_RELEASE)
        PS2Runtime *g_runtime = nullptr;
        uint64_t chargedCycles() { return g_runtime->eeScheduler().currentCycle() + g_ps2xEeDeferred; }
        const char *replayCase()
        {
            const char *replay = std::getenv("KAMITE_REPLAY_CASE");
            return replay && *replay ? replay : nullptr;
        }
        // A replay's rate as test_mode decides it (--fps60 0|1, else the setting): known at boot, unlike fps60(), which
        // follows the case only once it loads.
        int rate()
        {
            if (const char *fps = std::getenv("KAMITE_REPLAY_FPS60"); replayCase() && fps && *fps)
                return *fps == '1' ? 60 : 30;
            return kamite::fps60() ? 60 : 30;
        }
        // Beside the case during a replay, like the sampler's stacks (<case>.<30|60>.native.txt).
        std::string shadowOutput()
        {
            if (const char *replay = replayCase())
                return std::string(replay) + (rate() == 60 ? ".60" : ".30");
            return "logs/native_" + std::to_string(KM_GETPID());
        }
        std::string shadowHeader()
        {
            const char *replay = replayCase();
            return std::string("commit=") + KAMITE_GIT_COMMIT + " rate=" + std::to_string(rate()) + " case=" + (replay ? replay : "");
        }
        bool g_shadowing = false;
#endif
    }

    namespace
    {
        // Natives that run under the features' hooks at their address (rung 4 M4, K's round 1: the pose under
        // frame_patches', game_options' and style_pose's wrappers): the slot keeps FunctionHooks' trampoline from boot.
        constexpr uint32_t kUnderHooks[] = {0x158328u};
        template <size_t I>
        void underHooksEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            FunctionHooks::runUnder(kUnderHooks[I], rdram, ctx, runtime);
        }
    }

    void reserveUnderHooks(PS2Runtime &runtime)
    {
        (void)runtime;
        if (!FunctionHooks::instance().keepUnderHooks(kUnderHooks[0], "func_00158328 (motion pose)", &underHooksEntry<0>))
            std::fprintf(stderr, "[native] func_00158328: its natives can't go under its hooks\n");
    }

    void registerFunction(const NativeFunction &function)
    {
        registry().push_back(function);
    }

    void runEntryObservers(uint32_t address, uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
    {
        FunctionHooks::runEntryObservers(address, ram, ctx, runtime);
    }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        libvu0::registerFunctions();
        packet::registerFunctions();
        motion::registerFunctions();
        // Rung 3 step b's draw list (draw_list.h): live in every build unless KAMITE_NATIVE_DRAWLIST=0.
        draw_list::begin(runtime.memory().getRDRAM());

#if !defined(KAMITE_RELEASE)
        const bool replaying = replayCase() != nullptr;
        // KAMITE_NATIVE_SHADOW (claude/native-next.md rung 2): unset = during a replay, every native that is not on.
        const char *shadowValue = std::getenv("KAMITE_NATIVE_SHADOW");
        const bool shadowUnset = !shadowValue || !*shadowValue;
        const std::string shadowList = shadowUnset ? "" : lower(shadowValue);
        const bool shadowNone = shadowList == "none" || shadowList == "0";
        // Rung 3's chain digest (chain_digest.h): every replay, whatever is shadowed.
        if (replaying)
        {
            chain_digest::begin(shadowOutput().c_str());
            // Rung 3 step b's draw list report (draw_list.h): every replay, live or shadow.
            draw_list::report(shadowOutput().c_str(), shadowHeader());
        }
#endif
#if !defined(KAMITE_RELEASE)
        // KAMITE_NATIVE_OFF=<names, 0x addresses, subsystems> (dev probe, claude/strip-list.md): those natives are
        // neither installed nor shadowed (the recompiled code runs as in a build without them), to compare a build
        // with and without some natives at full speed.
        const char *offValue = std::getenv("KAMITE_NATIVE_OFF");
        const std::string offList = offValue && *offValue ? lower(offValue) : "";
        // A native may be registered twice at one address (rung 4, Nov 26: the twist variants after their checked
        // selves): installed in registry order, a later one wraps what the slot holds (the live native, whose declines
        // fall to the recompiled code). The harness keeps one slot per address: a second shadow there is left out.
        std::vector<uint32_t> shadowedAddresses;
#endif
        for (const NativeFunction &fn : registry())
        {
#if !defined(KAMITE_RELEASE)
            if (!offList.empty() && listed(offList, fn))
            {
                std::fprintf(stderr, "[native] %s 0x%08X: off (KAMITE_NATIVE_OFF)\n", fn.name, fn.address);
                continue;
            }
#endif
            std::string how;
            const bool on = switchedOn(fn, how);
            bool shadowed = false;
#if !defined(KAMITE_RELEASE)
            shadowed = !shadowNone && (shadowUnset ? replaying && !on : listed(shadowList, fn));
#endif
            if (!shadowed && !on)
                continue;
            if (!runtime.hasFunction(fn.address))
            {
                std::fprintf(stderr, "[native] %s 0x%08X: no recompiled function there, left out\n", fn.name, fn.address);
                continue;
            }
            // Every native goes in the function's place behind its entry observers (K, Nov 21): observers keep running
            // first; `held` is what a declined call runs (the recompiled function, or a hook's replacement).
#if !defined(KAMITE_RELEASE)
            if (shadowed && std::find(shadowedAddresses.begin(), shadowedAddresses.end(), fn.address) != shadowedAddresses.end())
            {
                std::fprintf(stderr, "[native] %s 0x%08X: not shadowed (another native is shadow-checked there)\n", fn.name,
                             fn.address);
                continue;
            }
            if (shadowed)
            {
                if (!g_shadowing)
                {
                    g_runtime = &runtime;
                    shadow::Settings settings;
                    settings.output = &shadowOutput;
                    settings.header = &shadowHeader;
                    settings.cycles = &chargedCycles;
                    // A backward branch's charge, read as PS2Runtime::eeCheckpointFull reads it.
                    const char *loop = std::getenv("PS2X_EE_LOOP_CYCLES");
                    settings.loopCycles = loop && *loop ? static_cast<uint32_t>(std::max(1, std::atoi(loop))) : 32u;
                    shadow::begin(settings);
                    g_shadowing = true;
                }
                const PS2Runtime::RecompiledFunction held = FunctionHooks::instance().replaceBehindObservers(
                    fn.address, fn.name, &shadow::entry, ReplacementStatus::Native, "shadow check: native beside the recompiled code");
                if (!held)
                    continue;
                shadow::add(fn, held);
                shadowedAddresses.push_back(fn.address);
                std::fprintf(stderr, "[native] %s 0x%08X: shadow-checked (%s)\n", fn.name, fn.address, how.c_str());
                continue;
            }
#endif
            if (g_installedCount == kMaxNatives)
            {
                std::fprintf(stderr, "[native] %s: more than %zu natives, left out\n", fn.name, kMaxNatives);
                continue;
            }
            g_installed[g_installedCount] = Installed{fn, nullptr};
            const PS2Runtime::RecompiledFunction held = FunctionHooks::instance().replaceBehindObservers(
                fn.address, fn.name, kEntries[g_installedCount], ReplacementStatus::Native, how);
            if (!held)
                continue;
            g_installed[g_installedCount].held = held;
            ++g_installedCount;
            std::fprintf(stderr, "[native] %s 0x%08X: native (%s)\n", fn.name, fn.address, how.c_str());
        }
    }

    void flush()
    {
#if !defined(KAMITE_RELEASE)
        chain_digest::end();
        draw_list::end();
        if (g_shadowing)
            shadow::writeSummary();
#endif
    }
}
