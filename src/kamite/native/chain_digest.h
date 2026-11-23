#pragma once

// Rung 3's end-to-end check (claude/native-next.md; dev builds only, nothing in KAMITE_RELEASE): during a test-case
// replay, every DMA chain the runtime's walker builds (runtime/ps2_dma_chain.h: what the game sends to VU1 and the GS)
// is hashed into <case>.<30|60>.chain.bin. Replay-Cases.cmd fingerprints the file, so two builds (or a build with a
// native packet builder on and off) sent byte-identical chains when the lines match. tools/dma/chain_compare.py finds
// the first chain that differs between two such files.
//
// Only chains started during the replay's own frames count (test_mode::replayFrame; not the boot or the state load).
// File: "GHRCHN1\0", then one 16-byte record per chain: u32 test frame, u32 channel << 28 (0x8 VIF0, 0x9 VIF1, 0xA GIF)
// | bytes, u64 XXH3 of the bytes. No build stamp in it: identical chains give an identical file.
//
// gs.bin (rung 3 step b): what the GS receives, so builds compare across the switch to the draw list (when listed
// packets leave the chain, chain.bin changes and this does not). Every packet the VU worker hands the GIF stage, per
// job that the EE queued during a replay frame: "GHRGSD1\0", then 16-byte records: u32 test frame (at the kick), u32
// packets, u64 XXH3 over each packet's (u32 size, u32 GIF path, bytes). Jobs without packets write nothing.

namespace kamite::native::chain_digest
{
    // During a replay (KAMITE_REPLAY_CASE): opens nothing yet, registers the walker's and the VU worker's observers; the
    // files are created at the first chain / job. path = the case + ".30" / ".60" (the shadow harness's output base).
    void begin(const char *outputBase);
    // At exit (native::flush).
    void end();
}
