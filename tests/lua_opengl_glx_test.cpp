// Integration contract: render one Lua-driven quad in a private GLX pbuffer.
// No Live or Max process is contacted; a compatibility-profile X display is required.
#include "../jitlua/lua_opengl.hpp"
#include "../jitlua/lua_runtime.hpp"
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

namespace {
struct GlxContext {
  Display *display = nullptr;
  GLXContext context = nullptr;
  GLXPbuffer pbuffer = 0;
  ~GlxContext() {
    if (!display)
      return;
    glXMakeContextCurrent(display, None, None, nullptr);
    if (context)
      glXDestroyContext(display, context);
    if (pbuffer)
      glXDestroyPbuffer(display, pbuffer);
    XCloseDisplay(display);
  }
};
bool current(void *) noexcept { return glXGetCurrentContext() != nullptr; }
void enable(void *, unsigned int value) noexcept { glEnable(value); }
void disable(void *, unsigned int value) noexcept { glDisable(value); }
void blend(void *, unsigned int source, unsigned int destination) noexcept {
  glBlendFunc(source, destination);
}
void begin(void *, unsigned int mode) noexcept { glBegin(mode); }
void end(void *) noexcept { glEnd(); }
void color(void *, double r, double g, double b, double a) noexcept { glColor4d(r, g, b, a); }
void vertex(void *, double x, double y, double z, double w) noexcept { glVertex4d(x, y, z, w); }
int fail(const char *code, const std::string &message) {
  std::cerr << code << ": " << message << '\n';
  return 1;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return fail("GLX_FIXTURE_ARGUMENT", "Pass the path to glx_quad.lua");
  std::ifstream script_file(argv[1], std::ios::binary);
  if (!script_file)
    return fail("GLX_FIXTURE_READ", "Cannot read the Lua fixture");
  const std::string script{std::istreambuf_iterator<char>(script_file),
                           std::istreambuf_iterator<char>()};
  GlxContext gl;
  gl.display = XOpenDisplay(nullptr);
  if (!gl.display)
    return fail("GLX_DISPLAY_UNAVAILABLE", "Set DISPLAY to an accessible X11 display");
  const int fb_attributes[] = {GLX_X_RENDERABLE,
                               True,
                               GLX_DRAWABLE_TYPE,
                               GLX_PBUFFER_BIT,
                               GLX_RENDER_TYPE,
                               GLX_RGBA_BIT,
                               GLX_RED_SIZE,
                               8,
                               GLX_GREEN_SIZE,
                               8,
                               GLX_BLUE_SIZE,
                               8,
                               GLX_ALPHA_SIZE,
                               8,
                               GLX_DOUBLEBUFFER,
                               False,
                               None};
  int count = 0;
  GLXFBConfig *configs =
      glXChooseFBConfig(gl.display, DefaultScreen(gl.display), fb_attributes, &count);
  if (!configs || count == 0) {
    if (configs)
      XFree(configs);
    return fail("GLX_CONFIG_UNAVAILABLE", "No RGBA pbuffer framebuffer config");
  }
  gl.context = glXCreateNewContext(gl.display, configs[0], GLX_RGBA_TYPE, nullptr, True);
  const int pbuffer_attributes[] = {GLX_PBUFFER_WIDTH, 4, GLX_PBUFFER_HEIGHT, 4, None};
  gl.pbuffer = glXCreatePbuffer(gl.display, configs[0], pbuffer_attributes);
  XFree(configs);
  if (!gl.context || !gl.pbuffer ||
      !glXMakeContextCurrent(gl.display, gl.pbuffer, gl.pbuffer, gl.context))
    return fail("GLX_CONTEXT_UNAVAILABLE", "Could not create or bind a compatibility pbuffer");
  const auto *renderer = glGetString(GL_RENDERER);
  const auto *version = glGetString(GL_VERSION);
  if (!renderer || !version)
    return fail("GLX_DRIVER_UNAVAILABLE", "GL context has no renderer/version");
  const jitlua::OpenGlApi api{nullptr, current, enable, disable, blend, begin, end, color, vertex};
  std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
  if (!state)
    return fail("LUA_STATE_UNAVAILABLE", "luaL_newstate returned null");
  luaL_openlibs(state.get()); // Standalone system Lua; never used in Max.
  jitlua::Runtime runtime(state.get());
  std::string error;
  if (!jitlua::installOpenGlPreload(state.get(), &api, error) ||
      !runtime.load(script.data(), script.size(), "glx-quad", error))
    return fail("LUA_GL_SETUP", error);
  glViewport(0, 0, 4, 4);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (!runtime.call("draw", {}, error))
    return fail("LUA_GL_DRAW", error);
  glFinish();
  glReadBuffer(GL_FRONT);
  std::array<unsigned char, 4> pixel{};
  glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
  const auto gl_error = glGetError();
  if (gl_error != GL_NO_ERROR)
    return fail("GLX_RENDER_ERROR", std::to_string(gl_error));
  if (pixel[0] < 240 || pixel[1] > 15 || pixel[2] > 15 || pixel[3] < 240)
    return fail("GLX_PIXEL_MISMATCH", "Expected an opaque red center pixel");
  std::cout << "GLX_QUAD_PASS renderer=" << reinterpret_cast<const char *>(renderer)
            << " version=" << reinterpret_cast<const char *>(version)
            << " pixel=" << static_cast<int>(pixel[0]) << ',' << static_cast<int>(pixel[1]) << ','
            << static_cast<int>(pixel[2]) << ',' << static_cast<int>(pixel[3]) << '\n';
}
