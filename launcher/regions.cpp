// FreedomUniteRecomp launcher -- see regions.h.
#include "regions.h"

#include "config.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

const GameRegion k_regions[REGION_COUNT] = {
    {"EU", "", "ULES01213", "ULES-01213", "Europe", "Monster Hunter Freedom Unite", "ULES01213_EBOOT.BIN",
     1910384, 0x10E6B8, true, true, false, false},
    {"US", "us", "ULUS10391", "ULUS-10391", "USA", "Monster Hunter Freedom Unite", "ULUS10391_EBOOT.BIN",
     1910672, 0x10E7D8, true, true, false, true},
    {"JP", "jp", "ULJM05500", "ULJM-05500", "Japan", "Monster Hunter Portable 2nd G", "ULJM05500_EBOOT.BIN",
     1890112, 0x10C1F0, true, true, false, false},
    {"FUC", "fuc", "ULJM05500", "ULJM-05500", "FUComplete", "Monster Hunter Freedom Unite Complete (FUComplete)",
     "ULJM05500_FUC_EBOOT.BIN", 1889772, 0x10C1F0, true, true, true, false},
};

// The release this launcher was built with (host/region.h through MHFU_GEN_DIR):
// the one an install with a single mhfu_host.exe has.
static int built_with() {
    for (int i = 0; i < REGION_COUNT; i++)
        if (!strcmp(k_regions[i].tag, MHFU_REGION_TAG)) return i;
    return REGION_EU;
}

static int g_cur = -1;

int region_index() { return g_cur < 0 ? built_with() : g_cur; }
const GameRegion &region() { return k_regions[region_index()]; }
void region_select(int i) { if (i >= 0 && i < REGION_COUNT) g_cur = i; }

std::string region_host_exe(int i) {
    std::string tag = k_regions[i].tag;
    for (auto &ch : tag) ch = (char)tolower((unsigned char)ch);
    const fs::path dir = fs::u8path(paths::exe_dir());
    std::error_code e;
    const fs::path own = dir / ("mhfu_host_" + tag + ".exe");
    if (fs::exists(own, e)) return own.u8string();
    const fs::path one = dir / "mhfu_host.exe";                      // a single-release install
    if (i == built_with() && fs::exists(one, e)) return one.u8string();
    return "";
}

bool region_built(int i) { return !region_host_exe(i).empty(); }

// ---- the disc ------------------------------------------------------------------

namespace {
struct Iso {
    std::ifstream f;
    explicit Iso(const std::string &p) : f(fs::u8path(p), std::ios::binary) {}
    bool read(uint64_t at, void *out, size_t n) {
        f.clear();
        f.seekg((std::streamoff)at);
        f.read((char *)out, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    }
    static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
    // {sector, size} of `name` in a directory, or {0, 0}.
    std::pair<uint32_t, uint32_t> find(uint32_t lba, uint32_t size, const char *name) {
        if (size > (16u << 20)) return {0, 0};
        std::vector<uint8_t> dir(size);
        if (!read((uint64_t)lba * 2048, dir.data(), size)) return {0, 0};
        for (size_t i = 0; i < dir.size();) {
            const uint8_t n = dir[i];
            if (n == 0) { i = (i / 2048 + 1) * 2048; continue; }
            if (i + 33 > dir.size()) break;
            std::string s((const char *)&dir[i + 33], dir[i + 32]);
            const size_t semi = s.find(';');
            if (semi != std::string::npos) s.resize(semi);
            if (!_stricmp(s.c_str(), name)) return {le32(&dir[i + 2]), le32(&dir[i + 10])};
            i += n;
        }
        return {0, 0};
    }
    // {sector, size} of a path like "PSP_GAME/SYSDIR/EBOOT.BIN".
    std::pair<uint32_t, uint32_t> path(const std::string &p) {
        uint8_t pvd[2048];
        if (!read(16 * 2048, pvd, sizeof pvd) || memcmp(pvd + 1, "CD001", 5)) return {0, 0};
        std::pair<uint32_t, uint32_t> at{le32(pvd + 156 + 2), le32(pvd + 156 + 10)};
        size_t from = 0;
        while (at.first) {
            const size_t slash = p.find('/', from);
            at = find(at.first, at.second, p.substr(from, slash - from).c_str());
            if (slash == std::string::npos) break;
            from = slash + 1;
        }
        return at;
    }
};
}

int region_of_disc(const std::string &iso) {
    Iso d(iso);
    if (!d.f) return -1;
    const auto sfo = d.path("PSP_GAME/PARAM.SFO");
    if (!sfo.first || sfo.second > 65536) return -1;
    std::string param(sfo.second, '\0');
    if (!d.read((uint64_t)sfo.first * 2048, param.data(), param.size())) return -1;
    if (param.find("ULES01213") != std::string::npos) return REGION_EU;
    if (param.find("ULUS10391") != std::string::npos) return REGION_US;
    if (param.find("ULJM05500") != std::string::npos)
        return d.path("FUC/PRELOAD.BIN").first ? REGION_FUC : REGION_JP;
    return -1;
}

bool eboot_from_disc(const std::string &iso, const std::string &out, std::string &err) {
    Iso d(iso);
    const auto e = d.path("PSP_GAME/SYSDIR/EBOOT.BIN");
    if (!e.first) { err = "no PSP_GAME/SYSDIR/EBOOT.BIN on the disc"; return false; }
    if (e.second != region().eboot_size) { err = "the disc's EBOOT is not the one this game was built from"; return false; }
    std::vector<char> buf(e.second);
    if (!d.read((uint64_t)e.first * 2048, buf.data(), buf.size())) { err = "could not read the disc's EBOOT"; return false; }
    if (memcmp(buf.data(), "\x7F" "ELF", 4)) { err = "the disc's EBOOT is encrypted"; return false; }
    std::error_code ec;
    fs::create_directories(fs::u8path(out).parent_path(), ec);
    std::ofstream o(fs::u8path(out), std::ios::binary);
    o.write(buf.data(), (std::streamsize)buf.size());
    if (!o) { err = "could not write " + out; return false; }
    return true;
}
