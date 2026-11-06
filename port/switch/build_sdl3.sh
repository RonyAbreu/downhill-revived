#!/bin/bash
# SDL3 for the Switch (finding 167): devkitPro's fork, branch switch-sdl-3.4 (video, audio, joystick,
# timer and filesystem backends for Horizon), static, into build/port/sdl3-switch/.
# Inside the build image:  docker run --rm -v "$PWD:/src" dhd-switch-dev bash port/switch/build_sdl3.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=build/port/sdl3-switch-src
PREFIX=$PWD/build/port/sdl3-switch
REF=${SDL3_SWITCH_REF:-switch-sdl-3.4}
[ -d "$SRC" ] || git clone -q --depth 1 -b "$REF" https://github.com/devkitPro/SDL.git "$SRC"
cmake -S "$SRC" -B "$SRC/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
cmake --build "$SRC/build" -j"$(nproc)" >/dev/null
cmake --install "$SRC/build" >/dev/null
echo "SDL3 for the Switch: $PREFIX/lib/libSDL3.a ($(git -C "$SRC" rev-parse --short HEAD))"
