#include "kamite/render/vu1_programs.h"

#include "runtime/ps2_vu1_native.h"

namespace kamite::render
{
    Vu1NativeProgram sceneryStripProgram();

    void registerNativeVu1Programs()
    {
        // Scenery strip (scenery_strip.cpp): pairs 0x20C0..0x2380 of the upload at 0x2000; FCAND at 0x2270 is
        // patched by near_geometry.cpp.
        Vu1NativeEntry strip;
        strip.name = "scenery strip";
        strip.codeBegin = 0x20C0u;
        strip.codeEnd = 0x2380u;
        strip.codeHash = 0x8D37B4DDF63F42AAull;
        strip.variable[0] = 0x2270u;
        strip.variableCount = 1u;
        strip.entryPcs[0] = 0x20C0u; // MSCAL
        strip.entryPcs[1] = 0x2338u; // MSCNT after a batch whose last vertex drew
        strip.entryPcs[2] = 0x2378u; // MSCNT after a batch whose last vertex had ADC
        strip.entryCount = 3u;
        strip.program = sceneryStripProgram();
        strip.checked = false; // bit-exact on every traced run; waits for K's check in the game (PS2X_VU1_NATIVE=1)
        registerVu1NativeProgram(strip);
    }
}
