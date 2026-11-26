# Max external loader probe

`jit.linuxprobe` is a logging-only test object, **not** a `jit.gl.lua` stub or
replacement. It must not be installed into a production Max search path.
It tests registration and allocation independently of Lua, Jitter and GL.

The official Max SDK base (Cycling '74, MIT-style license) is required. With
`SDK` pointing to its `c74support` directory, compile the probe and mock host:

```sh
cc -std=c11 -fPIC -shared -Wall -Wextra -Werror \
  -isystem "$SDK/max-includes" -isystem "$SDK/msp-includes" \
  jitlua/probe/jit_lua_probe.c -o /tmp/jit.linuxprobe.mxl_x86_64
cc -std=c11 -Wall -Wextra -Werror -rdynamic \
  -isystem "$SDK/max-includes" -isystem "$SDK/msp-includes" \
  tests/max_probe_host.c -ldl -o /tmp/max_probe_host
/tmp/max_probe_host /tmp/jit.linuxprobe.mxl_x86_64 jit.linuxprobe
```

The mock host checks the ELF loader and expected API calls; it does **not**
prove that Max can load or use the external. A disposable standalone Max run
started but did not load the probe; it was stopped after high CPU use.

## Symbol-origin probe

`jit.luaorigin` reports Lua symbol providers without creating a Lua state or
executing Lua. Its mock-host test passes; real Max loading is not verified.
The earlier VM probe crashed Max and was removed from the working tree. Its
source remains in commit `358e6ae` for analysis; do not build or load it. See
`~/audio/research/JIT-GL-LUA-2026-11-26.md`.

`jit.luanewprobe` obtains `luaL_newstate` and `lua_close` from the already-loaded
MaxLua module and logs before and after each call. It does not open libraries or
run scripts. Mock-host testing covers sequencing only; it may crash real Max.

`jit.lualibprobe` next opens Lua's standard libraries and logs before and after;
it may crash Max and has only been checked against a fake library.

## Experimental loader isolation

`maxlua_deepbind.c` is an **offline-only**, opt-in GNU/Linux loader experiment;
it is not included in the Live build or enabled in Max. With
`LIVE_MAXLUA_DEEPBIND=1`, it adds `RTLD_DEEPBIND` only to explicit `dlopen`
calls for a `MaxLua.so` basename. It cannot affect an already-loaded module or
an internally loaded dependency. `RTLD_DEEPBIND` can change symbol and allocator
interposition; do not deploy it in Live or Max without separate review and
runtime evidence.

`tests/test_maxlua_deepbind.py` builds synthetic libraries and verifies default
behavior, exact matching, opt-in isolation, and the already-loaded limitation.
The real `MaxLua.so` was also inspected in a disposable process without calling
Lua: a previously global stock Lua caused eight `luaopen_*` relocations to point
outside MaxLua; opt-in isolation kept all 147 observed slots in MaxLua. This
shows a possible mechanism, **not** the cause of the two editor crashes.

## Disposable vendor-library check

The optional `tools/maxlua_offline_probe.py` uses the user's local MaxLua and
JitterAPI binaries in a **separate process**, not Max or Live. It requires the
locally built deepbind probe, linker-only `offline_max_host.c` stubs, and a
separately supplied foreign Lua library. The stubs abort if any Max API is
called. The child checks observed Lua relocation slots before executing a fixed
Lua math expression; it disables core dumps and receives a limited environment.
It is not a container or security sandbox. Run the command only as a detached
agent job and inspect its log; a failure must not be retried unchanged.

On the local Max 9.1.5 libraries, the positive isolated run opened Lua's
standard libraries, evaluated `math.floor(4.9)` and closed the state. A
negative test with a forwarding-only shim stopped at the symbol preflight before
creating a state. Neither result proves the Max editor's bindings or a device
fix; Lua module registration, Jitter and OpenGL were not exercised.
