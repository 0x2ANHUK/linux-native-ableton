local gl = require("opengl")

function draw()
  gl.Enable(gl.BLEND)
  gl.Disable(gl.DEPTH_TEST)
  gl.BlendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
  gl.Begin(gl.QUADS)
  gl.Color({ 0.25, 0.5, 0.75 })
  gl.Vertex(0.5, 0.75)
  gl.End()
end

function bad_color()
  gl.Color(1, 2)
end
function bad_color_table()
  gl.Color({ 1, 2 })
end
function bad_enum()
  gl.Begin(-1)
end
function no_context()
  gl.Enable(gl.BLEND)
end
function unsupported()
  gl.TexCoord(0, 0)
end

function extra_end()
  gl.End(1)
end
function bad_type()
  gl.Enable("1")
end
function bad_vertex()
  gl.Vertex(1)
end
