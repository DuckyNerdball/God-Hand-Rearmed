#pragma once

// Native road rung 3, step b: the draw list (claude/native-next.md "Step b"). A native packet builder registers the
// PS2 packets it wrote ("listed" packets: kamite's draw list keeps them by head address). The DMA chain walker marks
// where each listed packet sits in the VIF1 chain it gathers, and the VU worker hands those marks to the draw list's
// observer before and after it runs the chain.
//   Live (build 2, the default): the walker leaves a listed packet's bytes out of the chain it copies and keeps them as
//   pointers into guest RAM; the VU thread runs the chain up to the packet, the packet's bytes read in place, then on
//   (PS2Memory::processVIF1Listed). Exact: build 1's shadow found, over K's 36 replays (1.76M packets, Nov 24), every
//   packet's VIF commands on its own edges and its bytes in RAM unchanged until the VU job ran.
//   Shadow (dev, KAMITE_NATIVE_DRAWLIST=shadow): the walker copies everything and marks the packets; the observer checks.
// Nothing here changes what the VU or the GS receive. Small on purpose: ps2_memory.h, ps2_dma_chain.h and
// ps2_async_vif.h include it.

#include <cstdint>
#include <vector>

namespace ps2x::listed
{
    // A run of guest bytes (inside the RAM backing), in chain order.
    struct Piece
    {
        const uint8_t *src;
        uint32_t len;
    };

    // One listed packet in a gathered chain.
    struct Segment
    {
        uint32_t head;       // the packet's first tag (the registry's key)
        uint32_t start, end; // byte offsets in the gathered chain (live: start = end, where its bytes belong)
        uint32_t pieceBegin, pieceEnd; // its bytes as guest RAM, in Segments::pieces
        bool closed;         // false: the chain ended before its tail tag (shadow: end = start, no pieces; live: the
                             // bytes up to the chain's end)
    };

    struct Segments
    {
        std::vector<Segment> segments;
        std::vector<Piece> pieces;
        bool live = false; // the packets' bytes are not in the gathered chain: read them from pieces
        bool empty() const { return segments.empty(); }
        void clear()
        {
            segments.clear();
            pieces.clear();
            live = false;
        }
        size_t listedBytes() const
        {
            size_t n = 0;
            for (const Piece &p : pieces)
                n += p.len;
            return n;
        }
    };

    // The walker's view of a listed packet while it walks the chain.
    struct Mark
    {
        uint32_t head, tail; // first and last tag
        uint32_t start;      // byte offset in the chain before the head tag's bytes
        uint32_t end;        // after the tail tag's bytes; 0 while open
        bool closed;
        uint32_t pieceBegin = 0u, pieceEnd = 0u; // live: its bytes, in the walker's listed pieces
    };

    // The draw list's registry: is the tag at headAddr (headTag = its first 8 bytes as the walker read them) the head of
    // a listed packet, and where is its last tag? Asked for the chain's first tag and for every NEXT tag's target, on the
    // thread that starts the VIF1 DMA (the game thread). nullptr (the default, and always in releases): no marks.
    using Lookup = bool (*)(uint32_t headAddr, uint64_t headTag, uint32_t &tailAddr);
    // live: leave the listed packets' bytes out of the gathered chain (else: copy everything, mark only)
    void setLookup(Lookup lookup, bool live);
    Lookup lookup();
    bool live();

    // The marks of one walk as Segments: each mark's bytes cut out of the walk's pieces (pieces = the walk's, total
    // in chain order; Piece mirrors ps2x::dma::ChainPiece). An open mark (the chain ended inside it): closed = false,
    // no pieces.
    template <class ChainPiece>
    void cutSegments(const std::vector<ChainPiece> &pieces, const std::vector<Mark> &marks, Segments &out)
    {
        out.clear();
        size_t p = 0;
        uint64_t pieceStart = 0; // chain offset of pieces[p]
        for (const Mark &m : marks)
        {
            Segment s{m.head, m.start, m.closed ? m.end : m.start, static_cast<uint32_t>(out.pieces.size()), 0u, m.closed};
            if (m.closed && m.end > m.start)
            {
                while (p < pieces.size() && pieceStart + pieces[p].len <= m.start)
                    pieceStart += pieces[p++].len;
                size_t q = p;
                uint64_t qStart = pieceStart;
                while (q < pieces.size() && qStart < m.end)
                {
                    const uint64_t from = qStart < m.start ? m.start - qStart : 0u;
                    const uint64_t to = qStart + pieces[q].len > m.end ? m.end - qStart : pieces[q].len;
                    out.pieces.push_back(Piece{pieces[q].src + from, static_cast<uint32_t>(to - from)});
                    qStart += pieces[q++].len;
                }
            }
            s.pieceEnd = static_cast<uint32_t>(out.pieces.size());
            out.segments.push_back(s);
        }
    }

    // Live: the marks as Segments, their bytes the walker's listed pieces (ChainPiece as above).
    template <class ChainPiece>
    void takeLive(const std::vector<ChainPiece> &listedPieces, const std::vector<Mark> &marks, Segments &out)
    {
        out.clear();
        out.live = true;
        for (const Mark &m : marks)
        {
            Segment s{m.head, m.start, m.start, static_cast<uint32_t>(out.pieces.size()), 0u, m.closed};
            for (uint32_t i = m.pieceBegin; i < m.pieceEnd && i < listedPieces.size(); ++i)
                out.pieces.push_back(Piece{listedPieces[i].src, static_cast<uint32_t>(listedPieces[i].len)});
            s.pieceEnd = static_cast<uint32_t>(out.pieces.size());
            out.segments.push_back(s);
        }
    }

    // Live: the whole chain in order as pieces (the gathered chain's pieces with each listed packet's put back where it
    // belongs), for tools that want every byte (the chain dump).
    template <class ChainPiece>
    void logicalPieces(const std::vector<ChainPiece> &pieces, const Segments &listed, std::vector<ChainPiece> &out)
    {
        out.clear();
        size_t p = 0;
        uint64_t at = 0;       // chain offset of the next byte of pieces[p]
        uint32_t used = 0;     // bytes of pieces[p] already out
        for (const Segment &s : listed.segments)
        {
            while (p < pieces.size() && at < s.start)
            {
                const uint32_t left = pieces[p].len - used;
                const uint32_t take = at + left <= s.start ? left : static_cast<uint32_t>(s.start - at);
                out.push_back(ChainPiece{pieces[p].src + used, take});
                at += take;
                used += take;
                if (used == pieces[p].len)
                    ++p, used = 0;
            }
            for (uint32_t i = s.pieceBegin; i < s.pieceEnd; ++i)
                out.push_back(ChainPiece{listed.pieces[i].src, listed.pieces[i].len});
        }
        for (; p < pieces.size(); ++p, used = 0)
            out.push_back(ChainPiece{pieces[p].src + used, pieces[p].len - used});
    }
}
