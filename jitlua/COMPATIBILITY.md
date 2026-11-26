# Native Linux Max compatibility — decision card

## Order

1. Diagnose the repeated MaxLua initialization crash offline before any further
   VM-executing tests in Max. Both recorded crashes reached the same address;
   their exact root cause remains unproven (kaizen K-261126-2036).
2. Build a clean-room `jit.gl.lua` implementation in independently tested
   stages. A Lua runtime alone is not an external or a device fix.
3. Reproduce each additional OpenGL issue separately, with a minimal patch,
   version, runtime logs and crash evidence. Do not infer a shared cause.
4. Verify actual device rendering, controls and audio before reporting support.
   Do not redistribute Ableton, Max or unlicensed third-party binaries or code.

## Baseline tool

`python3 tools/jitter_inventory.py --resources /path/to/Max/resources --json`
compares the installed Jitter catalog and object mappings with named files in
externals, extensions and packages. It does not load Max. An object without a
matching filename may be built in, registered dynamically, or use another
mapping; the tool does not claim it is missing. Catalog omissions are not
counted. On the local Max 9.1.5 runtime on 2026-11-26 it found 174 catalog rows
and 18 without named files; none was validated by this scan.

## Sources and limits

- Cycling '74 `max-sdk` and `max-sdk-base`: permissive SDK licenses. GL samples
  document Max/Jitter class wrapping and the `ob3d` lifecycle. Base revision
  `c03a2922a2a8` has a Linux `.mxl_<architecture>` CMake suffix, but its
  library finder must be verified against the actual installed Linux runtime.
- `min-api` is MIT-licensed and wraps the low-level SDK; it does not itself
  establish a Jitter Lua or OpenGL implementation for this host.
- `pi-gen` builds RNBO Raspberry Pi images. It is not a source distribution of
  desktop Max, Jitter or `jit.gl.lua`; do not use it as proof of equivalence.
- REA evidence `ev_c5cede9386dd48ef5fa3b31c929e2af22d76d5c41001ceb92125b9f1ed80fd14`
  identifies code at the recorded MaxLua crash offset, not its causal input.
  Runtime facts and the quarantined probe are recorded in
  `~/audio/research/JIT-GL-LUA-2026-11-26.md`.

## Read-only ABI evidence (2026-11-26)

`python3 tools/maxlua_bindings.py --pid PID --json` reads only mapped Lua
relocation slots and the exported class pointer; it does not attach or invoke
Lua. In the running Live-hosted Max, 147 Lua relocation slots pointed into
MaxLua, `lua_class` was initialized, and `luaO_nilobject_` pointed to MaxLua's
own symbol. Lazy slots may be unresolved. The crashed editor was a different
process, so these observations do not establish its bindings or crash cause.

REA `ev_4335c1c31e1d038221bd041f9b71d1ec859f30efb3a4664ee97f31d88aea23fc`
shows Max's `lua_create` initializes Max's Lua class, then opens libraries and
installs Max's module loader. Use of that lifecycle has not been tested. The
runtime core therefore borrows an initialized state and never opens libraries.

## Offline loader-isolation experiment

A disposable process loaded stock Lua globally before the installed MaxLua.
Eight MaxLua `luaopen_*` relocation slots then pointed to stock Lua. Applying
`RTLD_DEEPBIND` at MaxLua's explicit load kept all 147 observed slots in
MaxLua. No Lua function ran. A separately named, opt-in `dlopen` probe and
synthetic loader tests are in `jitlua/probe/`; they are not installed in Max.
This is a plausible interposition mechanism, not proof of the editor crash.
The probe cannot repair an existing mapping or intercept internal dependency
loads, and `RTLD_DEEPBIND` changes interposition semantics. `JitterAPI.so` has
`MaxLua.so` as a `DT_NEEDED` dependency. Synthetic tests confirm that dependency
loads bypass the wrapper; an explicit earlier protected load preserves local
bindings in that model. The real editor load order and allocator/runtime risks
remain unverified; do not deploy the experiment.

With foreign Lua loaded first, the opt-in isolation probe allowed the local
MaxLua 9.1.5 library to initialize its standard libraries, evaluate a fixed
math expression, and close in a disposable process. Linker-only fake Max
symbols abort if called. A forwarding-only loader was rejected by the
relocation preflight before a Lua state was created. This is not a test of
Max's own process, editor load order, Jitter calls, or the crashing device.
