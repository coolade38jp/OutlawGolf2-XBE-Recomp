#!/bin/bash
# The release build: run INSIDE the Ubuntu 22.04 WSL distro (bash linux/build-release.sh from Git Bash runs it there).
# Builds with clang-21 (apt.llvm.org, for #embed) on the old glibc SteamOS needs, then packages the AppImage.
#   bash linux/build-release.sh
set -e
cd "$(dirname "$0")/.."
MSYS2_ARG_CONV_EXCL='*' wsl -d Ubuntu-22.04 -- /bin/bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake ninja-build git pkg-config rsync curl python3 ca-certificates gnupg \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools libwayland-dev wayland-protocols libx11-dev libxext-dev \
    libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxkbcommon-dev libxkbcommon-x11-dev libxss-dev libxtst-dev \
    libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdecor-0-dev 2>&1 | tail -2
if ! clang-21 --version > /dev/null 2>&1; then
  curl -sSL https://apt.llvm.org/llvm.sh -o /tmp/llvm.sh
  bash /tmp/llvm.sh 21
fi
apt-get install -y -qq lld-21 2>&1 | tail -1
# jammy cmake is 3.22, the project needs 3.25+
if ! cmake --version | grep -qE "version (3\.(2[5-9]|[3-9])|[4-9])"; then
  apt-get install -y -qq python3-pip 2>&1 | tail -1
  pip3 install --quiet cmake
fi
cmake --version | head -1
mkdir -p ~/og2
rsync -a --delete --exclude .git --exclude build --exclude build-linux --exclude build-release --exclude out \
    "/mnt/c/Users/Coola/Desktop/KIMI K3 PROJECTS/Golf Test/OutlawGolf2-XBE-Recomp/" ~/og2/
cd ~/og2
CC=clang-21 CXX=clang++-21 cmake -G Ninja -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"
bash linux/appimage.sh build-release
'
