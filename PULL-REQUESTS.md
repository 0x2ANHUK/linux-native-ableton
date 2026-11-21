# Pull requests and submission branches

State on 2026-11-21. Upstream is `velkajelka/linux-native-ableton` (`origin`), the fork is
`0x2ANHUK/linux-native-ableton` (`fork`). Every branch below is independent: each carries the
`shim/libmenufix.so` built from its own source, so any of them can be merged first and the
others rebased and rebuilt (`. /shim/build.sh`).

Bodies live in this branch under `docs/pr/`, so a PR can be created or updated with
`--body-file` and nothing has to be rewritten.

## 1. Scroll wheel — submitted

| | |
|---|---|
| Branch | `pr/scroll-wheel` (`a55dc93`, = the wheel worktree's `feat/wheel-scroll-values`) |
| Fork branch | `fork/wheel-scroll-values`, `fork/pr/scroll-wheel` |
| PR | **#8**, open, base `main`, no longer a draft — https://github.com/velkajelka/linux-native-ableton/pull/8 |
| Body | `docs/pr/scroll-wheel.md` |

Commits: `04291ea` (LIVE_WHEEL_TRACE), `055a533` (the value controls), `a55dc93` (rebuilt shim).

## 2. Packs — prepared

| | |
|---|---|
| Branch | `pr/packs` (three commits on `main`, tip: `git log --oneline -1 pr/packs`) |
| Fork branch | `fork/pr/packs` |
| PR | none yet |
| Body | `docs/pr/packs.md` |

`fc5592c` the bug and its evidence · `7c20845` `tools/fix-pack-paths.py` + the installer step ·
`5f22600` the start-up rewrite in the shim and the rebuilt `libmenufix.so`.

## 3. Edit in Max — prepared

| | |
|---|---|
| Branch | `pr/max-editing` (three commits on `main`, tip: `git log --oneline -1 pr/max-editing`) |
| Fork branch | `fork/pr/max-editing` |
| PR | none yet |
| Body | `docs/pr/max-editing.md` |

`b35bb0f` `MAX_EDIT.md` · `a76c71c` the four editor fixes, the launch path and the wiring ·
`20ff2fd` the rebuilt binaries · `a1e7ff3` the README line.

## 4. State and handoff — this branch

`docs/state`: `MILESTONES.md`, `HANDOFF.md`, this file, and the three PR bodies. Not for
upstream; it is what the next agent (or the owner) reads first. Base it on `main` so it stays
mergeable.

## 5. Integration branch — local only

`feat/max-edit` (worktree `~/dev/linux-native-ableton-maxedit`) has wheel + packs + Edit in Max
in one tree, built as one shim. That is what is installed on this machine; it is **not** for
submission.

## Overlap between the two prepared PRs

`install.sh`, `README.md`, `shim/build.sh` and `shim/libmenufix.so` are touched by both.
Whoever lands first wins the merge; rebase the other branch, run `./shim/build.sh`, and
force-push that branch with `--force-with-lease`. The binary has to differ between the
branches (`libmenufix.so` holds the feature), so a merge conflict in it is expected, not a
mistake.

## Commands

Open the packs PR:

```sh
cd ~/dev/linux-native-ableton-maxedit
git push fork pr/packs:refs/heads/pr/packs
gh pr create --repo velkajelka/linux-native-ableton --base main --head 0x2ANHUK:pr/packs \
  --title "Pack folders were empty in the browser (Live 12.4.7)" \
  --body-file <(git show docs/state:docs/pr/packs.md)
```

Open the Edit in Max PR:

```sh
git push fork pr/max-editing:refs/heads/pr/max-editing
gh pr create --repo velkajelka/linux-native-ableton --base main --head 0x2ANHUK:pr/max-editing \
  --title "Edit in Max works on Linux (Live 12.4.7)" \
  --body-file <(git show docs/state:docs/pr/max-editing.md)
```

Recreate the wheel PR, or update its body:

```sh
cd ~/audio/linux-native-ableton
git push fork pr/scroll-wheel:refs/heads/wheel-scroll-values
gh pr create --repo velkajelka/linux-native-ableton --base main --head 0x2ANHUK:wheel-scroll-values \
  --title "Scroll wheel and trackpad change the value under the pointer (Live 12.4.7)" \
  --body-file <(git show docs/state:docs/pr/scroll-wheel.md)

gh pr edit 8 --repo velkajelka/linux-native-ableton \
  --body-file <(git show docs/state:docs/pr/scroll-wheel.md)
```

After one PR lands, rebase the others on the new `main`, rebuild `shim/libmenufix.so` in each,
and force-push that branch with `--force-with-lease` (the binary differs per branch by design).
