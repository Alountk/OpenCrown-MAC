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
check "puts the launcher in Contents/MacOS" 'echo "$out" | grep -q "opencrown-launcher .*OpenCrown.app/Contents/MacOS"'
check "copies the source tree into Contents/Resources/linux-port" 'echo "$out" | grep -q "Contents/Resources/linux-port"'
check "the copied tree leaves build folders and dist out" 'echo "$out" | grep "Contents/Resources/linux-port" | grep -q -- "--exclude=build" && echo "$out" | grep "Contents/Resources/linux-port" | grep -q -- "--exclude=dist"'
check "writes an Info.plist" 'echo "$out" | grep -q "Info.plist"'
check "signs the app ad hoc" 'echo "$out" | grep -q "^+ codesign .*--sign - .*OpenCrown.app"'
check "nothing is created in dry mode" '[ ! -e "$tmp/out" ]'
[ $fail -eq 0 ] && echo "make-app-mac: all tests passed"
exit $fail
