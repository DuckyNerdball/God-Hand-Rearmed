// The VIF executor's offline check (rung 3, Nov 25; claude/native-next.md "VIF executor"). PS2Memory::processVIF1Listed
// runs each listed packet command by command from its pieces (runListedPacket) instead of gathering it; this program
// feeds the same streams through it and through processVIF1Data on the whole gathered chain, and prints one digest per
// input of everything the VIF1 leaves behind: every GIF packet (bytes, in order), every MSCAL / MSCNT (pc, top, itop),
// then VU1 code and data and the VIF1 registers. Inputs: K's F8 RAM captures (the live walk as listed_check makes it)
// and random command streams cut into pieces (VIFcode | payload edges as the chains have them, and random edges that
// make the executor gather). Built twice, against the interpreter before the executor and after it: every line of all
// four runs (old / new binary x listed / whole) must match.
//
//   g++ -std=c++20 -O2 -msse4.1 <the runtime's includes> tools/dma/vif_executor_check.cpp
//       runtime/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp runtime/ps2xRuntime/src/lib/ps2_memory.cpp -o /tmp/vec
//   /tmp/vec listed|whole <random streams> [continuation 0|1] ram_<tick>.bin ...

#include "runtime/ps2_dma_chain.h"
#include "runtime/ps2_async_vif.h"

// draw_list.cpp's runtime hooks (ps2_memory.cpp holds the lookup's), the observer stubbed
namespace
{
    ps2x::asyncvif::ListedObserver g_observer = nullptr;
}
void ps2x::asyncvif::setListedObserver(ListedObserver observer) { g_observer = observer; }
bool ps2x::asyncvif::waitVuIdle(std::chrono::microseconds) { return true; } // (no VU worker here)

#include "kamite/native/draw_list.cpp"

#define XXH_INLINE_ALL
#include "../../thirdparty/pcsx2/3rdparty/include/xxhash.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    std::vector<uint8_t> g_rdram(PS2_RDRAM_SIZE), g_spr(PS2_SCRATCHPAD_SIZE);
    uint32_t translate(uint32_t a)
    {
        if (ps2IsScratchpadAddress(a))
            return ps2ScratchpadOffset(a);
        if (a >= 0x20000000u && a < 0x40000000u)
            return a & (PS2_RDRAM_SIZE - 1u);
        if (a >= 0x80000000u && a < 0xC0000000u)
            return a & 0x1FFFFFFFu;
        if (a < 0x80000000u)
            return a;
        throw std::runtime_error("unmapped");
    }
    uint64_t tagAt(uint32_t a)
    {
        uint64_t t;
        std::memcpy(&t, g_rdram.data() + (a & (PS2_RDRAM_SIZE - 1u)), 8);
        return t;
    }

    // The packets a fused native would have queued: per NEXT target, its first depth-0 NEXT at +0xA0 / +0x70.
    std::map<uint32_t, uint32_t> candidates(uint32_t head)
    {
        struct T
        {
            uint32_t addr, id, depth;
        };
        std::vector<T> tags;
        uint32_t tag = head, asp = 0, asr[2] = {0, 0};
        for (int n = 0; n < (1 << 20); ++n)
        {
            const uint64_t q = tagAt(tag);
            const uint32_t qwc = q & 0xFFFF, id = (q >> 28) & 7, addr = (q >> 32) & 0x7FFFFFFF;
            tags.push_back(T{tag, id, asp});
            if (id == 0 || id == 7)
                break;
            if (id == 1)
                tag = tag + 16 + qwc * 16;
            else if (id == 2)
                tag = addr;
            else if (id == 3 || id == 4)
                tag += 16;
            else if (id == 5)
            {
                if (asp < 2)
                    asr[asp++] = tag + 16 + qwc * 16;
                tag = addr;
            }
            else if (id == 6)
            {
                if (!asp)
                    break;
                tag = asr[--asp];
            }
        }
        std::map<uint32_t, uint32_t> out;
        for (size_t i = 0; i + 1 < tags.size(); ++i)
        {
            if (tags[i].id != 2)
                continue;
            const uint32_t t = tags[i + 1].addr;
            for (size_t j = i + 2; j < tags.size(); ++j) // past the head (an upload record's head is a NEXT itself)
                if (tags[j].id == 2 && tags[j].depth == tags[i + 1].depth)
                {
                    if (tags[j].addr == t + 0xA0u || tags[j].addr == t + 0x70u || tags[j].addr == t + 0x80u)
                        out[t] = tags[j].addr;
                    break;
                }
        }
        return out;
    }
}


