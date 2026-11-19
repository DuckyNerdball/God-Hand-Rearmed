#pragma once

#include <cstdint>

class PS2Runtime;

// The extension heap: one heap in the game's own cHeap format at the bottom of the extension RAM (PS2_XRAM_BASE,
// ps2_memory.h), so the game's own free (cHeap_free 0x2A9680: unlinks through the block's prev, ignores its heap
// argument) and trim (func_002A9648) work on its blocks. It holds the buffers of game files a mod made bigger, each
// standing in for a block of one of the game's heaps: that heap is written in the block's header at +0x14 (the game
// never touches +0x14..+0x1F) and a reset of it (func_002A94F0, also under Arena_init) frees them too, as it would
// have freed the block in the game's heap. Everything lives in guest RAM, so save states carry it (the XRAM section).
//   cHeap descriptor {+0 first block, +4 size, +8 0}; block {+0 desc, +4 prev, +8 next, +0xC free after it,
//   +0x10 used incl. the 0x20 header}. The heap is created at its first use; vanilla never touches the extension.
namespace kamite::xram_heap
{
    // A block of size bytes, data aligned to align (a power of two, >= 16), standing in for a block of the game heap
    // forHeap. 0: no room.
    uint32_t alloc(uint8_t *rdram, uint32_t size, uint32_t align, uint32_t forHeap);
    // Frees a block of this heap (the game's cHeap_free does the same); zeroes it so save states stay small.
    void free(uint8_t *rdram, uint32_t data);
    bool owns(uint32_t data);
    // A heap block's room for data (used - header), whichever heap it is in; 0 when data doesn't look like one.
    uint32_t blockRoom(const uint8_t *rdram, uint32_t data);

    // The reset observer (func_002A94F0). After FunctionHooks::attach; installed once, by the first user.
    void install(PS2Runtime &runtime);
}
