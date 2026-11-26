// Standalone Lua 5.1 smoke and stack-discipline checks; no Max or GL claims.
#include "../jitlua/lua_runtime.hpp"
#include <cassert>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
static int open_test_module(lua_State *state) {
  const char *name = lua_tostring(state, 1);
  if (!name || std::strcmp(name, "jit") != 0)
    return luaL_error(state, "wrong module name");
  lua_newtable(state);
  lua_pushnumber(state, 42);
  lua_setfield(state, -2, "answer");
  return 1;
}
static int fail_module(lua_State *state) { return luaL_error(state, "module failed"); }

int main() {
  std::unique_ptr<lua_State, decltype(&lua_close)> owned(luaL_newstate(), &lua_close);
  assert(owned);
  luaL_openlibs(owned.get()); // System Lua only: never use this initialization in Max.
  try {
    jitlua::Runtime invalid(nullptr);
    assert(false && "null state must be rejected");
  } catch (const std::invalid_argument &) {
  }
  jitlua::Runtime vm(owned.get());
  std::string error;
  assert(vm.registerModule("jit", open_test_module, error));
  assert(!vm.registerModule("broken", fail_module, error));
  assert(error.find("module failed") != std::string::npos);
  const char *binding = "assert(jit.answer == 42)";
  assert(vm.load(binding, std::strlen(binding), "binding", error));
  const char *good = "count=0; function draw() count=count+1 end";
  assert(vm.load(good, std::strlen(good), "good", error));
  assert(vm.call("draw", {}, error));
  assert(vm.call("missing", {}, error));
  lua_getglobal(vm.state(), "count");
  assert(lua_tointeger(vm.state(), -1) == 1);
  lua_pop(vm.state(), 1);
  const char *bad = "function broken(";
  assert(!vm.load(bad, std::strlen(bad), "bad", error) && !error.empty());
  const char *fail = "function fail() error('expected failure') end; notfn=42";
  assert(vm.load(fail, std::strlen(fail), "fail", error));
  assert(!vm.call("fail", {}, error) && error.find("expected failure") != std::string::npos);
  assert(!vm.call("notfn", {}, error) && error.find("not a function") != std::string::npos);
  const char *list = "function list(a,b) assert(a == 2.5 and b == 'hello') end";
  assert(vm.load(list, std::strlen(list), "list", error));
  assert(vm.call("list", {2.5, std::string("hello")}, error));
  assert(!vm.call("list", std::vector<jitlua::Argument>(257, 1.0), error));
  assert(error == "Too many Lua callback arguments");
  assert(lua_gettop(vm.state()) == 0);
}
