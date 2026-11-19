#!/usr/bin/env bash
# Copies the platform-neutral content the Push image lacks from a Windows
# Live 12.4.7 (or 11.3.43) Suite install (FINDINGS.md, "Feature parity with the Windows build"):
#   Resources/Core Library          -> <Music>/Ableton/Live 12 Core Library (Live 11 Core Library)
#   Resources/Builtin Lessons       -> AppLive/Resources/Builtin Lessons
#   Live 12: Resources/Builtin/Models/*.dat -> modules/musicai/models (high-quality stem models)
#   Live 11: Resources/Builtin/**   -> AppLive/Resources/Builtin, files the image lacks only
#                                      (Hybrid Reverb impulse responses, Wavetable images, Echo samples)
#   Program/Installation.cfg        -> push3_opt/push3 ({"variant": "Suite"}: the edition;
#                                      without it the Push build runs as "Beta")
# Hard links when both sides are on one filesystem (no extra space), copies otherwise.
# The Core Library must be a real folder: Live does not accept a symlink there.
# <Music> is where Live looks for it: the Music folder of the home Live runs
# with (shim/standalone.cpp): LIVE_DATA_DIR or live-home/ next to push3_opt/
# when isolated (their Music/), otherwise the desktop's Music folder
# (XDG_MUSIC_DIR in ~/.config/user-dirs.dirs, ~/Music without one).
# LIVE_MUSIC_DIR=<folder> sets it directly.
#
# The version: LIVE_MAJOR=12|11 (install.sh sets it), else 11 when the install
# folder has Live 11's launcher (push3_opt/push3/AbletonLive11), else 12.
#
# usage: tools/import-windows-content.sh <Windows app dir containing Resources/>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN="${1:?usage: $0 <Windows Live app dir, e.g. ableton_live_suite_12.4.7_64/app>}"
V=${LIVE_MAJOR:-}
if [ -z "$V" ]; then
    if [ -e "$ROOT/push3_opt/push3/AbletonLive11" ]; then V=11; else V=12; fi
fi
case $V in 12 | 11) ;; *) echo "LIVE_MAJOR must be 12 or 11, not $V" >&2; exit 1 ;; esac
RES="$WIN/Resources"
[ -d "$RES/Core Library" ] || { echo "no Core Library in $RES" >&2; exit 1; }

music_dir() {
    if [ -n "${LIVE_MUSIC_DIR:-}" ]; then echo "$LIVE_MUSIC_DIR"; return; fi
    if [ -n "${LIVE_DATA_DIR:-}" ]; then echo "$LIVE_DATA_DIR/Music"; return; fi
    if [ -d "$ROOT/live-home" ]; then echo "$ROOT/live-home/Music"; return; fi
    local dirs="${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs" line=""
    [ -r "$dirs" ] && line=$(grep -m1 '^XDG_MUSIC_DIR=' "$dirs" || true)
    line=${line#XDG_MUSIC_DIR=}; line=${line//\"/}; line=${line/\$HOME/$HOME}
    if [ -n "$line" ]; then echo "${line%/}"; else echo "$HOME/Music"; fi
}
MUSIC=$(music_dir)
APP="$ROOT/push3_opt/push3"

link_or_copy() { cp -al "$1" "$2" 2>/dev/null || cp -a "$1" "$2"; }
# put_tree SRC DEST: link_or_copy, but DEST is complete or not there at all:
# it is made next to its place and renamed, so an interrupted run leaves no half.
put_tree() {
    rm -rf "$2.part"
    cp -al "$1" "$2.part" 2>/dev/null || { rm -rf "$2.part"; cp -a "$1" "$2.part"; }
    mv "$2.part" "$2"
}

mkdir -p "$MUSIC/Ableton/Live $V Core Library"
link_or_copy "$RES/Core Library/." "$MUSIC/Ableton/Live $V Core Library/"

LESSONS="$APP/products/live/Live/AppLive/Resources/Builtin Lessons"
[ -e "$LESSONS" ] || put_tree "$RES/Builtin Lessons" "$LESSONS"

if [ "$V" = 12 ]; then
    for model in "$RES"/Builtin/Models/*.dat; do
        [ -e "$APP/modules/musicai/models/$(basename "$model")" ] || [ -e "$APP/modules/onnxruntime/models/$(basename "$model")" ] ||
            put_tree "$model" "$APP/modules/musicai/models/$(basename "$model")"
    done
else
    BUILTIN="$APP/products/live/Live/AppLive/Resources/Builtin"
    if [ -d "$RES/Builtin" ]; then
        (cd "$RES/Builtin" && find . -type f) | while IFS= read -r f; do
            [ -e "$BUILTIN/$f" ] && continue
            mkdir -p "$(dirname "$BUILTIN/$f")"
            cp -l "$RES/Builtin/$f" "$BUILTIN/$f" 2>/dev/null || cp "$RES/Builtin/$f" "$BUILTIN/$f"
        done
    fi
fi
[ -e "$APP/Installation.cfg" ] || [ ! -e "$WIN/Program/Installation.cfg" ] || put_tree "$WIN/Program/Installation.cfg" "$APP/Installation.cfg"
echo "done: Core Library in $MUSIC/Ableton/Live $V Core Library; Live scans it on its first start"
