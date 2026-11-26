// Contract: register a partial Lua 5.1 `opengl` module in a host-owned state.
// Call only on the GL context's owning thread; never from an audio callback.
// The immutable API table and opaque context must outlive the Lua state.
// Callbacks must not throw. Unsupported operations are absent. No GL calls here.
#pragma once
#include <lua.hpp>
#include <string>

namespace jitlua {
struct OpenGlApi {
  void *context;
  bool (*has_context)(void *) noexcept;
  void (*enable)(void *, unsigned int) noexcept;
  void (*disable)(void *, unsigned int) noexcept;
  void (*blend_func)(void *, unsigned int, unsigned int) noexcept;
  void (*begin)(void *, unsigned int) noexcept;
  void (*end)(void *) noexcept;
  void (*color)(void *, double, double, double, double) noexcept;
  void (*vertex)(void *, double, double, double, double) noexcept;
};

// Installs package.preload.opengl. Does not initialize Lua or create a context.
bool installOpenGlPreload(lua_State *state, const OpenGlApi *api, std::string &error);
} // namespace jitlua
