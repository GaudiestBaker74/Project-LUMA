#version 450

// GX textured vertex shader (M5.3): position + vertex color + texcoord0.
// The MVP is the GX projection matrix (clip-space ready) pushed as a uniform;
// texcoords arrive already resolved by the CPU texgen (GXSetTexCoordGen2).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform PC {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor = inColor;
    vUV = inUV;
}
