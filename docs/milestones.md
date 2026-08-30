# docs/milestones.md — Roadmap del port

> Regla de oro: cada milestone termina **compilando, enlazando y con `ctest` verde**. Nada de "casi hecho".

---

## M0 — Análisis ✅ (completado)

- [x] Clonar y analizar Petari (entry point, inicialización, dependencias Wii, inventario GX/OS/VI/DVD/WPAD/NAND).
- [x] Redactar `docs/architecture.md`, `gx.md`, `renderer.md`, `build.md`, `porting.md`, este roadmap.
- [x] Decidir estructura de directorios y ADRs (architecture.md §11–12).
- [ ] **Pendiente de confirmación del usuario** antes de tocar código.

## M1 — Build PC + núcleo de plataforma (Linux) ← *próximo*

**Objetivo:** demostrar que el proyecto compila y ejecuta en Linux con el toolchain host, y dejar la base de plataforma con tests.

Alcance:
- CMake `-DPLATFORM=PC` (Linux, GCC/Clang, C++20, ninja) + `ctest`.
- `Platform::Log` (niveles TRACE..FATAL, sinks consola/fichero, timestamps).
- `Platform::Memory` (asignación bruta + contadores para estadísticas).
- `Platform::Filesystem` (lectura de directorios, paths, VFS raíz).
- `Platform::Timing` (reloj de alta resolución, ticks, conversión).
- `Platform::Threading` (hilos, mutex, cola de mensajes — base del `compat/os`).
- Tests: filesystem, memory, endian helpers, log.
- **Smoke-test real:** compilar y ejecutar **un módulo de Petari en host** (candidatos: `JSystem/JKernel/JKRExpHeap.cpp` + dependencias mínimas, o `Game/Util/MathUtil.cpp` sin el asm) para destapar quirks de CodeWarrior/ABI lo antes posible. Si el fichero requiere el compat OS, se añade lo mínimo (p. ej. `OSReport` → Log).

Criterio de éxito:
- `cmake -B build -DPLATFORM=PC && cmake --build build && ctest` en verde en Linux.
- Un ejecutable `galaxy-pc` que arranca, loguea a INFO/DEBUG y ejecuta los tests del módulo portado (p. ej. allocar/deallocar en JKRExpHeap y verificar).
- Documentar en `porting.md` los primeros quirks encontrados.

Fuera de alcance: GX, ventana, input, audio (M3+).

## M2 — Windows ✅ (completado)

- [x] Mismo código compila con MSVC 2022 (x64); `#ifdef` solo en `platform/windows/`.
- [x] CI GitHub Actions (`.github/workflows/ci.yml`): Linux (GCC) + Windows (MSVC) con `ctest` + aarch64 cross-compile.
- [x] *(Opcional)* build aarch64 Linux (Steam Deck) verificado (cross-compile, solo compilación).
- Verificación hecha: build cruzado MinGW-w64 OK (compila contra `windows.h` real) y tests 36/36 bajo Wine en el `.exe` de Windows; **build MSVC 2022 real en verde por el usuario (build + `ctest` OK)**; demo M1 completa también bajo Wine.

Notas de portabilidad descubiertas en M2 (ver `porting.md`):
- `std::thread::id` se formatea en hexadecimal en Windows (puntero) y decimal en Linux — no parsear su stream; copiar sus bytes.
- `Inline.hpp` del vendered usa `__attribute__` sin guarda y el juego lo usa en posición postfija (`int f() NO_INLINE`) — MSVC no acepta atributos postfijos → el override expande a vacío en MSVC (noinline es solo optimización) y a `__attribute__` en GCC/Clang, con `#ifndef` para no pisar el fallback de `types.h` (C4005).
- MSVC resuelve `#include "..."` con la cadena completa de includes → los overrides "paraguas" (p. ej. `revolution/gx.h`) deben usar `<...>` con ángulos.
- `std::atomic<Stats>` no lock-free necesita `libatomic` en GCC/MinGW (no en MSVC).
- MSVC necesita `/EHsc` (semántica de excepciones) para el STL — añadido en el CMake.

## M3 — Ventana + Vulkan + bucle de juego ✅ (completado)

