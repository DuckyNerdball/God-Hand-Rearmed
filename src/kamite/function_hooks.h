#pragma once

// Run-time replacement and entry observation of recompiled guest functions (through the dispatch table).
// GENERIC: the Urban Reign port's src/urban/function_hooks.h (namespace renamed); installDllLoadLogger is the game's.

#include "ps2_runtime.h"

#include <cstdint>
#include <functional>
#include <string>

namespace kamite
{
    using GuestFunction = PS2Runtime::RecompiledFunction;
    using EntryObserver = std::function<void(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)>;

    enum class ReplacementStatus
    {
        Recompiled,
        Observed,
        Native,
        Hle,
        TemporaryStub,
    };

    struct FunctionBinding
    {
        uint32_t address = 0;
        std::string name;
        ReplacementStatus status = ReplacementStatus::Recompiled;
        std::string reason;
    };

    class FunctionHooks
    {
    public:
        static FunctionHooks &instance();

        void attach(PS2Runtime &runtime);
        // Runs `observer` each time the guest calls `address` (before the recompiled function; the function itself is
        // unchanged). Only calls that go through the dispatch table are seen (direct_tail_calls = false in the toml).
        bool observeEntry(uint32_t address, std::string name, EntryObserver observer);
        bool replace(uint32_t address, std::string name, GuestFunction implementation, ReplacementStatus status, std::string reason);
        // Puts `implementation` in the function's place behind its entry observers (claude/native-next.md rung 3, K's
        // pick for natives): the observers keep running first, now or if the address is observed later. Returns what
        // the implementation falls back to (the function it stands in for behind the observers), nullptr when the
        // address has no function.
        GuestFunction replaceBehindObservers(uint32_t address, std::string name, GuestFunction implementation,
                                             ReplacementStatus status, std::string reason);
        // Keeps `address`'s entry observers and what runs behind them (natives) under every hook added later: `entry` (a
        // function that calls runUnder(address, ...)) goes in the slot now, so a later replace() wraps it (its inner
        // function), a later observeEntry joins the observers and a later replaceBehindObservers sets what runs behind
        // them, neither touching the slot. Call before anything else hooks the address (rung 4 M4: the pose under its
        // three wrappers, K's round 1).
        bool keepUnderHooks(uint32_t address, std::string name, GuestFunction entry);
        static void runUnder(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        // Runs the entry observers of `address`, if any (a native that runs an observed function inline calls this
        // where the guest call would have entered it). Cheap when the address has none.
        static void runEntryObservers(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
        GuestFunction original(uint32_t address) const;
        void logBindings() const;

    private:
        FunctionHooks() = default;
        static void observedTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        PS2Runtime *m_runtime = nullptr;
    };

    // The game's SNR2 DLL loader, observed only (no behaviour change): logs every DLL image the game links - its name,
    // base address and size - to the console ([DLL] lines) and, with KAMITE_DLL_LOG=<file>, as CSV rows.
    //
    // SLUS_215.03 (checked against the ELF and recomp/out_B; see KAMITE_PORT_NOTES.md):
    //   0x394DD8 snDllLoaded(a0 = SNR2 image in RAM, a1 = u32 *out or 0): the link step. It calls
    //            0x393F80 func_00393F80(image), the header check (0 ok, 1 magic is not "SNR", 2 version not '1'/'2',
    //            4 image not aligned to header+0x28), then 0x394828 (relocation), then 0x394C18 (a0 = image name,
    //            a1 = image: registers it in the 12-byte entries at 0x3F5520), then the image's constructor runner
    //            (header+0x18, jalr). The game's own "###DLL:Loaded %08x %08x %08x %08x %s\n" print (0x45DBD8) runs
    //            only while the word at 0x3F8528 is 1; it is 0 in the ELF and is not touched here.
    //   Callers: cMc_DllLoad 0x2AE4A8 (jal at 0x2AE594) and func_002BE6A0 (jal at 0x2BE6AC, cRelSys slot load path).
    // Entry observers on 0x394DD8 (image header, before relocation) and, when the recompiled code has an entry for it,
    // 0x394C18 (after relocation). The check result logged is computed the way func_00393F80 does it.
    void installDllLoadLogger(PS2Runtime &runtime);

    // Internal resolution above 1x (PS2X_GS_SCALE > 1): sets the game's flicker-filter weight (0x3C3CCC) to 0x80 before
    // each run of the flip-chain builder 0x30B870, so the FBP 0x70 -> FBP 0 copy is 1:1 instead of blending the lines
    // above and below (which at high resolution are whole native rows away: a triple image). 1x is unchanged.
    void installHiresFlipCopy(PS2Runtime &runtime);
}
