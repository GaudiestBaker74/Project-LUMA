#version 450

// Samples a single combined image sampler (set 0, binding 0) — the M4.2
// texture path. Multiple bindings (GX TEV stages) come in M5.
layout(location = 0) in vec2 fragUV;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, fragUV);
}
