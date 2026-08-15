#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain + borealis deps ..."
# switch-dev 是包组（libnx / devkitA64 / switch-tools 等）
# borealis 需要 switch-glfw / switch-mesa / switch-glm（装到 portlibs/switch）
dkp-pacman -S --noconfirm switch-dev switch-glfw switch-mesa switch-glm

echo "==> Ensuring git (for borealis clone) ..."
command -v git >/dev/null 2>&1 || pacman -S --noconfirm git

echo "==> Fetching borealis (natinusala/borealis) ..."
cd /data/DecoTV-Switch
if [ ! -d library/borealis/.git ]; then
    mkdir -p library
    git clone --depth 1 https://github.com/natinusala/borealis.git library/borealis
fi

echo "==> Building (classic devkitPro Makefile + borealis) ..."
cd /data/DecoTV-Switch
make -j"$(nproc)"

echo "==> Tagging versioned artifact ..."
VERSION=$(cat /data/DecoTV-Switch/VERSION 2>/dev/null || echo "0.1.0")
cp /data/DecoTV-Switch/DecoTV.nro "/data/DecoTV-Switch/DecoTV_v${VERSION}.nro"

echo "==> Done. Artifacts:"
ls -lh /data/DecoTV-Switch/DecoTV.nro /data/DecoTV-Switch/DecoTV_v${VERSION}.nro
