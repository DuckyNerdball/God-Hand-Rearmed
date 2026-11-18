#include "kamite/saloon_image.h"

#include <algorithm>
#include <fstream>

#if defined(KAMITE_HAS_AVIF) && KAMITE_HAS_AVIF
#include <avif/avif.h>
#endif

namespace kamite::saloon_image
{
    bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &bytes)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size <= 0)
            return false;
        bytes.resize(static_cast<size_t>(size));
        in.seekg(0, std::ios::beg);
        return static_cast<bool>(in.read(reinterpret_cast<char *>(bytes.data()), size));
    }

#if defined(KAMITE_HAS_AVIF) && KAMITE_HAS_AVIF
    bool supported() { return true; }

    bool decode(const std::vector<uint8_t> &file, Kind kind, Pixels &out, int threads)
    {
        out = Pixels{};
        avifDecoder *decoder = avifDecoderCreate();
        if (decoder == nullptr)
            return false;
        decoder->maxThreads = threads < 1 ? 1 : threads;
        decoder->ignoreExif = AVIF_TRUE;
        decoder->ignoreXMP = AVIF_TRUE;
        bool ok = avifDecoderSetIOMemory(decoder, file.data(), file.size()) == AVIF_RESULT_OK &&
                  avifDecoderParse(decoder) == AVIF_RESULT_OK && avifDecoderNextImage(decoder) == AVIF_RESULT_OK;
        if (ok)
        {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, decoder->image);
            rgb.depth = 8;
            rgb.format = kind == Kind::Rgba ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
            rgb.alphaPremultiplied = kind == Kind::Rgba ? AVIF_TRUE : AVIF_FALSE;
            rgb.maxThreads = decoder->maxThreads;
            ok = avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK;
            if (ok)
            {
                ok = avifImageYUVToRGB(decoder->image, &rgb) == AVIF_RESULT_OK;
                if (ok)
                {
                    out.width = static_cast<int>(rgb.width);
                    out.height = static_cast<int>(rgb.height);
                    const size_t pixels = static_cast<size_t>(out.width) * static_cast<size_t>(out.height);
                    const int from = kind == Kind::Rgba ? 4 : 3;
                    out.channels = kind == Kind::Rgba ? 4 : kind == Kind::Rgb ? 3 : 1;
                    out.data.resize(pixels * static_cast<size_t>(out.channels));
                    for (uint32_t y = 0; y < rgb.height; ++y)
                    {
                        const uint8_t *row = rgb.pixels + static_cast<size_t>(y) * rgb.rowBytes;
                        uint8_t *dst = out.data.data() + static_cast<size_t>(y) * static_cast<size_t>(out.width) * out.channels;
                        if (out.channels == from)
                            std::copy(row, row + static_cast<size_t>(out.width) * from, dst);
                        else
                            for (int x = 0; x < out.width; ++x)
                                dst[x] = row[static_cast<size_t>(x) * from]; // R of a one-channel map
                    }
                    // An image without alpha decoded as RGBA: libavif fills alpha with 255 already.
                }
                avifRGBImageFreePixels(&rgb);
            }
        }
        avifDecoderDestroy(decoder);
        if (!ok)
            out = Pixels{};
        return ok;
    }
#else
    bool supported() { return false; }

    bool decode(const std::vector<uint8_t> &, Kind, Pixels &out, int)
    {
        out = Pixels{};
        return false;
    }
#endif
}
