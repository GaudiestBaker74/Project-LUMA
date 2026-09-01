#version 450

// GX TEV vertex shader (M5.4): fixed attribute layout for the GX path.
//   location 0: position        (vec3)
//   location 1: vertex color 0  (vec4)
//   location 2: vertex color 1  (vec4)
//   location 3..10: texcoord 0..7 (vec2)
// compat/gx::flushDraw always emits this full layout (missing attributes are
// filled with zeros / white), so one vertex format serves every GX draw.
// The MVP is the GX projection matrix (clip-space ready); texcoords arrive
// already resolved by the CPU texgen (GXSetTexCoordGen2).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor0;
layout(location = 2) in vec4 inColor1;
layout(location = 3) in vec2 inUV[8];

layout(push_constant) uniform PC {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 vColor0;
layout(location = 1) out vec4 vColor1;
layout(location = 2) out vec2 vUV[8];

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor0 = inColor0;
    vColor1 = inColor1;
    for (int i = 0; i < 8; ++i) {
        vUV[i] = inUV[i];
    }
}
