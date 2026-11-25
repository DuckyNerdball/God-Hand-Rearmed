#include "kamite/native/draw_list.h"

#include "runtime/ps2_listed_packets.h"
#include "runtime/ps2_async_vif.h"
#if !defined(KAMITE_RELEASE)
#define XXH_INLINE_ALL
#include "../../../thirdparty/pcsx2/3rdparty/include/xxhash.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kamite::native::draw_list
{
    bool g_on = false;
    uint32_t g_queueCaller = 0u;
    uint64_t g_uploadsUnlisted = 0u;

    namespace
    {
        bool g_live = false;
        const uint8_t *g_ram = nullptr; // the RAM backing (the lookup's tail check)
        // ---- the registry (game thread only: the fused natives register, the walker looks up) ----------------------
        // Open addressing by head address (RAM offset). An entry is valid while its generation is its half's current one:
        // a half starts a new generation the first time a native registers for it after registering for the other half
        // (frame n builds half h; frame n + 2 builds h again, after n's chain was kicked).
        struct Entry
        {
            uint32_t key = 0u; // head & RAM mask, | 1 so that 0 stays "empty"
            uint32_t tailDelta = 0u;
            uint64_t headTag = 0u;
            uint32_t gen = 0u;
            uint8_t half = 0u;
            bool seen = false;
            bool shadow = false; // a source not proven yet: copied and checked, not read in place
            uint8_t why = 0u;    // shadow: why it is copied (Why; the dev report's copied-bytes map)
            uint32_t caller = 0u; // Source::Queue: g_queueCaller at registration (dev report)
        };
        // Why a registered packet is copied (dev report): the queue rule's first frames, a buffer queued in both halves /
        // twice a frame, a both-halves writer's mark (b3 B), a source still shadowed
        enum Why : uint8_t
        {
            kWhyLive = 0,
            kWhyFirst,
            kWhyReused,
            kWhyMarked,
            kWhyShadow,
        };
        constexpr uint32_t kSlots = 1u << 14;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        std::vector<Entry> g_table;
        uint32_t g_gen[2] = {1u, 1u};
        int g_lastHalf = -1;
        uint32_t g_used = 0u;
        std::vector<uint32_t> g_genSlots[2]; // the slots each half registered in its current generation
        // One bit per RAM qword: "a head was registered here" (set on registering, cleared only when the table is
        // rebuilt): most NEXT targets the walker asks about are not heads, and a bit test is cheaper than the probe.
        std::vector<uint64_t> g_headBits;
        void setHeadBit(uint32_t key) { g_headBits[(key & kRamMask) >> 10] |= 1ull << (((key & kRamMask) >> 4) & 63u); }
        bool headBit(uint32_t off) { return (g_headBits[off >> 10] >> ((off >> 4) & 63u)) & 1u; }
        // b3 B: one bit per RAM qword, "a node packet a both-halves writer changed" (copied for good; never cleared)
        std::vector<uint64_t> g_rewrittenBits;
        uint64_t g_rewriteMarks = 0u, g_rewriteWaits = 0u, g_rewriteTimeouts = 0u;
        bool rewrittenBit(uint32_t key) { return (g_rewrittenBits[(key & kRamMask) >> 10] >> (((key & kRamMask) >> 4) & 63u)) & 1u; }

        // EE-side counts (game thread)
        uint64_t g_registered = 0u, g_halves = 0u, g_found = 0u, g_unseen = 0u, g_tagChanged = 0u, g_full = 0u;
        uint64_t g_registeredShadow = 0u; // registered to be copied (a shadow source, or a queue packet not in place yet)
        uint64_t g_queueLive = 0u, g_queueReused = 0u, g_queueNew = 0u; // queue registrations by their reuse verdict

        // Source::Queue's reuse record per head (game thread; sticky: a head seen in both halves stays copied).
        struct Reuse
        {
            uint32_t lastGen = 0u;
            uint32_t caller = 0u; // the queue native's guest caller, last time (dev report)
            uint8_t lastHalf = 0u;
            uint8_t halves = 0u; // bit per frame half the head was queued in
            uint8_t frames = 0u; // generations it was queued in (saturates)
            bool twice = false;  // queued twice in one generation (one frame)
        };
        std::unordered_map<uint32_t, Reuse> g_reuse;
        uint64_t g_reuseClears = 0u; // (dev report: the record's resets, each one re-copies every queue buffer once)
        // true: read in place (a double-buffered head, seen in two frames of this half only). sticky: copied for good.
        bool reuseOk(uint32_t key, uint32_t half, uint32_t gen, bool &sticky)
        {
            if (g_reuse.size() > (1u << 17))
            {
                g_reuse.clear(); // never in practice (heads are a few thousand buffers)
                ++g_reuseClears;
            }
            Reuse &r = g_reuse[key];
            r.halves |= static_cast<uint8_t>(1u << half);
            r.caller = g_queueCaller;
            if (r.frames != 0u && r.lastGen == gen && r.lastHalf == half)
                r.twice = true;
            else
            {
                r.lastGen = gen;
                r.lastHalf = static_cast<uint8_t>(half);
                if (r.frames < 255u)
                    ++r.frames;
            }
            sticky = r.halves == 3u || r.twice;
            if (sticky)
            {
                ++g_queueReused;
                return false;
            }
            if (r.frames < 2u)
            {
                ++g_queueNew;
                return false;
            }
            ++g_queueLive;
            return true;
        }

        uint32_t slotOf(uint32_t key) { return (key * 2654435761u) >> (32u - 14u); }
        bool current(const Entry &e) { return e.key != 0u && e.gen == g_gen[e.half]; }

        void newGeneration(uint32_t half)
        {
            ++g_halves;
            // the half's entries from two frames ago: count the ones the walker never met, then retire them
            for (const uint32_t s : g_genSlots[half])
            {
                const Entry &e = g_table[s];
                if (current(e) && e.half == half && !e.seen)
                    ++g_unseen;
            }
            g_genSlots[half].clear();
            ++g_gen[half];
            if (g_used > kSlots / 2u)
            {
                // rebuild with the live entries only (stale ones are tombstones until then)
                std::vector<Entry> live;
                for (const Entry &e : g_table)
                    if (current(e))
                        live.push_back(e);
                std::fill(g_table.begin(), g_table.end(), Entry{});
                std::fill(g_headBits.begin(), g_headBits.end(), 0ull);
                g_used = 0u;
                g_genSlots[0].clear();
                g_genSlots[1].clear();
                for (const Entry &e : live)
                {
                    uint32_t s = slotOf(e.key);
                    while (g_table[s].key != 0u)
                        s = (s + 1u) & (kSlots - 1u);
                    g_table[s] = e;
                    setHeadBit(e.key);
                    g_genSlots[e.half].push_back(s);
                    ++g_used;
                }
            }
        }

        bool lookup(uint32_t headAddr, uint64_t headTag, uint32_t &tailAddr, bool &shadow)
        {
            if ((headAddr & 0xF0000000u) == 0x70000000u) // the scratchpad: never a listed packet
                return false;
            if (!headBit(headAddr & kRamMask))
                return false;
            const uint32_t key = (headAddr & kRamMask) | 1u;
            for (uint32_t s = slotOf(key), n = 0; n < kSlots; s = (s + 1u) & (kSlots - 1u), ++n)
            {
                Entry &e = g_table[s];
                if (e.key == 0u)
                    return false;
                if (e.key != key || !current(e))
                    continue;
                if (e.headTag != headTag)
                {
                    ++g_tagChanged; // rewritten since the native registered it: walked as a plain packet
                    return false;
                }
                // the tail must still be the NEXT the queue's link wrote (an entry from before a save-state load whose
                // address now holds something else with the same first 8 bytes: never seen, but then it is not listed)
                const uint32_t tail = headAddr + e.tailDelta;
                const uint32_t tailOff = tail & kRamMask;
                uint64_t tailTag = 0u;
                if (g_ram && tailOff + 8u <= kRamMask + 1u)
                    std::memcpy(&tailTag, g_ram + tailOff, 8);
                if (((tailTag >> 28) & 7u) != 2u)
                {
                    ++g_tagChanged;
                    return false;
                }
                e.seen = true;
                ++g_found;
                tailAddr = tail;
                shadow = e.shadow;
                return true;
            }
            return false;
        }

    }

    void listPackets(uint32_t half, const uint32_t *heads, const uint32_t *tails, const uint64_t *headTags, uint32_t count,
                     Source source)
    {
        if (!g_on)
            return;
        half &= 1u;
        if (g_lastHalf != static_cast<int>(half))
        {
            g_lastHalf = static_cast<int>(half);
            newGeneration(half);
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t key = (heads[i] & kRamMask) | 1u;
            // a packet a both-halves writer changed (b3 B) is copied for good, whatever its source
            bool sticky = rewrittenBit(key);
            const bool marked = sticky;
            const bool shadow = sticky || source == Source::Shadow ||
                                (source == Source::Queue && !reuseOk(key, half, g_gen[half], sticky));
            const uint8_t why = !shadow ? kWhyLive : marked ? kWhyMarked : source == Source::Shadow ? kWhyShadow
                                : sticky ? kWhyReused : kWhyFirst;
            // the same packet's slot if it has one (its new entry replaces the old), else the first empty or retired one
            uint32_t found = kSlots, free = kSlots;
            for (uint32_t s = slotOf(key), n = 0; n < kSlots; s = (s + 1u) & (kSlots - 1u), ++n)
            {
                const Entry &e = g_table[s];
                if (e.key == key)
                {
                    found = s;
                    break;
                }
                if (e.key == 0u)
                {
                    if (free == kSlots)
                        free = s;
                    break;
                }
                if (free == kSlots && !current(e))
                    free = s;
            }
            const uint32_t at = found != kSlots ? found : free;
            if (at == kSlots)
            {
                ++g_full;
                continue;
            }
            Entry &e = g_table[at];
            if (shadow && !sticky && e.key == key && current(e) && e.half == half && !e.shadow)
                continue; // already listed live this generation: keep it (a reused buffer is not kept)
            if (e.key == 0u)
                ++g_used;
            if (!(e.key == key && current(e) && e.half == half)) // a second registration this generation: one slot
                g_genSlots[half].push_back(at);
            e.key = key;
            setHeadBit(key);
            e.tailDelta = tails[i] - heads[i];
            e.headTag = headTags[i];
            e.half = static_cast<uint8_t>(half);
            e.gen = g_gen[half];
            e.seen = false;
            e.shadow = shadow;
            e.why = why;
            e.caller = source == Source::Queue ? g_queueCaller : 0u;
            ++(shadow ? g_registeredShadow : g_registered);
        }
    }

#if !defined(KAMITE_RELEASE)
    namespace
    {
        // ---- the VU thread's check (dev replays) ------------------------------------------------------------------
        // Shadow: each packet's VIF edges, and its bytes in RAM against the walker's copy at job start and end. Live: its
        // bytes are only in RAM: the VIF edge check on them, and the counts.
        struct Diff
        {
            int phase;
            uint32_t head, at, len;
        };
        struct Stats
        {
            uint64_t items = 0u, chainBytes = 0u, segments = 0u, unclosed = 0u, bytes = 0u;
            uint64_t liveSegments = 0u, shadowSegments = 0u, liveSame = 0u, liveDiffer = 0u;
            std::vector<uint32_t> liveDiffs; // heads, the first few
            uint64_t parseOk = 0u, parseFail = 0u, unpackBeforeCycle = 0u, maskedUnpacks = 0u;
            uint64_t same[2] = {0u, 0u}, differ[2] = {0u, 0u};
            uint64_t marked = 0u; // shadow packets copied for a both-halves writer: not compared
            // the copied bytes' map (the gathered chain, at job start): registered packets by why they are copied, the
            // rest in gaps (DMA tags between packets and what no queue registers) by size and by VIF command
            uint64_t whyPackets[5] = {}, whyBytes[5] = {};
            uint64_t gapRuns[5] = {}, gapBytes[5] = {}; // <= 16, <= 64, <= 1 KB, <= 16 KB, bigger
            uint64_t gapOps[5] = {}; // DIRECT/DIRECTHL, UNPACK, MPG, other, unparsed (bytes)
            // VIF1 chains with no registered packet at all (all copied): their count, bytes and commands
            uint64_t unlistedItems = 0u, unlistedBytes = 0u, unlistedOps[5] = {};
            std::map<uint32_t, std::pair<uint64_t, uint64_t>> gapAfter; // the packet before a gap of > 1 KB -> runs, bytes
            // the registered packets still copied by (why << 32 | queue caller) -> packets, bytes; the first-frame ones
            // by how far into the run their half's generation was (< 30, < 300, later: a load's warm-up or churn)
            std::map<uint64_t, std::pair<uint64_t, uint64_t>> whyCaller;
            uint64_t firstByAge[3] = {}, firstBytesByAge[3] = {};
            uint64_t mscal = 0u, mscnt = 0u, mpg = 0u, direct = 0u, unpack = 0u, segmentsWithoutMpg = 0u;
            std::map<uint32_t, uint64_t> microcode; // MPG source (guest address) -> uploads
            std::vector<Diff> diffs;                 // the first few
            std::vector<std::string> parseNotes;     // the first few
        };
        std::mutex g_statsMutex;
        Stats g_stats;
        const uint8_t *g_rdram = nullptr;
        std::string g_path, g_header;

        uint32_t guestOf(const uint8_t *p)
        {
            return g_rdram && p >= g_rdram && p < g_rdram + kRamMask + 1u ? static_cast<uint32_t>(p - g_rdram) : 0xFFFFFFFFu;
        }

        // The guest address of byte `at` (a chain offset inside the segment), from the segment's pieces.
        uint32_t guestAt(const ps2x::listed::Segments &l, const ps2x::listed::Segment &s, uint32_t at)
        {
            uint32_t off = s.start;
            for (uint32_t i = s.pieceBegin; i < s.pieceEnd; ++i)
            {
                const ps2x::listed::Piece &p = l.pieces[i];
                if (at < off + p.len)
                    return guestOf(p.src + (at - off));
                off += p.len;
            }
            return 0xFFFFFFFFu;
        }

        // The packet's VIF commands from its first byte: do they end exactly on its last one (no command reaching into
        // the next packet or cut at the start)? The sizes as PS2Memory::processVIF1Data reads them.
        void parseSegment(const ps2x::listed::Segments &l, const ps2x::listed::Segment &s, const uint8_t *data, Stats &st)
        {
            uint32_t pos = s.start;
            const uint32_t end = s.end;
            uint32_t cl = 0u, wl = 0u;
            bool cycleSet = false, beforeCycle = false, mpg = false;
            bool ok = true;
            std::string note;
            while (pos + 4u <= end)
            {
                uint32_t cmd;
                std::memcpy(&cmd, data + pos, 4);
                const uint32_t at = pos;
                pos += 4u;
                const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
                uint32_t payload = 0u;
                if (op == 0x01u) // STCYCL
                    cl = imm & 0xFFu, wl = (imm >> 8) & 0xFFu, cycleSet = true;
                else if (op == 0x20u) // STMASK
                    payload = 4u;
                else if (op == 0x30u || op == 0x31u) // STROW / STCOL
                    payload = 16u;
                else if (op == 0x14u || op == 0x15u)
                    ++st.mscal;
                else if (op == 0x17u)
                    ++st.mscnt;
                else if (op == 0x4Au) // MPG
                {
                    payload = (num == 0u ? 256u : num) * 8u;
                    ++st.mpg;
                    mpg = true;
                    if (pos + payload <= end)
                        ++st.microcode[guestAt(l, s, pos)];
                }
                else if (op == 0x50u || op == 0x51u) // DIRECT / DIRECTHL
                {
                    payload = (imm == 0u ? 65536u : imm) * 16u;
                    ++st.direct;
                }
                else if ((op & 0x60u) == 0x60u) // UNPACK
                {
                    ++st.unpack;
                    if (op & 0x10u)
                        ++st.maskedUnpacks;
                    if (!cycleSet)
                        beforeCycle = true;
                    const uint32_t vn = (op >> 2) & 3u, vl = op & 3u, components = vn + 1u;
                    const uint32_t bits = vl == 0u ? 32u : vl == 1u ? 16u : vl == 2u ? 8u : (vn == 3u ? 4u : 16u);
                    const uint32_t bitsPerVector = (vl == 3u && vn == 3u) ? 16u : components * bits;
                    const uint32_t bytesPerVector = (bitsPerVector + 7u) / 8u;
                    const uint32_t writes = num == 0u ? 256u : num;
                    uint32_t c = cycleSet ? cl : 1u, w = cycleSet ? wl : 1u;
                    if (c == 0u)
                        c = 1u;
                    if (w == 0u)
                        w = 1u;
                    uint32_t sources = writes;
                    if (c < w)
                    {
                        uint32_t rem = writes % w;
                        sources = (writes / w) * c + (rem > c ? c : rem);
                    }
                    payload = (sources * bytesPerVector + 3u) & ~3u;
                }
                if (pos + payload > end)
                {
                    ok = false;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "head 0x%07x: command 0x%08x at +0x%x needs %u bytes, %u left",
                                  s.head, cmd, at - s.start, payload, end - pos);
                    note = buf;
                    break;
                }
                pos += payload;
            }
            if (ok && pos != end)
            {
                ok = false;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "head 0x%07x: %u stray bytes at the end", s.head, end - pos);
                note = buf;
            }
            if (ok)
                ++st.parseOk;
            else
            {
                ++st.parseFail;
                if (st.parseNotes.size() < 8u)
                    st.parseNotes.push_back(note);
            }
            if (beforeCycle)
                ++st.unpackBeforeCycle;
            if (!mpg)
                ++st.segmentsWithoutMpg;
        }

        thread_local std::unordered_map<const void *, std::vector<uint64_t>> t_starts; // live packets' job-start hashes

        // The registry's entry for a head (read from the VU thread: dev only, a racy read of a byte)
        Entry entryOf(uint32_t head)
        {
            const uint32_t key = (head & kRamMask) | 1u;
            for (uint32_t s = slotOf(key), n = 0; n < kSlots; s = (s + 1u) & (kSlots - 1u), ++n)
            {
                const Entry &e = g_table[s];
                if (e.key == key)
                    return e;
                if (e.key == 0u)
                    break;
            }
            Entry none;
            none.why = kWhyShadow;
            return none;
        }
        // A gap's VIF commands (it starts and ends on command edges: the packets around it do): their bytes by kind
        void vifOps(uint64_t *ops, const uint8_t *d, uint32_t n)
        {
            uint32_t pos = 0u, cycle = 0x0101u;
            while (pos + 4u <= n)
            {
                uint32_t cmd;
                std::memcpy(&cmd, d + pos, 4);
                const uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
                uint32_t len = 0u;
                uint64_t *bucket = &ops[3];
                if (op == 0x20u)
                    len = 4u;
                else if (op == 0x30u || op == 0x31u)
                    len = 16u;
                else if (op == 0x01u)
                    cycle = imm;
                else if (op == 0x4Au)
                    len = (num ? num : 256u) * 8u, bucket = &ops[2];
                else if (op == 0x50u || op == 0x51u)
                    len = (imm ? imm : 65536u) * 16u, bucket = &ops[0];
                else if ((op & 0x60u) == 0x60u)
                {
                    const uint32_t vn = (op >> 2) & 3u, vl = op & 3u;
                    const uint32_t bpc = vl == 0 ? 32u : vl == 1 ? 16u : vl == 2 ? 8u : (vn == 3 ? 4u : 16u);
                    const uint32_t bpv = (vl == 3 && vn == 3) ? 16u : (vn + 1u) * bpc;
                    const uint32_t w = num ? num : 256u;
                    uint32_t cl = cycle & 0xFFu, wl = (cycle >> 8) & 0xFFu, src = w;
                    if (!cl)
                        cl = 1u;
                    if (!wl)
                        wl = 1u;
                    if (cl < wl)
                        src = (w / wl) * cl + std::min(w % wl, cl);
                    len = (src * ((bpv + 7u) / 8u) + 3u) & ~3u;
                    bucket = &ops[1];
                }
                if (pos + 4u + len > n)
                    break;
                *bucket += 4u + len;
                pos += 4u + len;
            }
            ops[4] += n - pos;
        }
        void gapRun(Stats &st, const uint8_t *data, uint32_t from, uint32_t to, uint32_t before)
        {
            if (to <= from)
                return;
            const uint32_t n = to - from;
            const int b = n <= 16u ? 0 : n <= 64u ? 1 : n <= 1024u ? 2 : n <= 16384u ? 3 : 4;
            ++st.gapRuns[b];
            st.gapBytes[b] += n;
            if (n > 1024u && st.gapAfter.size() < 4096u)
            {
                auto &g = st.gapAfter[before];
                ++g.first;
                g.second += n;
            }
            vifOps(st.gapOps, data + from, n);
        }

        void observe(const ps2x::listed::Segments &l, const uint8_t *data, size_t size, int phase)
        {
            std::lock_guard<std::mutex> lock(g_statsMutex);
            Stats &st = g_stats;
            if (l.segments.empty())
            {
                // a chain no queue registers (the async VIF thread hands these over too when an observer is set)
                if (phase == 0)
                {
                    ++st.unlistedItems;
                    st.unlistedBytes += size;
                    vifOps(st.unlistedOps, data, static_cast<uint32_t>(size));
                }
                return;
            }
            if (phase == 0)
            {
                ++st.items;
                st.chainBytes += size;
                // the copied bytes' map: segments in chain order (live ones mark where their bytes belong)
                uint32_t at = 0u, before = 0u;
                for (const ps2x::listed::Segment &s : l.segments)
                {
                    const uint32_t start = std::min<uint32_t>(s.start, static_cast<uint32_t>(size));
                    gapRun(st, data, at, start, before);
                    at = std::max(at, start);
                    before = s.head;
                    if (s.live)
                        continue;
                    const uint32_t end = s.closed ? std::min<uint32_t>(s.end, static_cast<uint32_t>(size)) : start;
                    const Entry en = entryOf(s.head);
                    const uint8_t why = en.why < 5u ? en.why : kWhyShadow;
                    const uint64_t n = end > start ? end - start : 0u;
                    ++st.whyPackets[why];
                    st.whyBytes[why] += n;
                    if (st.whyCaller.size() < 4096u || st.whyCaller.count((uint64_t(why) << 32) | en.caller))
                    {
                        auto &c = st.whyCaller[(uint64_t(why) << 32) | en.caller];
                        ++c.first;
                        c.second += n;
                    }
                    if (why == kWhyFirst)
                    {
                        const int age = en.gen < 30u ? 0 : en.gen < 300u ? 1 : 2;
                        ++st.firstByAge[age];
                        st.firstBytesByAge[age] += n;
                    }
                    at = std::max(at, end);
                }
                gapRun(st, data, at, static_cast<uint32_t>(size), before);
            }
            for (const ps2x::listed::Segment &s : l.segments)
            {
                if (phase == 0)
                    ++(s.live ? st.liveSegments : st.shadowSegments);
                if (s.live)
                {
                    // in place: its bytes at job start vs job end (a rewrite during the job is what the VU may have read)
                    uint64_t h = 0u;
                    {
                        XXH3_state_t xs;
                        XXH3_64bits_reset(&xs);
                        for (uint32_t i = s.pieceBegin; i < s.pieceEnd; ++i)
                            XXH3_64bits_update(&xs, l.pieces[i].src, l.pieces[i].len);
                        h = XXH3_64bits_digest(&xs);
                    }
                    std::vector<uint64_t> &v = t_starts[&l];
                    const size_t idx = static_cast<size_t>(&s - l.segments.data());
                    if (phase == 0)
                    {
                        if (v.size() < l.segments.size())
                            v.resize(l.segments.size());
                        v[idx] = h;
                    }
                    else
                    {
                        if (idx < v.size() && v[idx] == h)
                            ++st.liveSame;
                        else
                        {
                            ++st.liveDiffer;
                            if (st.liveDiffs.size() < 16u)
                                st.liveDiffs.push_back(s.head);
                        }
                        continue;
                    }
                    ++st.segments;
                    if (!s.closed)
                        ++st.unclosed;
                    static thread_local std::vector<uint8_t> packet;
                    packet.clear();
                    for (uint32_t i = s.pieceBegin; i < s.pieceEnd; ++i)
                        packet.insert(packet.end(), l.pieces[i].src, l.pieces[i].src + l.pieces[i].len);
                    st.bytes += packet.size();
                    st.chainBytes += packet.size(); // the chain's whole size: its listed bytes are not in data
                    ps2x::listed::Segment t = s;
                    t.start = 0u;
                    t.end = static_cast<uint32_t>(packet.size());
                    parseSegment(l, t, packet.data(), st);
                    continue;
                }
                if (phase == 0)
                {
                    ++st.segments;
                    if (!s.closed || s.end > size)
                    {
                        ++st.unclosed;
                        continue;
                    }
                    st.bytes += s.end - s.start;
                    parseSegment(l, s, data, st);
                }
                else if (!s.closed || s.end > size)
                    continue;
                else if (rewrittenBit(s.head)) // copied because a both-halves writer changes it (b3 B): RAM moves on
                {
                    ++st.marked;
                    continue;
                }
                // the packet's bytes in guest RAM now vs the copy the walker made at the kick
                uint32_t off = s.start;
                bool same = true;
                for (uint32_t i = s.pieceBegin; i < s.pieceEnd && same; ++i)
                {
                    const ps2x::listed::Piece &p = l.pieces[i];
                    if (std::memcmp(p.src, data + off, p.len) != 0)
                    {
                        same = false;
                        if (st.diffs.size() < 16u)
                        {
                            uint32_t k = 0u;
                            while (k < p.len && p.src[k] == data[off + k])
                                ++k;
                            st.diffs.push_back(Diff{phase, s.head, guestOf(p.src + k), p.len - k});
                        }
                    }
                    off += p.len;
                }
                if (same)
                    ++st.same[phase];
                else
                    ++st.differ[phase];
            }
            if (phase == 1)
                t_starts.erase(&l);
        }
    }

    void report(const char *outputBase, const std::string &header)
    {
        if (!g_on)
            return;
        g_path = std::string(outputBase) + ".drawlist.txt";
        g_header = header;
        std::remove(g_path.c_str());
        ps2x::asyncvif::setListedObserver(&observe);
        std::fprintf(stderr, "[native] draw list report: %s\n", g_path.c_str());
    }

    void end()
    {
        if (!g_on || g_path.empty())
            return;
        ps2x::asyncvif::setListedObserver(nullptr);
        Stats st;
        {
            std::lock_guard<std::mutex> lock(g_statsMutex);
            st = g_stats;
        }
        std::FILE *f = std::fopen(g_path.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "# GHR draw list %s v1 %s\n", g_live ? "live" : "shadow", g_header.c_str());
        uint64_t hits = 0u, misses = 0u;
        ps2x::listed::cacheCounts(hits, misses);
        std::fprintf(f, "cache hits %llu misses %llu\n", static_cast<unsigned long long>(hits),
                     static_cast<unsigned long long>(misses));
        std::fprintf(f, "kinds live %llu shadow %llu registeredshadow %llu queuelive %llu queuereused %llu queuenew %llu\n",
                     static_cast<unsigned long long>(st.liveSegments), static_cast<unsigned long long>(st.shadowSegments),
                     static_cast<unsigned long long>(g_registeredShadow), static_cast<unsigned long long>(g_queueLive),
                     static_cast<unsigned long long>(g_queueReused), static_cast<unsigned long long>(g_queueNew));
        std::fprintf(f, "registered %llu found %llu unseen %llu tagchanged %llu full %llu halves %llu\n",
                     static_cast<unsigned long long>(g_registered), static_cast<unsigned long long>(g_found),
                     static_cast<unsigned long long>(g_unseen), static_cast<unsigned long long>(g_tagChanged),
                     static_cast<unsigned long long>(g_full), static_cast<unsigned long long>(g_halves));
        std::fprintf(f, "segments %llu unclosed %llu bytes %llu chainbytes %llu items %llu\n",
                     static_cast<unsigned long long>(st.segments), static_cast<unsigned long long>(st.unclosed),
                     static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.chainBytes),
                     static_cast<unsigned long long>(st.items));
        std::fprintf(f, "parse ok %llu fail %llu unpackbeforecycle %llu maskedunpacks %llu\n",
                     static_cast<unsigned long long>(st.parseOk), static_cast<unsigned long long>(st.parseFail),
                     static_cast<unsigned long long>(st.unpackBeforeCycle), static_cast<unsigned long long>(st.maskedUnpacks));
        std::fprintf(f, "live start same %llu differ %llu end same %llu differ %llu marked %llu\n",
                     static_cast<unsigned long long>(st.same[0]), static_cast<unsigned long long>(st.differ[0]),
                     static_cast<unsigned long long>(st.same[1]), static_cast<unsigned long long>(st.differ[1]),
                     static_cast<unsigned long long>(st.marked));
        // a queue packet's record: halves 3 or twice 1 = the reused buffer the rule keeps copied, as it should
        auto reuseOf = [](uint32_t head)
        {
            const auto r = g_reuse.find((head & kRamMask) | 1u);
            return r == g_reuse.end() ? Reuse{} : r->second;
        };
        auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::fprintf(f, "copiedwhy firstn %llu firstb %llu reusedn %llu reusedb %llu markedn %llu markedb %llu shadown %llu "
                        "shadowb %llu\n",
                     ull(st.whyPackets[kWhyFirst]), ull(st.whyBytes[kWhyFirst]), ull(st.whyPackets[kWhyReused]),
                     ull(st.whyBytes[kWhyReused]), ull(st.whyPackets[kWhyMarked]), ull(st.whyBytes[kWhyMarked]),
                     ull(st.whyPackets[kWhyShadow]), ull(st.whyBytes[kWhyShadow]));
        std::fprintf(f, "copiedgap n16 %llu b16 %llu n64 %llu b64 %llu n1k %llu b1k %llu n16k %llu b16k %llu nbig %llu bbig %llu\n",
                     ull(st.gapRuns[0]), ull(st.gapBytes[0]), ull(st.gapRuns[1]), ull(st.gapBytes[1]), ull(st.gapRuns[2]),
                     ull(st.gapBytes[2]), ull(st.gapRuns[3]), ull(st.gapBytes[3]), ull(st.gapRuns[4]), ull(st.gapBytes[4]));
        std::fprintf(f, "copiedops direct %llu unpack %llu mpg %llu other %llu unparsed %llu\n", ull(st.gapOps[0]),
                     ull(st.gapOps[1]), ull(st.gapOps[2]), ull(st.gapOps[3]), ull(st.gapOps[4]));
        std::fprintf(f, "copiedfirstage n30 %llu b30 %llu n300 %llu b300 %llu nlater %llu blater %llu reuseclears %llu\n",
                     ull(st.firstByAge[0]), ull(st.firstBytesByAge[0]), ull(st.firstByAge[1]), ull(st.firstBytesByAge[1]),
                     ull(st.firstByAge[2]), ull(st.firstBytesByAge[2]), ull(g_reuseClears));
        for (const auto &[k, c] : st.whyCaller)
            if (static_cast<uint32_t>(k >> 32) != kWhyLive)
                std::fprintf(f, "copiedby %u 0x%06x %llu %llu\n", static_cast<unsigned>(k >> 32), static_cast<uint32_t>(k),
                             ull(c.first), ull(c.second));
        std::fprintf(f, "copiedunlisted items %llu bytes %llu direct %llu unpack %llu mpg %llu other %llu unparsed %llu\n",
                     ull(st.unlistedItems), ull(st.unlistedBytes), ull(st.unlistedOps[0]), ull(st.unlistedOps[1]),
                     ull(st.unlistedOps[2]), ull(st.unlistedOps[3]), ull(st.unlistedOps[4]));
        {
            std::vector<std::pair<uint64_t, uint32_t>> big; // bytes, the packet before
            for (const auto &[head, g] : st.gapAfter)
                big.emplace_back(g.second, head);
            std::sort(big.rbegin(), big.rend());
            for (size_t i = 0; i < big.size() && i < 8u; ++i)
            {
                const Reuse r = reuseOf(big[i].second);
                std::fprintf(f, "copiedafter 0x%07x %llu caller 0x%06x\n", big[i].second, ull(big[i].first), r.caller);
            }
        }
        std::fprintf(f, "ops mscal %llu mscnt %llu mpg %llu direct %llu unpack %llu nompg %llu\n",
                     static_cast<unsigned long long>(st.mscal), static_cast<unsigned long long>(st.mscnt),
                     static_cast<unsigned long long>(st.mpg), static_cast<unsigned long long>(st.direct),
                     static_cast<unsigned long long>(st.unpack), static_cast<unsigned long long>(st.segmentsWithoutMpg));
        for (const auto &[src, n] : st.microcode)
            std::fprintf(f, "microcode 0x%07x %llu\n", src, static_cast<unsigned long long>(n));
        std::fprintf(f, "inplace same %llu differ %llu\n", static_cast<unsigned long long>(st.liveSame),
                     static_cast<unsigned long long>(st.liveDiffer));
        std::fprintf(f, "rewrites marks %llu waits %llu timeouts %llu\n", static_cast<unsigned long long>(g_rewriteMarks),
                     static_cast<unsigned long long>(g_rewriteWaits), static_cast<unsigned long long>(g_rewriteTimeouts));
        std::fprintf(f, "uploads unlisted %llu\n", static_cast<unsigned long long>(g_uploadsUnlisted));
        std::fprintf(f, "executor gathers %llu\n",
                     static_cast<unsigned long long>(ps2x::listed::executorGathers.load(std::memory_order_relaxed)));
        for (const uint32_t head : st.liveDiffs)
        {
            const Reuse r = reuseOf(head);
            std::fprintf(f, "diff inplace head 0x%07x halves %u twice %u frames %u caller 0x%06x\n", head, r.halves,
                         r.twice ? 1u : 0u, r.frames, r.caller);
        }
        for (const Diff &d : st.diffs)
        {
            const Reuse r = reuseOf(d.head);
            std::fprintf(f, "diff %s head 0x%07x at 0x%07x len %u halves %u twice %u caller 0x%06x\n",
                         d.phase == 0 ? "start" : "end", d.head, d.at, d.len, r.halves, r.twice ? 1u : 0u, r.caller);
        }
        for (const std::string &n : st.parseNotes)
            std::fprintf(f, "parsefail %s\n", n.c_str());
        std::fclose(f);
    }
