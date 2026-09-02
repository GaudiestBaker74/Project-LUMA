# docs/renderer.md — Abstracción de renderer (diseño)

> Estado: **M4 completo** (`src/platform/Renderer/`, el antiguo `src/platform/Video/` de M3 es
> su backend). M4.1: device/swapchain/present, passes, buffers, pipeline cache por hash de
> estado, uniforms por push constants, draw/drawIndexed. M4.2: texturas (staging upload +
> debug labels), samplers cacheados, render targets (color + profundidad Z24X8→
> D24_UNORM_S8_UINT), muestreo en fragment shader (array de combined image samplers por
> pipeline, `bindTexture`). M4.3: frame stats — GPU timestamps (query pool, auto-disable),
> CPU render time y `VK_EXT_memory_budget` (log FPS con vram). M5.2: **buffer dinámico** —
> `createDynamicBuffer`/`ensureBufferCapacity`/`updateDynamicBuffer` (host-visible,
> reutilizado entre frames, crecimiento con retiro de la asignación vieja hasta
> `endFrame`), que reemplaza los buffers temporales por-primitiva de compat/gx.
> M5.3: los shaders GX texturizados (`kGxTexVertSpv/kGxTexFragSpv`) usan el path de
> textura de M4.2 (set0/binding0, `bindTexture`) con `textureCount=1`.
> M5.4: **UBO dinámico por draw para el TEV** — `PipelineDesc.fragmentUbo` añade un
> set 1 con un `UNIFORM_BUFFER_DYNAMIC` (rango = stride del arena); el estado TEV se
> sube con `uploadFragmentUbo` a un **arena host-visible de 1 MiB** (stride 2048,
> un región por draw, offset dinámico en el bind; cursor reseteado en `endFrame`
> tras el fence) y las 8 texturas se enlazan con `bindFragmentTextures`
> (batch `vkUpdateDescriptorSets` + bind del set 0 del pipeline).
> M5.5: **estado de pixel engine en `PipelineDesc`** — cull mode, blend
> (factores/op con espejo alfa; logic op en `VkPipelineColorBlendStateCreateInfo`,
> que reemplaza el blending en Vulkan), depth test/write/compare +
> `depthFormat`/`passDepthFormat()` (pipeline sin depth → depth forzado off;
> `renderingInfo.depthAttachmentFormat` solo si hay depth), colorWrite/alphaWrite,
> y dstAlpha constante (`blendConstants[3]`). El fragment shader TEV evalúa el
> alpha compare y el fog (Dolphin WriteAlphaTest/WriteFog) por píxel.
> CPU time del render path y `VK_EXT_memory_budget` (opcional), logueados cada segundo
> (`cpu-render/gpu/vram`). El EFB real (render target 640×448/576 + `GXCopyDisp`) se conecta
> en M5 con el GX.

## 1. Principio

**No diseñamos un engine gráfico genérico.** El 100 % de la carga gráfica del juego llega por la API GX (docs/gx.md). El renderer expone exactamente lo que la traducción GX necesita:

- un **device** (Vulkan) con la lista de extensiones/capabilidades requeridas,
- **recursos**: textura, sampler, buffer (vertex/uniform/staging), render target,
- **pasadas de dibujo**: conjunto de estados (combinados) → pipeline Vulkan (caché),
- **ejecución**: acumulación de geometría + `draw`, subida de uniforms,
- **presentación**: render target "EFB" → swapchain,
- **debug**: labels, validation, timing, dumps (ver §5).

El renderer **no sabe qué es un "acteor" ni una "escena"**: eso lo decide el compat GX. Es una capa fina y explícita entre GX y Vulkan.

## 2. El modelo de datos: "estado GX → pipeline"

La unidad central es el **pipeline key**: un hash del estado GX relevante en el momento del draw:

```
PipelineKey = H(vertex_format, tev_state, blend, depth, cull, alpha_cmp, fog, chan_light, tex_samplers, ind_stages, color_update, alpha_update, dst_alpha, ...)
```

- `Renderer::getOrCreatePipeline(key)` → `PipelineHandle` (Vulkan pipeline + shaders generados).
- Los **shaders** se generan a partir del TEV (fragment) y del layout de vértices + iluminación (vertex). Se compilan una vez y se cachean; el juego reutiliza un número pequeño de combinaciones (los "modos canónicos" de SMG), así que la caché será estable.
- Los **draws** se agrupan por pipeline para minimizar cambios de estado.

## 3. API propuesta (boceto — se cierra en M4)

