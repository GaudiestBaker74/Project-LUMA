# docs/gx.md — Capa de compatibilidad GX (análisis y diseño)

> Estado: M5.4 ✅ (TEV básico; ver §7). Este documento define
> **qué** implementa `src/compat/gx/` y **cómo** se traduce a Vulkan.
> **M3**: primer olor — `GXClearColor`/`GXClear` dibujan el fondo del demo (firmas
> estándar declaradas localmente porque no están en el RVL_SDK vendered).
> **M5.1**: `GXInit` real, estado espejo (VCD/VAT, proyección, viewport/scissor,
> cull/blend, clear), captura de la FIFO simulada → vértices en VCD order, y el
> primer draw con color de vértice (quad GX en la demo).
> **M5.2**: `GXSetArray` + `GX_INDEX8/16` (atributos resueltos desde arrays, con
> stride y escalado VAT) y el acumulador de vértices convertido en **vertex buffer
> dinámico** del renderer (crece, se reutiliza entre frames).
> **M5.3**: objetos de textura GX (`GXInitTexObj/LOD`, `GXLoadTexObj`), decodificador
> BTI (deswizzle 4x4/8x4 → RGBA8, ver §6) y `GXSetTexCoordGen2`/`GXLoadTexMtxImm`
> resueltos en CPU.
> **M5.4**: TEV básico — `GXSetTevOp`, `GXSetTevColorIn/AlphaIn/ColorOp/AlphaOp`,
> `GXSetTevColor/S10`, `GXSetTevOrder`, `GXSetTevKColor(sel)`, swap modes/tablas,
> `GXSetNumTevStages`; combinador por etapa (≤16) en un fragment shader con UBO
> dinámico y evaluador CPU de referencia. Ver §7.
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
`GXSetCopyClear`, `GXSetCopyFilter`, `GXSetCopyClamp`, `GXSetDispCopySrc`, `GXSetDispCopyDst`, `GXSetDispCopyYScale`, `GXSetDispCopyGamma`, `GXCopyDisp`, `GXCopyTex`, `GXSetTexCopySrc/Dst`, `GXGetYScaleFactor`, `GXGetNumXfbLines`, `GXGetTexBufferSize`… (M5.7c ✅)
→ El juego dibuja a un **EFB** (framebuffer de 640×448/640×576 con ciertos modos 2×) y luego copia a un XFB (framebuffer de presentación). En PC el EFB es un **render target Vulkan** creado perezosamente con el tamaño de `GXSetDispCopySrc` (default 640×448) y el formato del swapchain (`passColorFormat`, para que las pipelines de `flushDraw` matcheen el attachment). `GXCopyDisp` hace un **blit del EFB al swapchain** (`Renderer::blitPassToSwapchain`, escalado LINEAR al tamaño de ventana) y `endFrame()` presenta; el **triple buffer XFB/JUTXfb/MainLoopFramework** se sustituye por el buffering propio del swapchain, y `compat/vi` (M8) disparará los callbacks de retrace alrededor del present.
→ `GXCopyTex` (EFB→textura) lee el rectángulo de `GXSetTexCopySrc` con `Renderer::readRenderTarget` (readback síncrono con staging + fence) y lo codifica al formato tiled GX de `GXSetTexCopyDst` (`Platform::CompatGx::encodeEfbRgbaToGx`: I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR + cadenas mip por box-filter), escribiendo en el buffer del juego (dimensionado con `GXGetTexBufferSize`); ese buffer se carga luego con `GXInitTexObj`/`GXLoadTexObj` normal. Si la copia se llama **entre passes**, `Renderer::flushFrame()` envía el command buffer del frame para que el readback vea el contenido recién dibujado; si se llama **en medio de un pass**, el readback ve el último frame enviado (TODO(PC_PORT): split del pass a mitad — patrón de capturas mid-frame tipo MarioActorSpecialDraw).
→ Los filtros de copia (vertical, AA, gamma, field rendering) se espejan pero no se aplican: el juego asume gamma 1.0 (`GXSetDispCopyGamma` != 1.0 loggea TODO(PC_PORT)), el filtro vertical se aproxima con el blit LINEAR y el EFB es progresivo. `GXSetCopyClear` enruta al clear color del renderer: el pass del EFB limpia con él al empezar (equivalente al copy-clear que limpia el EFB para el siguiente frame). `GXSetCopyMipmap` **no existe en el SDK vendered** (el mipmap de copia va en `GXSetTexCopyDst`); no se implementa.
→ **Fix de Bti.cpp (M5.7c)**: el decodificador CMPR original usaba un framing 8×4 con el byte de índices de la fila 3 fuera del subtile (offset 16, corrompía el bloque siguiente) y duplicaba el tamaño (`btiImageSize` = w×h vs el real w·h/2 de `GXGetTexBufferSize`, riesgo de overflow en buffers del juego). Corregido al framing GX/DXT1 estándar (bloques 8×8 = 4 subtiles de 8 bytes; índices: un byte por fila, LSB = izquierda); el codificador de `GXCopyTex` lo replica.

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
- **CMPR (M5.7c)**: el decodificador de Bti.cpp se corrigió al framing GX/DXT1 estándar (8×8 = 4 subtiles de 8 bytes, tamaño w·h/2); el antiguo framing 8×4 leía la fila 3 fuera del subtile y duplicaba el tamaño. Ver §J.

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
| M5.1 | `GXInit`, estado espejo, `GXSetViewport/Scissor/Projection`, clear, vértices inmediatos, primer draw | ✅ |
| M5.2 | `GXSetVtxDesc/VtxAttrFmt/Array`, buffers dinámicos | ✅ |
| M5.3 | Texturas: carga BTI→Vulkan, samplers, `GXSetTexCoordGen2` | ✅ |
| M5.4 | TEV básico (`GXSetTevOp`, order, color/alpha in/op, konst, swaps), UBO dinámico + shader TEV | ✅ |
| M5.5 | Blend/depth/cull/alpha compare/fog | ✅ |
| M5.6 | Display lists (intérprete `GXCallDisplayList`) | ✅ |
| M5.7a | Chan lighting (`GXSetNumChans/ChanCtrl/ChanMat/AmbColor`, `GXLoadLightObjImm`, pos/nrm mtx, MATINDEX_A, routing DL de chan/matrices/luces/LOADINDX) | ✅ |
| M5.7b | Ind stages (espejo + warp en shader + RIDs ind) | ✅ |
| M5.7c | Copy EFB→textura/swapchain (`GXCopyTex`/`GXCopyDisp`, copy-state APIs, fórmulas SDK, EFB RT + blit/present) | ✅ |

