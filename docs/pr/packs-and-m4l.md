# Pack contents in the browser, and Edit in Max on Linux

Two fixes for Live 12.4.7's Linux build, both inside the shim plus the installer. They are
in one branch because they share one rebuilt `shim/libmenufix.so`; if #8 (scroll wheel) lands
first, rebase and rebuild that file.

## 1. Pack folders were empty in the browser — fixed

**Symptom.** Places > Packs lists each pack's top-level folders, and opening any of them shows
nothing: no files, no subfolders. Beat Tools' drum kits never appear under Library > Drums
either. User folders and the Core Library are fine.

**Cause (verified).** A pack's `Ableton Folder Info/Metadata.cfg` stores its folder and file
keys as UTF-16 strings with **Windows separators**:

```
Ableton Folder Info\Previews\Drums
Ableton Folder Info\Previews\MIDI Clips\110 - 124 bpm
```

The Core Library's file, and everything Live writes itself, uses `/`:

| file | strings with `\` | strings with `/` |
|---|---|---|
| `Factory Packs/Beat Tools/Ableton Folder Info/Metadata.cfg` | 1207 | 0 |
| `Live 12 Core Library/Ableton Folder Info/Metadata.cfg` | 0 | 8400 |

Live resolves what a pack folder contains **from that map**, not from the directory: a
pack's paths are never passed to `getdents64` (an `strace` of a start-up scan shows only
`newfstatat`/`openat` of the pack root and `Ableton Folder Info/Properties.cfg`). On Linux a
backslashed key never matches, so everything below the first level disappears — while keys
without a separator (the top-level folder names) still match.

Isolated with four fixtures in one restart each:

| fixture | where | result |
|---|---|---|
| 5 `.adg` copies of `Beat Tools/Drums` | `User Library/CopyTest/Drums` | indexed fully |
| same + `Ableton Folder Info` copy | `User Library/CopyInfo/Drums` | indexed fully |
| same, no folder info | `Factory Packs/TestNoInfo/Drums` | not indexed at all |
| same + folder info | `Factory Packs/TestWithInfo/Drums` | becomes its own place, only its first level enumerated |

So the files, names and depth are all fine; the place/pack layer is what stops.

**Fix.** Rewrite `5C 00` → `2F 00` inside that file (one byte for one byte, so lengths and
offsets stay valid). Three ways, all idempotent:

- `shim/packmeta.cpp` — every start-up, for every pack under `$HOME/Music/Ableton/Factory Packs`,
  before Live starts `Ableton Index`, so a pack Live installs itself is fixed on the next
  start. `MENUFIX_PACK_META=0` turns it off. Atomic replace, nothing written when the file
  has no such byte.
- `tools/fix-pack-paths.py [--dry-run] [--quiet] DIR…` — by hand, for one folder or a parent.
- an `install.sh` step that runs the script.

**Measured.** Live's index (`Live-files-12300.db`, the table the browser reads) went from
9596 to 11828 rows; per pack place:

| pack | before | after |
|---|---|---|
| Bomblastic | 4 | 901 |
| Beat Tools | 9 | 631 |
| Drum Essentials | 5 | 388 |
| Breakbeats by KutMasta Kurt | 5 | 276 |
| Microtuner | 3 | 42 |
| Generators by Iftah | 3 | 21 |
| Core Library | 8295 | 8295 (untouched) |

`Beat Tools/Drums` now holds its 30+ `.adg` kits. The start-up rewrite was verified by
injecting 4019 separators back into Beat Tools' file: one start logged
`packmeta: …: 4019 separators`, the pack stayed indexed, and the next start wrote nothing.

## 2. Edit in Max works on Linux — enabled

Right-click a Max for Live device > Edit in Max opens the device in Live's **bundled Linux
Max editor** (`Resources/Max/Max --mfl`), edits and File > Save work, and Live loads the saved
device back. On by default; `LIVE_MAX_EDIT=0` restores the previous note.

The crash this replaces was "serious program error" followed by Live closing. Four fixes were
needed, each found at run time against a live editor (`shim/maxstatfix.c`, `shim/maxshmem.c`,
`shim/max-editor`):

1. **btrfs inodes** — Max identifies folders by `st_ino` alone and every btrfs subvolume root
   has inode 256, so the first database update never ends. Directories with inode 256 get the
   device number mixed in.
2. **`liveowner` class** — Live asks the editor for `object_new_typed(nobox, liveowner)` about
   four times a second; the function that registers it (`MaxAudioAPI+0x57bd0`) is never called
   in this build. The fix calls it once `MaxAudioAPI.so` is loaded, after its bytes are checked.
3. **Shared memory** — once the editor holds the device, Live's audio thread crashed in
   `audiobuffer_client_setschedtime` because `sysshmem_alloc/open` are stubs in both
   `libMaxPlugLib.so` and the editor. They are replaced with POSIX `shm_open`/`mmap` versions
   when the bytes are the known stub.
4. **zenity file filter** — Max's Save runs `zenity --file-filter= *.amxd`; zenity 4 shows no
   window for a filter with an empty name, so Save (and Live with it) hung. The blanks are
   dropped in `execvp`.

Also in the branch: `shim/max-editor` passes only absolute `LD_PRELOAD` entries on, so the
launcher's working-directory-relative paths do not make `ld.so` warn inside Max.

**Verified from Live, 2026-11-21:** open a device, edit it, save, close the patcher, Live takes
the device back; quitting Live afterwards is clean. The editor asks for an Ableton account (the
owner's own) on first use — the project does not feed it tokens.

**Known and recorded:** Max's own start-up aborts now and then (SIGABRT in Lua's allocator
while the gen extension loads, with a non-heap pointer, so the Lua heap is corrupted before
that point). A retry always produced a working editor, and a gdb-attached run does not abort at
all, so it is a race in Max's start-up rather than something the launch does wrong.

## How to test

```sh
# packs: with the shim loaded, open Places > Packs > Beat Tools > Drums: the kits list.
#        or check the index, without the UI:
DB=~/.local/share/Ableton/Live\ Database/Live-files-12300.db   # portable: under live-home
sqlite3 "file:$DB?mode=ro" "select count(*) from files;"
#        by hand, for one folder:
tools/fix-pack-paths.py --dry-run "~/Music/Ableton/Factory Packs"

