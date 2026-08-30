# docs/gx.md — Capa de compatibilidad GX (análisis y diseño)

> Estado: M0 (análisis). Este documento define **qué** implementa `src/compat/gx/` y **cómo** se traduce a Vulkan.
> **M3**: primer olor implementado en `src/compat/gx/GXCompat.{h,cpp}` — `GXInit`,
> `GXSetViewport`, `GXClearColor` y `GXClear` dibujan el fondo del demo a través de
> `Platform::Video` (ver §7, sub-hitos M5). `GXClearColor`/`GXClear` no existen en el
> RVL_SDK vendered: GXCompat declara las firmas estándar reutilizando `GXColor`.
> El inventario de llamadas se regenera con: `grep -rhoE "GX[A-Za-z0-9_]+\(" src/Game src/JSystem src/nw4r --include=*.cpp --include=*.hpp | sed 's/(//' | sort | uniq -c | sort -rn`

---

## 1. Objetivo

El game code (Game/JSystem/nw4r) llama a **~165 funciones GX distintas** (más de 2 500 llamadas) para dibujar todo el juego: geometría, partículas, UI, efectos de pantalla. Nuestro objetivo es **mantener esas llamadas intactas** en el código y hacer que `src/compat/gx/` las ejecute sobre Vulkan, con una fidelidad de imagen equivalente a la Wii (no byte-exacta, sí visual).

## 2. Modelo mental: GX como máquina de estado + FIFO

En la Wii, GX es una máquina de estado que se configura con registros (escrituras a la FIFO `GX_FIFO` de 32 bytes) y que dibuja con una lista de comandos (display list). El juego configura el estado (TEV, blending, depth, matrices…) y luego emite vértices con `GXBegin/GXEnd` + `GXPosition3f32/GXColor1u32/GXTexCoord2f32...`, o invoca display lists precapturadas (`GXCallDisplayList`).

Para el port adoptamos el modelo **"estado → pipeline"**:

1. `compat/gx` mantiene un **estado GX espejo** (estructuras que replican los registros GX relevantes).
2. Cada llamada GX de configuración actualiza el espejo.
3. En el momento del dibujo (primer vértice tras `GXBegin`, o `GXCallDisplayList`), el estado se **compila a un pipeline Vulkan** (shader generado + pipeline cache) si no existe ya en caché (hash del estado).
4. Los vértices se acumulan en buffers dinámicos y se dibujan.

## 3. Inventario del uso real (medido en el repo)

Las ~165 funciones GX usadas, clasificadas:

### A. Inicialización / control de FIFO (trivial)
`GXInit`, `GXSetMisc`, `GXResetWriteGatherPipe`, `GXAbortFrame`, `GXFlush`, `GXDrawDone`, `GXPixModeSync`, `GXSetDrawDoneCallback`, `GXSetDrawSync`, `GXReadDrawSync`…
→ En PC: no hay FIFO hardware. `GXInit` inicializa el backend; `GXFlush/GXDrawDone` son no-ops (el envío a Vulkan es inmediato); los draw sync se mapean a fences/eventos del renderer.

### B. Formatos y atributos de vértice (trivial-trasladable)
`GXSetVtxDesc`, `GXClearVtxDesc`, `GXSetVtxAttrFmt`, `GXSetArray` (buffers de atributos), `GXGetVtxDesc`…
→ Se traducen a **vertex layout** Vulkan (`VkVertexInputAttributeDescription`). Los formatos GX: `u8`, `s8`, `u16`, `s16`, `f32`, `rgb565`, `rgb8`, `rgba8`, con escalado (`GX_DIRECT`, `GX_INDEX8/16`). Cuidado: **los formatos "index8/index16" usan el campo `GXSetArray`** para resolver el índice; en Vulkan se resuelve en CPU (vertex buffer expandido) o con buffers de índices de atributos.

