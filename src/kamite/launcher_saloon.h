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
    void beginPaint(); // the ink's solid colour drawn over (normal blending): things on the paper, not in it
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
        bool showSkip = false;         // "Skip Launcher (hold Shift while starting to see it)" with its tick box
        bool skip = false;
        bool steam = false;            // Steam's mark in the upper left corner (Add to Steam)
        bool steamAdded = false;       // its label when focused: "Update / on Steam", else "Add to / Steam"
        bool volume = false;           // the mute mark in the upper right corner, the volume ruler down the right edge
        float level = 0.8f;            // the volume, 0..1 (the paperclip's place on the ruler)
        bool muted = false;
        int focus = -1;                // billTargets' order: the items, the skip line, the Steam mark, the mute mark
                                       // (-1: none; the ruler's slider takes focus with no loop, K)
        uint32_t focusSeed = 1;        // the loop's own wobble (a new one each move)
        float focusAge = 1.0f;         // seconds since the focus moved (the loop draws itself in)
    };
    // What can be clicked on the bill, in paper units: the items, then the skip line, the Steam mark and the mute mark
    // (when shown).
    std::vector<Rectangle> billTargets(const BillFonts &fonts, const Bill &bill);
    // The volume ruler down the bill's right edge (paper units): the box its slider covers, and the level at a paper y
    // (1 at the top, 0 at the bottom).
    Rectangle billRuler();
    float billRulerLevel(float paperY);
    void drawBill(const BillFonts &fonts, const Bill &bill, const Homography &paper);

    // ---- the look's settings: K's values from the dev tuning panel (Nov 18). The panel (F4, GHR.ini [SaloonTuning])
    // is gone for now; claude/saloon-launcher.md lists its sliders and ranges to bring it back.
    struct Tuning
    {
        float drift = 0.0f;        // the room against the table with the cursor (x parallax_max of the height; 0: both
                                   // still, K Nov 18: the rays, dust, logo and kana carry the depth)
        float tableShare = 0.4f;   // the table's part of it (it goes the other way: the view turns round a point between)
        float logoU = 0.5f;        // the logo's centre in the doorway quad (0..1 across, 0..1 down)
        float logoV = 0.125f;
        float logoWidth = 1.151f;  // the logo's width (GOD HAND + plate) in doorway widths
        float kanaX = 0.036f;      // the kana's offset in logo widths
        float kanaY = 0.035f;
        float kanaDrift = 0.4f;    // the kana's drift with the cursor, out past the doorway (the logo stays put, K)
        float kanaSize = 1.012f;   // the kana's size (x the logo canvas's)
        float kanaTurn = -4.489f;  // the kana's twist, degrees clockwise
        float airDrift = 1.0f;     // the rays' near ends and the dust in the air, against the room
        float burst = 1.2f;        // the speed lines behind the logo (size in logo widths)
        float raysMotion = 1.0f;   // the light rays' beams turning and the fan's swing (like the burst)
        float rays = 1.0f;         // the rays' strength
        float bloom = 1.024f;      // the doorway's (and windows') glow
        float motes = 1.0f;        // the bright motes in the light
        float dust = 1.323f;       // the dust hanging in the air past the table (faint in the dark, bright in the light)
        float vignette = 0.089f;
        float steam = 1.0f;        // the curry's steam (times the player's volume)
        float frost = 1.0f;        // the smoothie glass's frost
        float floatHeight = 0.07f; // how far an eaten snack rises (of the height)
        float zoomFps = 30.0f;     // the Options zoom's frames a second (stepped)
        float ambience = 1.0f;     // the bar's murmur under the music (x the file's level, x the player's volume)
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

        // The bill of fare's paper on screen this frame (with the table's drift); false when it is out of the frame.
        bool bill(Quad &out) const;
        // The open book's pages on screen (TL, TR, BR, BL each), on the zoom's last frame only: the Options book.
        bool spread(Quad &left, Quad &right) const;
        // The logo canvas (logo.png's whole canvas) in the doorway's glare; contentBox: the canvas fractions of
        // GOD HAND + plate (left, top, right, bottom). False when the doorway is out of view.
        bool logo(const float contentBox[4], float canvasAspect, Rectangle &out) const;
        // The kana (resources/images/kana.png, over the logo's canvas): drawn by the scene outside the doorway, behind
        // the doors and the frame (the outside matte), K Nov 18. The texture stays the launcher's.
        void setKana(const Texture2D &kana, const float contentBox[4], float canvasAspect);
        float overlays() const;                   // corner slips, the rights notice: 1 at Home, out through the zoom

        bool bookHit(Vector2 screen) const;
        enum class Bite
        {
            None,
            Fruit,
            Smoothie, // a garnish (an orange slice, the cherry and mint)
            Drink,    // the drink itself (the last sip)
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
        // A plain paper texture (corner slips): 256x256, mipmapped, tiles.
        const Texture2D &paper() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
