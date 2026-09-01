#version 450

// GX textured fragment shader (M5.3): texel * vertex color (the GX TEV
// "modulate" of stage 0). Samples TEXMAP0 = set 0, binding 0. Real TEV
// (multi-stage) arrives in M5.4.
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV) * vColor;
}
