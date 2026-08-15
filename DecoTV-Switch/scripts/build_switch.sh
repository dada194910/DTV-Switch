#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain (libnx, switch-tools) ..."
# switch-dev 是包组（libnx / devkitA64 / switch-tools 等 13 个包）
# devkita64 镜像已内置大部分，这里幂等补装
dkp-pacman -S --noconfirm switch-dev

echo "==> Configuring (CMake, Switch toolchain) ..."
cd /data/DecoTV-Switch
mkdir -p build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" ..

echo "==> Building ..."
make -j"$(nproc)"

echo "==> Done. Artifact:"
ls -lh /data/DecoTV-Switch/build/DecoTV.nro