```cpp
namespace Platform::Renderer {

struct DeviceCreateInfo {
    bool enableValidation;      // validation layers
    bool enableDebugLabels;     // VK_EXT_debug_utils
    const char* appName;
};

class Device {
public:
    static Device* create(const DeviceCreateInfo&);
    void destroy();

    Texture*   createTexture(const TextureDesc&);
    Sampler*   createSampler(const SamplerDesc&);
    Buffer*    createBuffer(BufferUsage, size_t size, void* initialData);
    RenderTarget* createRenderTarget(uint32_t w, uint32_t h, Format, bool depth);
    Pipeline*  getOrCreatePipeline(const PipelineKey&);

    void beginFrame();
    void beginPass(RenderTarget*, const ClearValue&);
    void setViewport(...);  void setScissor(...);
    void bindPipeline(Pipeline*);
    void bindVertexBuffers(...);  void bindIndexBuffer(...);
    void bindUniforms(uint32_t set, const void* data, size_t size);
    void draw(uint32_t vertexCount, uint32_t instanceCount);
    void drawIndexed(...);
    void endPass();
    void endFrame();              // blit EFB → swapchain + present
    void waitIdle();
};

} // namespace Platform::Renderer
```

Observaciones:

- **Sin abstracción de "backend genérico"** de momento: `renderer.h` ES la API; el único backend es `vulkan/`. Si mañana llega D3D12, se introduce la interfaz virtual en ese momento (YAGNI ahora).
- Los **uniforms** son "push/UBO por draw": el compat GX agrupa matrices/colores/constantes TEV en un bloque pequeño por draw. El MVP es push constant (vertex); el estado TEV + pixel engine (1296 B std140, `TevUboData` — TEV + fog/alpha compare) va en un UBO dinámico por draw (`uploadFragmentUbo` sobre el arena de 1 MiB, `UNIFORM_BUFFER_DYNAMIC` en set 1 con offset dinámico por draw). El arena se resetea en `endFrame` tras el fence (one frame in flight); si se agota, `uploadFragmentUbo` devuelve `false` y `flushDraw` descarta el draw.
- Los **recursos se nombran** (debug labels) siempre que el device tenga `VK_EXT_debug_utils`.
- Los **render targets** son opacos: el renderer gestiona la conversión del formato GX de profundidad (Z24X8) a un formato Vulkan.

## 4. Bucle de presentación (sustituye a VI/XFB)

```
[GameSystem::frameLoop]                          [compat/vi]              [Renderer]
beginRender ────────────────► (estado de cámara/clear) ──► beginPass(EFB)
draw() ──► GXSet* + GXBegin ─────────────────────────────► draws al EFB (resolución interna fija)
endRender ─► GXCopyDisp ──► (retrace cb post) ───────────► endFrame(): blit EFB→swapchain + present
update() / calcAnim() ── simulación (timestep fijo 60 Hz)
waitForRetrace ──► vsync / espera de frame ───────────────► frame pacing
```

Detalles:

- **Resolución interna**: el EFB del juego es 640×448 (NTSC) / 640×576 (PAL) con el modo elegido en `RenderMode.cpp` (GXNtscIntDf, GXPalIntDf…). El port arranca con la resolución interna del modo y escala a la ventana (presentación). El "rendering a mayor resolución" es una mejora posterior (post-M10): implica reescribir partes que dependen de la resolución EFB (capturas de pantalla `ScreenPreserver`, filtros 2×).
- **Frame pacing**: simulación a timestep fijo; presentación al rate del monitor con vsync opcional. Los callbacks `VISetPreRetraceCallback/PostRetraceCallback` se invocan antes/después del present (semántica que espera JUTVideo/MainLoopFramework).
- **`GXCopyDisp`** se convierte en un blit EFB→backbuffer (con opción de filtro). La copia con `GX_SetCopyFilter` (filtro vertical) se puede simplificar en primera versión (equivalencia visual; documentar `TODO(PC_PORT)` si se aprecia diferencia).

## 5. Debugging desde el día uno

| Herramienta | Implementación |
|---|---|
| Validation layers | `VK_LAYER_KHRONOS_validation` si `enableValidation` (flag `--gpu-debug` / settings) |
| Logs | `Platform::Log` con niveles TRACE..FATAL; el compat GX loguea a TRACE cada llamada GX (o captura a fichero) |
| Frame timing | contadores de CPU por fase (sim, render, present) + GPU timestamps (VK_EXT_calibrated_timestamps / query pool) |
| Memoria GPU | `VK_EXT_memory_budget` → estadísticas por frame |
| Debug labels | `VK_EXT_debug_utils` en todos los recursos/pass (nombres: "EFB", "StarPointer", "ScreenWipe"…) |
| Dump del estado GX | `compat/gx` puede serializar el estado espejo completo a JSON (tool `--dump-gx`) |
| Captura de llamadas GX | log TRACE con filtro por función o por frame; útil para diffs entre Wii y PC |
| Captura de frames | opcional: integración con renderdoc (VK_LAYER_RENDERDOC) documentada, sin dependencia en runtime |

