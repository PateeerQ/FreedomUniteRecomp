// FreedomUniteRecomp launcher -- which release of the game it starts.
//
// Each release (Europe, USA, Japan's Portable 2nd G, the English FUComplete
// disc of it) is recompiled into a game of its own: mhfu_host_<tag>.exe next
// to the launcher (a universal build, scripts/build-universal.ps1), or one
// mhfu_host.exe for the release this launcher was built with (setup.ps1). The
// launcher picks the one for the player's disc: told from the disc itself
// (its product code, and FUComplete's FUC folder), or chosen under Advanced.
// Each keeps its files in its own work folder (work/, work/us, work/jp,
// work/fuc: saves, profiles, the EBOOT); the launcher's settings are shared.
#pragma once

#include <cstdint>
#include <string>

struct GameRegion {
    const char *tag;            // "EU", "US", "JP", "FUC"
    const char *sub;            // its work folder under work/ ("" for EU)
    const char *game_id;        // save folder, CWCheat
    const char *game_id_dash;
    const char *name;           // "Europe" ...
    const char *title;          // the game's name
    const char *eboot_file;     // in its work folder
    uint32_t eboot_size;        // PPSSPP's dump (or the disc's own, FUComplete)
    uint32_t code_end;          // CWCheat offset where the EBOOT's code ends
    bool fps60, mods;           // the 60 fps mode and the built-in mods are in it
    bool eboot_on_disc;         // the EBOOT comes from the disc (FUComplete: nothing to dump)
    bool mods_native;           // the built-in mods were written for it (the USA one)
};

enum { REGION_EU, REGION_US, REGION_JP, REGION_FUC, REGION_COUNT };
extern const GameRegion k_regions[REGION_COUNT];

const GameRegion &region();                 // the one in use
int region_index();
void region_select(int i);
bool region_built(int i);                   // its game is next to the launcher
std::string region_host_exe(int i);         // where its game is ("" if not there)

// The release a disc image is: its product code (PARAM.SFO) and FUComplete's
// FUC folder; -1 if it is none of them.
int region_of_disc(const std::string &iso);

// FUComplete: PSP_GAME/SYSDIR/EBOOT.BIN out of the disc into `out` (ISO 9660).
bool eboot_from_disc(const std::string &iso, const std::string &out, std::string &err);
