#!/bin/sh
# check.sh -- corpus/swiftconformance: a control for runtime/bridge.m's
# _dyld_register_func_for_add_image bridge (the fix for the _DictionaryStorage.allocate crash --
# see coordination/reviews/2026-09-27-... and .agent-work/plan-and-analysis/swift-c/status.md,
# "Session 4"). fixture.swift is the intended, minimal, portable version of this control: a
# protocol conformance (Widget: Hashable) that only a __swift5_proto record in the GUEST image
# makes findable, discovered at run time through `x is Hashable` -- exactly the mechanism the
# bridge fixes, without needing the real demo's crash to reproduce it.
#
# It could not be built this round: this machine has only Xcode's Command Line Tools (no full
# Xcode), and the xmake iphoneos-sdk package is an SDK-only extraction (headers and libs, not
# swiftc's iOS cross-compilation resource files -- `swiftc -target arm64-apple-ios12.0 -sdk ...`
# fails with "Cannot read legacy layout file ... layouts-arm64.yaml", a file only a full Xcode
# installs). Confirmed: `xcode-select -p` names only CommandLineTools, `mdfind` finds no Xcode.app
# anywhere on this machine. fixture.swift is checked in anyway (compiles and prints "hashable" when
# run through the plain macOS `swiftc` in this same directory, confirmed as an oracle) so a real
# Xcode toolchain, whenever one is available, can build and add it to this control.
#
# What this DOES run today: the same underlying mechanism, already compiled, already known to
# crash without the bridge (swift/demo-arm64's own Dictionary<String,Int> literal needs String's
# Hashable conditional conformance to instantiate _DictionaryStorage<String,Int>). Builds the real
# demo twice via scripts/translate.sh -- once normally (bridge active) and once with
# XL_EXTRA_CFLAGS=-DXL_TEST_NO_DYLD_IMAGE_BRIDGE=1 (a build compiled with the guest-image feeding
# loop in _dyld_register_func_for_add_image compiled OUT, everything else identical) -- and expects
# a real device to show the control crashing where the normal build does not.
# Needs a claimed device (workspace skill device-session): CHARON_DEVICE_HOLDER and CHARON_DEVICE
# exported, the claim held. Output is kept in out/ (untracked): normal/, control/, run-normal.txt,
# run-control.txt.
set -eu
LAB=$(cd "$(dirname "$0")/../.." && pwd)
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
CHARON=${CHARON:-$HOME/Git/projects/ios/charon}
DEMO=${XL_SWIFTCONFORMANCE_DEMO:-$LAB/../../../swift/demo-arm64}
R3=${XL_SWIFTCONFORMANCE_LIBS:-$LAB/.agent-work/runs/swift-c/r3/libs}
mkdir -p "$out"
[ -x "$DEMO" ] || { echo "no prebuilt Swift demo at $DEMO (see this file's own header comment: no Swift-iOS cross-compiler here to build fixture.swift itself, let alone a fresh demo)"; exit 1; }

export XL_REPLACES="/usr/lib/libc++.1.dylib=$R3/libc++.1.dylib"
scripts_translate() { (cd "$LAB" && sh scripts/translate.sh "$DEMO" "$LAB/targets/swiftdemo-includes.h" "$1" "$R3/libswiftCore.dylib" "$R3/libc++.1.dylib" "$R3/libc++abi.1.dylib"); }

XL_EXTRA_CFLAGS= scripts_translate "$out/normal"
XL_EXTRA_CFLAGS="-DXL_TEST_NO_DYLD_IMAGE_BRIDGE=1" scripts_translate "$out/control"

cat > "$out/copy.lua" <<'LUA'
import("device", {rootdir = path.join(os.getenv("HOME"), "Git/projects/ios/charon/modules")})
function main(src, dst) device.copy(device.bind(os.projectdir(), nil), src, dst) end
LUA
(cd "$CHARON" && xmake l "$out/copy.lua" "$out/normal/demo" /var/tmp/xl-demo-normal)
(cd "$CHARON" && xmake l "$out/copy.lua" "$out/control/demo" /var/tmp/xl-demo-control)
(cd "$CHARON" && xmake device run "chmod +x /var/tmp/xl-demo-normal && /var/tmp/xl-demo-normal; echo exit=\$?; rm -f /var/tmp/xl-demo-normal") > "$out/run-normal.txt" 2>&1 || true
(cd "$CHARON" && xmake device run "chmod +x /var/tmp/xl-demo-control && /var/tmp/xl-demo-control; echo exit=\$?; rm -f /var/tmp/xl-demo-control") > "$out/run-control.txt" 2>&1 || true

grep -q "^exit=139$" "$out/run-control.txt" || { echo "FAIL: control (bridge compiled out) did not crash (SIGSEGV, exit 139) as expected"; cat "$out/run-control.txt"; exit 1; }
if grep -q "^exit=139$" "$out/run-normal.txt"; then
  echo "FAIL: normal build (bridge active) still crashed the same way"
  cat "$out/run-normal.txt"
  exit 1
fi
echo "ok: control crashes without the bridge (exit 139), normal build does not"
