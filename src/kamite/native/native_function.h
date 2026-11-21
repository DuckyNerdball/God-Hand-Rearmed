#pragma once

// A native EE function: hand-written C++ that replaces one recompiled guest function (claude/native-next.md, rung 0
// rules; rung 2's shadow harness checks it). Each one is described by a NativeFunction and registered by its
// subsystem's file (libvu0.cpp ...); native.cpp installs it, shadow.cpp checks it against the recompiled code.
//
// The rules a native's run() follows (claude/gt3-revisited.md "Rules for native replacements"):
//   - It works on the guest RAM through env.ram (the real RAM, or the shadow harness's scratch copy of the footprint)
//     and on ctx, nothing else: no globals, no runtime calls, no host state.
//   - It returns false, having changed nothing, when it declines a call (huge or special values, a pointer outside
//     plain RAM: the entry checks); the slot's previous function then runs. It returns true when it ran.
//   - EE time is charged where the original charges it: env.checkpoint(32) at each taken backward branch, env.call()
//     at each guest call (8 cycles; ctx->pc set to the callee first). When either returns true (a checkpoint came due)
//     it leaves ctx and RAM exactly as the original has them at that point and returns true: the original's resume
//     entry carries on later.
//   - It leaves ctx as the original does, scratch registers included (the harness compares the whole context), unless
//     its NativeFunction lists a don't-care field with a reason.
//   - Floats run under the game thread's MXCSR (chop + DAZ / FTZ); maths through vu_vec.h only.

#include "ps2_runtime.h"
#include "runtime/ps2_address.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// The natives' one-instruction helpers (ee_ctx.h, plainRam) must inline: a call per guest instruction costs more than
// the instruction (GCC and MSVC leave them out of line in a big file otherwise).
#if defined(_MSC_VER)
#define KM_NATIVE_INLINE __forceinline
#else
#define KM_NATIVE_INLINE inline __attribute__((always_inline))
#endif

namespace kamite::native
{
    // The guest bytes a call reads or writes, as the function addresses them (before the RAM mask). The harness
    // copies them into its scratch RAM before the native runs and compares them after the original ran.
    struct Footprint
    {
        static constexpr int kMaxRanges = 16;
        struct Range
        {
            uint32_t address;
            uint32_t size;
        };
        Range ranges[kMaxRanges];
        int count = 0;
        bool overflow = false; // more than kMaxRanges: the call is not checked
        void add(uint32_t address, uint32_t size)
        {
            if (count < kMaxRanges)
                ranges[count++] = Range{address, size};
            else
                overflow = true;
        }
    };

    // A recorded footprint (a NativeFunction with footprint == nullptr; rung 3, K's pick): the harness runs the native
    // on its scratch with every first touch of a byte filled from the real RAM and logged; the log is the call's
    // footprint. Ranges are RAM offsets (address & PS2_RAM_MASK). A rerun (an early exit, or shadow_test rerun) starts
    // from the saved bytes: a range it hadn't touched before is filled with zeros and marks it strayed.
    struct Recording
    {
        std::vector<Footprint::Range> ranges;
        std::vector<uint8_t> in;        // each range's bytes as filled (the call's input), in range order
        const uint8_t *real = nullptr;  // the RAM to fill from; nullptr on a rerun
        uint32_t maxBytes = 0;
        uint32_t bytes = 0;
        bool overflow = false;          // more than kMaxRecordedRanges ranges or maxBytes bytes: not checked
        bool strayed = false;           // a rerun touched a byte the first run hadn't
        static constexpr size_t kMaxRecordedRanges = 256;
        void reset(const uint8_t *from, uint32_t limit)
        {
            ranges.clear();
            in.clear();
            real = from;
            maxBytes = limit;
            bytes = 0;
            overflow = strayed = false;
        }
        // recording.cpp: fill [offset, offset + size) on first touch; mark / unmark the ranges' bytes as present (a
        // rerun marks the saved ranges first; the harness unmarks them after every run).
        void touch(uint8_t *scratch, uint32_t offset, uint32_t size);
        void mark() const;
        void unmark() const;
    };