#endif

    void rewriting(uint32_t node, uint32_t head, uint32_t caller)
    {
        if (!g_on)
            return;
        const uint32_t n = node & kRamMask;
        for (const uint32_t h : {n, n + 0xB0u})
        {
            uint64_t &word = g_rewrittenBits[(h & kRamMask) >> 10];
            const uint64_t bit = 1ull << (((h & kRamMask) >> 4) & 63u);
            if (!(word & bit))
                ++g_rewriteMarks;
            word |= bit;
        }
        // the written packet in place in its half's current generation: the VU job in flight may still read it
        const uint32_t key = (head & kRamMask) | 1u;
        for (uint32_t s = slotOf(key), k = 0; k < kSlots; s = (s + 1u) & (kSlots - 1u), ++k)
        {
            Entry &e = g_table[s];
            if (e.key == 0u)
                break;
            if (e.key != key || !current(e))
                continue;
            if (!e.shadow)
            {
                ++g_rewriteWaits;
                if (!ps2x::asyncvif::waitVuIdle(std::chrono::milliseconds(250)))
                {
                    if (g_rewriteTimeouts++ == 0u)
                        std::fprintf(stderr, "[native] draw list: the VU worker stayed busy 250 ms (packet 0x%07x)\n", head);
                }
                e.shadow = true; // (its job has run: the entry is for the record only now)
            }
            break;
        }
        (void)caller;
    }

    void begin(const uint8_t *rdram)
    {
        const char *value = std::getenv("KAMITE_NATIVE_DRAWLIST");
        const std::string mode = value ? value : "";
        if (mode == "0")
        {
            std::fprintf(stderr, "[native] draw list: off (KAMITE_NATIVE_DRAWLIST=0)\n");
            return;
        }
#if !defined(KAMITE_RELEASE)
        g_live = mode != "shadow";
        ps2x::listed::setCache(mode != "nocache"); // nocache: live, every listed packet's tags walked
        g_rdram = rdram;
#else
        g_live = true;
        (void)rdram;
#endif
        g_table.assign(kSlots, Entry{});
        g_headBits.assign((kRamMask + 1u) / 16u / 64u, 0ull);
        g_rewrittenBits.assign((kRamMask + 1u) / 16u / 64u, 0ull);
        g_ram = rdram;
        g_on = true;
        ps2x::listed::setLookup(&lookup, g_live);
        std::fprintf(stderr, "[native] draw list: %s (KAMITE_NATIVE_DRAWLIST=%s)\n", g_live ? "live" : "shadow",
                     mode.empty() ? "unset" : mode.c_str());
    }
}