## 6. Dependencias Vulkan (M4)

- Vulkan SDK ≥ 1.3 (headers + `volk` como cargador dinámico recomendado — permite un solo binario sin require la SDK en runtime).
- Extensiones: `VK_KHR_swapchain`, `VK_EXT_debug_utils`, `VK_EXT_memory_budget` (opc.), `VK_KHR_dynamic_rendering` (evita render passes clásicos; compatible con todas las GPUs modernas).
- Target mínimo de hardware: cualquier GPU con Vulkan 1.1+ (Vulkan 1.3 preferido); Steam Deck (RADV) y GPUs NVIDIA/AMD/Intel en Windows/Linux.

## 7. Roadmap del renderer

- M3: device + swapchain + clear + triángulo (smoke test de la pila Vulkan).
- M4: API `renderer.h` cerrada + buffers/uniforms/viewport + pasadas con caché de pipelines básica + debug labels + frame timing.
- M5.2: buffer dinámico (host-visible, crecimiento con retiro diferido al `endFrame`). M5.x: generación de shaders TEV + conversión de texturas + display lists (a través de compat/gx).
- M9+: copy EFB→present con filtros, dithering opcional, y (post-M10) resolución interna variable.

## 8. Invariantes de validación Vulkan (M9.4)

Desde M9.4 la suite corre con `vulkan-validationlayers` instalado y display
(Xvfb + llvmpipe), así que los dos tests que antes se skipeaban sin ventana
(`renderer_dynamic_buffer`, `gx_copy_efb_present_and_readback`) se ejecutan y
`--boot --gpu-debug` debe dar **0 errores**. Los defectos que esa pasada
destapó (latentes desde M5.7c, invisibles sin capas de validación) fijan las
reglas siguientes:

- **Punteros de `VkSubmitInfo`**: todo lo que apunta `submit` (p. ej.
  `pWaitDstStageMask`) debe vivir hasta `vkQueueSubmit`. `endFrame()` declaraba
  `waitStage` dentro del `if (!mFrameAcquireConsumed)` → puntero colgante (UB:
  las capas veían un máscara de stages basura). Regla: declarar fuera del
  bloque.
- **Usage del swapchain**: `blitPassToSwapchain()` hace `vkCmdBlitImage` contra
  la imagen del swapchain → `sci.imageUsage` incluye `TRANSFER_DST` si
  `caps.supportedUsageFlags` lo permite (antes solo `COLOR_ATTACHMENT`).
- **Aspect en depth/stencil combinado**: sin `separateDepthStencilLayouts`,
  todo `VkImageMemoryBarrier` sobre D24S8 cubre `DEPTH_BIT|STENCIL_BIT`
  (VUID-VkImageMemoryBarrier-image-03320). El `VkImageView` del attachment sí
  puede exponer solo depth.
- **Dynamic state solo con el command buffer grabando**: `setViewport` /
  `setScissor` retornan sin grabar si `!mFrameRecording`. El juego llama
  `GXSetViewport` durante el init de escena (antes de `beginRender`); compat
  guarda el valor en su mirror y `flushDraw()` lo re-aplica dentro del pass
  (`sScissorSet` evita que el scissor por defecto `{0,0,0,0}` recorte todo).
- **Layout del color de un RT trackeado**: `GpuRenderTarget::colorLayout`
  refleja el layout real (los blit/readback dejan `TRANSFER_SRC_OPTIMAL`, no
  `COLOR_ATTACHMENT_OPTIMAL`). Las barreras de `blitPassToSwapchain` y
  `readRenderTarget` usan ese valor como `oldLayout` en vez de uno fijo; si no,
  `GXCopyTex` antes del primer pass del frame (frame B del test) validaba mal.
  `beginPass` sigue descartando contenido (`UNDEFINED → COLOR_ATTACHMENT`).
- **Present siempre en `PRESENT_SRC_KHR`**: `mSwapPresentReady` se resetea en
  `beginFrame()` y lo ponen `endPass()` (swapchain) y `blitPassToSwapchain()`.
  Un frame vacío/abortado (transición de escena sin pass ni blit) llegaba a
  `vkQueuePresentKHR` con la imagen en `UNDEFINED` → `endFrame()` graba una
  barrera de respaldo antes del submit.

Ambos son también requisitos para la máquina del usuario (NVIDIA/Windows con
Vulkan SDK): el boot con `--gpu-debug` debe seguir en 0 VUIDs.
