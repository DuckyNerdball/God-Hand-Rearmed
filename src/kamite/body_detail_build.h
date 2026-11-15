#pragma once

// The women's detailed bust (Money > 100%) and seat (Cash > 100%), built from the player's disc (claude/female-models.md;
// the offline prototypes are tools/models/body_detail, xfer.transfer3 and transplant.build2, which this follows).
//
// Pure geometry and file layout, no runtime: body_detail.cpp reads the files and installs the result.
//
// A model (scr, tools/models/gamemodel.py) keeps its body in the node named "body": vertex buffers of triangle strips
// (positions f32 x, y, z + a strip flag; int8 normals; int16 UVs; RGBA; three bone weights). The rebuilt body has
// - the bust: the triangles touching bone 29 subdivided once, the new interior pushed along its smoothed normal (straight
//   back near the centre line) onto Olivia's bunny-suit bust (el/elf4.dat), box-mapped to the woman's bone-29 extents,
//   the height smoothed;
// - the seat: the costume piece holding the seat cut in a window behind the hips and Shannon's seat (el/el15.dat
//   body_u1) joined in at her own size (only her hip width matched), blended onto the woman's surface near its edge.
// Every new vertex takes the woman's own UVs, colours and weights, so her textures stay hers.
//
// One topology serves both states: each changed vertex has a flat position (on the woman's original surface: the
// subdivision midpoints on their edges, the seat patch at its closest points on the cut surface) and a detailed one, so
// the shape switches live by rewriting positions and normals in place (body_detail.cpp). The file carries the flat state.
// Cash's growth rides on the detailed seat: a third position per seat-region record (+ its move at 130% x the scale).
// The space comes from the distance meshes (*_mip / *_mi), which never draw since the LOD was dropped (model_detail):
// they become empty nodes and the model keeps its size.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kamite::body_detail
{
    struct D3
    {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    // A node's vertex records as a triangle list (xfer.load_node).
    struct Mesh
    {
        std::vector<D3> P, N;
        std::vector<float> U, V;                 // UVs as stored (int16 units), fractional after interpolation
        std::vector<float> C;                    // 4 per record
        std::vector<int> J;                      // 3 per record: bone indices
        std::vector<float> W;                    // 3 per record: percent
        std::vector<int> VB;                     // the record's vertex buffer
        std::vector<int> T;                      // 3 per triangle: record indices
        std::vector<int> TM;                     // the triangle's buffer material
        size_t records() const { return P.size(); }
        size_t triangles() const { return TM.size(); }
    };

    // The donors' meshes from their files (a dat: entry 1 is the MD).
    bool loadDonor(const uint8_t *dat, size_t size, const char *node, Mesh &out);

    // A record whose position or normal differs between the two states, as offsets from the body mdb's start.
    struct Change
    {
        uint32_t pos = 0; // x, y, z (the strip flag after them is never touched)
        uint32_t nrm = 0; // 3 x int8
        float flat[3] = {}, detail[3] = {};
        int8_t flatN[3] = {}, detailN[3] = {};
        float grow[3] = {};  // Cash: the move at 130% on top of the detailed state (0 outside the growth)
        int8_t growN[3] = {}; // the normal there
        uint8_t region = 0; // 1 bust, 2 seat (the detailed seat and / or Cash's growth)
    };

    struct Built
    {
        std::vector<uint8_t> scr;  // the model rebuilt, zero-padded to the original's size, flat state
        uint32_t bodyMdb = 0;      // the body node's mdb in scr
        uint32_t bodyMdbSize = 0;
        std::vector<Change> changes;
        int sentinel[3] = {-1, -1, -1}; // per region: a change whose position differs between the states
        size_t used = 0;           // bytes before the padding
        int bustAdded = 0, seatPatch = 0, seatShared = 0, cashGrown = 0;
        std::string error;
    };

    // bust = false: her own bust (one already detailed, with attachments on it); seat = false: her own seat (a skirt or
    // a dress over it).
    bool build(const uint8_t *scr, size_t size, const Mesh &bunny, const Mesh &shannon, bool bust, bool seat, Built &out);

    // Cash's growth (tools/models/body_detail/cash.py): every body's seat region grown from the detailed state along a
    // side profile K picked (Nov 15), so Cash above 101% makes the seat fuller (the most in the cheeks' lower half), the
    // hips and upper thighs a little. Linear in Cash: the move at 130% x cashGrowthScale(Cash) (0 at 100%, 1 at 130%).
    float cashGrowthScale(float cash);

    // A change's position in a state (detailed: + grow x its growth), as apply writes it.
    void wantedPosition(const Change &c, bool detailed, float grow, float out[3]);

    // The changes written into an mdb (guest RAM or a file copy): region 1 / 2 to their flat or detailed state; grow:
    // cashGrowthScale for the seat (the normals blended to the 130% ones).
    void apply(uint8_t *mdb, const std::vector<Change> &changes, int region, bool detailed, float grow = 0.0f);
}
