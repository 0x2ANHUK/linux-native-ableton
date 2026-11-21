# Live packs: browser shows nothing inside a pack folder

Status: root cause **not** fixed. First hard evidence below (2026-11-21,
pi session, Live 12.4.7 portable at `~/audio/AbletonLive12-native-portable`,
index DB `live-home/.local/share/Ableton/Live Database/Live-files-12300.db`).

## What the bug looks like

- Places > Packs: each pack lists its top-level folders (Drums, Samples, ...).
- Opening any of those folders shows nothing: no files, no subfolders.
- User folders and the Core Library list normally.
- Beat Tools kits do not appear under Library > Drums either.

## Evidence: the pack contents are not in Live's index at all

Read-only queries against `Live-files-12300.db` (`sqlite3 "file:$DB?mode=ro"`):

| Query | Result |
|---|---|
| `select count(*) from files` | 9596 |
| rows under Core Library (`ancestor_id=9`) | **8295** |
| rows under Factory Packs (`ancestor_id=9528`) | **35** |
| rows per `place_id`: `9` (Core Library) / `9529` (Beat Tools) | 8295 / **9** |
| children of `Drums` (`parent_id=9546`) | **0** |

The 35 rows under Factory Packs are the six pack roots plus their subfolder
stubs. Beat Tools itself has 8 subfolder rows (`Ableton Folder Info`, `Drums`,
`Effect Racks`, `Lessons`, `MIDI Clips`, `Samples`, `Sets`, `Sounds`); each of
those has **no children**. On disk `Beat Tools/Drums` holds 30+ `.adg` files.

So the browser shows nothing because the indexer never descended past the
first level inside a pack. It is not a rendering or filtering problem, and it
is not a file-permission problem: every pack path is `drwxr-xr-x marvin` on
btrfs (`/dev/nvme0n1p2`, `/home`), and `getfattr -d` is empty.

`places` lists the six packs as places (`file_id` 9529-9534, `folder_kind=0`,
`level=0`), same shape as Core Library (`file_id=9`). `Library.cfg` lists all
six with `LibrarySliceInfo`, and the indexer log confirms it was configured
with them:

```
Configure: FactoryPacks: '.../Factory Packs/Beat Tools' [Beat Tools], ... '.../Live 12 Core Library' [Core Library]
```

Every Live start runs `Starting scan, Reason: Live, All Folders?: true`, and
the log still ends with 0 pack content, so the skip is repeatable per start,
not a one-off failed scan.

Scratchpad inodes are clean: no `ino 256` anywhere under Factory Packs, so the
Max btrfs-inode bug (`maxstatfix.c`) is not this.

## Isolated: the pack/place layer stops the scan, not the files

Three fixtures, each indexed with one Live restart (scan runs at every start).

| Fixture | Where | Result in the index DB |
|---|---|---|
| 5 `.adg` copies of `Beat Tools/Drums`, one nested folder | `User Library/CopyTest/Drums` | **indexed fully**, place 10, files at ids 9742-9747 |
| same two `.adg` + `Ableton Folder Info` copy | `User Library/CopyInfo/Drums` | **indexed fully**, place 10, `Drums` gets both files (9763, 9764) |
| same two `.adg`, no `Ableton Folder Info` | `Factory Packs/TestNoInfo/Drums` | **not indexed at all** |
| same two `.adg` + `Ableton Folder Info` copy | `Factory Packs/TestWithInfo/Drums` | becomes **its own place** (9749); subfolders created (`Drums` 9752, `Ableton Folder Info` 9751) and **no children below them** |

So the files, names, depth and the `Ableton Folder Info` metadata are all fine:
the identical bytes index normally inside the User Library. What changes the
outcome is the *place*: a folder under Factory Packs that carries
`Ableton Folder Info/Properties.cfg` is promoted to a pack place, and a pack
place is only ever enumerated one level deep.

The runtime agrees. An `strace -f` of Live's startup (filtered to
`openat,getdents64,statx,newfstatat`) shows Live stat-ing and opening only:

```
.../Factory Packs/Beat Tools                      (41 stat/branch hits)
.../Factory Packs/Beat Tools/Ableton Folder Info/Properties.cfg  (18)
```

and **no `getdents64` on any pack path** — the pack directories are never
listed, so the indexer cannot have seen their contents. 799 trace lines
mention Factory Packs; 732 are `newfstatat` and 67 `openat`, the two above
included.

