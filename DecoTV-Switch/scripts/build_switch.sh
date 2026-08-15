#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain + deko3d ..."
# switch-dev 是包组（libnx / devkitA64 / switch-tools / uam 等）；borealis(deko3d) 需要 deko3d 库
# 注意包名是 deko3d 而非 switch-deko3d（且 switch-dev 组已含 deko3d，此处幂等补装）
dkp-pacman -S --noconfirm switch-dev deko3d

echo "==> Ensuring git (for borealis clone) ..."
command -v git >/dev/null 2>&1 || pacman -S --noconfirm git

echo "==> Fetching borealis (pinned to 20e2d33, deko3d backend, incl. switch-libpulsar submodule) ..."
cd /data/DecoTV-Switch
if [ ! -d library/borealis/.git ]; then
    mkdir -p library
    git clone --recurse-submodules https://github.com/natinusala/borealis.git library/borealis
    cd library/borealis
    git checkout 20e2d33b6c4ffce139ce304c503c04f5b94da920
    git submodule update --init --recursive
    cd /data/DecoTV-Switch
fi

echo "==> Building (classic devkitPro Makefile + borealis) ..."
cd /data/DecoTV-Switch
make -j"$(nproc)"

echo "==> Tagging versioned artifact ..."
VERSION=$(cat /data/DecoTV-Switch/VERSION 2>/dev/null || echo "0.1.0")
cp /data/DecoTV-Switch/DecoTV.nro "/data/DecoTV-Switch/DecoTV_v${VERSION}.nro"

echo "==> Done. Artifacts:"
ls -lh /data/DecoTV-Switch/DecoTV.nro /data/DecoTV-Switch/DecoTV_v${VERSION}.nro
