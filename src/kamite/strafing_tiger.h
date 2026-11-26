#pragma once

#include <atomic>

class PS2Runtime;

// Strafing Tiger (prototype, F2 > Controls; later.md "Strafing Tiger", the tech deck's strafing-tiger.md): in Modern's
// free camera, Gene faces where the camera points and the left stick moves him relative to the camera without turning
// him. Prototype step 1: his own walk / run / back-pedal play and slide the way the stick points (the root motion's
// delta turned at cObjBase_addNullSpeed); the strafe animations come in step 2. Off = nothing changes.
namespace kamite::strafing_tiger
{
    // KAMITE_STRAFING_TIGER=1 at start, or the F2 checkbox (no launcher / GHR.ini row while it's a prototype).
    std::atomic<bool> &setting();
    bool enabled();

    // Window thread (modern_controls' pad filter): Gene is to slide this far (radians, his yaw's sense) from where his
    // walk / run / back-pedal carries him, or not at all.
    void setSlide(float angle);
    void clearSlide();

    // Hooks cObjBase_addNullSpeed (0x14F928).
    void install(PS2Runtime &runtime);
}
