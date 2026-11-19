// The extension heap (see xram_heap.h).
#include "kamite/xram_heap.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstring>

namespace kamite::xram_heap
{
    namespace
    {
        constexpr uint32_t kMagic = 0x58524847u; // "GHRX"
        constexpr uint32_t kVersion = 1u;
        constexpr uint32_t kDesc = PS2_XRAM_BASE + 0x10u;
        constexpr uint32_t kRoot = PS2_XRAM_BASE + 0x40u;
        constexpr uint32_t kSize = PS2_XRAM_SIZE - 0x40u;
        constexpr uint32_t kHeader = 0x20u;
        constexpr uint32_t kHeapReset = 0x002A94F0u; // func_002A94F0(a0 cHeap *): empties the heap
        bool g_installed = false;

        uint32_t r32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }
        void w32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & (PS2_RAM_MASK & ~3u)), &v, 4); }

        // The heap's head, made at the first use (also after a state from before it was loaded: the extension is clear).
        void ensure(uint8_t *rdram)
        {
            if (r32(rdram, PS2_XRAM_BASE) == kMagic)
                return;
            w32(rdram, PS2_XRAM_BASE, kMagic);
            w32(rdram, PS2_XRAM_BASE + 4u, kVersion);
            w32(rdram, PS2_XRAM_BASE + 8u, kRoot + kHeader); // the highest byte ever handed out, + 1 (zero above it)
            w32(rdram, kDesc + 0u, kRoot); // Arena_init 0x2A9498, then func_002A94F0
            w32(rdram, kDesc + 4u, kSize);
            w32(rdram, kDesc + 8u, 0u);
            w32(rdram, kRoot + 0x0u, kDesc);
            w32(rdram, kRoot + 0x4u, 0u);
            w32(rdram, kRoot + 0x8u, 0u);
            w32(rdram, kRoot + 0xCu, kSize - kHeader);
            w32(rdram, kRoot + 0x10u, kHeader);
            KM_INFO(Game, "extension heap made: 0x" << std::hex << kRoot << ", 0x" << kSize << std::dec << " bytes");
        }

        // cHeap_free's unlink, then the block and the free room after it cleared (up to the high-water mark: a trimmed
        // or freed block's old bytes would otherwise stay in every save state's XRAM section).
        void unlink(uint8_t *rdram, uint32_t block)
        {
            const uint32_t prev = r32(rdram, block + 4u), next = r32(rdram, block + 8u), used = r32(rdram, block + 0x10u);
            const uint32_t end = std::min(block + used + r32(rdram, block + 0xCu), r32(rdram, PS2_XRAM_BASE + 8u));
            w32(rdram, prev + 8u, next);
            w32(rdram, prev + 0xCu, r32(rdram, prev + 0xCu) + used + r32(rdram, block + 0xCu));
            if (next != 0u)
                w32(rdram, next + 4u, prev);
            if (end > block)
                std::memset(rdram + (block & PS2_RAM_MASK), 0, end - block);
        }

        // func_002A94F0 entry: a game heap is emptied; the blocks standing in for its blocks go with it.
        void onReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t heap = GPR_U32(ctx, 4);
            if (r32(rdram, PS2_XRAM_BASE) != kMagic || heap == kDesc)
                return;
            uint32_t freed = 0, bytes = 0;
            for (uint32_t block = r32(rdram, kRoot + 8u), n = 0; block != 0u && n < 0x100000u; ++n)
            {
                const uint32_t next = r32(rdram, block + 8u);
                if (r32(rdram, block + 0x14u) == heap && r32(rdram, block + 0x18u) == kMagic)
                {
                    bytes += r32(rdram, block + 0x10u);
                    unlink(rdram, block);
                    ++freed;
                }
                block = next;
            }
            if (freed != 0u)
                KM_INFO(Game, "extension heap: heap 0x" << std::hex << heap << std::dec << " reset, " << freed << " block(s) ("
                                                        << bytes << " bytes) freed with it");
        }
    }

    uint32_t alloc(uint8_t *rdram, uint32_t size, uint32_t align, uint32_t forHeap)
    {
        ensure(rdram);
        if (align < 16u || (align & (align - 1u)) != 0u)
            align = 16u;
        const uint64_t need = static_cast<uint64_t>(size) + kHeader;
        // First fit, placed as cHeap_allocFirstFit places it: after the block's used part, data aligned.
        for (uint32_t block = kRoot, n = 0; block != 0u && n < 0x100000u; ++n)
        {
            const uint32_t free = r32(rdram, block + 0xCu), end = block + r32(rdram, block + 0x10u);
            const uint32_t data = (end + kHeader + align - 1u) & ~(align - 1u);
            const uint32_t header = data - kHeader, gap = header - end;
            if (need + gap <= free)
            {
                const uint32_t next = r32(rdram, block + 8u);
                w32(rdram, header + 0x0u, kDesc);
                w32(rdram, header + 0x4u, block);
                w32(rdram, header + 0x8u, next);
                w32(rdram, header + 0xCu, free - gap - static_cast<uint32_t>(need));
                w32(rdram, header + 0x10u, static_cast<uint32_t>(need));
                w32(rdram, header + 0x14u, forHeap);
                w32(rdram, header + 0x18u, kMagic);
                w32(rdram, header + 0x1Cu, 0u);
                if (next != 0u)
                    w32(rdram, next + 4u, header);
                w32(rdram, block + 8u, header);
                w32(rdram, block + 0xCu, gap);
                w32(rdram, PS2_XRAM_BASE + 8u, std::max(r32(rdram, PS2_XRAM_BASE + 8u), header + static_cast<uint32_t>(need)));
                return data;
            }
            block = r32(rdram, block + 8u);
        }
        return 0u;
    }

    void free(uint8_t *rdram, uint32_t data)
    {
        if (owns(data) && r32(rdram, data - kHeader + 0x18u) == kMagic)
            unlink(rdram, data - kHeader);
    }

    bool owns(uint32_t data)
    {
        const uint32_t a = data & PS2_RAM_MASK;
        return a >= kRoot + 2u * kHeader && a < PS2_RDRAM_SIZE;
    }

    uint32_t blockRoom(const uint8_t *rdram, uint32_t data)
    {
        const uint32_t a = data & PS2_RAM_MASK;
        if (a < 0x100000u + kHeader || a >= PS2_RDRAM_SIZE)
            return 0u;
        const uint32_t used = r32(rdram, a - kHeader + 0x10u);
        return used > kHeader && used < PS2_RDRAM_SIZE ? used - kHeader : 0u;
    }

    void install(PS2Runtime &)
    {
        if (g_installed)
            return;
        g_installed = FunctionHooks::instance().observeEntry(kHeapReset, "xram_heap_reset", &onReset);
    }
}
