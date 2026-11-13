#!/bin/bash
# sync the Windows checkout into WSL's ext4 and build the Linux port; run from Git Bash
set -e
SRC="/mnt/c/Users/Coola/Desktop/KIMI K3 PROJECTS/Golf Test/OutlawGolf2-XBE-Recomp"
MSYS2_ARG_CONV_EXCL='*' wsl -d Ubuntu -- /bin/bash -c '
set -e
mkdir -p ~/og2
rsync -a --delete --exclude .git --exclude build --exclude build-linux "/mnt/c/Users/Coola/Desktop/KIMI K3 PROJECTS/Golf Test/OutlawGolf2-XBE-Recomp/" ~/og2/
cd ~/og2
CC=clang CXX=clang++ cmake -G Ninja -B build-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo > build-linux/configure.log 2>&1 || { tail -30 build-linux/configure.log; exit 1; }
cmake --build build-linux -j"$(nproc)" > build-linux/build.log 2>&1 || true
grep -E "error:|FAILED:" build-linux/build.log | head -50
tail -3 build-linux/build.log
'
