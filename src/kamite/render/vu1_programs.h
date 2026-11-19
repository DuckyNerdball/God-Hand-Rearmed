#pragma once

// The game's VU1 microprograms rewritten as native code (claude/native-vu1.md "Program specs"): each one decodes what
// the game uploads to VU1 memory, transforms it and emits the same GS packets the microprogram would. The runtime runs
// them in place of the compiled microcode (runtime/ps2_vu1_native.h). PS2X_VU1_NATIVE (GHR.ini [Runtime]): unset, the
// programs checked in the game; 1, every one; 0, none (the compiled microcode only).
namespace kamite::render
{
    // Registers every native program with the runtime. Once, before the game's first VU1 run.
    void registerNativeVu1Programs();
}
