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
cmake -S xlate -B xlate/build && cmake --build xlate/build -j
```

Then translate an arm64 executable:

```sh
scripts/translate.sh path/to/app-arm64 includes.h out/
```

where `includes.h` imports the frameworks the app uses. The output `out/<name>` is an armv7 iOS 6 executable; drop it into the app bundle in place of the original.

## Scope and limits

- Objective-C and C are supported. Swift is not (Swift metadata is 64-bit-only and is a separate effort).
- iOS 7+ system APIs the app calls must exist on the target; ones that don't are reported, not stubbed (they belong to a separate backports effort).
- Un-liftable functions (hand-written assembly in crash reporters, etc.) become traps that fault only if actually called.

## License

MIT — see `LICENSE`.
