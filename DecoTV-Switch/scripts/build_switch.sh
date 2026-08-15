#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain + deko3d + glm + curl ..."
# --needed：已安装的包直接跳过，避免每次构建重复下载（也规避 devkitPro CDN 偶发 403）
# switch-dev 是包组（libnx / devkitA64 / switch-tools / uam 等）；borealis(deko3d) 需要 deko3d 库
# 注意包名是 deko3d 而非 switch-deko3d；switch-glm 提供 glm 头文件（nanovg dk_renderer 需要）；
# switch-curl 提供 libcurl（无外部 SSL，HTTP 直连足够）
dkp-pacman -S --noconfirm --needed switch-dev deko3d switch-glm switch-curl

echo "==> Ensuring git + curl (for borealis clone / json fetch) ..."
command -v git  >/dev/null 2>&1 || pacman -S --noconfirm git
command -v curl >/dev/null 2>&1 || pacman -S --noconfirm curl

echo "==> Vendoring nlohmann/json.hpp (single header) ..."
mkdir -p /data/DecoTV-Switch/include/nlohmann
if [ ! -s /data/DecoTV-Switch/include/nlohmann/json.hpp ]; then
    curl -sL "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp" \
        -o /data/DecoTV-Switch/include/nlohmann/json.hpp
fi

echo "==> Fetching borealis (xfangfang fork, wiliwili branch @5f08b286, deko3d backend) ..."
cd /data/DecoTV-Switch
if [ ! -d library/borealis/.git ]; then
    mkdir -p library
    git clone https://github.com/xfangfang/borealis.git library/borealis
    cd library/borealis
    git checkout 5f08b286f3df737f3321d2247a6fe633fcead03c
    cd /data/DecoTV-Switch
    # 无需子模块：switch-libpulsar/nanovg/libromfs 等全部 vendored 在仓库内
    # （.gitmodules 里的 glfw/SDL 是 PC 端用的，Switch 构建不需要）
    # 无补丁需求：dk_renderer.hpp 已含 <optional>；switch_ime 已不用 swkbdConfigSetStringLenMaxExt
fi

echo "==> Building (classic devkitPro Makefile + borealis) ..."
cd /data/DecoTV-Switch
make -j"$(nproc)"

echo "==> Tagging versioned artifact ..."
VERSION=$(cat /data/DecoTV-Switch/VERSION 2>/dev/null || echo "0.1.0")
cp /data/DecoTV-Switch/DecoTV.nro "/data/DecoTV-Switch/DecoTV_v${VERSION}.nro"

echo "==> Done. Artifacts:"
ls -lh /data/DecoTV-Switch/DecoTV.nro /data/DecoTV-Switch/DecoTV_v${VERSION}.nro
