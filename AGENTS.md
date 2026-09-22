# Katabasis — contributor guide

Katabasis is a static binary recompiler: it takes a compiled 64-bit (arm64) iOS application and
turns it into a 32-bit (armv7) binary that runs unmodified on iOS 6, by lifting arm64 machine
code to LLVM IR (via Rellume), rewriting Objective-C metadata and data segments for the 32-bit
runtime, and re-emitting armv7 against a small support runtime plus generated ABI bridges. See
`README.md` for the pipeline overview and `CHANGELOG.md` for history. This file is the
contributor guide; see `$HOME/Git/projects/ios/AGENTS.md` for how this repository fits into the
wider workspace, and `$HOME/Git/projects/ios/charon/COORDINATION.md` for the current fleet.

## Layout

| Path | What it is |
| --- | --- |
| `xlate/` | The lifter: arm64 → LLVM IR, jump-table recovery, Objective-C metadata/data-bind rewriting, emits `lifted.bc`. Built with CMake. |
| `xlate/` (`xlgen`) | Generates guest↔host ABI bridges (calling convention, `va_list`, blocks, Objective-C message dispatch, data-symbol shims) from SDK headers. |
| `runtime/` | The armv7 support runtime: message routing, block bridging, weak references, ivar-layout reconciliation, Objective-C compatibility shims. |
| `scripts/translate.sh` | End-to-end pipeline: arm64 executable → armv7 iOS 6 executable. |
| `corpus/` | Small self-contained test apps (UI, blocks, drawing) — the translator's own regression suite. |
| `perf/` | On-device / in-emulator translated-vs-native benchmark. |
| `targets/` | One directory per real-world app being ported. Only `uistack3-official` (the Telegram port, current priority #1 — see `$HOME/Git/projects/ios/coordination/FLEET.md`) is actively maintained source; everything else under `targets/*-out*`, `*-arm64`, `*.app`, `*.ipa`, `*.log` is **generated or investigative material** — never read it as source, never grep it wholesale, never move it (matches the workspace contract's rule for `emulator-lab/`). |
| `.agent-work/` | Untracked, gitignored agent work area (plans, analysis, records, scratch) — see `$HOME/Git/projects/ios/AGENTS.md` §2 for the shape. |

## Building

Requires an LLVM 23 toolchain, a recent iOS SDK, `ld64`, `ldid`.

```sh
cmake -S xlate -B xlate/build && cmake --build xlate/build -j
scripts/translate.sh path/to/app-arm64 includes.h out/
```

`targets/uistack3-official` builds separately, through xmake and the `charon` addon (it is an
iOS 6 *app* target, not a translator run) — see the target-specific notes below.

## Conventions

- Keep translator changes **universal**: a fix must work for any input binary, not be
  special-cased to one app. App-specific behavior belongs in `targets/<app>/`, not in
  `xlate/`/`runtime/`.
- Missing target-OS APIs are **reported, not stubbed** by the translator — that belongs to the
  separate backports effort (`charon`), not here. The one exception is when a target genuinely
  needs a backport that doesn't exist yet: route that request to the relevant backports band,
  don't improvise a stub in `runtime/`.
- Run `corpus/` (and `perf/` where possible) before and after a translator change.
- No comments explaining *what* code does; only the non-obvious *why*, matching the rest of the
  workspace's style.
- Forward-only, same as the rest of the workspace: no destructive git operations without
  explicit sign-off, nothing that another session produced gets reverted or discarded.

## `targets/uistack3-official`: building and verifying

This target links against `charon`'s apple-ios toolchain and `apple-backports`/`swift-runtime`
packages via xmake, not via the translator pipeline above (the Telegram binary itself arrives
through a separate Swift/Objective-C recompilation chain feeding `objs/*.o`, `main.mm`,
`stubs.m` in this directory — that chain lives in the working scratchpad, not in this repo).

```sh
cd targets/uistack3-official
XMAKE_GLOBALDIR=<parent-of-your-.xmake> xmake -y
```

Post-check success looks like: no `error: these imports are not exported by the device's iOS`,
possibly followed by `imports: every non-weak import of the ... slices ... resolves against ...
exports`.

### Traps hit while getting this target to build (all measured, not theoretical)

- **`XMAKE_GLOBALDIR` is the *parent* of `.xmake`, not `.xmake` itself** — xmake appends `.xmake`
  internally (`core/base/global.lua`: `path.join(rootdir, "." .. xmake._NAME)`). Setting it to a
  path that already ends in `.xmake` produces a nested `.xmake/.xmake`, and packages resolved
  under the wrong (empty) nested tree fail with confusing toolchain errors
  (`clang: error: invalid linker name in argument '-fuse-ld=...'` pointing at a path that
  doesn't exist).
- **`add_addons("charon latest")` is not purely driven by this project's own
  `add_repositories`/lockfile.** xmake keeps a *store-global* `<XMAKE_GLOBALDIR>/addons/
  addons.conf` that records every version of the `charon` addon ever resolved by *any* project
  sharing that store, plus one `active` pointer xmake actually loads — independent of what the
  current project's `add_repositories`/`add_addons` line says, and independent of the
  project-local `xmake-addons.lock` (deleting the local lock alone does not force a fresh
  resolution; it gets regenerated from the global `active` pin). If a `XMAKE_GLOBALDIR` is
  inherited/shared across sessions, check `addons/addons.conf`'s `charon.active` before trusting
  that "latest" means current `charon` `main` — it may be pinned to a stale commit from an
  unrelated session's checkout, in which case the fix is editing that file's `active` field to
  point at a version whose on-disk content actually matches current `charon` `main` (verify with
  `diff` against the live checkout's `modules/apple/{dyld,platform}.lua`, not by trusting the
  version label).
- **The `ordering`/`exempt` split in `dyld.lua:check()` (commit `b159226`) is correct.** A weak
  symbol bound to the wrong system framework, while a *provided* library (backports,
  shared Swift runtime) exports it, is reported as a warning — not a build failure — as long as
  the offending image is that provided library's own (e.g. `libswiftFoundation.dylib` linking
  `Foundation` ahead of `libFoundationBackports.dylib` when *it* was built is the swift-runtime
  package's problem, not this program's). If you see every such entry landing in `missing`
  instead of being exempted, suspect the addon-version trap above before suspecting the exempt
  logic itself — confirm the actually-loaded `dyld.lua` is a recent one before re-diagnosing
  `check()`.
- **Ordering violations in *this program's own* binary are correctly refused, not exempted** —
  only another package's own image gets the warning-not-refusal treatment. If uistack3's own
  executable (not a `provided`/carried library) shows up as the offending binary in an `ordering`
  entry, that is a genuine project link-order bug: the fix is in this target's link flags
  (`libFoundationBackports`/`libUIKitBackports` before the corresponding system framework), not
  in `charon`.
- **Libraries linked by raw absolute path (`add_ldflags("/path/to/lib.dylib", ...)`) are invisible
  to the app-bundling and post-check machinery** unless also routed through `charon.libraries`/
  `app.frameworks` (carried) or a proper `add_packages()` (provided). `dyld.check` will correctly
  refuse them as neither provided nor carried even though they link fine. Resolved for this
  target's four ffmpeg dylibs by giving them a real local `package()` in `xmake.lua`
  (`set_sourcedir` + `on_install` copying the prebuilt `.dylib`s, no `add_urls`/no remote fetch)
  and carrying it through `set_values("app.frameworks", "uistack-ffmpeg")` — see
  `.agent-work/plan-and-analysis/uistack3-dyld-exempt/status.md` for the full story, including
  two upstream bugs the checks correctly caught: the system linker stamping
  `LC_ENCRYPTION_INFO` on armv7 output (iOS 6 refuses to load it — build with charon's own
  `ld64`), and the upstream build stripping the shared libraries before charon's own
  pointer-mode check could read their symbol table (`--disable-stripping`; charon's own
  pipeline strips them afterward).
- **For a local, no-download xmake `package()` (`set_sourcedir`, no `add_urls`), declare
  `add_links(...)` as a top-level package DSL call**, the same place every real xmake-repo
  package puts it (see `brotli`'s recipe for the pattern) — not `package:add("links", ...)`
  inside `on_install` (silently doesn't reach consuming targets) and never inside `on_fetch`
  unless you intend to replace installation entirely: **`on_fetch` returning a non-nil value
  makes xmake treat the package as already satisfied and skip `on_install` outright.** A package
  stuck that way installs nothing, anywhere, with zero trace under `XMAKE_GLOBALDIR/packages` —
  which looks exactly like a caching bug and isn't one.

## Devices

iPhone 4S and iPad 2, both iOS 6.1.3. Follow the device discipline in
`$HOME/Git/projects/ios/coordination/FLEET.md` and `$HOME/Git/projects/ios/charon/COORDINATION.md`
§5: `xmake device claim` is mandatory before any device use and a failed claim is a stop; hold it
briefly. Never `killall SpringBoard`/`backboardd`, never respring. Only `revtouch tap X Y` (in
points) delivers a touch. Verify which UDID a tunnel actually serves
(`ps | grep iproxy`) before writing to a device — the port number means nothing. The 4S carries
the owner's real Telegram account: never read, log, or forward chat/contact content, never tap a
chat-list row, and don't leave Telegram open after a check.
