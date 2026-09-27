# Katabasis

**Static binary recompiler that takes a compiled 64-bit (arm64) iOS application and turns it into a 32-bit (armv7) binary that runs on iOS 6 — without source, without rewriting the app.**

The name is κατάβασις, "the descent" — lowering a living, modern binary down into a long-dead operating system.

Katabasis lifts each arm64 function to LLVM IR (via [Rellume](https://github.com/aengelke/rellume)), rewrites the Objective-C metadata and data segments for the 32-bit runtime, re-emits the code as armv7, and links it against a small support runtime plus generated ABI bridges. The result is an ordinary armv7 Mach-O that a real iOS 6 device (or an emulator) loads and runs as a normal app.

This is a research prototype, not a product. It is complete enough to take a real 64-bit App Store application to a live, interactive screen on iOS 6.

## Status

- **OneBusAway 2.3.2** (official arm64 App Store IPA) recompiles to armv7/iOS 6 and reaches its full, interactive main screen — map view, search bar, tab bar, a native `UIAlertView`, and touch input (tapping *Cancel* dismisses the alert) — with **zero app-specific rules** and no source changes.
- Measured on an iPhone 4S (A5), the translated compute kernels run at a **geometric mean of ~1.9× native armv7** (1.05× for memory-bound hashes, up to ~2.9× for pointer-chasing), with a translation-machinery resident overhead of a few hundred KB.
- A second, unrelated app (an xkcd reader that statically links Realm and a crash reporter) drove out several **universal** translator fixes: resilient lifting, floating-point data-symbol bridging, and automatic long-call fallback for large binaries (see `CHANGELOG.md`).

## How it works

| Component | Role |
|---|---|
| `xlate/` | Lifts arm64 → LLVM IR (Rellume), recovers jump tables, rewrites Objective-C class/protocol/ivar metadata and data binds to the 32-bit layout, emits `lifted.bc`. |
| `xlgen/` (in `xlate/`) | Generates the guest↔host ABI bridges (calling-convention, `va_list`, blocks, Objective-C message dispatch, data-symbol shims) from the SDK headers. |
| `runtime/` | The armv7 support runtime: message routing, block bridging, weak references, ivar-layout reconciliation, and Objective-C compatibility shims. |
| `scripts/translate.sh` | The end-to-end pipeline: `arm64 executable` → armv7 iOS 6 executable. |
| `corpus/` | Small self-contained test apps (UI, blocks, drawing). |
| `perf/` | On-device and in-emulator translated-vs-native benchmark. |

The guest keeps its arm64 memory image (rebased below 4 GiB); code runs natively as armv7; only crossings into the system frameworks go through bridges.

## Building

Requires an LLVM 23 toolchain, a recent iOS SDK (for header-driven bridge generation), `ld64`, and `ldid`. Build the `xlate`/`xlgen` tools with CMake:

```sh
cmake -S xlate -B xlate/build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build xlate/build -j
```

Then translate an arm64 executable:

```sh
scripts/translate.sh path/to/app-arm64 includes.h out/
```

where `includes.h` imports the frameworks the app uses. The output `out/<name>` is an armv7 iOS 6 executable; drop it into the app bundle in place of the original.

### Guest libraries, and what a bind names

Further arguments are guest images lifted with the executable (`scripts/translate.sh app-arm64 includes.h out/ libA.dylib libB.dylib`;
`XL_FRAMEWORKS_DIR` finds a bundle's embedded frameworks). A bind takes an image by the install name it spells, exactly: an app that binds the
host's `NSString` next to an embedded image that defines its own keeps the host's. Two images with one install name are an error.

A guest image that stands for a *system* library under another name is said to do so by the invoker, because nothing in the inputs says it:
`XL_REPLACES="/usr/lib/libc++.1.dylib=path/to/libc++.1.dylib ..."` (`xlate --replaces LIBRARY=IMAGE`; a library named twice, or one that is an
image's own install name, is refused). Without it the bind is the host's, and xlate prints the `--replaces` line to use when an image's file name
matches.

The Swift demo needs this. It binds `/usr/lib/libc++.1.dylib`, and the guest libc++ it runs on (built by `charon@libcxx`, with libswiftCore and
libc++abi from `charon@swift-runtime` built with `library_evolution=true`) is `@rpath/libc++.1.dylib`, the recipe's name for it. Translate it with

```sh
XL_REPLACES=/usr/lib/libc++.1.dylib=<libs>/libc++.1.dylib \
  scripts/translate.sh swift/demo-arm64 targets/swiftdemo-includes.h out/ <libs>/libswiftCore.dylib <libs>/libc++.1.dylib <libs>/libc++abi.1.dylib
```

or the operators, guards and exception classes it takes from libc++ (307 names re-exported from libc++abi among them) come from the host's C++ runtime
instead of the guest's. The run takes about 12 minutes.

## Scope and limits

- Objective-C and C are supported. Swift is being brought up on the demo (`swift/`, `libswiftCore` lifted as a guest image, the run described above);
  it does not run to the end yet, and what a Swift program meets is measured one failure at a time (`.agent-work` status of the band).
- iOS 7+ system APIs the app calls must exist on the target; ones that don't are reported, not stubbed (they belong to a separate backports effort).
- Un-liftable functions (hand-written assembly in crash reporters, etc.) become traps that fault only if actually called.

## License

MIT — see `LICENSE`.
