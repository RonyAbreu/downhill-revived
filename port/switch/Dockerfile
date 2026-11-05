# Build environment for the Nintendo Switch target (finding 144).
#   docker build -t dhd-switch-dev port/switch
# devkitPro's image brings devkitA64, libnx and the Switch portlibs; this adds what the shared
# pipeline needs (build_wasm.py, build_host64.py): LLVM, the wasi-sdk sysroot, WABT, SIMDe and
# Python 3.12. Versions follow docs/BUILDING.md.
FROM devkitpro/devkita64

ARG LLVM_VERSION=23.1.4
ARG WASI_SDK=34
ARG WABT=1.0.42

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git xz-utils zstd ninja-build cmake p7zip-full \
    && rm -rf /var/lib/apt/lists/*

ENV DHD_DEVCACHES=/opt/DevCaches
RUN mkdir -p $DHD_DEVCACHES/llvm $DHD_DEVCACHES/wasi-sdk $DHD_DEVCACHES/wabt \
    && curl -fsSL https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/LLVM-${LLVM_VERSION}-Linux-X64.tar.zst \
       | zstd -d --long=31 | tar -x -C $DHD_DEVCACHES/llvm --strip-components=1 \
    && curl -fsSL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK}/wasi-sdk-${WASI_SDK}.0-x86_64-linux.tar.gz \
       | tar -xz -C $DHD_DEVCACHES/wasi-sdk \
    && curl -fsSL https://github.com/WebAssembly/wabt/releases/download/${WABT}/wabt-${WABT}-linux-x64.tar.gz \
       | tar -xz -C $DHD_DEVCACHES/wabt \
    && git clone -q --depth 1 https://github.com/simd-everywhere/simde.git $DHD_DEVCACHES/simde-src

RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh \
    && UV_PYTHON_INSTALL_DIR=/opt/python uv python install 3.12 \
    && ln -s "$(UV_PYTHON_INSTALL_DIR=/opt/python uv python find 3.12)" /usr/local/bin/python3.12

ENV WASI_SYSROOT=$DHD_DEVCACHES/wasi-sdk/wasi-sdk-${WASI_SDK}.0-x86_64-linux/share/wasi-sysroot \
    SIMDE_DIR=$DHD_DEVCACHES/simde-src \
    WABT_DIR=$DHD_DEVCACHES/wabt/wabt-${WABT} \
    PATH=$DHD_DEVCACHES/llvm/bin:$DHD_DEVCACHES/wabt/wabt-${WABT}/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH

# The shared object cache (tools/objcache.py) stays off unless a folder is named, and this one is
# a Docker volume so it survives the container and lives on the engine's own disk rather than the
# bind mount:  docker run -v dhd-objcache:/objcache ...
# Without it every build recompiles all ~60 objects, vu0_interp.c alone taking about seven minutes.
# libpng for the texture packs and the button icons (port/switch/png_switch.c): the other
# targets decode PNG in Rust, which Horizon has none of.
RUN dkp-pacman -S --noconfirm --needed switch-libpng

ENV DHD_OBJCACHE_DIR=/objcache
VOLUME /objcache

WORKDIR /src
