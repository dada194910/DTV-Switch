#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain (libnx, switch-tools) ..."
# switch-dev 是包组（libnx / devkitA64 / switch-tools 等 13 个包）
# devkita64 镜像已内置大部分，这里幂等补装
dkp-pacman -S --noconfirm switch-dev

echo "==> Building (classic devkitPro Makefile) ..."
cd /data/DecoTV-Switch
make -j"$(nproc)"

echo "==> Tagging versioned artifact ..."
VERSION=$(cat /data/DecoTV-Switch/VERSION 2>/dev/null || echo "0.1.0")
cp /data/DecoTV-Switch/DecoTV.nro "/data/DecoTV-Switch/DecoTV_v${VERSION}.nro"

echo "==> Done. Artifacts:"
ls -lh /data/DecoTV-Switch/DecoTV.nro /data/DecoTV-Switch/DecoTV_v${VERSION}.nro
