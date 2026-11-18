#pragma once

// The launcher's saloon art (resources/saloon, claude/saloon-launcher.md): AVIF images decoded to plain pixels on any
// thread (libavif + dav1d, fetched at configure time; KAMITE_HAS_AVIF). No raylib here: the launcher uploads them.
#include <cstdint>
#include <filesystem>
#include <vector>

namespace kamite::saloon_image
{
    enum class Kind
    {
        Rgb,  // 3 bytes a pixel (the opaque layers)
        Rgba, // 4 bytes, alpha premultiplied (sprites: no dark or coloured fringes when filtered)
        Gray, // 1 byte (masks, mattes, shadow multipliers: one-channel AVIFs decode as RGB, R is kept)
    };

    struct Pixels
    {
        int width = 0, height = 0, channels = 0;
        std::vector<uint8_t> data;
        bool empty() const { return data.empty(); }
    };

    bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes);

    // False when the bytes aren't an image this build can decode (or the build has no AVIF decoder).
    bool decode(const std::vector<uint8_t> &file, Kind kind, Pixels &out, int threads = 1);

    // Whether this build decodes AVIF at all.
    bool supported();
}
