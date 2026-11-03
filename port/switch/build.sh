#!/bin/bash
# The Switch's dhd.nro (finding 144) from build/wasm/dhd_core.wasm, inside the build image:
#   docker build -t dhd-switch-dev port/switch
#   docker run --rm -v "$PWD:/src" dhd-switch-dev bash port/switch/build.sh [build_host64.py options]
# SDL3 first (port/switch/build_sdl3.sh); DHD_VUTEXT or assets/vutext.bin as for every target.
set -euo pipefail
cd "$(dirname "$0")/../.."
SDL=$PWD/build/port/sdl3-switch
[ -f "$SDL/lib/libSDL3.a" ] || bash port/switch/build_sdl3.sh
VENV=build/port/venv-switch
[ -x "$VENV/bin/python" ] || { python3.12 -m venv "$VENV" && "$VENV/bin/pip" install -q -r requirements.txt; }
export DEVKITPRO=/opt/devkitpro
exec "$VENV/bin/python" port/tools/build_host64.py --target switch-arm64 build/wasm/dhd_core.wasm \
  --sdl-lib "$SDL/lib/libSDL3.a" --sdl-include "$SDL/include" \
  --sdl-libs "-lglad -lEGL -lglapi -ldrm_nouveau -lstdc++ -lpthread" "$@"
