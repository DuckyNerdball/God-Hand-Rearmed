// The launcher's saloon (see launcher_saloon.h; decisions in claude/saloon-launcher.md).
#include "kamite/launcher_saloon.h"

#include "kamite/json_reader.h"
#include "kamite/log.h"
#include "kamite/saloon_image.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kamite::launcher::saloon
{
    namespace
    {
        namespace fs = std::filesystem;
        using json::Json;
        using saloon_image::Kind;
        using saloon_image::Pixels;

        constexpr float kPi = 3.14159265358979f;
        constexpr int kZoomFrames = 8;
        constexpr float kPushIn = 0.2f; // the 2D push-in on frames 5..8 (1.0 -> 1.2, K Nov 18)
        constexpr float kFloatTime = 0.6f;  // an eaten snack's rise
        constexpr float kShadowFade = 0.4f; // its shadow
        constexpr int kMoteCount = 90; // bright motes in the doorway's light (the fine dust: the plumes, kDustShader)

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        // ---- shaders (raylib's default vertex shader; the header for the GL version raylib runs) ---------------------
        Shader loadShader(const char *fragment)
        {
            const int gl = rlGetVersion();
            const char *header = (gl == RL_OPENGL_33 || gl == RL_OPENGL_43) ? "#version 330\n"
                                 : gl == RL_OPENGL_ES_30                    ? "#version 300 es\nprecision highp float;\n"
                                                                            : nullptr;
            if (header == nullptr)
                return Shader{};
            const std::string source = std::string(header) + fragment;
            return LoadShaderFromMemory(nullptr, source.c_str());
        }
        bool usable(const Shader &shader) { return shader.id != 0u && shader.id != rlGetShaderIdDefault(); }
        void unload(Shader &shader)
        {
            if (usable(shader))
                UnloadShader(shader);
            shader = Shader{};
        }
        void unload(Texture2D &texture)
        {
            if (texture.id != 0u)
                UnloadTexture(texture);
            texture = Texture2D{};
        }

        // Ink: premultiplied, so the multiply blend leaves the paper alone where a letter's edge fades out.
        const char *const kInkShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main()
{
    float a = texture(texture0, fragTexCoord).a * fragColor.a * colDiffuse.a;
    finalColor = vec4(fragColor.rgb * colDiffuse.rgb * a, a);
}
)glsl";

        // A snack's shadow multiplier (one channel), faded by the vertex alpha (an eaten piece's shadow going).
        const char *const kShadowShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;
void main()
{
    float s = texture(texture0, fragTexCoord).r;
    finalColor = vec4(vec3(mix(1.0, s, fragColor.a)), 1.0);
}
)glsl";

        // The light rays (a screen layer): near ones follow the table, far ones the room (the pack's weight map: ~1 at
        // the doorway, ~0.3 at the near floor); the doorway's fan swings a little and beams sweep round it both ways,
        // like the burst's turning layers (the windows' rays keep still).
        const char *const kRaysShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D weightMap;
uniform vec2 shiftRoom;  // the room's drift, in this texture's uv
uniform vec2 shiftTable; // the table's
uniform vec2 shiftAir;   // the air's against the room (the near ends move, the doorway's end stays)
uniform vec4 lock;       // the doors' box (uv): the air drift fades out over it (their shading stays on them)
uniform vec2 feather;    // the box's soft edge (uv)
uniform vec2 source;     // where the doorway's rays fan out from, uv
uniform vec2 size;       // the texture's size, pixels
uniform float time;
uniform float motion;
out vec4 finalColor;
void main()
{
    vec2 uv = fragTexCoord;
    float w = texture(weightMap, uv).r;
    // the air's drift: in the doorway's fan only (the windows' rays stay on their windows), not over the doors
    vec2 inLo = smoothstep(lock.xy - feather, lock.xy, uv), inHi = 1.0 - smoothstep(lock.zw, lock.zw + feather, uv);
    float locked = inLo.x * inLo.y * inHi.x * inHi.y;
    float airFan = 1.0 - smoothstep(0.15, 0.24, abs(uv.x - source.x));
    uv -= mix(shiftTable, shiftRoom, w) + shiftAir * (1.0 - w) * airFan * (1.0 - locked);
    vec2 d = (uv - source) * size;
    float r = length(d);
    float a = atan(d.y, d.x);
    float fan = 1.0 - smoothstep(0.15, 0.24, abs(uv.x - source.x));
    float swing = motion * 0.022 * (0.6 * sin(time * 0.52) + 0.4 * sin(time * 0.83 + 1.3)) * smoothstep(30.0, 300.0, r) * fan;
    float c = cos(swing), s = sin(swing);
    vec2 turned = vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    vec3 col = texture(texture0, source + turned / size).rgb;
    // waves round the circle (whole numbers: no seam), each turning at a few degrees a second, some each way
    float b = 0.40 * sin(9.0 * a - time * 0.80) + 0.30 * sin(14.0 * a + time * 0.62 + 1.1)
            + 0.20 * sin(23.0 * a - time * 1.15 + 2.3) + 0.25 * sin(5.0 * a + time * 0.38 + 0.7);
    float beams = max(0.0, 1.0 + motion * 0.65 * b * smoothstep(40.0, 350.0, r) * fan);
    float pulse = 1.0 + motion * 0.11 * (0.7 * sin(time * 1.7 + 1.0) + 0.3 * sin(time * 2.9));
    finalColor = vec4(col * beams * pulse * fragColor.a, 1.0);
}
)glsl";

        const char *const kNoise = R"glsl(
float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i)
    {
        v += a * noise(p);
        p = p * 2.03 + vec2(17.1, 9.2);
        a *= 0.5;
    }
    return v;
}
)glsl";

        // The curry's steam: thin wisps rising off the bowl in puffs, curling and spreading as they go (strength in the
        // vertex alpha).
        // The kana outside the doorway: its own picture, kept to the openings by the outside matte (the doors and the
        // frame in front of it).
        const char *const kKanaShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0; // the kana
uniform sampler2D matte;    // the outside matte (1: outside)
uniform vec2 centre;        // the kana's centre in the matte's uv
uniform vec4 axes;          // its (turned) across and down vectors in the matte's uv: across (xy), down (zw)
out vec4 finalColor;
void main()
{
    vec4 c = texture(texture0, fragTexCoord) * fragColor;
    float m = texture(matte, centre + (fragTexCoord.x - 0.5) * axes.xy + (fragTexCoord.y - 0.5) * axes.zw).r;
    finalColor = vec4(c.rgb * c.a * m, c.a * m); // premultiplied
}
)glsl";

        // Dust hanging in the room's air past the table (K, Nov 18): fine billowy plumes, faintly lit of their own against
        // the dark (a dusty room, not film grain), much brighter where the rays reach; drawn before the table set, so
        // the table stands in front of it. The rays layer's own rectangle (the back's).
        const char *const kDustShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0; // the rays: the light in the air
uniform vec2 size;          // its size, pixels
uniform vec2 drift;         // the air's drift (uv)
uniform float time;
uniform float amount;       // the tuning's Dust plumes (past 1: dustier)
out vec4 finalColor;
%NOISE%
void main()
{
    vec2 uv = fragTexCoord;
    vec3 light = texture(texture0, uv).rgb;
    vec2 p = (uv - drift) * size / vec2(340.0, 260.0);
    float t = time * 0.03;
    vec2 eddy = vec2(soft(p * 0.5 + vec2(t, -0.7 * t)), soft(p * 0.5 + vec2(4.7 - 0.8 * t, 2.1 + t))) - 0.5;
    float plume = smoothstep(0.40, 0.80, soft(p + eddy * 2.2 + vec2(0.3 * t, -0.2 * t)));
    float veil = smoothstep(0.40, 0.85, soft(p * 2.2 + eddy * 3.0 + vec2(-0.4 * t, 0.15 * t))); // finer veils in them
    float dust = plume * (0.45 + 0.55 * veil);
    float band = smoothstep(0.02, 0.22, uv.y) * (1.0 - smoothstep(0.82, 0.98, uv.y)); // thin at the ceiling and the floor
    vec3 lit = vec3(0.62, 0.52, 0.40) * 0.30 + light * 1.0;
    finalColor = vec4(lit * dust * band * amount, 1.0);
}
)glsl";

        const char *const kSteamShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform float time;
uniform float aspect; // the quad's width / height
out vec4 finalColor;
%NOISE%
void main()
{
    float x = (fragTexCoord.x - 0.5) * 2.0; // -1..1 across
    float h = 1.0 - fragTexCoord.y;         // 0 at the bowl, 1 at the top
    // where the vapour rises: four plumes' soft envelope, curling and swaying more the higher they get
    float env = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float fi = float(i);
        float base = (fi - 1.5) * 0.26 + 0.06 * sin(time * 0.21 + fi * 2.1);
        float curl = (soft(vec2(h * 1.6 - time * 0.20 + fi * 7.3, fi * 1.7)) - 0.5) * (0.3 + 1.3 * h);
        float sway = 0.10 * h * sin(time * 0.47 + fi * 1.9 + h * 4.5);
        float width = (0.05 + 0.19 * h) * (1.0 + 0.3 * sin(fi * 4.1));
        float d = (x - (base * (1.0 - 0.35 * h) + curl + sway)) / width;
        env += exp(-d * d * 0.9);
    }
    env = clamp(env, 0.0, 1.0);
    // the vapour: carried up by the warm air, folded by slow eddies (stronger higher up), drawn out upward into
    // soft-edged veils (K: steam, not smoke; no texture of its own)
    vec2 p = vec2(x * aspect * 1.1, h * 1.3 - time * 0.36);
    vec2 eddy = vec2(soft(p * 0.8 + vec2(0.0, time * 0.07)), soft(p * 0.8 + vec2(5.2, 1.3 - time * 0.06))) - 0.5;
    p += eddy * (0.5 + 1.5 * h);
    float veil = smoothstep(0.36, 0.70, soft(vec2(p.x * 1.4, p.y * 0.6)));
    float shape = smoothstep(0.0, 0.08, h) * (1.0 - smoothstep(0.50, 1.0, h));
    float a = env * (0.25 + 0.75 * veil) * shape * fragColor.a * 0.78;
    finalColor = vec4(vec3(0.97, 0.95, 0.91) * a, a);
}
)glsl";

        // The smoothie glass in the warm saloon: a milky film of condensation where the cold drink is, thicker lower
        // down, with beads of water in it; a drop now and then runs down the glass's curve, wiping a clear trail that
        // fogs over again.
        const char *const kFrostShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0; // the drink's mask (alpha)
uniform sampler2D blurMap;  // the drink blurred (premultiplied), over the smoothie's rect
uniform vec4 blurRect;      // this quad's uv into blurMap's: offset (xy), scale (zw)
uniform vec2 size;          // the quad's size, frame pixels
uniform vec2 band;          // the glass's rim and the drink's bottom, uv
uniform vec4 drop;          // the running drop: x, y (uv), size, alpha
uniform vec4 trail[24];     // where it ran: x, y (uv), age (s), 1 = used
out vec4 finalColor;
%NOISE%
void main()
{
    vec2 uv = fragTexCoord;
    float mask = texture(texture0, uv).a;
    vec2 p = uv * size;
    // the drop, and the clear track behind it (it gathered the mist as it went; the mist creeps back)
    vec2 dq = (p - drop.xy * size) / (vec2(3.2, 4.2) * drop.z);
    float dr = length(dq);
    float body = (1.0 - smoothstep(0.82, 1.0, dr)) * drop.w;
    float wiped = 0.0;
    for (int i = 0; i < 23; ++i)
    {
        if (trail[i + 1].w < 0.5)
            break;
        vec2 a = trail[i].xy * size, b = trail[i + 1].xy * size, ab = b - a;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
        float dist = length(p - a - ab * t);
        float age = mix(trail[i].z, trail[i + 1].z, t);
        float width = 2.6 * drop.z * (1.0 - 0.35 * smoothstep(0.0, 1.0, age / 10.0));
        wiped = max(wiped, (1.0 - smoothstep(width * 0.6, width, dist)) * (1.0 - smoothstep(3.0, 12.0, age)));
    }
    // the fog: the drink seen blurred through a misted glass, from nothing at the rim to full low down, a pale haze
    // over it; the drop and its track clear it (K, Nov 18: no beads, the drop is the only water to see)
    float thick = smoothstep(band.x, mix(band.x, band.y, 0.8), uv.y);
    float fog = mask * (1.0 - wiped) * (1.0 - body) * thick;
    vec4 blurred = texture(blurMap, blurRect.xy + uv * blurRect.zw);
    // chilled: paler where the mist is thickest, so the drop's track shows in the colour too (K)
    vec3 col = mix(blurred.rgb, vec3(0.92, 0.95, 0.98) * blurred.a, 0.06 + 0.22 * thick) * fog * fragColor.a;
    float a = blurred.a * fog * fragColor.a;
    // the drop: clear, a dark rim heavier low, a bright speck top left, a light caustic low
    float edge = smoothstep(0.5, 0.95, dr) * body * (0.55 + 0.45 * clamp(0.5 + 0.7 * dq.y, 0.0, 1.0));
    float speck = (1.0 - smoothstep(0.0, 0.3, length(dq - vec2(-0.3, -0.42)))) * drop.w;
    float caustic = (1.0 - smoothstep(0.0, 0.34, length(dq - vec2(0.06, 0.55)))) * drop.w;
    float dropA = max(edge * 0.7, max(speck, caustic * 0.5));
    vec3 dropCol = vec3(0.06, 0.12, 0.22) * edge * 0.7 + vec3(0.8, 0.92, 1.0) * caustic * 0.5 + vec3(1.0) * speck;
    dropCol = min(dropCol, vec3(dropA));
    finalColor = vec4(dropCol + col * (1.0 - dropA), dropA + a * (1.0 - dropA));
}
)glsl";

        // The plate's front lip over the front cherry pair (see lipFrom): its edge, and the box redrawn below it.
        constexpr Vector2 kPlateLip[] = {{2330.0f, 1089.0f}, {2350.0f, 1095.0f}, {2370.0f, 1099.0f}, {2390.0f, 1102.0f},
                                         {2410.0f, 1105.0f}, {2430.0f, 1108.0f}, {2450.0f, 1111.0f}};
        constexpr Rectangle kLipBox{2330.0f, 1084.0f, 120.0f, 40.0f};

        // Smooth gradient noise (no value noise's grid, no fine octaves): steam and dust, things of air.
        const char *const kSoftNoise = R"glsl(
