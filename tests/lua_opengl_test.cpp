// Contract: exercise the partial Lua OpenGL API with a deterministic fake host.
#include "../jitlua/lua_opengl.hpp"
#include "../jitlua/lua_runtime.hpp"
#include <GL/gl.h>
#include <array>
#include <cassert>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Recorder {
  bool current = true;
  std::vector<unsigned int> enabled;
  std::vector<unsigned int> disabled;
  std::array<unsigned int, 2> blend{};
  unsigned int primitive = 0;
  int ended = 0;
  std::array<double, 4> color{};
  std::array<double, 4> vertex{};
};
Recorder &rec(void *p) { return *static_cast<Recorder *>(p); }
bool current(void *p) noexcept { return rec(p).current; }
void enable(void *p, unsigned int v) noexcept { rec(p).enabled.push_back(v); }
void disable(void *p, unsigned int v) noexcept { rec(p).disabled.push_back(v); }
void blend(void *p, unsigned int a, unsigned int b) noexcept { rec(p).blend = {a, b}; }
void begin(void *p, unsigned int v) noexcept { rec(p).primitive = v; }
void end(void *p) noexcept { ++rec(p).ended; }
void color(void *p, double a, double b, double c, double d) noexcept {
  rec(p).color = {a, b, c, d};
}
void vertex(void *p, double a, double b, double c, double d) noexcept {
  rec(p).vertex = {a, b, c, d};
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  std::ifstream input(argv[1], std::ios::binary);
  assert(input.is_open());
  const std::string script{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  Recorder recorder;
  jitlua::OpenGlApi api{&recorder, current, enable, disable, blend, begin, end, color, vertex};
  std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), &lua_close);
  assert(state);
  std::string error;
  const int original_top = lua_gettop(state.get());
  assert(!jitlua::installOpenGlPreload(state.get(), &api, error));
  assert(error.find("JITLUA_GL_NO_PACKAGE") != std::string::npos);
  assert(lua_gettop(state.get()) == original_top);
  luaL_openlibs(state.get()); // System Lua in this standalone test, never MaxLua.
  assert(!jitlua::installOpenGlPreload(nullptr, &api, error));
  assert(error.find("JITLUA_GL_INVALID_API") != std::string::npos);
  assert(jitlua::installOpenGlPreload(state.get(), &api, error));
  assert(lua_gettop(state.get()) == original_top);
  assert(!jitlua::installOpenGlPreload(state.get(), &api, error));
  assert(error.find("JITLUA_GL_ALREADY_REGISTERED") != std::string::npos);
  jitlua::Runtime runtime(state.get());
  assert(runtime.load(script.data(), script.size(), "opengl-smoke", error));
  assert(runtime.call("draw", {}, error));
  assert(recorder.enabled == std::vector<unsigned int>{GL_BLEND});
  assert(recorder.disabled == std::vector<unsigned int>{GL_DEPTH_TEST});
  assert((recorder.blend == std::array<unsigned int, 2>{GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA}));
  assert(recorder.primitive == GL_QUADS && recorder.ended == 1);
  assert((recorder.color == std::array<double, 4>{0.25, 0.5, 0.75, 1.0}));
  assert((recorder.vertex == std::array<double, 4>{0.5, 0.75, 0.0, 1.0}));
  assert(!runtime.call("bad_color", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!runtime.call("bad_color_table", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!runtime.call("bad_enum", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!runtime.call("unsupported", {}, error));
  assert(!runtime.call("extra_end", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!runtime.call("bad_type", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!runtime.call("bad_vertex", {}, error) &&
         error.find("JITLUA_GL_BAD_ARGUMENT") != std::string::npos);
  assert(!jitlua::installOpenGlPreload(state.get(), &api, error));
  assert(error.find("JITLUA_GL_ALREADY_REGISTERED") != std::string::npos);
  recorder.current = false;
  assert(!runtime.call("no_context", {}, error) &&
         error.find("JITLUA_GL_NO_CONTEXT") != std::string::npos);
  assert(recorder.enabled.size() == 1);
  auto incomplete = api;
  incomplete.end = nullptr;
  assert(!jitlua::installOpenGlPreload(state.get(), &incomplete, error));
  assert(error.find("JITLUA_GL_INVALID_API") != std::string::npos);
  return 0;
}