Cada función nueva se añade con su fila en la tabla de inventario y su clase; ver `docs/porting.md` (flujo de trabajo para añadir GX).

### M5.1 — Estado espejo + vértices inmediatos (✅)

Modelo fiel al hardware (ver §2): los emisores de vértices del vendered
(`GXVert.h`, override PC) escriben a `GXWGFifo` — ahora un `GXFifoPipe` cuyas
palabras capturan cada write con su tipo (`GXCompatFifo.h`). `GXCompat.cpp`
reconstruye los vértices a partir del stream consumido **posicionalmente en
orden VCD** (exactamente como el vertex loader de la consola: los writes deben
seguir el orden del descriptor — pos → nrm → clr → tex), usando el VAT para el
número de componentes y el escalado (`S16` con frac n → `/2ⁿ`; colores `u8`
→ `/255`; `GXColor1u32` desempaqueta 4 bytes big-endian). El primitivo se
serializa a floats en VCD order y `flushDraw` lo envía a `Platform::Renderer`
(pipeline pos+color0, `kGxVertSpv/kGxFragSpv`, MVP = proyección GX en push
constant; `GX_QUADS` → 2 triángulos en CPU; buffer temporal liberado en
`GXCompatEndFrame`).

Pendiente M5.1 (documentado en `GXCompat.h`): el vertex shader consume solo
posición + color0 (normales/texcoords se capturan pero no se renderizan), y
los emisores deben escribirse en orden VCD (invariante de hardware).

Validación: 4 tests headless de captura (`gx_test.cpp`: VCD order, color
empaquetado `u32`, escalado de atributos, VCD con texcoord) + test offscreen
pixel-exacto `vulkan_offscreen_gx_quad` (mismo layout/shaders que `flushDraw`,
interpolación de color de vértice verificada en 4 cuadrantes) + demo bajo
Xvfb con `--gpu-debug` sin VUIDs.

### M5.3 — Texturas GX + texgen (✅)

- **Objetos de textura**: `GXInitTexObj`/`GXInitTexObjCI` guardan los datos GX
  crudos (tiled) del objeto; `GXLoadTexObj` los decodifica a RGBA8 con `Bti.cpp`
  (deswizzle hardware) y sube la textura al renderer (`createTexture`), creando
  también el sampler a partir de wrap/filtros GX (GX_CLAMP/REPEAT/MIRROR →
  ClampToEdge/Repeat/MirrorRepeat; filtros min/mag/mip → `SamplerDesc`).
  `GXInitTexObjLOD` guarda LOD/bias/anisotropía (mips TODO M5.x — se sube solo el
  nivel base). El estado del objeto se guarda en el `dummy[8]` del `GXTexObj` SDK
  (puntero), así el juego puede tenerlos embebidos donde sea. **TEXMAP0** alimenta
  el shader texturizado (`kGxTex*Spv`: texel × color de vértice, el "modulate" del
  TEV etapa 0); TEXMAP1..7 se guardan pero no se consumen (TEV real en M5.4).
- **BTI** (`Bti.h/.cpp`, puro y testeable): parse del header de 32 bytes (BE) y
  decodificadores por formato — I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR/C4/C8/
  C14X2 — con el swizzle 4x4 (RGBA8 en dos planos AR/GB, CMPR 8x4 con subtiles
  DXT1) y TLUT (IA8/RGB565/RGB5A3). Todos se decodifican a RGBA8 en carga
  (equivalencia visual; véase §5).
- **Texgen**: `GXSetTexCoordGen2` + `GXLoadTexMtxImm` se resuelven **en CPU** por
  vértice (matriz 2x4/3x4 sobre TEX0..7 o POS; `GX_IDENTITY` pasa UV sin tocar).
  BUMP*/SRTG, TLUT management y mipmaps quedan documentados como TODO (M5.7).
- Validación: 11 tests de decodificador BTI (pixel-exactos, `bti_test.cpp`) + 4 de
  texgen headless (matriz 2x4/3x4, identidad, passthrough) + `vulkan_offscreen_gx_
  textured_quad` (shaders GX texturizados + checkerboard 4×4, colores exactos por
  celda) + demo con textura procedimental RGB565 bajo Xvfb sin VUIDs.

