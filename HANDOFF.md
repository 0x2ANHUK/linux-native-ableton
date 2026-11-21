# Handoff for the next agent

Read this before touching the machine. It is the working state on marvin, not a tutorial for
the upstream project. Milestones and evidence: `MILESTONES.md`, `PACK-BUG.md`, `MAX_EDIT.md`,
`README.md`.

## Where things are

| Thing | Path |
|---|---|
| Live 12.4.7 (portable, this machine) | `~/audio/AbletonLive12-native-portable` |
| Live's data (portable mode: `HOME` is this) | `~/audio/AbletonLive12-native-portable/live-home` |
| Live binary / cwd it needs | `…/push3_opt/push3/AbletonLive12`, run with cwd `…/push3_opt/push3` |
| Installed shim | `…/shim/{libmenufix.so,libmaxstatfix.so,libpathfix.so,max-editor}` |
| Live's index DB (what the browser lists) | `live-home/.local/share/Ableton/Live Database/Live-files-12300.db` |
| Live log / indexer log | `live-home/.config/Ableton/Live 12.4.7/{Log.txt,Indexer.txt}` |
| Max editor logs (editor's `HOME` is also live-home) | `live-home/.config/Cycling '74/Max 9/Logs/Max.log` |
| Repo worktree for packs + Max editing | `~/dev/linux-native-ableton-maxedit` (`feat/max-edit`) |
| Repo worktree for the wheel work | `~/audio/linux-native-ableton` (`feat/wheel-scroll-values`) |
| Remotes | `origin` = velkajelka/linux-native-ableton, `fork` = 0x2ANHUK/linux-native-ableton |

## Restart Live so that the indexer actually runs

Live only scans and fixes packs at start-up, so most experiments need a restart. Two traps:

1. A killed Live leaves `live-home/.config/Ableton/Live 12.4.7/CrashRecoveryInfo.cfg`. The
   next start stops at "Live unexpectedly quit" and **never starts Ableton Index**. Move that
   file aside (do not delete it with `rm -f`; `mv` it) before starting.
2. `HOME` must be `live-home`, `DISPLAY=:1`, `WAYLAND_DISPLAY=wayland-1`,
   `XDG_RUNTIME_DIR=/run/user/1000`, cwd `…/push3_opt/push3`,
   `LD_PRELOAD=<prefix>/shim/libavx512emu.so:<prefix>/shim/libmenufix.so`,
   `LD_LIBRARY_PATH=<prefix>/push3_opt/push3/lib:<prefix>/compat_lib`. Without `DISPLAY`
   Live aborts in Qt immediately.

```sh
P=~/audio/AbletonLive12-native-portable
cd "$P/push3_opt/push3" && HOME=$P/live-home DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 \
  XDG_RUNTIME_DIR=/run/user/1000 AGENT_LIVE_STATE_PATH=$P/live-home/.local/state/agent-bridge/state.json \
  LD_PRELOAD=$P/shim/libavx512emu.so:$P/shim/libmenufix.so \
  LD_LIBRARY_PATH=$P/push3_opt/push3/lib:$P/compat_lib ./AbletonLive12
```

`MENUFIX_DEBUG=/tmp/shim.log` makes the shim log (`packmeta:`, `maxedit:`, `maxviews:`, …).

## Read the index instead of clicking

The browser is fed by the SQLite DB, so a fix can be judged without the UI:

```sh
DB="$P/live-home/.local/share/Ableton/Live Database/Live-files-12300.db"
sqlite3 "file:$DB?mode=ro" "select count(*) from files;"
sqlite3 "file:$DB?mode=ro" "select p.name, count(f.file_id) from places p
  left join files f on f.place_id = p.file_id group by p.name order by 2 desc;"
```

`places` holds the Places panel (Core Library, each pack, User Library); `flags` 1027 marks a
library item, 515/3 a folder row. A pack with only 3-9 rows has no content indexed.

## Packs

- The shim rewrites Windows separators in every pack's `Metadata.cfg` at start-up
  (`shim/packmeta.cpp`, `MENUFIX_PACK_META=0` off). Verified by injecting separators back and
  watching one start fix them and the next start write nothing.
- `tools/fix-pack-paths.py [--dry-run] [--quiet] DIR…` does the same by hand for one folder.
- Backups of the pre-fix files were in `/tmp/metadata-backup` (a tmpfs — gone after a
  reboot; regenerate with the script's inverse if needed: `2F 00` → `5C 00`).

## Edit in Max

- On by default since 2026-11-21; `LIVE_MAX_EDIT=0` gives the old note, `LIVE_MAX_EDIT=1`
  is now the same as the default.
- Launch path: `Wires::EditWithPreview` hook (`shim/maxedit.cpp`) → `shim/max-editor` (sets
  `LIVE_MAX_EDITOR`, preloads `libmaxstatfix.so`, passes only absolute `LD_PRELOAD` entries).
- The editor asks for an Ableton account on first use; the owner logged in. Do not feed it
  tokens from outside — licensing is out of scope here.
- Max's own start-up aborts intermittently (Lua allocator inside the gen extension). A retry
  works. Under gdb it does not reproduce, so ptrace changes the timing: do not conclude "no
  bug" from a gdb run.
- Cores land in `/tmp/core.Max.<pid>` (≈1 GB each, `core_pattern=/tmp/core.%e.%p`). Delete
  them after reading; `/tmp` is a tmpfs.

## Traps that cost time here

1. **Never** kill by pattern from a shell whose own command line contains the pattern
   (`pkill -f`, `pgrep -f`): the shell kills itself (exit 143). Select targets with
   `ps -eo pid,args` and filter out your own pid, then kill by pid.
2. Paths with spaces (`Factory Packs/Beat Tools`, `Cycling '74`) break `for` loops and
   `basename`; use Python or quoted variables.
3. Killing the Max editor while Live runs has crashed Live before. Quit Live first.
4. `SIGKILL` on Live leaves the recovery file (see above) and stale `/dev/shm` semaphores.
5. Two Live instances on the same `live-home` corrupt the index; check `ps` first.

## Rebuild and deploy the shim

```sh
cd ~/dev/linux-native-ableton-maxedit && ./shim/build.sh       # libmenufix.so + libpathfix.so + libmaxstatfix.so + swupdate stub
cp shim/libmenufix.so shim/libmaxstatfix.so shim/libpathfix.so shim/max-editor \
   ~/audio/AbletonLive12-native-portable/shim/
```

The build is deterministic from the source tree: the same source gives the same `.so` hash,
so "differs from HEAD" always means "source changed". The repository ships the built `.so`
(the installer copies it, it does not build).

## Remotes, branches, pull requests

- `~/audio/linux-native-ableton`: `feat/wheel-scroll-values` → PR #8.
- `~/dev/linux-native-ableton-maxedit`: `feat/max-edit` → everything (wheel, packs, Max
  editing).
- Submission branches and their state: `PULL-REQUESTS.md`.
