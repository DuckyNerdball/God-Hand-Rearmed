// The launcher's newspaper (see launcher_paper.h; claude/newspaper.md).
#include "kamite/launcher_paper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace kamite::launcher::paper
{
    namespace
    {
        constexpr Color kPaperColour{226, 220, 202, 255};
        constexpr Color kInk{28, 24, 20, 255};
        constexpr Color kInkSoft{70, 64, 56, 215};
        constexpr Color kRed{178, 28, 22, 255};
        constexpr Color kCream{246, 236, 214, 255};
        constexpr float kMargin = 44.0f;
        // The page's parts (page units).
        constexpr float kBodyTop = 168.0f, kBodyBottom = 832.0f, kAdsRule = 846.0f, kAdTop = 866.0f, kAdBottom = 1004.0f;
        // The corner (K, Nov 19: A, the top-left corner tilted; low, so it covers little of the render) and the open page.
        constexpr float kCornerAngle = -4.0f; // raylib's degrees (clockwise): the paper rises to the right
        constexpr float kCornerScale = 0.40f; // screen pixels a page unit at 900 lines
        constexpr float kCornerShown = 0.80f; // of the page's width on screen
        constexpr float kOpenAngle = 0.8f;

        struct Placement
        {
            Vector2 centre{};
            float scale = 1.0f; // screen pixels a page unit
            float angle = 0.0f; // degrees, raylib's
        };

        Vector2 rotate(Vector2 v, float degrees)
        {
            const float a = degrees * DEG2RAD, c = std::cos(a), s = std::sin(a);
            return Vector2{v.x * c - v.y * s, v.x * s + v.y * c};
        }

        float smoothstep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        float easeInOut(float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
        }

        std::vector<int> codepoints()
        {
            std::vector<int> out;
            for (int c = 32; c < 127; ++c)
                out.push_back(c);
            for (int c = 0xA0; c <= 0xFF; ++c)
                out.push_back(c);
            for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x00D7})
                out.push_back(c);
            return out;
        }

        Font loadFont(const std::filesystem::path &path, int size)
        {
            std::vector<int> cps = codepoints();
            const std::u8string utf8 = path.u8string();
            Font font = LoadFontEx(reinterpret_cast<const char *>(utf8.c_str()), size, cps.data(), static_cast<int>(cps.size()));
            if (font.texture.id != 0u)
            {
                GenTextureMipmaps(&font.texture);
                SetTextureFilter(font.texture, TEXTURE_FILTER_TRILINEAR);
            }
            return font;
        }

        float measure(const Font &font, const std::string &text, float size)
        {
            return MeasureTextEx(font, text.c_str(), size, 0.0f).x;
        }

        // Text in page units: x by align (0 left, 1 centre, 2 right), y its centre line; k: pixels a unit.
        void text(const Font &font, const std::string &s, float x, float y, float size, Color colour, int align, float k)
        {
            const float px = size * k;
            const float w = MeasureTextEx(font, s.c_str(), px, 0.0f).x;
            const float left = x * k - (align == 1 ? w * 0.5f : align == 2 ? w : 0.0f);
            DrawTextEx(font, s.c_str(), Vector2{std::round(left), std::round(y * k - px * 0.5f)}, px, 0.0f, colour);
        }

        // The size (up to `size`) that keeps `s` within `width` units.
        float fit(const Font &font, const std::string &s, float size, float width)
        {
            const float w = measure(font, s, size);
            return w > width && w > 0.0f ? size * width / w : size;
        }

        std::vector<std::string> wrap(const Font &font, const std::string &s, float size, float width)
        {
            std::vector<std::string> out;
            std::string line;
            std::istringstream words(s);
            std::string word;
            while (words >> word)
            {
                const std::string next = line.empty() ? word : line + " " + word;
                if (!line.empty() && measure(font, next, size) > width)
                {
                    out.push_back(line);
                    line = word;
                }
                else
                    line = next;
            }
            if (!line.empty())
                out.push_back(line);
            return out;
        }

        void rule(float x0, float x1, float y, float thick, float k, Color colour = kInk)
        {
            DrawRectangleRec(Rectangle{x0 * k, y * k, (x1 - x0) * k, std::max(1.0f, thick * k)}, colour);
        }

        void box(Rectangle r, float thick, float k, Color colour = kInk)
        {
            DrawRectangleLinesEx(Rectangle{r.x * k, r.y * k, r.width * k, r.height * k}, std::max(1.0f, thick * k), colour);
        }

        uint32_t hash(uint32_t x)
        {
            x ^= x >> 16;
            x *= 0x7FEB352Du;
            x ^= x >> 15;
            x *= 0x846CA68Bu;
            x ^= x >> 16;
            return x;
        }
        float random01(uint32_t &state)
        {
            state = hash(state + 0x9E3779B9u);
            return static_cast<float>(state & 0xFFFFFFu) / 16777215.0f;
        }

        // The pencil loop round a box (page units): an uneven oval, its ends overlapping.
        void loop(Rectangle r, Color colour, uint32_t seed, float k)
        {
            const float cx = r.x + r.width * 0.5f, cy = r.y + r.height * 0.5f;
            const float rx = r.width * 0.5f + 13.0f, ry = r.height * 0.5f + 10.0f;
            uint32_t s = seed;
            const float wob = random01(s) * 6.28f;
            Vector2 last{};
            constexpr int kSteps = 120;
            for (int i = 0; i <= kSteps; ++i)
            {
                const float u = static_cast<float>(i) / kSteps;
                const float a = -0.5f + u * 6.6f; // a little past a full turn
                const float wobble = 1.0f + 0.025f * std::sin(a * 3.0f + wob) + 0.03f * u;
                const Vector2 p{(cx + rx * wobble * std::cos(a)) * k, (cy + ry * wobble * std::sin(a)) * k};
                if (i > 0)
                    DrawLineEx(last, p, 4.0f * k, colour);
                DrawCircleV(p, 2.0f * k, colour);
                last = p;
            }
        }

        // A rubber stamp: the word in a rounded frame, worn (paper-coloured specks).
        void stamp(const Font &font, const char *word, Vector2 centre, float size, float angle, float k)
        {
            Color red = kRed;
            red.a = 225;
            const float px = size * k;
            const Vector2 extent = MeasureTextEx(font, word, px, 0.0f);
            DrawTextPro(font, word, Vector2{centre.x * k, centre.y * k}, Vector2{extent.x * 0.5f, extent.y * 0.5f}, angle, px, 0.0f, red);
            const float hw = extent.x * 0.5f + size * 0.32f * k, hh = extent.y * 0.5f + size * 0.12f * k;
            const Vector2 c{centre.x * k, centre.y * k};
            const Vector2 corners[4] = {rotate({-hw, -hh}, angle), rotate({hw, -hh}, angle), rotate({hw, hh}, angle), rotate({-hw, hh}, angle)};
            for (int i = 0; i < 4; ++i)
            {
                const Vector2 a{c.x + corners[i].x, c.y + corners[i].y}, b{c.x + corners[(i + 1) % 4].x, c.y + corners[(i + 1) % 4].y};
                DrawLineEx(a, b, size * 0.09f * k, red);
            }
            uint32_t s = 0x51A3Fu;
            Color speck = kPaperColour;
            for (int i = 0; i < 160; ++i)
            {
                const Vector2 local{(random01(s) * 2.0f - 1.0f) * hw, (random01(s) * 2.0f - 1.0f) * hh};
                const Vector2 p = rotate(local, angle);
                speck.a = static_cast<unsigned char>(90 + 120 * random01(s));
                DrawCircleV(Vector2{c.x + p.x, c.y + p.y}, (0.4f + 1.4f * random01(s)) * size * 0.03f * k, speck);
            }
        }

        // Small newsprint under the corner's headings: columns of words.
        void filler(const Font &head, const Font &body, float x0, float x1, float y0, float y1, float k)
        {
            static const char *const kWords[] = {"brawl", "hold", "the", "saloon", "roulette", "reel", "gene", "patch", "a",
                                                 "of", "fresh", "press", "stun", "olivia", "demon", "free", "frames", "ring",
                                                 "smooth", "edition", "launcher", "god", "hand", "rearmed", "and", "to",
                                                 "tension", "unleash", "camera", "wide", "news", "town", "mayor", "sheriff"};
            constexpr int kCols = 3;
            const float gap = 16.0f, colW = (x1 - x0 - gap * (kCols - 1)) / kCols, size = 15.0f, line = 17.0f;
            uint32_t s = 0xC0FFEEu;
            for (int c = 0; c < kCols; ++c)
            {
                const float cx = x0 + c * (colW + gap);
                if (c > 0)
                    DrawRectangleRec(Rectangle{(cx - gap * 0.5f) * k, y0 * k, std::max(1.0f, 0.8f * k), (y1 - y0) * k}, Color{60, 55, 48, 140});
                float y = y0;
                bool heading = c != 1;
                while (y < y1)
                {
                    if (heading)
                    {
                        std::string words;
                        for (int i = 0; i < 3; ++i)
                            words += std::string(i ? " " : "") + kWords[static_cast<size_t>(random01(s) * std::size(kWords)) % std::size(kWords)];
                        for (char &ch : words)
                            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        text(head, words, cx, y + 10.0f, 19.0f, Color{40, 36, 30, 230}, 0, k);
                        y += 26.0f;
                        heading = false;
                        continue;
                    }
                    std::string l;
                    for (;;)
                    {
                        const std::string w = kWords[static_cast<size_t>(random01(s) * std::size(kWords)) % std::size(kWords)];
                        const std::string next = l.empty() ? w : l + " " + w;
                        if (measure(body, next, size) > colW)
                            break;
                        l = next;
                    }
                    text(body, l, cx, y + line * 0.5f, size, kInkSoft, 0, k);
                    y += line;
                    if (random01(s) < 0.05f)
                    {
                        y += line;
                        heading = true;
                    }
                }
            }
        }

        Placement cornerPlacement(bool update, Rectangle shownBox)
        {
            const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
            (void)update;
            Placement p;
            p.scale = kCornerScale * sh / 900.0f;
            p.angle = kCornerAngle;
            // the page's top-left corner, so that its masthead and date line show above the screen's bottom edge
            const Vector2 tl{sw - kCornerShown * kW * p.scale, sh - (shownBox.height + 10.0f) * p.scale};
            const Vector2 half = rotate(Vector2{kW * p.scale * 0.5f, kH * p.scale * 0.5f}, p.angle);
            p.centre = Vector2{tl.x + half.x, tl.y + half.y};
            return p;
        }

        Placement openPlacement()
        {
            const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
            Placement p;
            p.scale = std::min(0.94f * sh / kH, 0.92f * sw / kW);
            p.angle = kOpenAngle;
            p.centre = Vector2{sw * 0.5f, sh * 0.5f + sh * 0.006f};
            return p;
        }

        Placement blend(const Placement &a, const Placement &b, float e)
        {
            Placement p;
            p.centre = Vector2{a.centre.x + (b.centre.x - a.centre.x) * e, a.centre.y + (b.centre.y - a.centre.y) * e};
            p.scale = a.scale + (b.scale - a.scale) * e;
            p.angle = a.angle + (b.angle - a.angle) * e;
            return p;
        }

        View viewOf(const Placement &p)
        {
            View v;
            const float hw = kW * p.scale * 0.5f, hh = kH * p.scale * 0.5f;
            const Vector2 local[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            for (int i = 0; i < 4; ++i)
            {
                const Vector2 r = rotate(local[i], p.angle);
                v.quad.p[i] = Vector2{p.centre.x + r.x, p.centre.y + r.y};
            }
            v.map = saloon::Homography::rectToQuad(kW, kH, v.quad);
            return v;
        }

        void drawPage(const RenderTexture2D &target, const Placement &p, float alpha)
        {
            const float w = kW * p.scale, h = kH * p.scale;
            const Color tint{255, 255, 255, static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
            DrawTexturePro(target.texture, Rectangle{0.0f, 0.0f, static_cast<float>(target.texture.width), -static_cast<float>(target.texture.height)},
                           Rectangle{p.centre.x, p.centre.y, w, h}, Vector2{w * 0.5f, h * 0.5f}, p.angle, tint);
        }

        void shadow(const Placement &p, float alpha)
        {
            const float w = kW * p.scale, h = kH * p.scale, sh = static_cast<float>(GetScreenHeight());
            for (int ring = 3; ring >= 1; --ring)
            {
                const float grow = sh * 0.003f * static_cast<float>(ring);
                const Color shade{0, 0, 0, static_cast<unsigned char>(std::clamp(alpha * 0.11f, 0.0f, 1.0f) * 255.0f)};
                DrawRectanglePro(Rectangle{p.centre.x + sh * 0.004f, p.centre.y + sh * 0.008f, w + grow * 2.0f, h + grow * 2.0f},
                                 Vector2{w * 0.5f + grow, h * 0.5f + grow}, p.angle, shade);
            }
        }

        void newsprintFill(const Texture2D &tile, float w, float h)
        {
            if (tile.id != 0u)
                DrawTexturePro(tile, Rectangle{0.0f, 0.0f, w * 0.75f, h * 0.75f}, Rectangle{0.0f, 0.0f, w, h}, Vector2{}, 0.0f, WHITE);
            const float edge = std::max(6.0f, w * 0.03f);
            const Color dark{60, 46, 20, 34}, clear{60, 46, 20, 0};
            DrawRectangleGradientH(0, 0, static_cast<int>(edge), static_cast<int>(h), dark, clear);
            DrawRectangleGradientH(static_cast<int>(w - edge), 0, static_cast<int>(edge), static_cast<int>(h), clear, dark);
            DrawRectangleGradientV(0, 0, static_cast<int>(w), static_cast<int>(edge), dark, clear);
            DrawRectangleGradientV(0, static_cast<int>(h - edge), static_cast<int>(w), static_cast<int>(edge), clear, dark);
        }

        std::string trim(std::string s)
        {
            const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!s.empty() && space(static_cast<unsigned char>(s.back())))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && space(static_cast<unsigned char>(s[i])))
                ++i;
            return s.substr(i);
        }
    }

    Story story(const std::string &tag, const std::string &date, const std::string &body)
    {
        Story out;
        out.headline = date.empty() ? tag : tag + " \xE2\x80\x94 " + date;
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue; // (a dated heading copied from the CHANGELOG)
            if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0)
                line = trim(line.substr(2));
            for (const char *mark : {"**", "`"})
                for (size_t at; (at = line.find(mark)) != std::string::npos;)
                    line.erase(at, std::char_traits<char>::length(mark));
            if (!line.empty())
                out.bullets.push_back(line);
        }
        return out;
    }

    bool Paper::load(const std::filesystem::path &fonts, const Font *ears)
    {
        unload();
        const float big = std::clamp(static_cast<float>(GetScreenHeight()) / 1080.0f, 1.0f, 2.0f);
        m_fonts.masthead = loadFont(fonts / "UnifrakturMaguntia-Book.ttf", static_cast<int>(104 * big));
        m_fonts.narrow = loadFont(fonts / "ArchivoNarrow-SemiBold.ttf", static_cast<int>(64 * big));
        m_fonts.body = loadFont(fonts / "EBGaramond-Regular.ttf", static_cast<int>(56 * big));
        m_fonts.wood = loadFont(fonts / "HoltwoodOneSC-Regular.ttf", static_cast<int>(72 * big));
        m_fonts.west = loadFont(fonts / "Wendyville.ttf", static_cast<int>(84 * big));
        m_ears = ears;
        for (const Font *f : {&m_fonts.masthead, &m_fonts.narrow, &m_fonts.body, &m_fonts.wood, &m_fonts.west})
            if (f->texture.id == 0u)
            {
                unload();
                return false;
            }
        // newsprint: a grey-cream with soft blotches and a fine grain (tiles)
        constexpr int kSize = 256;
        Image image = GenImageColor(kSize, kSize, kPaperColour);
        auto *px = static_cast<Color *>(image.data);
        const auto noise = [](int x, int y, int s) { return static_cast<float>(hash(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(s) * 83492791u) & 0xFFFFu) / 65535.0f; };
        const auto smooth = [&](float x, float y, int period, int s) {
            const float fx = x * period / kSize, fy = y * period / kSize;
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - x0, ty = fy - y0, ux = tx * tx * (3 - 2 * tx), uy = ty * ty * (3 - 2 * ty);
            const auto h = [&](int xx, int yy) { return noise(((xx % period) + period) % period, ((yy % period) + period) % period, s); };
            return (h(x0, y0) * (1 - ux) + h(x0 + 1, y0) * ux) * (1 - uy) + (h(x0, y0 + 1) * (1 - ux) + h(x0 + 1, y0 + 1) * ux) * uy;
        };
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
            {
                const float blot = smooth(static_cast<float>(x), static_cast<float>(y), 4, 1) * 0.6f + smooth(static_cast<float>(x), static_cast<float>(y), 12, 2) * 0.4f;
                const float k = 0.955f + 0.06f * blot - 0.035f * noise(x, y, 3);
                px[y * kSize + x] = Color{static_cast<unsigned char>(std::clamp(226.0f * k, 0.0f, 255.0f)),
                                          static_cast<unsigned char>(std::clamp(220.0f * k, 0.0f, 255.0f)),
                                          static_cast<unsigned char>(std::clamp(200.0f * k, 0.0f, 255.0f)), 255};
            }
        m_newsprint = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureWrap(m_newsprint, TEXTURE_WRAP_REPEAT);
        SetTextureFilter(m_newsprint, TEXTURE_FILTER_BILINEAR);
        m_loaded = true;
        return true;
    }

    void Paper::unload()
    {
        for (Font *f : {&m_fonts.masthead, &m_fonts.narrow, &m_fonts.body, &m_fonts.wood, &m_fonts.west})
            if (f->texture.id != 0u)
            {
                UnloadFont(*f);
                *f = Font{};
            }
        if (m_newsprint.id != 0u)
            UnloadTexture(m_newsprint);
        m_newsprint = Texture2D{};
        for (RenderTexture2D *t : {&m_corner, &m_page})
            if (t->id != 0u)
            {
                UnloadRenderTexture(*t);
                *t = RenderTexture2D{};
            }
        m_cornerKey.clear();
        m_pageKey.clear();
        m_loaded = false;
    }

    Rectangle Paper::cornerBox(bool update) const
    {
        return Rectangle{0.0f, 0.0f, kCornerShown * kW, update ? 206.0f : 170.0f};
    }

    Rectangle Paper::adBox(const Content &content, int which) const
    {
        if (!content.updateAd)
            return Rectangle{kW * 0.5f - 190.0f, kAdTop, 380.0f, kAdBottom - kAdTop};
        return which == 0 ? Rectangle{kMargin + 30.0f, kAdTop, 360.0f, kAdBottom - kAdTop}
                          : Rectangle{kW - kMargin - 30.0f - 410.0f, kAdTop, 410.0f, kAdBottom - kAdTop};
    }

    View Paper::view(float t, bool update) const
    {
        const float e = easeInOut(t);
        return viewOf(blend(cornerPlacement(update, cornerBox(update)), openPlacement(), e));
    }

    void Paper::renderCorner(const Content &c, int w, int h)
    {
        if (m_corner.id == 0u || m_corner.texture.width != w || m_corner.texture.height != h)
        {
            if (m_corner.id != 0u)
                UnloadRenderTexture(m_corner);
            m_corner = LoadRenderTexture(w, h);
            SetTextureFilter(m_corner.texture, TEXTURE_FILTER_BILINEAR);
        }
        const float k = static_cast<float>(w) / kW;
        const bool update = !c.outNow.empty();
        BeginTextureMode(m_corner);
        ClearBackground(kPaperColour);
        newsprintFill(m_newsprint, static_cast<float>(w), static_cast<float>(h));
        const float m = 36.0f;
        const std::string mast = "God Hand: Rearmed News";
        text(m_fonts.masthead, mast, m, 62.0f, fit(m_fonts.masthead, mast, 84.0f, 572.0f), kInk, 0, k);
        rule(m - 6.0f, kW, 110.0f, 3.5f, k);
        rule(m - 6.0f, kW, 116.0f, 1.0f, k);
        std::string line = c.edition + "  \xC2\xB7  VOL. " + c.volume;
        if (!c.date.empty())
            line += "  \xC2\xB7  " + c.date;
        text(m_fonts.narrow, line, m, 138.0f, fit(m_fonts.narrow, line, 34.0f, kCornerShown * kW - m - 20.0f), kInk, 0, k);
        rule(m - 6.0f, kW, 158.0f, 1.0f, k);
        float fillerTop = 172.0f;
        if (update)
        {
            const std::string out = c.outNow + " OUT NOW  \xE2\x80\x94  READ ALL ABOUT IT!";
            text(m_fonts.narrow, out, m, 184.0f, fit(m_fonts.narrow, out, 32.0f, kCornerShown * kW - m - 20.0f), kRed, 0, k);
            fillerTop = 208.0f;
        }
        filler(m_fonts.narrow, m_fonts.body, m, kW - m, fillerTop, kH, k);
        if (update)
            stamp(m_fonts.wood, "EXTRA!", Vector2{698.0f, 76.0f}, 38.0f, -9.0f, k);
        EndTextureMode();
    }

    float Paper::layoutStories(const Content &c, float top, bool draw, float k)
    {
        const Fonts &F = m_fonts;
        float y = top;
        if (!c.stop.empty())
        {
            // a failed update: the box with the reason, instead of the stories
            const float bw = 720.0f, x0 = kW * 0.5f - bw * 0.5f;
            const std::vector<std::string> reason = wrap(F.body, c.stop, 23.0f, bw - 60.0f);
            const float bh = 110.0f + 30.0f * static_cast<float>(reason.size()) + 44.0f;
            if (draw)
            {
                box(Rectangle{x0, y + 10.0f, bw, bh}, 4.0f, k, kRed);
                box(Rectangle{x0 + 8.0f, y + 18.0f, bw - 16.0f, bh - 16.0f}, 1.2f, k, kRed);
                text(F.wood, "STOP THE PRESSES!", kW * 0.5f, y + 62.0f, 40.0f, kRed, 1, k);
                float ly = y + 112.0f;
                for (const std::string &l : reason)
                {
                    text(F.body, l, kW * 0.5f, ly, 23.0f, kInk, 1, k);
                    ly += 30.0f;
                }
                text(F.body, "Nothing was changed.", kW * 0.5f, ly + 8.0f, 23.0f, kInk, 1, k);
            }
            return bh + 30.0f;
        }
        if (!c.outNow.empty())
        {
            if (draw)
                text(F.narrow, c.outNow + " OUT NOW", kW * 0.5f, y + 22.0f, 32.0f, kRed, 1, k);
            y += 48.0f;
        }
        if (!c.notice.empty())
        {
            y += 60.0f;
            for (const std::string &l : wrap(F.body, c.notice, 30.0f, 700.0f))
            {
                if (draw)
                    text(F.body, l, kW * 0.5f, y, 30.0f, kInk, 1, k);
                y += 38.0f;
            }
            return y - top;
        }
        const float size = 23.0f, lineH = 28.0f, gap = 10.0f, colGap = 40.0f;
        const float colW = (kW - 2.0f * kMargin - colGap) * 0.5f;
        for (const Story &s : c.stories)
        {
            const float hs = fit(F.wood, s.headline, 40.0f, kW - 2.0f * kMargin);
            if (draw)
                text(F.wood, s.headline, kW * 0.5f, y + 24.0f, hs, kInk, 1, k);
            y += 52.0f;
            if (draw)
                rule(kMargin, kW - kMargin, y, 1.0f, k);
            y += 16.0f;
            std::vector<std::vector<std::string>> items;
            float total = 0.0f;
            for (const std::string &b : s.bullets)
            {
                items.push_back(wrap(F.body, b, size, colW - 22.0f));
                total += static_cast<float>(items.back().size()) * lineH + gap;
            }
            // two columns: the first takes items until it holds half (the one crossing the middle goes where it
            // leaves the columns closer)
            size_t split = items.size();
            float acc = 0.0f;
            for (size_t i = 0; i < items.size(); ++i)
            {
                const float h = static_cast<float>(items[i].size()) * lineH + gap;
                if (acc + h > total * 0.5f)
                {
                    split = (acc + h - total * 0.5f) < (total * 0.5f - acc) ? i + 1 : i;
                    break;
                }
                acc += h;
            }
            float colH[2] = {0.0f, 0.0f};
            for (size_t i = 0; i < items.size(); ++i)
            {
                const int col = i < split ? 0 : 1;
                const float x = kMargin + static_cast<float>(col) * (colW + colGap);
                float ly = y + colH[col];
                if (draw)
                    text(F.body, "\xE2\x80\xA2", x + 2.0f, ly + lineH * 0.5f, size, kInk, 0, k);
                for (const std::string &l : items[i])
                {
                    if (draw)
                        text(F.body, l, x + 20.0f, ly + lineH * 0.5f, size, kInk, 0, k);
                    ly += lineH;
                }
                colH[col] = ly - y + gap;
            }
            const float height = std::max(colH[0], colH[1]);
            if (draw && colH[0] > 0.0f && colH[1] > 0.0f)
                DrawRectangleRec(Rectangle{(kMargin + colW + colGap * 0.5f) * k, y * k, std::max(1.0f, 0.8f * k), (height - gap) * k},
                                 Color{60, 55, 48, 170});
            y += height + 20.0f;
        }
        return y - top;
    }

    void Paper::renderPage(const Content &c, int w, int h)
    {
        if (m_page.id == 0u || m_page.texture.width != w || m_page.texture.height != h)
        {
            if (m_page.id != 0u)
                UnloadRenderTexture(m_page);
            m_page = LoadRenderTexture(w, h);
            SetTextureFilter(m_page.texture, TEXTURE_FILTER_BILINEAR);
        }
        const float k = static_cast<float>(w) / kW;
        const Fonts &F = m_fonts;
        BeginTextureMode(m_page);
        ClearBackground(kPaperColour);
        newsprintFill(m_newsprint, static_cast<float>(w), static_cast<float>(h));
        // the ears beside the masthead (K: Dominican, inside their boxes; the open page only)
        const float earW = 128.0f, earH = 64.0f, earY = 40.0f;
        const auto ear = [&](float x, const char *a, const char *b) {
            box(Rectangle{x, earY, earW, earH}, 1.6f, k);
            if (m_ears == nullptr || m_ears->texture.id == 0u)
                return;
            const float s = std::min(fit(*m_ears, a, 19.0f, earW - 14.0f), fit(*m_ears, b, 19.0f, earW - 14.0f));
            text(*m_ears, a, x + earW * 0.5f, earY + earH * 0.32f, s, kInk, 1, k);
            text(*m_ears, b, x + earW * 0.5f, earY + earH * 0.70f, s, kInk, 1, k);
        };
        ear(kMargin, "All the News", "That's Fit to Patch");
        ear(kW - kMargin - earW, "Price: Free", "Bring Your Own Disc");
        const std::string mast = "God Hand: Rearmed News";
        text(F.masthead, mast, kW * 0.5f, 70.0f, fit(F.masthead, mast, 70.0f, kW - 2.0f * kMargin - 2.0f * (earW + 14.0f)), kInk, 1, k);
        rule(kMargin, kW - kMargin, 114.0f, 3.5f, k);
        rule(kMargin, kW - kMargin, 120.0f, 1.0f, k);
        text(F.narrow, c.edition, kMargin, 138.0f, 23.0f, kInk, 0, k);
        text(F.narrow, "VOL. " + c.volume + (c.latest ? "  \xC2\xB7  LATEST EDITION" : ""), kW * 0.5f, 138.0f, 23.0f, kInk, 1, k);
        if (!c.date.empty())
            text(F.narrow, c.date, kW - kMargin, 138.0f, 23.0f, kInk, 2, k);
        rule(kMargin, kW - kMargin, 154.0f, 1.0f, k);
        // the stories, scrolled within the body
        const float area = kBodyBottom - kBodyTop;
        const float height = layoutStories(c, kBodyTop, false, k);
        m_maxScroll = std::max(0.0f, height - area);
        const float scroll = std::clamp(c.scroll, 0.0f, m_maxScroll);
        BeginScissorMode(0, static_cast<int>(kBodyTop * k), w, static_cast<int>(area * k));
        layoutStories(c, kBodyTop - scroll, true, k);
        EndScissorMode();
        if (m_maxScroll > 0.0f)
        {
            // a pencil line down the right margin, its thumb where the page is
            const float x = kW - kMargin * 0.45f, thumb = std::max(60.0f, area * area / height);
            DrawRectangleRec(Rectangle{x * k, kBodyTop * k, std::max(1.0f, 1.0f * k), area * k}, Color{60, 55, 48, 120});
            const float ty = kBodyTop + (area - thumb) * (scroll / m_maxScroll);
            DrawRectangleRec(Rectangle{(x - 2.0f) * k, ty * k, std::max(2.0f, 5.0f * k), thumb * k}, kInkSoft);
        }
        // the ads
        rule(kMargin, kW - kMargin, kAdsRule, 3.0f, k);
        rule(kMargin, kW - kMargin, kAdsRule + 6.0f, 1.0f, k);
        const bool printing = c.printing >= 0.0f;
        {
            const Rectangle r = adBox(c, 0);
            const Color ink = printing ? Color{28, 24, 20, 90} : kInk;
            box(r, 3.0f, k, ink);
            box(Rectangle{r.x + 7.0f, r.y + 7.0f, r.width - 14.0f, r.height - 14.0f}, 1.0f, k, ink);
            text(F.west, "BACK", r.x + r.width * 0.5f, r.y + r.height * 0.42f, 60.0f, ink, 1, k);
            text(F.body, "to the saloon", r.x + r.width * 0.5f, r.y + r.height - 25.0f, 22.0f, ink, 1, k);
        }
        if (c.updateAd)
        {
            const Rectangle r = adBox(c, 1);
            box(r, 3.0f, k);
            box(Rectangle{r.x + 7.0f, r.y + 7.0f, r.width - 14.0f, r.height - 14.0f}, 1.0f, k);
            const Rectangle fill{r.x + 12.0f, r.y + 12.0f, r.width - 24.0f, r.height - 24.0f};
            Color red = kRed;
            red.a = 238;
            DrawRectangleRec(Rectangle{fill.x * k, fill.y * k, fill.width * k, fill.height * k}, red);
            if (printing)
            {
                char label[48];
                if (c.printingKnown)
                    std::snprintf(label, sizeof(label), "PRINTING\xE2\x80\xA6 %d%%", static_cast<int>(std::round(std::clamp(c.printing, 0.0f, 1.0f) * 100.0f)));
                else
                    std::snprintf(label, sizeof(label), "PRINTING\xE2\x80\xA6");
                text(F.wood, label, r.x + r.width * 0.5f, r.y + r.height * 0.40f, fit(F.wood, label, 34.0f, fill.width - 30.0f), kCream, 1, k);
                const Rectangle bar{fill.x + 26.0f, r.y + r.height - 40.0f, fill.width - 52.0f, 14.0f};
                box(bar, 1.5f, k, kCream);
                const float shown = c.printingKnown ? std::clamp(c.printing, 0.0f, 1.0f) : std::fmod(static_cast<float>(GetTime()) * 0.6f, 1.0f);
                DrawRectangleRec(Rectangle{(bar.x + 3.0f) * k, (bar.y + 3.0f) * k, (bar.width - 6.0f) * shown * k, (bar.height - 6.0f) * k}, kCream);
            }
            else
            {
                text(F.wood, "UPDATE NOW", r.x + r.width * 0.5f, r.y + r.height * 0.42f, fit(F.wood, "UPDATE NOW", 40.0f, fill.width - 30.0f), kCream, 1, k);
                text(F.narrow, "Fresh off the press! Free delivery.", r.x + r.width * 0.5f, r.y + r.height - 27.0f, 20.0f, kCream, 1, k);
            }
        }
        if (c.focus == 0 || (c.focus == 1 && c.updateAd))
            loop(adBox(c, c.focus), c.focus == 1 ? kInk : kRed, static_cast<uint32_t>(c.focus * 977 + 13), k);
        EndTextureMode();
    }

    void Paper::draw(const Content &c, float t, float alpha)
    {
        if (!m_loaded)
            return;
        const float e = easeInOut(t);
        const bool update = !c.outNow.empty();
        const Placement corner = cornerPlacement(update, cornerBox(update)), open = openPlacement();
        // the textures, re-rendered when what they show (or their size) changes
        const auto key = [&](int w, int h, bool page) {
            std::ostringstream o;
            o << w << 'x' << h << '|' << c.edition << '|' << c.volume << '|' << c.date << '|' << c.outNow << '|' << c.latest;
            if (page)
            {
                o << '|' << c.notice << '|' << c.updateAd << '|' << std::lround(c.printing * 100.0f) << '|' << c.printingKnown << '|'
                  << c.stop << '|' << c.focus << '|' << std::lround(c.scroll * 2.0f);
                if (c.printing >= 0.0f && !c.printingKnown)
                    o << '|' << std::lround(GetTime() * 20.0); // the running bar
                for (const Story &s : c.stories)
                {
                    o << '|' << s.headline;
                    for (const std::string &b : s.bullets)
                        o << '\n' << b;
                }
            }
            return o.str();
        };
        if (e < 1.0f)
        {
            const int w = std::max(16, static_cast<int>(kW * corner.scale)), h = std::max(16, static_cast<int>(kH * corner.scale));
            const std::string k = key(w, h, false);
            if (k != m_cornerKey)
            {
                renderCorner(c, w, h);
                m_cornerKey = k;
            }
        }
        if (e > 0.0f)
        {
            const int w = std::max(16, static_cast<int>(kW * open.scale)), h = std::max(16, static_cast<int>(kH * open.scale));
            const std::string k = key(w, h, true);
            if (k != m_pageKey)
            {
                renderPage(c, w, h);
                m_pageKey = k;
            }
        }
        if (e > 0.0f)
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, static_cast<unsigned char>(165.0f * e)});
        const Placement p = blend(corner, open, e);
        const float a = e > 0.0f ? 1.0f : alpha;
        shadow(p, a);
        const float f = smoothstep(0.2f, 0.5f, e);
        if (f < 1.0f)
            drawPage(m_corner, p, a);
        if (f > 0.0f)
            drawPage(m_page, p, f);
    }
}
