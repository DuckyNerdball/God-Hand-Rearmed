#pragma once

// The launcher's saloon (claude/saloon-launcher.md, stage 1): a pre-rendered saloon seen from a table, its layers
// (resources/saloon: saloon_layers.json + AVIF images) put back together with life on top: the room drifting with the
// cursor, the batwing doors swaying (and flung open by Play), light rays, the doorway's bloom, dust motes, the curry's
// steam, the smoothie's frost, the snacks (an egg: click them), the lit book, and the Options zoom down to the book
// (stepped pre-rendered frames, reversed by Back). The launcher puts its menu on the bill of fare (ink through the
// bill's perspective, the helpers below) and its logo in the doorway's glare.
//
// Frame coordinates: the 32:9 master (5120x1440 px), x right, y down; quads are normalised to it. The picture is
// scaled by the window's height and centred on the doors (4:3 to 32:9 fill the window; narrower: scaled to the width,
// bars above and below; wider: bars at the sides).
#include "raylib.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::launcher::saloon
{
    // A quad: top left, top right, bottom right, bottom left.
    struct Quad
    {
        Vector2 p[4]{};
    };

    // A perspective map (homography) from a rectangle's own units onto a quad, and back.
    struct Homography
    {
        double m[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};
        // (0, 0) -> TL, (w, 0) -> TR, (w, h) -> BR, (0, h) -> BL.
        static Homography rectToQuad(float w, float h, const Quad &quad);
        Vector2 map(float x, float y) const;
        Vector2 map(Vector2 p) const { return map(p.x, p.y); }
        Homography inverse() const;
    };
    bool inside(const Quad &quad, Vector2 point);

    // The bill of fare's paper in its own units (the texture the set was rendered with): x right, y down.
    constexpr float kPaperW = 700.0f, kPaperH = 1000.0f;

    // ---- ink on paper (through a homography; drawn with raylib, multiplied into what is under it) -----------------
    // Text whose box is centred on `centre` (paper units); returns its width. spacing: extra paper units a letter.
    float inkText(const Font &font, const char *text, Vector2 centre, float size, Color color, const Homography &paper,
                  float spacing = 0.0f);
    float inkWidth(const Font &font, const char *text, float size, float spacing = 0.0f);
    // A pen line through points (paper units), `width` wide, its ends tapered over `taper` of the length.
    void inkLine(const Vector2 *points, int count, float width, Color color, const Homography &paper, float taper = 0.0f);
    // The focus mark: a red grease-pencil loop round a box (paper units), drawn in as `drawn` goes 0..1. seed: its
    // own wobble (a new one each time the focus moves).
    void inkLoop(Rectangle box, float drawn, uint32_t seed, Color color, const Homography &paper, float width);
    // A tick box (inked square) and its tick, `size` paper units.
    void inkTickBox(Vector2 centre, float size, bool ticked, Color color, const Homography &paper);
    // Multiply blending for ink (paper shows through); End restores normal blending.
    void beginInk();
    void endInk();

    // ---- the bill of fare: Home's menu in ink -----------------------------------------------------------------------
    struct BillFonts
    {
        Font heading{}; // Rye (the bill's title)
        Font item{};    // EB Garamond, heavier (the menu)
        Font text{};    // EB Garamond (the splash line, the tagline, the small print)
    };
    // resources/fonts; extra: text whose letters must be in the atlases (the splash line). False if one is missing.
    bool loadBillFonts(BillFonts &fonts, const std::filesystem::path &folder, const std::string &extra);
    void unloadBillFonts(BillFonts &fonts);

    struct Bill
    {
        std::string splash;            // this start's line ("" for none)
        std::vector<std::string> items; // Play (Install), Options, Quit
        bool showDontShow = false;     // the last line: "Don't show this again..." with its tick box
        bool dontShow = false;
        int focus = -1;                // an item, or items.size() for the last line; -1: none
        uint32_t focusSeed = 1;        // the loop's own wobble (a new one each move)
        float focusAge = 1.0f;         // seconds since the focus moved (the loop draws itself in)
    };
    // What can be clicked on the bill, in paper units: the items, then the last line (when shown).
    std::vector<Rectangle> billTargets(const BillFonts &fonts, const Bill &bill);
    void drawBill(const BillFonts &fonts, const Bill &bill, const Homography &paper);

    // ---- the dev tuning panel's values (F3; GHR.ini [SaloonTuning]; the defaults once K is happy) -------------------
    struct Tuning
    {
        float drift = 1.0f;        // the room's drift with the cursor (x parallax_max of the height)
        float logoU = 0.5f;        // the logo's centre in the doorway quad (0..1 across, 0..1 down)
        float logoV = 0.125f;
        float logoWidth = 1.19f;   // the logo's width (GOD HAND + plate) in doorway widths
        float logoDrift = 1.0f;    // how much of the room's drift the logo follows
        float kanaX = 0.0f;        // the kana's offset in logo widths
        float kanaY = 0.0f;
        float kanaDrift = 0.4f;    // the kana's own extra drift (x the room's)
        float burst = 0.8f;        // the speed lines behind the logo (size in logo widths)
        float raysWarp = 0.5f;     // the light rays' slow sway and shimmer
        float rays = 1.0f;         // the rays' strength
        float bloom = 0.6f;        // the doorway's (and windows') glow
        float motes = 1.0f;        // the dust in the light
        float vignette = 0.22f;
        float steam = 1.0f;        // the curry's steam (times the player's volume)
        float frost = 1.0f;        // the smoothie glass's frost
        float floatHeight = 0.07f; // how far an eaten snack rises (of the height)
    };

    class Scene
    {
    public:
        Scene();
        ~Scene();
        Scene(const Scene &) = delete;
        Scene &operator=(const Scene &) = delete;

        // resources/saloon: reads the layout and starts decoding on worker threads (Home first, the zoom after).
        // False when the pack is missing or this build has no AVIF decoder (the launcher then stays plain).
        bool open(const std::filesystem::path &folder);
        bool failed() const;      // an image failed to load: the launcher falls back to its plain look
        bool ready() const;       // Home's layers are up
        void finishLoading();     // waits for every image (the zoom's, when Options comes before they are ready)
        void close();             // textures and threads (before the window goes to the game)

        struct Frame
        {
            float dt = 0.0f;
            Vector2 parallax{0.0f, 0.0f}; // -1..1 each way, eased (the room leans toward the cursor)
            float volume = 0.0f;          // the player's volume, 0 when muted: the curry's steam
            bool bookLit = false;         // Options has the focus: the book catches the light
        };
        void update(const Frame &frame); // main thread, once a frame: uploads, clocks, the doors, the zoom
        void draw(float dim);            // the picture (black outside it); dim: 0..1 darker (Install, first run)

        // This frame's picture on screen.
        Rectangle picture() const;
        float scale() const; // screen pixels per frame pixel
        Vector2 toScreen(Vector2 normalised) const;

        // The bill of fare's paper on screen this frame; false when it is out of the frame (deep in the zoom).
        bool bill(Quad &out) const;
        // The logo canvas (logo.png's whole canvas) in the doorway's glare; contentBox: the canvas fractions of
        // GOD HAND + plate (left, top, right, bottom). False when the doorway is out of view.
        bool logo(const float contentBox[4], float canvasAspect, Rectangle &out) const;
        Vector2 kanaShift(float logoWidth) const; // the kana's extra drift, screen pixels
        float overlays() const;                   // corner slips, the rights notice: 1 at Home, out through the zoom

        bool bookHit(Vector2 screen) const;
        enum class Bite
        {
            None,
            Fruit,
            Smoothie,
        };
        // A click at Home: a snack under it is eaten (floats up). None: nothing there, or one is still floating.
        Bite snackClick(Vector2 screen);
        bool snackHit(Vector2 screen) const;

        // The Options zoom: 8 frames at 12 a second, stepped; Back reverses from the frame showing.
        bool zoomReady() const;
        bool zoomBroken() const; // its images failed: Options opens without it
        void zoomIn();
        void zoomOut();
        int zoomFrame() const;   // 0 = Home, 1..8
        bool zoomMoving() const; // stepping either way
        bool zoomingIn() const;
        bool atHome() const;     // frame 0, still
        bool landed() const;     // frame 8, still
        // Play: the doors fling outward (8 frames at 24 a second) while the launcher fades out.
        void fling();

        Tuning &tuning();
        const Tuning &tuning() const;
        void loadTuning(const IniFile &ini);
        void saveTuning(IniFile &ini) const;
        // The dev panel (an ImGui window; F3 in a dev build). Returns false once closed.
        bool drawTuning(IniFile &ini);
        // A plain paper texture (corner slips): 256x256, mipmapped, tiles.
        const Texture2D &paper() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
