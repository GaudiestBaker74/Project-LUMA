#version 450

// Demo triangle: position + color. A 2D rotation matrix is pushed via a
// push constant (Platform::Renderer::setUniforms) so the rotation exercises
// the uniform path instead of rewriting the vertex buffer per frame.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform Rot {
    mat3 m; // column-major 2D rotation (48 bytes, std430)
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    vec3 p = pc.m * vec3(inPos, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
    fragColor = inColor;
}
