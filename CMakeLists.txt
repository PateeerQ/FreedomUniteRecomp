# MHFU-Android -- Monster Hunter Freedom Unite (EU, ULES01213), statically
# recompiled with psprecomp.
#
# Builds:
#   - psprecomp (runtime library + the allegrexrecomp tool), from the submodule
#   - mhfu_host, the desktop bring-up host, once work/gen/ holds emitted C
#
# work/gen/ is produced from your own decrypted EBOOT by scripts/setup.ps1 and
# is never committed.

cmake_minimum_required(VERSION 3.16)
project(mhfu_android C)

# Archives fetched by URL take the time they are extracted (CMake 3.24+; the
# Android SDK's CMake is 3.22, which has neither the policy nor the option).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(PSPRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/psprecomp" CACHE PATH "psprecomp checkout")
set(MHFU_GEN_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/work/gen"  CACHE PATH "allegrexrecomp emit output")
set(MHFU_OVL_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/work/gen_ovl" CACHE PATH "recompiled overlays")

if(NOT EXISTS "${PSPRECOMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "psprecomp not found at ${PSPRECOMP_DIR}. Run:\n"
        "    git submodule update --init --recursive")
endif()

# ccache: every object kept by its contents, so a build after setup.ps1 re-emits the
# recompiled C (new timestamps, mostly the same code), a fresh build folder, or the
# switch between regions compiles only what really changed. Ninja and Makefiles
# only (CMake's Visual Studio generators run no launcher: scripts\build-universal.ps1
# builds with Ninja for that). Android's Gradle uses Ninja, Linux too.
option(MHFU_CCACHE "Compile through ccache when it is installed (Ninja/Makefile builds)" ON)
if(MHFU_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    file(GLOB _mhfu_winget_ccache "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/Ccache.Ccache_*/ccache-*")
    find_program(MHFU_CCACHE_EXE ccache HINTS "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links" ${_mhfu_winget_ccache})
    if(MHFU_CCACHE_EXE)
        message(STATUS "ccache: ${MHFU_CCACHE_EXE}")
        set(CMAKE_C_COMPILER_LAUNCHER "${MHFU_CCACHE_EXE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${MHFU_CCACHE_EXE}")
    endif()
endif()
# MSVC's debug info: /Zi writes one shared PDB, which ccache cannot keep (and which
# Ninja's parallel compiles contend for); /Z7 puts it in each object. The link's
# /DEBUG still makes the exe's PDB (host/prof.c).
if(MSVC AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    set(MHFU_MSVC_DEBUG_INFO /Z7)
else()
    set(MHFU_MSVC_DEBUG_INFO /Zi)
endif()

# psprecomp only got as far as gameplay in its own test game with real guest
# threads, so they are on by default here.
set(PSPRECOMP_REAL_THREADS ON CACHE BOOL "Enable cooperative host-thread scheduling")

add_subdirectory("${PSPRECOMP_DIR}" psprecomp)
if(MSVC)
    # Full debug info for the runtime (small), so host/prof.c names its static functions too.
    target_compile_options(psprecomp PRIVATE ${MHFU_MSVC_DEBUG_INFO})
endif()

# Profile-guided optimisation (clang: Android; scripts/pgo-android.ps1). GEN builds a
# host that counts every branch and writes files/pgo/*.profraw on the phone; USE
# rebuilds with the merged profile: hot code laid out together, branches the way
# they go. The recompiled code is ~250 MB a release, so where its hot part sits
# matters to the phone's caches. A profile only fits the release it was made from
# (static functions are named with their file); several merged into one serve each.
set(MHFU_PGO "OFF" CACHE STRING "Profile-guided optimisation (clang): OFF, GEN or USE")
set_property(CACHE MHFU_PGO PROPERTY STRINGS OFF GEN USE)
set(MHFU_PGO_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/work/pgo/mhfu.profdata" CACHE FILEPATH "MHFU_PGO=USE's merged profile")
string(TOUPPER "${MHFU_PGO}" MHFU_PGO_MODE)
set(MHFU_PGO_FLAGS "")
if(MHFU_PGO_MODE STREQUAL "GEN")
    set(MHFU_PGO_FLAGS -fprofile-generate)
elseif(MHFU_PGO_MODE STREQUAL "USE")
    if(NOT EXISTS "${MHFU_PGO_PROFILE}")
        message(FATAL_ERROR "MHFU_PGO=USE: no profile at ${MHFU_PGO_PROFILE} (scripts/pgo-android.ps1 -Merge)")
    endif()
    set(MHFU_PGO_FLAGS "-fprofile-use=${MHFU_PGO_PROFILE}" -Wno-profile-instr-unprofiled
        -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
endif()
if(MHFU_PGO_FLAGS)
    if(MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MHFU_PGO needs clang (the Android NDK)")
    endif()
    message(STATUS "PGO: ${MHFU_PGO_MODE} ${MHFU_PGO_FLAGS}")
    target_compile_options(psprecomp PRIVATE ${MHFU_PGO_FLAGS})
endif()

# --- tools ------------------------------------------------------------------------

# Wraps an overlay the host saved as an ELF for allegrexrecomp (setup.ps1 runs it).
add_executable(mhfu_ovl tools/mhfu_ovl.c)
set_target_properties(mhfu_ovl PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_definitions(mhfu_ovl PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The room network (ad hoc multiplayer, see host/net/proto.h): a library the
# host links, the standalone hub for a server, and a check that runs it end to
# end without the game.
add_library(mhfu_net STATIC host/net/sys.c host/net/hub.c host/net/client.c)
target_include_directories(mhfu_net PUBLIC host)
set_target_properties(mhfu_net PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(WIN32)
    target_link_libraries(mhfu_net PUBLIC ws2_32 advapi32)
    target_compile_definitions(mhfu_net PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mhfu_net PUBLIC Threads::Threads)
endif()
add_executable(mhfu_netcheck tools/mhfu_netcheck.c)
target_link_libraries(mhfu_netcheck PRIVATE mhfu_net)
set_target_properties(mhfu_netcheck PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
# The internet relay is private (server/, gitignored): built only where it is.
if(EXISTS "${CMAKE_SOURCE_DIR}/server/mhfu_hub.c")
    add_executable(mhfu_hub server/mhfu_hub.c)
    target_link_libraries(mhfu_hub PRIVATE mhfu_net)
    set_target_properties(mhfu_hub PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endif()

# Gamepads (host/gamepad.c) and sound output (host/audio.c): SDL3, built
# static from source, with only what those need. SDL reads the DualSense and
# DualShock 4 itself, over USB or Bluetooth, which XInput cannot, and plays
# through WASAPI with a short buffer, where WinMM lagged the picture by about
# 100 ms. The Android port will use SDL3 for its window, input and sound as
# well. MHFU_GAMEPAD=OFF builds without it (WinMM sound, no controllers). The
# launcher draws its window with it too.
option(MHFU_GAMEPAD "Controller support and sound through SDL3" ON)
option(MHFU_LAUNCHER "Build the launcher (Dear ImGui over SDL3)" ON)
if(MHFU_GAMEPAD OR MHFU_LAUNCHER)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO ON CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.26
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# The launcher (launcher/): Play, profiles, settings, controls, achievements,
# updates, log. It needs no recompiled code, so it builds before that check.
if(MHFU_LAUNCHER)
    add_subdirectory(launcher)
endif()

# --- the host -------------------------------------------------------------------
#
# One host per region's recompiled code: the code is the game's own, so each
# release of the game (Europe, USA, Japan, FUComplete) is a host of its own.
# A normal build makes one, mhfu_host, from MHFU_GEN_DIR (scripts/setup.ps1
# builds each region in its own build folder that way). MHFU_UNIVERSAL=ON makes
# every region set up so far in one build: mhfu_host_eu, _us, _jp, _fuc (on
# Android libmain_eu.so ...), which the launcher and the app choose from by the
# disc the player has (scripts/build-universal.ps1).

option(MHFU_TEXPACK "Texture packs (PPSSPP's textures.ini, PNG and KTX2)" ON)
if(ANDROID OR MHFU_TEXPACK)
    # The hosts link as C++ (SDL3's HID code on Android; the KTX2 transcoder):
    # enabled here, at the top scope.
    enable_language(CXX)
endif()
option(MHFU_UNIVERSAL "A host for every region set up in work/ (mhfu_host_<region>)" OFF)
option(MHFU_RCHEEVOS "RetroAchievements in game (rcheevos)" ON)
# Function-entry tracing for crash reports. FULL: every entry, label and stack
# check through a call (bring-up). LITE: the entry ring only, one inline store per
# function (the release builds and Android). OFF: nothing. ON = FULL (old caches).
set(MHFU_TRACE "LITE" CACHE STRING "Recompiled-code tracing: FULL, LITE or OFF")
set_property(CACHE MHFU_TRACE PROPERTY STRINGS FULL LITE OFF)
if(ANDROID OR NOT WIN32)
    set(MHFU_SDL_WINDOW_DEFAULT ON)
else()
    set(MHFU_SDL_WINDOW_DEFAULT OFF)
endif()
option(MHFU_SDL_WINDOW "The SDL3 window instead of the Win32 one" ${MHFU_SDL_WINDOW_DEFAULT})
if(MHFU_SDL_WINDOW AND NOT MHFU_GAMEPAD)
    message(FATAL_ERROR "MHFU_SDL_WINDOW needs SDL3 (MHFU_GAMEPAD=ON)")
endif()
if(ANDROID)
    set(MHFU_FFMPEG_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg-android")
elseif(UNIX AND NOT APPLE)
    # Static, only the decoders the game uses (scripts/build-ffmpeg-linux.sh).
    set(MHFU_FFMPEG_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg-linux")
else()
    set(MHFU_FFMPEG_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg")
endif()
set(MHFU_FFMPEG_DIR "${MHFU_FFMPEG_DEFAULT}" CACHE PATH "FFmpeg (include/, lib/, bin/)")

# The internet relay's address (host/about.h): kept out of the sources, in
# server/relay.txt ("host:port", gitignored with the rest of server/), or
# -DMHFU_RELAY=host:port. Without either, rooms are on the LAN only.
set(MHFU_RELAY "" CACHE STRING "The internet relay builds use by default (host:port)")
set(MHFU_RELAY_VALUE "${MHFU_RELAY}")
if(NOT MHFU_RELAY_VALUE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/relay.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/server/relay.txt" MHFU_RELAY_VALUE LIMIT_COUNT 1)
    string(STRIP "${MHFU_RELAY_VALUE}" MHFU_RELAY_VALUE)
endif()
if(MHFU_RELAY_VALUE)
    message(STATUS "mhfu_host: rooms over the internet through the relay in server/relay.txt")
else()
    message(STATUS "mhfu_host: no server/relay.txt: rooms on the LAN only")
endif()

# What every host shares, made once.
set(MHFU_HOST_SOURCES host/main.c host/achievements.c host/chat.c host/hle_mhfu.c host/input.c host/keys.c
    host/stats.c host/libfont.c host/utility.c host/adhoc.c host/lobby.c host/mods.c host/audio.c host/atrac.c
    host/mpeg.c host/prof.c host/chnnlsv.c host/perfhint.c host/curl_dl.c)
set(MHFU_HOST_DEPS_READY OFF)

# The PSP's KIRK crypto engine (PPSSPP's ext/libkirk, GPL v3): decrypts save data
# a PSP or PPSSPP encrypted (host/chnnlsv.c).
add_library(mhfu_kirk STATIC third_party/libkirk/AES.c third_party/libkirk/SHA1.c third_party/libkirk/bn.c
    third_party/libkirk/ec.c third_party/libkirk/kirk_engine.c)
target_include_directories(mhfu_kirk PUBLIC third_party/libkirk)
if(MSVC)
    target_compile_options(mhfu_kirk PRIVATE /W0)
    target_compile_definitions(mhfu_kirk PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mhfu_kirk PRIVATE -w)
endif()
macro(mhfu_host_deps)
    if(NOT MHFU_HOST_DEPS_READY)
        set(MHFU_HOST_DEPS_READY ON)
        # RetroAchievements while playing (host/achievements.c): rcheevos'
        # rc_client, built from source; the launcher signs the player in.
        if(MHFU_RCHEEVOS)
            include(FetchContent)
            FetchContent_Declare(rcheevos
                GIT_REPOSITORY https://github.com/RetroAchievements/rcheevos.git
                GIT_TAG        v11.6.0
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(rcheevos)   # no CMakeLists of its own: just fetched
            file(GLOB RCHEEVOS_SOURCES
                "${rcheevos_SOURCE_DIR}/src/rc_client.c" "${rcheevos_SOURCE_DIR}/src/rc_compat.c"
                "${rcheevos_SOURCE_DIR}/src/rc_util.c" "${rcheevos_SOURCE_DIR}/src/rc_version.c"
                "${rcheevos_SOURCE_DIR}/src/rapi/*.c" "${rcheevos_SOURCE_DIR}/src/rcheevos/*.c"
                "${rcheevos_SOURCE_DIR}/src/rhash/*.c")
            add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
            target_include_directories(rcheevos PUBLIC "${rcheevos_SOURCE_DIR}/include" PRIVATE "${rcheevos_SOURCE_DIR}/src")
            target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_DISABLE_LUA PRIVATE _CRT_SECURE_NO_WARNINGS)
            set_target_properties(rcheevos PROPERTIES C_STANDARD 11)
        endif()
        # The SDL window draws its overlay with stb_truetype; texture packs read
        # PNG files with stb_image.
        if(MHFU_SDL_WINDOW OR MHFU_GAMEPAD)
            include(FetchContent)
            FetchContent_Declare(stb
                URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz)
            FetchContent_MakeAvailable(stb)
        endif()
        if(MHFU_SDL_WINDOW)
            set_source_files_properties(host/window_sdl.c host/main.c host/window.c PROPERTIES COMPILE_DEFINITIONS MHFU_SDL_WINDOW=1)
        endif()
        # Texture packs (host/texpack.c, PPSSPP's format): their KTX2 pictures
        # through Basis Universal's transcoder, with its zstd decoder (UASTC +
        # zstd is how PPSSPP's packs ship). Needs SDL (the loader's thread).
        if(MHFU_TEXPACK AND MHFU_GAMEPAD)
            include(FetchContent)
            # SOURCE_SUBDIR names no folder: fetched only (its own CMakeLists builds the encoder: not wanted).
            FetchContent_Declare(basisu
                URL https://github.com/BinomialLLC/basis_universal/archive/refs/tags/v1_60.tar.gz
                SOURCE_SUBDIR fetch-only)
            FetchContent_MakeAvailable(basisu)
            add_library(mhfu_basisu STATIC "${basisu_SOURCE_DIR}/transcoder/basisu_transcoder.cpp"
                                           "${basisu_SOURCE_DIR}/zstd/zstddeclib.c")
            target_include_directories(mhfu_basisu PUBLIC "${basisu_SOURCE_DIR}/transcoder")
            target_compile_definitions(mhfu_basisu PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
            # Its asserts do not compile where uint64_t is unsigned long (Android: safe_shift_left(1ULL, ...)).
            target_compile_definitions(mhfu_basisu PRIVATE NDEBUG)
            if(MSVC)
                target_compile_options(mhfu_basisu PRIVATE /W0)
            else()
                target_compile_options(mhfu_basisu PRIVATE -w)
            endif()
        endif()
        if(EXISTS "${MHFU_FFMPEG_DIR}/include/libavcodec/avcodec.h")
            message(STATUS "mhfu_host: FFmpeg from ${MHFU_FFMPEG_DIR}")
        else()
            message(STATUS "mhfu_host: no FFmpeg in ${MHFU_FFMPEG_DIR}: movies black, music silent")
        endif()
    endif()
endmacro()

# mhfu_add_host(TARGET GEN_DIR OVL_DIR [OUTPUT_NAME]): one host from one
# region's generated code. Returns quietly when GEN_DIR holds none.
function(mhfu_add_host target gen_dir ovl_dir)
    file(GLOB gen_sources CONFIGURE_DEPENDS "${gen_dir}/*.c")
    if(NOT gen_sources)
        message(STATUS "${target}: no recompiled code in ${gen_dir}: skipped (run scripts/setup.ps1)")
        return()
    endif()
    list(LENGTH gen_sources gen_count)
    message(STATUS "${target}: ${gen_count} generated source files from ${gen_dir}")

    # Overlays: one directory per recompiled overlay, plus the table that names
    # them. Before any exist, an empty table. Then the built-in mods' data.
    file(GLOB ovl_sources CONFIGURE_DEPENDS "${ovl_dir}/*/*.c")
    if(EXISTS "${ovl_dir}/overlays.c")
        list(APPEND ovl_sources "${ovl_dir}/overlays.c")
    else()
        set(ovl_sources host/overlays_none.c)
    endif()
    if(EXISTS "${ovl_dir}/mods_builtin.c")
        list(APPEND ovl_sources "${ovl_dir}/mods_builtin.c")
    else()
        list(APPEND ovl_sources host/mods_none.c)
    endif()
    list(APPEND gen_sources ${ovl_sources})

    if(ANDROID)
        # SDL's Android activity loads libmain*.so and calls its SDL_main. SDL3 for
        # Android has some C++ (its HID code): linked with the C++ driver, so the
        # static libc++ (ANDROID_STL=c++_static) comes in. CXX is enabled at the top:
        # enabled in here, its link rule was empty ("cd . && cd .") and no .so came out.
        add_library(${target} SHARED ${MHFU_HOST_SOURCES} host/android.c ${gen_sources})
        set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
        target_link_libraries(${target} PRIVATE android log EGL GLESv3 OpenSLES)
    else()
        add_executable(${target} ${MHFU_HOST_SOURCES} ${gen_sources})
    endif()
    if(ARGC GREATER 3)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${ARGV3}")
    endif()
    target_include_directories(${target} PRIVATE "${gen_dir}" host)
    if(MHFU_RELAY_VALUE)
        target_compile_definitions(${target} PRIVATE "MHFU_DEFAULT_RELAY=\"${MHFU_RELAY_VALUE}\"")
    endif()
    target_link_libraries(${target} PRIVATE psprecomp mhfu_net mhfu_kirk)
    if(WIN32)
        target_sources(${target} PRIVATE assets/icon/mhfu.rc)       # the crest, as the exe's icon
        target_link_libraries(${target} PRIVATE user32 gdi32 opengl32 ole32 windowscodecs)
    endif()
    if(TARGET rcheevos)
        target_link_libraries(${target} PRIVATE rcheevos $<$<BOOL:${WIN32}>:winhttp>)
        if(UNIX AND NOT ANDROID)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})    # libcurl, opened at run time
        endif()
        target_compile_definitions(${target} PRIVATE MHFU_RCHEEVOS=1)
    endif()
    # The window: Win32 (host/window.c: the Multiplayer menu and dialogs) on
    # Windows, SDL3 (host/window_sdl.c) on Android and Linux.
    if(MHFU_SDL_WINDOW)
        target_sources(${target} PRIVATE host/window_sdl.c)
        target_include_directories(${target} PRIVATE "${stb_SOURCE_DIR}")
    else()
        target_sources(${target} PRIVATE host/window.c)
    endif()
    if(MHFU_GAMEPAD)
        target_sources(${target} PRIVATE host/gamepad.c)
        target_link_libraries(${target} PRIVATE SDL3::SDL3-static)
        target_compile_definitions(${target} PRIVATE MHFU_GAMEPAD=1)
    endif()
    if(TARGET mhfu_basisu)
        target_sources(${target} PRIVATE host/texpack.c host/ktx2.cpp)
        target_include_directories(${target} PRIVATE "${stb_SOURCE_DIR}")
        target_link_libraries(${target} PRIVATE mhfu_basisu)
        # Only main.c asks for it: on the whole target, turning it over would
        # recompile every generated file.
        set_property(SOURCE host/main.c host/window_sdl.c APPEND PROPERTY COMPILE_DEFINITIONS MHFU_TEXPACK=1)
    endif()
    # Movies and music (host/mpeg.c, host/atrac.c): FFmpeg's H.264 and ATRAC3+
    # decoders, from a shared LGPL build in deps/ffmpeg (on Windows, BtbN's
    # win64-lgpl-shared; the DLLs are copied next to the executable).
    if(EXISTS "${MHFU_FFMPEG_DIR}/include/libavcodec/avcodec.h")
        target_include_directories(${target} PRIVATE "${MHFU_FFMPEG_DIR}/include")
        foreach(lib avcodec swscale swresample avutil)
            if(ANDROID)
                # Static (scripts/build-ffmpeg-android.sh); named directly, since the
                # NDK's toolchain only searches inside its sysroot.
                target_link_libraries(${target} PRIVATE "${MHFU_FFMPEG_DIR}/lib/lib${lib}.a")
            else()
                find_library(MHFU_FF_${lib} ${lib} PATHS "${MHFU_FFMPEG_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
                target_link_libraries(${target} PRIVATE ${MHFU_FF_${lib}})
            endif()
        endforeach()
        if(UNIX AND NOT ANDROID)
            # deps/ffmpeg-linux is static: its own dependencies come after it.
            find_package(Threads REQUIRED)
            target_link_libraries(${target} PRIVATE Threads::Threads m)
        endif()
        if(ANDROID)
            target_link_libraries(${target} PRIVATE z m)
            # FFmpeg's aarch64 assembly reaches its tables (ff_tx_tab_*) with
            # relocations a shared library may only use for symbols it binds itself.
            target_link_options(${target} PRIVATE "-Wl,-Bsymbolic")
        endif()
        target_compile_definitions(${target} PRIVATE MHFU_FFMPEG=1)
        if(WIN32)
            file(GLOB ff_dlls "${MHFU_FFMPEG_DIR}/bin/*.dll")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ff_dlls} "$<TARGET_FILE_DIR:${target}>")
        endif()
    endif()
    # Function-entry tracing: the report then names the last function reached and
    # dumps the recent call trail. Slower, but bring-up needs it.
    string(TOUPPER "${MHFU_TRACE}" trace_mode)
    if(trace_mode STREQUAL "FULL" OR trace_mode STREQUAL "ON")
        target_compile_definitions(${target} PRIVATE PSPRECOMP_TRACE)
    elseif(trace_mode STREQUAL "LITE")
        target_compile_definitions(${target} PRIVATE PSPRECOMP_TRACE_LITE)
    endif()
    if(MHFU_PGO_FLAGS)
        target_compile_options(${target} PRIVATE ${MHFU_PGO_FLAGS})
        if(MHFU_PGO_MODE STREQUAL "GEN")
            target_link_options(${target} PRIVATE -fprofile-generate)
            target_compile_definitions(${target} PRIVATE MHFU_PGO_GEN=1)
        endif()
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    # Generated code is machine output: no warnings, and build it in parallel.
    if(MSVC)
        target_compile_options(${target} PRIVATE /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        # A PDB of the public symbols (no /Zi: the code is unchanged), for host/prof.c's
        # report; /DEBUG turns the size optimisations off unless they are asked for.
        target_link_options(${target} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
        target_link_libraries(${target} PRIVATE dbghelp winmm)
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "/W0")
        set_property(SOURCE ${MHFU_HOST_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${MHFU_MSVC_DEBUG_INFO})
    else()
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endfunction()

mhfu_host_deps()     # once, here: FetchContent's variables stay in this scope

if(MHFU_UNIVERSAL)
    # Every region set up so far (scripts/setup.ps1's folders), each a host of its own.
    set(MHFU_WORK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/work" CACHE PATH "setup.ps1's work folder")
    foreach(r eu us jp fuc)
        if(r STREQUAL "eu")
            set(w "${MHFU_WORK_ROOT}")
        else()
            set(w "${MHFU_WORK_ROOT}/${r}")
        endif()
        if(ANDROID)
            mhfu_add_host(mhfu_host_${r} "${w}/gen" "${w}/gen_ovl" main_${r})
        else()
            mhfu_add_host(mhfu_host_${r} "${w}/gen" "${w}/gen_ovl")
        endif()
    endforeach()
elseif(ANDROID)
    mhfu_add_host(mhfu_host "${MHFU_GEN_DIR}" "${MHFU_OVL_DIR}" main)
else()
    mhfu_add_host(mhfu_host "${MHFU_GEN_DIR}" "${MHFU_OVL_DIR}")
endif()