### M5.4 — TEV básico (✅)

**APIs** (`GXTev.cpp`, espejo del `GXTev.c` vendered): `GXSetTevOp` (5 presets
`MODULATE/DECAL/BLEND/REPLACE/PASSCLR` con las tablas `TEVCOpTableST0/1` +
`TEVAOpTableST0/1`; etapa 0 → fuentes `RASC/RASA`, etapas ≥1 → `CPREV/APREV`),
`GXSetTevColorIn/AlphaIn/ColorOp/AlphaOp`, `GXSetTevColor`/`GXSetTevColorS10`
(registros `TEVPREV`/`C0..C2`, s10 clamp a [−1024,1023]),
`GXSetTevOrder` (con el mapeo `c2r[]` de canales, `GX_TEXMAP_NULL`/`GX_TEX_DISABLE`
deshabilitan la etapa y el mapa se enmascara ≥`GX_MAX_TEXMAP` → `GX_TEXMAP0`),
`GXSetTevKColor`/`GXSetTevKColorSel`/`GXSetTevKAlphaSel` (constantes K0..K3,
fracciones 1..1/8, componentes Kx_R..Kx_A; los selects 0x08–0x0B se tratan como
0 — interpretación Dolphin, ver §7), `GXSetTevSwapMode(Table)` (4 tablas de
swizzle RGBA) y `GXSetNumTevStages` (1..16, fuera de rango ignorado).

**Evaluación**: un solo fragment shader universal (`kGxTevVertSpv/kGxTevFragSpv`,
`tools/shaders/gx_tev_*.`) evalúa la cadena de hasta 16 etapas por píxel. El
estado TEV se serializa por draw en un **UBO std140 dinámico** (set 1, `TevUboData`
= 1296 B: `prev` inicial, `tevReg[3]`, `konst[4]`, `stage[16][4]` — colorEnv,
alphaEnv, opParams, reservado —, `header` = nº etapas/texgens, `swapTables[4]`,
más los extras de pixel engine M5.5: `fogColor/fogReg/fogAB/alphaCmp`),
subido al **arena UBO de 1 MiB** del renderer con stride 2048 (un región por
draw, offset dinámico en el bind; el cursor se resetea en `endFrame` tras el
fence). `flushDraw` pasa a un layout de vértice fijo (27 floats: pos3, clr0/1 4,
uv0..7 2 — los atributos ausentes en el VCD se rellenan: colores blanco,
texcoords 0), enlaza **las 8 TEXMAP** en set 0 (slots sin textura → textura
blanca 1×1 `ensureWhiteFallback`) y enlaza el set 1 con el offset dinámico.

**Fórmula por etapa** (verificada contra Dolphin `WriteTevRegular`, `gx.md §3`):
`out = ((D+bias) << sl) ± ((((A<<8) + (B−A)·(C+(C>>7))) << sl) + r) >> 8`, con
`sl` de escala (2/4), `r = 128` (ADD) / `127` (SUB), `bias = ±128` (ADDHALF/
SUBHALF), `DIVIDE_2` desplaza el total sin redondeo, clamp [0,255] o [−1024,1023].
Ops de comparación (`GX_TEV_COMP_*`): modo `(op>>1)&3` (R8/GR16/BGR24/RGB8) y
GT/EQ de `op&1`; en el combinador de alfa los modos R8/GR16/BGR24 comparan los
A/B del combinador de **color** (Dolphin). Entradas: `prev/c0/c1/c2`,
`textemp` (texel muestreado con la texcoord de la etapa, swizzle `texSel`),
`rastemp` (canal `c2r[colorChan]`, swizzle `rasSel`), `konsttemp` (KColorSel/
KAlphaSel). Quirks Dolphin aplicados: `prev` inicial = registro TEVPREV real;
texcoord de etapa ≥ nº texgens → texcoord 0; último stage con dest ≠ TEVPREV se
copia a prev al final (el resultado va a pantalla igualmente); si `GXSetNumTexGens(0)`
toda textura activa muestrea negro.

**Espejo CPU de referencia** (`evalTevChain`, `GXTevInternal.h`): implementa
exactamente la misma fórmula y semántica que el shader (lockstep), usada por los
tests para fijar la fórmula sin depender de la GPU.

Validación: 11 tests headless del combinador (`gx_tev_test.cpp`: presets por
etapa, ops color/alfa con bias/escala/clamp, routing de registros C0..C2 y
copiado final a prev, TEVPREV inicial, s10, constantes K, swap tables, semántica
de `GXSetTevOrder`, ops de comparación, límites de `GXSetNumTevStages`) + test
offscreen **multi-textura** `vulkan_offscreen_gx_tev_quad` (3 etapas: TEXMAP0
rojo → TEXMAP1 verde → KONST K0; resultado pixel-exacto (10,20,30,40); verifica
el layout std140 del UBO, la cadena multi-etapa, el muestreo de 2 texturas y las
constantes K) + demo con cadena de 2 etapas (MODULATE → lerp hacia K2) bajo
Xvfb `--gpu-debug`, 0 VUIDs. Total suite: 86 tests, 0 fallos.

