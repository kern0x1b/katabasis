#!/bin/sh
# check.sh — classes laid out by the Swift runtime, and the calls that hand them to libobjc (main.c has the story).
#   1. main.c is built for macOS, where objc4 is the real thing, and run: expected.txt is what it prints, and must stay so.
#   2. The same program is translated from arm64 and run on the iPad 2, and must print the same. iOS 6 libobjc has none of
#      objc_readClassPair, _objc_realizeClassFromSwift and objc_setHook_lazyClassNamer, so the program cannot be built for it
#      natively: the macOS run is the oracle. Needs a claimed device (workspace skill device-session):
#      CHARON_DEVICE_HOLDER and CHARON_DEVICE exported, the claim held.
#   The output is kept in out/ (untracked): run-mac.txt, run.txt.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
CHARON=${CHARON:-$HOME/Git/projects/ios/charon}
mkdir -p "$out"

xcrun clang -arch arm64 -O0 -w "$here/main.c" -lobjc -o "$out/ClassPair-mac"
"$out/ClassPair-mac" > "$out/run-mac.txt"
diff "$here/expected.txt" "$out/run-mac.txt" || { echo "FAIL: objc4 on this Mac prints something else than expected.txt"; exit 1; }

# ld64 of the charon recipes: the linker of Xcode 15 and later drops __objc_clsrolist, which Swift binaries carry.
LD=$(ls $HOME/.xmake/packages/l/ld64/956.6/*/bin/ld | head -1)
xcrun clang -target arm64-apple-ios12.0 -isysroot "$SDK" -fuse-ld="$LD" -O2 -w "$here/main.c" -lobjc -o "$out/ClassPair-arm64"
"$LAB/scripts/translate.sh" "$out/ClassPair-arm64" "$here/includes.h" "$out/xl"
cat > "$out/copy.lua" <<'LUA'
import("device", {rootdir = path.join(os.getenv("HOME"), "Git/projects/ios/charon/modules")})
function main(src, dst) device.copy(device.bind(os.projectdir(), nil), src, dst) end
LUA
(cd "$CHARON" && xmake l "$out/copy.lua" "$out/xl/ClassPair" /var/tmp/ClassPair)
(cd "$CHARON" && xmake device run "chmod +x /var/tmp/ClassPair && /var/tmp/ClassPair 2>/dev/null; rm -f /var/tmp/ClassPair") > "$out/run.txt"
diff "$here/expected.txt" "$out/run.txt" || { echo "FAIL: the translated program prints something else than objc4 does"; exit 1; }
echo "ok"
