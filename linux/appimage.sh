#!/bin/bash
# Package the Linux build as an AppImage for Steam Deck. Run INSIDE the packaging environment (an Ubuntu 22.04 WSL
# distro or container: SteamOS 3's glibc must be newer than the build's, so the build itself must happen on an old
# enough base; Ubuntu 22.04's glibc 2.35 is the safe choice per port-feasibility.md).
#
#   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release   # in the packaging environment
#   bash linux/appimage.sh build
#
# The game ships no game code: the player's xbe is compiled at first start from the compiler/ folder beside the
# AppImage, exactly as on Windows. Game files (assets/) are the player's own, dropped beside the AppImage.
set -e
BUILD=${1:-build}
APP=OutlawGolf2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=$ROOT/out
rm -rf "$OUT"
mkdir -p "$OUT/$APP.AppDir/usr/bin" "$OUT/$APP.AppDir/usr/share/applications" "$OUT/$APP.AppDir/usr/share/icons/hicolor/256x256/apps"

# the executable and the first-launch compiler, beside it as the game expects
cp "$BUILD/$APP" "$OUT/$APP.AppDir/usr/bin/"
cp -r "$BUILD/compiler" "$OUT/$APP.AppDir/usr/bin/"

# SDL3 is static; glslang and the C++ runtime are linked in. The Vulkan loader is deliberately NOT bundled:
# the system's own, as the feasibility notes say.

cat > "$OUT/$APP.AppDir/usr/share/applications/$APP.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Outlaw Golf 2
Exec=$APP
Icon=$APP
Categories=Game;
Terminal=false
EOF
# appimagetool wants the desktop file at the AppDir root as well
cp "$OUT/$APP.AppDir/usr/share/applications/$APP.desktop" "$OUT/$APP.AppDir/$APP.desktop"

# the icon: a png from the repo if there is one, else a 1x1 placeholder so appimagetool is satisfied
ICON=$(find "$ROOT" -name "*.png" -not -path "*/out/*" -not -path "*/build*" 2>/dev/null | head -1)
if [ -z "$ICON" ]; then
  ICON="$OUT/$APP.png"
  echo "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==" | base64 -d > "$ICON"
fi
cp "$ICON" "$OUT/$APP.AppDir/usr/share/icons/hicolor/256x256/apps/$APP.png"
cp "$ICON" "$OUT/$APP.AppDir/$APP.png"

cat > "$OUT/$APP.AppDir/AppRun" <<EOF
#!/bin/bash
# the directory holding the AppImage is the game folder, as on Windows: the player's assets/ and the exe's
# settings, logs, hdd, shader cache and first-launch build live beside the image; the mount itself is read-only
HERE=\$(dirname "\$(readlink -f "\$0")")
if [ -n "\${APPIMAGE:-}" ]; then
  GAME_DIR=\$(dirname "\$(readlink -f "\$APPIMAGE")")
else
  GAME_DIR=\$HERE
fi
cd "\$GAME_DIR"
export XBE_GAME_DIR="\$GAME_DIR"
exec "\$HERE/usr/bin/$APP" "\$@"
EOF
chmod +x "$OUT/$APP.AppDir/AppRun"

# appimagetool: the AppDir holds everything (SDL3 is static; glslang and the C++ runtime are linked in; the Vulkan
# loader deliberately comes from the system, as the feasibility notes say)
if [ ! -x "$OUT/appimagetool-x86_64.AppImage" ]; then
  curl -L https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage -o "$OUT/appimagetool-x86_64.AppImage"
  chmod +x "$OUT/appimagetool-x86_64.AppImage"
fi

cd "$OUT"
./appimagetool-x86_64.AppImage --no-appstream "$APP.AppDir" "$APP-x86_64.AppImage"
echo "built $OUT/$APP-x86_64.AppImage"
