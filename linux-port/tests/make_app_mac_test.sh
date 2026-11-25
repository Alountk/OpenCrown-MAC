#!/bin/sh
# Tests of packaging/make-app-mac.sh in dry mode (DCP_APP_DRY=1 prints the commands instead of running them). Usage: make_app_mac_test.sh <make-app-mac.sh>
script="$1"
fail=0
check() { if ! eval "$2"; then echo "FAIL: $1"; fail=1; fi; }

tmp=$(cd "$(mktemp -d "${TMPDIR:-/tmp}/dcp_app_XXXXXX")" && pwd)
trap 'rm -rf "$tmp"' EXIT
out=$(DCP_APP_DRY=1 "$script" "$tmp/out" 2>&1); rc=$?
check "dry run succeeds" '[ $rc -eq 0 ]'
check "builds the launcher as an arm64 DCP_RECOMPILED release" 'echo "$out" | grep "^+ cmake -S" | grep -q "DCP_RECOMPILED=ON" && echo "$out" | grep "^+ cmake -S" | grep -q "CMAKE_OSX_ARCHITECTURES=arm64" && echo "$out" | grep -q "^+ cmake --build .* --target opencrown_launcher"'
check "puts the package unpacker next to the launcher" 'echo "$out" | grep -q "dcp_pkg .*OpenCrown.app/Contents/MacOS/dcp_pkg"'
check "puts the launcher in Contents/MacOS" 'echo "$out" | grep -q "opencrown-launcher .*OpenCrown.app/Contents/MacOS"'
check "copies the source tree into Contents/Resources/linux-port" 'echo "$out" | grep -q "Contents/Resources/linux-port"'
check "the copied tree leaves build folders and dist out" 'echo "$out" | grep "Contents/Resources/linux-port" | grep -q -- "--exclude=build" && echo "$out" | grep "Contents/Resources/linux-port" | grep -q -- "--exclude=dist"'
check "writes an Info.plist" 'echo "$out" | grep -q "Info.plist"'
check "signs the app ad hoc" 'echo "$out" | grep -q "^+ codesign .*--sign - .*OpenCrown.app"'
check "builds AppIcon.icns from packaging/AppIcon.png with iconutil" 'echo "$out" | grep -q "^+ iconutil .*AppIcon.icns"'
check "puts the icon in Contents/Resources" 'echo "$out" | grep "AppIcon.icns" | grep -q "OpenCrown.app/Contents/Resources"'
check "the Info.plist names the icon" 'grep -q "<key>CFBundleIconFile</key><string>AppIcon</string>" "$script"'
png="$(dirname "$script")/AppIcon.png"
check "AppIcon.png is a 1024 x 1024 PNG" '[ "$(od -An -tx1 -N8 "$png" | tr -d " \n")" = 89504e470d0a1a0a ] && [ "$(od -An -tu1 -j16 -N8 "$png" | xargs)" = "0 0 4 0 0 0 4 0" ]'
check "makes a compressed DMG named with the version" 'echo "$out" | grep -q "^+ hdiutil create .*-format UDZO .*OpenCrown-[0-9.]*-mac\.[0-9][0-9]*\.dmg"'
check "the DMG holds the app and a link to Applications" 'echo "$out" | grep -q "^+ ln -s /Applications .*/Applications" && echo "$out" | grep "^+ hdiutil create" | grep -q -- "-srcfolder"'
check "the DMG is written next to the app" 'echo "$out" | grep "^+ hdiutil create" | grep -q "$tmp/out/OpenCrown-"'
cm="$(dirname "$script")/../CMakeLists.txt"
check "the Mac revision is stated once in CMakeLists.txt" '[ "$(grep -c "^set(OPENCROWN_MAC_REVISION " "$cm")" = 1 ]'
check "the plist build number is numeric (upstream version + Mac revision)" 'grep -q "<key>CFBundleVersion</key><string>\$version.\$revision</string>" "$script"'
check "nothing is created in dry mode" '[ ! -e "$tmp/out" ]'
[ $fail -eq 0 ] && echo "make-app-mac: all tests passed"
exit $fail
