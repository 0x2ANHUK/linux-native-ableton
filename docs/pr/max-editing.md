# Edit in Max works on Linux (Live 12.4.7)

Right-click a Max for Live device > Edit in Max used to show "serious program error" and close
Live. It now opens the device in Live's **bundled Linux Max editor** (`Resources/Max/Max
--mfl`); edits and File > Save work, and Live takes the saved device back. On by default;
`LIVE_MAX_EDIT=0` restores the previous note.

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
# 1. Load Live with the shim (as install.sh sets it up) and add a Max for Live device.
# 2. Right-click it > Edit in Max. The editor opens the device's patcher.
# 3. Edit it, File > Save, close the patcher: Live takes the device back.
#    Quit Live afterwards; it should be clean.
# 4. LIVE_MAX_EDIT=0 and repeat step 2: a note appears and nothing is started.
# 5. If no window appears, try again (Max's own start-up aborts now and then).
#    MENUFIX_DEBUG=/tmp/shim.log logs the maxedit lines.
```

## Risk and reverting

Everything is inside the shim or the installer; the shipped Live binary is untouched.
`MENUFIX_PACK_META=0` and `LIVE_MAX_EDIT=0` switch the two features off, the pack rewrite only
touches UTF-16 separators in files Live otherwise rewrites itself, and the editor hooks are
gated on the Live build (`livebuild()`), so other Live versions get the stock behaviour.

## Files

- `shim/maxstatfix.c` (new), `shim/maxshmem.c` (new) — the editor's four fixes
- `shim/maxedit.cpp` (new), `shim/max-editor` (new) — the launch path and its preload hygiene
- `shim/maxviews.cpp`, `shim/build.sh`, `install.sh` — wiring and installation
- `MAX_EDIT.md`, `README.md` — the evidence and the user-facing line
- `shim/libmenufix.so`, `shim/libmaxstatfix.so` — rebuilt (drop these if you prefer to build)
