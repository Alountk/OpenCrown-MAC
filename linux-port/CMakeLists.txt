cmake_minimum_required(VERSION 3.22)  # Ubuntu 22.04 stock cmake
project(OpenCrownLinux C CXX)
set(OPENCROWN_VERSION "1.1")  # Linux build; versioned separately from the Windows build in ../port
# Updater end-to-end tests build a fake newer release in a separate build dir (packaging/make-appimage.sh, OC_VERSION).
if(OPENCROWN_VERSION_OVERRIDE)
  set(OPENCROWN_VERSION "${OPENCROWN_VERSION_OVERRIDE}")
endif()

# Linux / Steam Deck build. Separate from the Windows tree (../port): own sources, own build, no shared files.
# Targets are added here as each part of src/ is ported (see README.md, Phase 1).
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  message(FATAL_ERROR "linux-port builds on Linux or macOS only (WSL2 works); the Windows build is ../port")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# arm64: the platform layer runs recompiled game code instead of the PS4's x86-64 code (see odd/tasks/arm64-static-recomp.md).
option(DCP_RECOMPILED "Run recompiled game code (arm64 macOS)" OFF)
if(DCP_RECOMPILED)
  add_compile_definitions(DCP_RECOMPILED=1)
endif()
add_compile_options(-Wall -Wextra)
if(APPLE)
  # ucontext.h needs _XOPEN_SOURCE on macOS; _DARWIN_C_SOURCE keeps the BSD/Mach extras visible.
  add_compile_definitions(_XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
endif()
# Release files must not carry this PC's paths (mbedTLS and assert() put __FILE__ into the binary): the source tree,
# the build dir and every FETCHCONTENT_SOURCE_DIR_* override are mapped to neutral names.
set(DCP_PREFIX_MAPS "${CMAKE_CURRENT_SOURCE_DIR}=." "${CMAKE_BINARY_DIR}=build")
get_cmake_property(_dcp_cache CACHE_VARIABLES)
foreach(_v IN LISTS _dcp_cache)
  if(_v MATCHES "^FETCHCONTENT_SOURCE_DIR_(.+)$" AND NOT "${${_v}}" STREQUAL "")
    list(APPEND DCP_PREFIX_MAPS "${${_v}}=deps/${CMAKE_MATCH_1}")
  endif()
endforeach()
foreach(_m IN LISTS DCP_PREFIX_MAPS)
  add_compile_options("-ffile-prefix-map=${_m}")
endforeach()
# Everything PIE: dcp_run must not load at 0x400000 itself, that is where the eboot wants to go (loader/elf.cpp).
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# File-format readers. Operate only on user-supplied game data; no game data lives in this tree.
add_library(dcp_formats STATIC
  src/formats/utf_table.cpp
  src/formats/crilayla.cpp
  src/formats/cpk.cpp
  src/formats/afs2.cpp
  src/formats/hca_header.cpp
  src/formats/eboot_image.cpp
  src/formats/hca_decoder.cpp
  src/formats/acb.cpp
  src/formats/gnm_detile.cpp
  src/formats/ftx.cpp
  src/formats/dds.cpp
  src/formats/bc7.cpp
  src/formats/mbs.cpp
  src/formats/mcb.cpp
  src/formats/bsb.cpp)
target_include_directories(dcp_formats PUBLIC src)

# Dump tools (CLI).
foreach(tool dcp_cpk dcp_tex dcp_audio dcp_sprite)
  add_executable(${tool} tools/${tool}/main.cpp)
  target_link_libraries(${tool} PRIVATE dcp_formats)
endforeach()

# Loader: maps the user's eboot into this process, relocates, binds imports to shims / module exports / stubs.
add_library(dcp_loader STATIC src/loader/nid.cpp src/loader/elf.cpp src/loader/hle.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86" OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
  target_sources(dcp_loader PRIVATE src/loader/cpu_compat.cpp)  # x86-only: replaces MOVBE / BMI1 / LZCNT with older instructions
else()
  target_link_libraries(dcp_loader PUBLIC dcp_rt)  # arm64: imports are dispatched by the recompiled-code runtime
endif()
target_include_directories(dcp_loader PUBLIC src)

include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # third-party projects below declare old minimums

# FreeType (FTL / GPLv2, we use it under the FTL) for the libSceFont shim. Fetched at configure time; no optional deps.
# Offline / if the download fails: -DFETCHCONTENT_SOURCE_DIR_FREETYPE=<unpacked freetype source>.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz)
FetchContent_MakeAvailable(freetype)

# miniupnpc (BSD-3) for UPnP IGD port mapping of the P2P UDP port (net/upnp.*, best effort). Pinned release tag;
# offline: -DFETCHCONTENT_SOURCE_DIR_MINIUPNPC=<unpacked miniupnp repo root> (the CMake project is in miniupnpc/).
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniupnpc
  URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_2_8.tar.gz
  SOURCE_SUBDIR miniupnpc)
FetchContent_MakeAvailable(miniupnpc)
target_compile_options(libminiupnpc-static PRIVATE -w)  # third-party

# mbedTLS 3.6 LTS (Apache-2.0) for the wss:// lobby link (net/tls.cpp) and SHA-256 (net/identity.cpp), static.
# The release tarball carries the generated sources (no Python needed). Offline: -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<dir>.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2)
FetchContent_MakeAvailable(mbedtls)
target_compile_options(freetype PRIVATE -w)  # third-party
foreach(t mbedtls mbedx509 mbedcrypto)
  target_compile_options(${t} PRIVATE -w)  # third-party
endforeach()

# Guest texture write tracking (render/texwatch.h): used by the file shim (dcp_hle) and the GL texture cache (dcp_render).
add_library(dcp_texwatch STATIC src/render/texwatch.cpp)
target_include_directories(dcp_texwatch PUBLIC src)

# P2P transport under the libSceRudp shim (Carrier interface + own reliable UDP framing); host C++, testable alone.
add_library(dcp_net STATIC src/net/net.cpp src/net/udp_carrier.cpp src/net/stun.cpp src/net/upnp.cpp src/net/rudp.cpp
  src/net/json.cpp src/net/ws.cpp src/net/lobby_client.cpp src/net/lobby_server.cpp src/net/tls.cpp
  src/net/online_check.cpp src/net/identity.cpp src/net/friends.cpp src/net/bug_report.cpp
  src/net/http.cpp src/net/update.cpp src/net/chat.cpp)
target_include_directories(dcp_net PUBLIC src)
target_link_libraries(dcp_net PUBLIC Threads::Threads PRIVATE libminiupnpc-static mbedtls mbedx509 mbedcrypto)

# HLE shims for the PS4 system libraries (kernel + services). -fno-exceptions: nothing may unwind through game frames.
add_library(dcp_hle STATIC src/hle/kernel/pure.cpp src/hle/kernel/memory.cpp src/hle/kernel/thread.cpp
  src/hle/kernel/file.cpp src/hle/kernel/misc.cpp src/hle/services.cpp src/hle/savedata.cpp src/hle/dialogs.cpp
  src/hle/pad.cpp src/hle/audio.cpp src/hle/video.cpp src/hle/gnm.cpp src/hle/font.cpp src/hle/net.cpp src/hle/np.cpp
  src/hle/matching2.cpp src/hle/net_test.cpp src/hle/skip_offers.cpp src/hle/confirm_button.cpp src/hle/own_camera.cpp src/render/capture.cpp)
target_include_directories(dcp_hle PUBLIC src)
target_compile_options(dcp_hle PRIVATE -fno-exceptions)
target_link_libraries(dcp_hle PUBLIC dcp_loader freetype dcp_texwatch dcp_net Threads::Threads)

# SDL3 (zlib) for the window, GL context, audio and input of dcp_run --render and dcp_viewer: same release as the
# Windows build (3.4.18), built static so the binaries (and the future AppImage) need no system SDL3. SDL still loads
# X11 / Wayland / libdecor / PulseAudio / PipeWire / ALSA / libudev / dbus / libGL at run time (dlopen), so only their
# dev headers are needed to build: see build.sh. GL entry points come through SDL_GL_GetProcAddress (no -lGL).
# Offline: -DFETCHCONTENT_SOURCE_DIR_SDL3=<unpacked SDL3 source>. -DDCP_RENDER=OFF builds a headless dcp_run only.
option(DCP_RENDER "dcp_run --render and dcp_viewer (fetches SDL3)" ON)
if(DCP_RENDER)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz)
  FetchContent_MakeAvailable(SDL3)
  target_compile_options(SDL3-static PRIVATE -w)  # third-party
  if(APPLE)  # SDL 3.4.18 calls getpagesize() under _XOPEN_SOURCE=700, which the current macOS SDK no longer declares
    target_compile_options(SDL3-static PRIVATE "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl_macos_compat.h")
  endif()

  # Stage 4 renderer: shared by dcp_viewer and dcp_run --render. Two devices under one replay (src/render/device.h):
  # OpenGL 3.3 (GL entry points come from the caller's loader) and SDL_gpu on Vulkan (SPIR-V prebuilt into
  # src/render/shaders/generated, so no shader compiler / Vulkan SDK is needed; SDL dlopens libvulkan at run time).
  add_library(dcp_render STATIC src/render/gl.cpp src/render/gl_device.cpp src/render/gpu_device.cpp src/render/png.cpp
    src/render/backend.cpp src/render/prompts.cpp)
  target_include_directories(dcp_render PUBLIC src)
  target_compile_definitions(dcp_render PUBLIC DCP_HAVE_SDL_GPU)
  target_link_libraries(dcp_render PUBLIC dcp_formats dcp_texwatch freetype SDL3::SDL3-static)
