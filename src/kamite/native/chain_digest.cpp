#include "kamite/native/chain_digest.h"

#if !defined(KAMITE_RELEASE)

#include "kamite/test_mode.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_dma_chain.h"

#define XXH_INLINE_ALL
#include "../../../thirdparty/pcsx2/3rdparty/include/xxhash.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace kamite::native::chain_digest
{
    namespace
    {
        std::mutex g_mutex;
        std::string g_path;
        std::FILE *g_file = nullptr;
        bool g_failed = false;

        // KAMITE_CHAIN_DUMP=<last frame> (dev probe, claude/strip-list.md): the replay's chains up to that test frame as
        // their source pieces, into <case>.<rate>.chain.dump: "GHRCDMP1", then per chain u32 frame, u32 channel base,
        // u32 piece count, and per piece u32 RAM offset (0xFFFFFFFF outside the RAM backing), u32 bytes, u64 XXH3, and
        // the bytes themselves when there are at most 4096 of them (EE-built packets; big pieces are model data).
        // tools/dma/chain_dump_compare.py compares two dumps.
        std::string g_dumpPath;
        std::FILE *g_dump = nullptr;
        uint32_t g_dumpLast = 0;

        void observePieces(uint32_t channelBase, const ps2x::dma::ChainPiece *pieces, size_t count, const uint8_t *rdram)
        {
            const uint32_t frame = test_mode::replayFrame();
            if (frame == test_mode::kReplayIdle || frame > g_dumpLast)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_dump)
            {
                g_dump = std::fopen(g_dumpPath.c_str(), "wb");
                if (!g_dump)
                    return;
                std::fwrite("GHRCDMP1", 1, 8, g_dump);
            }
            const uint32_t head[3] = {frame, channelBase, static_cast<uint32_t>(count)};
            std::fwrite(head, sizeof(head), 1, g_dump);
            for (size_t i = 0; i < count; ++i)
            {
                const ps2x::dma::ChainPiece &p = pieces[i];
                const uintptr_t at = reinterpret_cast<uintptr_t>(p.src) - reinterpret_cast<uintptr_t>(rdram);
                const uint32_t offset = p.src >= rdram && at < PS2_RDRAM_SIZE ? static_cast<uint32_t>(at) : 0xFFFFFFFFu;
                const uint64_t hash = XXH3_64bits(p.src, p.len);
                const uint32_t rec[4] = {offset, p.len, static_cast<uint32_t>(hash), static_cast<uint32_t>(hash >> 32)};
                std::fwrite(rec, sizeof(rec), 1, g_dump);
                if (p.len <= 4096u)
                    std::fwrite(p.src, 1, p.len, g_dump);
            }
        }

        void observe(uint32_t channelBase, const uint8_t *data, size_t size)
        {
            // Only the replay's own frames: before the state loads (boot, loading) the frame count follows the host.
            const uint32_t frame = test_mode::replayFrame();
            if (frame == test_mode::kReplayIdle)
                return;
            const uint64_t hash = XXH3_64bits(data, size);
            const uint32_t record[4] = {frame, (((channelBase >> 12) & 0xFu) << 28) | (static_cast<uint32_t>(size) & 0x0FFFFFFFu),
                                        static_cast<uint32_t>(hash), static_cast<uint32_t>(hash >> 32)};
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_file)
            {
                if (g_failed)
                    return;
                g_file = std::fopen(g_path.c_str(), "wb");
                if (!g_file)
                {
                    g_failed = true;
                    std::fprintf(stderr, "[native] chain digest: cannot write %s\n", g_path.c_str());
                    return;
                }
                std::fwrite("GHRCHN1\0", 1, 8, g_file);
            }
            std::fwrite(record, sizeof(record), 1, g_file);
        }
    }

    namespace
    {
        // ---- gs.bin: the GS-input digest (rung 3 step b, claude/native-next.md) ----------------------------------------
        // Every packet the VU worker hands the GIF stage (asyncvif::GifObservers: all GIF paths, in the GS's order),
        // hashed per job; a job counts when the EE queued it during a replay frame (the tag). The VU worker calls packet /
        // jobEnd (one thread), so the running hash needs no lock; the file does.
        std::string g_gsPath;
        std::FILE *g_gsFile = nullptr;
        bool g_gsFailed = false;
        XXH3_state_t g_gsState;
        uint32_t g_gsPackets = 0u;

        uint32_t gsJobTag() { return test_mode::replayFrame(); }
        void gsPacket(const uint8_t *data, uint32_t size, int path)
        {
            if (g_gsPackets++ == 0u)
                XXH3_64bits_reset(&g_gsState);
            const uint32_t head[2] = {size, static_cast<uint32_t>(path)};
            XXH3_64bits_update(&g_gsState, head, sizeof(head));
            XXH3_64bits_update(&g_gsState, data, size);
        }
        void gsJobEnd(uint32_t tag)
        {
            const uint32_t packets = g_gsPackets;
            g_gsPackets = 0u;
            if (packets == 0u || tag == test_mode::kReplayIdle)
                return;
            const uint64_t hash = XXH3_64bits_digest(&g_gsState);
            const uint32_t record[4] = {tag, packets, static_cast<uint32_t>(hash), static_cast<uint32_t>(hash >> 32)};
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_gsFile)
            {
                if (g_gsFailed || g_gsPath.empty())
                    return;
                g_gsFile = std::fopen(g_gsPath.c_str(), "wb");
                if (!g_gsFile)
                {
                    g_gsFailed = true;
                    std::fprintf(stderr, "[native] gs digest: cannot write %s\n", g_gsPath.c_str());
                    return;
                }
                std::fwrite("GHRGSD1\0", 1, 8, g_gsFile);
            }
            std::fwrite(record, sizeof(record), 1, g_gsFile);
        }
        const ps2x::asyncvif::GifObservers g_gsObservers{&gsJobTag, &gsPacket, &gsJobEnd};
    }

    void begin(const char *outputBase)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_gsPath = std::string(outputBase) + ".gs.bin";
            std::remove(g_gsPath.c_str());
        }
        ps2x::asyncvif::setGifObservers(&g_gsObservers);
        std::fprintf(stderr, "[native] gs digest: %s\n", g_gsPath.c_str());
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = std::string(outputBase) + ".chain.bin";
        std::remove(g_path.c_str()); // a previous run's (Replay-Cases.cmd deletes it too)
        ps2x::dma::setChainObserver(&observe);
        std::fprintf(stderr, "[native] chain digest: %s\n", g_path.c_str());
        if (const char *dump = std::getenv("KAMITE_CHAIN_DUMP"); dump && *dump)
        {
            g_dumpLast = static_cast<uint32_t>(std::strtoul(dump, nullptr, 10));
            g_dumpPath = std::string(outputBase) + ".chain.dump";
            std::remove(g_dumpPath.c_str());
            ps2x::dma::setChainPiecesObserver(&observePieces);
            std::fprintf(stderr, "[native] chain dump up to frame %u: %s\n", g_dumpLast, g_dumpPath.c_str());
        }
    }

    void end()
    {
        ps2x::dma::setChainObserver(nullptr);
        ps2x::dma::setChainPiecesObserver(nullptr);
        ps2x::asyncvif::setGifObservers(nullptr);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_gsFile)
        {
            std::fclose(g_gsFile);
            g_gsFile = nullptr;
        }
        if (g_file)
        {
            std::fclose(g_file);
            g_file = nullptr;
        }
        if (g_dump)
        {
            std::fclose(g_dump);
            g_dump = nullptr;
        }
    }
}

#else // KAMITE_RELEASE: no digest

namespace kamite::native::chain_digest
{
    void begin(const char *) {}
    void end() {}
}

#endif
