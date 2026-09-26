#!/bin/sh
# check.sh — a guest image that exports a class named like a host framework's takes the subclass
# of that class only from an app whose bind names that image. Needs xlate/build.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
CC="xcrun clang -target arm64-apple-ios12.0 -isysroot $SDK -w"
$CC -dynamiclib -install_name @rpath/libfake.dylib -lobjc "$here/fake.m" -o "$out/libfake.dylib"
$CC -framework Foundation "$here/app.m" -o "$out/host-app"
$CC "$here/app.m" "$out/libfake.dylib" -framework Foundation -o "$out/guest-app"
kind() {
  "$LAB/xlate/build/xlate" --objc-manifest "$out/$1.json" "$out/$1" "$out/libfake.dylib"
  python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['images'][0]['classes'][0]['superclass']['kind'])" "$out/$1.json"
}
[ "$(kind host-app)" = import ] || { echo "FAIL: a bind to Foundation's NSString was taken by libfake"; exit 1; }
[ "$(kind guest-app)" = local ] || { echo "FAIL: a bind to libfake's NSString was not resolved to libfake"; exit 1; }
echo "ok"
