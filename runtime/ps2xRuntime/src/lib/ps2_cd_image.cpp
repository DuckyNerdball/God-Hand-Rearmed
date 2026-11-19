#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

namespace
{
    uint32_t le32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool seekFile(std::FILE *file, uint64_t offset)
    {
#if defined(_WIN32)
        return offset <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
               _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
        return offset <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
               fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }
}

PS2CdImage &PS2CdImage::instance()
{
    static PS2CdImage image;
    return image;
}

std::string PS2CdImage::normalizeKey(std::string_view ps2Path)
{
    std::string_view path = ps2Path;
    const size_t colon = path.find(':');
    if (colon != std::string_view::npos && colon < 8)
    {
        path.remove_prefix(colon + 1);
    }
    const size_t version = path.rfind(';');
    if (version != std::string_view::npos)
    {
        path = path.substr(0, version);
    }
    std::string key;
    key.reserve(path.size() + 1);
    for (char c : path)
    {
        if (c == '\\')
        {
            c = '/';
        }
        if (c == '/' && (key.empty() || key.back() == '/'))
        {
            continue;
        }
        key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    while (!key.empty() && (key.back() == '/' || key.back() == '.'))
    {
        key.pop_back();
    }
    return key;
}

bool PS2CdImage::open(const std::filesystem::path &imagePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openAttempted = true;
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_entries.clear();
#if defined(_WIN32)
    m_file = ::_wfopen(imagePath.c_str(), L"rb");
#else
    m_file = std::fopen(imagePath.string().c_str(), "rb");
#endif
    if (!m_file)
    {
        std::cerr << "[CDIMAGE] cannot open " << imagePath.string() << std::endl;
        return false;
    }
    std::error_code error;
    m_size = std::filesystem::file_size(imagePath, error);
    m_path = imagePath;

    std::vector<uint8_t> sector(kSectorSize);
    uint32_t lba = 16;
    for (;; ++lba)
    {
        if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, sector.data(), kSectorSize) ||
            std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[0] == 255)
        {
            std::cerr << "[CDIMAGE] " << imagePath.string() << " is not an ISO9660 image" << std::endl;
            std::fclose(m_file);
            m_file = nullptr;
            return false;
        }
        if (sector[0] == 1)
        {
            break;
        }
    }
    const uint8_t *root = sector.data() + 156;
    if (!parseDirectory(le32(root + 2), le32(root + 10), std::string(), 0))
    {
        std::fclose(m_file);
        m_file = nullptr;
        m_entries.clear();
        return false;
    }
    std::cout << "[CDIMAGE] mounted " << imagePath.string() << " (" << m_entries.size() << " entries)" << std::endl;
    return true;
}

bool PS2CdImage::parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth)
{
    if (depth > 16 || size > (64u << 20))
    {
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, data.data(), size))
    {
        return false;
    }
    size_t offset = 0;
    while (offset < data.size())
    {
        const uint8_t length = data[offset];
        if (length == 0)
        {
            offset = (offset / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (offset + length > data.size() || length < 34)
        {
            return false;
        }
        const uint8_t *record = data.data() + offset;
        const uint32_t entryLba = le32(record + 2);
        const uint32_t entrySize = le32(record + 10);
        const bool directory = (record[25] & 2u) != 0u;
        const uint8_t nameLength = record[32];
        offset += length;
        if (nameLength == 1 && (record[33] == 0 || record[33] == 1))
        {
            continue;
        }
        const std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
        const std::string key = normalizeKey(prefix + "/" + name);
        m_entries[key] = Entry{entryLba, entrySize, directory};
        if (directory && !parseDirectory(entryLba, entrySize, key, depth + 1))
        {
            return false;
        }
    }
    return true;
}

void PS2CdImage::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_entries.clear();
    m_openAttempted = false;
}

bool PS2CdImage::isOpen() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file != nullptr;
}

bool PS2CdImage::ensureOpenFromIoPaths()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file)
        {
            return true;
        }
        if (m_openAttempted)
        {
            return false;
        }
    }
    const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
    if (image.empty())
    {
        return false;
    }
    return open(image);
}

std::vector<std::string> PS2CdImage::rootFileNames() const
{
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &[key, entry] : m_entries)
    {
        if (!entry.directory && key.find('/') == std::string::npos)
        {
            names.push_back(key);
        }
    }
    return names;
}