Restart recipe used (portable install, Live data in `live-home`, so `HOME` is
that folder):

```sh
P=~/audio/AbletonLive12-native-portable
cd $P/push3_opt/push3 && HOME=$P/live-home \
  DISPLAY=:1 WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000 \
  LD_PRELOAD=$P/shim/libavx512emu.so:$P/shim/libmenufix.so \
  LD_LIBRARY_PATH=$P/push3_opt/push3/lib:$P/compat_lib \
  ./AbletonLive12
```

`Indexer.txt` then logs `Indexer: start process` and
`Starting scan, Reason: Live, All Folders?: true`. Delete
`live-home/.config/Ableton/Live 12.4.7/CrashRecoveryInfo.cfg` before a restart
or Live stops at the recovery dialog and never starts the indexer.

## Ruled out

- Missing authorization / web pack list: `FetchDownloadablePackListFromWeb`
  ends in 0-1 ms and only affects the *downloadable* list, not installed packs.
- Licence: Live accepts the packs' licences.
- `Inotify *_rm_watch: Invalid argument` errors in `Indexer.txt` are only ever
  reported for the `Live Recordings/* Temp Project` folders, not for packs.

## Root cause (verified): pack metadata uses Windows separators

A pack's `Ableton Folder Info/Metadata.cfg` stores its folder and file entries as
UTF-16 strings with **backslashes**:

```
Ableton Folder Info\Previews\Drums
Ableton Folder Info\Previews\MIDI Clips\110 - 124 bpm
```

The Core Library's `Metadata.cfg` uses forward slashes, and so does everything
Live writes itself:

| File | strings with `\` | strings with `/` |
|---|---|---|
| `Factory Packs/Beat Tools/Ableton Folder Info/Metadata.cfg` | 1207 | 0 |
| `Live 12 Core Library/Ableton Folder Info/Metadata.cfg` | 0 | 8400 |

Live resolves what a pack folder contains from that map. On Linux a key with a
backslash never matches the path being asked for, so nothing is listed below the
first level (keys without a separator, i.e. the pack's top-level folder names,
still match, which is exactly the observed behaviour). It also explains why the
pack paths are never passed to `getdents64`.

### Fix, measured

Replacing `5C 00` with `2F 00` in that file (UTF-16LE backslash to slash) makes
Live index the pack normally. Rows per pack place, before and after
(`select p.name, count(f.file_id) from places p left join files f on
f.place_id = p.file_id`):

| Pack | before | after |
|---|---|---|
| Bomblastic | 4 | **901** |
| Beat Tools | 9 | **631** |
| Drum Essentials | 5 | **388** |
| Breakbeats by KutMasta Kurt | 5 | **276** |
| Microtuner | 3 | **42** |
| Generators by Iftah | 3 | **21** |
| Core Library | 8295 | 8295 (untouched) |

The whole index went from 9596 to 11827 files, and `Beat Tools/Drums` now has
its 30+ `.adg` kits as rows, which is what the browser lists.

Shipped in the repo: `tools/fix-pack-paths.py` (idempotent, `--dry-run`) and an
`install.sh` step that runs it over the portable `live-home` packs and
`~/Music/Ableton/Factory Packs`. A pack installed later by Live needs the script
run again; a shim hook on `open()` of `Metadata.cfg` would cover that too.

## Superseded: static probe notes

REA with the Ghidra provider is verified working on this machine (JDK 25 pinned
by `~/.local/bin/rea-mcp`; `open_binary` returned the `Ableton Index` ELF). The
static route is no longer needed for this bug.

## Older probe (needs a Live restart)

Strace the indexer during a real startup scan, which is the only place a full
scan happens:

```sh
IDX=$(pgrep -f 'Ableton Index /run/user' | head -1)
timeout 120 strace -f -p "$IDX" -e trace=openat,getdents64,statx,newfstatat \
  -o /tmp/idx.strace &
# start Live, open a set, then quit
rg 'Factory Packs' /tmp/idx.strace
```

Question to answer from the trace: does the indexer `openat`/`getdents64` the
pack subfolders at all? If it never opens them, the skip is a decision made
before the filesystem (place metadata / pack registry). If it opens them and
then drops the entries, the filter is per entry (file type, `flags`, or name).

`ptrace_scope` is 0 on this machine, so attaching works. Attaching to an idle
indexer only and touching pack files produces no scan (`/tmp/idx.strace`
stayed empty), so a restart is required.
