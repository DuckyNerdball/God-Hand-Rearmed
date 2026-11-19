#include "kamite/render/vu1_programs.h"

#include "runtime/ps2_vu1_native.h"

namespace kamite::render
{
    Vu1NativeProgram sceneryStripProgram();
    Vu1NativeProgram characterSkinProgram();

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
        strip.checked = true; // bit-exact on every traced run, K's in-game check and all test cases (Nov 19)
        registerVu1NativeProgram(strip);

        // Character (character_skin.cpp): pairs 0x00B8..0x0530 of the character images (c7e9 / d905); FCAND at 0x0378
        // is patched by near_geometry.cpp.
        Vu1NativeEntry character;
        character.name = "character";
        character.codeBegin = 0x00B8u;
        character.codeEnd = 0x0530u;
        character.codeHash = 0x9DD630AB008D2E9Bull;
        character.variable[0] = 0x0378u;
        character.variableCount = 1u;
        character.entryPcs[0] = 0x00B8u; // MSCAL
        character.entryPcs[1] = 0x0520u; // MSCNT ("b 0x00B8")
        character.entryCount = 2u;
        character.program = characterSkinProgram();
        character.checked = false; // PS2X_VU1_NATIVE=1 until checked in the game (claude/native-vu1.md "Done")
        registerVu1NativeProgram(character);
    }
}