    // native.cpp: the entry observers of `address` (FunctionHooks::observeEntry), run by a native that runs an observed
    // function inline where the guest call would have entered it (K's pick, Nov 21).
    void runEntryObservers(uint32_t address, uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime);

    // Where a native's memory accesses go: the real RAM (RealEnv) or the shadow scratch (ShadowEnv). Both are the
    // runtime's 128 MB backing: a guest address in plain RAM is ram + (address & PS2_RAM_MASK). New natives reach RAM
    // through env.at(offset, size) (the bytes of one access, offset from plainRam): the real RAM's pointer, or the
    // scratch's with a recorded footprint filled on first touch.
    // PS2X_EE_LOOP_CYCLES, read as the runtime reads it (eeCheckpointFull): what a backward branch's 32 stands for.
    inline uint32_t eeLoopCycles()
    {
        static const uint32_t cycles = [] {
            const char *v = std::getenv("PS2X_EE_LOOP_CYCLES");
            const int n = v && *v ? std::atoi(v) : 32;
            return static_cast<uint32_t>(n < 1 ? 1 : n);
        }();
        return cycles;
    }

    // Both envs can run a native without mid-function checkpoints (NativeFunction::kChargeAtEnd; K's answer 2 on the
    // method, Nov 21): with atEnd set, checkpoint() and call() only add up what the original would charge there (a
    // backward branch's 32 as PS2X_EE_LOOP_CYCLES) and never come due; finish(), after the native ran to its end, charges
    // the sum once and says whether a checkpoint came due (ctx is then the original's at its return: the scheduler resumes
    // the caller at ra). native.cpp and the harness set atEnd and call finish() (runNative below); natives don't.
    struct RealEnv
    {
        uint8_t *ram;
        PS2Runtime *runtime;
        bool atEnd = false;
        uint64_t owed = 0;   // atEnd: cycles summed so far (loops apart)
        uint64_t loops = 0;  // atEnd: backward branches so far
        bool checkpoint(uint32_t cycles)
        {
            if (atEnd)
            {
                if (cycles == 32u)
                    ++loops;
                else
                    owed += cycles;
                return false;
            }
            return runtime->eeCheckpointDue(cycles);
        }
        bool call() // EeScheduler::kGuestDispatchCycles
        {
            if (atEnd)
            {
                owed += 8u;
                return false;
            }
            return runtime->eeCheckpointDue(8u);
        }
        bool finish()
        {
            uint64_t total = owed + loops * eeLoopCycles();
            atEnd = false;
            owed = loops = 0;
            bool due = false;
            while (total != 0u)
            {
                // eeCheckpointDue reads 32 as a loop's PS2X_EE_LOOP_CYCLES: a sum of exactly 32 goes as 31 + 1 then.
                uint32_t part = total > 0x40000000u ? 0x40000000u : static_cast<uint32_t>(total);
                if (part == 32u && eeLoopCycles() != 32u)
                    part = 31u;
                due |= runtime->eeCheckpointDue(part);
                total -= part;
            }
            return due;
        }
        void discard()
        {
            atEnd = false;
            owed = loops = 0;
        }
        uint8_t *at(uint32_t offset, uint32_t) { return ram + offset; }
        // An observed function run inline: its observers first, on the real RAM and context (ctx at its entry).
        void observe(uint32_t address, R5900Context *ctx) { runEntryObservers(address, ram, ctx, runtime); }
    };

