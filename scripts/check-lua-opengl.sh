#!/usr/bin/env bash
# Contract: serialize builds; run mock and private-GLX-pbuffer tests; remove test binaries.
set -euo pipefail

if [ "${1:-}" = "--help" ]; then
  printf '%s\n' "Usage: scripts/check-lua-opengl.sh" \
    "Requires a C++17 compiler, Lua 5.1, GLX, X11 and an accessible DISPLAY."
  exit 0
fi
if [ "$#" -ne 0 ]; then
  printf '%s\n' "JITLUA_CHECK_ARGUMENT: no arguments expected; use --help" >&2
  exit 2
fi
if ! pkg-config --exists lua51 gl x11; then
  printf '%s\n' "JITLUA_CHECK_DEPENDENCY: install Lua 5.1, OpenGL and X11 development packages" >&2
  exit 2
fi
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
state=${XDG_STATE_HOME:-"$HOME/.local/state"}
case "$state" in
/*) ;;
*)
  printf '%s\n' "JITLUA_CHECK_STATE: XDG_STATE_HOME must be absolute" >&2
  exit 2
  ;;
esac
mkdir -p "$state/locks" "$state/agent-jobs"
out=$(mktemp -d "$state/agent-jobs/jitlua-check.XXXXXXXX")
cleanup() {
  rm -f -- "$out/unit" "$out/glx"
  rmdir -- "$out"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
exec 9>>"$state/locks/build.lock"
flock 9
read -r -a cflags <<<"$(pkg-config --cflags lua51 gl x11)"
read -r -a libs <<<"$(pkg-config --libs lua51 gl x11)"
compiler=${CXX:-c++}
cd "$root"
nice -n 10 "$compiler" -std=c++17 -Wall -Wextra -Werror "${cflags[@]}" \
  jitlua/lua_runtime.cpp jitlua/lua_opengl.cpp tests/lua_opengl_test.cpp \
  -o "$out/unit" "${libs[@]}"
nice -n 10 "$out/unit" tests/fixtures/opengl_smoke.lua
printf '%s\n' "LUA_OPENGL_UNIT_PASS"
nice -n 10 "$compiler" -std=c++17 -Wall -Wextra -Werror "${cflags[@]}" \
  jitlua/lua_runtime.cpp jitlua/lua_opengl.cpp tests/lua_opengl_glx_test.cpp \
  -o "$out/glx" "${libs[@]}"
nice -n 10 "$out/glx" tests/fixtures/glx_quad.lua