Pendiente M5.4 (documentado): el índice de mapa se indexa con un if-chain de
índice estático en el shader (el renderer no habilita
`shaderSampledImageArrayDynamicIndexing`); mipmaps y bias de LOD no afectan al
muestreo (solo nivel base, M5.x); bump/chan lighting (`GX_ALPHA_BUMP*`) → ras 0
hasta M5.7.

### M5.5 — Blend / depth / cull / alpha compare / fog (✅)

**Pixel engine** (`GXCompat.cpp`, espejo del `GXPixel.c` vendered; el estado se
aplica en `flushDraw` → `PipelineDesc` del renderer):

- `GXSetCullMode` — mirror completo (`GX_CULL_NONE/BACK/FRONT/ALL`), default
  `GX_CULL_BACK`; `flushDraw` → `PipelineDesc.cullMode` (mapa
  `cullModeFromGx` → `VkCullModeFlags`).
- `GXSetBlendMode(mode, src, dst, logic)` — mirror completo. `GX_BM_NONE` no
  mezcla; `GX_BM_BLEND` activa blend con los factores (los 8 factores GX →
  `VkBlendFactor`, `blendFactorFromGx`; `GX_BL_SRCCLR` → `SRC_COLOR`,
  `GX_BL_INVSRCCLR` → `ONE_MINUS_SRC_COLOR`); `GX_BM_SUBTRACT` activa blend con
  `VkBlendOp::REVERSE_SUBTRACT` (GX resta *dst − src*); `GX_BM_LOGIC` desactiva
  blend y activa **logic op** (`GXLogicOp` 0–15 = `VkLogicOp` 1:1, Vulkan
  reemplaza el blending cuando `logicOpEnable`). GX aplica el mismo factor a
  RGBA (sin factor alfa separado): el slot alfa espeja el slot color.
- `GXSetZMode(test, func, write)` + `GXSetZCompLoc` — mirror completo. Default
  `TRUE/LEQUAL/TRUE`; `flushDraw` → `PipelineDesc.depthTest/depthWrite/
  depthCompare` (`compareFromGx` 1:1 `GXCompare` → `VkCompareOp`).
  `GXSetZCompLoc` (¿compare antes o después del alpha test?) se guarda en el
  mirror; hoy el shader siempre hace alpha-test antes de blend y no hay depth
  real en el pipeline GX → se aplica cuando llegue el depth real (M5.7, junto
  con el dither y el formato de EFB — ver `GXSetDither`/`GXSetPixelFmt`).
- `GXSetColorUpdate`/`GXSetAlphaUpdate` — mirror completo → `PipelineDesc.
  colorWrite/alphaWrite` (máscaras de escritura RGBA/A del attachment).
- `GXSetDstAlpha(enable, alpha)` — mirror completo → `PipelineDesc.dstAlphaEnable
  /dstAlphaValue`; los factores `GX_BL_DSTALPHA`/`GX_BL_INVDSTALPHA` se mapean a
  `CONSTANT_ALPHA`/`ONE_MINUS_CONSTANT_ALPHA` con `cb.blendConstants[3]` =
  `dstAlphaValue/255` cuando está habilitado (el DSTALPHA de GX es una constante
  del estado, no la alpha del destino).
- `GXSetDither` / `GXSetPixelFmt` — mirror de estado (defaults `ENABLE` /
  `RGB8_Z24`/`ZC_LINEAR`); el dither RGB8 y la conversión del formato de EFB se
  implementan en M5.7 con el pipeline de copia EFB→present.
- `GXSetAlphaCompare(comp0, ref0, logic, comp1, ref1)` — completo, **en el
  shader**: pack `comp0(3)|ref0(8)|logic(2)|comp1(3)|ref1(8)` en
  `TevUboData.alphaCmp[0]`; el fragment shader evalúa ambas comparaciones sobre
  la alpha de TEV (después del mask `&255`) y las combina con
  `GX_AOP_AND/OR/XOR/XNOR`; si falla → `discard`. Default `ALWAYS/0/AND/ALWAYS/0`
  (siempre pasa).
- `GXSetFog(type, startz, endz, nearz, farz, color)` — completo, espejo exacto
  del `GXPixel.c` vendered (ver fórmulas en `GXTev.cpp`/`gx.md §3`): `fsel =
  type&7`, `proj = (type>>3)&1`. Rama **ortográfica** (proj=1): `a =
  (farz−nearz)/(endz−startz)`, `c = (startz−nearz)/(endz−startz)`; B_MAG/B_SHF
  no se escriben (el SDK los deja). Rama **perspectiva** (proj=0): `A =
  farz·nearz/((farz−nearz)(endz−startz))`, `B = farz/(farz−nearz)`, `C =
  startz/(endz−startz)`, `B` normalizado a [0.5,1) con `b_m = B_mant·8388638`
  (u0.24) y `b_s = exp+1`, `a = A/2^b_s`, `c = C`. Los valores a/c se truncan al
  registro de 20 bits (mantisa 11 bits) **exactamente como el hardware**:
  `fogA = bit_cast<float>((sign<<31)|(exp<<23)|(mant<<12))` (Dolphin
  `FogParam0/3::FloatValue`, NaN → A=0 / C=±inf). El shader (Dolphin `WriteFog`):
  `zCoord = int((1−gl_FragCoord.z)·2²⁴)` (convención GX: profundidad invertida,
  cerca = 0xFFFFFF), persp `ze = A·2²⁴/(B_MAG−(Zs>>B_SHF))`, orto `ze = A·Zs/2²⁴`;
  `fog = clamp(ze−C, 0, 1)`; `fsel 4/5/6/7` = EXP / EXP2 / REVEXP / REVEXP2
  (fórmulas de Dolphin), resto lineal; `ifog = round(fog·256)`;
  `rgb = (rgb·(256−ifog) + fogColor·ifog)>>8` (el alfa no se modifica).
  `GXSetFogRangeAdj` guarda el estado; la tabla K y el ajuste por rango quedan
  para M5.7 (TODO en el shader y en `setFogRangeAdjState`).