    struct ShadowEnv
    {
        uint8_t *ram;
        int dueAt = 0;          // the checkpoint (1-based) that comes due in this run; 0 = none
        int checkpoints = 0;    // checkpoints charged so far
        uint64_t charged = 0;   // their EE cycles
        uint32_t loopCycles = 32u; // what a backward branch's 32 stands for (PS2X_EE_LOOP_CYCLES)
        Recording *record = nullptr; // a recorded footprint: fill on first touch
        bool atEnd = false;     // as RealEnv's: sum, charge once at finish() (one checkpoint)
        uint64_t owed = 0;
        uint8_t *at(uint32_t offset, uint32_t size)
        {
            if (record)
                record->touch(ram, offset, size);
            return ram + offset;
        }
        bool due()
        {
            ++checkpoints;
            return checkpoints == dueAt;
        }
        bool checkpoint(uint32_t cycles)
        {
            if (atEnd)
            {
                owed += cycles == 32u ? loopCycles : cycles;
                return false;
            }
            charged += cycles == 32u ? loopCycles : cycles;
            return due();
        }
        bool call()
        {
            if (atEnd)
            {
                owed += 8u;
                return false;
            }
            charged += 8u;
            return due();
        }
        bool finish()
        {
            charged += owed;
            const bool hadOwed = owed != 0u;
            atEnd = false;
            owed = 0;
            return hadOwed && due();
        }
        void discard()
        {
            atEnd = false;
            owed = 0;
        }
        // The shadow run leaves observers to the real call (they act on the real RAM).
        void observe(uint32_t, R5900Context *) {}
    };

    // A context field the comparison leaves out (with the reason, which the summary prints).
    struct DontCare
    {
        uint32_t offset;
        uint32_t size;
        const char *reason;
    };

    struct NativeFunction
    {
        const char *name;      // the recompiled function's name (kamite_functions.csv)
        uint32_t address;      // its entry
        const char *subsystem; // its switch: KAMITE_NATIVE_<SUBSYSTEM> (unset = checked ones, 1 = all, 0 = none)
        bool checked;          // on by default (0 harness mismatches over all cases, replays identical, faster)
        bool (*runReal)(RealEnv &env, R5900Context *ctx);
        bool (*runShadow)(ShadowEnv &env, R5900Context *ctx);
        void (*footprint)(const R5900Context *ctx, Footprint &fp); // nullptr: recorded (Recording; env.at only)
        uint32_t ulps = 0;     // 0 = bit-exact; else a loose function: float words may differ by this many ulps
        uint32_t every = 1;    // the shadow harness checks every Nth call
        const DontCare *dontCare = nullptr;
        int dontCareCount = 0;
        uint32_t maxBytes = 0; // a recorded footprint's limit (0 = 64 KB); more is skipped
        uint32_t flags = 0;    // kRenderPath, kChargeAtEnd
        uint32_t deadStack = 0; // kRenderPath: the bytes below the entry sp that are dead after the call (0 = 64 KB)

        // K's answer 2 on the method (Nov 21), for render-path natives (packets, object fields: what the game keeps):
        //  - kRenderPath: the harness compares what lasts: guest RAM outside the call's dead stack, v0 / v1's low 64
        //    bits (the return value), f0, the callee-saved registers (s0-s7, gp, sp, fp, ra; f20-f31), pc and the
        //    rest of the context, and the EE cycles; the scratch registers (at, a0-a3, t0-t9, k0 / k1, v0 / v1's upper
        //    halves, hi / lo, sa, f1-f19, the FPU accumulator and fcr31, VU0's vf1-vf31 / vi1-vi15 / ACC / Q / P / I
        //    and its status / MAC / clip flags) and the stack below the entry sp are don't-care (lastingContextByte,
        //    deadStackByte). A native still sets everything the original's callers can read.
        //  - kChargeAtEnd: the native runs without mid-function checkpoints, its cycles (the original's own sum)
        //    charged once at its end (RealEnv / ShadowEnv atEnd). A call the original stopped at one of its own
        //    checkpoints can't be compared with a whole run: the harness counts it as interrupted.
        //    Proven by one K build (every .ram.bin identical); else back to the original's checkpoints.
        static constexpr uint32_t kRenderPath = 1u;
        static constexpr uint32_t kChargeAtEnd = 2u;
        static constexpr uint32_t kDefaultDeadStack = 64u * 1024u;
    };

