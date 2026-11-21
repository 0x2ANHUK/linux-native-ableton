# Scroll wheel and trackpad change the value under the pointer

Live 12.4.7's Linux build scrolls lists, but a wheel over a knob, fader, number box,
mixer strip or Max for Live control does nothing. This makes it change that value instead,
with the same undo, automation and modulation behaviour as a mouse drag.

## What was done

Three commits, all inside the existing shim (`shim/`), plus one README paragraph:

1. **`LIVE_WHEEL_TRACE=1`** (`wheeltrace.cpp`, read-only diagnostics): logs the three points
   where a scroll can be observed — the native XInput2 event Qt reads, the `QWheelEvent` Qt
   builds from it, and Qt's idea of the widget under the pointer — one line each on a shared
   millisecond clock. This is how the path was established and how a future bug report about
   "my wheel does nothing" can be answered.
2. **Value controls** (`wheelvalues.cpp`): knobs, sliders, number boxes and the mixer volume
   pack react to the wheel. Ctrl or Shift gives fine steps; fractions of a pixel are carried,
   so touchpads scroll smoothly instead of rounding each event to zero.
3. **Max for Live** (`maxwheel.cpp`): `live.dial`, `live.slider` and `live.numbox` react to
   the wheel over the Max device's own window.
4. `install.sh`/README text, and `shim/libmenufix.so` rebuilt from 1-3 (repo convention: the
   built shim is tracked; drop that commit if you prefer to rebuild).

## What it covers

| Surface | Covered | How |
|---|---|---|
| Device knobs / "Poti" | yes | wheel → vertical drag through the class's own `OnDrag` |
| Device and mixer faders, sliders | yes | same |
| Number boxes / `EditField` | yes | same |
| Mixer volume pack | yes | synthetic press, move, release |
| Clip Gain (modulated vertical slider) | yes | synthetic press, move, release (its `OnDrag` does nothing outside a press) |
| Rack macros, sends, pan, tempo | yes (verified) | same paths as above |
| Max for Live `live.dial` / `live.slider` / `live.numbox` | yes | wheel → press/move/release into the Max window via `XSendEvent` |
| Lists, scrollers, browser | unchanged | Live already handled these |
| Text fields, buttons, toggles, menus, dialogs | **deliberately not** | a press there would click/commit them |
| EQ curve displays, other Max objects | no | not implemented; see Not verified |
| Live 11 and builds other than 12.4.7 | **no** | every vtable is gated on the build (`livebuild()`); other builds behave exactly as before |

Undo, automation and modulation come for free: the value is changed exactly as a mouse drag
would change it, not by a bespoke "set value" path.

## What it enables

- Mixing and sound design on Linux without reaching for a mouse drag for every parameter:
  scroll the control under the pointer, Ctrl/Shift for fine steps.
- A diagnostic switch (`LIVE_WHEEL_TRACE=1` with `MENUFIX_DEBUG=<file>`) that turns any wheel
  complaint into a three-line log with the native event, the Qt event and the widget Qt thinks
  is under the pointer.
- A pattern for the rest of the shim: the same shape (RTTI lookup → vtable slot patch → keep
  the original behaviour for every class not in an explicit list) can carry any future
  "Live's Linux build never wired this input" fix.

## How it was done

Live's `TQtView::wheelEvent` already forwards every wheel event to
`AViewEventHandler::DispatchMouseWheelEvent`, which hit-tests the view tree and asks each view
whether it wants the wheel — view vtable slot 102 `WantsWheel(point, delta, axis)` and slot 104
`OnWheel(point, delta, axis)` — but only for views with one flag bit set in their flags word
(offset `0x40`, bit 9). Only `AScrollerView` implements those slots, which is why lists
scrolled and knobs did not.

For Live's own controls:

1. Find the value-control vtables by RTTI name (Itanium RTTI: the vtable's slot -1 points at
   the `type_info`, whose second word is the mangled name). An explicit allow list
   (`Slider`, `Poti`, `EditField`, `VolumePack`, `Fader`) with an explicit deny list
   (`Dialog`, `Button`, `Check`, `Time`, `Title`, …) decides membership.
2. Patch slots 102/104 to claim the wheel, and set bit 9 in the flags word the first time the
   pointer is over the view (always before the first scroll).
3. Turn the wheel delta into a vertical drag: through the class's own `OnDrag` (slot 96) where
   it has a usable one, else as a synthetic press, move and release on the view under the
   pointer. While a synthetic drag runs, `pointerwarp.cpp` reports a virtual pointer position
   so the real cursor never moves.
   About 3 px of drag per notch, a fifth of that with Ctrl or Shift.

For Max for Live, the Max runtime draws in its own X window and its JUCE box only reacts if the
object has a `mousewheel` method — which `live.dial`, `live.slider` and `live.numbox` do not.
`BoxComponent`'s vtable is patched once the runtime is loaded (from `maxviews.cpp`, on the first
placed Max device), the box object is read at `+0x188` of the component, and the wheel becomes
a press, a move past Max's 3 px drag threshold and a release, sent to that window with
`XSendEvent`. Buttons, toggles and menus are left alone.

Everything is gated on the Live build (`livebuild()`): the addresses and slot numbers are
Live 12.4.7's, and any other build gets the stock behaviour.

`LIVE_WHEEL_VALUES=0` turns the whole feature off. It is on by default.

## Verified

On Live 12.4.7, Hyprland/Xwayland, driving wheel events with `ydotool`:

- tempo, pan, sends, track fader, clip Gain
- Reverb device knobs, Audio Effect Rack macros
- Max for Live LFO: Rate, Depth and Offset

## Not verified

- Touchpad *pixel* deltas (only notched wheel events were driven; the carry logic is written
  for them but was not exercised with a real touchpad).
- EQ curve displays, rack chain volume sliders, other Max objects (`live.text`, `live.menu`,
  third-party or gen~ UIs).
- Live 11 (intentionally untouched) and any Live 12 build other than 12.4.7.
- Multiple Max devices open at once, and the editor window of a Max device.

## How to test

```sh
# 1. Load Live with the shim (as install.sh sets it up), open a set with a Reverb and a Max LFO.
# 2. Wheel over a knob: the value changes; Ctrl or Shift gives fine steps; undo works.
# 3. LIVE_WHEEL_VALUES=0 and repeat: the wheel does nothing, as before.
# 4. If something does not react:
LIVE_WHEEL_TRACE=1 MENUFIX_DEBUG=/tmp/wheel.log <live>   # then read /tmp/wheel.log
```

## Files

- `shim/wheelvalues.cpp` (new) — value controls, wheel → drag
- `shim/maxwheel.cpp` (new) — Max for Live dials/sliders/number boxes
- `shim/wheeltrace.cpp` (new) — `LIVE_WHEEL_TRACE` diagnostics
- `shim/menufix.cpp` — install the two hooks at start-up
- `shim/maxviews.cpp` — install the Max wheel hook once the runtime is up
- `shim/pointerwarp.cpp` — virtual pointer while a synthetic wheel drag runs
- `shim/build.sh`, `shim/libmenufix.so`, `README.md`

## Risk and reverting

All of it is inside the shim and off-switchable (`LIVE_WHEEL_VALUES=0`); the stock Live binary
is untouched. The patches only touch vtable slots of classes named in the allow list, and every
one of them is gated on the Live build, so other builds and other Live versions keep the
current behaviour.
