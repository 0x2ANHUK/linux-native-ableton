// Small compatibility-profile OpenGL binding for observed device calls.
// I/O: Lua module functions forward to an injected, context-checked GL API.
// Invariant: no OpenGL call is made when the host reports no current context.
#include "lua_opengl.hpp"
#include <GL/gl.h>
#include <cmath>
#include <limits>

namespace jitlua {
namespace {
const OpenGlApi *checkedApi(lua_State *state) {
  auto *api = static_cast<const OpenGlApi *>(lua_touserdata(state, lua_upvalueindex(1)));
  if (!api || !api->has_context(api->context)) {
    luaL_error(state, "JITLUA_GL_NO_CONTEXT: OpenGL context is not current");
    return nullptr;
  }
  return api;
}

double number(lua_State *state, int index) {
  if (lua_type(state, index) != LUA_TNUMBER) {
    luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: expected a number");
    return 0;
  }
  const double value = lua_tonumber(state, index);
  if (!std::isfinite(value)) {
    luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: expected a finite number");
    return 0;
  }
  return value;
}

unsigned int glEnum(lua_State *state, int index) {
  const double value = number(state, index);
  if (value < 0 || value > std::numeric_limits<unsigned int>::max() || std::floor(value) != value) {
    luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: expected an unsigned integer enum");
    return 0;
  }
  return static_cast<unsigned int>(value);
}

int enable(lua_State *state) {
  if (lua_gettop(state) != 1)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: enable expects 1 argument(s)");
  const auto *api = checkedApi(state);
  api->enable(api->context, glEnum(state, 1));
  return 0;
}
int disable(lua_State *state) {
  if (lua_gettop(state) != 1)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: disable expects 1 argument(s)");
  const auto *api = checkedApi(state);
  api->disable(api->context, glEnum(state, 1));
  return 0;
}
int blendFunc(lua_State *state) {
  if (lua_gettop(state) != 2)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: blendFunc expects 2 argument(s)");
  const auto *api = checkedApi(state);
  const auto source = glEnum(state, 1);
  const auto destination = glEnum(state, 2);
  api->blend_func(api->context, source, destination);
  return 0;
}
int begin(lua_State *state) {
  if (lua_gettop(state) != 1)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: begin expects 1 argument(s)");
  const auto *api = checkedApi(state);
  api->begin(api->context, glEnum(state, 1));
  return 0;
}
int end(lua_State *state) {
  if (lua_gettop(state) != 0)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: end expects 0 argument(s)");
  const auto *api = checkedApi(state);
  api->end(api->context);
  return 0;
}
int color(lua_State *state) {
  const auto *api = checkedApi(state);
  const int count = lua_gettop(state);
  double values[4] = {0.0, 0.0, 0.0, 1.0};
  if (count == 1 && lua_istable(state, 1)) {
    const size_t length = lua_objlen(state, 1);
    if (length != 3 && length != 4)
      return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: Color table expects three or four numbers");
    for (size_t i = 0; i < length; ++i) {
      lua_rawgeti(state, 1, static_cast<int>(i + 1));
      values[i] = number(state, -1);
      lua_pop(state, 1);
    }
  } else {
    if (count != 3 && count != 4)
      return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: Color expects three or four numbers");
    for (int i = 0; i < count; ++i)
      values[i] = number(state, i + 1);
  }
  api->color(api->context, values[0], values[1], values[2], values[3]);
  return 0;
}

int vertex(lua_State *state) {
  const auto *api = checkedApi(state);
  const int count = lua_gettop(state);
  if (count < 2 || count > 4)
    return luaL_error(state, "JITLUA_GL_BAD_ARGUMENT: Vertex expects two to four numbers");
  const double x = number(state, 1), y = number(state, 2);
  const double z = count >= 3 ? number(state, 3) : 0.0;
  const double w = count == 4 ? number(state, 4) : 1.0;
  api->vertex(api->context, x, y, z, w);
  return 0;
}

void function(lua_State *state, const char *name, lua_CFunction callback, const OpenGlApi *api) {
  lua_pushlightuserdata(state, const_cast<OpenGlApi *>(api));
  lua_pushcclosure(state, callback, 1);
  lua_setfield(state, -2, name);
}
void constant(lua_State *state, const char *name, unsigned int value) {
  lua_pushnumber(state, value);
  lua_setfield(state, -2, name);
}
int openModule(lua_State *state) {
  auto *api = static_cast<const OpenGlApi *>(lua_touserdata(state, lua_upvalueindex(1)));
  lua_newtable(state);
  function(state, "Enable", enable, api);
  function(state, "Disable", disable, api);
  function(state, "BlendFunc", blendFunc, api);
  function(state, "Begin", begin, api);
  function(state, "End", end, api);
  function(state, "Color", color, api);
  function(state, "Vertex", vertex, api);
  constant(state, "BLEND", GL_BLEND);
  constant(state, "DEPTH_TEST", GL_DEPTH_TEST);
  constant(state, "ZERO", GL_ZERO);
  constant(state, "ONE", GL_ONE);
  constant(state, "SRC_ALPHA", GL_SRC_ALPHA);
  constant(state, "ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA);
  constant(state, "POINTS", GL_POINTS);
  constant(state, "LINES", GL_LINES);
  constant(state, "LINE_STRIP", GL_LINE_STRIP);
  constant(state, "LINE_LOOP", GL_LINE_LOOP);
  constant(state, "TRIANGLES", GL_TRIANGLES);
  constant(state, "TRIANGLE_STRIP", GL_TRIANGLE_STRIP);
  constant(state, "TRIANGLE_FAN", GL_TRIANGLE_FAN);
  constant(state, "QUADS", GL_QUADS);
  constant(state, "POLYGON", GL_POLYGON);
  return 1;
}
} // namespace

bool installOpenGlPreload(lua_State *state, const OpenGlApi *api, std::string &error) {
  error.clear();
  if (!state || !api || !api->has_context || !api->enable || !api->disable || !api->blend_func ||
      !api->begin || !api->end || !api->color || !api->vertex) {
    error = "JITLUA_GL_INVALID_API: an initialized Lua state and complete API are required";
    return false;
  }
  const int top = lua_gettop(state);
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    error = "JITLUA_GL_NO_PACKAGE: host must initialize package before registering OpenGL";
    lua_settop(state, top);
    return false;
  }
  lua_getfield(state, -1, "loaded");
  if (!lua_istable(state, -1)) {
    error = "JITLUA_GL_NO_LOADED: package.loaded is not a table";
    lua_settop(state, top);
    return false;
  }
  lua_getfield(state, -1, "opengl");
  if (!lua_isnil(state, -1)) {
    error = "JITLUA_GL_ALREADY_REGISTERED: opengl is already loaded";
    lua_settop(state, top);
    return false;
  }
  lua_pop(state, 2);
  lua_getfield(state, -1, "preload");
  if (!lua_istable(state, -1)) {
    error = "JITLUA_GL_NO_PRELOAD: package.preload is not a table";
    lua_settop(state, top);
    return false;
  }
  lua_getfield(state, -1, "opengl");
  if (!lua_isnil(state, -1)) {
    error = "JITLUA_GL_ALREADY_REGISTERED: opengl is already preloaded";
    lua_settop(state, top);
    return false;
  }
  lua_pop(state, 1);
  lua_pushlightuserdata(state, const_cast<OpenGlApi *>(api));
  lua_pushcclosure(state, openModule, 1);
  lua_setfield(state, -2, "opengl");
  lua_settop(state, top);
  return true;
}
} // namespace jitlua