bool PS2CdImage::find(std::string_view ps2Path, Entry &out) const
{
    const std::string key = normalizeKey(ps2Path);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
    {
        return false;
    }
    const auto it = m_entries.find(key);
    if (it == m_entries.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

bool PS2CdImage::readUnlocked(uint64_t offset, void *destination, size_t size) const
{
    if (!m_file)
    {
        return false;
    }
    if (size == 0)
    {
        return true;
    }
    if (!seekFile(m_file, offset) || std::fread(destination, 1u, size, m_file) != size)
    {
        return false;
    }
    PS2DiscOverlay::apply(offset, destination, size);
    return true;
}

namespace
{
    std::vector<PS2DiscOverlay::Patch> g_overlay;
    std::atomic<bool> g_overlayActive{false};
}

void PS2DiscOverlay::set(std::vector<Patch> patches)
{
    g_overlayActive.store(false);
    g_overlay = std::move(patches);
    g_overlayActive.store(!g_overlay.empty());
}

void PS2DiscOverlay::add(std::vector<Patch> patches)
{
    if (patches.empty())
    {
        return;
    }
    g_overlayActive.store(false);
    for (Patch &patch : patches)
    {
        g_overlay.push_back(std::move(patch));
    }
    g_overlayActive.store(!g_overlay.empty());
}

bool PS2DiscOverlay::active()
{
    return g_overlayActive.load(std::memory_order_acquire);
}

void PS2DiscOverlay::apply(uint64_t offset, void *destination, size_t size)
{
    if (!active() || !destination || size == 0)
    {
        return;
    }
    uint8_t *out = static_cast<uint8_t *>(destination);
    const uint64_t end = offset + size;
    for (const Patch &patch : g_overlay)
    {
        const uint64_t patchEnd = patch.first + patch.second.size();
        if (patchEnd <= offset || patch.first >= end)
        {
            continue;
        }
        const uint64_t from = std::max(offset, patch.first);
        const uint64_t to = std::min(end, patchEnd);
        std::memcpy(out + (from - offset), patch.second.data() + (from - patch.first), static_cast<size_t>(to - from));
    }
}

bool PS2CdImage::read(uint64_t offset, void *destination, size_t size) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return readUnlocked(offset, destination, size);
}

bool PS2CdImage::readSectors(uint32_t lba, uint32_t count, void *destination) const
{
    return read(static_cast<uint64_t>(lba) * kSectorSize, destination, static_cast<size_t>(count) * kSectorSize);
}

uint64_t PS2CdImage::sizeBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

std::filesystem::path PS2CdImage::path() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}

namespace
{
    struct ServedFile
    {
        uint32_t lba = 0, sectors = 0;
        uint64_t size = 0;
        std::filesystem::path host; // empty: bytes
        std::vector<uint8_t> bytes;
        std::unique_ptr<std::ifstream> stream; // the host file, opened at the first read
    };
    std::mutex g_servedMutex;
    std::vector<ServedFile> g_served; // by LBA
    uint32_t g_nextServedLba = PS2ServedFiles::kFirstLba;

    uint32_t addServed(ServedFile file)
    {
        if (file.size == 0u)
        {
            return 0u;
        }
        std::lock_guard<std::mutex> lock(g_servedMutex);
        const uint64_t sectors = (file.size + PS2CdImage::kSectorSize - 1u) / PS2CdImage::kSectorSize;
        if (sectors > 0x7FFFFFFFu - 1u - g_nextServedLba)
        {
            return 0u;
        }
        file.lba = g_nextServedLba;
        file.sectors = static_cast<uint32_t>(sectors);
        g_nextServedLba += file.sectors + 1u; // a free sector after each, so a read past its end is not the next file
        g_served.push_back(std::move(file));
        return g_served.back().lba;
    }
}

uint32_t PS2ServedFiles::serve(const std::filesystem::path &hostFile)
{
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(hostFile, error);
    if (error)
    {
        return 0u;
    }
    ServedFile file;
    file.size = size;
    file.host = hostFile;
    return addServed(std::move(file));
}

uint32_t PS2ServedFiles::serve(std::vector<uint8_t> bytes)
{
    ServedFile file;
    file.size = bytes.size();
    file.bytes = std::move(bytes);
    return addServed(std::move(file));
}

bool PS2ServedFiles::any()
{
    std::lock_guard<std::mutex> lock(g_servedMutex);
    return !g_served.empty();
}

bool PS2ServedFiles::isServed(uint32_t lba)
{
    if (lba < kFirstLba)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_servedMutex);
    const auto it = std::upper_bound(g_served.begin(), g_served.end(), lba, [](uint32_t l, const ServedFile &f) { return l < f.lba; });
    return it != g_served.begin() && lba < std::prev(it)->lba + std::prev(it)->sectors;
}

bool PS2ServedFiles::read(uint32_t lba, uint32_t sectors, void *destination, size_t size)
{
    if (lba < kFirstLba)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_servedMutex);
    const auto it = std::upper_bound(g_served.begin(), g_served.end(), lba, [](uint32_t l, const ServedFile &f) { return l < f.lba; });
    if (it == g_served.begin())
    {
        return false;
    }
    ServedFile &file = *std::prev(it);
    if (lba >= file.lba + file.sectors)
    {
        return false;
    }
    uint8_t *out = static_cast<uint8_t *>(destination);
    size = static_cast<size_t>(std::min<uint64_t>(size, static_cast<uint64_t>(sectors) * PS2CdImage::kSectorSize));
    std::memset(out, 0, size);
    const uint64_t offset = static_cast<uint64_t>(lba - file.lba) * PS2CdImage::kSectorSize;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(size, file.size - offset));
    if (file.host.empty())
    {
        std::memcpy(out, file.bytes.data() + offset, count);
        return true;
    }
    if (!file.stream)
    {
        file.stream = std::make_unique<std::ifstream>(file.host, std::ios::binary);
    }
    std::ifstream &in = *file.stream;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(count));
    const bool ok = static_cast<size_t>(in.gcount()) == count;
    if (!ok)
    {
        std::cerr << "[CDIMAGE] served file " << file.host.string() << ": read failed at 0x" << std::hex << offset << std::dec << std::endl;
        file.stream.reset();
    }
    return ok;
}