### C. Vértices inmediatos (trivial-trasladable)
`GXBegin/GXEnd`, `GXPosition3f32/2f32`, `GXPosition2u16`, `GXNormal3f32`, `GXColor1u32`, `GXColor4u8`, `GXTexCoord2f32/2s16/2u16`, `GXTexCoord2f32ptr`, `GXCmd1f32ptr`…
→ Se acumulan en un buffer dinámico y se convierten al layout definido en B. **Cuidado con el orden de atributos**: el formato de vértice define pos/normal/color/tex en un orden concreto (bitmask), y los parámetros pueden intercalarse de forma distinta a la declaración (el juego declara el layout en `GXSetVtxAttrFmt` y luego emite en un orden fijo).

### D. Display lists (medio)
`GXBeginDisplayList`, `GXEndDisplayList`, `GXCallDisplayList`, `GXFastCallDisplayList`…
→ En la Wii una DL es una secuencia de comandos GX crudos en memoria. En PC: o (a) reinterpretar la DL como secuencia de llamadas (interprete ligero), o (b) capturar el estado y re-ejecutar. **Decisión inicial: intérprete de display list** que consume el mismo formato de comandos que genera el juego (los comandos se escriben con macros `GXCmd*`/`GDWrite*`). J3D y nw4r lyt generan DLs con estas macros; hay que soportar el subconjunto de opcodes que emiten (documentar en este fichero a medida que se implemente).

### E. Transformación y vista (trivial)
`GXLoadPosMtxImm`, `GXLoadNrmMtxImm`, `GXLoadTexMtxImm`, `GXSetCurrentMtx`, `GXSetProjection` (perspectiva/ortográfica), `GXSetViewport`, `GXSetScissor`, `GXSetClipMode`, `GXGetProjection`…
→ Se traducen a **uniform buffers** (matrices) + viewport/scissor de Vulkan. La matriz del juego ya viene en el formato de 3×4 fila-mayor de GX; se sube tal cual (Vulkan espera column-major → transponer en frontera o subir con el layout esperado; documentado en renderer.md).

### F. Texturas (medio)
`GXInitTexObj` (formato, wrap, filtros), `GXInitTexObjLOD`, `GXLoadTexObj`, `GXLoadTexMtxImm` (ya en E), `GXGetTexObjWidth/Height/Format`…
→ **Los formatos de textura GX no son nativos de Vulkan** (I8, IA8, RGB565, RGB5A3, RGBA8, CMPR/DXT1, Z24X8…): se convierten en carga (o se suben con formatos emulados en shader) a formatos Vulkan (`VkFormat`). Tabla de conversión en §6. `GXLoadTexObj` asocia el slot de textura (0..7) → se mapea a descriptores.

### G. TEV — *la parte difícil* (requiere shaders)
`GXSetNumTevStages`, `GXSetTevOrder`, `GXSetTevColorIn`, `GXSetTevAlphaIn`, `GXSetTevColorOp`, `GXSetTevAlphaOp`, `GXSetTevColor`, `GXSetTevColorS10`, `GXSetTevKColor`, `GXSetTevKColorSel`, `GXSetTevKAlphaSel`, `GXSetTevSwapModeTable`, `GXSetTevDirect`, `GXSetTevOp`, `GXSetNumIndStages`, `GXSetIndTexOrder`, `GXSetIndTexMtx`, `GXSetIndTexCoordScale`, `GXSetTevIndirect`, `GXTevStageID`…
→ El TEV permite **hasta 16 etapas** por pasada, cada una con: fuente de color (registro, textura de un slot, color del canal, K) y alfa, operación (add, sub, lerp con factor), clamp/bias, y **swizzle** de canales. Más las **etapas indirectas** (distorsión UV por una textura indirecta con matriz 2×3). Estrategia:
  - Compilar cada conjunto de etapas TEV activas a un **fragment shader generado** (GLSL/HLSL textual o SPIR-V con reflect) con las constantes como push constants/uniforms.
  - El "rasterizador" (interpolación de color/UV) se hace con los atributos del vértice; el TEV opera por píxel.
  - La emulación de las **ind stages** (hasta 3) se hace con un sampleo extra de la textura indirecta y una transformación UV en el shader.
  - **Caché de pipelines** con hash del estado completo (blend+TEV+depth+...); el número de combinaciones reales usadas por el juego es finito y pequeño (el juego configura pocos modos "canónicos").
  - Primera implementación: **GXSetTevOp** (combinaciones estándar tipo "replace/modulate") y el TEV directo usado por J3D; después los casos con ind stages.

