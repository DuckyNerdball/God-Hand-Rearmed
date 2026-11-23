// Rung 3 step b's shadow, offline (claude/native-next.md "Step b"): the draw list's registry, the walker's marks
// (runtime/ps2_dma_chain.h with a lookup), cutSegments and the VU thread's check (src/kamite/native/draw_list.cpp,
// #included as is) on K's F8 RAM captures. Without the game running, the packets the fused native would register are
// found from the chain itself: a NEXT target T whose first NEXT tag outside a CALL is at T + 0xA0 (a node's packet) or
// T + 0x70 (a model's own packet); it finds scenery packets too, and ones nested in others (never asked about). Checks:
// the walk with the lookup is byte-identical to the walk without one, every mark closes at its tail, the cut pieces are
// the gathered bytes; draw_list.cpp's report (VIF commands on the packets' edges, bytes in RAM = the copy) is printed.
// Nov 23 on 649722 / 650290 / 15923: 646 / 642 / 173 marks, all closed and cut right, parse ok on every one.
//
//   g++ -std=c++20 -O2 -msse4.1 -I runtime/ps2xRuntime/include -I runtime/ps2xIOP/include -I src
//       tools/dma/listed_check.cpp -o /tmp/listed_check
//   /tmp/listed_check ram_<tick>.bin ...      (prints each capture's drawlist report)

#include "runtime/ps2_dma_chain.h"
#include "runtime/ps2_async_vif.h"

// draw_list.cpp's runtime hooks, stubbed: the test calls what they would have registered
namespace
{
    ps2x::listed::Lookup g_lookup = nullptr;
    ps2x::asyncvif::ListedObserver g_observer = nullptr;
}
void ps2x::listed::setLookup(Lookup lookup) { g_lookup = lookup; }
ps2x::listed::Lookup ps2x::listed::lookup() { return g_lookup; }
void ps2x::asyncvif::setListedObserver(ListedObserver observer) { g_observer = observer; }

#include "kamite/native/draw_list.cpp"

#include <cstdio>
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
            for (size_t j = i + 1; j < tags.size(); ++j)
                if (tags[j].id == 2 && tags[j].depth == tags[i + 1].depth)
                {
                    if (tags[j].addr == t + 0xA0u || tags[j].addr == t + 0x70u)
                        out[t] = tags[j].addr;
                    break;
                }
        }
        return out;
    }
}

int main(int argc, char **argv)
{
    int failures = 0;
    for (int a = 1; a < argc; ++a)
    {
        std::FILE *f = std::fopen(argv[a], "rb");
        if (!f || std::fread(g_rdram.data(), 1, 32u << 20, f) != (32u << 20))
        {
            std::fprintf(stderr, "%s: cannot read 32 MB\n", argv[a]);
            return 2;
        }
        std::fclose(f);
        uint32_t half;
        std::memcpy(&half, g_rdram.data() + 0x7476B0, 4);
        const uint32_t fin = 1u - (half & 1u);
        uint32_t head;
        std::memcpy(&head, g_rdram.data() + 0x754C80 + fin * 128, 4);

        const std::string out = std::string("/tmp/listed_check_") + std::to_string(a);
        kamite::native::draw_list::begin(out.c_str(), "offline", g_rdram.data());
        const std::map<uint32_t, uint32_t> cands = candidates(head);
        std::vector<uint32_t> heads, tails;
        std::vector<uint64_t> headTags;
        for (const auto &[h, t] : cands)
            heads.push_back(h), tails.push_back(t), headTags.push_back(tagAt(h));
        kamite::native::draw_list::listPackets(fin, heads.data(), tails.data(), headTags.data(),
                                               static_cast<uint32_t>(heads.size()));

        auto walk = [&](bool withLookup, std::vector<ps2x::dma::ChainPiece> &pieces, std::vector<ps2x::listed::Mark> &marks,
                        ps2x::dma::ChainWalk &w, size_t &bytes)
        {
            w = ps2x::dma::ChainWalk{};
            w.tagAddr = head;
            return ps2x::dma::walkChain(w, false, true, g_rdram.data(), g_spr.data(), translate, pieces, bytes,
                                        withLookup ? g_lookup : nullptr, withLookup ? &marks : nullptr);
        };
        std::vector<ps2x::dma::ChainPiece> p0, p1;
        std::vector<ps2x::listed::Mark> m0, m1;
        ps2x::dma::ChainWalk w0, w1;
        size_t b0 = 0, b1 = 0;
        const bool k0 = walk(false, p0, m0, w0, b0), k1 = walk(true, p1, m1, w1, b1);
        ps2x::ByteBuffer g0, g1;
        ps2x::dma::gatherChain(p0, b0, g0);
        ps2x::dma::gatherChain(p1, b1, g1);
        const bool same = k0 == k1 && b0 == b1 && std::memcmp(g0.data(), g1.data(), b0) == 0 && w0.tagAddr == w1.tagAddr &&
                          w0.asp == w1.asp && w0.asr0 == w1.asr0 && w0.asr1 == w1.asr1 &&
                          w0.lastTagUpper == w1.lastTagUpper && w0.tagsProcessed == w1.tagsProcessed;
        ps2x::listed::Segments segs;
        ps2x::listed::cutSegments(p1, m1, segs);
        size_t closed = 0, cutOk = 0;
        for (const ps2x::listed::Segment &s : segs.segments)
        {
            closed += s.closed;
            uint32_t off = s.start;
            bool ok = s.closed;
            for (uint32_t i = s.pieceBegin; i < s.pieceEnd && ok; ++i)
            {
                ok = std::memcmp(segs.pieces[i].src, g1.data() + off, segs.pieces[i].len) == 0;
                off += segs.pieces[i].len;
            }
            cutOk += ok && off == s.end;
        }
        if (g_observer)
        {
            g_observer(segs, g1.data(), b1, 0);
            g_observer(segs, g1.data(), b1, 1);
        }
        kamite::native::draw_list::end();
        // candidates inside another candidate's packet are never asked about (the walker asks only outside a listed
        // packet): the heuristic finds more than a native registers, so marks <= candidates
        std::printf("%s: chain %zu bytes, %d tags; candidates %zu (%zu inside another), marks %zu (closed %zu, cut = "
                    "gathered %zu); walk with lookup %s\n",
                    argv[a], b1, w1.tagsProcessed, cands.size(), cands.size() - m1.size(), m1.size(), closed, cutOk,
                    same ? "identical" : "DIFFERS");
        failures += !same || closed != m1.size() || cutOk != m1.size() || m1.empty();
        std::FILE *r = std::fopen((out + ".drawlist.txt").c_str(), "rb");
        char line[256];
        while (r && std::fgets(line, sizeof(line), r))
            std::printf("    %s", line);
        if (r)
            std::fclose(r);
        // a fresh registry and stats for the next capture
        kamite::native::draw_list::g_stats = {};
        kamite::native::draw_list::g_registered = kamite::native::draw_list::g_found = 0;
        kamite::native::draw_list::g_unseen = kamite::native::draw_list::g_tagChanged = 0;
        kamite::native::draw_list::g_lastHalf = -1;
        kamite::native::draw_list::g_genSlots[0].clear();
        kamite::native::draw_list::g_genSlots[1].clear();
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
