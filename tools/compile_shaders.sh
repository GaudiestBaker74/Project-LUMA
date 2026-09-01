#!/usr/bin/env bash
# Regenerates src/platform/Renderer/vk_demo_shaders.h from tools/shaders/*.{vert,frag}.
# Requires glslangValidator (Linux: apt install glslang-tools; Windows: Vulkan SDK ships glslc).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=src/platform/Renderer/vk_demo_shaders.h

rm -f /tmp/gpc_*.spv
glslangValidator -V tools/shaders/tri.vert -o /tmp/gpc_tri_vert.spv
glslangValidator -V tools/shaders/tri.frag -o /tmp/gpc_tri_frag.spv
glslangValidator -V tools/shaders/tri_tex.vert -o /tmp/gpc_tri_tex_vert.spv
glslangValidator -V tools/shaders/tri_tex.frag -o /tmp/gpc_tri_tex_frag.spv
glslangValidator -V tools/shaders/gx_vert.vert -o /tmp/gpc_gx_vert.spv
glslangValidator -V tools/shaders/gx_frag.frag -o /tmp/gpc_gx_frag.spv
glslangValidator -V tools/shaders/gx_tex_vert.vert -o /tmp/gpc_gx_tex_vert.spv
glslangValidator -V tools/shaders/gx_tex_frag.frag -o /tmp/gpc_gx_tex_frag.spv
glslangValidator -V tools/shaders/gx_tev_vert.vert -o /tmp/gpc_gx_tev_vert.spv
glslangValidator -V tools/shaders/gx_tev_frag.frag -o /tmp/gpc_gx_tev_frag.spv

python3 - "$OUT" <<'PY'
import struct
import sys

out = sys.argv[1]

def spv_array(path, name):
    data = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = []
    for i in range(0, len(words), 6):
        chunk = ', '.join('0x%08x' % w for w in words[i:i + 6])
        lines.append('    ' + chunk + ',')
    return 'static const uint32_t %s[%d] = {\n%s\n};' % (name, len(words), '\n'.join(lines))

hdr = '// Auto-generated from tools/shaders (GLSL -> SPIR-V via glslangValidator).\n'
hdr += '// Regenerate: tools/compile_shaders.sh  -  do not edit by hand.\n\n'
hdr += spv_array('/tmp/gpc_tri_vert.spv', 'kTriangleVertSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_tri_frag.spv', 'kTriangleFragSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_tri_tex_vert.spv', 'kTriangleTexVertSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_tri_tex_frag.spv', 'kTriangleTexFragSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_vert.spv', 'kGxVertSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_frag.spv', 'kGxFragSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_tex_vert.spv', 'kGxTexVertSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_tex_frag.spv', 'kGxTexFragSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_tev_vert.spv', 'kGxTevVertSpv') + '\n\n'
hdr += spv_array('/tmp/gpc_gx_tev_frag.spv', 'kGxTevFragSpv') + '\n'
open(out, 'w').write(hdr)
print('regenerated', out)
PY