**Renderer** (`Renderer.cpp`): `PipelineDesc` ampliado con
`cullMode/blendEnable/srcBlendFactor/dstBlendFactor/blendOp/logicOpEnable/logicOp/
dstAlphaEnable/dstAlphaValue/depthTest/depthWrite/depthCompare/colorWrite/
alphaWrite/depthFormat` (hash y `operator==` cubren todos los campos — el hash
se revalida por colisión en los tests `renderer_pipeline_desc_equality`).
Creación de pipeline: `rs.cullMode`; blend con factores/op + espejo alfa +
`blendConstants[3]`; logic op en `VkPipelineColorBlendStateCreateInfo` (no en el
attachment — error corregido en esta pasada); `ds.depthCompareOp` + depth
test/write forzado a off si `depthFormat == Undefined` (pipeline sin depth);
`renderingInfo.depthAttachmentFormat = textureFormatToVk(depthFormat)` cuando
hay depth. `passDepthFormat()`: swapchain → `Undefined`; render target → formato
de su depth attachment.

**Fragment shader M5.5** (`tools/shaders/gx_tev_frag.frag`, regenarado en
`vk_demo_shaders.h`): la salida del TEV pasa por el pipeline de pixel engine en
orden Dolphin: `prev &= 255` → alpha compare + `discard` → quirk hardware
"alpha == 1 no hace nada en blending" (`prev.a = 0` si `prev.a == 1`, incondicional
— replica Dolphin, solo afecta a blending) → fog (fsel 4–7) → salida. El quirk
es incondicional como en Dolphin; documentado para revisar si un juego concreto
se ve afectado.

**Tests M5.5** (suite: 91 tests, 0 fallos; ctest 1/1; demo `--gpu-debug --frames
5` 0 VUIDs):

- `gx_pixel_engine_state` (`gx_test.cpp`): defaults de `GXInit` (cull BACK, blend
  NONE/SRCALPHA/INVSRCALPHA/CLEAR, z TRUE/LEQUAL/TRUE, ZCompLoc TRUE, updates
  ENABLE, dstalpha DISABLE, dither ENABLE, RGB8_Z24/ZC_LINEAR) y mirror de
  `GXSetBlendMode` (BLEND/SUBTRACT/LOGIC), `GXSetZMode`, `GXSetZCompLoc`,
  `GXSetColorUpdate/AlphaUpdate`, `GXSetDstAlpha`, `GXSetDither`, `GXSetPixelFmt`
  vía `GXCompatDebugPeState`.
- `pe_alpha_compare_logic` (`gx_tev_test.cpp`): CPU reference
  (`evalPixelEngine`) con AND/OR/XOR/XNOR y las 4 combinaciones pass/fail.
- `pe_fog_ortho_linear` + `pe_fog_off`: CPU reference con `GXSetFog(ORTHO_LIN,
  2, 8, 1, 9, (255,128,64))` a `zCoord = 8388608` → `ifog = 128` →
  `(100,150,200)` → `(177,139,132)` (mezcla 50/50 con el color de niebla, alfa
  intacto); sin fog → pasa el TEV tal cual.
- `vulkan_offscreen_gx_fog_alpha` (`video_test.cpp`): **GPU == CPU end-to-end**.
  El quad usa el MVP identidad con z de vértice = 0.5 (¡Vulkan NDC depth [0,1],
  cerca = 0! con z=0 el `gl_FragCoord.z` es 0 y el `zCoord` GX invertido sale
  0xFFFFFF → niebla total; z=0.5 → `zCoord = 8388608`). Frame 1: alpha compare
  `GREATER(128)` pasa → color con niebla exacto vs `evalPixelEngine`. Frame 2:
  `LESS(128)` → `discard` → el quad deja el clear (0.1,0.1,0.15,1.0).

**Documentado para M5.7**: `GXSetZCompLoc`/`GXSetDither`/`GXSetPixelFmt` son
mirror de estado (el pipeline GX aún no tiene depth attachment real, dither RGB8
ni conversión de formato de EFB); fog range adjustment; la quirk "alpha==1"
replica Dolphin incondicionalmente.

### M5.2 — Arrays + atributos indexados + buffer dinámico (✅)

- `GXSetArray(attr, base, stride)` registra los buffers de atributos; con
  `GX_INDEX8/GX_INDEX16` en el VCD, la FIFO lleva **un índice por atributo**
  (emisores `GXPosition1x8/1x16`, `GXColor1x8/1x16`, `GXTexCoord1x8/1x16` — los
  emisores 1x16/Color1x faltaban en el vendered y se añadieron al override
  `GXVert.h`, documentado) y el dato se resuelve desde el array con el stride
  de `GXSetArray` y la conversión del VAT (comps, tipo, escalado `/2^frac`,
  colores `/255`). VCD mixto DIRECT+INDEX permitido; el flujo consumido
  posicionalmente en VCD order (hardware-fiel). `GXClearVtxDesc` y
  `GXSetVtxAttrFmtv` (lista terminada en `GX_VA_NULL`) implementadas.
