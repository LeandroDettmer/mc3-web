cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Midnight Club 3: DUB Edition (Xbox) - Static Recompilation
#
# Hosts: Linux (GCC, SDL2 + OpenGL or Vulkan; the day-to-day debug build) and
# Nintendo Switch homebrew (devkitA64 + libnx, built with switch/build.sh).
# Layout and workflow: README.md, CLAUDE.md, docs/decisions/.
# ============================================================================

project(mcdub3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "xboxrecomp toolkit (vendored; see docs/decisions/0002)")

# Which game: dub (Midnight Club 3: DUB Edition) or remix (DUB Edition
# Remix). Each has its own config/<game>/ (XBE facts, editions, seeds,
# game.h), src/games/<game>/recomp_manual.c (the overrides, at its own
# addresses) and work-<game>/ (lift and builds); tools/env.sh sets
# MC3_GAME from GAME=. Everything else is shared.
if(DEFINED ENV{MC3_GAME})
    set(_mc3_game "$ENV{MC3_GAME}")
else()
    set(_mc3_game "dub")
endif()
set(MC3_GAME "${_mc3_game}" CACHE STRING "Game: dub or remix")
set(MC3_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config/${MC3_GAME}")
set(MC3_MANUAL_C "${CMAKE_CURRENT_SOURCE_DIR}/src/games/${MC3_GAME}/recomp_manual.c")
if(NOT EXISTS "${MC3_MANUAL_C}")
    message(FATAL_ERROR "No overrides for game '${MC3_GAME}' (${MC3_MANUAL_C}).")
endif()

# The lifted C (~100 MB) is generated from the retail XBE by tools/regen.sh
# and never committed. Default matches tools/env.sh: <repo>/work-<game>.
if(DEFINED ENV{MC3_WORK})
    set(_mc3_work "$ENV{MC3_WORK}")
else()
    set(_mc3_work "${CMAKE_CURRENT_SOURCE_DIR}/work-${MC3_GAME}")
endif()
set(MC3_GEN_DIR "${_mc3_work}/gen" CACHE PATH
    "tools.recomp output (recomp_*.c, recomp_funcs.h, recomp_types.h)")
if(NOT EXISTS "${MC3_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${MC3_GEN_DIR}. Run tools/first_lift.sh "
                        "(or tools/regen.sh), or pass -DMC3_GEN_DIR=<dir>.")
endif()
if(NOT EXISTS "${MC3_CONFIG_DIR}/xbe_facts.h")
    message(FATAL_ERROR "config/${MC3_GAME}/xbe_facts.h is missing: run make lift GAME=${MC3_GAME}.")
endif()

# On the Switch: sdmc:/switch/<MC3_SD_DIR>/ for the game, saves, settings and
# caches; files in it are named after MC3_APP (<MC3_APP>.nro, _env.txt, _log.txt).
# Per game: mcdub3 / mcdub3-nx for the DUB Edition, mcdub3remix /
# mcdub3remix-nx for the Remix (config/<game>/game.env says the same for
# the scripts). The executable is named after MC3_APP too.
if(MC3_GAME STREQUAL "remix")
    set(_app "mcdub3remix")
    set(MC3_GAME_TITLE "Midnight Club 3: DUB Edition Remix")
    set(MC3_DISC_PREFIX "game-remix")
else()
    set(_app "mcdub3")
    set(MC3_GAME_TITLE "Midnight Club 3: DUB Edition")
    set(MC3_DISC_PREFIX "game")
endif()
set(MC3_APP "${_app}" CACHE STRING "Application name: the NRO, the executable and their files")
set(MC3_SD_DIR "${_app}-nx" CACHE STRING "Switch data directory, sdmc:/switch/<this>")
set(XBOXRECOMP_APP_NAME "${MC3_SD_DIR}" CACHE STRING "" FORCE)

# Vulkan (nv2a_vk; NVK on the Switch) instead of OpenGL (nv2a_gl).
option(MC3_VULKAN "Render with Vulkan instead of OpenGL" OFF)
set(XBOXRECOMP_VULKAN ${MC3_VULKAN} CACHE BOOL "" FORCE)

# Windows: the same renderer and SDL host as Linux and macOS, not the
# toolkit's Direct3D 11 path (MC3 draws through the pushbuffer executor).
if(WIN32)
    set(XBOXRECOMP_WIN_SDL_GL ON CACHE BOOL "" FORCE)
endif()

# Browser (Emscripten, tools/build_web.sh): WebAssembly + WebGL 2 on the
# desktop path, SDL2 from Emscripten's port. Threads are Web Workers, so
# every object needs -pthread (shared memory); setjmp/longjmp are wasm
# exceptions everywhere (one model for every object).
if(EMSCRIPTEN)
    option(MC3_WEB_NODE "Build for Node.js (NODERAWFS: the host's files; headless boot test)" OFF)
    add_compile_options(-pthread -sSUPPORT_LONGJMP=wasm -sUSE_SDL=2)
    # The toolkit asks find_package(SDL2): Emscripten's port has no CMake package.
    file(WRITE "${CMAKE_BINARY_DIR}/cmake/FindSDL2.cmake"
"if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
        INTERFACE_COMPILE_OPTIONS -sUSE_SDL=2 INTERFACE_LINK_OPTIONS -sUSE_SDL=2)
endif()
set(SDL2_FOUND TRUE)
")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}/cmake")
endif()

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB MC3_GEN_SOURCES CONFIGURE_DEPENDS "${MC3_GEN_DIR}/*.c")

