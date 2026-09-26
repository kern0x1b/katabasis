#!/bin/sh
# check.sh — translate a program that weakly imports a function iOS 6 has and xlgen cannot bridge, and
# check that the translation reports it as left unbound and defines no trap for it.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
xcrun clang -target arm64-apple-ios12.0 -isysroot "$SDK" -O2 -w "$here/main.c" -o "$out/WeakImport-arm64"
cp "$LAB/p3/includes.h" "$out/includes.h"
"$LAB/scripts/translate.sh" "$out/WeakImport-arm64" "$out/includes.h" "$out/xl"
grep -q '^_execl: UNSUPPORTED.*(weak import, left unbound)$' "$out/xl/report.txt" || { echo "FAIL: report.txt does not say _execl was left unbound"; exit 1; }
! grep -q 'xl_message_.*execl' "$out/xl/guest.m" || { echo "FAIL: guest.m defines a trap for _execl"; exit 1; }
echo ok
