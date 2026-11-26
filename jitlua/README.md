# Jitter Lua runtime (experimental)

A borrowed-state Lua 5.1 execution core and a partial `opengl` module. It does
**not** register a Max object and cannot yet run `jit.gl.lua` devices: the
Jitter and `this` bindings, message routing, embedded-file loading and host GL
lifecycle remain unimplemented. Do not install it into Live.

Run the isolated test from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror jitlua/lua_runtime.cpp tests/lua_runtime_test.cpp \
  $(pkg-config --cflags --libs lua51) -o /tmp/jitlua-runtime-test
/tmp/jitlua-runtime-test
```

The implementation is based on the Lua 5.1 public API. The host must create and initialize the state, keep it alive, invoke this core
on the Max thread and bind its APIs before loading a script. The core must not
call `luaL_openlibs` inside Max: that call crashed twice in the editor. REA
identified Max-owned `lua_create` / `lua_getstate` lifecycle functions, but
their safe use has not yet been verified. The test uses the
system Lua 5.1 library; it does not establish ABI compatibility with MaxLua.

## OpenGL subset

`lua_opengl` supplies `require("opengl")` with `Enable`, `Disable`,
`BlendFunc`, `Begin`, `End`, `Color` and `Vertex`, plus their documented enum
constants. `Color` accepts three or four numbers or a three/four-element table.
These are the operations observed in the installed Bouncy Notes resource, not a
claim to implement the complete Max OpenGL module. The immutable callback table
must outlive the Lua state; the caller must provide a current GL context and run
on the graphics thread, never the audio callback. Missing contexts, invalid
arguments and existing `opengl` registrations are errors.

The local tests use system Lua 5.1. The mock test checks dispatch and errors;
the GLX test creates a separate pbuffer, draws a red quad through all seven
operations and verifies a pixel. Both passed locally on NVIDIA GLX; neither
loads a Max device or proves MaxLua integration. Keep test binaries outside the
repository and serialize compilation with the machine build lock.

Check with `./scripts/check-lua-opengl.sh` (C++17, Lua 5.1, X11, GL and an accessible
`DISPLAY`). It serializes builds, runs both tests and removes generated binaries. Agents
run it as a detached `agent-job`.
