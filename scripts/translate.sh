#!/bin/sh
# translate.sh INPUT INCLUDES OUT — arm64 iOS executable to an armv7 iOS 6 executable.
set -eu
LAB=$HOME/Git/projects/ios/emulator-lab/recompile
SDK=$(ls -d $HOME/.xmake/packages/i/iphoneos-sdk/16.4/*/Developer.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS16.4.sdk | head -1)
LD=$(ls $HOME/.xmake/packages/l/ld64/956.6/*/bin/ld | head -1)
LLVM=/opt/homebrew/opt/llvm/bin
input=$1 includes=$2 out=$3
shift 3
extra_images="$*"
name=$(basename "$input" -arm64)
mkdir -p "$out"
GUEST="-target arm64-apple-ios12.0 -isysroot $SDK -O2 -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -w"
HOST="-target armv7-apple-ios6.0 -marm -isysroot $SDK -O2 -w"
for source in runtime data; do
  xcrun clang $GUEST -fno-builtin -I "$LAB/deps/BlocksRuntime" -c "$LAB/deps/BlocksRuntime/$source.c" -o "$out/blocks-$source.o"
done
nm -gU "$out"/blocks-*.o | awk 'NF==3 {print $3}' | sort -u > "$out/provided.txt"
{ for image in "$input" $extra_images; do xcrun dyld_info -imports "$image" | tail -n +3 | awk '{print $1}'; done; nm -u "$out"/blocks-*.o | grep '^_'; printf '_objc_retain\n_objc_release\n'; } | sort -u > "$out/all-imports.txt"
for image in $extra_images; do nm -gU "$image" | awk 'NF==3 {print $3}'; done | sort -u > "$out/images-provided.txt"
sort -u "$out/provided.txt" "$out/images-provided.txt" -o "$out/provided.txt"
grep -vxF -f "$out/images-provided.txt" "$out/all-imports.txt" > "$out/imports.txt" || true
"$LAB/xlate/build/xlate" --base "${XL_BASE:-0x10000000}" --objc-manifest "$out/manifest.json" "$input" $extra_images
"$LAB/xlate/build/xlgen" --sdk "$SDK" --resource-dir /opt/homebrew/opt/llvm/lib/clang/23 --includes "$includes" \
  --abi-header "$LAB/runtime/objc_abi.h" --symbols "$out/imports.txt" --guest-provided "$out/provided.txt" \
  --objc-manifest "$out/manifest.json" --guest-out "$out/guest.m" --host-out "$out/host.m" \
  --passthrough-out "$out/passthrough.txt" --report-out "$out/report.txt"
grep -oE 'xl_ivar_fixup_[0-9_]+\[\] = \{[^;]*\}' "$out/host.m" | grep -qE '\{\(int32_t \*\)0x[0-9a-f]+u, 0u\}' && { echo "ivar fixup with zero guest offset (ivar.offset_value not read from image)"; exit 1; } || true
# Visibility over device surprises: an imported symbol with no bridge becomes a trap that
# aborts if reached. List those (imports that the report marks UNSUPPORTED) at build time.
awk -F: 'FNR==NR{need[$1]=1;next} /UNSUPPORTED/{s=$1; if(need[s]) print "  " $0}' "$out/imports.txt" "$out/report.txt" > "$out/unbridged-imports.txt" || true
[ -s "$out/unbridged-imports.txt" ] && { echo "warning: imported symbols without a bridge (abort if reached):"; cat "$out/unbridged-imports.txt"; }
traps=$(grep -o 'xl_trap_[A-Za-z0-9_]*' "$out/guest.m" | sort -u | sed 's/^/-Wl,-U,_/' | tr '\n' ' ')
xcrun clang $GUEST -x objective-c -fno-objc-arc -fblocks -fno-builtin -c "$out/guest.m" -o "$out/guest.o"
xcrun clang -target arm64-apple-ios12.0 -isysroot "$SDK" -dynamiclib -install_name @rpath/libxl-guest.dylib \
  "$out/guest.o" "$out"/blocks-*.o -o "$out/libxl-guest.dylib" $traps
"$LAB/xlate/build/xlate" --base "${XL_BASE:-0x10000000}" --output "$out/lifted.bc" --layout "$out/layout.txt" --passthrough "$out/passthrough.txt" \
  --state-header "$out/xl_state.h" "$input" $extra_images "$out/libxl-guest.dylib"
