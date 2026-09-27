#!/bin/sh
# check.sh — a guest image that defines a name a host library also has (NSString, getpid) answers a bind only when
# the bind names that image: the superclass pointer, a class reference and a call stub, each. Needs xlate/build.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
xlate=$LAB/xlate/build/xlate
CC="xcrun clang -target arm64-apple-ios12.0 -isysroot $SDK -w"
$CC -dynamiclib -install_name @rpath/libfake.dylib -lobjc "$here/fake.m" -o "$out/libfake.dylib"
$CC -dynamiclib -install_name @rpath/libbridge.dylib "$here/bridge.c" -o "$out/libbridge.dylib"
$CC -framework Foundation "$here/app.m" -o "$out/host-app"
$CC "$here/app.m" "$out/libfake.dylib" -framework Foundation -o "$out/guest-app"
kind() {
  "$xlate" --objc-manifest "$out/$1.json" "$out/$1" "$out/libfake.dylib"
  python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['images'][0]['classes'][0]['superclass']['kind'])" "$out/$1.json"
}
[ "$(kind host-app)" = import ] || { echo "FAIL: a bind to Foundation's NSString was taken by libfake"; exit 1; }
[ "$(kind guest-app)" = local ] || { echo "FAIL: a bind to libfake's NSString was not resolved to libfake"; exit 1; }

# The class reference and the stub are not in the manifest: they are resolved when the image is lifted. A bind that
# a guest image answers leaves no reference to the host symbol in the module; one it does not answer does.
$CC -framework Foundation "$here/classref.m" -o "$out/host-classref"
$CC "$here/classref.m" "$out/libfake.dylib" -framework Foundation -o "$out/guest-classref"
$CC "$here/call.c" -o "$out/host-call"
$CC "$here/call.c" "$out/libfake.dylib" -o "$out/guest-call"
nm -u "$out/host-classref" "$out/libfake.dylib" | grep '^_' | sort -u > "$out/passthrough.txt"
# A passthrough bind is a declaration of the host symbol in the lifted module.
lift() {
  "$xlate" --base 0x10000000 --output "$out/$1.bc" --layout "$out/$1.layout" --passthrough "$out/passthrough.txt" \
    --state-header "$out/$1.h" --bridge "$out/libbridge.dylib" "$out/$1" "$out/libfake.dylib" "$out/libbridge.dylib" 2> "$out/$1.err"
  /opt/homebrew/opt/llvm/bin/llvm-dis "$out/$1.bc" -o "$out/$1.ll"
}
lift host-classref
lift guest-classref
grep -qF '@"OBJC_CLASS_$_NSString" = external' "$out/host-classref.ll" || { echo "FAIL: a class reference to Foundation's NSString was taken by libfake"; exit 1; }
! grep -qF '@"OBJC_CLASS_$_NSString"' "$out/guest-classref.ll" || { echo "FAIL: a class reference to libfake's NSString was not resolved to libfake"; exit 1; }

# A call stub to libSystem's getpid has no answer in a guest image that merely defines getpid, and strict says so.
if "$xlate" --base 0x10000000 --output "$out/host-call.bc" --layout "$out/host-call.layout" --passthrough "$out/passthrough.txt" \
  --state-header "$out/host-call.h" "$out/host-call" "$out/libfake.dylib" 2> "$out/host-call.err"; then
  echo "FAIL: a call stub to libSystem's getpid was taken by libfake"; exit 1
fi
grep -q 'call stub for unresolved import _getpid' "$out/host-call.err" || { echo "FAIL: xlate refused host-call for another reason"; cat "$out/host-call.err"; exit 1; }
"$xlate" --base 0x10000000 --output "$out/guest-call.bc" --layout "$out/guest-call.layout" --passthrough "$out/passthrough.txt" \
  --state-header "$out/guest-call.h" "$out/guest-call" "$out/libfake.dylib" 2> "$out/guest-call.err" || { echo "FAIL: a call stub to libfake's getpid was not resolved to libfake"; cat "$out/guest-call.err"; exit 1; }
echo "ok"