#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <random>

// What ps2_memory.cpp / the interpreter reach outside themselves: nothing of it runs here (no GS, no arbiter, no VU
// worker); GIF packets go to the callback.
std::atomic<uint32_t> g_gsDownloadSerial{0};
thread_local int g_gsTracePath = 0;
uint32_t g_gsTraceSrc = 0;
static bool g_continuation = true;
bool ps2xGsGifContinuationEnabled() { return g_continuation; }
void ps2xGsReleaseStall() {}
void ps2xWakeEeForGsInterrupt() noexcept {}
void ps2xEeFlushDeferredCycles() noexcept {}
void ps2xEeClearLatchedDmac(uint32_t) {}
void ps2xNoteVu1Upload(uint32_t, const uint8_t *, uint32_t) {}
namespace ps2x::vu1hooks { void runUploadHook(uint8_t *, uint32_t, uint32_t, uint32_t) {} }
namespace ps2x::asyncvif
{
    bool active(PS2Memory &) { return false; }
    void enqueue(PS2Memory &, std::vector<Item> &&) {}
    bool onAsyncThread() { return false; }
    bool postGifStallRelease() { return false; }
    void stop(PS2Memory &) {}
    bool submitGif(const uint8_t *, uint32_t) { return false; }
    void sync(PS2Memory &) {}
    ps2x::ByteBuffer takeBuffer() { return {}; }
    void traceEvent(char, uint32_t) noexcept {}
    bool vu1Busy(PS2Memory &) { return false; }
    void vu1Kick() {}
}
uint32_t GS::consumeLocalToHostBytes(uint8_t *, uint32_t) { return 0; }
bool GS::processNativePackedGIFPacket(const uint8_t *, uint32_t) { return false; }
void GS::uploadImageNative(uint64_t, uint64_t, uint64_t, uint64_t, const uint8_t *, uint32_t) {}
void GifArbiter::drain() {}
void GifArbiter::submit(GifPathId, const uint8_t *, uint32_t, bool) {}
extern "C"
{
    void ps2_ipu_init(void) {}
    uint32_t ps2_ipu_read32(uint32_t) { return 0; }
    void ps2_ipu_write32(uint32_t, uint32_t) {}
    void ps2_ipu_dma_in(const void *, uint32_t) {}
    void ps2_ipu_dma_out(void *, uint32_t) {}
    uint32_t ps2_ipu_in_space(void) { return 0; }
    uint32_t ps2_ipu_out_avail(void) { return 0; }
    uint32_t ps2_ipu_want(void) { return 0; }
}

