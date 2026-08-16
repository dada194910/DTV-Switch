#!/bin/bash
# Build script for DecoTV Switch homebrew (.nro)
# Runs INSIDE the devkitpro/devkita64 container; /data is the mounted workspace.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"

echo "==> Ensuring devkitA64 toolchain + deko3d + glm + curl + dav1d ..."
# --needed：已安装的包直接跳过，避免每次构建重复下载（也规避 devkitPro CDN 偶发 403）
# switch-dev 是包组（libnx / devkitA64 / switch-tools / uam 等）；borealis(deko3d) 需要 deko3d 库
# 注意包名是 deko3d 而非 switch-deko3d；switch-glm 提供 glm 头文件（nanovg dk_renderer 需要）；
# switch-curl 提供 libcurl（无外部 SSL，HTTP 直连足够）
# switch-dav1d：switch-ffmpeg 预编译包依赖（AV1 解码）
dkp-pacman -S --noconfirm --needed switch-dev deko3d switch-glm switch-curl switch-dav1d

echo "==> Ensuring git + curl (for borealis clone / json fetch) ..."
command -v git  >/dev/null 2>&1 || pacman -S --noconfirm git
command -v curl >/dev/null 2>&1 || pacman -S --noconfirm curl

echo "==> Vendoring nlohmann/json.hpp (single header) ..."
mkdir -p /data/DecoTV-Switch/include/nlohmann
if [ ! -s /data/DecoTV-Switch/include/nlohmann/json.hpp ]; then
    curl -sL "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp" \
        -o /data/DecoTV-Switch/include/nlohmann/json.hpp
fi

echo "==> Fetching borealis (xfangfang fork @5f08b286 tarball, deko3d backend) ..."
# 用 tarball 快照而非 git clone：CI 容器里 git clone 曾被观察到工作区不完整
# （switch-libpulsar/deps.mk 缺失导致 include 失败），tarball 是 5f08b286 的
# 权威完整快照（switch-libpulsar/nanovg/libromfs 等全部 vendored 在内）
cd /data/DecoTV-Switch
if [ ! -d library/borealis/library ]; then
    mkdir -p library /tmp/xb
    curl -sL -H "User-Agent: DecoTV-CI" -m 120 -o /tmp/xb.tar.gz \
        "https://api.github.com/repos/xfangfang/borealis/tarball/5f08b286f3df737f3321d2247a6fe633fcead03c"
    tar -xzf /tmp/xb.tar.gz -C /tmp/xb
    # GitHub tarball 根目录名格式：<owner>-<repo>-<sha前10>（如 xfangfang-borealis-5f08b28）
    mv /tmp/xb/xfangfang-borealis-* library/borealis
fi
echo "==> borealis 就绪检查:"
ls library/borealis/library/lib/extern/switch-libpulsar/deps.mk && echo "  deps.mk OK"
ls library/borealis/resources/font/ >/dev/null && echo "  resources OK"

echo "==> Installing ffmpeg + libmpv (wiliwili prebuilt deko3d pkgs) ..."
# P4b 播放依赖：wiliwili 同款预编译包（已验证可下载）。
# libuam 是 switch-libmpv 的依赖，必须先装（libmpv 的音频/UI 库）
MPV_BASE="https://github.com/xfangfang/wiliwili/releases/download/v0.1.0"
for pkg in "libuam-f8c9eef01ffe06334d530393d636d69e2b52744b-1-any.pkg.tar.zst" \
           "switch-ffmpeg-7.1-1-any.pkg.tar.zst" \
           "switch-libmpv_deko3d-0.36.0-2-any.pkg.tar.zst"; do
    if [ ! -f "/tmp/$pkg" ]; then
        curl -sL -m 180 -o "/tmp/$pkg" "$MPV_BASE/$pkg" || echo "  !! download $pkg failed"
    fi
    echo "  installing $pkg ..."
    dkp-pacman -U --noconfirm "/tmp/$pkg" || echo "  !! install $pkg failed (see error above)"
done
# 确认 mpv 头文件可用（诊断路径）
echo "  PORTLIBS_PREFIX=$PORTLIBS_PREFIX"
for cand in "$PORTLIBS_PREFIX/include/mpv/client.h" \
            "/opt/devkitpro/portlibs/switch/include/mpv/client.h" \
            "/opt/devkitpro/portlibs/switch/include/mpv/client.h"; do
    if [ -f "$cand" ]; then echo "  mpv header found: $cand"; fi
done
ls "$PORTLIBS_PREFIX/include/" 2>/dev/null | head -20
ls "$PORTLIBS_PREFIX/lib/" 2>/dev/null | grep -iE "mpv|avcodec|libuam" | head
if [ -f "$PORTLIBS_PREFIX/include/mpv/client.h" ]; then
    echo "  mpv headers OK"
else
    echo "  !! mpv headers MISSING"
fi
echo "  mpv libs: $(pkg-config --static --libs mpv 2>/dev/null || echo 'pkg-config FAILED')"

echo "==> Building (classic devkitPro Makefile + borealis) ..."
cd /data/DecoTV-Switch
make -j"$(nproc)"

echo "==> Tagging versioned artifact ..."
VERSION=$(cat /data/DecoTV-Switch/VERSION 2>/dev/null || echo "0.1.0")
cp /data/DecoTV-Switch/DecoTV.nro "/data/DecoTV-Switch/DecoTV_v${VERSION}.nro"

echo "==> Done. Artifacts:"
ls -lh /data/DecoTV-Switch/DecoTV.nro /data/DecoTV-Switch/DecoTV_v${VERSION}.nro