### H. Iluminación por canal (medio)
`GXSetChanCtrl`, `GXSetNumChans`, `GXSetChanMatColor`, `GXSetChanAmbColor`, `GXSetLightDir`, `GXSetLightColor`, `GXSetNumLight`…
→ En la Wii la iluminación se calcula en la unidad de vértices (GX lighting por canal: material color × luz). En PC se implementa en el **vertex shader** (o se precalcula en CPU). SMG usa iluminación por canal sobre todo para objetos con luz de color (light areas) y para el "matcolor" de los actores (fade). Soporte inicial: `GX_AMBIENT` + `GX_DIFFUSE` con color de material y luz direccional.

### I. Pipeline de píxeles (medio)
`GXSetZMode`, `GXSetZCompLoc`, `GXSetAlphaCompare`, `GXSetAlphaUpdate`, `GXSetColorUpdate`, `GXSetDstAlpha`, `GXSetBlendMode`, `GXSetCullMode`, `GXSetDither`, `GXSetFog`, `GXSetTevSwapMode`, `GXSetDither`…
→ 1:1 con estado de pipeline Vulkan: depth test/compare/write, blend (los 8 modos GX → `VkBlendFactor`), cull, alpha-to-coverage (alpha compare → discard en shader o alpha test), fog (GLSL mix con fórmula de GX). `GXSetZCompLoc` (z compare antes/después de alpha test) se emula ordenando las operaciones en el shader.

### J. Copy / EFB → XFB (presentación)
`GXSetCopyClear`, `GXSetCopyFilter`, `GXSetCopyClamp`, `GXSetDispCopySrc`, `GXSetDispCopyDst`, `GXSetDispCopyYScale`, `GXSetDispCopyGamma`, `GXCopyDisp`, `GXSetCopyMipmap`…
→ El juego dibuja a un **EFB** (framebuffer de 640×448/640×576 con ciertos modos 2×) y luego copia a un XFB (framebuffer de presentación). En PC: el EFB es un **render target Vulkan** (posiblemente con resolución interna fija = la del modo elegido, y opción de escala), y `GXCopyDisp` se convierte en un **blit/present** (con la opción de que el usuario renderice a resolución nativa de monitor más adelante). La "copia" con filtros verticales y gamma se puede simplificar (equivalencia visual) o emular con un blit por shader. El clear del EFB (`GXSetCopyClear`) es el clear del render target.
→ **El triple buffer XFB/JUTXfb/MainLoopFramework** (ver architecture.md §3) se sustituye por la presentación del renderer; `compat/vi` dispara los callbacks de retrace alrededor del present.

### K. Misc / getters (trivial)
`GXGetTexObjWidth/Height/Format/LOD`, `GXGetProjection`, `GXGetVtxDesc`, `GXGetNumXfbLines`, `GXGetYScaleFactor`, `GXGetGPStatus`…
→ Se responden desde el estado espejo (sin GPU).

## 4. Clasificación resumida (para planificar M5)

