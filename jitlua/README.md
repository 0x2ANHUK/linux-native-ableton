# Jitter Lua runtime (experimental)

A per-device Lua 5.1 execution core. It does **not** register a Max object and
cannot yet run `jit.gl.lua` device scripts: the Jitter, OpenGL and `this` bindings,
message routing, embedded-file loading and GL lifecycle are not implemented.
Do not install it into Live.

Run the isolated test from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror jitlua/lua_runtime.cpp tests/lua_runtime_test.cpp \
  $(pkg-config --cflags --libs lua51) -o /tmp/jitlua-runtime-test
/tmp/jitlua-runtime-test
```

The implementation is based on the Lua 5.1 public API. The host must invoke it
on the Max thread and bind its APIs before loading a script. The test uses the
system Lua 5.1 library; it does not establish ABI compatibility with MaxLua.