- [x] SDL3: ventana (`Platform::Window`, resizable, fullscreen con F11, `SDL_WINDOW_HIGH_PIXEL_DENSITY`), vsync FIFO / `--no-vsync` IMMEDIATE.
- [x] Vulkan vía volk (sin enlazar vulkan-1): instance con extensiones SDL, surface, device 1.3 con dynamic rendering, swapchain recreada en resize/out-of-date, 1 frame in flight (fence + 2 semáforos).
- [x] Validation layers + debug messenger (`--gpu-debug`; warn si no está instalada) — **0 VUIDs**.
- [x] Bucle de juego: timestep fijo 60 Hz con accumulator + clamp 0,25 s; delta correcto; cierre limpio (Esc/cierre de ventana → shutdown ordenado). `--frames N` para run automatizado.
- [x] Input mínimo SDL3 como `Platform::Input` (quit, F11 edge-triggered, flechas/espacio level-triggered).
- [x] Demo: triángulo con SPIR-V embebido (regenerable con `tools/compile_shaders.sh`), rotación por step fijo, FPS por `Platform::Log`.
- [x] Primer olor GX: `compat/gx/GXCompat` (`GXInit`, `GXSetViewport`, `GXClearColor`, `GXClear`) dibujando el fondo.
- [x] Test Vulkan headless en CI (offscreen + readback de píxeles; SKIP sin ICD) + smoke test de la demo bajo Xvfb con validación.

Verificación hecha:
- **Linux (sandbox, lavapipe)**: build OK, `ctest` 37/37, demo bajo Xvfb a 800+ fps sin VUIDs con `--gpu-debug`, salida limpia.
- **Windows (MSVC, máquina del usuario — RTX 2060)**: build + `ctest` + demo OK. La validación cazó un bug real que lavapipe no reproduce: **reutilización del semáforo `renderFinished`** (VUID-vkQueueSubmit-pSignalSemaphores-00067) con swapchain de 3 imágenes → fix con **un semáforo por imagen** (`mRenderFinished[imageIndex]`, ligado al ciclo de vida del swapchain). Documentado en porting.md.
- Quirks encontrados en el camino: `SDL_STATIC` hay que activarlo explícitamente en Windows (SDL3 por defecto solo hace shared) y el `find_package(Vulkan)` pide headers+loader (no existe componente `headers`); ambos con mensaje de error amigable en el CMake.

## M4 — Renderer (API cerrada) ← *próximo*

**Objetivo:** convertir la pila Vulkan de M3 (device/swapchain/present) en la API cerrada `Platform::Renderer` de `docs/renderer.md`: la capa fina entre GX y Vulkan. `Platform::Video` desaparece como módulo público (su código pasa a ser el backend de Renderer).

Sub-hitos (cada uno termina compilando + `ctest` verde):
- **M4.1 — Núcleo ✅**: `Renderer` absorbe device/swapchain/present de Video (con los fixes de M3); API de frames y passes (`beginFrame/endFrame/beginPass/endPass`, `setViewport/setScissor`), `createBuffer`/`bindVertexBuffer`, pipeline con **caché por hash de estado** (`getOrCreatePipeline`), uniforms vía **push constants**, `draw`/`drawIndexed`. Demo y GXCompat migrados a la nueva API; rotación del demo vía uniform (push constant) con shaders regenerados. Validado: ctest 40/40 en Linux (incluye 3 tests nuevos de formatos/equality), demo bajo Xvfb con lavapipe sin VUIDs, pixel-check del frame (fondo = GXClearColor pulsante, triángulo dibujado). Quirks nuevos en porting.md: hay que habilitar `VK_KHR_swapchain` en el device (si no, volk deja NULL los entry points del swapchain) y `pickPhysicalDevice` debe aceptar out-params null (test headless).
- **M4.2 — Recursos ✅**: `createTexture` (subida vía staging → `SHADER_READ_ONLY`, debug labels), `createSampler` (caché por `SamplerDesc`), `createRenderTarget` (color + profundidad, Z24X8→`D24_UNORM_S8_UINT`, el futuro EFB), `beginPass(rt)` con depth attachment, y **muestreo de texturas en el fragment shader** (descriptor set por pipeline, `bindTexture` sobre el array `set0/binding0`). Validado: ctest **44/44** en Linux — el test `vulkan_offscreen_textured_quad` renderiza un quad con un checkerboard 4×4 subido por staging y verifica los colores exactos pixel a pixel; la demo sigue sin VUIDs con `--gpu-debug`. Quirk nuevo en porting.md: el header público no puede usar tipos volk ni en métodos privados (firmas opacas), y un `friend struct` declarado en namespace anónimo NO coincide con la declaración del header — hay que forward-declararlo fuera de la clase y definirlo en namespace nombrado.
- **M4.3 — Timing y presupuesto ✅**: GPU timestamps (`VK_QUERY_TYPE_TIMESTAMP`, 2 queries por frame, auto-deshabilitados si `timestampComputeAndGraphics` es falso) + CPU time de la fase de render (Timing) + `VK_EXT_memory_budget` (opcional: se habilita solo si la extensión existe). El log de FPS muestra `cpu-render X ms | gpu Y ms | vram U/B MB` cada segundo. Helper puro `gpuTicksToMs` testeable sin device. Validado: ctest **45/45**, demo con lavapipe reportando timestamps (period 1 ns), gpu ~1.7 ms y presupuesto VRAM real, 0 VUIDs.
- **Tests M4**: conversión de formatos (tabla pura, sin Vulkan), pipeline cache (misma desc → mismo handle), offscreen draw con la API (píxeles verificados), y los tests existentes siguen en verde.

