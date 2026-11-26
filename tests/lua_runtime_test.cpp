// Standalone Lua 5.1 smoke and stack-discipline checks; no Max or GL claims.
#include "../jitlua/lua_runtime.hpp"
#include <cassert>
#include <cstring>
#include <string>
int main() {
  jitlua::Runtime vm;
  std::string error;
  const char *good = "count=0; function draw() count=count+1 end";
  assert(vm.load(good, std::strlen(good), "good", error));
  assert(vm.call("draw", error));
  assert(vm.call("missing", error));
  lua_getglobal(vm.state(), "count");
  assert(lua_tointeger(vm.state(), -1) == 1);
  lua_pop(vm.state(), 1);
  const char *bad = "function broken(";
  assert(!vm.load(bad, std::strlen(bad), "bad", error) && !error.empty());
  const char *fail = "function fail() error('expected failure') end; notfn=42";
  assert(vm.load(fail, std::strlen(fail), "fail", error));
  assert(!vm.call("fail", error) && error.find("expected failure") != std::string::npos);
  assert(!vm.call("notfn", error) && error.find("not a function") != std::string::npos);
  assert(lua_gettop(vm.state()) == 0);
}
