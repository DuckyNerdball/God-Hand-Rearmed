#pragma once

class PS2Runtime;

// The women's detailed bust (Money above 100%) and seat (Cash above 100%): the geometry is in body_detail_build.h.
//
// Boot (the disc mounted, before the game runs): every listed woman's model (body_detail_models.inc) is read from the
// player's disc, rebuilt with the extra geometry in the space of its distance meshes, and written over the original
// through a same-size disc overlay, in the flat state (her original shape). A block whose bytes differ from the list's
// is left alone (another disc).
//
// Live: each time a model is packed for drawing (cModel_Trans' first cModel_getMeshPtr call, 0x14B6F8 from 0x14A4C0),
// its "body" node's mdb (node +0x388) is matched against the rebuilt bodies (bytes no state changes), and the bust /
// seat vertices are rewritten in place when they are not in the state Money / Cash ask for. The game's draw packets
// point at the mdb's vertex arrays (DMA ref tags, func_00154E98), so the next draw shows the new shape. Nothing is kept
// about guest RAM: a save state or a newly loaded model is put right the first time it is drawn.
namespace kamite::body_detail
{
    void install(PS2Runtime &runtime);
}
