#!/bin/sh
# check.sh — weak imports, by what the translated program does and by what strict says.
#   1. A call stub is weak or strong by its own image: a library that checks for getppid does not excuse an executable
#      that calls it. Needs xlate/build.
#   2. The translated program runs on the iPad 2 and prints what expected.txt says. Needs a claimed device
#      (workspace skill device-session): CHARON_DEVICE_HOLDER and CHARON_DEVICE exported, the claim held.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
CHARON=${CHARON:-$HOME/Git/projects/ios/charon}
mkdir -p "$out"
xlate=$LAB/xlate/build/xlate
CC="xcrun clang -target arm64-apple-ios12.0 -isysroot $SDK -O2 -w"

$CC -dynamiclib -install_name @rpath/libweaklib.dylib "$here/weaklib.c" -o "$out/libweaklib.dylib"
$CC "$here/strong.c" "$out/libweaklib.dylib" -o "$out/strong"
$CC "$here/weakonly.c" "$out/libweaklib.dylib" -o "$out/weakonly"
: > "$out/passthrough.txt"
lift() {
  "$xlate" --base 0x10000000 --output "$out/$1.bc" --layout "$out/$1.layout" --passthrough "$out/passthrough.txt" \
    --state-header "$out/$1.h" "$out/$1" "$out/libweaklib.dylib" 2> "$out/$1.err"
}
if lift strong; then echo "FAIL: strong's call stub to getppid passed because libweaklib imports it weakly"; exit 1; fi
grep -q 'strong: call stub for unresolved import _getppid' "$out/strong.err" || { echo "FAIL: strong was refused for another reason"; cat "$out/strong.err"; exit 1; }
! grep -q 'libweaklib.dylib: call stub' "$out/strong.err" || { echo "FAIL: libweaklib's weak stub was refused"; exit 1; }
lift weakonly || { echo "FAIL: a call stub that only libweaklib makes, weakly, was refused"; cat "$out/weakonly.err"; exit 1; }

$CC "$here/main.c" -o "$out/WeakImport-arm64"
cp "$LAB/p3/includes.h" "$out/includes.h"
"$LAB/scripts/translate.sh" "$out/WeakImport-arm64" "$out/includes.h" "$out/xl"
cat > "$out/copy.lua" <<'LUA'
import("device", {rootdir = path.join(os.getenv("HOME"), "Git/projects/ios/charon/modules")})
function main(src, dst) device.copy(device.bind(os.projectdir(), nil), src, dst) end
LUA
(cd "$CHARON" && xmake l "$out/copy.lua" "$out/xl/WeakImport" /var/tmp/WeakImport)
(cd "$CHARON" && xmake device run "chmod +x /var/tmp/WeakImport && /var/tmp/WeakImport; rm -f /var/tmp/WeakImport") > "$out/run.txt"
diff "$here/expected.txt" "$out/run.txt" || { echo "FAIL: the translated program printed something else"; exit 1; }
echo "ok"