set(MC3_SOURCES
    src/main.c
    ${MC3_MANUAL_C}
    src/ac97_nabm.c
    src/difftest.c
    src/bink_native.c
    src/autostart.c
)
if(NINTENDO_SWITCH)
    list(APPEND MC3_SOURCES src/switch_nx.c)
endif()
if(WIN32)
    list(APPEND MC3_SOURCES src/windows_host.c)   # settings, disc, log when double-clicked
endif()
if(EMSCRIPTEN)
    list(APPEND MC3_SOURCES src/web_fs.c)         # the disc over HTTP, saves in OPFS
endif()

add_executable(${PROJECT_NAME} ${MC3_SOURCES} ${MC3_GEN_SOURCES})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${MC3_APP}")
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
target_compile_definitions(${PROJECT_NAME} PRIVATE MC3_APP="${MC3_APP}" MC3_SD_DIR="${MC3_SD_DIR}"
    MC3_GAME_TITLE="${MC3_GAME_TITLE}" MC3_DISC_PREFIX="${MC3_DISC_PREFIX}")
# Version (config/<game>/app.env) and build ID (CI: the run; locally "local"),
# printed at boot so a log names exactly what ran.
set(MC3_VERSION "dev" CACHE STRING "Release version (config/<game>/app.env APP_VERSION)")
set(MC3_BUILD_ID "local" CACHE STRING "Build identifier (CI run, commit)")
set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
    MC3_VERSION="${MC3_VERSION}" MC3_BUILD_ID="${MC3_BUILD_ID}")
# The edition this package is for (make switch EDITION=eur ->
# mcdub3-nx-eur.nro); empty: the lifted one (config/xbe_facts.h).
set(MC3_PACKAGE_EDITION "" CACHE STRING "Edition of this package (usa, eur); empty: the lifted one")
if(MC3_PACKAGE_EDITION)
    set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
        MC3_PACKAGE_EDITION="${MC3_PACKAGE_EDITION}")
endif()

