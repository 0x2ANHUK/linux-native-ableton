# Editing Max for Live devices on native Linux Live 12.4.7

Status: the crash is fixed by `shim/maxedit.cpp`. Editing itself does not work yet. Evidence comes first; each line is marked **verified** (read in the shipped binaries or logs) or **inferred**.

## Root cause (verified, static analysis + Live's log)

Paths are relative to `push3_opt/push3/`. The Max files are in `products/live/Live/AppLive/Resources/Max/`.

1. `Log.txt`, 2026-11-21 08:19:04: `Exception: The Max function "EditWithPreview" returned with error -1: Internal error.`, then a stack trace through `QWidget::event` / mouse event, then "A serious program error has occurred", "Aborting with crashdump".
2. `Live` 0x34d60d0 resolves the Max entry points by name (`EditWithPreview` string at 0x76d3380, `CloseEditor` at 0x76d3390). The wrapper at 0x34d5270 calls the function pointer and passes the result to 0x34d0850. That function returns when the result is 0 and otherwise builds and throws an exception (constructor 0x1ba8150). The throw comes after the wrapper's own try block (tail jump at 0x34d52dd), so it reaches the UI event handler and Live aborts.
3. `resources/support/libpatcher.so` 0x265e0 `Wires::EditWithPreview(plug_DeviceRef*)`: if a Max editor process (global 0x5fd00) exists and the device has a remote peer (+0xa8), it calls `sysprocess_activate` and sends `front` over `remote_object_method_typed`. Otherwise it calls `WiresPrivate::mfl_device_edit`.
4. libpatcher.so 0x2d860 `mfl_device_edit` asks for `sysprocess_getid(WiresUtil::get_editor_path())` and otherwise calls `sysprocess_launch(get_editor_path(), "--mfl")`. When the launch returns 0 it prints `error launching max editor process!` and returns **-1**. That -1 is the value in the log.
5. libpatcher.so 0x387a0 `get_editor_path` builds the path with libMaxPlugLib's `path_mfl_getapppath` (0xabe490, from `g_max_app_pathobject` and `g_mfl_appfilename`). It prints `Couldn't get app path` on failure and then returns NULL.
6. libMaxPlugLib.so 0xad5f80 `sysprocess_launch_withflags` returns 0 for an empty path. Otherwise it calls `fork` and `execv` with no check that the file exists. A failed `execv` returns into the child, so the child keeps running as a copy of Live.
7. Conclusion: the editor is a **separate process** by design (`Max --mfl`, IPC through Max's `remote_object`). **Verified at run time** (2026-11-21, by reading the running Live's memory): the runtime's `s_editor_path` is the folder `…/products/live/Live/AppLive/Resources/Max`, a Mac-style app path. `sysprocess_launch` forked, `execv` failed on the folder, and the child kept running as a second copy of Live (seen in ps, then killed). The call returned -1, which Live makes fatal.

Notable: Live ships a **native Linux Max editor**, `Resources/Max/Max` (ELF x86-64, BuildID 066046b1…). Its strings include `--mfl`, `mfl_device`, `mfl_kernel`, `showauthorizationwindow` and `max: reauthorize finished` (verified). `MaxRuntime` and `MaxRuntimeNOCEF` are the standalone runtimes. Live itself holds `MaxAppPathFrame`, `sDialogTextFallingBackToBundledMaxApp` and `sMenuTextEditInMax` (verified): Live has a "Max application" setting that falls back to the bundled Max.

## The fix (shim/maxedit.cpp)

Once the Max runtime is loaded (first view pass with a Max device), `Wires::EditWithPreview` in libpatcher.so is wrapped. This happens only after its 15-byte prologue matches Max 9.1.5, and only on Live 12.4.7. A non-zero result shows one non-modal note and becomes 0, Max's "no error". Live carries on, and the device and the set stay as they are. `LIVE_MAX_EDIT=0` turns it off.

Before every launch the editor path is checked: when it is not an executable file, nothing is launched (no stray copy of Live) and the note is shown. `LIVE_MAX_EDIT=1` (experimental) turns the runtime's folder path into `Resources/Max/Max`, Live's own Linux Max editor, so Edit in Max should start `Max --mfl` as on other systems. Live's shim is not passed on to it: menufix.cpp removes itself from `LD_PRELOAD`.

Run-time result of the first version (no path check): Live no longer aborted (log: `maxedit: EditWithPreview returned -1, ignored`), but the launch left the stray copy of Live described in point 7.

### Native editor test (2026-11-21, verified)

`Resources/Max/Max --mfl` was started alone, with a scratch HOME, DISPLAY=:1, no Live running and a 25 s timeout. It loaded every extension, built its database, ran `interface_init` and `maxwindow_mayshow`, and started Node for Max/RNBO. It did not crash and showed no authorisation prompt in Max.log (only `authwindow_initclass`). The only complaints were missing fonts (Lato, Arial). Nothing of the user's install was written: all files went to the scratch HOME, which is deleted. Not tested: starting it from Live, the IPC handshake, editing and saving, and authorisation in that mode.

## Options

| Option | What works / blockers | Licence impact | Effort, risk | Verdict |
|---|---|---|---|---|
| (e) Bundled native `Max --mfl` | The editor binary exists and starts on Linux (verified, test above), and Live, libpatcher and Max all have the IPC path. Blocker: the editor path is a folder; `LIVE_MAX_EDIT=1` points it at the binary. Next step is to make `path_mfl_getapppath` resolve to `Resources/Max/Max` (set Live's Max application path, or the two globals) and then watch the launch. Unknown: whether the Linux editor starts, finds its resources and authorises. It shows an authorisation window, so it may need the Ableton account like Max on other systems. | Inferred: Max for Live in Suite is authorised by Live and the editor asks Live. Not verified. Do not try it with the user's set or licence before a test with a scratch HOME is agreed. | Small to medium, low risk once tested in a sandbox | **Recommended next experiment** |
| (a) Windows Max 9 under wine | Max 9 runs under wine as community reports say, with known trouble in the CEF/Chromium browser and docs views, D2D/GPU drawing and ASIO. shibco/ableton-linux installs Windows Live with Max 8/9 under wine. That Max edits devices inside the *wine* Live, not this native Live. | Separate Max licence or the Ableton-account check inside wine. It does not touch native Live's Unlock. | Medium, medium risk | Usable only as an offline editor |
| (b) Bridge: launcher, then wine Max, then reload | Live never asks for an external app here: the edit request ends in the in-process runtime, which wants a `Max --mfl` that speaks Max's `remote_object` IPC. A wine Max does not speak that protocol to a Linux process (inferred). What remains is copy-out editing: open the device's `.amxd` in wine Max, save it, and Live reloads the device (hot-swap or re-drop). | As (a) | Medium. The reload is manual unless a Remote Script does it | Workable fallback, not "Edit in Max" |
| (c) In-process editor in Live | libMaxPlugLib.so contains `PatcherComponent`, `PatcherToolbarComponent`, `InspectorButtonComponent`, `patcherview_edit`, `patcherview_vis` and `jpatcher_get_firstview` (verified). But `mfl_device_edit` on the local path only sends `disableui` to the device view and sets up the remote owner. The editor is always the other process (verified from the code path). Opening a patcher window in Live's process would mean driving Max internals (`patcherview_vis` plus edit mode) that Live does not expect: undo, saving and device reload all run through the editor process's IPC. | None expected | High, high risk (state in Live's Max runtime) | **Not viable**, no prototype made |
| (d) Text editing of `.amxd` | `.amxd` is a binary header (`ampf`, device type, `ptch` chunk) followed by JSON patcher text. The runtime has `loader_getpatcherdictionary_fromamxd` and `max_makeamxd` (verified). Hand edits are fine for attributes and small changes, not for patching. | None | Small | Niche helper |

## Not verified

- Run time: which of path or exec fails (needs Live's stderr). Whether `Resources/Max/Max --mfl` starts on Linux at all. How it authorises.
- That the shim fix behaves at run time: built without warnings, not run in Live.
- Option (a) behaviour of Max 9 under wine on this machine. Since this study a peer agent has copied a Windows Max runtime (Max.exe) into `~/.wine-ableton-beta/drive_c/ProgramData/Ableton/Live 12 Beta/Resources/`, without the user's licence or settings and without Live. Starting it under wine and authorising it are not tested.
