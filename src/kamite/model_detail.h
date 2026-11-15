#pragma once

// Character detail: the full meshes at every distance (no distance LOD).
//
// A character model carries near meshes and distance meshes (nodes named *_mip / *_mi, cMeshInfo.flags +0x34 bit
// 0x01000000). cModel_Trans works out a far flag each frame, the model's distance past its LOD threshold (cModel +0x2B2,
// 180 by default from the constructor at 0x14969C), and passes it in $a2 to the node tagging methods
// cModel_Tag_set_scr (0x14AAB8) and cModel_Tag_set_rate (0x14AC90), which tag only the nodes whose far bit matches it
// (a model with no far nodes gets cModel.objFlags 0x400 at setup and draws every node). The LOD was a PS2 performance
// measure; on a PC the full meshes cost nothing, so the flag is cleared at both entries: the near meshes draw at every
// distance and the distance meshes never do (no LOD swap as characters move away). A model whose nodes are all
// distance meshes keeps the game's choice.
namespace kamite::model_detail
{
    void install();
}
