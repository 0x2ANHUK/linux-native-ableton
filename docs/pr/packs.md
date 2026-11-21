# Pack folders were empty in the browser (Live 12.4.7)

A pack's folder contents never reached Live's library, so every pack below the first level
listed nothing. This fixes it in the shim, in the installer, and with a script for a single
folder.

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

## How to test

```sh
# with the shim loaded: Places > Packs > Beat Tools > Drums lists the kits.
# without the UI, check Live's index (portable install: under live-home):
DB="$HOME/.local/share/Ableton/Live Database/Live-files-12300.db"
sqlite3 "file:$DB?mode=ro" "select count(*) from files;"
# by hand, for one folder:
tools/fix-pack-paths.py --dry-run "$HOME/Music/Ableton/Factory Packs"
```

## Risk and reverting

`MENUFIX_PACK_META=0` turns the start-up rewrite off; the script never writes a byte when a
file has none to change; and the rewrite only touches UTF-16 separators in a file Live
otherwise owns.

## Files

- `shim/packmeta.cpp` (new), `shim/menufix.cpp`, `shim/build.sh` — the start-up rewrite
- `tools/fix-pack-paths.py` (new), `install.sh` — by hand and at install time
- `PACK-BUG.md`, `README.md` — the evidence and the user-facing line
- `shim/libmenufix.so` — rebuilt (drop it if you prefer to build)