- **Buffer dinámico del renderer** (`createDynamicBuffer`/
  `ensureBufferCapacity`/`updateDynamicBuffer`): host-visible, reutilizado
  entre primitivas y frames; crece por reasignación preservando el contenido y
  **retirando** la asignación vieja (se destruye en `endFrame` tras el fence,
  porque los draws de este frame aún la referencian). `flushDraw` acumula cada
  primitiva al final del buffer y dibuja con `draw(count, firstVertex=offset)`.
  Los buffers temporales por-primitiva de M5.1 desaparecen.
- Validación: 6 tests headless nuevos (`gx_test.cpp`: INDEX16 pos, VCD mixto
  DIRECT+INDEX8, stride de array > atributo, escalado S16 en array,
  `GXClearVtxDesc`, `GXSetVtxAttrFmtv`) + `vulkan_offscreen_gx_quad` ahora
  dibuja en dos `draw` con offsets (patrón multi-rango del buffer dinámico) +
  test `renderer_dynamic_buffer` (API del renderer con ventana SDL oculta:
  creación, crecimiento preservando el handle, bounds-check; SKIP sin
  display/ICD) + demo con quad INDEX16 bajo Xvfb, 0 VUIDs.

### M5.6 — Display lists: intérprete (✅)

**Decisión de diseño.** No se implementan `GXBeginDisplayList`/`GXEndDisplayList`:
en hardware real ambos solo capturan en memoria las escrituras GD/FIFO, y el
juego las genera con `GDInitGDLObj`/`GDSetCurrent`/`GDWrite*`/`GDFlushCurrToMem`
(el pipeline J3D `J3DDisplayListObj::beginDL/endDL`). En el portado las
escrituras GD ya van directas al espejo de estado, de modo que capturar no
tiene sentido: el "DL" es la propia secuencia de bytes que la app escribe en su
buffer (véase `J3DGD.cpp`, `OceanSphere.cpp`). Por eso el punto de entrada real
es el *reproductor*: `GXCallDisplayList(list, nbytes)`, que el juego llama en
`J3DDisplayListObj::callDL`. Además es el único punto donde el PC recibe una DL
capturada *fuera* del proceso (el formato binario FIFO es el del hardware).

**Intérprete FIFO** (`GXCompat.cpp`, `DlReader` + `dlRun` y handlers, en
`extern "C"` por la declaración vendered de `GXDispList.h`):

- Opcodes soportados (formato exacto del hardware, confirmado con el parser de
  Dolphin): CP `0x08` (sub-command + palabra BE), XF `0x10` (base address en
  bits 0–15, stream size `((cmd2>>16)&0xF)+1`, datos BE), BP `0x61`
  (registro + 24 bits de valor), primitivas `0x80–0xBF` (`vat = cmd&7`,
  `primitiva = (cmd&0x78)>>3`), `CALL_DL 0x40` (anidado, recursivo con
  contador de profundidad; address y size alineados `&~31`), y **LOADINDX
  `0x20–0x38`** (XF indexed loads, M5.7a).
- Sub-commands CP: `VCD_LO 0x50` / `VCD_HI 0x60` (descripciones de atributo →
  espejo `sVcd`), `VAT A/B/C 0x70/0x80/0x90+vtxfmt` (formato: comp/count/stride
  → `sVat[]`), `ARRAY_BASE 0xA0+idx` / `ARRAY_STRIDE 0xB0+idx` (actualizan
  punteros y strides por atributo, igual que `GXSetArray`), y `MATINDEX_A
  0x30` / `MATINDEX_B 0x40` (índices de matriz del vértice; M5.7a). Los
  sub-commands raros (TEX4CNT/FMT, etc.) se ignoran con warning, como el
  quirk de Dolphin.
- XF: se decodifican los registros de texgen (`0x1040+coord` + `0x1050+coord`
  dual), el estado de channel lighting y las matrices/luces (M5.7a):
  - Channel lighting (el formato que hornea J3DMatBlock en los DLs):
    `SETNUMCHAN 0x1009`, amb `0x100A/0x100B`, mat `0x100C/0x100D` (u32 RGBA,
    R=MSB) y chan control `0x100E–0x1011` (bits del registro XF COLOR0CNTRL:
    bit 0 mat src, bit 1 enable, bits 2–5 luces 0–3, bit 6 amb src, bits 7–8
    diff fn, bit 9 attn enable, bit 10 attn select, bits 11–14 luces 4–7;
    attn se decodifica SPEC=(0,0), SPOT=(1,1), NONE=(0,1)) → espejo
    `GXLight.cpp` (`dlSetNumChans`/`dlAmbColorReg`/`dlMatColorReg`/
    `dlChanCtrlReg`).
  - Matrices: pos `0x000–0x0FF` (`dlXfPosReg`; la matriz del slot k está en
    los regs `[12k,12k+12)` → espejo `sPosMtx[3k]` por id GX_PNMTX), normales
    `0x400–0x45F` (`dlXfNrmReg`; slot k en `0x400+9k` → `sNrmMtx[k]`), luces
    `0x600–0x67F` (`dlLightReg`; 16 regs por luz: 0–2 reservados, 3 = color
    RGBA, 4–6 a, 7–9 k, 10–12 pos, 13–15 dir — el layout que escribe
    `WriteLightObjPS`), y los índices `SETMATRIXINDA/B 0x1018/0x1019`
    (0x1018 → selección de matriz actual, igual que MATINDEX_A).
