#pragma once

#include <cstdint>

namespace kamite
{
    // Walls and floors next to the camera drawn instead of dropped (near_geometry.cpp): the scenery renderers' VU1
    // clip test patched to the far plane only, and every PATH1 packet's triangles that the GS cannot take (corners
    // off its 16-bit screen range or behind the camera) cut and sent again. [Video] SeeThrough (KAMITE_NEAR_GEOMETRY follows it,
    // on by default), read here; call before the game starts.
    void installNearGeometry();
}