endif()

# dcp_run: --render (GL window / screenshots) with DCP_RENDER, headless layer-3 behaviour otherwise.
add_executable(dcp_run tools/dcp_run/main.cpp)

# Recompiled game code (arm64): the C++ that dcp_recompile wrote from the user's own eboot and PRX files. Pass the directory with
# -DDCP_RECOMPILED_DIR=<dir>; it holds eboot_*.cpp, libc_*.cpp and the *_table.cpp files. Linked as objects so the strong
# dcp_register_<module> definitions replace the weak ones in dcp_run.
if(DCP_RECOMPILED AND DCP_RECOMPILED_DIR)
  file(GLOB DCP_GEN_SOURCES CONFIGURE_DEPENDS "${DCP_RECOMPILED_DIR}/*.cpp")
  if(NOT DCP_GEN_SOURCES)
    message(FATAL_ERROR "DCP_RECOMPILED_DIR=${DCP_RECOMPILED_DIR} has no .cpp files: run dcp_recompile first")
  endif()
  add_library(dcp_game OBJECT ${DCP_GEN_SOURCES})
  target_include_directories(dcp_game PRIVATE recomp/runtime)
  target_compile_options(dcp_game PRIVATE -O1 -w)  # generated code: no warnings, a build time that stays in minutes
  target_sources(dcp_run PRIVATE $<TARGET_OBJECTS:dcp_game>)
