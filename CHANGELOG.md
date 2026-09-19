# Changelog

All notable changes to Katabasis. This project is a research prototype; versions
are informal.

## Unreleased

### Milestone
- OneBusAway 2.3.2 (official arm64 App Store IPA) recompiled to armv7/iOS 6 reaches
  its full interactive main screen, rule-free, and responds to touch input.
- On-device benchmark (iPhone 4S / A5): translated compute is ~1.9× native armv7
  (geomean); translation-machinery resident overhead ~0.4 MB.

### Universal translator fixes
- **ivar layout**: never slide guest ivars *down* when the host superclass is smaller
  than the arm64 one; the lifted code addresses ivars with arm64 offsets, so shrinking
  the instance size left the last ivar's 8-byte slot past the allocation and corrupted
  memory nondeterministically. (Surfaced by OneBusAway.)
- **resilient lifting**: a function Rellume cannot model (e.g. hand-written crash-reporter
  assembly) is skipped and becomes a runtime trap, instead of aborting the whole build.
- **floating-point data symbols**: bridge system `double`/`CGFloat` globals
  (`NSFoundationVersionNumber`, `UIWindowLevelNormal`, ...) via a startup-filled guest
  copy in the guest's float width.
- **scale / module splitting**: a large app (e.g. an xkcd reader that statically links
  Realm) can lift into a single object whose inter-function BL branches exceed the armv7
  ±32 MB range. When the direct-call compile hits "Relocation out of range", the lifted
  module is split into range-sized pieces (`llvm-split`) and each is compiled separately,
  so cross-piece calls become relocations and ld64 inserts branch islands where needed —
  no long calls, no text relocations, and no cost for small and medium apps.
- **jump-table recovery, va_list, Objective-C metadata, block bridging**: see `docs/research.md`.

### Known limitations
- Swift is not supported (Swift metadata is 64-bit-only; separate effort).
- iOS 7+ system APIs an app calls must exist on the target; missing ones are reported,
  not stubbed (separate backports effort).
- Very large translated binaries (100 MB+, e.g. the Realm-linked xkcd reader) link cleanly
  via module splitting and load on a real iOS 6 device (dyld maps all segments and reaches
  symbol binding). The bundled emulator's dyld faults on such an image — an emulator limit,
  not a size or translation limit. An app that then references iOS 7+ system symbols (xkcd
  uses `NSURLSession`, absent from iOS 6 Foundation) stops at dyld symbol binding; that is a
  missing-OS-API matter for a separate backports effort, not the translator.
