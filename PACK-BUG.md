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

## Ruled out

- Missing authorization / web pack list: `FetchDownloadablePackListFromWeb`
  ends in 0-1 ms and only affects the *downloadable* list, not installed packs.
- Licence: Live accepts the packs' licences.
- `Inotify *_rm_watch: Invalid argument` errors in `Indexer.txt` are only ever
  reported for the `Live Recordings/* Temp Project` folders, not for packs.

## Next probe (needs a Live restart)

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
