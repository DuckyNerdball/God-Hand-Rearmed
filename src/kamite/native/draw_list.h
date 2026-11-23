#pragma once

// Native road rung 3, step b: the draw list (claude/native-next.md "Step b"; K's answers Nov 23). The fused natives
// (packet.cpp: cModel_Tag_set_rate's subtree) register each PS2 packet they queue: the model's own packet (0x70 bytes,
// its tail at +0x70) and every node's packet (node + half * 0xB0, its tail at +0xA0). The game still gets those packets
// exactly as before (K, Q2): the list sits beside them.
//
// Build 1 (this one) is the shadow: during dev replays the DMA walker marks the registered packets in the VIF1 chain
// (runtime/ps2_listed_packets.h) and the VU thread checks, per packet, that its VIF commands start and end on its own
// edges and that its bytes in guest RAM are still the ones the walker copied when the job runs (start and end of the
// job): what the live list (read in place, no copy) needs to be exact. Results in <case>.<30|60>.drawlist.txt;
// tools/fps60/replay_verdict.py sums them. Nothing changes in play; nothing at all in KAMITE_RELEASE.
//
// KAMITE_NATIVE_DRAWLIST ([Runtime], hidden): unset = the shadow during replays; 0 = off. (1 = live: not built yet.)

#include <cstdint>
#include <string>

namespace kamite::native::draw_list
{
#if !defined(KAMITE_RELEASE)
    extern bool g_on; // registering (the shadow during a replay)

    // After a fused native committed (game thread): `count` packets it queued for frame half `half`, each as its head
    // tag's address, its tail tag's address and the head tag's first 8 bytes as now in RAM.
    void listPackets(uint32_t half, const uint32_t *heads, const uint32_t *tails, const uint64_t *headTags, uint32_t count);

    // Replays (native.cpp installFromEnvironment): registry, walker lookup and VU-thread observer on, unless
    // KAMITE_NATIVE_DRAWLIST=0. outputBase = the case + ".30" / ".60"; header = the .native.txt header line's text;
    // rdram = the RAM backing (to name guest addresses in the report).
    void begin(const char *outputBase, const std::string &header, const uint8_t *rdram);
    // At exit (native::flush): writes <outputBase>.drawlist.txt.
    void end();
#else
    constexpr bool g_on = false;
    inline void listPackets(uint32_t, const uint32_t *, const uint32_t *, const uint64_t *, uint32_t) {}
    inline void begin(const char *, const std::string &, const uint8_t *) {}
    inline void end() {}
#endif
}