# Edit in Max: right-click a Max device > Edit in Max. The editor opens the patcher.
#              Edit, File > Save, close the patcher, and Live takes the device back.
#              LIVE_MAX_EDIT=0 for the old note.
#              If no window appears, try again (see the start-up abort above).
#              MENUFIX_DEBUG=/tmp/shim.log logs maxedit/packmeta lines.
```

## Risk and reverting

Everything is inside the shim or the installer; the shipped Live binary is untouched.
`MENUFIX_PACK_META=0` and `LIVE_MAX_EDIT=0` switch the two features off, the pack rewrite only
touches UTF-16 separators in files Live otherwise rewrites itself, and the editor hooks are
gated on the Live build (`livebuild()`), so other Live versions get the stock behaviour.

## Files

- `shim/packmeta.cpp` (new) — pack metadata separators at start-up
- `shim/maxstatfix.c`, `shim/maxshmem.c` — the editor's four fixes
- `shim/maxedit.cpp`, `shim/max-editor` — the launch path and its preload hygiene
- `shim/menufix.cpp`, `shim/maxviews.cpp`, `shim/build.sh` — wiring
- `tools/fix-pack-paths.py` (new), `install.sh` — the installer-side pack fix
- `PACK-BUG.md`, `MAX_EDIT.md`, `MILESTONES.md`, `HANDOFF.md`, `README.md` — evidence and state
- `shim/libmenufix.so`, `shim/libmaxstatfix.so` — rebuilt (drop these if you prefer to build)