# Windows: the icon (MC3_WIN_ICON, made by tools/windows/win_icon.py) and
# the version Explorer shows, as a resource in mcdub3.exe -- the desktop
# shortcut takes its icon from there. Skipped without a resource compiler.
if(WIN32)
    include(CheckLanguage)
    check_language(RC)
    if(CMAKE_RC_COMPILER)
        enable_language(RC)
        string(REGEX MATCHALL "[0-9]+" _v "${MC3_VERSION}.0.0.0.0")
        list(SUBLIST _v 0 4 _v)
        string(REPLACE ";" "," _vnum "${_v}")
        set(_rc "${CMAKE_BINARY_DIR}/${MC3_APP}.rc")
        set(_rc_text "")
        if(MC3_WIN_ICON AND EXISTS "${MC3_WIN_ICON}")
            string(APPEND _rc_text "1 ICON \"${MC3_WIN_ICON}\"\n")
        endif()
        string(APPEND _rc_text
"1 VERSIONINFO
FILEVERSION ${_vnum}
PRODUCTVERSION ${_vnum}
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"Midnight Club 3: DUB Edition (${MC3_SD_DIR})\"
      VALUE \"ProductName\", \"${MC3_SD_DIR}\"
      VALUE \"ProductVersion\", \"${MC3_VERSION}\"
      VALUE \"FileVersion\", \"${MC3_VERSION}\"
      VALUE \"OriginalFilename\", \"${MC3_APP}.exe\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
        file(WRITE "${_rc}" "${_rc_text}")
        target_sources(${PROJECT_NAME} PRIVATE "${_rc}")
    endif()
endif()
if(MC3_VULKAN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MC3_VULKAN=1)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MC3_CONFIG_DIR}
    ${MC3_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

# The generated chunks are huge straight-line functions. -O1 keeps GCC's time
# and memory per chunk sane on the PC; the Switch's CPU is the bottleneck, so
# it gets -O2; the browser -O3 + wasm SIMD (no fast-math: x87 stays exact).
# -fwrapv / no strict aliasing: x86 semantics, not C's.
if(EMSCRIPTEN)
    set(MC3_GEN_OPT "-O3;-msimd128" CACHE STRING "Optimisation for the lifted code")
elseif(NINTENDO_SWITCH)
    set(MC3_GEN_OPT "-O2" CACHE STRING "Optimisation for the lifted code")
else()
    set(MC3_GEN_OPT "-O1" CACHE STRING "Optimisation for the lifted code")
endif()
set(MC3_GEN_FLAGS "${MC3_GEN_OPT};-w;-fno-strict-aliasing;-fwrapv")
# Debug info for the lifted C is gigabytes per build tree. Off by default:
# backtraces and gdb breakpoints still name every sub_XXXXXXXX (symbols stay);
# turn it on to step through lifted C line by line.
option(MC3_GEN_DEBUGINFO "Debug info (-g) for the lifted code" OFF)
if(NOT MC3_GEN_DEBUGINFO)
    list(APPEND MC3_GEN_FLAGS "-g0")
endif()
# Browser: lifted code reaches guest memory as g_guest + VA, g_guest a weak
# symbol at wasm address 0 (recomp_types.h): no jump or lookup tables, which
# wasm-ld would fill with g_guest + K as 0; address 0 is real memory.
if(EMSCRIPTEN)
    list(APPEND MC3_GEN_FLAGS "-fno-jump-tables" "-fno-delete-null-pointer-checks")
endif()
set_source_files_properties(${MC3_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${MC3_GEN_FLAGS}")
target_link_libraries(${PROJECT_NAME} PRIVATE m)

# Browser link: <app>.js + <app>.wasm. The game runs in a worker
# (PROXY_TO_PTHREAD), where it may block like a desktop main thread; WebGL 2
# from that worker through an OffscreenCanvas; WASMFS for the disc (HTTP)
# and saves (OPFS), src/web_fs.c. web/pre.js hands settings to getenv.
if(EMSCRIPTEN)
    target_link_options(${PROJECT_NAME} PRIVATE
        -O3 -pthread -sPROXY_TO_PTHREAD -sSUPPORT_LONGJMP=wasm -sUSE_SDL=2
        -sOFFSCREENCANVAS_SUPPORT -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        # No -sFULL_ES3 (client-side array emulation in every draw call; the
        # renderer draws from buffers only) and no JS-side GL error tracking.
        -sGL_ENABLE_GET_PROC_ADDRESS -sGL_TRACK_ERRORS=0
        # Fixed memory, no growth: with pthreads, growth makes every JS heap
        # access (the GL thread's WebGL calls) check for a grown buffer.
        -sWASMFS -sINITIAL_MEMORY=2GB
        -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB -sPTHREAD_POOL_SIZE=40
        # Guest RAM's first 64 MB at wasm address 0 (platform/wasm_memory.c):
        # Emscripten's data, stacks and heap above it.
        -sGLOBAL_BASE=67108864   # 0x04000000
        -sEXPORTED_RUNTIME_METHODS=ENV
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js"
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/audio.js"   # sound: AudioWorklet ring (apu_web.c)
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/net.js"     # System Link: mailbox <-> the page's switch link (xnet_web.c)
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/disc_cache.js"   # the disc cache (OPFS)
        # The disc backend: WASMFS fetch backend reading through a memory LRU
        # and the OPFS disc cache in 1 MiB blocks; replaces Emscripten's JS half.
        "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/web/wasmfs_fetch.js")
    # WebGPU renderer (ADR 0011): our own JS encoder calls navigator.gpu on the OffscreenCanvas in the render
    # pthread (no emdawnwebgpu/-sUSE_WEBGPU). Both files are js-libraries; wgpu_fixed.js holds plain fixed WGSL.
    foreach(f wgpu.js wgpu_fixed.js wgpu_stats.js)
        target_link_options(${PROJECT_NAME} PRIVATE "--js-library=${CMAKE_CURRENT_SOURCE_DIR}/web/${f}")
        set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/${f}")
    endforeach()
    if(MC3_WEB_NODE)
        target_link_options(${PROJECT_NAME} PRIVATE -sENVIRONMENT=node -sNODERAWFS)
    else()
        target_link_options(${PROJECT_NAME} PRIVATE -sENVIRONMENT=web,worker,node)
        # The start page beside the build (no game data: the disc is served
        # separately, tools/web_serve.py).
        configure_file(web/index.html "${CMAKE_BINARY_DIR}/index.html" @ONLY)
        configure_file(web/lan.js "${CMAKE_BINARY_DIR}/lan.js" COPYONLY)
        configure_file(web/rtc.js "${CMAKE_BINARY_DIR}/rtc.js" COPYONLY)
        configure_file(web/touch.js "${CMAKE_BINARY_DIR}/touch.js" COPYONLY)   # touch controls, phone page
        configure_file(web/disc_cache.js "${CMAKE_BINARY_DIR}/disc_cache.js" COPYONLY)
    endif()
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js;${CMAKE_CURRENT_SOURCE_DIR}/web/audio.js;${CMAKE_CURRENT_SOURCE_DIR}/web/net.js;${CMAKE_CURRENT_SOURCE_DIR}/web/disc_cache.js;${CMAKE_CURRENT_SOURCE_DIR}/web/wasmfs_fetch.js")
endif()

# Movies: FFmpeg decodes Bink video in place of the lifted decoder
# (src/bink_native.c). Linux only for now; the Switch needs its own LGPL,
# Bink-only FFmpeg build. Without FFmpeg the lifted decoder plays the movies.
option(MC3_NATIVE_BINK "Decode Bink movies (video and sound) with FFmpeg" ON)
if(MC3_NATIVE_BINK)
    # Linux: the system's shared FFmpeg. Switch: devkitPro's static
    # switch-ffmpeg from the portlibs (in the pinned container image; GPL
    # build, all decoders, NEON) -- linked with its static dependency list.
    if(NINTENDO_SWITCH AND NOT PKG_CONFIG_EXECUTABLE
       AND EXISTS "$ENV{DEVKITPRO}/portlibs/switch/bin/aarch64-none-elf-pkg-config")
        set(PKG_CONFIG_EXECUTABLE "$ENV{DEVKITPRO}/portlibs/switch/bin/aarch64-none-elf-pkg-config")
    endif()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        if(NINTENDO_SWITCH)
            pkg_check_modules(MC3_FFMPEG libavformat libavcodec libavutil)
        else()
            pkg_check_modules(MC3_FFMPEG IMPORTED_TARGET libavformat libavcodec libavutil)
        endif()
    endif()
    if(MC3_FFMPEG_FOUND)
        target_compile_definitions(${PROJECT_NAME} PRIVATE MC3_NATIVE_BINK=1)
        if(NINTENDO_SWITCH)
            target_include_directories(${PROJECT_NAME} PRIVATE ${MC3_FFMPEG_STATIC_INCLUDE_DIRS})
            target_link_libraries(${PROJECT_NAME} PRIVATE ${MC3_FFMPEG_STATIC_LDFLAGS})
        else()
            target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::MC3_FFMPEG)
        endif()
        # Movie sound plays on the runtime's software mixer (apu/apu.h).
        set_property(SOURCE src/bink_native.c APPEND PROPERTY
            INCLUDE_DIRECTORIES ${XBOXRECOMP_DIR}/src)
        message(STATUS "Bink movies: FFmpeg ${MC3_FFMPEG_libavcodec_VERSION}")
    else()
        message(STATUS "Bink movies: FFmpeg not found, lifted decoder")
    endif()
endif()

# ── Nintendo Switch homebrew (devkitA64 + libnx) ────────────────
if(NINTENDO_SWITCH)
    set(MC3_SWITCH_TITLE "Midnight Club 3 (Xbox recomp)" CACHE STRING "Title in hbmenu")
    set(MC3_SWITCH_AUTHOR "${MC3_SD_DIR}" CACHE STRING "Author in hbmenu")
    set(MC3_SWITCH_VERSION "0.0.1" CACHE STRING "Version in hbmenu")   # switch/build.sh sets all four
    set(MC3_SWITCH_ICON "" CACHE FILEPATH
        "256x256 baseline JPEG icon (switch/build.sh: from config/<game>/app.env APP_ICON; libnx default when empty)")

    if(MC3_VULKAN)
        target_link_libraries(${PROJECT_NAME} PRIVATE nx m)    # NVK comes with nv2a_vk
    else()
        # stdc++ after Mesa: its GLSL compiler is C++, and with FFmpeg's static
        # flags in the line, CMake put Mesa after SDL2's -lstdc++ (2026-10-16).
        target_link_libraries(${PROJECT_NAME} PRIVATE EGL glapi drm_nouveau stdc++ nx m)
    endif()

    set(MC3_NACP "${CMAKE_CURRENT_BINARY_DIR}/${MC3_APP}.nacp")
    nx_generate_nacp(OUTPUT "${MC3_NACP}"
        NAME "${MC3_SWITCH_TITLE}" AUTHOR "${MC3_SWITCH_AUTHOR}"
        VERSION "${MC3_SWITCH_VERSION}")
    if(EXISTS "${MC3_SWITCH_ICON}")
        nx_create_nro(${PROJECT_NAME} NACP "${MC3_NACP}" ICON "${MC3_SWITCH_ICON}")
    else()
        nx_create_nro(${PROJECT_NAME} NACP "${MC3_NACP}")
    endif()
endif()
