# Pull requests and submission branches

State on 2026-11-21. Upstream is `velkajelka/linux-native-ableton` (`origin`); the fork is
`0x2ANHUK/linux-native-ableton` (`fork`). Bodies are kept here so a PR can be recreated or
updated without rewriting it: `docs/pr/scroll-wheel.md`, `docs/pr/packs-and-m4l.md`.

## 1. Scroll wheel — submitted

| | |
|---|---|
| Branch | `pr/scroll-wheel` = `a55dc93` = the wheel worktree's `feat/wheel-scroll-values` |
| Fork branch | `fork/wheel-scroll-values`, and `fork/pr/scroll-wheel` (same commit) |
| PR | **#8**, open, base `main` — https://github.com/velkajelka/linux-native-ableton/pull/8 |
| Body | `docs/pr/scroll-wheel.md` |
| State | The maintainer (or the owner) took it out of draft; no review comments yet |

Commits: `04291ea` (LIVE_WHEEL_TRACE), `055a533` (the value controls), `a55dc93` (rebuilt shim).

## 2. Packs + Edit in Max — prepared, not submitted

| | |
|---|---|
| Branch | `pr/packs-and-m4l` = the maxedit worktree's `feat/max-edit` (tip: `git log --oneline -1`), pushed to the fork |
| Fork branch | `fork/pr/packs-and-m4l` (same commit); `fork/feat/max-edit` is the same tree |
| PR | none yet |
| Body | `docs/pr/packs-and-m4l.md` |

Why one branch for two features: both need the same rebuilt `shim/libmenufix.so`, and the
repository ships the built shim. If #8 lands first, rebase this branch on the new `main` and
rebuild the shim (`./shim/build.sh`) so the binary contains both changes.

Commits on top of the wheel work: packs (`c064085`, `31ae80a`, `8da3649`, `a1c17a0`,
`7fc77bb`), Edit in Max (`d2753d4`…`b7e0152`, `da4407f`, `53cd0f5`, `39bff78`), and the docs
(`MILESTONES.md`, `HANDOFF.md`, this file).

## Commands

Create the second PR from the prepared branch and body:

```sh
cd ~/dev/linux-native-ableton-maxedit
git push fork pr/packs-and-m4l:refs/heads/pr/packs-and-m4l
gh pr create --repo velkajelka/linux-native-ableton --base main \
  --head 0x2ANHUK:pr/packs-and-m4l \
  --title "Pack contents in the browser, and Edit in Max on Linux (Live 12.4.7)" \
  --body-file docs/pr/packs-and-m4l.md
```

Update an existing PR's body after editing the file above:

```sh
gh pr edit 8 --repo velkajelka/linux-native-ableton --body-file docs/pr/scroll-wheel.md
```

Recreate the first PR (if its branch is ever lost):

```sh
cd ~/audio/linux-native-ableton
git push fork pr/scroll-wheel:refs/heads/wheel-scroll-values
gh pr create --repo velkajelka/linux-native-ableton --base main \
  --head 0x2ANHUK:wheel-scroll-values --title "Scroll wheel and trackpad change the value under the pointer (Live 12.4.7)" \
  --body-file ~/dev/linux-native-ableton-maxedit/docs/pr/scroll-wheel.md
```

Do not push the shim rebuild of one branch onto the other by force: `libmenufix.so` differs
between them, and a rebase needs a rebuild anyway.