- **LOADINDX** (`0x20`=XF_A pos, `0x28`=XF_B nrm, `0x30`=XF_C tex, `0x38`=XF_D
  light): payload `u32 = (index<<16)|((size-1)<<12)|address`; el intérprete
  copia `size` floats desde el array CP `sArrays[attr]` (attr = `(cmd>>3)+8 +
  GX_VA_POS`, registrado por `GXSetArray(GX_POS/NRM_MTX_ARRAY, …)`) a la
  dirección XF indicada (pos `0x000+4*id` con el addr empaquetado `0xB000|
  slot*0x0C`; nrm `0x400+9*slot` con CNT 8) — el cuerpo exacto de
  `LoadIndexedXF` de Dolphin. Este es el pipeline PCPU/NCPU de J3D
  (`J3DSys::loadPosMtxIndx`/`loadNrmMtxIndx`); el pipeline estándar PNCPU usa
  escrituras XF imm (`0x10`) y ya estaba cubierto.
- BP: los registros TEV se enrutan al mismo espejo que la API (TREF 0x28–0x2F,
  TEVC/TEVA 0xC0/0xC1+2i, KSEL 0xF6+i, KREGISTERL/H 0xE0/0xE1+bit23) y el resto
  del estado de píxel (cull/blend/z/dither/dst-alpha/fog) vía `dlApplyBpTev` /
  `dlApplyBp` → espejos `sPe*`/`sTev`. Un `0x61 0xFE` (BP mask) desprotege para
  escribir el registro siguiente, igual que la API (`GXSet*` internos escriben
  con el bit de máscara).
- Vertices: `dlReadVertex` lee de los arrays según VCD/VAT activos (directos e
  indexados, INDEX8/16/32), emite `(pos, nrm, clr0, tex0..tex7)` al buffer
  dinámico y llama al mismo `flushDraw` que la vía API. Si no hay `GXBegin`
  activo se usa el vat/primitiva del propio byte de comando, y al terminar se
  cierra la primitiva pendiente.
- Protección: `pc` acotado por `nbytes`, `CALL_DL` con profundidad limitada,
  `stride/comp` validados antes de leer; lecturas fuera de rango → stop con
  warning (nunca segfault).

**GDBase para PC.** `GDWrite*` inline del SDK escriben en `__GDCurrentDL` que
provee `GDBase.c`. En el build PC se compila un parche
(`src/compat/patches/RVL_SDK/gd/GDBase.c`) cuya única diferencia es
`DCFlushRange` → no-op (la coherencia de caché la gestiona el hardware); el
resto es idéntico al original. Así el código del juego que captura DLs con la
API GD compila y funciona sin cambios, y los tests pueden construir DLs con los
mismos `GDWrite*`/`GDSetCurrent` que el juego.

- Validación: 5 tests headless nuevos en `gx_test.cpp` (`DlBuilder` construye
  DLs con `GDWriteCPCmd/BPCmd/XFCmd` + `GDSetCurrent`): triángulo directo con
  `GXCallDisplayList` en medio de `GXBegin`, triángulo indexado INDEX8 con
  `GXSetArray` + sub-commands `ARRAY_BASE/STRIDE`, estado PE desde BP igual al
  de la API (cull/blend/z/dither/dst-alpha), estado TEV desde BP igual al de la
  API (TREF/TEVC/TEVA/KSEL/KREGISTER con bit 23, alpha env APREV×4, swaps,
  `opParams`), y texgen XF (registro 0x1000 → mapa `sXfTex`). Suite global:
  **95 passed / 0 failed / 1 skipped** (skip = SDL headless, por diseño).

### M5.7b — Ind stages (✅)

Espejo completo del estado de indirect texturing de la Flipper y warp por
etapa TEV en el shader. Módulos: `src/compat/gx/GXInd.cpp` (mirror + setters +
decode DL + evaluador CPU de referencia) y `GXIndInternal.h` (interfaz
interna consumida por `GXTev.cpp`); `TevUboData` extendido en
`GXTevInternal.h`; warp en `tools/shaders/gx_tev_frag.frag`.

- **Mirror** (`GXInd`): 4 ind stages (`nind` en NBMP de genMode, bits 6–7) con
  order (texcoord/tmap por `GXSetTevIndOrder`), coord scale (S/T por stage,
  `GXSetTevIndCoordScale`), tex scale (`GXSetTevIndTexScale`), y por etapa TEV
  (`GXSetTevIndirect`): matrix slot, bias S/T, `GX_ITF_*` format, `GX_ITBA_*`
  alpha select, `GX_ITB_*` bump alpha, wrap S/T y añade/no-añade al texcoord.
  Matrices 2×3 por slot (`GXSetTevIndMtx`, `MTXA/B/C`).
