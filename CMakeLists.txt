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

# psprecomp only got as far as gameplay in its own test game with real guest
# threads, so they are on by default here.
set(PSPRECOMP_REAL_THREADS ON CACHE BOOL "Enable cooperative host-thread scheduling")

add_subdirectory("${PSPRECOMP_DIR}" psprecomp)

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

option(MHFU_UNIVERSAL "A host for every region set up in work/ (mhfu_host_<region>)" OFF)
option(MHFU_RCHEEVOS "RetroAchievements in game (rcheevos)" ON)
option(MHFU_TRACE "Record every recompiled function entry" ON)
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
else()
    set(MHFU_FFMPEG_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg")
endif()
set(MHFU_FFMPEG_DIR "${MHFU_FFMPEG_DEFAULT}" CACHE PATH "FFmpeg (include/, lib/, bin/)")

# What every host shares, made once.
set(MHFU_HOST_SOURCES host/main.c host/achievements.c host/chat.c host/hle_mhfu.c host/input.c host/keys.c
    host/stats.c host/libfont.c host/utility.c host/adhoc.c host/lobby.c host/mods.c host/audio.c host/atrac.c
    host/mpeg.c)
set(MHFU_HOST_DEPS_READY OFF)
macro(mhfu_host_deps)
    if(NOT MHFU_HOST_DEPS_READY)
        set(MHFU_HOST_DEPS_READY ON)
        # RetroAchievements while playing (host/achievements.c): rcheevos'
        # rc_client, built from source; the launcher signs the player in.
        if(MHFU_RCHEEVOS AND (WIN32 OR ANDROID))
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
        # The SDL window draws its overlay with stb_truetype.
        if(MHFU_SDL_WINDOW)
            include(FetchContent)
            FetchContent_Declare(stb
                URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz)
            FetchContent_MakeAvailable(stb)
            set_source_files_properties(host/window_sdl.c host/main.c host/window.c PROPERTIES COMPILE_DEFINITIONS MHFU_SDL_WINDOW=1)
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
        # SDL's Android activity loads libmain*.so and calls its SDL_main.
        # SDL3 for Android has some C++ (its HID code): link with the C++ driver,
        # so the static libc++ (ANDROID_STL=c++_static) comes in.
        enable_language(CXX)
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
    target_link_libraries(${target} PRIVATE psprecomp mhfu_net)
    if(WIN32)
        target_sources(${target} PRIVATE assets/icon/mhfu.rc)       # the crest, as the exe's icon
        target_link_libraries(${target} PRIVATE user32 gdi32 opengl32 ole32 windowscodecs)
    endif()
    if(TARGET rcheevos)
        target_link_libraries(${target} PRIVATE rcheevos $<$<BOOL:${WIN32}>:winhttp>)
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
    if(MHFU_TRACE)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_TRACE)
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    # Generated code is machine output: no warnings, and build it in parallel.
    if(MSVC)
        target_compile_options(${target} PRIVATE /MP)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "/W0")
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
