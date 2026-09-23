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
- **Git holds source only.** Run output — device crash logs and syslogs, screenshots, message
  traces, script output (`report.txt`, `imports.txt`, `results.txt`), generated tables such as
  `*.absent.tsv`, survey dumps — goes to `.agent-work/`, never into a commit. Record the finding
  in a commit message, a facts line or a doc, and point at the file in `.agent-work/`, not at a
  tracked copy. Before committing, read `git status` and `git diff --cached --stat`: a new file
  that a script or a device wrote is not source.
- Device identifiers (UDIDs, crash-log `CrashReporter Key`s) and screenshots do not go even into
  `.agent-work/` notes — that's a standing privacy rule, not a git rule. Delete a screenshot once
  it has verified the state it was taken for.

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

### Traps

- **`XMAKE_GLOBALDIR` is the *parent* of `.xmake`, not `.xmake` itself** — xmake appends `.xmake`
  internally (`core/base/global.lua`: `path.join(rootdir, "." .. xmake._NAME)`). A path that
  already ends in `.xmake` nests it (`.xmake/.xmake`) and resolves packages against an empty
  tree, surfacing as opaque toolchain errors (e.g. `-fuse-ld=` naming a linker path that doesn't
  exist) rather than a clear "wrong directory" message.
- **`add_addons("charon latest")` resolves against a shared, already-installed payload directory
  (`<XMAKE_GLOBALDIR>/addons/charon/<version>/`) that nothing in the ordinary build path
  refreshes.** Editing `addons.conf`'s `active` pointer, clearing a project-local
  `xmake-addons.lock`/`.xmake` cache, or repointing `add_repositories` at a current checkout all
  leave that on-disk payload untouched. When it's stale, use the `xmake-addon-refresh` skill
  (`.agent/skills/xmake-addon-refresh/SKILL.md`) to diff and replace it file-by-file against a
  current `charon` checkout — never `xmake addon --remove` it, since the directory is shared
  machine-wide and a removal drops other bands' working plugins (`device`/`deb`/`emulate`) with
  it.
- **The `ordering`/`exempt` split in `dyld.lua:check()` (commit `b159226`) is correct**: a weak
  symbol bound to the wrong system framework is a warning, not a build failure, only when the
  offending image is itself a `provided` library's own (e.g. `libswiftFoundation.dylib` linking
  `Foundation` ahead of `libFoundationBackports.dylib` is the swift-runtime package's problem, not
  this program's). If every such entry lands in `missing` instead of being exempted, suspect a
  stale addon payload (above) before suspecting `check()` itself.
- **Ordering violations in this program's own binary are never exempted** — only another
  package's own image gets the warning-not-refusal treatment. If uistack3's own executable shows
  up as the offending binary in an `ordering` entry, fix this target's link order (the matching
  backports library before the system framework), not `charon`.
- **A library linked by raw absolute path (`add_ldflags("/path/to/lib.dylib", ...)`) is invisible
  to the app-bundling and post-check machinery** — `dyld.check` correctly refuses it as neither
  provided nor carried even though it links fine. Fix: give it a real local `package()`
  (`set_sourcedir` + `on_install` copying the prebuilt `.dylib`s, no `add_urls`) and carry it
  through `set_values("app.frameworks", "<package-name>")`, as done for this target's ffmpeg
  dylibs (`uistack-ffmpeg` in `xmake.lua`). Two related upstream bugs the check also catches:
  the system linker stamps `LC_ENCRYPTION_INFO` on armv7 output and iOS 6 refuses to load it
  (build with charon's own `ld64`), and an upstream build can strip its shared libraries before
  charon's pointer-mode check reads their symbol table (build with `--disable-stripping`; charon's
  own pipeline strips them afterward).
- **A local, no-download `package()` (`set_sourcedir`, no `add_urls`) needs `add_links(...)` as a
  top-level package DSL call**, the same place any xmake-repo package puts it (see `brotli`'s
  recipe, `~/.xmake/repositories/xmake-repo/packages/b/brotli/xmake.lua`) — not
  `package:add("links", ...)` inside `on_install`, which silently doesn't reach consuming targets.
- **`on_fetch` returning a non-nil value makes xmake treat the package as already satisfied and
  skip `on_install` outright.** A package stuck that way installs nothing, anywhere, with zero
  trace under `XMAKE_GLOBALDIR/packages` — which looks like a caching bug and isn't one.

Ground truth for "is it actually fixed", in order of trust:

1. `grep -c ordering <installed-dyld.lua>` and its `mtime`, compared against a fresh `charon`
   checkout's value — the only thing that reflects what's physically on disk.
2. The *final* build line, `[100%]: build ok` or an `error:` — never an intermediate
   `imports: ... resolves against N exports` line; those print during normal successful
   sub-checks too, and print identically in a build that goes on to fail at final link.
3. `addons.conf`'s `active` field and a build log's cosmetic `upgrade charon: ...` message are
   not evidence of anything — both can be current while the payload is still stale.

See `.agent-work/plan-and-analysis/uistack3-dyld-exempt/status.md` for the full record of wiring
in the ffmpeg dylibs.

## Devices

iPhone 4S and iPad 2, both iOS 6.1.3. Follow the device discipline in
`$HOME/Git/projects/ios/coordination/FLEET.md` and `$HOME/Git/projects/ios/charon/COORDINATION.md`
§5: `xmake device claim` is mandatory before any device use and a failed claim is a stop; hold it
briefly. Never `killall SpringBoard`/`backboardd`, never respring. Only `revtouch tap X Y` (in
points) delivers a touch. Verify which UDID a tunnel actually serves
(`ps | grep iproxy`) before writing to a device — the port number means nothing. The 4S carries
the owner's real Telegram account: never read, log, or forward chat/contact content, never tap a
chat-list row, and don't leave Telegram open after a check.
