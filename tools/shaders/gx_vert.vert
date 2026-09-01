#version 450

// GX vertex shader (M5.1): position + vertex color. The MVP is the GX
// projection matrix (clip-space ready) pushed as a uniform; vertices arrive
// in the GX attribute order serialized to floats.
layout(location = 0) in vec3 inPos;     // GX position (2D -> z=0)
layout(location = 1) in vec4 inColor;   // GX color0 (RGBA8, 0..1)

layout(push_constant) uniform PC {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor = inColor;
}
