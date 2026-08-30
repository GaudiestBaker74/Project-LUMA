# docs/renderer.md — Abstracción de renderer (diseño)

> Estado: M0 (diseño preliminar). Se refina en M4, cuando se valide contra el inventario GX (docs/gx.md).

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
- Los **uniforms** son "push/UBO por draw": el compat GX agrupa matrices/colores/constantes TEV en un bloque pequeño por draw.
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
- M5: generación de shaders TEV + conversión de texturas + display lists (a través de compat/gx).
- M9+: copy EFB→present con filtros, dithering opcional, y (post-M10) resolución interna variable.
