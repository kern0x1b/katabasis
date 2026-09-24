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
| `targets/` | Untracked (`.gitignore`: `targets/*`, only `targets/*-includes.h` is kept): one directory per real-world app, living only on this disk. Everything under it — `*-out*`, `*-arm64`, `*.app`, `*.ipa`, `*.log`, `uistack3-official` — is **generated or investigative material**: never read it as source, never grep it wholesale, never move it. |
| `.agent-work/` | Untracked, gitignored agent work area (plans, analysis, records, scratch) — see `$HOME/Git/projects/ios/AGENTS.md` §2 for the shape. |

## Building

Requires an LLVM 23 toolchain, a recent iOS SDK, `ld64`, `ldid`.

```sh
cmake -S xlate -B xlate/build && cmake --build xlate/build -j
scripts/translate.sh path/to/app-arm64 includes.h out/
```

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

## Translation input

The input is an app's own compiled arm64 binary — the decrypted executable and embedded frameworks
of its `.ipa` — and nothing else. Never source, and never objects compiled from an edited copy of
the source. Reason: `targets/uistack3-official` linked `objs/*.o` from a scratchpad chain
(`graph.py` rewriting `#available`, 23 text replacements in 17 `patches/*.replace.json`, 2
`*.exclude` file lists, 31 directories of added `.swift` files) that edited
copies of the Telegram source, which the owner forbids (`coordination/FLEET.md`, 2026-09-24).
That chain was in `/private/tmp`, which was wiped on reboot. The target no longer builds and is
no longer this repository's: building Telegram from unmodified source belongs to the
Telegram-from-source band.

## The charon addon

- **Check an addon version only in your own `XMAKE_GLOBALDIR`**, the way charon's `tests/addon`
  does (`store_test.lua` sets `XMAKE_GLOBALDIR` for the `xmake addon --install`). On top of that,
  by fleet rule (not something `tests/addon` does): after cloning, delete the clone's `*.lock`, and
  read the shared `~/.xmake/addons/addons.conf` `active` before and after: they must match.
  Reason: on xmake 3.1.1 every addon install becomes the machine's `active` version (`coordination/crutches.md`, the xmake addon lock entry).
- **The payload under `~/.xmake/addons` is never edited by hand**: no copying files into it, no
  `xmake addon --remove`. A stale payload is diagnosed read-only
  (`.agents/skills/xmake-addon-refresh`) and handed to the coordinator.

## Devices

iPhone 4S and iPad 2, both iOS 6.1.3. The procedure for any device session — claim, run,
install, launch, tap, respring, cleanup — is the workspace skill `device-session`
(`$HOME/Git/projects/ios/.agents/skills/device-session/SKILL.md`); the fleet's device traps are
in `$HOME/Git/projects/ios/charon/COORDINATION.md` §5 "Traps". Specific to this repository: the
4S carries the owner's real Telegram account — never read, log, or forward chat or contact
content, never tap a chat-list row, and don't leave Telegram open after a check.

Workspace-wide procedures are skills in `$HOME/Git/projects/ios/.agents/skills/`: `device-session` (claim, run, install, launch, tap on a real device), `canon-install`, `patch-merge`, `worktree-sweep`, `session-handoff`, `band-launch`, `band-supervise`. A session started inside this repository does not list them — read `<name>/SKILL.md` there.
