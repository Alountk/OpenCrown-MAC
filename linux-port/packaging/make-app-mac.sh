#!/bin/sh
# Builds OpenCrown.app for Apple Silicon: the launcher plus the source tree play-mac.sh needs (the game code is recompiled from the user's own
# dump at the first Play, so no game files or generated code are ever inside the app).
#   sh packaging/make-app-mac.sh [output folder]      (default: dist/ next to this folder); the icon is packaging/AppIcon.png
# It also writes OpenCrown-<version>-mac.<n>.dmg (the app and a link to Applications) in the same folder.
# The app is signed ad hoc, not notarized: the first time, open it with right click > Open. It still needs cmake and the Xcode command line
# tools on the Mac (play-mac.sh checks and says so). DCP_APP_DRY=1 prints the commands without running them (tests/make_app_mac_test.sh).
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)  # linux-port
out=${1:-$here/dist}
build=${DCP_APP_BUILD_DIR:-$here/build-app}
dry=${DCP_APP_DRY:-0}
die() { echo "make-app-mac: $*" >&2; exit 1; }
run() {
  echo "+ $*"
  [ "$dry" = 1 ] || "$@"
}

if [ "$dry" != 1 ]; then
  [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] || die "this script is for Apple Silicon Macs"
  command -v cmake >/dev/null 2>&1 || die "cmake not found (brew install cmake)"
fi
version=$(sed -n 's/^set(OPENCROWN_VERSION "\(.*\)").*/\1/p' "$here/CMakeLists.txt" | head -1)
[ -n "$version" ] || die "cannot read the version from CMakeLists.txt"
revision=$(sed -n 's/^set(OPENCROWN_MAC_REVISION "\(.*\)").*/\1/p' "$here/CMakeLists.txt" | head -1)
[ -n "$revision" ] || die "cannot read OPENCROWN_MAC_REVISION from CMakeLists.txt"
label="$version-mac.$revision"
app="$out/OpenCrown.app"

run cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DDCP_RECOMPILED=ON
run cmake --build "$build" --target opencrown_launcher -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

run rm -rf "$app"
run mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/linux-port"
run cp "$build/opencrown-launcher" "$app/Contents/MacOS/opencrown-launcher"
# The source tree, without build folders, the output folder or git data.
echo "+ tar -C $here --exclude=build* --exclude=dist --exclude=.git -cf - . | tar -C $app/Contents/Resources/linux-port -xf -"
if [ "$dry" != 1 ]; then
  tar -C "$here" --exclude='build*' --exclude=dist --exclude=.git -cf - . | tar -C "$app/Contents/Resources/linux-port" -xf -
fi

# The icon: packaging/AppIcon.png (drawn by make-icon.py) in the sizes macOS wants, packed into AppIcon.icns.
iconset="$build/AppIcon.iconset"
run rm -rf "$iconset"
run mkdir -p "$iconset"
for size in 16 32 128 256 512; do
  run sips -z "$size" "$size" "$here/packaging/AppIcon.png" --out "$iconset/icon_${size}x${size}.png"
  run sips -z "$((size * 2))" "$((size * 2))" "$here/packaging/AppIcon.png" --out "$iconset/icon_${size}x${size}@2x.png"
done
run iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"

echo "+ write $app/Contents/Info.plist"
if [ "$dry" != 1 ]; then
  cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>OpenCrown</string>
  <key>CFBundleDisplayName</key><string>OpenCrown</string>
  <key>CFBundleIdentifier</key><string>io.github.alountk.opencrown</string>
  <key>CFBundleExecutable</key><string>opencrown-launcher</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$label</string>
  <key>CFBundleVersion</key><string>$version.$revision</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
fi

run codesign --force --deep --sign - "$app"

# The disk image: the app and a link to Applications, so it is dragged in as usual.
dmg="$out/OpenCrown-$label.dmg"
stage="$build/dmg"
run rm -rf "$stage" "$dmg"
run mkdir -p "$stage"
run cp -R "$app" "$stage/OpenCrown.app"
run ln -s /Applications "$stage/Applications"
run hdiutil create -volname OpenCrown -srcfolder "$stage" -ov -format UDZO "$dmg"
echo "make-app-mac: $app"
echo "make-app-mac: $dmg"
