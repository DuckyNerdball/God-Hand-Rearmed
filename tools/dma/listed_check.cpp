// Rung 3 step b's shadow, offline (claude/native-next.md "Step b"): the draw list's registry, the walker's marks
// (runtime/ps2_dma_chain.h with a lookup), cutSegments and the VU thread's check (src/kamite/native/draw_list.cpp,
// #included as is) on K's F8 RAM captures. Without the game running, the packets the fused native would register are
// found from the chain itself: a NEXT target T whose first NEXT tag outside a CALL is at T + 0xA0 (a node's packet) or
// T + 0x70 (a model's own packet) or T + 0x80 (a texture upload's record, b2; its head is a NEXT itself, so the search
// starts past the head); it finds scenery packets too, and ones nested in others (never asked about). Checks:
// the walk with the lookup is byte-identical to the walk without one, every mark closes at its tail, the cut pieces are
// the gathered bytes; draw_list.cpp's report (VIF commands on the packets' edges, bytes in RAM = the copy) is printed.
// Nov 23 on 649722 / 650290 / 15923: 646 / 642 / 173 marks, all closed and cut right, parse ok on every one.
// Build 2 (live): the walk with the listed bytes routed out has the same walk state; the copied chain with each packet
// put back, logicalPieces, and chain_digest's streaming hash all equal the full chain. (The report printed below holds
// both the shadow and the live pass's counts.)
// Build 3 (the cached decode): a cold walk that records and a warm one that jumps equal the uncached live walk; a
// changed REF tag inside a remembered packet misses once, re-records, and equals a fresh walk; restoring it misses once.
// LISTED_BENCH=1 times the walks (Nov 24, sandbox, best of 9 x 300): 649722 plain 406 us, live uncached 429, cached 366;
// 650290 308 / 353 / 313; 15923 77 / 81 / 58 (69-72% of the tags sit in listed packets; checking them reads the same
// memory as walking them, so the cache mostly wins back the lookups).
// b2 (upload records too): 649722 copies 2.85 MB + 4.72 MB in place; 650290 2.00 + 4.72; 15923 0.46 + 1.61; parse ok on
// every packet.
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
    bool g_liveMode = false;
    ps2x::asyncvif::ListedObserver g_observer = nullptr;
}
void ps2x::listed::setLookup(Lookup lookup, bool live) { g_lookup = lookup, g_liveMode = live; }
ps2x::listed::Lookup ps2x::listed::lookup() { return g_lookup; }
bool ps2x::listed::live() { return g_liveMode; }
void ps2x::asyncvif::setListedObserver(ListedObserver observer) { g_observer = observer; }
namespace
{
    ps2x::listed::Cache g_cache;
}
void ps2x::listed::setCache(bool) {}
void ps2x::listed::cacheCounts(uint64_t &hits, uint64_t &misses) { hits = g_cache.hits, misses = g_cache.misses; }

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
        kamite::native::draw_list::begin(g_rdram.data());
        kamite::native::draw_list::report(out.c_str(), "offline");
        const std::map<uint32_t, uint32_t> cands = candidates(head);
        std::vector<uint32_t> heads, tails;
        std::vector<uint64_t> headTags;
        for (const auto &[h, t] : cands)
            heads.push_back(h), tails.push_back(t), headTags.push_back(tagAt(h));
        kamite::native::draw_list::listPackets(fin, heads.data(), tails.data(), headTags.data(),
                                               static_cast<uint32_t>(heads.size()));

        auto walk = [&](bool withLookup, std::vector<ps2x::dma::ChainPiece> &pieces, std::vector<ps2x::listed::Mark> &marks,
                        ps2x::dma::ChainWalk &w, size_t &bytes, std::vector<ps2x::dma::ChainPiece> *listedPieces = nullptr)
        {
            w = ps2x::dma::ChainWalk{};
            w.tagAddr = head;
            return ps2x::dma::walkChain(w, false, true, g_rdram.data(), g_spr.data(), translate, pieces, bytes,
                                        withLookup ? g_lookup : nullptr, withLookup ? &marks : nullptr, listedPieces);
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

        // Live (build 2): the same walk with the listed bytes routed out. The walk's state must match, the gathered chain
        // with each packet's bytes put back must be the full chain, logicalPieces must give it too, and chain_digest's
        // streaming hash of it must be the one-shot hash of the full chain (chain.bin unchanged).
        std::vector<ps2x::dma::ChainPiece> p2, lp2, whole;
        std::vector<ps2x::listed::Mark> m2;
        ps2x::dma::ChainWalk w2;
        size_t b2 = 0;
        const bool k2 = walk(true, p2, m2, w2, b2, &lp2);
        ps2x::ByteBuffer g2;
        ps2x::dma::gatherChain(p2, b2, g2);
        ps2x::listed::Segments live;
        ps2x::listed::takeLive(lp2, m2, live);
        std::vector<uint8_t> rebuilt;
        XXH3_state_t state;
        XXH3_64bits_reset(&state);
        size_t pos = 0;
        for (const ps2x::listed::Segment &sg : live.segments)
        {
            rebuilt.insert(rebuilt.end(), g2.data() + pos, g2.data() + sg.start);
            XXH3_64bits_update(&state, g2.data() + pos, sg.start - pos);
            pos = sg.start;
            for (uint32_t i = sg.pieceBegin; i < sg.pieceEnd; ++i)
            {
                rebuilt.insert(rebuilt.end(), live.pieces[i].src, live.pieces[i].src + live.pieces[i].len);
                XXH3_64bits_update(&state, live.pieces[i].src, live.pieces[i].len);
            }
        }
        rebuilt.insert(rebuilt.end(), g2.data() + pos, g2.data() + b2);
        XXH3_64bits_update(&state, g2.data() + pos, b2 - pos);
        ps2x::listed::logicalPieces(p2, live, whole);
        ps2x::ByteBuffer gw;
        size_t bw = 0;
        for (const auto &pc : whole)
            bw += pc.len;
        ps2x::dma::gatherChain(whole, bw, gw);
        const bool liveSame = k2 == k0 && w2.tagAddr == w0.tagAddr && w2.asp == w0.asp && w2.tagsProcessed == w0.tagsProcessed &&
                              w2.lastTagUpper == w0.lastTagUpper && m2.size() == m1.size() &&
                              b2 + live.listedBytes() == b0 && rebuilt.size() == b0 &&
                              std::memcmp(rebuilt.data(), g0.data(), b0) == 0 && bw == b0 &&
                              std::memcmp(gw.data(), g0.data(), b0) == 0 &&
                              XXH3_64bits_digest(&state) == XXH3_64bits(g0.data(), b0);
        if (g_observer)
            g_observer(live, g2.data(), b2, 0);

        // The cached decode: a cold walk records, a warm one jumps; both must give the uncached live walk's pieces,
        // marks and walk state. Then a REF tag inside a remembered packet points elsewhere: the warm walk must notice
        // and give what a fresh uncached walk of the changed RAM gives.
        auto cacheSame = [&](const char *what, bool withCache)
        {
            std::vector<ps2x::dma::ChainPiece> pa, la, pb, lb;
            std::vector<ps2x::listed::Mark> ma, mb;
            ps2x::dma::ChainWalk wa, wb;
            size_t ba = 0, bb = 0;
            wa.tagAddr = wb.tagAddr = head;
            const bool ka = ps2x::dma::walkChain(wa, false, true, g_rdram.data(), g_spr.data(), translate, pa, ba, g_lookup,
                                                 &ma, &la, nullptr);
            const bool kb = ps2x::dma::walkChain(wb, false, true, g_rdram.data(), g_spr.data(), translate, pb, bb, g_lookup,
                                                 &mb, &lb, withCache ? &g_cache : nullptr);
            auto eqp = [](const std::vector<ps2x::dma::ChainPiece> &x, const std::vector<ps2x::dma::ChainPiece> &y)
            {
                if (x.size() != y.size())
                    return false;
                for (size_t i = 0; i < x.size(); ++i)
                    if (x[i].src != y[i].src || x[i].len != y[i].len)
                        return false;
                return true;
            };
            bool ok = ka == kb && ba == bb && eqp(pa, pb) && eqp(la, lb) && ma.size() == mb.size() &&
                      wa.tagAddr == wb.tagAddr && wa.asp == wb.asp && wa.asr0 == wb.asr0 && wa.asr1 == wb.asr1 &&
                      wa.lastTagUpper == wb.lastTagUpper && wa.tagsProcessed == wb.tagsProcessed;
            for (size_t i = 0; ok && i < ma.size(); ++i)
                ok = ma[i].head == mb[i].head && ma[i].start == mb[i].start && ma[i].end == mb[i].end &&
                     ma[i].closed == mb[i].closed && ma[i].pieceBegin == mb[i].pieceBegin && ma[i].pieceEnd == mb[i].pieceEnd;
            std::printf("    cache %-34s %s (hits %llu misses %llu)\n", what, ok ? "= uncached" : "DIFFERS",
                        static_cast<unsigned long long>(g_cache.hits), static_cast<unsigned long long>(g_cache.misses));
            return ok;
        };
        g_cache = {};
        failures += !cacheSame("cold (records)", true);
        failures += !cacheSame("warm (jumps)", true);
        // change a REF tag's address inside the first remembered packet that has one (and put it back after)
        uint32_t changedAt = 0u;
        uint64_t old = 0u;
        for (const auto &[key, e] : g_cache.entries)
        {
            for (size_t i = 1; i < e.tags.size(); ++i) // not the head: that one the registry checks itself
            {
                const auto &[off, value] = e.tags[i];
                if (((value >> 28) & 7u) == 3u && ((value >> 32) & 0x7FFFFFFFu) >= 0x100000u)
                {
                    changedAt = off, old = value;
                    break;
                }
            }
            if (changedAt)
                break;
        }
        if (std::getenv("LISTED_BENCH"))
        {
            // the game thread's walk of this chain, live: uncached vs the warm cache (and the plain walk for scale)
            auto time = [&](int mode)
            {
                std::vector<ps2x::dma::ChainPiece> pa, la;
                std::vector<ps2x::listed::Mark> ma;
                const auto t0 = std::chrono::steady_clock::now();
                for (int n = 0; n < 300; ++n)
                {
                    ps2x::dma::ChainWalk wa;
                    wa.tagAddr = head;
                    size_t ba = 0;
                    if (mode == 0)
                        ps2x::dma::walkChain(wa, false, true, g_rdram.data(), g_spr.data(), translate, pa, ba);
                    else
                        ps2x::dma::walkChain(wa, false, true, g_rdram.data(), g_spr.data(), translate, pa, ba, g_lookup, &ma,
                                             &la, mode == 2 ? &g_cache : nullptr);
                }
                return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 300.0;
            };
            size_t listedTags = 0;
            for (const auto &kv : g_cache.entries)
                listedTags += kv.second.tags.size();
            double best[3] = {1e30, 1e30, 1e30};
            for (int round = 0; round < 9; ++round)
                for (int m = 0; m < 3; ++m)
                    best[m] = std::min(best[m], time(m));
            std::printf("    walk (best of 9 x 300): plain %.0f us, live uncached %.0f us, live cached %.0f us (listed tags "
                        "%zu of %d)\n", best[0], best[1], best[2], listedTags, w0.tagsProcessed);
        }
        {
            // A ring (the game links its texture queue into one after a 60 -> 30 state load): a listed packet's tail
            // pointed back at its own head. The walk must end (dropped as looped), cold and with the cache warm.
            const auto &[ringHead, ringTail] = *cands.rbegin();
            uint64_t tailTag;
            std::memcpy(&tailTag, g_rdram.data() + ringTail, 8);
            const uint64_t ring = (tailTag & 0x80000000FFFFFFFFull) | (static_cast<uint64_t>(ringHead) << 32);
            std::memcpy(g_rdram.data() + ringTail, &ring, 8);
            for (int pass = 0; pass < 2; ++pass)
            {
                std::vector<ps2x::dma::ChainPiece> pr, lr;
                std::vector<ps2x::listed::Mark> mr;
                ps2x::dma::ChainWalk wr;
                wr.tagAddr = head;
                size_t br = 0;
                const bool kept = ps2x::dma::walkChain(wr, false, true, g_rdram.data(), g_spr.data(), translate, pr, br,
                                                       g_lookup, &mr, &lr, &g_cache);
                std::printf("    ring (%s): %s after %d tags\n", pass ? "warm" : "cold",
                            !kept && wr.looped ? "dropped as looped" : "NOT DROPPED", wr.tagsProcessed);
                failures += kept || !wr.looped;
            }
            std::memcpy(g_rdram.data() + ringTail, &tailTag, 8);
        }
        if (changedAt)
        {
            const uint64_t moved = old - (0x40ull << 32); // the payload read 0x40 bytes earlier
            std::memcpy(g_rdram.data() + changedAt, &moved, 8);
            failures += !cacheSame("after a tag changed", true);
            failures += !cacheSame("warm again", true);
            std::memcpy(g_rdram.data() + changedAt, &old, 8);
            failures += !cacheSame("tag restored", true);
        }
        else
            std::printf("    cache: no REF tag to change\n"), ++failures;
        std::printf("    live: chain copy %zu bytes + %zu listed in place = %zu; rebuilt / logical pieces / streaming hash %s\n",
                    b2, live.listedBytes(), b2 + live.listedBytes(), liveSame ? "= the full chain" : "DIFFER");
        failures += !liveSame;
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
        kamite::native::draw_list::g_on = false;
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