# Compile the lifted code to a single object (fast path). A large app (e.g. one that
# statically links a heavy templated C++ library) can lift into a single object whose
# inter-function BL branches exceed the armv7 ±32 MB range ("Relocation out of range",
# reported by the ARM backend as "cannot compile inline asm"). Only then split the module
# into range-sized pieces and compile each separately: cross-piece calls become ordinary
# relocations, so ld64 inserts branch islands where needed and external calls keep their
# normal stubs -- no long calls, no text relocations, and no cost for small/medium apps.
lifted_objs="$out/lifted.o"
if ! $LLVM/clang $HOST -c "$out/lifted.bc" -o "$out/lifted.o" 2>"$out/lifted-cc.log"; then
  if grep -q 'out of range' "$out/lifted-cc.log"; then
    echo "lifted.o: out-of-range branches in a large module; splitting into range-sized objects" >&2
    rm -rf "$out/split"; mkdir -p "$out/split"
    "$LLVM/llvm-split" -j 8 -preserve-locals -o "$out/split/p" "$out/lifted.bc"
    lifted_objs=""
    for piece in "$out"/split/p[0-9]*; do
      case "$piece" in *.o) continue;; esac
      $LLVM/clang $HOST -x ir -c "$piece" -o "$piece.o"
      lifted_objs="$lifted_objs $piece.o"
    done
  else
    cat "$out/lifted-cc.log" >&2; exit 1
  fi
fi
$LLVM/clang $HOST -I "$out" -I "$LAB/runtime" -c "$LAB/runtime/runtime.c" -o "$out/runtime.o"
for source in "$LAB/runtime/bridge.m" "$LAB/runtime/objc_bridge.m" "$LAB/runtime/objc_compat.m" "$out/host.m"; do
  object="$out/$(basename "$source" .m).o"
  $LLVM/clang $HOST -fno-objc-arc -fblocks -I "$out" -I "$LAB/runtime" -c "$source" -o "$object"
  python3 "$LAB/scripts/rename_sections.py" "$object" toxl
done
# Bind APIs the backports provide (iOS 7+ classes like NSURLSession, UIAlertController)
# from the backports libraries rather than the stock frameworks, where they are absent on
# the target OS. Listing the backport dylibs BEFORE the frameworks makes the two-level
# linker resolve exactly the symbols they export from them (their install_names point at
# the on-device backports path), and -dead_strip_dylibs drops any backport dylib an app
# does not actually use, so a translated app depends only on the backports it needs.
# BACKPORTS_DIR points at an apple-backports build for this target band; unset skips it.
backport_libs=""; strip_dylibs=""
if [ -n "${BACKPORTS_DIR:-}" ] && ls "$BACKPORTS_DIR"/lib*Backports.dylib >/dev/null 2>&1; then
  backport_libs=$(echo "$BACKPORTS_DIR"/lib*Backports.dylib)
  strip_dylibs="-Wl,-dead_strip_dylibs"
fi
# Weak-link frameworks outside a core set that is always present on the target. This makes
# the image tolerant: a class an app references that the target lacks (e.g. WKWebView -- the
# public WebKit is iOS 8+, and iOS 6's WebKit framework exists but has no WKWebView, so a
# framework-presence test is not enough) resolves to nil and faults only if actually used,
# rather than blocking load. Core frameworks stay hard-linked; backported classes still bind
# from the backports libs above regardless of this.
CORE_FRAMEWORKS="Foundation CoreFoundation CoreGraphics UIKit QuartzCore CoreText Security"
framework_flags=""
for fwpath in $(otool -L "$input" | awk '/\.framework\// { print $1 }' | sort -u); do
  fw=$(basename "$fwpath")
  case " $CORE_FRAMEWORKS " in
    *" $fw "*) framework_flags="$framework_flags -framework $fw" ;;
    *) framework_flags="$framework_flags -weak_framework $fw" ;;
  esac
done
xcrun clang -target armv7-apple-ios6.0 -isysroot "$SDK" -fuse-ld="$LD" -Wl,-no_pie $strip_dylibs -Wl,-no_objc_category_merging -Wl,-no_deduplicate $(cat "$out/layout.txt") \
  $lifted_objs "$out/runtime.o" "$out/bridge.o" "$out/objc_bridge.o" "$out/objc_compat.o" "$out/host.o" \
  $backport_libs \
  $framework_flags \
  -framework Foundation -framework CoreGraphics -framework UIKit -lobjc -lz -o "$out/$name"
python3 "$LAB/scripts/rename_sections.py" "$out/$name"
ldid -S "$out/$name"
nm "$out/$name" | awk '$3 ~ /^_xl_guest_class_/ { if ("_xl_guest_class_" $1 != $3) { print "misplaced " $3 " at " $1; bad = 1 } } END { exit bad }'
otool -ov "$out/$name" | awk '/^[^ ]/ { class = $1 } /instanceSize +0$/ { print "zero instanceSize: " class; bad = 1 } END { exit bad }'
xmake l "$LAB/scripts/imports.lua" "$HOME/.charon/dyld/6.0/dyld_shared_cache_armv7" "$out/$name"
