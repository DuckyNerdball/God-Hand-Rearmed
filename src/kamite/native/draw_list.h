#pragma once

// Native road rung 3, step b: the draw list (claude/native-next.md "Step b"; K's answers Nov 23). The fused natives
// (packet.cpp: cModel_Tag_set_rate's subtree) register each PS2 packet they queue: the model's own packet (0x70 bytes,
// its tail at +0x70) and every node's packet (node + half * 0xB0, its tail at +0xA0). The game still gets those packets
// written exactly as before (K, Q2): the list sits beside them.
//
// Live (build 2, the default in every build): the DMA walker leaves the registered packets' bytes out of the VIF1 chain
// it copies and the VU thread reads them in place (runtime/ps2_listed_packets.h, PS2Memory::processVIF1Listed).
// Build 1's shadow proved that exact over K's 36 replays (Nov 24): every packet's VIF commands on its own edges, its
// bytes unchanged in RAM until the VU job ran. The shadow stays available (KAMITE_NATIVE_DRAWLIST=shadow, dev builds):
// the walker copies everything and the VU thread checks each packet against RAM. During dev replays either mode writes
// <case>.<30|60>.drawlist.txt (tools/fps60/replay_verdict.py sums them).
//
// KAMITE_NATIVE_DRAWLIST (hidden, GHR.ini [Runtime] like the other natives' switches): unset = live; 0 = off (the walker
// copies every packet as before build 2); shadow = the shadow (dev builds; live in releases); nocache = live without the
// walker's cached decode (every listed packet's tags walked each frame, as in build 2).

#include <cstdint>
#include <string>

namespace kamite::native::draw_list
{
    extern bool g_on; // registering (live or shadow)

    // Where a registration comes from. Proven: the fused natives' own packets (read in place). Shadow: copied, and the
    // dev replays check them (never downgrades a live entry). Queue (b3 A: every packet the queue natives link): read in
    // place once the head has been queued in two frames of the same half and never in the other (a double-buffered
    // packet); a head queued in both halves, or twice in one frame, is a buffer the game rewrites while our VU thread may
    // still be reading an earlier chain (electricity_check, Nov 24), so it stays copied for good.
    enum class Source : uint8_t
    {
        Proven,
        Shadow,
        Queue
    };
    // The guest caller (ra) of the queue native running now (game thread; set by packet.cpp's queue natives): kept with
    // each Source::Queue head for the dev report.
    extern uint32_t g_queueCaller;
    // After a native committed (game thread): `count` packets it queued for frame half `half`, each as its head tag's
    // address, its tail tag's address and the head tag's first 8 bytes as now in RAM.
    void listPackets(uint32_t half, const uint32_t *heads, const uint32_t *tails, const uint64_t *headTags, uint32_t count,
                     Source source = Source::Proven);

    // Rung 3 step b3 B (K, Nov 24): func_00155D60's native (game thread) before it changes a node packet's +0x30 in the
    // half the game is not building (cObj_setMeshValue / objScroll_Set_test write both halves; the other half's packet
    // may be read in place by the VU job in flight): the node's two packets (node, node + 0xB0) are copied for good from
    // now on, whatever registers them; when the written packet is in place in its half's current generation, the game
    // thread first waits for the VU worker (host only, once per node). caller = the writer's return address (dev report).
    void rewriting(uint32_t node, uint32_t head, uint32_t caller);

    // native.cpp installFromEnvironment, every build: reads KAMITE_NATIVE_DRAWLIST, starts the registry and the
    // walker's lookup. rdram = the RAM backing (to name guest addresses in the report).
    void begin(const uint8_t *rdram);

#if !defined(KAMITE_RELEASE)
    // Dev replays, after begin(): the VU thread's observer (shadow: the checks; live: counts and the VIF edge check) and
    // the report. outputBase = the case + ".30" / ".60"; header = the .native.txt header line's text.
    void report(const char *outputBase, const std::string &header);
    // At exit (native::flush): writes <outputBase>.drawlist.txt.
    void end();
#else
    inline void report(const char *, const std::string &) {}
    inline void end() {}
#endif
}
