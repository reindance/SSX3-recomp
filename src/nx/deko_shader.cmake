# The Vulkan build's GLSL as deko3d GLSL (src/nx/dkr.c), for uam:
#   cmake -DIN=<xbox.frag> -DOUT=<out.glsl> [-DDEFS=W_DEPTH;...] -P deko_shader.cmake
#
# - push constants -> a std140 uniform block at binding 1 (dkr.c fills it
#   per draw, one buffer for both stages; the members keep their offsets)
# - no descriptor sets: RC at uniform binding 0, the samplers at 0-3, the
#   vertex program's storage buffer at 0
# - specialization constants -> plain constants (SPEC = 0: the combiners
#   are interpreted, as RECOMP_VKR_SPEC=0 on the desktop)
# - gl_VertexIndex -> gl_VertexID (GLSL, not Vulkan GLSL)
file(READ "${IN}" src)
set(head "#version 460\n#define DEKO 1\n")
foreach(d ${DEFS})
    string(APPEND head "#define ${d} 1\n")
endforeach()
string(REGEX REPLACE "#version 450\n" "${head}" src "${src}")
string(REPLACE "layout(push_constant) uniform PC" "layout(std140, binding = 1) uniform PC" src "${src}")
string(REPLACE "layout(set = 0, binding = 4, std140) uniform RC" "layout(std140, binding = 0) uniform RC" src "${src}")
string(REPLACE "layout(set = 0, binding = 0) uniform sampler2D" "layout(binding = 0) uniform sampler2D" src "${src}")
string(REPLACE "layout(std430, set = 0, binding = 5)" "layout(std430, binding = 0)" src "${src}")
string(REGEX REPLACE "layout\\(constant_id = [0-9]+\\) const" "const" src "${src}")
string(REPLACE "gl_VertexIndex" "gl_VertexID" src "${src}")
if(NOT src MATCHES "#define DEKO")
    message(FATAL_ERROR "${IN}: no '#version 450' line to replace")
endif()
file(WRITE "${OUT}" "${src}")