namespace
{
    struct Run
    {
        PS2Memory mem;
        XXH3_state_t log;
        uint64_t gifs = 0, calls = 0;
        Run()
        {
            mem.initialize();
            XXH3_64bits_reset(&log);
            mem.setGifPacketCallback([this](const uint8_t *d, uint32_t n)
                                     { ++gifs, XXH3_64bits_update(&log, &n, 4), XXH3_64bits_update(&log, d, n); });
            mem.setVu1MscalCallback([this](uint32_t pc, uint32_t top, uint32_t itop)
                                    { uint32_t w[4] = {1, pc, top, itop}; ++calls, XXH3_64bits_update(&log, w, 16); });
            mem.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                    { uint32_t w[3] = {2, top, itop}; ++calls, XXH3_64bits_update(&log, w, 12); });
        }
        uint64_t digest()
        {
            XXH3_64bits_update(&log, mem.getVU1Code(), PS2_VU1_CODE_SIZE);
            XXH3_64bits_update(&log, mem.getVU1Data(), PS2_VU1_DATA_SIZE);
            XXH3_64bits_update(&log, &mem.vif1_regs, sizeof(mem.vif1_regs));
            return XXH3_64bits_digest(&log);
        }
    };

    // Runs a chain: whole = processVIF1Data on every byte in order; listed = processVIF1Listed(data, live).
    uint64_t runChain(bool whole, const uint8_t *data, uint32_t size, const ps2x::listed::Segments &live, uint64_t &gifs,
                      uint64_t &calls)
    {
        // one PS2Memory for every input (initializing one costs ~60 ms): each input starts from the previous one's state,
        // the same sequence in every run, so the digests still compare
        static Run r;
        XXH3_64bits_reset(&r.log);
        r.gifs = r.calls = 0;
        if (whole)
        {
            std::vector<uint8_t> all;
            uint32_t pos = 0;
            for (const auto &s : live.segments)
            {
                if (!s.live)
                    continue;
                all.insert(all.end(), data + pos, data + s.start);
                pos = s.start;
                for (uint32_t i = s.pieceBegin; i < s.pieceEnd; ++i)
                    all.insert(all.end(), live.pieces[i].src, live.pieces[i].src + live.pieces[i].len);
            }
            all.insert(all.end(), data + pos, data + size);
            r.mem.processVIF1Data(all.data(), static_cast<uint32_t>(all.size()));
        }
        else
            r.mem.processVIF1Listed(data, size, live);
        gifs = r.gifs, calls = r.calls;
        return r.digest();
    }

    // A random VIF1 command stream as a chain: plain bytes and listed packets whose pieces cut it at VIFcode | payload
    // edges (as the DMA chains do) and sometimes anywhere (the executor's gather).
    struct RandomChain
    {
        std::vector<uint8_t> plain;               // the gathered chain (non-listed bytes)
        std::vector<std::vector<uint8_t>> blobs;  // the listed pieces' bytes
        ps2x::listed::Segments live;
    };
    void randomCommand(std::mt19937_64 &g, std::vector<uint8_t> &out, std::vector<uint32_t> &payloadAt)
    {
        auto put32 = [&](uint32_t v) { out.insert(out.end(), reinterpret_cast<uint8_t *>(&v), reinterpret_cast<uint8_t *>(&v) + 4); };
        auto bytes = [&](uint32_t n) { for (uint32_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(g())); };
        const uint32_t irq = g() % 16 == 0 ? 0x80000000u : 0u;
        const uint32_t pick = g() % 20;
        if (pick < 6) // UNPACK: any vn / vl, masked sometimes, small num
        {
            const uint32_t vn = g() % 4, vl = g() % 4, m = g() % 6 == 0 ? 0x10 : 0;
            const uint32_t num = g() % 8 == 0 ? 0 : 1 + g() % 24;
            const uint32_t imm = (g() % 0x400) | (g() % 3 == 0 ? 0x8000 : 0) | (g() % 3 == 0 ? 0x4000 : 0);
            const uint32_t cmd = irq | ((0x60 | m | (vn << 2) | vl) << 24) | (num << 16) | imm;
            put32(cmd);
            payloadAt.push_back(static_cast<uint32_t>(out.size()));
            // the payload's size as the interpreter takes it (STCYCL kept to cl >= wl here: see below)
            const int bpc = vl == 0 ? 32 : vl == 1 ? 16 : vl == 2 ? 8 : (vn == 3 ? 4 : 16);
            const int bpv = (vl == 3 && vn == 3) ? 16 : (int(vn) + 1) * bpc;
            const uint32_t n = num ? num : 256;
            bytes(((n * ((bpv + 7) / 8)) + 3) & ~3u);
        }
        else if (pick < 8) // DIRECT / DIRECTHL: GIF tags and data
        {
            const uint32_t qw = 1 + g() % 6;
            put32(irq | ((g() % 2 ? 0x50u : 0x51u) << 24) | qw);
            payloadAt.push_back(static_cast<uint32_t>(out.size()));
            for (uint32_t q = 0; q < qw; ++q)
            {
                uint64_t lo = g(), hi = g();
                if (q == 0) // a plausible GIFtag: small nloop, EOP, a format
                    lo = (lo & ~0x7FFFull & ~(3ull << 58)) | (g() % 4) | (1ull << 15) | ((g() % 3ull) << 58);
                out.insert(out.end(), reinterpret_cast<uint8_t *>(&lo), reinterpret_cast<uint8_t *>(&lo) + 8);
                out.insert(out.end(), reinterpret_cast<uint8_t *>(&hi), reinterpret_cast<uint8_t *>(&hi) + 8);
            }
        }
        else if (pick == 8) // MPG
        {
            const uint32_t num = 1 + g() % 8;
            put32(irq | (0x4Au << 24) | (num << 16) | (g() % 0x7F8));
            payloadAt.push_back(static_cast<uint32_t>(out.size()));
            bytes(num * 8);
        }
        else if (pick == 9) // STCYCL (cl >= wl, and sometimes cl < wl: the source count shrinks)
        {
            const uint32_t wl = 1 + g() % 4, cl = g() % 5 == 0 ? (wl > 1 ? wl - 1 : 1) : wl + g() % 3;
            put32(irq | (0x01u << 24) | (wl << 8) | cl);
        }
        else if (pick == 10) { put32(irq | (0x20u << 24)); payloadAt.push_back(static_cast<uint32_t>(out.size())); bytes(4); }
        else if (pick == 11) { put32(irq | ((g() % 2 ? 0x30u : 0x31u) << 24)); payloadAt.push_back(static_cast<uint32_t>(out.size())); bytes(16); }
        else if (pick == 12) put32(irq | (0x14u << 24) | (g() % 0x100)); // MSCAL
        else if (pick == 13) put32(irq | (0x17u << 24));                // MSCNT
        else if (pick == 14) put32(irq | (0x02u << 24) | (g() % 0x400)); // OFFSET
        else if (pick == 15) put32(irq | (0x03u << 24) | (g() % 0x400)); // BASE
        else if (pick == 16) put32(irq | (0x05u << 24) | (g() % 4));     // STMOD
        else if (pick == 17) put32(irq | (0x06u << 24) | (g() % 2 ? 0x8000 : 0)); // MSKPATH3
        else if (pick == 18) put32(irq | (0x04u << 24) | (g() % 0x400)); // ITOP
        else put32(static_cast<uint32_t>(g()) & 0x80FFFFFFu);            // NOP / anything unknown
    }
    RandomChain randomChain(uint64_t seed)
    {
        std::mt19937_64 g(seed);
        RandomChain c;
        const int packets = 1 + g() % 6;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> listed; // (offset in plain, bytes + edges)
        std::vector<std::vector<uint32_t>> edges;
        for (int p = 0; p < packets; ++p)
        {
            std::vector<uint32_t> pa;
            const int pre = g() % 4;
            for (int k = 0; k < pre; ++k)
                randomCommand(g, c.plain, pa);
            std::vector<uint8_t> pk;
            std::vector<uint32_t> payloads;
            const int cmds = 1 + g() % 12;
            for (int k = 0; k < cmds; ++k)
                randomCommand(g, pk, payloads);
            if (g() % 10 == 0 && pk.size() > 4) // a truncated packet end (the stream drops what doesn't fit)
                pk.resize(pk.size() - 1 - g() % 3);
            std::vector<uint32_t> cut;
            for (uint32_t at : payloads)
                if (g() % 2 && at < pk.size())
                    cut.push_back(at); // the chains' shape: VIFcode | payload
            if (g() % 6 == 0)
                cut.push_back(1 + g() % static_cast<uint32_t>(pk.size())); // anywhere: the executor gathers
            if (g() % 8 == 0)
                cut.push_back(static_cast<uint32_t>(pk.size()) / 2); // a long payload split in two
            std::sort(cut.begin(), cut.end());
            cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
            listed.emplace_back(static_cast<uint32_t>(c.plain.size()), pk);
            edges.push_back(cut);
        }
        std::vector<uint32_t> pa;
        for (int k = 0; k < static_cast<int>(g() % 3); ++k)
            randomCommand(g, c.plain, pa);
        c.blobs.reserve(64);
        for (size_t p = 0; p < listed.size(); ++p)
        {
            ps2x::listed::Segment s{};
            s.start = s.end = listed[p].first;
            s.live = true, s.closed = true;
            s.pieceBegin = static_cast<uint32_t>(c.live.pieces.size());
            uint32_t from = 0;
            edges[p].push_back(static_cast<uint32_t>(listed[p].second.size()));
            for (uint32_t to : edges[p])
            {
                if (to <= from)
                    continue;
                c.blobs.emplace_back(listed[p].second.begin() + from, listed[p].second.begin() + to);
                c.live.pieces.push_back(ps2x::listed::Piece{c.blobs.back().data(), to - from});
                from = to;
            }
            s.pieceEnd = static_cast<uint32_t>(c.live.pieces.size());
            c.live.segments.push_back(s);
        }
        c.live.live = true;
        return c;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const bool whole = std::string(argv[1]) == "whole";
    const int randoms = std::atoi(argv[2]);
    int a = 3;
    if (argc > 3 && (std::string(argv[3]) == "0" || std::string(argv[3]) == "1"))
        g_continuation = argv[3][0] == '1', ++a;
    XXH3_state_t all;
    XXH3_64bits_reset(&all);
    for (int k = 0; k < randoms; ++k)
    {
        const RandomChain c = randomChain(0x9E3779B97F4A7C15ull * (k + 1));
        uint64_t gifs, calls;
        const uint64_t d = runChain(whole, c.plain.data(), static_cast<uint32_t>(c.plain.size()), c.live, gifs, calls);
        XXH3_64bits_update(&all, &d, 8);
        if (k < 4 || k % 10000 == 0)
            std::printf("random %d: %016llx (gif %llu, calls %llu)\n", k, static_cast<unsigned long long>(d),
                        static_cast<unsigned long long>(gifs), static_cast<unsigned long long>(calls));
    }
    std::printf("random streams %d: all %016llx (executor gathers %llu)\n", randoms,
                static_cast<unsigned long long>(XXH3_64bits_digest(&all)),
                static_cast<unsigned long long>(ps2x::listed::executorGathers.load()));
    for (; a < argc; ++a)
    {
        std::FILE *f = std::fopen(argv[a], "rb");
        if (!f || std::fread(g_rdram.data(), 1, 32u << 20, f) != (32u << 20))
            return 2;
        std::fclose(f);
        uint32_t half;
        std::memcpy(&half, g_rdram.data() + 0x7476B0, 4);
        const uint32_t fin = 1u - (half & 1u);
        uint32_t head;
        std::memcpy(&head, g_rdram.data() + 0x754C80 + fin * 128, 4);
        kamite::native::draw_list::begin(g_rdram.data());
        const std::map<uint32_t, uint32_t> cands = candidates(head);
        std::vector<uint32_t> heads, tails;
        std::vector<uint64_t> headTags;
        for (const auto &[h, t] : cands)
            heads.push_back(h), tails.push_back(t), headTags.push_back(tagAt(h));
        kamite::native::draw_list::listPackets(fin, heads.data(), tails.data(), headTags.data(),
                                               static_cast<uint32_t>(heads.size()));
        std::vector<ps2x::dma::ChainPiece> p2, lp2;
        std::vector<ps2x::listed::Mark> m2;
        ps2x::dma::ChainWalk w{};
        w.tagAddr = head;
        size_t b2 = 0;
        ps2x::dma::walkChain(w, false, true, g_rdram.data(), g_spr.data(), translate, p2, b2, ps2x::listed::lookup(), &m2, &lp2);
        ps2x::ByteBuffer g2;
        ps2x::dma::gatherChain(p2, b2, g2);
        ps2x::listed::Segments live;
        ps2x::listed::takeLive(p2, lp2, m2, live);
        uint64_t gifs, calls;
        const uint64_t before = ps2x::listed::executorGathers.load();
        const uint64_t d = runChain(whole, g2.data(), static_cast<uint32_t>(b2), live, gifs, calls);
        std::printf("%s: %016llx (packets %zu live, gif %llu, calls %llu, executor gathers %llu)\n", argv[a],
                    static_cast<unsigned long long>(d), live.segments.size(), static_cast<unsigned long long>(gifs),
                    static_cast<unsigned long long>(calls),
                    static_cast<unsigned long long>(ps2x::listed::executorGathers.load() - before));
    }
    return 0;
}