vec2 gradOf(vec2 i)
{
    float a = hash(i) * 6.2831853;
    return vec2(cos(a), sin(a));
}
float gnoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(gradOf(i), f), b = dot(gradOf(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(gradOf(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0)), d = dot(gradOf(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return 0.5 + 0.75 * mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float soft(vec2 p)
{
    float v = 0.0, a = 0.55, sum = 0.0;
    mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
    for (int i = 0; i < 3; ++i)
    {
        v += a * gnoise(p);
        sum += a;
        p = turn * p * 2.0 + vec2(3.7, 1.9);
        a *= 0.45;
    }
    return v / sum;
}
)glsl";

        std::string withNoise(const char *source)
        {
            std::string text(source);
            const size_t at = text.find("%NOISE%");
            if (at != std::string::npos)
                text.replace(at, 7, std::string(kNoise) + kSoftNoise);
            return text;
        }

        Shader g_inkShader{};
        bool g_inkTried = false;

        // ---- blending -----------------------------------------------------------------------------------------------
        void beginScreen()
        {
            rlSetBlendFactors(RL_ONE, RL_ONE_MINUS_SRC_COLOR, RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM);
        }
        void beginMultiply()
        {
            rlSetBlendFactors(RL_DST_COLOR, RL_ZERO, RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM);
        }

        // ---- pixels ------------------------------------------------------------------------------------------------
        Texture2D upload(const Pixels &pixels, bool mipmaps)
        {
            if (pixels.empty())
                return Texture2D{};
            Image image{};
            image.data = const_cast<uint8_t *>(pixels.data.data());
            image.width = pixels.width;
            image.height = pixels.height;
            image.mipmaps = 1;
            image.format = pixels.channels == 4   ? PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
                           : pixels.channels == 3 ? PIXELFORMAT_UNCOMPRESSED_R8G8B8
                                                  : PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
            Texture2D texture = LoadTextureFromImage(image);
            if (texture.id == 0u)
                return texture;
            if (mipmaps)
            {
                GenTextureMipmaps(&texture);
                SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
            }
            else
                SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
            return texture;
        }

        // The alpha of premultiplied RGBA (a snack's hit mask).
        Pixels alphaOf(const Pixels &rgba)
        {
            Pixels out;
            out.width = rgba.width;
            out.height = rgba.height;
            out.channels = 1;
            out.data.resize(static_cast<size_t>(rgba.width) * rgba.height);
            for (size_t i = 0; i < out.data.size(); ++i)
                out.data[i] = rgba.data[i * 4 + 3];
            return out;
        }

        uint8_t at(const Pixels &gray, int x, int y)
        {
            if (gray.empty() || x < 0 || y < 0 || x >= gray.width || y >= gray.height)
                return 0;
            return gray.data[static_cast<size_t>(y) * gray.width + x];
        }

        // Separable gaussian on a float map (in place).
        void blur(std::vector<float> &map, int w, int h, float sigma)
        {
            const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
            std::vector<float> kernel(static_cast<size_t>(radius) * 2 + 1);
            float sum = 0.0f;
            for (int i = -radius; i <= radius; ++i)
                sum += kernel[static_cast<size_t>(i + radius)] = std::exp(-0.5f * i * i / (sigma * sigma));
            for (float &k : kernel)
                k /= sum;
            std::vector<float> tmp(map.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float v = 0.0f;
                    for (int i = -radius; i <= radius; ++i)
                        v += kernel[static_cast<size_t>(i + radius)] * map[static_cast<size_t>(y) * w + std::clamp(x + i, 0, w - 1)];
                    tmp[static_cast<size_t>(y) * w + x] = v;
                }
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float v = 0.0f;
                    for (int i = -radius; i <= radius; ++i)
                        v += kernel[static_cast<size_t>(i + radius)] * tmp[static_cast<size_t>(std::clamp(y + i, 0, h - 1)) * w + x];
                    map[static_cast<size_t>(y) * w + x] = v;
                }
        }

        // The doorway's (and the windows') bloom from the outside matte, at an eighth of its size: the light spills
        // out round the openings but not into them. The matte is closed first (its thin dark lines filled: window
        // bars, the post in the doorway), and the glow is kept off the closed openings, so those thin silhouettes
        // stay dark instead of being washed out.
        Pixels bloomFrom(const Pixels &matte)
        {
            constexpr int kStep = 8;
            const int w = std::max(1, (matte.width + kStep - 1) / kStep), h = std::max(1, (matte.height + kStep - 1) / kStep);
            std::vector<float> small(static_cast<size_t>(w) * h, 0.0f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float sum = 0.0f;
                    int n = 0;
                    for (int yy = y * kStep; yy < std::min(matte.height, (y + 1) * kStep); ++yy)
                        for (int xx = x * kStep; xx < std::min(matte.width, (x + 1) * kStep); ++xx, ++n)
                            sum += matte.data[static_cast<size_t>(yy) * matte.width + xx];
                    small[static_cast<size_t>(y) * w + x] = n ? sum / (255.0f * n) : 0.0f;
                }
            // closing: dilate then erode a binary opening mask by 2 (16 px of the matte)
            std::vector<uint8_t> open(small.size()), grown(small.size()), closed(small.size());
            for (size_t i = 0; i < small.size(); ++i)
                open[i] = small[i] > 0.3f;
            const auto morph = [&](const std::vector<uint8_t> &in, std::vector<uint8_t> &out, bool dilate) {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        bool v = !dilate;
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dx = -2; dx <= 2; ++dx)
                            {
                                const bool s = in[static_cast<size_t>(std::clamp(y + dy, 0, h - 1)) * w + std::clamp(x + dx, 0, w - 1)] != 0;
                                v = dilate ? (v || s) : (v && s);
                            }
                        out[static_cast<size_t>(y) * w + x] = v;
                    }
            };
            morph(open, grown, true);
            morph(grown, closed, false);
            std::vector<float> keepOff(closed.begin(), closed.end());
            blur(keepOff, w, h, 0.8f);
            std::vector<float> near = small, far = small;
            blur(near, w, h, 4.0f);
            blur(far, w, h, 14.0f);
            Pixels out;
            out.width = w;
            out.height = h;
            out.channels = 1;
            out.data.resize(small.size());
            for (size_t i = 0; i < small.size(); ++i)
            {
                const float glow = (0.6f * near[i] + 0.55f * far[i]) * (1.0f - keepOff[i]);
                out.data[i] = static_cast<uint8_t>(std::clamp(glow, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            return out;
        }

        // The rays' brightness at a sixteenth of their size (where the motes catch the light).
        Pixels lumaOf(const Pixels &rgb, int step)
        {
            Pixels out;
            out.width = std::max(1, rgb.width / step);
            out.height = std::max(1, rgb.height / step);
            out.channels = 1;
            out.data.resize(static_cast<size_t>(out.width) * out.height);
            for (int y = 0; y < out.height; ++y)
                for (int x = 0; x < out.width; ++x)
                {
                    int sum = 0, n = 0;
                    for (int yy = y * step; yy < (y + 1) * step && yy < rgb.height; yy += 2)
                        for (int xx = x * step; xx < (x + 1) * step && xx < rgb.width; xx += 2, ++n)
                        {
                            const uint8_t *p = &rgb.data[(static_cast<size_t>(yy) * rgb.width + xx) * rgb.channels];
                            sum += (p[0] * 3 + p[1] * 6 + p[2]) / 10;
                        }
                    out.data[static_cast<size_t>(y) * out.width + x] = static_cast<uint8_t>(n ? sum / n : 0);
                }
            return out;
        }

        float sample(const Pixels &gray, float u, float v)
        {
            if (gray.empty())
                return 0.0f;
            const float x = std::clamp(u * gray.width - 0.5f, 0.0f, gray.width - 1.001f);
            const float y = std::clamp(v * gray.height - 0.5f, 0.0f, gray.height - 1.001f);
            const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
            const float fx = x - x0, fy = y - y0;
            const auto g = [&](int xx, int yy) { return gray.data[static_cast<size_t>(yy) * gray.width + xx] / 255.0f; };
            return (g(x0, y0) * (1 - fx) + g(x0 + 1, y0) * fx) * (1 - fy) + (g(x0, y0 + 1) * (1 - fx) + g(x0 + 1, y0 + 1) * fx) * fy;
        }

        // ---- the layout ---------------------------------------------------------------------------------------------
        Vector2 point(const Json *value)
        {
            if (value == nullptr || value->kind != Json::Kind::Array || value->items.size() < 2)
                return Vector2{0.0f, 0.0f};
            return Vector2{static_cast<float>(value->items[0].number), static_cast<float>(value->items[1].number)};
        }
        bool quadOf(const Json *value, Quad &out)
        {
            if (value == nullptr || value->kind != Json::Kind::Array || value->items.size() != 4)
                return false;
            for (int i = 0; i < 4; ++i)
                out.p[i] = point(&value->items[static_cast<size_t>(i)]);
            return true;
        }
        Rectangle rectOf(const Json *value)
        {
            if (value == nullptr || value->kind != Json::Kind::Array || value->items.size() != 4)
                return Rectangle{};
            const float x0 = static_cast<float>(value->items[0].number), y0 = static_cast<float>(value->items[1].number);
            return Rectangle{x0, y0, static_cast<float>(value->items[2].number) - x0, static_cast<float>(value->items[3].number) - y0};
        }
        std::vector<Vector2> polygonOf(const Json *value)
        {
            std::vector<Vector2> out;
            if (value != nullptr && value->kind == Json::Kind::Array)
                for (const Json &item : value->items)
                    out.push_back(point(&item));
            return out;
        }
        std::string textOf(const Json *value) { return value && value->kind == Json::Kind::String ? value->text : std::string(); }

        // A quad worth drawing on: finite, near the frame, convex with some area (the zoom's projections of things
        // behind the camera are not).
        bool sane(const Quad &q)
        {
            float sign = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                const Vector2 a = q.p[i], b = q.p[(i + 1) % 4], c = q.p[(i + 2) % 4];
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || a.x < -3.0f || a.x > 4.0f || a.y < -3.0f || a.y > 4.0f)
                    return false;
                const float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
                if (std::fabs(cross) < 1e-7f || (sign != 0.0f && (cross > 0.0f) != (sign > 0.0f)))
                    return false;
                sign = cross;
            }
            return true;
        }

        bool inPolygon(const std::vector<Vector2> &poly, Vector2 p)
        {
            bool in = false;
            for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
                if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
                    (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
                    in = !in;
            return in;
        }

        // ---- the batwing doors' breeze loop, decoded as it plays (one frame ahead; never kept in VRAM) ---------------
        class DoorStream
        {
        public:
            ~DoorStream() { stop(); }
            void start(std::vector<std::vector<uint8_t>> files)
            {
                m_files = std::move(files);
                if (m_files.empty())
                    return;
                m_worker = std::thread([this] { run(); });
            }
            void stop()
            {
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    m_quit = true;
                }
                m_wake.notify_all();
                if (m_worker.joinable())
                    m_worker.join();
            }
            int count() const { return static_cast<int>(m_files.size()); }
            // The frame now showing: the next two are decoded behind it.
            void showing(int frame)
            {
                {
                    std::lock_guard<std::mutex> lock(m_lock);
                    m_showing = frame;
                    for (auto it = m_ready.begin(); it != m_ready.end();)
                        it = wanted(it->first) ? std::next(it) : m_ready.erase(it);
                }
                m_wake.notify_all();
            }
            bool take(int frame, Pixels &out)
            {
                std::lock_guard<std::mutex> lock(m_lock);
                const auto it = m_ready.find(frame);
                if (it == m_ready.end())
                    return false;
                out = std::move(it->second);
                m_ready.erase(it);
                return true;
            }

        private:
            bool wanted(int frame) const
            {
                const int n = static_cast<int>(m_files.size());
                for (int d = 1; d <= 2; ++d)
                    if (frame == ((m_showing + d) % n + n) % n)
                        return true;
                return false;
            }
            int next() const
            {
                const int n = static_cast<int>(m_files.size());
                for (int d = 1; d <= 2; ++d)
                {
                    const int frame = ((m_showing + d) % n + n) % n;
                    if (frame != m_busy && m_ready.find(frame) == m_ready.end())
                        return frame;
                }
                return -1;
            }
            void run()
            {
                for (;;)
                {
                    int frame = -1;
                    {
                        std::unique_lock<std::mutex> lock(m_lock);
                        m_wake.wait(lock, [this] { return m_quit || next() >= 0; });
                        if (m_quit)
                            return;
                        frame = next();
                        m_busy = frame;
                    }
                    Pixels pixels;
                    const bool ok = saloon_image::decode(m_files[static_cast<size_t>(frame)], Kind::Rgba, pixels, 1);
                    std::lock_guard<std::mutex> lock(m_lock);
                    m_busy = -1;
                    if (ok && wanted(frame))
                        m_ready[frame] = std::move(pixels);
                }
            }

            std::vector<std::vector<uint8_t>> m_files;
            std::thread m_worker;
            std::mutex m_lock;
            std::condition_variable m_wake;
            std::map<int, Pixels> m_ready;
            int m_showing = -1, m_busy = -1;
            bool m_quit = false;
        };

        // ---- the scene's pieces -------------------------------------------------------------------------------------
        struct Piece
        {
            std::string name;
            Rectangle rect{}; // frame pixels
            Texture2D sprite{}, shadow{};
            Pixels hit; // Home only: the sprite's alpha
        };
        struct Smoothie
        {
            bool present = false;
            Rectangle rect{};
            std::vector<Texture2D> states, garnish;
            std::vector<bool> garnishBehind; // a garnish other things stood in front of (the back slice): it floats off
                                             // under the state's picture, so the front slice and the cream still hide it
            std::vector<Pixels> hit;      // Home only, per state
            std::vector<Texture2D> blur;  // Home only, per state: the drink blurred (the frost), half size
            std::vector<Pixels> blurPx;   // (made on a worker, uploaded with the state)
        };
        struct Snacks
        {
            std::vector<Piece> pieces; // back to front
            Rectangle shadows{};
            Smoothie smoothie;
        };
        struct ZoomFrame
        {
            Texture2D frame{}, bloom{};
            Quad bill{}, door{};
            Quad pageL{}, pageR{}; // the open book's pages (the last frame only): the Options book is drawn on them
            bool billInFront = true, hasBill = false, hasDoor = false, hasPages = false;
            Snacks snacks;
        };
        struct Mote
        {
            Vector2 base;  // frame pixels
            Vector2 swing; // orbit radii
            float px, py, p2, phase1, phase2, phase3, size, bright, depth;
        };
        struct Floating
        {
            int piece = -1;   // a Home piece, or
            int garnish = -1; // a smoothie garnish
            float start = 0.0f;
        };

        // A decode job: worker threads decode (and post-process), the main thread uploads.
        struct Job
        {
            enum State
            {
                Queued,
                Decoded,
                Failed,
                Finished
            };
            fs::path path;
            Kind kind = Kind::Rgb;
            int group = 0; // 0: Home, 1: the zoom and the door fling, 2: the zoom's snacks
            int threads = 1;
            std::function<void(Pixels &)> work; // worker side, after decoding
            std::function<void(Pixels &)> done; // main thread
            std::atomic<int> state{Queued};
            Pixels pixels;
        };
    }

    // ---- homography ---------------------------------------------------------------------------------------------------
    Homography Homography::rectToQuad(float w, float h, const Quad &q)
    {
        const double x0 = q.p[0].x, y0 = q.p[0].y, x1 = q.p[1].x, y1 = q.p[1].y;
        const double x2 = q.p[2].x, y2 = q.p[2].y, x3 = q.p[3].x, y3 = q.p[3].y;
        const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
        double g = 0.0, hh = 0.0;
        if (std::fabs(sx) > 1e-12 || std::fabs(sy) > 1e-12)
        {
            const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
            const double den = dx1 * dy2 - dx2 * dy1;
            if (std::fabs(den) > 1e-18)
            {
                g = (sx * dy2 - dx2 * sy) / den;
                hh = (dx1 * sy - sx * dy1) / den;
            }
        }
        Homography out;
        const double a = x1 - x0 + g * x1, b = x3 - x0 + hh * x3, d = y1 - y0 + g * y1, e = y3 - y0 + hh * y3;
        const double sw = w != 0.0f ? 1.0 / w : 1.0, sh = h != 0.0f ? 1.0 / h : 1.0;
        out.m[0] = a * sw;
        out.m[1] = b * sh;
        out.m[2] = x0;
        out.m[3] = d * sw;
        out.m[4] = e * sh;
        out.m[5] = y0;
        out.m[6] = g * sw;
        out.m[7] = hh * sh;
        out.m[8] = 1.0;
        return out;
    }

    Vector2 Homography::map(float x, float y) const
    {
        const double w = m[6] * x + m[7] * y + m[8];
        const double iw = std::fabs(w) > 1e-12 ? 1.0 / w : 0.0;
        return Vector2{static_cast<float>((m[0] * x + m[1] * y + m[2]) * iw), static_cast<float>((m[3] * x + m[4] * y + m[5]) * iw)};
    }

    Homography Homography::inverse() const
    {
        const double *a = m;
        const double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
        const double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
        Homography out;
        if (std::fabs(det) < 1e-18)
            return out;
        const double k = 1.0 / det;
        out.m[0] = c00 * k;
        out.m[1] = (a[2] * a[7] - a[1] * a[8]) * k;
        out.m[2] = (a[1] * a[5] - a[2] * a[4]) * k;
        out.m[3] = c01 * k;
        out.m[4] = (a[0] * a[8] - a[2] * a[6]) * k;
        out.m[5] = (a[2] * a[3] - a[0] * a[5]) * k;
        out.m[6] = c02 * k;
        out.m[7] = (a[1] * a[6] - a[0] * a[7]) * k;
        out.m[8] = (a[0] * a[4] - a[1] * a[3]) * k;
        return out;
    }

    bool inside(const Quad &quad, Vector2 p)
    {
        float sign = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            const Vector2 a = quad.p[i], b = quad.p[(i + 1) % 4];
            const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            if (cross == 0.0f)
                continue;
            if (sign != 0.0f && (cross > 0.0f) != (sign > 0.0f))
                return false;
            sign = cross;
        }
        return true;
    }

    // ---- ink ------------------------------------------------------------------------------------------------------------
    void beginInk()
    {
        if (!g_inkTried)
        {
            g_inkTried = true;
            g_inkShader = loadShader(kInkShader);
        }
        if (usable(g_inkShader))
            BeginShaderMode(g_inkShader);
        BeginBlendMode(BLEND_MULTIPLIED);
        rlDisableBackfaceCulling(); // a homography can turn a quad's winding round
    }

    // Paint: the ink's geometry drawn over (normal blending), for things that sit on the paper rather than in it.
    void beginPaint()
    {
        if (!g_inkTried)
        {
            g_inkTried = true;
            g_inkShader = loadShader(kInkShader);
        }
        if (usable(g_inkShader))
            BeginShaderMode(g_inkShader);
        BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
        rlDisableBackfaceCulling();
    }

    void endPaint() { endInk(); }

    void endInk()
    {
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();
        EndBlendMode();
        if (usable(g_inkShader))
            EndShaderMode();
    }

    float inkWidth(const Font &font, const char *text, float size, float spacing)
    {
        if (font.texture.id == 0u || font.baseSize <= 0 || text == nullptr)
            return 0.0f;
        const float scale = size / static_cast<float>(font.baseSize);
        float width = 0.0f;
        for (int i = 0; text[i] != '\0';)
        {
            int bytes = 0;
            const int codepoint = GetCodepointNext(&text[i], &bytes);
            i += bytes > 0 ? bytes : 1;
            const int index = GetGlyphIndex(font, codepoint);
            width += (font.glyphs[index].advanceX != 0 ? static_cast<float>(font.glyphs[index].advanceX) : font.recs[index].width) * scale + spacing;
        }
        return std::max(0.0f, width - spacing);
    }

    float inkText(const Font &font, const char *text, Vector2 centre, float size, Color color, const Homography &paper, float spacing)
    {
        if (font.texture.id == 0u || font.baseSize <= 0 || text == nullptr)
            return 0.0f;
        const float scale = size / static_cast<float>(font.baseSize);
        const float width = inkWidth(font, text, size, spacing);
        float x = centre.x - width * 0.5f;
        const float y = centre.y - size * 0.5f;
        const float pad = static_cast<float>(font.glyphPadding);
        const float tw = static_cast<float>(font.texture.width), th = static_cast<float>(font.texture.height);
        rlSetTexture(font.texture.id);
        rlBegin(RL_QUADS);
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlNormal3f(0.0f, 0.0f, 1.0f);
        for (int i = 0; text[i] != '\0';)
        {
            int bytes = 0;
            const int codepoint = GetCodepointNext(&text[i], &bytes);
            i += bytes > 0 ? bytes : 1;
            const int index = GetGlyphIndex(font, codepoint);
            const Rectangle rec = font.recs[index];
            if (codepoint != ' ' && codepoint != '\t')
            {
                const float x0 = x + font.glyphs[index].offsetX * scale - pad * scale, y0 = y + font.glyphs[index].offsetY * scale - pad * scale;
                const float x1 = x0 + (rec.width + 2.0f * pad) * scale, y1 = y0 + (rec.height + 2.0f * pad) * scale;
                const float u0 = (rec.x - pad) / tw, v0 = (rec.y - pad) / th, u1 = (rec.x + rec.width + pad) / tw, v1 = (rec.y + rec.height + pad) / th;
                const Vector2 tl = paper.map(x0, y0), bl = paper.map(x0, y1), br = paper.map(x1, y1), tr = paper.map(x1, y0);
                rlTexCoord2f(u0, v0);
                rlVertex2f(tl.x, tl.y);
                rlTexCoord2f(u0, v1);
                rlVertex2f(bl.x, bl.y);
                rlTexCoord2f(u1, v1);
                rlVertex2f(br.x, br.y);
                rlTexCoord2f(u1, v0);
                rlVertex2f(tr.x, tr.y);
            }
            x += (font.glyphs[index].advanceX != 0 ? static_cast<float>(font.glyphs[index].advanceX) : rec.width) * scale + spacing;
        }
        rlEnd();
        rlSetTexture(0);
        return width;
    }

    void inkLine(const Vector2 *points, int count, float width, Color color, const Homography &paper, float taper)
    {
        if (count < 2)
            return;
        std::vector<float> along(static_cast<size_t>(count), 0.0f);
        for (int i = 1; i < count; ++i)
            along[static_cast<size_t>(i)] = along[static_cast<size_t>(i - 1)] +
                                            std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        const float total = std::max(along.back(), 1e-4f);
        // Each point: the line's edges and, a screen pixel further out, its feathered rim (vertex alpha to nothing),
        // so thin strokes stay smooth at any size.
        struct Edge
        {
            Vector2 outerL, innerL, innerR, outerR;
        };
        std::vector<Edge> edges(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            const Vector2 a = points[std::max(0, i - 1)], b = points[std::min(count - 1, i + 1)];
            float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::max(std::hypot(dx, dy), 1e-6f);
            dx /= len;
            dy /= len;
            float half = width * 0.5f;
            if (taper > 0.0f)
            {
                const float t = along[static_cast<size_t>(i)] / total;
                half *= std::clamp(std::min(t, 1.0f - t) / taper, 0.25f, 1.0f);
            }
            const Vector2 p = points[i], n{-dy, dx};
            const Vector2 here = paper.map(p.x, p.y), there = paper.map(p.x + n.x, p.y + n.y);
            const float pixel = 1.0f / std::max(std::hypot(there.x - here.x, there.y - here.y), 1e-4f); // paper units a pixel
            const float core = std::max(0.0f, half - pixel * 0.5f), rim = core + pixel;
            Edge &e = edges[static_cast<size_t>(i)];
            e.outerL = paper.map(p.x + n.x * rim, p.y + n.y * rim);
            e.innerL = paper.map(p.x + n.x * core, p.y + n.y * core);
            e.innerR = paper.map(p.x - n.x * core, p.y - n.y * core);
            e.outerR = paper.map(p.x - n.x * rim, p.y - n.y * rim);
        }
        // a thinner-than-a-pixel line keeps its weight as fainter ink
        const Vector2 m0 = paper.map(points[0].x, points[0].y), m1 = paper.map(points[0].x, points[0].y + width);
        const float onScreen = std::hypot(m1.x - m0.x, m1.y - m0.y);
        Color solid = color;
        if (onScreen < 1.0f)
            solid.a = static_cast<unsigned char>(color.a * std::max(0.25f, onScreen));
        Color clear = solid;
        clear.a = 0;
        const auto quad = [](Vector2 a, Color ca, Vector2 b, Color cb, Vector2 c, Color cc, Vector2 d, Color cd) {
            // a-b along one side, d-c along the other (two triangles, either winding: culling is off for ink)
            rlColor4ub(ca.r, ca.g, ca.b, ca.a);
            rlVertex2f(a.x, a.y);
            rlColor4ub(cb.r, cb.g, cb.b, cb.a);
            rlVertex2f(b.x, b.y);
            rlColor4ub(cc.r, cc.g, cc.b, cc.a);
            rlVertex2f(c.x, c.y);
            rlColor4ub(ca.r, ca.g, ca.b, ca.a);
            rlVertex2f(a.x, a.y);
            rlColor4ub(cc.r, cc.g, cc.b, cc.a);
            rlVertex2f(c.x, c.y);
            rlColor4ub(cd.r, cd.g, cd.b, cd.a);
            rlVertex2f(d.x, d.y);
        };
        rlBegin(RL_TRIANGLES);
        for (int i = 0; i + 1 < count; ++i)
        {
            const Edge &e0 = edges[static_cast<size_t>(i)], &e1 = edges[static_cast<size_t>(i + 1)];
            quad(e0.innerL, solid, e1.innerL, solid, e1.innerR, solid, e0.innerR, solid);
            quad(e0.outerL, clear, e1.outerL, clear, e1.innerL, solid, e0.innerL, solid);
            quad(e0.innerR, solid, e1.innerR, solid, e1.outerR, clear, e0.outerR, clear);
        }
        rlEnd();
    }

    void inkLoop(Rectangle box, float drawn, uint32_t seed, Color color, const Homography &paper, float width)
    {
        drawn = std::clamp(drawn, 0.0f, 1.0f);
        if (drawn <= 0.0f)
            return;
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float cx = box.x + box.width * 0.5f, cy = box.y + box.height * 0.5f;
        const float rx = box.width * 0.5f, ry = box.height * 0.5f;
        // A quick hand: starts upper left, goes round once and a bit past, the end a little inside the start;
        // two slow wobbles and a slight tilt.
        const float start = kPi * (1.08f + 0.12f * unit(random));
        const float sweep = 2.0f * kPi + 0.45f + 0.25f * unit(random);
        const float tilt = (unit(random) - 0.5f) * 0.06f;
        const float w1 = 0.035f * (0.5f + unit(random)), w2 = 0.025f * (0.5f + unit(random));
        const float p1 = unit(random) * 6.28f, p2 = unit(random) * 6.28f;
        constexpr int kSteps = 96;
        const int steps = std::max(2, static_cast<int>(kSteps * drawn));
        std::vector<Vector2> points;
        points.reserve(static_cast<size_t>(steps) + 1);
        for (int i = 0; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / kSteps;
            const float a = start + sweep * t;
            const float shrink = 1.0f - 0.07f * t; // a spiral: the end passes inside the start
            const float wob = 1.0f + w1 * std::sin(a * 2.0f + p1) + w2 * std::sin(a * 3.0f + p2);
            const float x = std::cos(a) * rx * shrink * wob, y = std::sin(a) * ry * shrink * wob;
            points.push_back(Vector2{cx + x * std::cos(tilt) - y * std::sin(tilt), cy + x * std::sin(tilt) + y * std::cos(tilt)});
        }
        // The grease pencil: waxy, a slightly uneven pressure (two passes, the second thinner and offset).
        inkLine(points.data(), static_cast<int>(points.size()), width, color, paper, 0.08f);
        for (Vector2 &p : points)
        {
            p.x += width * 0.18f;
            p.y -= width * 0.12f;
        }
        Color second = color;
        second.a = static_cast<unsigned char>(color.a * 0.45f);
        inkLine(points.data(), static_cast<int>(points.size()), width * 0.55f, second, paper, 0.15f);
    }

    void inkTickBox(Vector2 c, float size, bool ticked, Color color, const Homography &paper)
    {
        const float h = size * 0.5f, w = std::max(1.5f, size * 0.09f);
        // A hand-ruled square: each side its own stroke, a hair past the corners.
        const Vector2 sides[4][2] = {{{c.x - h - w, c.y - h + 0.4f}, {c.x + h + w * 0.5f, c.y - h - 0.3f}},
                                     {{c.x + h, c.y - h - w}, {c.x + h + 0.4f, c.y + h + w * 0.6f}},
                                     {{c.x + h + w * 0.4f, c.y + h + 0.3f}, {c.x - h - w * 0.6f, c.y + h - 0.2f}},
                                     {{c.x - h - 0.3f, c.y + h + w * 0.5f}, {c.x - h + 0.2f, c.y - h - w}}};
        for (const auto &side : sides)
            inkLine(side, 2, w, color, paper);
        if (ticked)
        {
            const Vector2 tick[3] = {{c.x - h * 0.62f, c.y + h * 0.02f}, {c.x - h * 0.12f, c.y + h * 0.55f}, {c.x + h * 0.95f, c.y - h * 0.95f}};
            inkLine(tick, 3, w * 1.5f, color, paper, 0.2f);
        }
    }


    // ---- the bill of fare ---------------------------------------------------------------------------------------------
    namespace
    {
        constexpr Color kInk{38, 26, 18, 238};
        constexpr Color kPencil{164, 30, 22, 225};
        constexpr const char *kSkip = "Skip Launcher (hold Shift while starting to see it)";
        // The fine print at the foot of the bill (K, Nov 18): the tagline, then the rights notice; a line per \n, each
        // paragraph balanced.
        constexpr const char *kLegal = "God\u00A0Hand\u2122 for PS2 Recompiled, Refined, Rearmed.\n"
                                       "God\u00A0Hand\u2122 is a trademark of CAPCOM CO.,\u00A0LTD.\n"
                                       "God Hand: Rearmed is an unofficial fan-made project, not affiliated with or endorsed by Capcom or "
                                       "Clover Studio. It contains no game data: play your own damn copy!";
        constexpr float kItemSize = 84.0f, kItemTop = 372.0f, kItemStep = 116.0f;
        // (the text keeps clear of the volume ruler down the right edge: as wide as it can be, centred, without reaching it)
        constexpr float kSkipSize = 34.0f, kSkipY = 704.0f, kBox = 29.0f, kBoxGap = 12.0f, kPaperRoom = 560.0f;
        constexpr float kLegalSize = 24.0f, kLegalStep = 27.0f, kLegalRoom = 590.0f; // (below the ruler's end: a little wider)
        constexpr Vector2 kSteamAt{70.0f, 76.0f}; // the upper left corner, clear of the title
        constexpr float kSteamR = 30.0f;
        constexpr Vector2 kMuteAt{630.0f, 76.0f}; // the upper right corner, opposite (K, Nov 18)
        constexpr float kLoopTime = 0.28f; // seconds the pencil loop takes to draw (about the pen's scribble)
        constexpr float kMuteR = 30.0f;
        // the volume ruler down the right edge, loud at the top; its ticks point in, the paperclip sits over the edge
        constexpr float kRulerX = 684.0f, kRulerTop = 150.0f, kRulerBottom = 800.0f; // (ends above the fine print)

        std::vector<int> codepointsFor(const std::string &extra)
        {
            std::vector<int> codepoints;
            for (int c = 0x20; c < 0x7F; ++c)
                codepoints.push_back(c);
            for (int c = 0xA0; c < 0x100; ++c)
                codepoints.push_back(c);
            for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122})
                codepoints.push_back(c);
            for (size_t i = 0; i < extra.size();)
            {
                int bytes = 0;
                const int c = GetCodepointNext(extra.c_str() + i, &bytes);
                i += bytes > 0 ? static_cast<size_t>(bytes) : 1u;
                if (std::find(codepoints.begin(), codepoints.end(), c) == codepoints.end())
                    codepoints.push_back(c);
            }
            return codepoints;
        }

        Font loadFont(const fs::path &path, std::vector<int> &codepoints)
        {
            std::error_code error;
            if (!fs::is_regular_file(path, error))
                return Font{};
            Font font = LoadFontEx(utf8(path).c_str(), 96, codepoints.data(), static_cast<int>(codepoints.size()));
            if (font.texture.id == 0u || font.texture.id == GetFontDefault().texture.id)
                return Font{};
            GenTextureMipmaps(&font.texture);
            SetTextureFilter(font.texture, TEXTURE_FILTER_TRILINEAR);
            return font;
        }

        // The splash line: as big as fits (shrinking to a floor), else two lines.
        struct SplashFit
        {
            std::string lines[2];
            int count = 0;
            float size = 0.0f;
        };
        SplashFit fitSplash(const Font &font, const std::string &text)
        {
            SplashFit fit;
            if (text.empty())
                return fit;
            constexpr float kMax = 58.0f, kMin = 32.0f;
            const float one = inkWidth(font, text.c_str(), kMax);
            if (one <= kPaperRoom || one * kMin / kMax <= kPaperRoom)
            {
                fit.lines[0] = text;
                fit.count = 1;
                fit.size = std::min(kMax, kMax * kPaperRoom / std::max(one, 1.0f));
                return fit;
            }
            size_t cut = std::string::npos;
            long bestDistance = 0;
            const long middle = static_cast<long>(text.size() / 2);
            for (size_t i = text.find(' '); i != std::string::npos; i = text.find(' ', i + 1))
            {
                const long distance = std::labs(static_cast<long>(i) - middle);
                if (cut == std::string::npos || distance < bestDistance)
                {
                    bestDistance = distance;
                    cut = i;
                }
            }
            if (cut == std::string::npos)
            {
                fit.lines[0] = text;
                fit.count = 1;
                fit.size = kMax * kPaperRoom / std::max(one, 1.0f);
                return fit;
            }
            fit.lines[0] = text.substr(0, cut);
            fit.lines[1] = text.substr(cut + 1);
            fit.count = 2;
            const float wide = std::max(inkWidth(font, fit.lines[0].c_str(), kMax), inkWidth(font, fit.lines[1].c_str(), kMax));
            fit.size = std::min(kMax * 0.9f, kMax * kPaperRoom / std::max(wide, 1.0f));
            return fit;
        }

        float skipSize(const Font &font)
        {
            const float wide = inkWidth(font, kSkip, kSkipSize) + kBox + kBoxGap;
            return wide > kPaperRoom ? kSkipSize * kPaperRoom / wide : kSkipSize;
        }

        // Words broken greedily into lines no wider than room.
        std::vector<std::string> wrapWords(const Font &font, const std::vector<std::string> &words, float size, float room)
        {
            std::vector<std::string> lines;
            std::string line;
            for (const std::string &word : words)
            {
                const std::string tried = line.empty() ? word : line + " " + word;
                if (!line.empty() && inkWidth(font, tried.c_str(), size) > room)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = tried;
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }

        // The rights notice broken into lines that fit the bill, each paragraph balanced (as narrow as it can go
        // without taking another line, so no word is left hanging on its own).
        const std::vector<std::string> &legalLines(const Font &font)
        {
            static std::vector<std::string> lines;
            static unsigned int madeFor = 0u;
            if (madeFor == font.texture.id && !lines.empty())
                return lines;
            madeFor = font.texture.id;
            lines.clear();
            std::vector<std::string> words;
            std::string word;
            auto paragraph = [&]() {
                if (words.empty())
                    return;
                const size_t count = wrapWords(font, words, kLegalSize, kLegalRoom).size();
                float lo = kLegalRoom * 0.4f, hi = kLegalRoom;
                for (int i = 0; i < 14; ++i)
                {
                    const float mid = (lo + hi) * 0.5f;
                    (wrapWords(font, words, kLegalSize, mid).size() <= count ? hi : lo) = mid;
                }
                for (std::string &line : wrapWords(font, words, kLegalSize, hi))
                {
                    // the no-break spaces kept words together; the font has no glyph of its own for them
                    for (size_t at; (at = line.find("\xC2\xA0")) != std::string::npos;)
                        line.replace(at, 2, " ");
                    lines.push_back(std::move(line));
                }
                words.clear();
            };
            for (const char *c = kLegal;; ++c)
            {
                if (*c == ' ' || *c == '\n' || *c == '\0')
                {
                    if (!word.empty())
                        words.push_back(word);
                    word.clear();
                    if (*c != ' ')
                        paragraph();
                    if (*c == '\0')
                        break;
                }
                else
                    word += *c;
            }
            return lines;
        }

        // Steam's mark in ink (a piston: the big wheel's hub, the arm, the small wheel), for Add to Steam.
        void inkSteamMark(Vector2 c, float r, Color color, const Homography &paper)
        {
            const auto circle = [&](Vector2 at, float radius, float width) {
                Vector2 points[33];
                for (int i = 0; i <= 32; ++i)
                {
                    const float a = kPi * 2.0f * static_cast<float>(i) / 32.0f;
                    points[i] = Vector2{at.x + std::cos(a) * radius, at.y + std::sin(a) * radius};
                }
                inkLine(points, 33, width, color, paper);
            };
            circle(c, r, r * 0.13f);
            const Vector2 hub{c.x + r * 0.30f, c.y - r * 0.24f}, small{c.x - r * 0.38f, c.y + r * 0.34f};
            circle(hub, r * 0.27f, r * 0.12f);
            circle(hub, r * 0.10f, r * 0.10f);
            circle(small, r * 0.17f, r * 0.11f);
            const Vector2 arm[2] = {{small.x + r * 0.13f, small.y - r * 0.10f}, {hub.x - r * 0.22f, hub.y + r * 0.16f}};
            inkLine(arm, 2, r * 0.17f, color, paper);
        }

        // The mute mark: a speaker, crossed out when muted, else one or two waves (over half).
        void inkSpeaker(Vector2 c, bool silent, float level, Color color, const Homography &paper)
        {
            const float mx = c.x - 25.0f, my = c.y;
            const Vector2 speaker[7] = {{mx, my - 9.0f},          {mx + 10.0f, my - 9.0f}, {mx + 24.0f, my - 22.0f}, {mx + 24.0f, my + 22.0f},
                                        {mx + 10.0f, my + 9.0f}, {mx, my + 9.0f},         {mx, my - 9.0f}};
            inkLine(speaker, 7, 4.5f, color, paper);
            if (silent)
            {
                const Vector2 a[2] = {{mx + 32.0f, my - 10.0f}, {mx + 50.0f, my + 10.0f}}, b[2] = {{mx + 32.0f, my + 10.0f}, {mx + 50.0f, my - 10.0f}};
                inkLine(a, 2, 4.0f, color, paper);
                inkLine(b, 2, 4.0f, color, paper);
                return;
            }
            for (int wave = 0; wave < (level > 0.5f ? 2 : 1); ++wave)
            {
                Vector2 arc[9];
                const float r = 14.0f + 11.0f * static_cast<float>(wave);
                for (int i = 0; i < 9; ++i)
                {
                    const float a = -0.8f + 1.6f * static_cast<float>(i) / 8.0f;
                    arc[i] = Vector2{mx + 24.0f + std::cos(a) * r, my + std::sin(a) * r};
                }
                inkLine(arc, 9, 3.5f, color, paper);
            }
        }

        // The volume ruler down the right edge: ticks every 5 %, longer every 10 and 50 (the paperclip: paintClip).
        void inkRuler(Color color, const Homography &paper)
        {
            const Vector2 line[2] = {{kRulerX, kRulerTop}, {kRulerX, kRulerBottom}};
            inkLine(line, 2, 3.0f, color, paper);
            for (int i = 0; i <= 20; ++i)
            {
                const float y = kRulerTop + (kRulerBottom - kRulerTop) * static_cast<float>(i) / 20.0f;
                const float tall = i % 10 == 0 ? 26.0f : i % 2 == 0 ? 15.0f : 9.0f;
                const Vector2 tick[2] = {{kRulerX - tall, y}, {kRulerX, y}};
                inkLine(tick, 2, 2.2f, color, paper);
            }
        }

        // The paperclip on the ruler (K: a red one, with some weight to it): a plastic-coated wire clip clipped over
        // the bill's edge at the level, two nested loops lying across the edge. A soft shadow on the paper (ink),
        // then the wire painted over it (not inked): its body, a darker underside and a light glint along the top.
        void paintClip(float level, const Homography &paper)
        {
            const float y = kRulerBottom + (kRulerTop - kRulerBottom) * std::clamp(level, 0.0f, 1.0f);
            const auto wire = [&](float w, float left, float right, Vector2 off, Vector2 *out) {
                for (int i = 0; i < 12; ++i)
                {
                    const float a = kPi * static_cast<float>(i) / 11.0f;
                    out[i] = Vector2{left - std::sin(a) * w + w + off.x, y - std::cos(a) * w + off.y};
                    out[12 + i] = Vector2{right + std::sin(a) * w - w + off.x, y + std::cos(a) * w + off.y};
                }
            };
            const auto loops = [&](Vector2 off, float width, Color color) {
                Vector2 points[24];
                wire(15.0f, kRulerX - 70.0f, 716.0f, off, points);
                inkLine(points, 24, width, color, paper);
                wire(9.5f, kRulerX - 53.0f, 706.0f, off, points);
                inkLine(points, 24, width, color, paper);
            };
            beginInk(); // the shadow, on the paper
            loops(Vector2{3.0f, 5.0f}, 9.5f, Color{60, 40, 30, 75});
            endInk();
            beginPaint();
            loops(Vector2{0.0f, 0.0f}, 7.0f, Color{124, 13, 17, 255});  // the wire, its shaded side
            loops(Vector2{-0.5f, -0.7f}, 5.0f, Color{171, 28, 30, 255}); // its lit face
            loops(Vector2{-1.0f, -1.9f}, 1.8f, Color{232, 150, 140, 200}); // the glint
            endPaint();
        }
    }

    bool loadBillFonts(BillFonts &fonts, const std::filesystem::path &folder, const std::string &extra)
    {
        std::vector<int> codepoints = codepointsFor(extra);
        fonts.heading = loadFont(folder / "Rye-Regular.ttf", codepoints);
        fonts.item = loadFont(folder / "EBGaramond-Ink.ttf", codepoints);
        fonts.text = loadFont(folder / "EBGaramond-Regular.ttf", codepoints);
        if (fonts.heading.texture.id == 0u || fonts.item.texture.id == 0u || fonts.text.texture.id == 0u)
        {
            unloadBillFonts(fonts);
            return false;
        }
        return true;
    }

    void unloadBillFonts(BillFonts &fonts)
    {
        for (Font *font : {&fonts.heading, &fonts.item, &fonts.text})
            if (font->texture.id != 0u)
            {
                UnloadFont(*font);
                *font = Font{};
            }
    }

    std::vector<Rectangle> billTargets(const BillFonts &fonts, const Bill &bill)
    {
        std::vector<Rectangle> out;
        for (size_t i = 0; i < bill.items.size(); ++i)
        {
            const float w = inkWidth(fonts.item, bill.items[i].c_str(), kItemSize);
            const float y = kItemTop + kItemStep * static_cast<float>(i);
            out.push_back(Rectangle{kPaperW * 0.5f - w * 0.5f - 34.0f, y - kItemSize * 0.52f, w + 68.0f, kItemSize * 1.04f});
        }
        if (bill.showSkip)
        {
            const float size = skipSize(fonts.text);
            const float w = inkWidth(fonts.text, kSkip, size) + kBox + kBoxGap;
            out.push_back(Rectangle{kPaperW * 0.5f - w * 0.5f - 10.0f, kSkipY - 26.0f, w + 20.0f, 52.0f});
        }
        if (bill.steam)
            out.push_back(Rectangle{kSteamAt.x - kSteamR - 8.0f, kSteamAt.y - kSteamR - 8.0f, kSteamR * 2.0f + 16.0f, kSteamR * 2.0f + 16.0f});
        if (bill.volume)
            out.push_back(Rectangle{kMuteAt.x - kMuteR - 8.0f, kMuteAt.y - kMuteR - 8.0f, kMuteR * 2.0f + 16.0f, kMuteR * 2.0f + 16.0f});
        return out;
    }

    Rectangle billRuler() { return Rectangle{kRulerX - 56.0f, kRulerTop - 14.0f, 712.0f + 6.0f - (kRulerX - 56.0f), kRulerBottom - kRulerTop + 28.0f}; }
    float billRulerLevel(float paperY) { return std::clamp((kRulerBottom - paperY) / (kRulerBottom - kRulerTop), 0.0f, 1.0f); }

    void drawBill(const BillFonts &fonts, const Bill &bill, const Homography &paper)
    {
        beginInk();
        // the head: the title and a hand-ruled line under it
        inkText(fonts.heading, "Bill of Fare", Vector2{kPaperW * 0.5f, 120.0f}, 80.0f, kInk, paper);
        const Vector2 rule[4] = {{140.0f, 189.0f}, {280.0f, 187.5f}, {420.0f, 188.6f}, {560.0f, 187.0f}};
        inkLine(rule, 4, 4.0f, kInk, paper, 0.04f);
        // this start's splash line
        const SplashFit splash = fitSplash(fonts.text, bill.splash);
        for (int i = 0; i < splash.count; ++i)
        {
            const float y = splash.count == 1 ? 252.0f : 236.0f + i * splash.size * 1.08f;
            inkText(fonts.text, splash.lines[i].c_str(), Vector2{kPaperW * 0.5f, y}, splash.size, kInk, paper);
        }
        // the menu
        for (size_t i = 0; i < bill.items.size(); ++i)
            inkText(fonts.item, bill.items[i].c_str(), Vector2{kPaperW * 0.5f, kItemTop + kItemStep * static_cast<float>(i)}, kItemSize, kInk, paper);
        // Skip Launcher, with its tick box
        Rectangle tickBox{};
        if (bill.showSkip)
        {
            const float size = skipSize(fonts.text);
            const float w = inkWidth(fonts.text, kSkip, size);
            const float left = kPaperW * 0.5f - (w + kBox + kBoxGap) * 0.5f;
            const float box = kBox * size / kSkipSize;
            inkTickBox(Vector2{left + kBox * 0.5f, kSkipY}, box, bill.skip, kInk, paper);
            inkText(fonts.text, kSkip, Vector2{left + kBox + kBoxGap + w * 0.5f, kSkipY}, size, kInk, paper);
            tickBox = Rectangle{left + kBox * 0.5f - box * 0.5f - 6.0f, kSkipY - box * 0.5f - 6.0f, box + 12.0f, box + 12.0f};
        }
        // the fine print at the foot, as far from the bottom as from the sides (K, Nov 18)
        const std::vector<std::string> &legal = legalLines(fonts.text);
        float widest = 0.0f;
        for (const std::string &line : legal)
            widest = std::max(widest, inkWidth(fonts.text, line.c_str(), kLegalSize));
        const float margin = (kPaperW - widest) * 0.5f;
        const float first = kPaperH - margin - kLegalSize * 0.5f - kLegalStep * static_cast<float>(legal.empty() ? 0 : legal.size() - 1);
        for (size_t i = 0; i < legal.size(); ++i)
            inkText(fonts.text, legal[i].c_str(), Vector2{kPaperW * 0.5f, first + kLegalStep * static_cast<float>(i)}, kLegalSize, kInk, paper);
        const std::vector<Rectangle> targets = billTargets(fonts, bill);
        const size_t skipIndex = bill.showSkip ? bill.items.size() : SIZE_MAX;
        const size_t steamIndex = bill.steam ? bill.items.size() + (bill.showSkip ? 1u : 0u) : SIZE_MAX;
        // Add to Steam: Steam's mark in the upper left corner, named under it while it has the focus
        if (bill.steam)
        {
            inkSteamMark(kSteamAt, kSteamR, kInk, paper);
            if (static_cast<size_t>(bill.focus) == steamIndex)
            {
                const char *lines[2] = {bill.steamAdded ? "Update" : "Add to", bill.steamAdded ? "on Steam" : "Steam"};
                for (int i = 0; i < 2; ++i)
                {
                    const float size = skipSize(fonts.text); // as big as the skip line (K)
                    inkText(fonts.text, lines[i], Vector2{kSteamAt.x, kSteamAt.y + kSteamR + size * 0.95f + size * static_cast<float>(i)}, size, kInk, paper);
                }
            }
        }
        // the volume: the mute mark in the upper right corner, the ruler down the right edge with its paperclip
        if (bill.volume)
        {
            inkSpeaker(kMuteAt, bill.muted || bill.level <= 0.0f, bill.level, kInk, paper);
            inkRuler(kInk, paper);
            endInk();
            paintClip(bill.muted ? 0.0f : bill.level, paper);
            beginInk();
        }
        // the focus: a red grease-pencil loop drawn round it (the skip line's: round its tick box only, K)
        if (bill.focus >= 0 && static_cast<size_t>(bill.focus) < targets.size())
        {
            Rectangle box = static_cast<size_t>(bill.focus) == skipIndex ? tickBox : targets[static_cast<size_t>(bill.focus)];
            const bool small = static_cast<size_t>(bill.focus) >= bill.items.size();
            box.x -= small ? 6.0f : 4.0f;
            box.width += small ? 12.0f : 8.0f;
            box.y -= small ? 4.0f : 0.0f;
            box.height += small ? 8.0f : 0.0f;
            inkLoop(box, bill.focusAge / kLoopTime, bill.focusSeed, kPencil, paper, small ? 4.5f : 7.0f);
        }
        endInk();
    }

    // ---- the scene ----------------------------------------------------------------------------------------------------
    struct Scene::Impl
    {
        fs::path folder;
        bool opened = false, broken = false, zoomBad = false;
        float frameW = 5120.0f, frameH = 1440.0f;
        float backMargin = 0.0f, fgMargin = 0.0f, parallaxMax = 0.015f;
        Quad billN{}, doorN{}, bookN{};
        bool hasBill = false, hasDoor = false;

        // Home
        Texture2D back{}, fg{}, lip{}, rays{}, raysWeight{}, bloom{}, bookLit{};
        Pixels bookMask, raysLuma;
        Rectangle bookRect{};
        Snacks snacks;
        Vector2 curryCentre{}, curryUp{};
        std::vector<Vector2> currySource, glassBowl, glassDrink;
        float rimY = 0.0f;
        Texture2D drinkMask{}; // the drink polygon over the bowl's box (frost)
        Rectangle glassBox{};

        // the doors
        Rectangle doorRect{};
        Texture2D doorTexture{};
        DoorStream doorStream;
        std::vector<Pixels> fling;
        int doorShown = -1, flingShown = -1;
        float flingAt = -1.0f;

        // the zoom
        std::array<ZoomFrame, kZoomFrames> zoom{};
        int frame = 0, direction = 0;
        float stepClock = 0.0f;

        // loading
        std::vector<std::unique_ptr<Job>> jobs;
        std::vector<std::thread> workers;
        std::atomic<size_t> nextJob{0};
        std::atomic<bool> stopWorkers{false};
        bool homeReady = false, zoomLoaded = false;

        // life
        float time = 0.0f, appear = 0.0f;
        // The view turns round a point between the table and the room (K): the room leans toward the cursor, the
        // table the other way. Frame pixels.
        Vector2 parallax{}, roomShift{}, tableShift{}, airShift{};
        Texture2D kana{};            // the launcher's (not unloaded here)
        float kanaBox[4]{}, kanaAspect = 1.0f;
        Pixels outsidePx;            // the outside matte, sharp (the kana's mask), until uploaded
        Texture2D outsideMask{};
        Shader kanaShader{};
        int kanaMatteLoc = -1, kanaCentreLoc = -1, kanaAxesLoc = -1;
        float overlays = 1.0f, homeLife = 1.0f, bookLight = 0.0f, volume = 0.0f;
        std::vector<bool> eaten;
        std::vector<float> eatenAt;
        int smoothieState = 0;
        float smoothieEmptyAt = -1.0f;
        std::vector<Floating> floats;
        std::vector<Mote> motes;
        // the drop on the glass: it runs down the glass's curve in spurts (a lane across the drink, 0..1), leaving a trail
        struct TrailPoint
        {
            Vector2 at; // in the glass's box, pixels
            float born;
        };
        std::vector<Vector2> drinkLocal; // the drink polygon in the glass's box, pixels
        float drinkTop = 0.0f, drinkBottom = 0.0f;
        bool dropRunning = false;
        float dropNext = 5.0f, dropLane = 0.3f, dropY = 0.0f, dropEndY = 0.0f, dropSpeed = 12.0f, dropPhase = 0.0f, dropStopAt = -1.0f;
        Vector2 dropAt{};
        std::vector<TrailPoint> trail;
        bool prewarmed = false;

        // drawing
        Shader shadowShader{}, raysShader{}, steamShader{}, frostShader{}, dustShader{};
        int raysWeightLoc = -1, raysRoomLoc = -1, raysTableLoc = -1, raysAirLoc = -1, raysLockLoc = -1, raysFeatherLoc = -1, raysSourceLoc = -1, raysSizeLoc = -1, raysTimeLoc = -1, raysMotionLoc = -1;
        int steamTimeLoc = -1, steamAspectLoc = -1, dustSizeLoc = -1, dustDriftLoc = -1, dustTimeLoc = -1, dustAmountLoc = -1;
        int frostSizeLoc = -1, frostBandLoc = -1, frostDropLoc = -1, frostTrailLoc = -1, frostBlurLoc = -1, frostBlurRectLoc = -1;
        Texture2D dot{}, vignette{}, paper{}, white{};
        float vignetteMade = -1.0f;
        float viewScale = 1.0f;
        Vector2 origin{};
        Rectangle pictureRect{};
        Tuning tuning;

        // ---------------------------------------------------------------------------------------------------------
        Vector2 frameToScreen(float x, float y) const { return Vector2{origin.x + x * viewScale, origin.y + y * viewScale}; }
        Vector2 screenToFrame(Vector2 p) const { return Vector2{(p.x - origin.x) / viewScale, (p.y - origin.y) / viewScale}; }
        Vector2 onTable(Vector2 screen) const // a point on screen in the table set's own frame pixels (its drift undone)
        {
            const Vector2 p = screenToFrame(screen);
            return Vector2{p.x - tableShift.x, p.y - tableShift.y};
        }
        Rectangle frameRect(Rectangle r) const
        {
            const Vector2 p = frameToScreen(r.x, r.y);
            return Rectangle{p.x, p.y, r.width * viewScale, r.height * viewScale};
        }

        void layout()
        {
            const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
            // 4:3 .. 32:9 fill by height (centred on the doors); narrower: the 4:3 middle fills the width.
            const float fourThree = frameH * 4.0f / 3.0f;
            viewScale = w / h >= 4.0f / 3.0f ? h / frameH : w / fourThree;
            const float doorX = hasDoor ? (doorN.p[0].x + doorN.p[1].x + doorN.p[2].x + doorN.p[3].x) * 0.25f * frameW : frameW * 0.5f;
            origin = Vector2{w * 0.5f - doorX * viewScale, (h - frameH * viewScale) * 0.5f};
            const float x0 = std::max(0.0f, origin.x), y0 = std::max(0.0f, origin.y);
            const float x1 = std::min(w, origin.x + frameW * viewScale), y1 = std::min(h, origin.y + frameH * viewScale);
            pictureRect = Rectangle{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
            // The zoom's last frames push in on the open book in 2D (K, Nov 18: its pages fill more of the height), about
            // the spread's middle; the picture's box (the bars of a narrow window) stays.
            const float push = 1.0f + kPushIn * std::clamp(static_cast<float>(frame - 4) / 4.0f, 0.0f, 1.0f);
            if (push != 1.0f)
            {
                const ZoomFrame &last = zoom[kZoomFrames - 1];
                float cy = 0.5f;
                if (last.hasPages)
                    cy = (last.pageL.p[0].y + last.pageL.p[3].y + last.pageR.p[1].y + last.pageR.p[2].y) * 0.25f;
                const Vector2 c{frameW * 0.5f, frameH * cy}, at = frameToScreen(c.x, c.y);
                viewScale *= push;
                origin = Vector2{at.x - c.x * viewScale, at.y - c.y * viewScale};
            }
        }

        // ---- loading -----------------------------------------------------------------------------------------------
        Job &add(const fs::path &path, Kind kind, int group, std::function<void(Pixels &)> done,
                 std::function<void(Pixels &)> work = {}, int threads = 1)
        {
            auto job = std::make_unique<Job>();
            job->path = path;
            job->kind = kind;
            job->group = group;
            job->done = std::move(done);
            job->work = std::move(work);
            job->threads = threads;
            jobs.push_back(std::move(job));
            return *jobs.back();
        }

        void workerLoop()
        {
            while (!stopWorkers.load())
            {
                const size_t index = nextJob.fetch_add(1);
                if (index >= jobs.size())
                    return;
                Job &job = *jobs[index];
                std::vector<uint8_t> bytes;
                bool ok = saloon_image::readFile(job.path, bytes) && saloon_image::decode(bytes, job.kind, job.pixels, job.threads);
                if (ok && job.work)
                    job.work(job.pixels);
                if (!ok)
                    KM_WARN(Boot, "launcher: saloon image unreadable: " << utf8(job.path));
                job.state.store(ok ? Job::Decoded : Job::Failed);
            }
        }

        // Main thread: uploads what the workers finished (all of Home's at once; the rest a few a frame).
        void uploads(bool all)
        {
            int budget = all ? 1 << 20 : 3;
            bool homeDone = true, restDone = true;
            for (const auto &jobPtr : jobs)
            {
                Job &job = *jobPtr;
                int state = job.state.load();
                if (state == Job::Decoded && (job.group == 0 || budget > 0))
                {
                    if (job.done)
                        job.done(job.pixels);
                    job.pixels = Pixels{};
                    job.state.store(Job::Finished);
                    state = Job::Finished;
                    if (job.group != 0)
                        --budget;
                }
                if (state == Job::Failed)
                {
                    if (job.group == 0)
                        broken = true;
                    else
                        zoomBad = true;
                }
                if (state != Job::Finished)
                {
                    if (job.group == 0)
                        homeDone = false;
                    else
                        restDone = false;
                }
            }
            if (homeDone && !homeReady && !broken)
            {
                homeReady = true;
                KM_INFO(Boot, "launcher: saloon up");
            }
            if (restDone && !zoomBad && !zoomLoaded)
                KM_INFO(Boot, "launcher: saloon zoom ready");
            zoomLoaded = restDone && !zoomBad;
        }

        // The drink as a misted glass shows it (the frost): a smoothie state's pixels inside the drink polygon, blurred
        // without the glass round it bleeding in (a masked blur), at half size; premultiplied RGBA. Worker side.
        Pixels frostedDrink(const Pixels &px, Rectangle rect) const
        {
            Pixels out;
            if (px.channels != 4 || px.empty() || glassDrink.size() < 3 || frameW <= 0.0f || rect.width <= 0.0f)
                return out;
            std::vector<Vector2> drink;
            for (const Vector2 &p : glassDrink)
                drink.push_back(Vector2{p.x * frameW, p.y * frameH});
            const int w = std::max(1, px.width / 2), h = std::max(1, px.height / 2);
            const size_t n = static_cast<size_t>(w) * h;
            std::vector<float> channel[4], inside(n, 0.0f);
            for (std::vector<float> &c : channel)
                c.assign(n, 0.0f);
            const float sx = rect.width / static_cast<float>(px.width), sy = rect.height / static_cast<float>(px.height);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (!inPolygon(drink, Vector2{rect.x + (x * 2.0f + 1.0f) * sx, rect.y + (y * 2.0f + 1.0f) * sy}))
                        continue;
                    inside[i] = 1.0f;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            const size_t at = (static_cast<size_t>(std::min(y * 2 + dy, px.height - 1)) * px.width + std::min(x * 2 + dx, px.width - 1)) * 4;
                            for (int k = 0; k < 4; ++k)
                                channel[k][i] += px.data[at + static_cast<size_t>(k)] * 0.25f;
                        }
                }
            constexpr float kSigma = 2.4f; // half-size pixels (about 5 frame pixels)
            for (std::vector<float> &c : channel)
                blur(c, w, h, kSigma);
            blur(inside, w, h, kSigma);
            out.width = w;
            out.height = h;
            out.channels = 4;
            out.data.resize(n * 4);
            for (size_t i = 0; i < n; ++i)
            {
                const float k = inside[i] > 1e-3f ? 1.0f / inside[i] : 0.0f;
                for (int c = 0; c < 4; ++c)
                    out.data[i * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(channel[c][i] * k + 0.5f, 0.0f, 255.0f));
            }
            return out;
        }

        // The plate's front lip drawn again over the front cherry pair (K, Nov 18): the pair was rendered whole, so it
        // showed over the lip; this is the table set's own pixels below the lip's edge (traced off fg, 5120x1440 frame
        // pixels), identical to what lies under it once the cherries are eaten. Main thread.
        Pixels lipFrom(const Pixels &px) const
        {
            Pixels out;
            if (px.channels != 4 || px.empty() || frameW != 5120.0f || frameH != 1440.0f)
                return out;
            const int w = static_cast<int>(kLipBox.width), h = static_cast<int>(kLipBox.height);
            out.width = w;
            out.height = h;
            out.channels = 4;
            out.data.assign(static_cast<size_t>(w) * h * 4, 0);
            for (int x = 0; x < w; ++x)
            {
                const float fx = kLipBox.x + x + 0.5f;
                float edge = kPlateLip[0].y;
                for (size_t i = 1; i < std::size(kPlateLip); ++i)
                    if (fx >= kPlateLip[i - 1].x && fx <= kPlateLip[i].x)
                        edge = kPlateLip[i - 1].y + (kPlateLip[i].y - kPlateLip[i - 1].y) * (fx - kPlateLip[i - 1].x) / (kPlateLip[i].x - kPlateLip[i - 1].x);
                    else if (fx > kPlateLip[i].x)
                        edge = kPlateLip[i].y;
                for (int y = 0; y < h; ++y)
                {
                    const float cover = std::clamp(kLipBox.y + y + 0.5f - edge + 0.5f, 0.0f, 1.0f);
                    const int sx = static_cast<int>(kLipBox.x + fgMargin) + x, sy = static_cast<int>(kLipBox.y + fgMargin) + y;
                    if (cover <= 0.0f || sx < 0 || sy < 0 || sx >= px.width || sy >= px.height)
                        continue;
                    const size_t from = (static_cast<size_t>(sy) * px.width + sx) * 4, to = (static_cast<size_t>(y) * w + x) * 4;
                    for (int k = 0; k < 4; ++k)
                        out.data[to + static_cast<size_t>(k)] = static_cast<uint8_t>(px.data[from + static_cast<size_t>(k)] * cover + 0.5f);
                }
            }
            return out;
        }

        void loadSnacks(const fs::path &dir, const Json *js, Snacks &out, int group, bool home)
        {
            if (js == nullptr)
                return;
            out.shadows = rectOf((*js)["shadows_rect"]);
            if (const Json *pieces = (*js)["pieces"]; pieces && pieces->kind == Json::Kind::Array)
            {
                out.pieces.resize(pieces->items.size());
                for (size_t i = 0; i < pieces->items.size(); ++i)
                {
                    const Json &p = pieces->items[i];
                    Piece &piece = out.pieces[i];
                    piece.name = textOf(p["name"]);
                    piece.rect = rectOf(p["rect"]);
                    add(dir / textOf(p["sprite"]), Kind::Rgba, group,
                        [&piece, home](Pixels &px) {
                            piece.sprite = upload(px, false);
                            if (home)
                                piece.hit = alphaOf(px);
                        });
                    add(dir / textOf(p["shadow"]), Kind::Gray, group, [&piece](Pixels &px) { piece.shadow = upload(px, false); });
                }
            }
            if (const Json *sm = (*js)["smoothie"]; sm && sm->kind == Json::Kind::Object)
            {
                Smoothie &s = out.smoothie;
                s.present = true;
                s.rect = rectOf((*sm)["rect"]);
                if (const Json *states = (*sm)["states"]; states && states->kind == Json::Kind::Array)
                {
                    s.states.resize(states->items.size());
                    if (home)
                    {
                        s.hit.resize(states->items.size());
                        s.blur.resize(states->items.size());
                        s.blurPx.resize(states->items.size());
                    }
                    for (size_t i = 0; i < states->items.size(); ++i)
                        add(
                            dir / states->items[i].text, Kind::Rgba, group,
                            [&s, i, home](Pixels &px) {
                                s.states[i] = upload(px, false);
                                if (home)
                                {
                                    s.hit[i] = alphaOf(px);
                                    s.blur[i] = upload(s.blurPx[i], false);
                                    s.blurPx[i] = Pixels{};
                                }
                            },
                            home ? std::function<void(Pixels &)>([this, &s, i](Pixels &px) { s.blurPx[i] = frostedDrink(px, s.rect); })
                                 : std::function<void(Pixels &)>{});
                }
                if (const Json *garnish = (*sm)["garnish"]; garnish && garnish->kind == Json::Kind::Array)
                {
                    s.garnish.resize(garnish->items.size());
                    s.garnishBehind.resize(garnish->items.size());
                    for (size_t i = 0; i < garnish->items.size(); ++i)
                        s.garnishBehind[i] = garnish->items[i].text.find("back") != std::string::npos;
                    for (size_t i = 0; i < garnish->items.size(); ++i)
                        add(dir / garnish->items[i].text, Kind::Rgba, group, [&s, i](Pixels &px) { s.garnish[i] = upload(px, false); });
                }
            }
        }

        bool open(const fs::path &dir)
        {
            folder = dir;
            std::vector<uint8_t> bytes;
            Json root;
            if (!saloon_image::supported())
            {
                KM_INFO(Boot, "launcher: no AVIF decoder in this build, plain launcher");
                return false;
            }
            if (!saloon_image::readFile(dir / "saloon_layers.json", bytes) ||
                !json::JsonReader(std::string(bytes.begin(), bytes.end())).read(root) || root.kind != Json::Kind::Object)
            {
                KM_WARN(Boot, "launcher: no saloon (" << utf8(dir / "saloon_layers.json") << " missing or unreadable)");
                return false;
            }
            if (const Json *size = root["size"]; size && size->kind == Json::Kind::Array && size->items.size() == 2)
            {
                frameW = static_cast<float>(size->items[0].number);
                frameH = static_cast<float>(size->items[1].number);
            }
            backMargin = static_cast<float>(json::number(root["back_margin"], 0.0));
            fgMargin = static_cast<float>(json::number(root["fg_margin"], 0.0));
            parallaxMax = static_cast<float>(json::number(root["parallax_max"], 0.015));
            hasBill = quadOf(root["bill"], billN);
            hasDoor = quadOf(root["door"], doorN);
            quadOf(root["book"], bookN);
            const Json *home = root["home"];
            if (home == nullptr || home->kind != Json::Kind::Object || frameW <= 0.0f || frameH <= 0.0f)
                return false;
            const fs::path homeDir = dir / "home";
            const Json *layers = (*home)["layers"];
            if (layers == nullptr || layers->kind != Json::Kind::Array || layers->items.size() < 2)
                return false;
            // Home first (group 0): the room, the table set, the rays, the openings' matte (its bloom), the book.
            add(homeDir / layers->items[0].text, Kind::Rgb, 0, [this](Pixels &px) { back = upload(px, true); }, {}, 2);
            add(homeDir / layers->items[1].text, Kind::Rgba, 0, [this](Pixels &px) {
                    lip = upload(lipFrom(px), false);
                    fg = upload(px, true);
                }, {}, 2);
            add(homeDir / textOf((*home)["rays"]), Kind::Rgb, 0, [this](Pixels &px) { rays = upload(px, true); },
                [this](Pixels &px) { raysLuma = lumaOf(px, 16); }, 2);
            add(homeDir / textOf((*home)["rays_weight"]), Kind::Gray, 0, [this](Pixels &px) { raysWeight = upload(px, false); });
            add(homeDir / textOf((*home)["outside"]), Kind::Gray, 0,
                [this](Pixels &px) {
                    bloom = upload(px, false);
                    outsideMask = upload(outsidePx, false);
                    outsidePx = Pixels{};
                },
                [this](Pixels &px) {
                    outsidePx = px; // sharp, for the kana
                    px = bloomFrom(px);
                });
            if (const Json *book = (*home)["book"]; book && book->kind == Json::Kind::Object)
            {
                bookRect = rectOf((*book)["rect"]);
                add(homeDir / textOf((*book)["lit"]), Kind::Rgba, 0, [this](Pixels &px) { bookLit = upload(px, false); });
                add(homeDir / textOf((*book)["mask"]), Kind::Gray, 0, [this](Pixels &px) { bookMask = std::move(px); });
            }
            loadSnacks(homeDir / "snacks", (*home)["snacks"], snacks, 0, true);
            eaten.assign(snacks.pieces.size(), false);
            eatenAt.assign(snacks.pieces.size(), -1.0f);
            if (const Json *curry = (*home)["curry"]; curry && curry->kind == Json::Kind::Object)
            {
                curryCentre = point((*curry)["centre"]);
                curryUp = point((*curry)["up10cm"]);
                currySource = polygonOf((*curry)["source"]);
            }
            if (const Json *glass = (*home)["glass"]; glass && glass->kind == Json::Kind::Object)
            {
                glassBowl = polygonOf((*glass)["bowl"]);
                glassDrink = polygonOf((*glass)["drink"]);
                rimY = static_cast<float>(json::number((*glass)["rim_y"], 0.0));
            }
            // The doors: the breeze loop streams from its files; the fling is decoded ahead (Play can't wait).
            const Json *doors = (*home)["doors"];
            std::vector<std::vector<uint8_t>> swayFiles;
            if (doors && doors->kind == Json::Kind::Object)
            {
                doorRect = rectOf((*doors)["rect"]);
                const auto name = [](std::string pattern, int i) {
                    char number[8];
                    std::snprintf(number, sizeof(number), "%02d", i);
                    const size_t at = pattern.find("{:02d}");
                    if (at != std::string::npos)
                        pattern.replace(at, 6, number);
                    return pattern;
                };
                const int sway = static_cast<int>(json::number((*doors)["sway"], 0.0));
                const int flings = static_cast<int>(json::number((*doors)["fling"], 0.0));
                const std::string swayPattern = textOf((*doors)["sway_pattern"]), flingPattern = textOf((*doors)["fling_pattern"]);
                for (int i = 0; i < sway; ++i)
                {
                    std::vector<uint8_t> file;
                    if (!saloon_image::readFile(homeDir / "doors" / name(swayPattern, i), file))
                    {
                        swayFiles.clear();
                        break;
                    }
                    swayFiles.push_back(std::move(file));
                }
                fling.resize(static_cast<size_t>(std::max(0, flings)));
                for (int i = 0; i < flings; ++i)
                    add(homeDir / "doors" / name(flingPattern, i), Kind::Rgba, 1, [this, i](Pixels &px) { fling[static_cast<size_t>(i)] = std::move(px); });
            }
            // The zoom: frames 1-7 at half size, 8 full; each frame's matte for its bloom; then its snacks.
            if (const Json *zoomJs = root["zoom"]; zoomJs && zoomJs->kind == Json::Kind::Object)
            {
                for (int f = 1; f <= kZoomFrames; ++f)
                {
                    char key[16];
                    std::snprintf(key, sizeof(key), "zoom%02d", f);
                    const Json *z = (*zoomJs)[key];
                    ZoomFrame &zf = zoom[static_cast<size_t>(f - 1)];
                    if (z == nullptr || z->kind != Json::Kind::Object)
                    {
                        zoomBad = true;
                        continue;
                    }
                    const fs::path zdir = dir / key;
                    zf.hasBill = quadOf((*z)["bill"], zf.bill);
                    zf.hasDoor = quadOf((*z)["door"], zf.door);
                    if (const Json *pages = (*z)["pages"])
                        zf.hasPages = quadOf((*pages)["left"], zf.pageL) && quadOf((*pages)["right"], zf.pageR);
                    if (const Json *front = (*z)["bill_in_front"]; front && front->kind == Json::Kind::Bool)
                        zf.billInFront = front->flag;
                    add(zdir / textOf((*z)["frame"]), Kind::Rgb, 1, [&zf](Pixels &px) { zf.frame = upload(px, true); }, {}, 2);
                    add(zdir / textOf((*z)["outside"]), Kind::Gray, 1, [&zf](Pixels &px) { zf.bloom = upload(px, false); },
                        [](Pixels &px) { px = bloomFrom(px); });
                }
                for (int f = 1; f <= kZoomFrames; ++f)
                {
                    char key[16];
                    std::snprintf(key, sizeof(key), "zoom%02d", f);
                    if (const Json *z = (*zoomJs)[key])
                        loadSnacks(dir / key / "snacks", (*z)["snacks"], zoom[static_cast<size_t>(f - 1)].snacks, 2, false);
                }
            }
            else
                zoomBad = true;

            // Shaders and the small made-up textures.
            shadowShader = loadShader(kShadowShader);
            raysShader = loadShader(kRaysShader);
            steamShader = loadShader(withNoise(kSteamShader).c_str());
            frostShader = loadShader(withNoise(kFrostShader).c_str());
            if (!usable(shadowShader))
            {
                KM_WARN(Boot, "launcher: saloon shaders unavailable, plain launcher");
                jobs.clear();
                return false;
            }
            raysWeightLoc = GetShaderLocation(raysShader, "weightMap");
            raysRoomLoc = GetShaderLocation(raysShader, "shiftRoom");
            raysTableLoc = GetShaderLocation(raysShader, "shiftTable");
            raysAirLoc = GetShaderLocation(raysShader, "shiftAir");
            raysLockLoc = GetShaderLocation(raysShader, "lock");
            raysFeatherLoc = GetShaderLocation(raysShader, "feather");
            raysSourceLoc = GetShaderLocation(raysShader, "source");
            raysSizeLoc = GetShaderLocation(raysShader, "size");
            raysTimeLoc = GetShaderLocation(raysShader, "time");
            raysMotionLoc = GetShaderLocation(raysShader, "motion");
            steamTimeLoc = GetShaderLocation(steamShader, "time");
            steamAspectLoc = GetShaderLocation(steamShader, "aspect");
            kanaShader = loadShader(kKanaShader);
            dustShader = loadShader(withNoise(kDustShader).c_str());
            dustSizeLoc = GetShaderLocation(dustShader, "size");
            dustDriftLoc = GetShaderLocation(dustShader, "drift");
            dustTimeLoc = GetShaderLocation(dustShader, "time");
            dustAmountLoc = GetShaderLocation(dustShader, "amount");
            kanaMatteLoc = GetShaderLocation(kanaShader, "matte");
            kanaCentreLoc = GetShaderLocation(kanaShader, "centre");
            kanaAxesLoc = GetShaderLocation(kanaShader, "axes");
            frostSizeLoc = GetShaderLocation(frostShader, "size");
            frostBandLoc = GetShaderLocation(frostShader, "band");
            frostBlurLoc = GetShaderLocation(frostShader, "blurMap");
            frostBlurRectLoc = GetShaderLocation(frostShader, "blurRect");
            frostDropLoc = GetShaderLocation(frostShader, "drop");
            frostTrailLoc = GetShaderLocation(frostShader, "trail");
            makeTextures();
            makeMotes();

            const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
            const unsigned count = std::clamp(cores - 1u, 1u, 4u);
            for (unsigned i = 0; i < count; ++i)
                workers.emplace_back([this] { workerLoop(); });
            doorStream.start(std::move(swayFiles));
            doorStream.showing(-1);
            opened = true;
            KM_INFO(Boot, "launcher: saloon loading (" << jobs.size() << " images, " << count << " threads)");
            return true;
        }

        void makeTextures()
        {
            // a soft dot (motes)
            {
                Image image = GenImageColor(64, 64, BLANK);
                auto *px = static_cast<Color *>(image.data);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x)
                    {
                        const float r = std::hypot(x - 31.5f, y - 31.5f) / 31.5f;
                        const float a = std::exp(-r * r * 4.5f) * (1.0f - std::clamp((r - 0.85f) / 0.15f, 0.0f, 1.0f));
                        const unsigned char v = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                        px[y * 64 + x] = Color{v, v, v, v}; // premultiplied
                    }
                dot = LoadTextureFromImage(image);
                UnloadImage(image);
                GenTextureMipmaps(&dot);
                SetTextureFilter(dot, TEXTURE_FILTER_TRILINEAR);
            }
            // plain paper (the corner slips): warm, with soft blotches and fibres; tiles
            {
                constexpr int kSize = 256;
                Image image = GenImageColor(kSize, kSize, BLANK);
                auto *px = static_cast<Color *>(image.data);
                const auto hash = [](int x, int y, int s) {
                    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(s) * 2246822519u;
                    h = (h ^ (h >> 13)) * 1274126177u;
                    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
                };
                const auto smooth = [&](float x, float y, int period, int s) {
                    const float fx = x * period / kSize, fy = y * period / kSize;
                    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                    const float tx = fx - x0, ty = fy - y0, ux = tx * tx * (3 - 2 * tx), uy = ty * ty * (3 - 2 * ty);
                    const auto h = [&](int xx, int yy) { return hash(((xx % period) + period) % period, ((yy % period) + period) % period, s); };
                    return (h(x0, y0) * (1 - ux) + h(x0 + 1, y0) * ux) * (1 - uy) + (h(x0, y0 + 1) * (1 - ux) + h(x0 + 1, y0 + 1) * ux) * uy;
                };
                for (int y = 0; y < kSize; ++y)
                    for (int x = 0; x < kSize; ++x)
                    {
                        const float blot = smooth(static_cast<float>(x), static_cast<float>(y), 4, 1) * 0.55f +
                                           smooth(static_cast<float>(x), static_cast<float>(y), 9, 2) * 0.3f +
                                           smooth(static_cast<float>(x), static_cast<float>(y), 32, 3) * 0.15f;
                        const float fibre = smooth(static_cast<float>(x), static_cast<float>(y) * 0.25f, 64, 4);
                        const float grain = hash(x, y, 5);
                        const float k = std::clamp(0.62f + 0.38f * blot - 0.05f * fibre - 0.04f * grain, 0.0f, 1.0f);
                        px[y * kSize + x] = Color{static_cast<unsigned char>(150 + 75 * k), static_cast<unsigned char>(128 + 76 * k),
                                                  static_cast<unsigned char>(92 + 72 * k), 255};
                    }
                paper = LoadTextureFromImage(image);
                UnloadImage(image);
                GenTextureMipmaps(&paper);
                SetTextureFilter(paper, TEXTURE_FILTER_TRILINEAR);
                SetTextureWrap(paper, TEXTURE_WRAP_REPEAT);
            }
            {
                Image image = GenImageColor(1, 1, WHITE);
                white = LoadTextureFromImage(image);
                UnloadImage(image);
            }
            // the drink's mask over the glass's box (anti-aliased, 4x4 samples a pixel)
            if (glassBowl.size() >= 3 && glassDrink.size() >= 3)
            {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const Vector2 &p : glassBowl)
                {
                    x0 = std::min(x0, p.x * frameW);
                    y0 = std::min(y0, p.y * frameH);
                    x1 = std::max(x1, p.x * frameW);
                    y1 = std::max(y1, p.y * frameH);
                }
                glassBox = Rectangle{std::floor(x0) - 4.0f, std::floor(y0) - 4.0f, std::ceil(x1 - x0) + 8.0f, std::ceil(y1 - y0) + 8.0f};
                const int w = static_cast<int>(glassBox.width), h = static_cast<int>(glassBox.height);
                std::vector<Vector2> drink;
                for (const Vector2 &p : glassDrink)
                    drink.push_back(Vector2{p.x * frameW - glassBox.x, p.y * frameH - glassBox.y});
                drinkLocal = drink;
                drinkTop = 1e9f;
                drinkBottom = -1e9f;
                for (const Vector2 &p : drink)
                {
                    drinkTop = std::min(drinkTop, p.y);
                    drinkBottom = std::max(drinkBottom, p.y);
                }
                Image image = GenImageColor(w, h, BLANK);
                auto *px = static_cast<Color *>(image.data);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        int in = 0;
                        for (int sy = 0; sy < 4; ++sy)
                            for (int sx = 0; sx < 4; ++sx)
                                in += inPolygon(drink, Vector2{x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f});
                        px[y * w + x] = Color{255, 255, 255, static_cast<unsigned char>(in * 255 / 16)};
                    }
                drinkMask = LoadTextureFromImage(image);
                UnloadImage(image);
                SetTextureFilter(drinkMask, TEXTURE_FILTER_BILINEAR);
                SetTextureWrap(drinkMask, TEXTURE_WRAP_CLAMP);
            }
        }

        void makeVignette()
        {
            if (vignette.id != 0u && vignetteMade == tuning.vignette)
                return;
            unload(vignette);
            vignetteMade = tuning.vignette;
            constexpr int kSize = 128;
            Image image = GenImageColor(kSize, kSize, WHITE);
            auto *px = static_cast<Color *>(image.data);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    const float dx = (x + 0.5f) / kSize * 2.0f - 1.0f, dy = (y + 0.5f) / kSize * 2.0f - 1.0f;
                    const float r = std::sqrt(dx * dx * 0.82f + dy * dy * 1.0f);
                    const float t = std::clamp((r - 0.45f) / 0.85f, 0.0f, 1.0f);
                    const float v = 1.0f - tuning.vignette * t * t * (3.0f - 2.0f * t);
                    const unsigned char c = static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
                    px[y * kSize + x] = Color{c, c, c, 255};
                }
            vignette = LoadTextureFromImage(image);
            UnloadImage(image);
            SetTextureFilter(vignette, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(vignette, TEXTURE_WRAP_CLAMP);
        }

        void makeMotes()
        {
            std::mt19937 random(0x5A1007u);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            motes.resize(kMoteCount);
            for (size_t i = 0; i < static_cast<size_t>(kMoteCount); ++i)
            {
                Mote &m = motes[i];
                // in the air between us and the doorway, mostly in the light's cone
                const float u = unit(random), v = unit(random);
                m.base = Vector2{frameW * (0.5f + (u - 0.5f) * (0.30f + 0.35f * v)), frameH * (0.08f + 0.70f * v)};
                m.swing = Vector2{frameW * (0.015f + 0.04f * unit(random)), frameH * (0.04f + 0.10f * unit(random))};
                m.px = 22.0f + 40.0f * unit(random);
                m.py = 17.0f + 33.0f * unit(random);
                m.p2 = 7.0f + 9.0f * unit(random);
                m.phase1 = unit(random) * 6.28f;
                m.phase2 = unit(random) * 6.28f;
                m.phase3 = unit(random) * 6.28f;
                m.depth = unit(random);
                m.size = 2.2f + 5.5f * m.depth * m.depth;
                m.bright = 0.35f + 0.65f * unit(random);
            }
        }

        // ---- the clocks --------------------------------------------------------------------------------------------
        void update(const Frame &in)
        {
            if (!opened)
                return;
            uploads(false);
            if (!homeReady)
                return;
            const float dt = std::clamp(in.dt, 0.0f, 0.1f);
            time += dt;
            appear = std::min(1.0f, appear + dt / 0.35f);
            volume = in.volume;
            parallax = in.parallax;
            const float reach0 = parallaxMax * frameH, reach = reach0 * tuning.drift;
            const float tableShare = std::clamp(tuning.tableShare, 0.0f, 1.0f);
            const Vector2 lean = frame == 0 ? parallax : Vector2{0.0f, 0.0f};
            roomShift = Vector2{lean.x * reach * (1.0f - tableShare), lean.y * reach * (1.0f - tableShare)};
            tableShift = Vector2{-lean.x * reach * tableShare, -lean.y * reach * tableShare};
            // what hangs in the air (the rays' near ends, the dust) against the room; the logo out past the doorway
            airShift = Vector2{-lean.x * reach0 * tuning.airDrift, -lean.y * reach0 * tuning.airDrift};

            // the zoom: stepped frames
            if (direction != 0)
            {
                stepClock += dt;
                const float step = 1.0f / std::clamp(tuning.zoomFps, 4.0f, 60.0f);
                while (direction != 0 && stepClock >= step)
                {
                    stepClock -= step;
                    frame += direction;
                    if (frame >= kZoomFrames)
                    {
                        frame = kZoomFrames;
                        direction = 0;
                    }
                    else if (frame <= 0)
                    {
                        frame = 0;
                        direction = 0;
                    }
                }
            }
            const bool home = frame == 0 && direction == 0;
            const float toward = home ? 1.0f : 0.0f;
            overlays += std::clamp(toward - overlays, -dt / 0.15f, dt / 0.25f);
            homeLife += std::clamp(toward - homeLife, -dt / 0.15f, dt / 0.6f);
            const float lit = in.bookLit && home ? 1.0f : 0.0f;
            bookLight += std::clamp(lit - bookLight, -dt / 0.2f, dt / 0.2f);

            // the doors
            if (flingAt >= 0.0f)
            {
                const int k = std::min(static_cast<int>(fling.size()) - 1, static_cast<int>((time - flingAt) * 24.0f));
                if (k >= 0 && k != flingShown && !fling[static_cast<size_t>(k)].empty())
                {
                    showDoor(fling[static_cast<size_t>(k)]);
                    flingShown = k;
                }
            }
            else if (doorStream.count() > 0 && frame == 0) // (no uploads while the zoom shows)
            {
                const int k = static_cast<int>(time * 24.0f) % doorStream.count();
                if (k != doorShown)
                {
                    Pixels pixels;
                    if (doorStream.take(k, pixels))
                        showDoor(pixels);
                    doorShown = k;
                    doorStream.showing(k);
                }
            }

            // the snacks' floats
            floats.erase(std::remove_if(floats.begin(), floats.end(), [&](const Floating &f) { return time - f.start >= kFloatTime; }),
                         floats.end());

            updateDrop(dt);
        }

        // The drink's left and right edges at a height (the glass's box, pixels).
        bool drinkEdges(float y, float &left, float &right) const
        {
            left = 1e9f;
            right = -1e9f;
            for (size_t i = 0, j = drinkLocal.size() - 1; i < drinkLocal.size(); j = i++)
            {
                const Vector2 a = drinkLocal[i], b = drinkLocal[j];
                if ((a.y > y) == (b.y > y))
                    continue;
                const float x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
                left = std::min(left, x);
                right = std::max(right, x);
            }
            return right > left;
        }

        // Every 10-20 s while the drink is cold (once the last trail has fogged over): a drop runs down the glass in
        // spurts, keeping its lane across the drink so it follows the glass's curve in.
        void updateDrop(float dt)
        {
            static std::mt19937 random(0xD20Bu);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            if (drinkLocal.size() < 3)
                return;
            if (!dropRunning && dropStopAt < 0.0f && time >= dropNext && smoothieState < 4)
            {
                dropRunning = true;
                dropLane = unit(random) < 0.5f ? 0.12f + 0.22f * unit(random) : 0.66f + 0.22f * unit(random);
                dropY = drinkTop + 3.0f + 4.0f * unit(random);
                dropEndY = drinkBottom - 6.0f - 10.0f * unit(random);
                dropSpeed = 9.0f + 6.0f * unit(random);
                dropPhase = unit(random) * 6.28f;
                trail.clear();
            }
            if (dropRunning)
            {
                // spurts: it sticks, lets go, sticks again, a little faster each time (it gathers water as it goes)
                const float spurt = std::max(0.0f, std::sin(dropPhase));
                dropPhase += dt * 6.28f / 0.95f;
                dropSpeed += dt * 4.0f;
                dropY += dt * dropSpeed * spurt * spurt * 2.2f;
                float left = 0.0f, right = 0.0f;
                if (dropY >= dropEndY || !drinkEdges(dropY, left, right))
                {
                    dropRunning = false;
                    dropStopAt = time; // it stops low on the glass and soaks back into the film
                }
                else
                {
                    dropAt = Vector2{left + dropLane * (right - left), dropY};
                    if (trail.empty() || std::hypot(dropAt.x - trail.back().at.x, dropAt.y - trail.back().at.y) > 4.5f)
                    {
                        trail.push_back(TrailPoint{dropAt, time});
                        if (trail.size() > 24)
                            trail.erase(trail.begin());
                    }
                }
            }
            if (dropStopAt >= 0.0f && time - dropStopAt > 12.0f) // the trail has fogged over: the next one later
            {
                dropStopAt = -1.0f;
                trail.clear();
                dropNext = time + 4.0f + 6.0f * unit(random);
            }
        }

        void showDoor(const Pixels &pixels)
        {
            if (pixels.empty())
                return;
            if (doorTexture.id == 0u || doorTexture.width != pixels.width || doorTexture.height != pixels.height)
            {
                unload(doorTexture);
                doorTexture = upload(pixels, false);
            }
            else
                UpdateTexture(doorTexture, pixels.data.data());
        }

        // ---- drawing -----------------------------------------------------------------------------------------------
        void drawAt(const Texture2D &texture, Rectangle frameRectangle, Color tint = WHITE) const
        {
            if (texture.id == 0u)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           frameRect(frameRectangle), Vector2{0.0f, 0.0f}, 0.0f, tint);
        }

        float pieceShadow(int i) const
        {
            if (!eaten[static_cast<size_t>(i)])
                return 1.0f;
            return std::max(0.0f, 1.0f - (time - eatenAt[static_cast<size_t>(i)]) / kShadowFade);
        }

        // A frame's snacks: shadows multiplied in, then the uneaten pieces back to front, then the smoothie.
        void drawSnacks(const Snacks &s, Vector2 offset, bool home = false)
        {
            const auto nameEaten = [&](const std::string &name, float &shade) {
                for (size_t i = 0; i < snacks.pieces.size(); ++i)
                    if (snacks.pieces[i].name == name)
                    {
                        shade = pieceShadow(static_cast<int>(i));
                        return static_cast<bool>(eaten[i]);
                    }
                shade = 1.0f;
                return false;
            };
            if (s.shadows.width > 0.0f && usable(shadowShader))
            {
                BeginShaderMode(shadowShader);
                beginMultiply();
                for (const Piece &p : s.pieces)
                {
                    float shade = 1.0f;
                    nameEaten(p.name, shade);
                    if (shade > 0.0f && p.shadow.id != 0u)
                        drawAt(p.shadow, Rectangle{s.shadows.x + offset.x, s.shadows.y + offset.y, s.shadows.width, s.shadows.height},
                               Color{255, 255, 255, static_cast<unsigned char>(shade * 255.0f)});
                }
                EndBlendMode();
                EndShaderMode();
            }
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            for (const Piece &p : s.pieces)
            {
                float shade = 1.0f;
                if (!nameEaten(p.name, shade))
                    drawAt(p.sprite, Rectangle{p.rect.x + offset.x, p.rect.y + offset.y, p.rect.width, p.rect.height});
            }
            EndBlendMode();
            if (home)
                drawFloats(tuning.floatHeight * frameH, offset, true); // (under the smoothie: the back slice)
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            if (s.smoothie.present && !s.smoothie.states.empty())
            {
                const size_t state = std::min(static_cast<size_t>(smoothieState), s.smoothie.states.size() - 1);
                const Rectangle r = s.smoothie.rect;
                drawAt(s.smoothie.states[state], Rectangle{r.x + offset.x, r.y + offset.y, r.width, r.height});
            }
            EndBlendMode();
        }

        // The eaten snacks floating off; behind: only the garnishes that go under the smoothie's picture, else the rest.
        void drawFloats(float rise, Vector2 offset, bool behind)
        {
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            for (const Floating &f : floats)
            {
                const bool under = f.garnish >= 0 && static_cast<size_t>(f.garnish) < snacks.smoothie.garnishBehind.size() &&
                                   snacks.smoothie.garnishBehind[static_cast<size_t>(f.garnish)];
                if (under != behind)
                    continue;
                const float u = std::clamp((time - f.start) / kFloatTime, 0.0f, 1.0f), e = 1.0f - (1.0f - u) * (1.0f - u);
                const unsigned char a = static_cast<unsigned char>((1.0f - u) * 255.0f);
                const Color tint{a, a, a, a};
                if (f.piece >= 0)
                {
                    const Piece &p = snacks.pieces[static_cast<size_t>(f.piece)];
                    drawAt(p.sprite, Rectangle{p.rect.x + offset.x, p.rect.y + offset.y - rise * e, p.rect.width, p.rect.height}, tint);
                }
                else if (f.garnish >= 0 && static_cast<size_t>(f.garnish) < snacks.smoothie.garnish.size())
                {
                    const Rectangle r = snacks.smoothie.rect;
                    drawAt(snacks.smoothie.garnish[static_cast<size_t>(f.garnish)],
                           Rectangle{r.x + offset.x, r.y + offset.y - rise * e, r.width, r.height}, tint);
                }
            }
            EndBlendMode();
        }

        // (Blend mode first, then the shader, then its extra texture: raylib forgets a shader's extra textures at every
        // batch flush, and changing the blend mode flushes.)
        void drawRays()
        {
            if (rays.id == 0u)
                return;
            const Rectangle r{-backMargin, -backMargin, static_cast<float>(rays.width), static_cast<float>(rays.height)};
            const unsigned char strength = static_cast<unsigned char>(std::clamp(tuning.rays, 0.0f, 1.0f) * 255.0f);
            const bool shaded = usable(raysShader) && raysWeight.id != 0u;
            beginScreen();
            if (shaded)
            {
                BeginShaderMode(raysShader);
                const float room[2] = {roomShift.x / rays.width, roomShift.y / rays.height};
                const float table[2] = {tableShift.x / rays.width, tableShift.y / rays.height};
                const float air[2] = {airShift.x / rays.width, airShift.y / rays.height};
                // the doorway's rays fan out from its upper glare
                const float source[2] = {(0.5f * frameW + backMargin) / rays.width, (0.27f * frameH + backMargin) / rays.height};
                const float size[2] = {static_cast<float>(rays.width), static_cast<float>(rays.height)};
                SetShaderValue(raysShader, raysRoomLoc, room, SHADER_UNIFORM_VEC2);
                SetShaderValue(raysShader, raysTableLoc, table, SHADER_UNIFORM_VEC2);
                SetShaderValue(raysShader, raysAirLoc, air, SHADER_UNIFORM_VEC2);
                const float lockBox[4] = {(doorRect.x + backMargin) / rays.width, (doorRect.y + backMargin) / rays.height,
                                          (doorRect.x + doorRect.width + backMargin) / rays.width,
                                          (doorRect.y + doorRect.height + backMargin) / rays.height};
                const float feather[2] = {160.0f / rays.width, 160.0f / rays.height};
                SetShaderValue(raysShader, raysLockLoc, lockBox, SHADER_UNIFORM_VEC4);
                SetShaderValue(raysShader, raysFeatherLoc, feather, SHADER_UNIFORM_VEC2);
                SetShaderValue(raysShader, raysSourceLoc, source, SHADER_UNIFORM_VEC2);
                SetShaderValue(raysShader, raysSizeLoc, size, SHADER_UNIFORM_VEC2);
                SetShaderValue(raysShader, raysTimeLoc, &time, SHADER_UNIFORM_FLOAT);
                SetShaderValue(raysShader, raysMotionLoc, &tuning.raysMotion, SHADER_UNIFORM_FLOAT);
                SetShaderValueTexture(raysShader, raysWeightLoc, raysWeight);
                drawAt(rays, r, Color{255, 255, 255, strength});
                EndShaderMode();
            }
            else // without the shader the rays drift with the room whole
                drawAt(rays, Rectangle{r.x + roomShift.x, r.y + roomShift.y, r.width, r.height}, Color{strength, strength, strength, 255});
            EndBlendMode();
        }

        // The kana outside, behind the doors and the frame (Home frame only): the logo canvas's place (the logo itself stays put),
        // sized and moved by the tuning, the cursor drifting it as something far off.
        void drawKana()
        {
            if (kana.id == 0u || outsideMask.id == 0u || !usable(kanaShader) || frame != 0 || !hasDoor)
                return;
            Quad frameQuad{};
            for (int i = 0; i < 4; ++i)
                frameQuad.p[i] = Vector2{doorN.p[i].x * frameW, doorN.p[i].y * frameH};
            const Homography door = Homography::rectToQuad(1.0f, 1.0f, frameQuad);
            const Vector2 centre = door.map(tuning.logoU, tuning.logoV);
            const Vector2 left = door.map(0.0f, tuning.logoV), right = door.map(1.0f, tuning.logoV);
            const float content = std::hypot(right.x - left.x, right.y - left.y) * tuning.logoWidth;
            const float canvasW = content / std::max(0.05f, kanaBox[2] - kanaBox[0]), canvasH = canvasW * kanaAspect;
            // the logo canvas, in frame pixels
            const float lx = centre.x - canvasW * (kanaBox[0] + kanaBox[2]) * 0.5f, ly = centre.y - canvasH * (kanaBox[1] + kanaBox[3]) * 0.5f;
            const float reach = parallaxMax * frameH * tuning.kanaDrift;
            const float w = canvasW * tuning.kanaSize, h = canvasH * tuning.kanaSize;
            const float cx = lx + canvasW * 0.5f + tuning.kanaX * canvasW + parallax.x * reach;
            const float cy = ly + canvasH * 0.5f + tuning.kanaY * canvasW + parallax.y * reach;
            const Vector2 middle{cx + roomShift.x, cy + roomShift.y};
            // the matte covers the back layer's rectangle (and moves with it); the kana turned by KanaTurn (degrees,
            // clockwise) about its middle, as raylib turns a texture
            const Rectangle matte{-backMargin + roomShift.x, -backMargin + roomShift.y, frameW + 2.0f * backMargin, frameH + 2.0f * backMargin};
            const float turn = tuning.kanaTurn * kPi / 180.0f, c = std::cos(turn), sn = std::sin(turn);
            const float middleUv[2] = {(middle.x - matte.x) / matte.width, (middle.y - matte.y) / matte.height};
            const float axes[4] = {c * w / matte.width, sn * w / matte.height, -sn * h / matte.width, c * h / matte.height};
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            BeginShaderMode(kanaShader);
            SetShaderValue(kanaShader, kanaCentreLoc, middleUv, SHADER_UNIFORM_VEC2);
            SetShaderValue(kanaShader, kanaAxesLoc, axes, SHADER_UNIFORM_VEC4);
            SetShaderValueTexture(kanaShader, kanaMatteLoc, outsideMask);
            const Vector2 at = frameToScreen(middle.x, middle.y);
            const float sw = w * viewScale, shh = h * viewScale;
            DrawTexturePro(kana, Rectangle{0.0f, 0.0f, static_cast<float>(kana.width), static_cast<float>(kana.height)},
                           Rectangle{at.x, at.y, sw, shh}, Vector2{sw * 0.5f, shh * 0.5f}, tuning.kanaTurn, WHITE);
            EndShaderMode();
            EndBlendMode();
        }

        void drawDust(Vector2 room)
        {
            const float strength = homeLife * tuning.dust;
            if (strength <= 0.01f || rays.id == 0u || !usable(dustShader))
                return;
            const Rectangle r{-backMargin + room.x, -backMargin + room.y, static_cast<float>(rays.width), static_cast<float>(rays.height)};
            const float size[2] = {static_cast<float>(rays.width), static_cast<float>(rays.height)};
            const float drift[2] = {airShift.x / rays.width, airShift.y / rays.height};
            beginScreen();
            BeginShaderMode(dustShader);
            SetShaderValue(dustShader, dustSizeLoc, size, SHADER_UNIFORM_VEC2);
            SetShaderValue(dustShader, dustDriftLoc, drift, SHADER_UNIFORM_VEC2);
            SetShaderValue(dustShader, dustTimeLoc, &time, SHADER_UNIFORM_FLOAT);
            SetShaderValue(dustShader, dustAmountLoc, &strength, SHADER_UNIFORM_FLOAT);
            drawAt(rays, r);
            EndShaderMode();
            EndBlendMode();
        }

        void drawBloom(const Texture2D &texture, Rectangle at)
        {
            if (texture.id == 0u || tuning.bloom <= 0.0f)
                return;
            const float k = std::clamp(tuning.bloom, 0.0f, 2.0f) * 0.5f;
            beginScreen();
            drawAt(texture, at,
                   Color{static_cast<unsigned char>(std::min(255.0f, 255.0f * k)), static_cast<unsigned char>(std::min(255.0f, 236.0f * k)),
                         static_cast<unsigned char>(std::min(255.0f, 205.0f * k)), 255});
            EndBlendMode();
        }

        void drawMotes()
        {
            const float life = homeLife * tuning.motes;
            if (life <= 0.01f || dot.id == 0u || raysLuma.empty())
                return;
            BeginBlendMode(BLEND_ADDITIVE);
            for (const Mote &m : motes)
            {
                const float t = time;
                // in the air between the table and the room: their drift between the two by depth, and against the room
                const float k = 0.3f + 0.6f * m.depth;
                const Vector2 drift{tableShift.x + (roomShift.x - tableShift.x) * k + airShift.x * (1.0f - k),
                                    tableShift.y + (roomShift.y - tableShift.y) * k + airShift.y * (1.0f - k)};
                const float x = m.base.x + m.swing.x * (std::sin(t * 6.2832f / m.px + m.phase1) + 0.35f * std::sin(t * 6.2832f / m.p2 + m.phase2)) + drift.x;
                const float y = m.base.y + m.swing.y * std::sin(t * 6.2832f / m.py + m.phase3) + 0.25f * m.swing.y * std::sin(t * 0.9f + m.phase2) +
                                drift.y;
                const float light = sample(raysLuma, (x + backMargin) / (frameW + 2.0f * backMargin), (y + backMargin) / (frameH + 2.0f * backMargin));
                const float twinkle = 0.75f + 0.25f * std::sin(t * (1.3f + m.depth) + m.phase3 * 3.0f);
                const float a = std::clamp((light - 0.03f) * 3.0f, 0.0f, 1.0f) * m.bright * twinkle * life;
                if (a < 0.01f)
                    continue;
                const float size = m.size * (1.0f + 0.3f * std::sin(t * 0.7f + m.phase1));
                const Vector2 c = frameToScreen(x, y);
                const float s = size * viewScale;
                const unsigned char v = static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                DrawTexturePro(dot, Rectangle{0.0f, 0.0f, 64.0f, 64.0f}, Rectangle{c.x - s * 0.5f, c.y - s * 0.5f, s, s}, Vector2{0.0f, 0.0f},
                               0.0f, Color{255, 242, 214, v});
            }
            EndBlendMode();
        }

        void drawSteam()
        {
            const float strength = homeLife * volume * tuning.steam;
            if (strength <= 0.01f || !usable(steamShader) || currySource.empty())
                return;
            float x0 = 1e9f, x1 = -1e9f;
            for (const Vector2 &p : currySource)
            {
                x0 = std::min(x0, p.x);
                x1 = std::max(x1, p.x);
            }
            const float width = (x1 - x0) * frameW * 1.6f;
            const float up = std::hypot(curryUp.x * frameW, curryUp.y * frameH);
            const float cx = curryCentre.x * frameW + tableShift.x, cy = curryCentre.y * frameH + tableShift.y;
            const Rectangle r{cx - width * 0.5f, cy - up * 1.7f, width, up * 1.7f + up * 0.06f};
            const float aspect = r.width / r.height;
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            BeginShaderMode(steamShader);
            SetShaderValue(steamShader, steamTimeLoc, &time, SHADER_UNIFORM_FLOAT);
            SetShaderValue(steamShader, steamAspectLoc, &aspect, SHADER_UNIFORM_FLOAT);
            drawAt(white, r, Color{255, 255, 255, static_cast<unsigned char>(std::clamp(strength, 0.0f, 1.0f) * 255.0f)});
            EndShaderMode();
            EndBlendMode();
        }

        void drawFrost()
        {
            // the cold drink gone (the last sip), the film dries off over a few seconds
            float strength = homeLife * tuning.frost;
            if (smoothieState >= 4)
                strength *= smoothieEmptyAt < 0.0f ? 0.0f : std::max(0.0f, 1.0f - (time - smoothieEmptyAt) / 4.0f);
            if (strength <= 0.01f || !usable(frostShader) || drinkMask.id == 0u)
                return;
            float drop[4] = {0.0f, 0.0f, 1.7f, 0.0f};
            if (dropRunning || (dropStopAt >= 0.0f && time - dropStopAt < 0.8f))
            {
                drop[0] = dropAt.x / glassBox.width;
                drop[1] = dropAt.y / glassBox.height;
                drop[3] = dropRunning ? 1.0f : 1.0f - (time - dropStopAt) / 0.8f;
            }
            float points[24 * 4] = {};
            for (size_t i = 0; i < trail.size() && i < 24; ++i)
            {
                points[i * 4 + 0] = trail[i].at.x / glassBox.width;
                points[i * 4 + 1] = trail[i].at.y / glassBox.height;
                points[i * 4 + 2] = time - trail[i].born;
                points[i * 4 + 3] = 1.0f;
            }
            const Smoothie &sm = snacks.smoothie;
            if (sm.blur.empty())
                return;
            const Texture2D &blurred = sm.blur[std::min(static_cast<size_t>(std::max(0, smoothieState)), sm.blur.size() - 1)];
            if (blurred.id == 0u || sm.rect.width <= 0.0f || sm.rect.height <= 0.0f)
                return;
            const float size[2] = {glassBox.width, glassBox.height};
            const float band[2] = {(rimY * frameH - glassBox.y) / glassBox.height, drinkBottom / glassBox.height};
            const float blurRect[4] = {(glassBox.x - sm.rect.x) / sm.rect.width, (glassBox.y - sm.rect.y) / sm.rect.height,
                                       glassBox.width / sm.rect.width, glassBox.height / sm.rect.height};
            // (blend mode, shader, then its extra texture: see drawRays)
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            BeginShaderMode(frostShader);
            SetShaderValue(frostShader, frostSizeLoc, size, SHADER_UNIFORM_VEC2);
            SetShaderValue(frostShader, frostBandLoc, band, SHADER_UNIFORM_VEC2);
            SetShaderValue(frostShader, frostBlurRectLoc, blurRect, SHADER_UNIFORM_VEC4);
            SetShaderValue(frostShader, frostDropLoc, drop, SHADER_UNIFORM_VEC4);
            SetShaderValueV(frostShader, frostTrailLoc, points, SHADER_UNIFORM_VEC4, 24);
            SetShaderValueTexture(frostShader, frostBlurLoc, blurred);
            drawAt(drinkMask, Rectangle{glassBox.x + tableShift.x, glassBox.y + tableShift.y, glassBox.width, glassBox.height},
                   Color{255, 255, 255, static_cast<unsigned char>(std::clamp(strength, 0.0f, 1.0f) * 255.0f)});
            EndShaderMode();
            EndBlendMode();
        }

        void drawVignette()
        {
            makeVignette();
            if (vignette.id == 0u || tuning.vignette <= 0.0f)
                return;
            beginMultiply();
            DrawTexturePro(vignette, Rectangle{0.0f, 0.0f, static_cast<float>(vignette.width), static_cast<float>(vignette.height)}, pictureRect,
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndBlendMode();
        }

        void drawHome()
        {
            // the room (and the doors in it) drifting with the cursor
            const Vector2 room = roomShift, table = tableShift;
            drawAt(back, Rectangle{-backMargin + room.x, -backMargin + room.y, static_cast<float>(back.width), static_cast<float>(back.height)});
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            if (doorTexture.id != 0u)
                drawAt(doorTexture, Rectangle{doorRect.x + room.x, doorRect.y + room.y, doorRect.width, doorRect.height});
            EndBlendMode();
            drawDust(room);
            BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
            // the table set leans the other way
            drawAt(fg, Rectangle{-fgMargin + table.x, -fgMargin + table.y, static_cast<float>(fg.width), static_cast<float>(fg.height)});
            EndBlendMode();
            drawSnacks(snacks, table, true);
            if (bookLight > 0.0f && bookLit.id != 0u)
            {
                BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
                const unsigned char a = static_cast<unsigned char>(std::clamp(bookLight, 0.0f, 1.0f) * 255.0f);
                drawAt(bookLit, Rectangle{bookRect.x + table.x, bookRect.y + table.y, bookRect.width, bookRect.height}, Color{a, a, a, a});
                EndBlendMode();
            }
            drawFloats(tuning.floatHeight * frameH, table, false);
            if (lip.id != 0u) // over the pieces and their floats: an eaten pair rises from behind the lip
            {
                BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
                drawAt(lip, Rectangle{kLipBox.x + table.x, kLipBox.y + table.y, kLipBox.width, kLipBox.height});
                EndBlendMode();
            }
            drawFrost();
            drawSteam();
            drawRays();
            drawBloom(bloom, Rectangle{-backMargin + room.x, -backMargin + room.y, frameW + 2.0f * backMargin, frameH + 2.0f * backMargin});
            // (over the glow, or it washes out to white; kept to the openings by the matte, which the frame and the
            // batwings at rest cover)
            drawKana();
            drawMotes();
        }

        void drawZoom(int f)
        {
            ZoomFrame &z = zoom[static_cast<size_t>(f - 1)];
            drawAt(z.frame, Rectangle{0.0f, 0.0f, frameW, frameH});
            drawSnacks(z.snacks, Vector2{0.0f, 0.0f});
            drawBloom(z.bloom, Rectangle{0.0f, 0.0f, frameW, frameH});
        }

        void draw(float dim)
        {
            layout();
            ClearBackground(BLACK);
            if (!homeReady)
                return;
            BeginScissorMode(static_cast<int>(pictureRect.x), static_cast<int>(pictureRect.y), static_cast<int>(std::ceil(pictureRect.width)),
                             static_cast<int>(std::ceil(pictureRect.height)));
            if (zoomLoaded && !prewarmed) // a tiny draw of every zoom texture under Home, so the first zoom doesn't hitch
            {
                prewarmed = true;
                for (const ZoomFrame &z : zoom)
                {
                    const Rectangle tiny{pictureRect.x, pictureRect.y, 2.0f, 2.0f};
                    for (const Texture2D *t : {&z.frame, &z.bloom})
                        if (t->id != 0u)
                            DrawTexturePro(*t, Rectangle{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, tiny, Vector2{0, 0}, 0.0f, WHITE);
                    for (const Piece &p : z.snacks.pieces)
                        for (const Texture2D *t : {&p.sprite, &p.shadow})
                            if (t->id != 0u)
                                DrawTexturePro(*t, Rectangle{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, tiny, Vector2{0, 0}, 0.0f, WHITE);
                }
            }
            if (frame == 0)
                drawHome();
            else
                drawZoom(frame);
            drawVignette();
            const float dark = std::clamp(1.0f - (1.0f - dim) * appear, 0.0f, 1.0f);
            if (dark > 0.0f)
                DrawRectangleRec(pictureRect, Color{0, 0, 0, static_cast<unsigned char>(dark * 255.0f)});
            EndScissorMode();
        }

        // ---- input -------------------------------------------------------------------------------------------------
        Bite snackClick(Vector2 screen)
        {
            if (!homeReady || frame != 0 || direction != 0 || !floats.empty())
                return Bite::None;
            const Vector2 p = onTable(screen);
            for (int i = static_cast<int>(snacks.pieces.size()) - 1; i >= 0; --i)
            {
                const Piece &piece = snacks.pieces[static_cast<size_t>(i)];
                if (eaten[static_cast<size_t>(i)])
                    continue;
                const int x = static_cast<int>(p.x - piece.rect.x), y = static_cast<int>(p.y - piece.rect.y);
                if (at(piece.hit, x, y) > 110)
                {
                    eaten[static_cast<size_t>(i)] = true;
                    eatenAt[static_cast<size_t>(i)] = time;
                    floats.push_back(Floating{i, -1, time});
                    return Bite::Fruit;
                }
            }
            const Smoothie &s = snacks.smoothie;
            if (s.present && smoothieState < 4 && static_cast<size_t>(smoothieState) < s.hit.size())
            {
                const int x = static_cast<int>(p.x - s.rect.x), y = static_cast<int>(p.y - s.rect.y);
                if (at(s.hit[static_cast<size_t>(smoothieState)], x, y) > 48) // the glass counts too
                {
                    if (smoothieState < 3)
                        floats.push_back(Floating{-1, smoothieState, time});
                    ++smoothieState;
                    if (smoothieState >= 4)
                    {
                        smoothieEmptyAt = time;
                        return Bite::Drink;
                    }
                    return Bite::Smoothie;
                }
            }
            return Bite::None;
        }

        bool snackHit(Vector2 screen) const
        {
            if (!homeReady || frame != 0)
                return false;
            const Vector2 p = onTable(screen);
            for (size_t i = 0; i < snacks.pieces.size(); ++i)
                if (!eaten[i] && at(snacks.pieces[i].hit, static_cast<int>(p.x - snacks.pieces[i].rect.x),
                                    static_cast<int>(p.y - snacks.pieces[i].rect.y)) > 110)
                    return true;
            const Smoothie &s = snacks.smoothie;
            return s.present && smoothieState < 4 && static_cast<size_t>(smoothieState) < s.hit.size() &&
                   at(s.hit[static_cast<size_t>(smoothieState)], static_cast<int>(p.x - s.rect.x), static_cast<int>(p.y - s.rect.y)) > 48;
        }

        bool bookHit(Vector2 screen) const
        {
            if (!homeReady || frame != 0 || bookMask.empty())
                return false;
            const Vector2 p = onTable(screen);
            return at(bookMask, static_cast<int>(p.x - bookRect.x), static_cast<int>(p.y - bookRect.y)) > 127;
        }

        // ---- teardown ----------------------------------------------------------------------------------------------
        void close()
        {
            stopWorkers.store(true);
            for (std::thread &t : workers)
                if (t.joinable())
                    t.join();
            workers.clear();
            doorStream.stop();
            jobs.clear();
            opened = false;
            homeReady = false;
            if (!IsWindowReady()) // the window (and its textures) already gone
                return;
            for (Texture2D *t : {&back, &fg, &lip, &outsideMask, &rays, &raysWeight, &bloom, &bookLit, &doorTexture, &dot, &vignette, &paper, &white, &drinkMask})
                unload(*t);
            const auto freeSnacks = [](Snacks &s) {
                for (Piece &p : s.pieces)
                {
                    unload(p.sprite);
                    unload(p.shadow);
                }
                for (Texture2D &t : s.smoothie.states)
                    unload(t);
                for (Texture2D &t : s.smoothie.garnish)
                    unload(t);
                for (Texture2D &t : s.smoothie.blur)
                    unload(t);
            };
            freeSnacks(snacks);
            for (ZoomFrame &z : zoom)
            {
                unload(z.frame);
                unload(z.bloom);
                freeSnacks(z.snacks);
            }
            for (Shader *s : {&shadowShader, &raysShader, &steamShader, &frostShader, &kanaShader, &dustShader})
                unload(*s);
            unload(g_inkShader);
            g_inkTried = false;
            jobs.clear();
            opened = false;
            homeReady = false;
        }
    };

    // ---- Scene ----------------------------------------------------------------------------------------------------------
    Scene::Scene() : m(std::make_unique<Impl>()) {}
    Scene::~Scene()
    {
        if (m)
            m->close();
    }

    bool Scene::open(const std::filesystem::path &folder)
    {
        if (m->open(folder))
            return true;
        m->close();
        m->broken = true;
        return false;
    }
    bool Scene::failed() const { return m->broken; }
    bool Scene::ready() const { return m->homeReady && !m->broken; }

    void Scene::finishLoading()
    {
        if (!m->opened)
            return;
        for (std::thread &t : m->workers)
            if (t.joinable())
                t.join();
        m->workers.clear();
        m->uploads(true);
    }

    void Scene::close() { m->close(); }
    void Scene::update(const Frame &frame) { m->update(frame); }
    void Scene::draw(float dim) { m->draw(dim); }
    Rectangle Scene::picture() const { return m->pictureRect; }
    float Scene::scale() const { return m->viewScale; }
    Vector2 Scene::toScreen(Vector2 n) const { return m->frameToScreen(n.x * m->frameW, n.y * m->frameH); }

    bool Scene::spread(Quad &left, Quad &right) const
    {
        const Impl &s = *m;
        if (!s.homeReady || s.frame != kZoomFrames)
            return false;
        const ZoomFrame &z = s.zoom[kZoomFrames - 1];
        if (!z.hasPages || !sane(z.pageL) || !sane(z.pageR))
            return false;
        for (int i = 0; i < 4; ++i)
        {
            left.p[i] = toScreen(z.pageL.p[i]);
            right.p[i] = toScreen(z.pageR.p[i]);
        }
        return true;
    }

    bool Scene::bill(Quad &out) const
    {
        const Impl &s = *m;
        if (!s.homeReady)
            return false;
        Quad q{};
        if (s.frame == 0)
        {
            if (!s.hasBill)
                return false;
            q = s.billN;
            for (Vector2 &p : q.p)
                p = Vector2{p.x + s.tableShift.x / s.frameW, p.y + s.tableShift.y / s.frameH};
        }
        else
        {
            const ZoomFrame &z = s.zoom[static_cast<size_t>(s.frame - 1)];
            if (!z.hasBill || !z.billInFront || !sane(z.bill))
                return false;
            q = z.bill;
        }
        for (int i = 0; i < 4; ++i)
            out.p[i] = toScreen(q.p[i]);
        return true;
    }

    bool Scene::logo(const float box[4], float canvasAspect, Rectangle &out) const
    {
        const Impl &s = *m;
        if (!s.homeReady)
            return false;
        Quad q{};
        Vector2 drift{0.0f, 0.0f};
        if (s.frame == 0)
        {
            if (!s.hasDoor)
                return false;
            q = s.doorN;
            drift = s.roomShift; // (no drift of its own: the logo stays put, K)
        }
        else
        {
            const ZoomFrame &z = s.zoom[static_cast<size_t>(s.frame - 1)];
            if (!z.hasDoor || !sane(z.door))
                return false;
            q = z.door;
        }
        Quad frameQuad{};
        for (int i = 0; i < 4; ++i)
            frameQuad.p[i] = Vector2{q.p[i].x * s.frameW, q.p[i].y * s.frameH};
        const Homography door = Homography::rectToQuad(1.0f, 1.0f, frameQuad);
        const Vector2 centre = door.map(s.tuning.logoU, s.tuning.logoV);
        const Vector2 left = door.map(0.0f, s.tuning.logoV), right = door.map(1.0f, s.tuning.logoV);
        const float doorWidth = std::hypot(right.x - left.x, right.y - left.y);
        const float content = doorWidth * s.tuning.logoWidth;
        const float canvasW = content / std::max(0.05f, box[2] - box[0]), canvasH = canvasW * canvasAspect;
        const float x = centre.x + drift.x - canvasW * (box[0] + box[2]) * 0.5f, y = centre.y + drift.y - canvasH * (box[1] + box[3]) * 0.5f;
        const Vector2 tl = s.frameToScreen(x, y);
        out = Rectangle{tl.x, tl.y, canvasW * s.viewScale, canvasH * s.viewScale};
        return out.y + out.height > s.pictureRect.y && out.y < s.pictureRect.y + s.pictureRect.height;
    }

    void Scene::setKana(const Texture2D &kana, const float contentBox[4], float canvasAspect)
    {
        m->kana = kana;
        for (int i = 0; i < 4; ++i)
            m->kanaBox[i] = contentBox[i];
        m->kanaAspect = canvasAspect;
    }


    float Scene::overlays() const { return m->homeReady ? m->overlays : 0.0f; }
    bool Scene::bookHit(Vector2 screen) const { return m->bookHit(screen); }
    Scene::Bite Scene::snackClick(Vector2 screen) { return m->snackClick(screen); }
    bool Scene::snackHit(Vector2 screen) const { return m->snackHit(screen); }
    bool Scene::zoomReady() const { return m->zoomLoaded; }
    bool Scene::zoomBroken() const { return m->zoomBad; }

    void Scene::zoomIn()
    {
        Impl &s = *m;
        if (!s.zoomLoaded)
            return;
        if (s.frame == 0 && s.direction == 0)
        {
            s.frame = 1; // the room snaps to centre on frame 1
            s.direction = 1;
            s.stepClock = 0.0f;
        }
        else if (s.direction < 0)
        {
            s.direction = 1;
            s.stepClock = 0.0f;
        }
    }

    void Scene::zoomOut()
    {
        Impl &s = *m;
        if (s.frame == 0)
            return;
        // from the frame showing: the next one back at once
        s.frame -= 1;
        s.direction = s.frame > 0 ? -1 : 0;
        s.stepClock = 0.0f;
    }

    int Scene::zoomFrame() const { return m->frame; }
    bool Scene::zoomMoving() const { return m->direction != 0; }
    bool Scene::zoomingIn() const { return m->direction > 0; }
    bool Scene::atHome() const { return m->frame == 0 && m->direction == 0; }
    bool Scene::landed() const { return m->frame == kZoomFrames && m->direction == 0; }

    void Scene::fling()
    {
        Impl &s = *m;
        if (s.flingAt < 0.0f && !s.fling.empty() && !s.fling[0].empty())
        {
            s.flingAt = s.time;
            s.flingShown = -1;
        }
    }

    Tuning &Scene::tuning() { return m->tuning; }
    const Tuning &Scene::tuning() const { return m->tuning; }

    const Texture2D &Scene::paper() const { return m->paper; }
}
