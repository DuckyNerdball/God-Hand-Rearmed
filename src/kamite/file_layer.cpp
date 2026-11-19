// The file layer: game files replaced whole, served at the read (see file_layer.h).
#include "kamite/file_layer.h"
#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kamite::file_layer
{
    namespace
    {
        constexpr uint32_t kFnameRange = 0x00324640u;  // ADXF_GetFnameRangeEx
        constexpr uint32_t kStreamBind = 0x00327E88u;  // ADXSTM's bind (the fields; under ADXSTM_BindFile / Nw)
        constexpr uint32_t kFstSize = 0x001FFD98u;     // BuildPackedAddr_1FFD98: a path table entry's size
        constexpr uint32_t kReadToBuffer = 0x00200B88u; // cDvd_ReadToBuffer
        constexpr uint32_t kLoadEntry = 0x001FFD00u;    // LoadEntryByValue_1FFD00 (FileBuf_init's fonts)
        constexpr uint32_t kFstTable = 0x00580D40u;    // the path table (func_001FF858)
        constexpr uint32_t kDvdRomName = 0x004545B0u;  // "DVD-ROM" in DVCI's data: the whole disc
        constexpr uint32_t kAfsPartition = 0u;         // _DATA.AFS's CRI partition

        std::map<std::string, Replaced> g_replaced;          // disc path -> replacement
        std::unordered_map<uint32_t, Replaced> g_byIndex;    // archive index -> replacement
        PS2Runtime *g_runtime = nullptr;
        bool g_hooked = false;
        GuestFunction g_origFnameRange = nullptr, g_origStreamBind = nullptr;

        std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        uint32_t r32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }
        void w32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (a != 0u)
                std::memcpy(rdram + (a & (PS2_RAM_MASK & ~3u)), &v, 4);
        }
        std::string guestString(const uint8_t *rdram, uint32_t a, size_t max = 260)
        {
            std::string s;
            for (uint32_t p = a & PS2_RAM_MASK; s.size() < max && p < PS2_RDRAM_SIZE && rdram[p]; ++p)
                s.push_back(static_cast<char>(rdram[p]));
            return s;
        }

        // Partition 0's file name, as CRI hands it out: "_DATA.AFS" (a device, folder or ";1" around it allowed).
        bool isDataAfs(const std::string &name)
        {
            std::string leaf = lower(name);
            if (const size_t semi = leaf.find(';'); semi != std::string::npos)
                leaf.resize(semi);
            if (const size_t cut = leaf.find_last_of("/\\:"); cut != std::string::npos)
                leaf.erase(0, cut + 1);
            return leaf == "_data.afs";
        }

        // ADXF_GetFnameRangeEx: a replaced file of _DATA.AFS lies where it is served.
        void onFnameRange(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t partition = GPR_U32(ctx, 4), index = GPR_U32(ctx, 5), name = GPR_U32(ctx, 6);
            const uint32_t first = GPR_U32(ctx, 8), sectors = GPR_U32(ctx, 9), bytes = GPR_U32(ctx, 10);
            g_origFnameRange(rdram, ctx, runtime);
            if (partition != kAfsPartition || static_cast<int32_t>(GPR_U32(ctx, 2)) < 0)
                return;
            const auto it = g_byIndex.find(index);
            if (it == g_byIndex.end())
                return;
            if (const std::string afs = guestString(rdram, name); !isDataAfs(afs))
            {
                static bool told = false;
                if (!told)
                    KM_WARN(File, "file layer: partition 0 is \"" << afs << "\", not _DATA.AFS; replacements not served");
                told = true;
                return;
            }
            w32(rdram, first, it->second.lba);
            w32(rdram, sectors, (it->second.size + PS2CdImage::kSectorSize - 1u) / PS2CdImage::kSectorSize);
            w32(rdram, bytes, it->second.size);
        }

        // ADXSTM's bind: a range in a served file is read from the whole disc ("DVD-ROM": LBA = the range's sector).
        void onStreamBind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (PS2ServedFiles::isServed(GPR_U32(ctx, 7)))
            {
                SET_GPR_U32(ctx, 5, kDvdRomName);
                SET_GPR_U32(ctx, 6, 0u);
            }
            g_origStreamBind(rdram, ctx, runtime);
        }

        // The path table's size of a file (the original's arithmetic), the replacement's for a replaced one.
        void onFstSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t table = GPR_U32(ctx, 4), entry = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            uint32_t size = 0u;
            if (entry + 12u <= PS2_RDRAM_SIZE)
            {
                uint16_t low = 0;
                std::memcpy(&low, rdram + entry + 2u, 2);
                size = ((static_cast<uint32_t>(rdram[entry + 1u]) << 16) + low) << 4;
                if (table == kFstTable && rdram[entry] == 2u)
                    if (const auto it = g_byIndex.find(r32(rdram, entry + 8u)); it != g_byIndex.end())
                        size = (it->second.size + 15u) & ~15u;
            }
            SET_GPR_U32(ctx, 2, size);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // cDvd_ReadToBuffer(a0 cDvd, a1 path, a2 buffer, a3 id, t0 size or 0): a replaced file read into a buffer the
        // caller made. A heap block's header sits 0x20 before its data (+0x10 used bytes): a buffer smaller than the
        // replacement is cut to (t0) so the read cannot run past it into the next block, which hung the game (Oct 27,
        // Gene's buffer from a save state made with a smaller file).
        void onReadToBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string path;
            const Replaced *r = replacedPath(rdram, GPR_U32(ctx, 5), &path);
            if (!r)
                return;
            const uint32_t buffer = GPR_U32(ctx, 6) & PS2_RAM_MASK, asked = GPR_U32(ctx, 8);
            const uint32_t used = buffer >= 0x20u ? r32(rdram, buffer - 0x20u + 0x10u) : 0u;
            if (asked != 0u && asked < r->size)
                KM_WARN(Game, "file layer: " << path << " read into a buffer with " << asked << " bytes asked; the replacement is "
                                             << r->size << " (cut short)");
            else if (used > 0x20u && used - 0x20u < r->size)
            {
                const uint32_t room = (used - 0x20u) & ~0x7FFu;
                SET_GPR_U32(ctx, 8, room);
                KM_ERROR(Game, "file layer: " << path << " (" << r->size << " bytes) does not fit the game's buffer of " << (used - 0x20u)
                                              << " bytes (ra 0x" << std::hex << GPR_U32(ctx, 31) << std::dec << "); read cut to " << room
                                              << ". Was a save state made with another build or other mods? Restart the game.");
            }
        }

        // LoadEntryByValue_1FFD00(a0 path table, a1 path, a2 buffer) reads the whole file (FileBuf_init: etc/moji8.tm2 or
        // moji81.tm2 into a 4 KB heap block). It can't be cut; a replacement larger than the block is reported.
        void onLoadEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string path;
            const Replaced *r = replacedPath(rdram, GPR_U32(ctx, 5), &path);
            const uint32_t buffer = GPR_U32(ctx, 6) & PS2_RAM_MASK;
            const uint32_t used = r && buffer >= 0x20u ? r32(rdram, buffer - 0x20u + 0x10u) : 0u;
            if (used > 0x20u && used - 0x20u < ((r->size + 0x7FFu) & ~0x7FFu))
                KM_ERROR(Game, "file layer: " << path << " (" << r->size << " bytes) is read whole into the game's buffer of "
                                              << (used - 0x20u) << " bytes; the read runs past it");
        }

        void hook()
        {
            if (g_hooked || !g_runtime || g_byIndex.empty())
                return;
            uint8_t *rdram = g_runtime->memory().getRDRAM();
            if (std::memcmp(rdram + kDvdRomName, "DVD-ROM", 8) != 0 || !g_runtime->hasFunction(kFnameRange) ||
                !g_runtime->hasFunction(kStreamBind) || !g_runtime->hasFunction(kFstSize) ||
                !(g_origFnameRange = g_runtime->lookupFunction(kFnameRange)) || !(g_origStreamBind = g_runtime->lookupFunction(kStreamBind)))
            {
                KM_ERROR(File, "file layer: this game's CRI file system is not the one expected; replaced files are not served");
                return;
            }
            FunctionHooks &hooks = FunctionHooks::instance();
            const bool ok = hooks.replace(kFnameRange, "ADXF_GetFnameRangeEx (file layer)", &onFnameRange, ReplacementStatus::Native,
                                          "replaced files lie where they are served") &&
                            hooks.replace(kStreamBind, "ADXSTM bind (file layer)", &onStreamBind, ReplacementStatus::Native,
                                          "a served file is read from the whole disc") &&
                            hooks.replace(kFstSize, "path table file size (file layer)", &onFstSize, ReplacementStatus::Native,
                                          "a replaced file's own size");
            if (!ok)
            {
                KM_ERROR(File, "file layer: the hooks could not be installed; replaced files are not served");
                return;
            }
            hooks.observeEntry(kReadToBuffer, "file_layer_read_to_buffer", &onReadToBuffer);
            hooks.observeEntry(kLoadEntry, "file_layer_load_entry", &onLoadEntry);
            g_hooked = true;
            KM_INFO(File, "file layer: " << g_byIndex.size() << " replaced file(s) served");
        }

        bool add(std::string_view path, uint32_t lba, uint32_t size, const std::string &from)
        {
            const auto file = disc_files::find(path);
            if (!file)
                return false;
            const std::string key = lower(path);
            if (lba == 0u)
            {
                KM_WARN(File, "file layer: " << key << ": " << from << " can't be read or is empty; the game's file stays");
                return false;
            }
            const Replaced r{file->index, file->size, size, lba};
            g_replaced[key] = r;
            g_byIndex[file->index] = r;
            KM_INFO(File, "file layer: " << key << " (AFS " << file->index << ", " << file->size << " -> " << size << " bytes) from " << from);
            hook();
            return true;
        }
    }

    bool replace(std::string_view path, const fs::path &looseFile)
    {
        if (!disc_files::open() || !disc_files::find(path))
            return false;
        std::error_code error;
        const uint64_t size = fs::file_size(looseFile, error);
        if (error || size > 0xFFFFFFFFull)
            return add(path, 0u, 0u, utf8(looseFile));
        return add(path, PS2ServedFiles::serve(looseFile), static_cast<uint32_t>(size), utf8(looseFile));
    }

    bool replace(std::string_view path, std::vector<uint8_t> bytes)
    {
        if (!disc_files::open() || !disc_files::find(path) || bytes.size() > 0xFFFFFFFFull)
            return false;
        const uint32_t size = static_cast<uint32_t>(bytes.size());
        return add(path, PS2ServedFiles::serve(std::move(bytes)), size, "memory");
    }

    int replaceFolder(const fs::path &folder, const std::string &who)
    {
        if (!disc_files::open())
        {
            KM_WARN(File, who << ": the disc image has no file archive / path table; nothing replaced");
            return 0;
        }
        // Sorted, so the same files are served at the same LBAs on every run (a save state stays valid).
        std::vector<std::pair<std::string, fs::path>> files;
        std::error_code error;
        for (const auto &entry : fs::recursive_directory_iterator(folder, error))
            if (entry.is_regular_file(error))
            {
                const std::string rel = lower(utf8(fs::relative(entry.path(), folder, error)));
                if (disc_files::paths().count(rel))
                    files.emplace_back(rel, entry.path());
            }
        std::sort(files.begin(), files.end());
        int count = 0;
        for (const auto &[rel, file] : files)
            count += replace(rel, file) ? 1 : 0;
        if (count != 0)
            KM_INFO(File, who << ": " << count << " game file(s) replaced");
        return count;
    }

    const std::map<std::string, Replaced> &replaced() { return g_replaced; }

    const Replaced *replacedPath(const uint8_t *rdram, uint32_t pathAddress, std::string *pathOut)
    {
        if (g_replaced.empty())
            return nullptr;
        std::string path = lower(guestString(rdram, pathAddress, 128));
        for (char &c : path)
            if (c == '\\')
                c = '/';
        if (const size_t semi = path.find(';'); semi != std::string::npos)
            path.resize(semi);
        if (const size_t colon = path.find(':'); colon != std::string::npos)
            path.erase(0, colon + 1);
        while (!path.empty() && (path[0] == '/' || path[0] == '.'))
            path.erase(0, 1);
        // The full path, or (when the game names a folder the table doesn't) what follows any '/' in it.
        for (size_t from = 0;;)
        {
            if (const auto it = g_replaced.find(path.substr(from)); it != g_replaced.end())
            {
                if (pathOut)
                    *pathOut = it->first;
                return &it->second;
            }
            const size_t slash = path.find('/', from);
            if (slash == std::string::npos)
                return nullptr;
            from = slash + 1;
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        hook();
    }
}
