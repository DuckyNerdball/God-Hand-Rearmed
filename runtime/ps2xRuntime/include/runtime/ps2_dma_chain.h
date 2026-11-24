#pragma once

// The DMA chain walker for the GIF / VIF1 / VIF0 channels' source-chain mode (PS2Memory::writeIORegister, CHCR with
// MOD = 1). Native road rung 3, step 0 (claude/native-next.md): the walk first lists the guest bytes the chain sends
// as pieces (adjacent ones merged), then the caller copies them in one go. It used to grow the buffer with a vector
// insert per tag (two with TTE) and translate every address through PS2Memory::translateAddress: ~5% of the game
// thread in crowded scenes (34,000 tags, 7.6 MB a frame on K's casino capture). The bytes sent, their order and the
// registers left behind are exactly the old walker's (tools/dma/chain_walk_check.cpp runs both on K's RAM captures).

#include "runtime/ps2_byte_buffer.h"
#include "runtime/ps2_listed_packets.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ps2x::dma
{
    struct ChainPiece
    {
        const uint8_t *src;
        uint32_t len;
    };

    // The channel's registers going in (TADR, ASR0 / ASR1, CHCR's ASP and tag bits) and coming out.
    struct ChainWalk
    {
        uint32_t tagAddr = 0u;
        uint32_t asr0 = 0u;
        uint32_t asr1 = 0u;
        uint32_t asp = 0u;
        uint32_t lastTagUpper = 0u;
        int tagsProcessed = 0;
        bool looped = false;
    };

    constexpr int kMaxChainTags = 1 << 20;

    // translate(address) is PS2Memory::translateAddress (it may throw); it is only called for addresses past the RAM
    // backing, where it returns the address itself for everything below (addresses under 0x20000000 that are not the
    // scratchpad's are taken as physical). tie = CHCR.TIE, tagData = TTE on a VIF channel (each tag's upper 64 bits go
    // in front of its payload). Returns false if the chain looped or had no end (the caller drops it).
    // Step b (ps2_listed_packets.h): with a lookup, the chain's first tag and every NEXT tag's target outside a listed
    // packet are asked whether they start one; each listed packet gets a mark (its byte range in the chain, up to and with
    // its tail tag). With listedPieces too (live), a listed packet's bytes go there instead of into pieces / bytesOut (the
    // mark keeps its range in listedPieces). The walk itself (tags, order, registers) is the same either way.
    template <typename Translate>
    bool walkChain(ChainWalk &w, bool tie, bool tagData, const uint8_t *rdram, const uint8_t *scratchpad,
                   Translate &&translate, std::vector<ChainPiece> &pieces, size_t &bytesOut,
                   ps2x::listed::Lookup lookup = nullptr, std::vector<ps2x::listed::Mark> *marks = nullptr,
                   std::vector<ChainPiece> *listedPieces = nullptr, ps2x::listed::Cache *cache = nullptr)
    {
        pieces.clear();
        if (!marks)
            lookup = nullptr;
        else
            marks->clear();
        if (!lookup)
            listedPieces = nullptr;
        else if (listedPieces)
            listedPieces->clear();
        bool headNext = lookup != nullptr; // ask about the next tag: the chain's first, then NEXT targets
        int open = -1;                     // the listed packet being walked (index into marks)
        size_t openPiece = 0u;             // live: its first piece in listedPieces (no merging across packets)
        // The cached decode (ps2_listed_packets.h Cache): rec = the entry being recorded for the open packet.
        if (!lookup)
            cache = nullptr;
        ps2x::listed::CacheEntry *rec = nullptr;
        uint32_t recKey = 0u;
        int recStart = 0;
        auto dropRec = [&]()
        {
            if (rec)
                cache->entries.erase(recKey);
            rec = nullptr;
        };
        size_t total = 0u;
        auto add = [&](const uint8_t *src, uint32_t len)
        {
            if (open >= 0 && (*marks)[open].live)
            {
                std::vector<ChainPiece> &lp = *listedPieces;
                if (lp.size() > openPiece && lp.back().src + lp.back().len == src)
                    lp.back().len += len;
                else
                    lp.push_back(ChainPiece{src, len});
                if (rec)
                {
                    if (src >= rdram && src + len <= rdram + PS2_RDRAM_SIZE)
                    {
                        const uint32_t off = static_cast<uint32_t>(src - rdram);
                        if (!rec->pieces.empty() && rec->pieces.back().first + rec->pieces.back().second == off)
                            rec->pieces.back().second += len;
                        else
                            rec->pieces.emplace_back(off, len);
                    }
                    else
                        dropRec(); // the scratchpad: not remembered
                }
                return;
            }
            if (rec)
            {
                if (src >= rdram && src + len <= rdram + PS2_RDRAM_SIZE)
                {
                    const uint32_t off = static_cast<uint32_t>(src - rdram);
                    if (!rec->pieces.empty() && rec->pieces.back().first + rec->pieces.back().second == off)
                        rec->pieces.back().second += len;
                    else
                        rec->pieces.emplace_back(off, len);
                }
                else
                    dropRec();
            }
            if (!pieces.empty() && pieces.back().src + pieces.back().len == src)
                pieces.back().len += len;
            else
                pieces.push_back(ChainPiece{src, len});
            total += len;
        };
        auto phys = [&](uint32_t address) -> uint32_t
        {
            return address < PS2_RDRAM_SIZE ? address : translate(address);
        };
        auto scratch = [](uint32_t address) { return address >= PS2_RDRAM_SIZE && ps2IsScratchpadAddress(address); };

        auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
            uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            const bool spr = scratch(srcAddr);
            uint32_t src = phys(srcAddr);
            const uint8_t *base = spr ? scratchpad : rdram;
            const uint32_t maxSz = spr ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
            while (bytes > 0)
            {
                if (src >= maxSz)
                    src = 0;
                uint32_t chunk = bytes;
                if (src + chunk > maxSz)
                    chunk = maxSz - src;
                if (chunk == 0)
                    break;
                add(base + src, chunk);
                bytes -= chunk;
                src += chunk;
            }
        };

        uint32_t tagAddr = w.tagAddr, asr0 = w.asr0, asr1 = w.asr1, asp = w.asp;
        uint32_t lastTagUpper = w.lastTagUpper;
        int tagsProcessed = 0;
        // A chain that comes back to a tag in the same walker state (address, call stack) loops forever: the PS2's
        // DMA would hang there. Brent's cycle check finds it exactly at no cost per tag.
        uint32_t loopTag = tagAddr, loopAsp = asp, loopAsr0 = asr0, loopAsr1 = asr1;
        int loopPower = 1, loopLength = 0;
        bool looped = false;

        while (tagsProcessed < kMaxChainTags)
        {
            const uint32_t currentTagAddr = tagAddr;
            const bool tagInSPR = scratch(tagAddr);
            uint32_t physTag = 0;
            if (tagAddr < PS2_RDRAM_SIZE)
                physTag = tagAddr;
            else
            {
                try
                {
                    physTag = translate(tagAddr);
                }
                catch (...)
                {
                    break;
                }
            }
            const uint8_t *tagBase = tagInSPR ? scratchpad : rdram;
            const uint32_t tagMax = tagInSPR ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
            if (physTag + 16 > tagMax)
                break;

            const uint8_t *tp = tagBase + physTag;
            uint64_t tag;
            std::memcpy(&tag, tp, sizeof(tag));
            const uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
            const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
            const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
            lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
            ++tagsProcessed;
            bool jumped = false; // the cached decode replayed the whole packet
            if (headNext)
            {
                headNext = false;
                uint32_t tail = 0u;
                bool shadowOnly = false;
                if (open < 0 && lookup(currentTagAddr, tag, tail, shadowOnly))
                {
                    marks->push_back(ps2x::listed::Mark{currentTagAddr, tail, static_cast<uint32_t>(total), 0u, false});
                    open = static_cast<int>(marks->size()) - 1;
                    marks->back().live = listedPieces && !shadowOnly;
                    if (listedPieces)
                        marks->back().pieceBegin = static_cast<uint32_t>(openPiece = listedPieces->size());
                    if (cache && !tagInSPR && tagAddr < PS2_RDRAM_SIZE)
                    {
                        auto it = cache->entries.find(physTag);
                        bool valid = it != cache->entries.end() && it->second.ready && it->second.headAsp == asp &&
                                     it->second.tie == tie && it->second.tagData == tagData;
                        if (valid)
                            for (const auto &[off, value] : it->second.tags)
                            {
                                uint64_t now;
                                std::memcpy(&now, rdram + off, sizeof(now));
                                if (now != value)
                                {
                                    valid = false;
                                    break;
                                }
                            }
                        if (valid)
                        {
                            const ps2x::listed::CacheEntry &e = it->second;
                            for (const auto &[off, len] : e.pieces)
                                add(rdram + off, len);
                            (*marks)[open].end = static_cast<uint32_t>(total);
                            (*marks)[open].closed = true;
                            if (listedPieces)
                                (*marks)[open].pieceEnd = static_cast<uint32_t>(listedPieces->size());
                            open = -1;
                            tagsProcessed += e.tagCount - 1;
                            lastTagUpper = e.lastTagUpper;
                            tagAddr = e.next;
                            if (e.wrote0)
                                asr0 = e.asr0;
                            if (e.wrote1)
                                asr1 = e.asr1;
                            headNext = true; // the tail is a NEXT
                            jumped = true;
                            ++cache->hits;
                        }
                        else
                        {
                            ++cache->misses;
                            if (cache->entries.size() >= 32768u)
                                cache->entries.clear();
                            recKey = physTag;
                            rec = &cache->entries[recKey];
                            rec->tags.clear();
                            rec->pieces.clear();
                            rec->ready = false;
                            rec->wrote0 = rec->wrote1 = false;
                            rec->headAsp = asp;
                            rec->tie = tie;
                            rec->tagData = tagData;
                            recStart = tagsProcessed;
                        }
                    }
                }
            }
            if (jumped)
            {
                // the loop check below, as after any tag
            }
            else
            {
            if (rec)
            {
                if (!tagInSPR && physTag + 8u <= PS2_RDRAM_SIZE && tagAddr < PS2_RDRAM_SIZE)
                    rec->tags.emplace_back(physTag, tag);
                else
                    dropRec();
            }

            uint32_t dataAddr = 0;
            bool hasPayload = (tagQwc > 0);
            bool endChain = false;

            switch (id)
            {
            case 0: // refe
                dataAddr = addr;
                tagAddr = tagAddr + 16;
                endChain = true;
                break;
            case 1: // cnt
                dataAddr = tagAddr + 16;
                tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                break;
            case 2: // next
                dataAddr = tagAddr + 16;
                tagAddr = addr;
                break;
            case 3: // ref
            case 4: // refs
                dataAddr = addr;
                tagAddr = tagAddr + 16;
                break;
            case 5: // call
                dataAddr = tagAddr + 16;
                {
                    const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                    if (rec && asp < 2u)
                        (asp == 0u ? rec->wrote0 : rec->wrote1) = true;
                    if (asp == 0u)
                    {
                        asr0 = retAddr;
                        asp = 1u;
                    }
                    else if (asp == 1u)
                    {
                        asr1 = retAddr;
                        asp = 2u;
                    }
                }
                tagAddr = addr;
                break;
            case 6: // ret
                dataAddr = tagAddr + 16;
                if (asp == 2u)
                {
                    tagAddr = asr1;
                    asp = 1u;
                }
                else if (asp == 1u)
                {
                    tagAddr = asr0;
                    asp = 0u;
                }
                else
                {
                    endChain = true;
                }
                break;
            case 7: // end
                dataAddr = tagAddr + 16;
                endChain = true;
                break;
            default:
                hasPayload = false;
                endChain = true;
                break;
            }

            if (tagData)
            {
                // The tag's own upper 64 bits, from the tag as the old walker read them (translated again: the same
                // address, so the same place).
                const bool localScratch = scratch(currentTagAddr);
                const uint32_t localPhys = phys(currentTagAddr);
                const uint8_t *localBase = localScratch ? scratchpad : rdram;
                const uint32_t localMax = localScratch ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
                if (localPhys + 16u <= localMax)
                    add(localBase + localPhys + 8u, 8u);
            }

            if (hasPayload)
                appendData(dataAddr, tagQwc);
            if (open >= 0 && currentTagAddr == (*marks)[open].tail)
            {
                (*marks)[open].end = static_cast<uint32_t>(total);
                (*marks)[open].closed = true;
                if (listedPieces)
                    (*marks)[open].pieceEnd = static_cast<uint32_t>(listedPieces->size());
                open = -1;
                if (rec)
                {
                    // remembered only if the packet ended the plain way: its tail a NEXT, no interrupt / end on the way,
                    // the call stack back where it was
                    if (id == 2u && !irq && !endChain && asp == rec->headAsp)
                    {
                        rec->next = tagAddr;
                        rec->lastTagUpper = lastTagUpper;
                        rec->asr0 = asr0;
                        rec->asr1 = asr1;
                        rec->tagCount = tagsProcessed - recStart + 1;
                        rec->ready = true;
                        rec = nullptr;
                    }
                    else
                        dropRec();
                }
            }
            else if (rec && (endChain || (irq && tie)))
                dropRec();
            if (lookup && id == 2u && open < 0)
                headNext = true;
            if (irq && tie)
                endChain = true;
            if (endChain)
                break;
            }
            if (tagAddr == loopTag && asp == loopAsp && asr0 == loopAsr0 && asr1 == loopAsr1)
            {
                looped = true;
                break;
            }
            if (++loopLength == loopPower)
            {
                loopTag = tagAddr;
                loopAsp = asp;
                loopAsr0 = asr0;
                loopAsr1 = asr1;
                loopPower <<= 1;
                loopLength = 0;
            }
        }

        w.tagAddr = tagAddr;
        w.asr0 = asr0;
        w.asr1 = asr1;
        w.asp = asp;
        w.lastTagUpper = lastTagUpper;
        w.tagsProcessed = tagsProcessed;
        w.looped = looped;
        bytesOut = total;
        if (open >= 0 && listedPieces) // the chain ended inside a listed packet: its bytes so far stay its own
            (*marks)[open].pieceEnd = static_cast<uint32_t>(listedPieces->size());
        dropRec();
        return !(looped || tagsProcessed >= kMaxChainTags);
    }

    // Copies the pieces into out (resized to bytes, no zero fill first).
    inline void gatherChain(const std::vector<ChainPiece> &pieces, size_t bytes, ByteBuffer &out)
    {
        out.resize(bytes);
        uint8_t *dst = out.data();
        for (const ChainPiece &p : pieces)
        {
            std::memcpy(dst, p.src, p.len);
            dst += p.len;
        }
    }

    // Dev builds' check of rung 3 (claude/native-next.md): called with every chain the walker built, on the thread that
    // started the DMA, before it is queued. Unset (nullptr) in play and releases. listed: the draw list's packets (live:
    // their bytes are not in data but belong at each segment's start; the whole chain is data with them put back).
    using ChainObserver = void (*)(uint32_t channelBase, const uint8_t *data, size_t size,
                                   const ps2x::listed::Segments *listed);
    void setChainObserver(ChainObserver observer);
    // The same chain as its source pieces (where each run of bytes came from: rdram = the RAM backing), for the dev
    // dump of early frames (chain_digest.h, KAMITE_CHAIN_DUMP); live listed packets put back in place. Unset in play and
    // releases.
    using ChainPiecesObserver = void (*)(uint32_t channelBase, const ChainPiece *pieces, size_t count, const uint8_t *rdram);
    void setChainPiecesObserver(ChainPiecesObserver observer);
}