    // kRenderPath: is this byte of R5900Context compared (what lasts after the call), or scratch?
    inline bool lastingContextByte(size_t offset)
    {
        auto in = [offset](size_t begin, size_t size) { return offset >= begin && offset < begin + size; };
        constexpr size_t r = offsetof(R5900Context, r), q = sizeof(__m128i);
        if (in(r, 32 * q))
        {
            const size_t reg = (offset - r) / q, byte = (offset - r) % q;
            if (reg == 1u || (reg >= 4u && reg <= 15u) || (reg >= 24u && reg <= 27u)) // at, a0-a3, t0-t7, t8-t9, k0-k1
                return false;
            if ((reg == 2u || reg == 3u) && byte >= 8u) // v0 / v1: the return value is the low 64 bits
                return false;
            return true;
        }
        if (in(offsetof(R5900Context, hi), 8) || in(offsetof(R5900Context, lo), 8) ||
            in(offsetof(R5900Context, hi1), 8) || in(offsetof(R5900Context, lo1), 8) ||
            in(offsetof(R5900Context, sa), 4))
            return false;
        if (in(offsetof(R5900Context, vu0_vf) + sizeof(__m128), 31 * sizeof(__m128))) // vf1-vf31 (vf0 is constant)
            return false;
        if (in(offsetof(R5900Context, vi) + 2, 15 * 2)) // vi1-vi15
            return false;
        if (in(offsetof(R5900Context, vu0_q), 4) || in(offsetof(R5900Context, vu0_p), 4) ||
            in(offsetof(R5900Context, vu0_i), 4) || in(offsetof(R5900Context, vu0_acc), sizeof(__m128)) ||
            in(offsetof(R5900Context, vu0_status), 2) || in(offsetof(R5900Context, vu0_mac_flags), 4) ||
            in(offsetof(R5900Context, vu0_clip_flags), 4) || in(offsetof(R5900Context, vu0_clip_flags2), 4))
            return false;
        if (in(offsetof(R5900Context, f) + 4, 19 * 4)) // f1-f19 (f0 = the float return value)
            return false;
        if (in(offsetof(R5900Context, f_acc), 4) || in(offsetof(R5900Context, fcr31), 4))
            return false;
        return true;
    }

    // kRenderPath: is this RAM offset in the call's dead stack, the deadStack bytes below the entry sp (sp at entry,
    // `before`)? Its frames and its callees' lie there; nothing reads them after the return.
    inline bool deadStackByte(const NativeFunction &fn, const R5900Context &before, uint32_t offset)
    {
        uint32_t sp;
        std::memcpy(&sp, &before.r[29], 4);
        const uint32_t window = fn.deadStack ? fn.deadStack : NativeFunction::kDefaultDeadStack;
        const uint32_t top = sp & PS2_RAM_MASK;
        return top >= window ? offset - (top - window) < window : offset < top;
    }

    // How native.cpp and the harness run a native: kChargeAtEnd sets the env's atEnd and charges the sum once after a
    // run (a decline charges nothing: the native changed nothing).
    template <class Env>
    inline bool runNative(const NativeFunction &fn, bool (*run)(Env &, R5900Context *), Env &env, R5900Context *ctx)
    {
        env.atEnd = (fn.flags & NativeFunction::kChargeAtEnd) != 0u;
        if (!run(env, ctx))
        {
            env.discard();
            return false;
        }
        if (env.atEnd)
            env.finish();
        return true;
    }

    // Called by each subsystem's register function before install().
    void registerFunction(const NativeFunction &function);

    // The guest address -> offset of a plain-RAM access of `size` bytes, or false for an address the native must decline
    // (scratchpad, I/O, VU memory, BIOS, KSEG2/3, or a range that wraps the backing): what READn / WRITEn take the
    // runtime's slow path for.
    KM_NATIVE_INLINE bool plainRam(uint32_t address, uint32_t size, uint32_t &offset)
    {
        if (Ps2IsSpecialAddress(address) || Ps2IsSpecialAddress(address + size - 1u))
            return false;
        offset = address & PS2_RAM_MASK;
        return offset + size <= PS2_RAM_MASK + 1u;
    }
}
