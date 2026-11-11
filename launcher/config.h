// FreedomUniteRecomp launcher -- settings (launcher.ini) and where things live.
//
// The launcher sits next to mhfu_host.exe. The install it serves is the
// folder that holds work/ (searched upwards from the executable, so a build
// in build/Release finds the repository's work/). Everything it writes stays
// in work/: work/launcher/launcher.ini, work/launcher/input.ini, the badge
// cache, and profiles under work/profiles/<name> (the Default profile is
// work/root, the device root run.ps1 uses).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "../host/about.h"
#include "../host/region.h"
#include "regions.h"

// The game's executable: mhfu_host.exe on Windows, mhfu_host on Linux.
#ifdef _WIN32
#define MHFU_EXE_SUFFIX ".exe"
#define LAUNCHER_OS "Windows"
#else
#define MHFU_EXE_SUFFIX ""
#define LAUNCHER_OS "Linux"
#endif
#define LAUNCHER_VERSION MHFU_VERSION

struct Config {
    std::string profile = "Default";
    int  render_scale = 2;              // the GE's internal resolution: 480x272 times this
    std::string output = "960x544";     // the window's client size, "WxH"
    bool fullscreen = false;
    int  volume = 100;                  // the game's master volume, percent (MHFU_VOLUME)
    int  mouse_sens = 100;              // the mouse camera's sensitivity, percent (MHFU_MOUSE_SENS)
    bool widescreen = false;            // fill the window (MHFU_WIDESCREEN)
    bool texpack = true;                // a texture pack, when there is one (MHFU_TEXPACK=0 when off)
    std::string texpack_dir;            // its folder; empty: <root>/textures/<game id>
    std::string texpack_style;          // a style in its ADDONS/ (MHFU_TEXPACK_STYLE); empty: MAIN's own
    std::map<std::string, bool> mods;   // built-in mods by id (mod_<id>= in the ini); one not there is on
    bool fps60 = false;                 // the 60 fps mode (MHFU_FPS=60; in a room, as the room allows)
    std::string nick;                   // multiplayer name (--nick); empty: net.cfg's
    std::string gamepad;                // "": the first connected, "none": keyboard only, else an SDL GUID
    std::string gamepad_name;           // that GUID's device name, for showing
    bool check_updates = true;
    bool close_on_start = false;
    std::string extra_env;              // KEY=VALUE per line
    std::string extra_args;
    std::string iso;                    // empty: work/iso.txt (written by setup.ps1)
    std::string eboot;                  // empty: work/<game id>_EBOOT.BIN
    // The release in use (regions.h: "EU", "US", "JP", "FUC"); "" at first: told
    // from the disc. iso/eboot above are that release's; the others' wait here.
    std::string region;
    std::map<std::string, std::string> iso_of, eboot_of;
    std::string ra_user, ra_key;        // RetroAchievements username and Web API key
    std::string ra_token;               // the login token for unlocking in game (never the password)
    bool ra_ingame = true;              // unlock achievements while playing
};

namespace paths {
void init(const char *exe_dir);         // finds the install
const std::string &exe_dir();           // where the launcher and mhfu_host are
const std::string &base();              // the folder that holds work/
std::string work();
std::string launcher();                 // work/launcher
std::string host_exe();
std::string iso(const Config &c);
std::string eboot(const Config &c);
std::string profile_root(const std::string &name);
std::vector<std::string> profiles();    // "Default" first
bool create_profile(const std::string &name, std::string &err);
bool delete_profile(const std::string &name, std::string &err);
}

bool config_load(Config &c);           // also picks the release to start (regions.h)
bool config_save(const Config &c);
void config_use_region(Config &c, int i);   // switch release: its own disc and EBOOT come back
void config_choose_region(Config &c);       // once at start: the release to start

// Small helpers shared by the launcher's files.
bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
std::string read_text(const std::string &path);
bool write_text(const std::string &path, const std::string &text);
void open_in_shell(const std::string &path_or_url);
