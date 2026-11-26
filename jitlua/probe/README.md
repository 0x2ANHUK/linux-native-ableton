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
`jit_lua_vm_probe.cpp` crashed Max and is retained solely for crash analysis.
Do not build or load it. See `~/audio/research/JIT-GL-LUA-2026-11-26.md`.
