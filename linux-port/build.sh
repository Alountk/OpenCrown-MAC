#!/bin/sh
# Build the Linux tree. Needs: cmake (3.22+), ninja or make, g++ 12+ (or clang 15+).
# Ubuntu / WSL2: sudo apt install build-essential g++-12 cmake ninja-build
# dcp_run --render / dcp_viewer (SDL3 is fetched and built static; it needs these headers, the libraries are
# loaded at run time): sudo apt install pkg-config libx11-dev libxext-dev libxrandr-dev libxcursor-dev
#   libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libegl1-mesa-dev
#   libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libpulse-dev libasound2-dev libpipewire-0.3-dev
#   libudev-dev libdbus-1-dev
# Headless only (no SDL3): DCP_RENDER=OFF ./build.sh
set -e
cd "$(dirname "$0")"
# Ubuntu 22.04 defaults to g++ 11; prefer 12 when installed (better C++20).
if [ -z "$CXX" ] && command -v g++-12 >/dev/null 2>&1; then export CC=gcc-12 CXX=g++-12; fi
# macOS: the PS4's x86-64 code runs directly, so build x86_64 (Rosetta 2 on Apple Silicon). Needs cmake and ninja (brew).
mac=""
if [ "$(uname -s)" = "Darwin" ]; then mac="-DCMAKE_OSX_ARCHITECTURES=x86_64"; fi
gen=""
command -v ninja >/dev/null 2>&1 && gen="-G Ninja"
cmake -S . -B build $gen -DCMAKE_BUILD_TYPE=Release -DDCP_RENDER="${DCP_RENDER:-ON}" $mac
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
