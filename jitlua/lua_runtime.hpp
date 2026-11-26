// Contract: one Lua 5.1 VM per device; all methods run on the owning Max thread.
// Bind Jitter, OpenGL and `this` before loading a device script. No audio-thread use.
#pragma once
#include <cstddef>
#include <lua.hpp>
#include <string>

namespace jitlua {
class Runtime {
public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  lua_State *state() const noexcept { return state_; }
  bool load(const char *source, std::size_t size, const char *name, std::string &error);
  bool call(const char *name, std::string &error);

private:
  lua_State *state_;
};
} // namespace jitlua
