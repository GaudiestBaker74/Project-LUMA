#version 450

// Textured quad/geometry: position + UV, 2D rotation pushed as a uniform.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;

layout(push_constant) uniform Rot {
    mat3 m; // column-major 2D rotation (48 bytes, std430)
} pc;

layout(location = 0) out vec2 fragUV;

void main() {
    vec3 p = pc.m * vec3(inPos, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
    fragUV = inUV;
}
