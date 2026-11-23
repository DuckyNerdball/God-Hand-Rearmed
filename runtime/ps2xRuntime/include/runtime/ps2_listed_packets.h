#pragma once

// Native road rung 3, step b: the draw list (claude/native-next.md "Step b"). A native packet builder registers the
// PS2 packets it wrote ("listed" packets: kamite's draw list keeps them by head address). The DMA chain walker marks
// where each listed packet sits in the VIF1 chain it gathers, and the VU worker hands those marks to the draw list's
// observer before and after it runs the chain. Today that is the dev builds' shadow check (does each listed packet's
// VIF stream start and end on its own edges, and are its bytes in guest RAM still the ones the walker copied when the
// VU thread runs?); the live list reads those packets in place instead of copying them. Nothing here changes what the
// VU or the GS receive. Small on purpose: ps2_memory.h, ps2_dma_chain.h and ps2_async_vif.h include it.

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
        uint32_t start, end; // byte offsets in the gathered chain
        uint32_t pieceBegin, pieceEnd; // its bytes as guest RAM, in Segments::pieces
        bool closed;         // false: the chain ended before its tail tag (end = start, no pieces)
    };

    struct Segments
    {
        std::vector<Segment> segments;
        std::vector<Piece> pieces;
        bool empty() const { return segments.empty(); }
        void clear()
        {
            segments.clear();
            pieces.clear();
        }
    };

    // The walker's view of a listed packet while it walks the chain.
    struct Mark
    {
        uint32_t head, tail; // first and last tag
        uint32_t start;      // byte offset in the chain before the head tag's bytes
        uint32_t end;        // after the tail tag's bytes; 0 while open
        bool closed;
    };

    // The draw list's registry: is the tag at headAddr (headTag = its first 8 bytes as the walker read them) the head of
    // a listed packet, and where is its last tag? Asked for the chain's first tag and for every NEXT tag's target, on the
    // thread that starts the VIF1 DMA (the game thread). nullptr (the default, and always in releases): no marks.
    using Lookup = bool (*)(uint32_t headAddr, uint64_t headTag, uint32_t &tailAddr);
    void setLookup(Lookup lookup);
    Lookup lookup();

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
}