| Clase | Funciones | Estrategia |
|---|---|---|
| Init/FIFO control | ~10 | no-op/fences en el renderer |
| Vértices: desc, formatos, arrays, inmediatos | ~20 | vertex layout + buffers dinámicos |
| Display lists | ~6 | intérprete de DL (opcodes GD/GX) |
| Transform/vista/proyección | ~12 | uniform buffers + viewport/scissor |
| Texturas (formato, LOD, carga) | ~15 | conversión de formato a Vulkan + descriptores |
| **TEV (etapas, ind stages)** | ~35 | **shaders generados + caché de pipelines** |
| Iluminación por canal | ~10 | vertex shader |
| Pixel pipeline (z, blend, fog, dither…) | ~20 | estado de pipeline Vulkan 1:1 |
| Copy EFB→XFB | ~15 | render target + blit/present |
| Getters | ~20 | estado espejo |

## 5. Fidelidad

- **Meta:** equivalencia visual; no se busca reproducir bugs de redondeo de la Wii.
- Excepciones conocidas donde la Wii tiene comportamiento distinto al estándar (a documentar en `porting.md`): conversiones `float→int` truncantes (`OSf32tou8`), orden de z vs alpha test (`GXSetZCompLoc`), y el gamma de la copia EFB→XFB (el juego asume gamma 1.0 en el copy por defecto).
- El **dithering** GX es de 6 bits → se puede ignorar inicialmente (`TODO(PC_PORT)`) sin pérdida visual apreciable en la mayoría de escenas, o emular con dither en shader.

## 6. Formatos de textura GX → Vulkan (tabla inicial)

| Formato GX | Bits | Vulkan | Nota |
|---|---|---|---|
| `GX_TF_I4` | 4 | emular en shader o expandir a R8 | conversión a `R8` en carga |
| `GX_TF_I8` | 8 | `R8_UNORM` | |
| `GX_TF_IA4` | 8 | `R8G8_UNORM` | luma+alpha |
| `GX_TF_IA8` | 16 | `R8G8_UNORM` | |
| `GX_TF_RGB565` | 16 | `R5G6B5_UNORM_PACK16` | |
| `GX_TF_RGB5A3` | 16 | `A1R5G5B5_UNORM_PACK16` | bit15 → 555 con alpha 1 |
| `GX_TF_RGBA8` | 32 | `R8G8B8A8_UNORM` | **swizzle ARGB→RGBA** |
| `GX_TF_CMPR` | 4 | `BC1_RGB_UNORM_BLOCK` | DXT1, con el alpha de 1 bit según uso |
| `GX_TF_Z24X8` | 32 | usar como profundidad o emular | solo para el clear_z_tobj del MainLoopFramework |
| `GX_CTF_*` (copias) | — | variantes para el path de copy | menor prioridad |

El layout en memoria de los formatos GX (bloques de 4×4, tiles) es **distinto** al de Vulkan → la conversión es un paso real en CPU en la carga (con tests). `JUTTexture`/`J3DTexture` ya cargan BTI y entregan el buffer crudo con el layout GX; `compat/gx` (o una utilidad `tools/convert-texture`) lo convierte.

## 7. Estado de implementación (se actualiza en cada milestone)

| Fase | Contenido | Estado |
|---|---|---|
| M5.1 | `GXInit`, estado espejo, `GXSetViewport/Scissor/Projection`, clear, vértices inmediatos, primer draw | ⬜ |
| M5.2 | `GXSetVtxDesc/VtxAttrFmt/Array`, buffers dinámicos | ⬜ |
| M5.3 | Texturas: carga BTI→Vulkan, samplers, `GXSetTexCoordGen2` | ⬜ |
| M5.4 | TEV básico (`GXSetTevOp`, order, color/alpha in/op), caché de pipelines | ⬜ |
| M5.5 | Blend/depth/cull/alpha compare/fog | ⬜ |
| M5.6 | Display lists (intérprete) | ⬜ |
| M5.7 | Ind stages, swap mode, chan lighting, copy EFB→present | ⬜ |

Cada función nueva se añade con su fila en la tabla de inventario y su clase; ver `docs/porting.md` (flujo de trabajo para añadir GX).