endif()
target_link_libraries(dcp_run PRIVATE dcp_loader dcp_hle ${CMAKE_DL_LIBS})
if(DCP_RENDER)
  target_sources(dcp_run PRIVATE tools/dcp_run/render_loop.cpp tools/dcp_run/sdl_audio.cpp tools/dcp_run/sdl_input.cpp)
  target_compile_definitions(dcp_run PRIVATE DCP_HAVE_RENDER)
  target_link_libraries(dcp_run PRIVATE dcp_render SDL3::SDL3-static)

  # Asset viewer (CPK browser, textures, sprite animations).
  add_executable(dcp_viewer tools/dcp_viewer/main.cpp tools/dcp_viewer/anim.cpp)
  target_link_libraries(dcp_viewer PRIVATE dcp_formats dcp_render SDL3::SDL3-static)

  # End-user launcher (GUI): dump folder, controls (input.ini), display options, online, starts dcp_run next to it.
  # Dear ImGui (MIT), same release as the Windows build; offline: -DFETCHCONTENT_SOURCE_DIR_IMGUI=<unpacked source>.
  # No -lGL: imgui_impl_opengl3's own loader dlopens libGL.so.1, the launcher's few GL calls go through SDL_GL_GetProcAddress.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz)
  FetchContent_MakeAvailable(imgui)
  add_library(dcp_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)  # sdlgpu3: dcp_run's name-entry box / chat on the SDL GPU renderer
  target_include_directories(dcp_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_options(dcp_imgui PRIVATE -w)  # third-party
  target_link_libraries(dcp_imgui PUBLIC SDL3::SDL3-static ${CMAKE_DL_LIBS})
  # dcp_run's name-entry box (the PS4 keyboard dialog's stand-in) and the room chat overlay.
  target_sources(dcp_run PRIVATE tools/dcp_run/ime_box.cpp tools/dcp_run/chat_box.cpp)
  target_link_libraries(dcp_run PRIVATE dcp_imgui)

  add_executable(opencrown_launcher tools/launcher/main.cpp)
  target_link_libraries(opencrown_launcher PRIVATE dcp_imgui dcp_formats dcp_net)
  set_target_properties(opencrown_launcher PROPERTIES OUTPUT_NAME "opencrown-launcher")
  target_compile_definitions(opencrown_launcher PRIVATE OPENCROWN_VERSION="${OPENCROWN_VERSION}")
  if(APPLE)  # Play runs play-mac.sh from the source tree this was built from
    target_compile_definitions(opencrown_launcher PRIVATE DCP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()

# Mock lobby server (notes/map/lobby_protocol.md) for local multiplayer tests.
add_executable(dcp_lobby tools/dcp_lobby/main.cpp)
target_link_libraries(dcp_lobby PRIVATE dcp_net)

enable_testing()
add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE dcp_formats)
# hooks/eboot.txt (what dcp_recompile --hooks generates slots for) must list the functions capture.cpp hooks.
add_test(NAME hooks_list COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/src/render/capture.cpp
         -DLIST=${CMAKE_CURRENT_SOURCE_DIR}/hooks/eboot.txt -P ${CMAKE_CURRENT_SOURCE_DIR}/hooks/check_hooks.cmake)
add_test(NAME formats COMMAND test_formats)
add_executable(test_loader tests/test_loader.cpp)
target_link_libraries(test_loader PRIVATE dcp_loader)
add_test(NAME loader COMMAND test_loader)
add_executable(test_cpu_compat tests/test_cpu_compat.cpp)  # MOVBE / BMI1 / LZCNT replacements vs the native instructions
target_link_libraries(test_cpu_compat PRIVATE dcp_loader)
add_test(NAME cpu_compat COMMAND test_cpu_compat)
add_executable(test_hle tests/test_hle.cpp)
target_link_libraries(test_hle PRIVATE dcp_hle)
add_test(NAME hle COMMAND test_hle)
add_executable(test_save_check tests/test_save_check.cpp)  # launcher check for unreadable (encrypted) saves
add_test(NAME save_check COMMAND test_save_check)
add_test(NAME play_mac_online COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/play_mac_online_test.sh ${CMAKE_CURRENT_SOURCE_DIR}/play-mac-online-test.sh)  # in dry mode
add_test(NAME play_mac COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/play_mac_test.sh ${CMAKE_CURRENT_SOURCE_DIR}/play-mac.sh)  # play-mac.sh in dry mode
add_executable(test_net tests/test_net.cpp)  # loopback only
target_link_libraries(test_net PRIVATE dcp_net)
add_test(NAME net COMMAND test_net)
add_executable(test_lobby tests/test_lobby.cpp)  # against the in-process mock lobby (loopback) unless $DCP_LOBBY_TEST_URL
target_link_libraries(test_lobby PRIVATE dcp_net)
add_test(NAME lobby COMMAND test_lobby)
add_executable(test_touch tests/test_touch.cpp)  # touch screen / Steam Deck input logic (header only)
add_test(NAME touch COMMAND test_touch)
add_executable(test_launcher_platform tests/test_launcher_platform.cpp)  # launcher spawn / opener / exe path glue (header only)
add_test(NAME launcher_platform COMMAND test_launcher_platform)
add_executable(test_yield_throttle tests/test_yield_throttle.cpp)  # busy-wait throttle (header only)
add_test(NAME yield_throttle COMMAND test_yield_throttle)
add_executable(test_texwatch tests/test_texwatch.cpp)  # concurrent faults on one protected page
target_link_libraries(test_texwatch PRIVATE dcp_texwatch)
add_test(NAME texwatch COMMAND test_texwatch)
add_executable(test_update tests/test_update.cpp)  # launcher self-updater: logic + HTTP client on loopback only
target_link_libraries(test_update PRIVATE dcp_net)
add_test(NAME update COMMAND test_update)

# Static recompiler (host tool, runs on the user's machine against their own dump; see ../odd/tasks/arm64-static-recomp.md).
# Independent of the target architecture: it only decodes x86-64 bytes.
option(DCP_RECOMP "Build the static recompiler" ON)
if(DCP_RECOMP)
  FetchContent_Declare(zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG v4.1.0
    GIT_SHALLOW TRUE)
  set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
  set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(zydis)
  add_library(dcp_recomp STATIC recomp/src/decoder.cpp recomp/src/cfg.cpp recomp/src/lift.cpp)
  target_include_directories(dcp_recomp PUBLIC recomp/src)
  target_link_libraries(dcp_recomp PUBLIC Zydis)
  add_executable(test_recomp recomp/tests/test_recomp.cpp)
  target_link_libraries(test_recomp PRIVATE dcp_recomp)
  add_test(NAME recomp COMMAND test_recomp)
  # Differential test of the lifter against real x86-64 execution: needs an x86-64 host (native or Rosetta 2).
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR (NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    add_executable(test_lift_diff recomp/tests/test_lift_diff.cpp)
    target_include_directories(test_lift_diff PRIVATE recomp/runtime)
    target_link_libraries(test_lift_diff PRIVATE dcp_recomp ${CMAKE_DL_LIBS})
    target_compile_definitions(test_lift_diff PRIVATE
      DCP_TEST_CXX="${CMAKE_CXX_COMPILER}"
      DCP_TEST_RT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime"
      DCP_TEST_ARCH="$<IF:$<BOOL:${APPLE}>,-arch x86_64 -isysroot ${CMAKE_OSX_SYSROOT},>")
    add_test(NAME lift_diff COMMAND test_lift_diff)
  endif()
  # Runtime of recompiled code (guest-address dispatch). Plain C++, builds for the target architecture (arm64 too).
  add_library(dcp_rt STATIC recomp/runtime/dcp_dispatch.cpp recomp/runtime/dcp_guest.cpp)
  target_include_directories(dcp_rt PUBLIC recomp/runtime)
  add_executable(test_atomic recomp/tests/test_atomic.cpp)
  target_include_directories(test_atomic PRIVATE recomp/runtime)
  target_link_libraries(test_atomic PRIVATE Threads::Threads)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    target_compile_options(test_atomic PRIVATE -mcx16)  # cmpxchg16b inline instead of libatomic
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_atomic PRIVATE atomic)  # GCC calls libatomic for 16-byte compare-exchange even with -mcx16
  endif()
  add_test(NAME atomic COMMAND test_atomic)
  add_executable(test_abi recomp/tests/test_abi.cpp)
  target_include_directories(test_abi PRIVATE recomp/runtime)
  add_test(NAME abi COMMAND test_abi)
  add_executable(test_guest recomp/tests/test_guest.cpp)
  target_link_libraries(test_guest PRIVATE dcp_rt)
  add_test(NAME guest COMMAND test_guest)
  add_executable(test_dispatch recomp/tests/test_dispatch.cpp)
  target_link_libraries(test_dispatch PRIVATE dcp_rt)
  add_test(NAME dispatch COMMAND test_dispatch)
  # End to end: lift hand-assembled functions, compile them with the dispatch runtime, run them. Host architecture, any.
  set(DCP_E2E_ARCH "")
  if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
      set(DCP_E2E_ARCH "-arch ${CMAKE_OSX_ARCHITECTURES}")
    endif()
    string(APPEND DCP_E2E_ARCH " -isysroot ${CMAKE_OSX_SYSROOT}")
  endif()
  add_executable(test_dispatch_e2e recomp/tests/test_dispatch_e2e.cpp)
  target_include_directories(test_dispatch_e2e PRIVATE recomp/runtime)
  target_link_libraries(test_dispatch_e2e PRIVATE dcp_recomp ${CMAKE_DL_LIBS})
  target_compile_definitions(test_dispatch_e2e PRIVATE
    DCP_TEST_CXX="${CMAKE_CXX_COMPILER}"
    DCP_TEST_RT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime"
    DCP_TEST_ARCH="${DCP_E2E_ARCH}")
  add_test(NAME dispatch_e2e COMMAND test_dispatch_e2e)
  # Reads a decrypted PS4 module and finds its functions (shared by dcp_scan and dcp_recompile).
  add_library(dcp_recomp_module STATIC recomp/tools/module.cpp recomp/tools/emit.cpp)
  target_include_directories(dcp_recomp_module PUBLIC recomp/tools)
  target_link_libraries(dcp_recomp_module PUBLIC dcp_recomp dcp_loader)
  add_executable(test_module recomp/tests/test_module.cpp)
  target_link_libraries(test_module PRIVATE dcp_recomp_module ${CMAKE_DL_LIBS})
  target_include_directories(test_module PRIVATE recomp/runtime)
  target_compile_definitions(test_module PRIVATE
    DCP_TEST_CXX="${CMAKE_CXX_COMPILER}"
    DCP_TEST_RT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/recomp/runtime"
    DCP_TEST_ARCH="${DCP_E2E_ARCH}")
  add_test(NAME module COMMAND test_module)
  add_executable(dcp_recompile recomp/tools/dcp_recompile.cpp)
  target_link_libraries(dcp_recompile PRIVATE dcp_recomp_module)
  add_executable(dcp_scan recomp/tools/dcp_scan.cpp)
  target_link_libraries(dcp_scan PRIVATE dcp_recomp_module)
endif()
