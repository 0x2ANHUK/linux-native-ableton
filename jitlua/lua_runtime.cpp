// Contract: preserve a balanced Lua stack on every success and failure.
#include "lua_runtime.hpp"
#include <stdexcept>

namespace jitlua {
Runtime::Runtime() : state_(luaL_newstate()) {
  if (!state_)
    throw std::bad_alloc();
  luaL_openlibs(state_);
}
Runtime::~Runtime() { lua_close(state_); }

bool Runtime::load(const char *source, std::size_t size, const char *name, std::string &error) {
  const int top = lua_gettop(state_);
  error.clear();
  const int compiled = luaL_loadbuffer(state_, source, size, name);
  const int result = compiled ? compiled : lua_pcall(state_, 0, 0, 0);
  if (result) {
    const char *message = lua_tostring(state_, -1);
    error = message ? message : "Lua raised a non-string error";
  }
  lua_settop(state_, top);
  return result == 0;
}

bool Runtime::call(const char *name, std::string &error) {
  const int top = lua_gettop(state_);
  error.clear();
  lua_getglobal(state_, name);
  if (lua_isnil(state_, -1)) {
    lua_settop(state_, top);
    return true;
  }
  if (!lua_isfunction(state_, -1)) {
    error = std::string("Lua callback is not a function: ") + name;
    lua_settop(state_, top);
    return false;
  }
  const int result = lua_pcall(state_, 0, 0, 0);
  if (result) {
    const char *message = lua_tostring(state_, -1);
    error = message ? message : "Lua raised a non-string error";
  }
  lua_settop(state_, top);
  return result == 0;
}
} // namespace jitlua
