---
name: xmake-addon-refresh
description: Refresh a stale charon xmake addon payload when a uistack3-official (or any charon-addon-consuming) build fails with a missing include, a missing symbol, or a check() result that doesn't match current charon source. Use when addons.conf's active pointer, the repository URL, or the project-local cache all look correct but the build still behaves like an old charon.
---

# Refresh a stale xmake addon payload

`add_addons("charon latest")` in a target's `xmake.lua` resolves against a shared,
already-installed payload directory, `<XMAKE_GLOBALDIR>/addons/charon/<version>/` (for the
"latest" bucket, `.../charon/latest/`). Nothing in the ordinary build path refreshes that
directory's *contents* once it exists on disk. If charon's source has moved on since that
directory was populated, the build keeps using the old payload no matter what else changes.

Symptoms that mean the payload — not the addon registration — is stale:

- `includes(@addon/charon/apple-ios) not found!` even though `addons.conf` names a version whose
  `repo.commit`/`repo.url` point at current `charon` `main`.
- A `dyld.lua` behavior (e.g. the `ordering`/`exempt` split) that doesn't match what's in the
  current `charon` checkout's `modules/apple/dyld.lua`.

Things that look like fixes but do not touch the payload, so don't waste a cycle on them first:

- Editing `addons.conf`'s `active` field to a version whose metadata points at current `charon` —
  a manifest entry is not evidence of a completed install.
- Deleting a project-local `xmake-addons.lock` or `.xmake` cache — lets the build run further
  instead of failing instantly on `includes()`, but the underlying payload on disk is untouched.
- Repointing this project's own `add_repositories` at a valid, current local clone —
  `add_addons("charon latest")` resolves against the shared, already-installed `latest` bucket,
  not a fresh resolution through the project's declared repository.

## Procedure

1. Find the active payload directory: `<XMAKE_GLOBALDIR>/addons/charon/<active-version-or-latest>/`.
2. Diff its `{modules,rules,toolchains,includes,plugins}` trees against the equivalent trees in a
   current `charon` `main` checkout — a plain recursive diff by path, not a version-label
   comparison.
3. Copy over **only the files that actually differ**, directly into the payload directory. Do not
   re-register the addon and do not run `xmake addon --remove` — that directory is shared
   machine-wide, and removing the addon drops every other in-flight build's working plugins
   (`device`/`deb`/`emulate`) out from under it. A hand-registered `addons.conf` entry that was
   never actually installed has caused exactly that kind of accidental removal before; don't
   repeat it.
4. Verify the replacement actually took, in order of trust:
   - `grep -c ordering <installed-dyld.lua>` and its `mtime`, compared against the fresh `charon`
     checkout's value — the only signal that reflects what's physically on disk.
   - The build's *final* line, `[100%]: build ok` or an `error:` — not an intermediate
     `imports: ... resolves against N exports` line, which prints during normal successful
     sub-checks too, and prints identically in a build that still fails at final link.
   - Do not trust `addons.conf`'s `active` field or a build log's cosmetic
     `upgrade charon: ...` message — both can be current while the payload is still stale.