Criterio de éxito M4: **cumplido** — la demo M3 sigue idéntica a través de la API nueva; ctest verde (45/45 en Linux; Windows pendiente de validar por el usuario); `--gpu-debug` sin VUIDs.

## M4 — Renderer (API cerrada)

- `Platform::Renderer` según `docs/renderer.md`: device, recursos (textura/sampler/buffer/RT), pipeline con **caché por hash de estado**, viewport/scissor, uniforms, draw/indexed, present EFB→swapchain.
- Frame timing (CPU+GPU), memory budget, debug labels completos.
- Tests: conversión de formatos de textura GX→Vulkan, matrices.

## M5 — compat/gx (iterativo)

- Sub-hitos según `docs/gx.md` §7 (M5.1…M5.7): estado espejo, vértices, texturas, TEV, pixel pipeline, display lists, ind stages.
- Cada sub-hito con un test de dibujo (render a offscreen y verificar píxeles, o dump GX comparado).
- Herramientas: `--dump-gx` (serializa estado GX), `--gx-log` (captura TRACE de llamadas).

## M6 — Input completo

- Gamepad (Xbox, DualSense/DualShock vía SDL), teclado, ratón; mapeo Wiimote→PC (`docs/porting.md` §5).
- Config de input editable sin recompilar.
- *(Opcional)* Wiimote/Nunchuk real por Bluetooth detrás de `Platform::Input`.

## M7 — VFS + assets

- VFS definitivo: paths DVD (`/StageData/...`) → árbol `assets/` del usuario; caché de lecturas; futura fuente empaquetada.
- `tools/verify-assets`: comprueba que la extracción está completa (existencia de archivos clave, cabeceras) sin incluir contenido.
- **Documentación al usuario:** qué extraer (con Dolphin, como documenta Petari) y dónde colocar cada cosa; restricciones legales.

## M8 — Audio

- `compat/ax` + mezclador final en CPU + `Platform::Audio` (SDL3).
- Carga de BNK/onda desde VFS; BGM y SE funcionando.
- Micrófono/speaker → stub documentado `TODO(PC_PORT)`.

## M9 — Primer boot real 🎯 *(primer hito de éxito del proyecto)*

- `galaxy-pc` → inicialización de GameSystem → **Logo → título → una escena con Mario renderizado**.
- Requiere: compat/os, dvd, vi, gx (subset), filesystem VFS, frame control, escena "Logo".
- Audio puede seguir en stub; NWC24 en stub.

## M10 — Primera galaxia jugable

- Cargar una galaxia completa: geometría (J3D/BMD), texturas, iluminación, cámaras, Mario (física/gravedad), enemigos, colisiones, UI (nw4r lyt), guardado.
- Audio (M8) integrado.

## M11 — Juego completo

- Ampliar compatibilidad hasta que el juego entero sea jugable de principio a fin (todas las galaxias, jefes, demos, minijuegos, Luigi, etc.), pendiente del progreso de decompilación upstream.
- Rendimiento (Steam Deck: 60 fps), opciones de resolución interna, pulido.

---

## Dependencias entre hitos (resumen)

```
M1 ──► M2 ──► M3 ──► M4 ──► M5 ──┐
                                 ├──► M9 ──► M10 ──► M11
M6 ──────────────────────────────┤
M7 ──► M9 (necesario para assets) │
M8 ──────────────────────────────┘   (M8 puede ir tras M9; antes solo stub)
```

## Qué puede adelantarse / atrasarse

- **M7 (VFS) se puede adelantar a M1.5**: no depende de nada y desbloquea probar parsers de assets reales (sin render).
- **M6 (input)** se puede aplazar hasta después de M9 si el boot se hace solo con teclado (mínimo en M3).
- **M8 (audio)** es el módulo más independiente; puede ir paralelo.
