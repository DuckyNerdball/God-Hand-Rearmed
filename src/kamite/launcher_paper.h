#pragma once

// The launcher's newspaper (K, Nov 19; claude/newspaper.md): the version and the updates as "God Hand: Rearmed News".
// In Home's lower right corner the top-left of the paper pokes in, tilted: the masthead and the date line (the
// version); a red EXTRA! stamp and an "OUT NOW" line when an update is out. Opened, it slides up to the middle over the
// dimmed saloon: the release notes in two columns, BACK / UPDATE NOW as ads at its foot. Drawn with raylib alone (two
// page textures, re-rendered when what they show changes); the launcher does the input, the sounds and the update.
#include "kamite/launcher_saloon.h"
#include "raylib.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kamite::launcher::paper
{
    // The page in its own units: x right, y down.
    constexpr float kW = 1000.0f, kH = 1040.0f;

    // One release on the page: its headline ("v0.7.0 — NOV. 24, 2026") and its notes' lines.
    struct Story
    {
        std::string headline;
        std::vector<std::string> bullets;
    };
    // A release's description (markdown-ish lines: "- " bullets; headings, ** and ` are dropped) as a story.
    Story story(const std::string &tag, const std::string &date, const std::string &body);

    struct Content
    {
        std::string edition = "BETA EDITION";
        std::string volume;     // "0.6.9.1"
        std::string date;       // "NOV. 19, 2026", a dev build's commit, or "" (offline)
        std::string outNow;     // "v0.7.0": an update is out (the red stamp and line)
        bool latest = false;    // the open page's date line adds "LATEST EDITION"
        std::vector<Story> stories;
        std::string notice;     // instead of stories: "The wire's down. No news today." and the like
        bool updateAd = false;  // UPDATE NOW beside BACK
        float printing = -1.0f; // >= 0: the update ad reads PRINTING... with a bar (0..1)
        bool printingKnown = true; // false: the size isn't known (no percent, the bar runs)
        std::string stop;       // a failed update: the STOP THE PRESSES box's reason
        int focus = -1;         // the pencil loop: 0 BACK, 1 UPDATE NOW
        float scroll = 0.0f;    // page units the stories are scrolled down
    };

    // Where the page is on screen.
    struct View
    {
        saloon::Quad quad;            // TL, TR, BR, BL
        saloon::Homography map;       // page units -> screen
    };

    class Paper
    {
    public:
        // fonts: resources/fonts; ears: the font of the open page's two boxes beside the masthead (Dominican).
        bool load(const std::filesystem::path &fonts, const Font *ears);
        void unload();
        bool loaded() const { return m_loaded; }
        // t: 0 in the corner .. 1 open (eased here); alpha: the corner's fade (the overlays' at Home). The open page
        // dims what is under it.
        void draw(const Content &content, float t, float alpha);
        View view(float t, bool update) const;
        // Page units: the corner's visible part (its click and focus box); an ad (0 BACK, 1 UPDATE NOW); the page.
        Rectangle cornerBox(bool update) const;
        Rectangle adBox(const Content &content, int which) const;
        // Page units the stories can scroll (after the last open render).
        float maxScroll() const { return m_maxScroll; }

    private:
        struct Fonts
        {
            Font masthead{}, narrow{}, body{}, wood{}, west{};
        };
        void renderCorner(const Content &content, int w, int h);
        void renderPage(const Content &content, int w, int h);
        float layoutStories(const Content &content, float top, bool draw, float scale);

        bool m_loaded = false;
        Fonts m_fonts;
        const Font *m_ears = nullptr;
        Texture2D m_newsprint{};
        RenderTexture2D m_corner{}, m_page{};
        std::string m_cornerKey, m_pageKey;
        float m_maxScroll = 0.0f;
    };
}