- **Empaquetado UBO** (std140, ver `TevUboData`, `static_assert` 1584 bytes):
  `header.z` = nº de ind stages; `stage[i][3]` = pack TEVIND (matrix/bias/
  format/alpha/bump/wrap) por etapa TEV; `indParams[4]` = order + coord scale
  + tex scale por ind stage; `indMtx[6]` = 2 filas × 3 slots de las matrices
  (raw s9.2 en xyz, escala en w); `texDims[8][2]` = dims de los texmaps
  (rellenadas en `flushDraw` desde `GXLoadTexObj`). Se construye con
  `packIndTevUbo()` tras `buildTevUbo()`.
- **Warp en shader**: por etapa TEV con ind stage activo se pre-muestrea el
  ind map (según order/format/bias, formatos U8/U16/RGB565), se aplica
  `alphaSel` (A/R/G/B/8), la matriz (raw × `2^scaleExp`) y el texcoord se
  desplaza en texels con wrap S/T (`GX_TG_CLAMP/MIRROR/WRAP`, en modo add o
  reemplazo). Modo normal-map (`GX_ITM_SNC/SNCL/SS/SSL`): `TODO(PC_PORT)` —
  no aparece en los call sites reales (SMG `OceanBowl`/`OceanRingDrawer` usa
  solo `GX_ITM_OFF/OFF`), los tests y el evaluador CPU cubren los modos usados.
- **Diseño verificado contra Dolphin** (`BPMemory.h` `IND_MTX` y
  `PixelShaderManager::SetIndMatrixChanged`): cada registro `MTXA/B/C` lleva
  una **columna** de la matriz 2×3 (MA/MB=col0, MC/MD=col1, ME/MF=col2), y el
  factor de escala es `128·2^scaleExp` con `scaleExp = v6 − 0x11` (bits 22–23
  del 3er registro MTX escrito; Dolphin: `.w = 17 − scale`). El evaluador CPU
  de referencia (`evalIndirectWarp`) aplica exactamente la fórmula del shader:
  `offsetTexels = dot(m_fila, uv_byte) · 2^scaleExp` y lo verifica con
  calibración por texmap dims.
- **API**: `GXSetTevIndOrder/CoordScale/TexScale/Indirect/IndMtx/IndTexMtxScale`,
  `GXSetTevIndWarp`, `GXSetNumIndStages` (vía genMode) → espejo.
- **DL**: el intérprete enruta los RIDs indirectos horneados por J3D
  (`dlApplyBpTev`): `0x06–0x08` = `dlIndMtxReg` (columnas), `0x10–0x1F` =
  `dlTevIndirect(stage)`, `0x24` = `dlIref`, `0x25/0x26` = `dlIndTexScale`;
  genMode NBMP leído en `dlApplyGenMode`.
- Validación: 3 tests nuevos (`gx_ind_mirror_state`, `gx_ind_warp_evaluator`
  con calibración 256 texels exacta, `gx_dl_ind_stages` con una DL J3D real
  de agua SMG estilo `OceanBowl`). Suite: **106 passed / 0 failed / 1
  skipped**.

### M5.7a — Channel lighting, luces y matrices (✅)

La iluminación por canal de GX se evalúa **en CPU por vértice** (función pura,
Dolphin-exacta contra `VideoCommon/LightingShaderGen.cpp`) y sustituye el color
de vértice que el shader TEV consume como `RASC`/`APREV` — el pipeline de
shaders no cambia. Módulos: `src/compat/gx/GXLight.cpp` (mirror + evaluador),
`GXLightInternal.h` (interfaz interna), el patch `GXLight.c` vendered
(`GXInitLight*` escriben el `GXLightObj`; `GXLoadLightObjImm` lo lee).

- **Mirror**: 4 slots de canal (`GXChannelID & 3`), amb/mat por par (u8 RGBA),
  luces `LightParams[8]` (color, a/k, pos, dir), matrices pos `sPosMtx[32]`
  (3x4, indexadas por id GX_PNMTX 0,3,6,…) y nrm `sNrmMtx[32]` (3x3, slot
  `id/3`), matriz actual por `GXSetCurrentMtx` (= CP `MATINDEX_A`, bits 0–5).
- **Evaluador** (`computeChannelLighting`): por slot — material (REG/VTX),
  acumulador (amb REG/VTX o color), por luz de la máscara: `ldir` según
  `GX_AF_*` (SPEC/SPOT/NONE), attenuación cos/dist, diffuse `GX_DF_NONE/SIGN/
  CLAMP`, y el producto final fijo `out = (mat * (lacc + (lacc>>7))) >> 8` con
  `lacc` clamp 0..255. Quirk del SDK: `GX_AF_SPEC` fuerza `GX_DF_NONE`.
- **API**: `GXSetNumChans`, `GXSetChanCtrl`, `GXSetChanAmbColor`/
  `GXSetChanMatColor`, `GXLoadLightObjImm`, `GXLoadPosMtxImm`/
  `GXLoadNrmMtxImm`, `GXSetCurrentMtx` → espejo.
- **DL**: el intérprete enruta el formato horneado por J3D (XF `0x1009`/
  `0x100A–0x100D`/`0x100E–0x1011`, pos/nrm/luces por XF `0x10`, `MATINDEX_A`
  CP `0x30`, y `LOADINDX 0x20/0x28` para el pipeline PCPU/NCPU) — ver el
  bloque "Intérprete FIFO" de la sección M5.6.
- Validación: 3 tests DL nuevos + los tests de la API en `gx_test.cpp`
  (`gx_chan_lighting_basic/spot/mirror_state/light_mvp_build`). Suite: **103
  passed / 0 failed / 1 skipped**.
