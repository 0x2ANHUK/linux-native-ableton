# Milestones

What is done, on which evidence, and where the proof lives. Dates are the owner's machine
(marvin, Live 12.4.7 portable). Each line points at the file that carries the detail; this
page only tracks state.

## 2026-11-21

### Scroll wheel and trackpad change the value under the pointer — done, in review

- Wheel over knobs, faders, number boxes, mixer strip, rack macros, sends, pan, tempo, and
  Max for Live `live.dial` / `live.slider` / `live.numbox` changes that value as a drag
  would, so undo, automation and modulation follow.
- Ctrl or Shift = fine steps; fractions carried for touchpads. `LIVE_WHEEL_VALUES=0` off.
- Only Live 12.4.7 (gated on `livebuild()`); Live 11 untouched.
- Diagnostic: `LIVE_WHEEL_TRACE=1` + `MENUFIX_DEBUG=<file>`.
- Verified with `ydotool` wheel events: tempo, pan, sends, track fader, clip Gain, Reverb
  knobs, Audio Effect Rack macros, Max LFO Rate/Depth/Offset.
- Code: `shim/wheeltrace.cpp`, `shim/wheelvalues.cpp`, `shim/maxwheel.cpp`, `shim/pointerwarp.cpp`.
- Submit: branch `pr/scroll-wheel` (= commit `a55dc93`), PR #8 open on
  `velkajelka/linux-native-ableton`; branch layout in `PULL-REQUESTS.md`.

### Pack contents show in the browser — done, fixed at start-up

- Cause: a pack's `Ableton Folder Info/Metadata.cfg` stores its folder and file keys as
  UTF-16 strings with Windows backslashes (1207 in Beat Tools) while the Core Library uses
  `/`. Live resolves a pack folder's contents from that map, so every key below the first
  level failed to match and packs looked empty.
- Fix: rewrite `5C 00` → `2F 00` in that file. Live's index went from 9596 to 11828 files;
  per pack: Bomblastic 4 → 901, Beat Tools 9 → 631, Drum Essentials 5 → 388, Breakbeats
  5 → 276, Microtuner 3 → 42, Generators by Iftah 3 → 21.
- Shipped three ways: `shim/packmeta.cpp` (automatic, every start, `MENUFIX_PACK_META=0`
  off), `tools/fix-pack-paths.py` (by hand, idempotent, `--dry-run`), and an `install.sh`
  step. Details and the isolated experiments: `PACK-BUG.md`. Submit: branch `pr/packs`.

### Edit in Max edits and saves on Linux — done, on by default

- Right-click a Max for Live device, Edit in Max, and the device opens in Live's bundled
  Linux Max editor; edits and save work and Live takes the saved device back. Verified from
  Live, saved by the owner.
- Four editor-side fixes, all found at run time: btrfs inode 256 in folder identity, the
  `liveowner` class that nothing registers, shared memory (`sysshmem_*` stubs) for the
  device audio, and zenity's empty `--file-filter` on Save (`shim/maxstatfix.c`,
  `shim/maxshmem.c`, `shim/max-editor`).
- The editor asks for an Ableton account the first time; the owner logged in. Nothing is
  fed a token from outside (licensing stays out of scope).
- Max's own start-up aborts now and then (Lua allocator in the gen extension, `PACK-BUG.md`
  has the backtrace); a retry works, and gdb attachment makes it disappear, so it is a race
  in Max, not in the launch.
- `LIVE_MAX_EDIT=0` restores the old note. Details: `MAX_EDIT.md`. Submit: branch
  `pr/max-editing`.

### Tooling that made this possible — repaired

- REA's Ghidra provider was unusable because the MCP server started with JDK 17;
  `~/.local/bin/rea-mcp` pins JDK 25 and raises Ghidra's start-up deadline. `open_binary`
  then analyzes ELF/PE targets again (255 tools).
- Live restart recipe with an index-scan capture: see `HANDOFF.md`.

## Open threads

- Packs the user downloads later: covered by `shim/packmeta.cpp` at the next start, but the
  download-time "Missing authorization" step (`FetchDownloadablePackListFromWeb`, 0-1 ms) is
  not understood. Notes in `PACK-BUG.md`.
- Edit in Max on audio and instrument devices, several editors at once, and other zenity
  versions: unverified.
- Touchpad *pixel* deltas for the wheel: written for, not exercised.
