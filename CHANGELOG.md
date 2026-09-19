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
- **scale / long calls**: when a large app lifts into a single object whose inter-function
  branches exceed the armv7 ±32 MB range, retry the compile with `-mlong-calls`; small and
  medium apps keep fast direct calls.
- **jump-table recovery, va_list, Objective-C metadata, block bridging**: see `docs/research.md`.
