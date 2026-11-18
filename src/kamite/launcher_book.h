#pragma once

// Options drawn on the open book (the saloon's zoom, its last frame; K Nov 18, claude/saloon-launcher.md "Stage 2").
// The UI is laid out with Dear ImGui in a virtual, screen-aligned rectangle close to where the spread sits on screen, and
// the frame's draw data is mapped onto the pages' trapezoid by a homography when it is rendered: text and shapes in ink
// (multiplied into the paper, as on the bill of fare), paper slips, photos and prompts painted over. The mouse is mapped
// the other way before ImGui sees it.

#include "imgui.h"
#include "raylib.h"

#include "kamite/launcher_saloon.h"

#include <cstdint>

namespace kamite::launcher::book
{
    struct Space
    {
        bool valid = false;
        Rectangle rect{};             // the spread laid flat, in virtual pixels (about its place on screen)
        float spine = 0.0f;           // the spine's x, virtual
        saloon::Homography toScreen;  // virtual -> screen
        saloon::Homography toVirtual; // screen -> virtual
        Vector2 map(Vector2 v) const { return toScreen.map(v); }
        Vector2 unmap(Vector2 s) const { return toVirtual.map(s); }
        float unit() const { return rect.height / 1000.0f; } // a page is 1000 units tall
    };
    // From the pages' screen quads (TL, TR, BR, BL each).
    Space spaceFrom(const saloon::Quad &left, const saloon::Quad &right);

    // In place of rlImGuiBegin / rlImGuiEnd while the book is up.
    void beginFrame(const Space &space);
    void endFrame(const Space &space);

    void setPaperTexture(const Texture2D &paper); // the scene's paper (slips, notes)

    // What follows in a draw list: ink (multiplied into the paper; the default) or paint (drawn over it).
    void ink(ImDrawList *list);
    void paint(ImDrawList *list);

    // Shapes drawn when the frame is rendered, through the homography, in ink (virtual pixels; colours take the style's
    // alpha when recorded): the grease-pencil loop (drawn: 0..1 of it), a hand-ruled tick box, a stroke.
    void loop(ImDrawList *list, ImVec2 min, ImVec2 max, float drawn, uint32_t seed, ImU32 color, float width);
    void tickBox(ImDrawList *list, ImVec2 centre, float size, bool ticked, ImU32 color);
    constexpr int kMaxLinePoints = 24;
    void line(ImDrawList *list, const ImVec2 *points, int count, float width, ImU32 color, float taper = 0.0f);
    // A piece of paper (the scene's texture) on the quad (TL, TR, BR, BL), its rim a little darker, with a soft shadow
    // down and out by `shadow` pixels (0: none). Painted.
    void paper(ImDrawList *list, const ImVec2 quad[4], ImU32 tint, float shadow);
}
