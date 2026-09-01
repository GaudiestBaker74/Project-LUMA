# docs/milestones.md — Roadmap del port

> Regla de oro: cada milestone termina **compilando, enlazando y con `ctest` verde**. Nada de "casi hecho".

---

## M0 — Análisis ✅ (completado)

- [x] Clonar y analizar Petari (entry point, inicialización, dependencias Wii, inventario GX/OS/VI/DVD/WPAD/NAND).
- [x] Redactar `docs/architecture.md`, `gx.md`, `renderer.md`, `build.md`, `porting.md`, este roadmap.
- [x] Decidir estructura de directorios y ADRs (architecture.md §11–12).
- [ ] **Pendiente de confirmación del usuario** antes de tocar código.

## M1 — Build PC + núcleo de plataforma (Linux) ✅ (completado)

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

## M4 — Renderer (API cerrada) ✅ (completado)

**Objetivo:** convertir la pila Vulkan de M3 (device/swapchain/present) en la API cerrada `Platform::Renderer` de `docs/renderer.md`: la capa fina entre GX y Vulkan. `Platform::Video` desaparece como módulo público (su código pasa a ser el backend de Renderer).

Sub-hitos (cada uno termina compilando + `ctest` verde):
- **M4.1 — Núcleo ✅**: `Renderer` absorbe device/swapchain/present de Video (con los fixes de M3); API de frames y passes (`beginFrame/endFrame/beginPass/endPass`, `setViewport/setScissor`), `createBuffer`/`bindVertexBuffer`, pipeline con **caché por hash de estado** (`getOrCreatePipeline`), uniforms vía **push constants**, `draw`/`drawIndexed`. Demo y GXCompat migrados a la nueva API; rotación del demo vía uniform (push constant) con shaders regenerados. Validado: ctest 40/40 en Linux (incluye 3 tests nuevos de formatos/equality), demo bajo Xvfb con lavapipe sin VUIDs, pixel-check del frame (fondo = GXClearColor pulsante, triángulo dibujado). Quirks nuevos en porting.md: hay que habilitar `VK_KHR_swapchain` en el device (si no, volk deja NULL los entry points del swapchain) y `pickPhysicalDevice` debe aceptar out-params null (test headless).
- **M4.2 — Recursos ✅**: `createTexture` (subida vía staging → `SHADER_READ_ONLY`, debug labels), `createSampler` (caché por `SamplerDesc`), `createRenderTarget` (color + profundidad, Z24X8→`D24_UNORM_S8_UINT`, el futuro EFB), `beginPass(rt)` con depth attachment, y **muestreo de texturas en el fragment shader** (descriptor set por pipeline, `bindTexture` sobre el array `set0/binding0`). Validado: ctest **44/44** en Linux — el test `vulkan_offscreen_textured_quad` renderiza un quad con un checkerboard 4×4 subido por staging y verifica los colores exactos pixel a pixel; la demo sigue sin VUIDs con `--gpu-debug`. Quirk nuevo en porting.md: el header público no puede usar tipos volk ni en métodos privados (firmas opacas), y un `friend struct` declarado en namespace anónimo NO coincide con la declaración del header — hay que forward-declararlo fuera de la clase y definirlo en namespace nombrado.
- **M4.3 — Timing y presupuesto ✅**: GPU timestamps (`VK_QUERY_TYPE_TIMESTAMP`, 2 queries por frame, auto-deshabilitados si `timestampComputeAndGraphics` es falso) + CPU time de la fase de render (Timing) + `VK_EXT_memory_budget` (opcional: se habilita solo si la extensión existe). El log de FPS muestra `cpu-render X ms | gpu Y ms | vram U/B MB` cada segundo. Helper puro `gpuTicksToMs` testeable sin device. Validado: ctest **45/45**, demo con lavapipe reportando timestamps (period 1 ns), gpu ~1.7 ms y presupuesto VRAM real, 0 VUIDs.
- **Tests M4**: conversión de formatos (tabla pura, sin Vulkan), pipeline cache (misma desc → mismo handle), offscreen draw con la API (píxeles verificados), y los tests existentes siguen en verde.

Criterio de éxito M4: **cumplido y validado en ambas plataformas** — la demo M3 sigue idéntica a través de la API nueva; ctest 45/45 en Linux; **Windows (RTX 2060, MSVC) validado por el usuario**: demo con `--gpu-debug` sin VUIDs, GPU timestamps activos (period 1 ns), `vram 43/5187 MB` real, fps clavado al vsync de 144 Hz y cierre limpio.

## M5 — compat/gx (iterativo) ✅

Sub-hitos según `docs/gx.md` §7, cada uno con test de dibujo (offscreen + píxeles verificados):

- **M5.1 — Estado espejo + vértices inmediatos ✅**: `GXInit` real, estado espejo completo
  (VCD/VAT por `GX_VTXFMT0..7`, proyección, viewport/scissor, cull/blend, clear),
  `GXSetVtxDesc/VtxAttrFmt`, `GXBegin/GXEnd`, emisores (`GXPosition/Color/Normal/TexCoord`)
  vía la **FIFO simulada** (`GXCompatFifo.h`: `GXWGFifo` es un `GXFifoPipe` que captura cada
  write con su tipo; `GXVert.h` vendered byte-for-byte, solo cambia el tipo del pipe), vértices
  reconstruidos **en VCD order** (hardware-fiel) y primer draw con color de vértice — la demo
  del triángulo es ahora un **quad GX real** rotando (`GX_QUADS` → 2 triángulos, shaders
  `kGxVertSpv/kGxFragSpv`, MVP = proyección GX). Bugs cazados en ruta: snapshot de stride en
  `flushDraw` tras el cálculo (no antes), `u32` de color sin pasar por `float` (pierde bytes),
  y enums `GX_*` de componentes con valores numéricos compartidos entre grupos (no se pueden
  switchear globalmente). Validado: ctest **50/50** en Linux — 4 tests headless de captura
  (`gx_test.cpp`) + `vulkan_offscreen_gx_quad` (píxeles verificados, interpolación de color de
  vértice en 4 cuadrantes) + demo `--gpu-debug` sin VUIDs (120 frames, llvmpipe).
- **M5.2 — VtxDesc/AttrFmt/Array + buffers dinámicos ✅**: atributos INDEX8/16
  (`GXSetArray` + emisores `GXPosition1x8/1x16`, `GXColor1x8/1x16`,
  `GXTexCoord1x8/1x16` — los 1x16/Color1x se añadieron al override del vendered,
  documentado), resolución de índices desde los arrays con stride + escalado VAT,
  VCD mixto DIRECT+INDEX, `GXClearVtxDesc` y `GXSetVtxAttrFmtv`. El acumulador de
  vértices es ahora un **vertex buffer dinámico del renderer**
  (`createDynamicBuffer`/`ensureBufferCapacity`/`updateDynamicBuffer`): host-visible,
  reutilizado entre frames, crece preservando contenido y retira la asignación vieja
  hasta `endFrame` (fence). La demo usa el quad con **INDEX16/INDEX8**. Bugs/quirk
  cazados: `SDL_Init` de SDL3 devuelve `bool` (true=éxito, invertido respecto a SDL2)
  → el test de buffer dinámico hacía SKIP siempre (quirk 17 en porting.md).
  Validado: ctest **57/57** bajo Xvfb (4 tests Vulkan ejecutándose, incluido el
  nuevo `renderer_dynamic_buffer`; SKIP limpio sin display), demo `--gpu-debug` sin
  VUIDs (300 frames).
- **M5.3 — Texturas ✅**: objetos de textura GX (`GXInitTexObj/LOD/CI/Tlut`,
  `GXLoadTexObj` → TEXMAP0..7), **decodificador BTI** puro (`Bti.h/.cpp`: parse del
  header BE + deswizzle 4x4/8x4 a RGBA8 de I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR/
  C4/C8/C14X2 con TLUT), samplers GX→`SamplerDesc` (wrap/filtros), y
  `GXSetTexCoordGen2`/`GXLoadTexMtxImm` resueltos **en CPU** por vértice (matriz
  2x4/3x4 sobre TEX/POS, `GX_IDENTITY` passthrough). Shaders `kGxTexVertSpv/
  kGxTexFragSpv` (texel × color de vértice; TEXMAP0). Nota: `GXInit` resetea
  también el estado de texturas (texgen/matrices/texmaps) — los tests headless
  cazaron el estado stale entre tests. Validado: ctest **74/74** bajo Xvfb (11
  tests BTI pixel-exactos + 4 texgen + `vulkan_offscreen_gx_textured_quad` con
  checkerboard verificado), demo con textura RGB565 procedimental sin VUIDs.
  TODO documentado (M5.7): mipmaps/LOD en sampler, TLUT vía `GXInitTexObjTlut`,
  BUMP*/SRTG (TEXMAP1..7 ya se consumen en el TEV, M5.4).
- **M5.4 — TEV básico ✅**: APIs TEV reales vendered (`GXSetTevOp` con las tablas
  ST0/ST1, `GXSetTevColorIn/AlphaIn/ColorOp/AlphaOp`, `GXSetTevColor/S10`,
  `GXSetTevOrder` con el mapeo `c2r[]`, `GXSetTevKColor/KColorSel/KAlphaSel`,
  `GXSetTevSwapMode(Table)`, `GXSetNumTevStages`). El combinador (≤16 etapas)
  vive en un fragment shader universal `kGxTevVertSpv/kGxTevFragSpv` (fórmula
  verificada contra Dolphin `WriteTevRegular`, incl. ops de comparación y
  quirks: prev inicial = registro TEVPREV, texcoord ≥ nº texgens → 0, último
  dest ≠ PREV copiado a prev, 0 texgens → negro) con el estado en un **UBO
  std140 dinámico** (`TevUboData`, 1296 B — incluye los extras de pixel engine
  de M5.5) subido al nuevo **arena UBO de 1 MiB**
  del renderer (`PipelineDesc.fragmentUbo` + set 1 `UNIFORM_BUFFER_DYNAMIC`,
  stride 2048, offset dinámico por draw, cursor reset en `endFrame`). El
  muestreo de TEXMAP0..7 en set 0 (`bindFragmentTextures`; slots sin textura →
  fallback blanco 1×1). Layout de vértice fijo de 27 floats (pos, clr0, clr1,
  uv0..7) con relleno de atributos ausentes. `GXTev.cpp` incluye un **evaluador
  CPU de referencia** (`evalTevChain`) en lockstep con el shader para los tests.
  Validado: ctest **86/86** bajo Xvfb (11 tests del combinador pixel-exactos
  `gx_tev_test.cpp` + `vulkan_offscreen_gx_tev_quad` multi-textura: TEXMAP0
  rojo → TEXMAP1 verde → KONST K0, resultado (10,20,30,40) — verifica el layout
  std140, la cadena y las constantes), demo con cadena de 2 etapas (MODULATE →
  lerp hacia K2) bajo `--gpu-debug` sin VUIDs. Nota: el índice de mapa se
  resuelve con un if-chain de índice estático (sin
  `shaderSampledImageArrayDynamicIndexing`); bump/chan lighting → ras 0 hasta
  M5.7 (ind stages).
- **M5.5 — Blend/depth/cull/alpha compare/fog ✅**: pixel engine espejo del
  `GXPixel.c` vendered en `GXCompat.cpp` (`GXSetCullMode` completo,
  `GXSetBlendMode` completo con los 8 factores + `GX_BM_SUBTRACT` →
  `REVERSE_SUBTRACT` + `GX_BM_LOGIC` → logic op 1:1 Vulkan, `GXSetZMode`/
  `GXSetZCompLoc`/`GXSetColorUpdate`/`GXSetAlphaUpdate`/`GXSetDstAlpha`/
  `GXSetDither`/`GXSetPixelFmt`; `GXSetAlphaCompare` y `GXSetFog` completos,
  este último espejo exacto de las fórmulas del SDK con truncado de registro de
  20 bits). `PipelineDesc` ampliado (cull, blend factors/op + espejo alfa, logic
  op en `VkPipelineColorBlendStateCreateInfo` — no en el attachment, depth
  test/write/compare + `depthFormat`/`passDepthFormat()`, colorWrite/alphaWrite,
  dstAlpha const). El fragment shader TEV gana la etapa de pixel engine (mask
  &255 → alpha compare + discard → quirk "alpha==1" → fog fsel 4–7, Dolphin
  `WriteFog`/`WriteAlphaTest`); zCoord GX invertido desde `gl_FragCoord.z`
  (¡Vulkan NDC depth [0,1]! z de vértice 0.5 → `zCoord = 8388608`). Validado:
  ctest **91/91** (test de estado `gx_pixel_engine_state`, CPU reference
  `pe_alpha_compare_logic`/`pe_fog_ortho_linear`/`pe_fog_off`, y
  `vulkan_offscreen_gx_fog_alpha` GPU==CPU: niebla 50/50 exacta y discard por
  alpha compare), demo `--gpu-debug --frames 5` 0 VUIDs. Documentado para M5.7:
  `GXSetZCompLoc`/`GXSetDither`/`GXSetPixelFmt` como mirror de estado (EFB
  format/dither/depth real en el pipeline de copia), fog range adjustment.
- **M5.6 — Display lists** (intérprete).
- **M5.7a — Chan lighting ✅** (`GXSetNumChans/ChanCtrl/ChanMatColor/ChanAmbColor`, `GXLoadLightObjImm`, pos/nrm mtx, MATINDEX_A, routing de DL; ver gx.md tabla §7).
- **M5.7b — Ind stages ✅** (espejo + warp en shader + RIDs ind; ver gx.md tabla §7).
- **M5.7c — Copy EFB→swapchain/textura ✅**: el EFB es un **render target Vulkan** creado perezosamente (`GXSetDispCopySrc` size, default 640×448, formato = swapchain) en `gx/GXCopy.cpp`; `GXCopyDisp` = `Renderer::blitPassToSwapchain` (blit EFB→imagen del swapchain, escalado LINEAR, present en `endFrame`) — el triple buffer XFB/JUTXfb se sustituye por el buffering del swapchain. `GXCopyTex` = readback síncrono (`Renderer::readRenderTarget`, staging + fence, `Renderer::flushFrame()` para ver el pass recién dibujado entre passes; mid-pass → último frame enviado, TODO(PC_PORT)) + codificación al tiled GX (`encodeEfbRgbaToGx`: I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR + mips box-filter). APIs de estado y fórmulas SDK completas (`GXSetCopyClear/Filter/Clamp`, `GXSetDispCopySrc/Dst/YScale/Gamma/Frame2Field`, `GXSetTexCopySrc/Dst`, `GXGetYScaleFactor/NumXfbLines/TexBufferSize`, `GXClearBoundingBox`); gamma/filtros se espejan sin aplicar (gamma 1.0 por defecto). **Fix en Bti.cpp**: decodificador CMPR corregido al framing GX/DXT1 estándar (8×8 = 4 subtiles de 8 bytes, tamaño w·h/2) — el anterior (8×4) leía la fila 3 fuera del subtile y duplicaba el tamaño. La demo renderiza al EFB y presenta vía `GXCopyDisp`. Validado: **131/131 tests bajo Xvfb** (`gx_copy_test.cpp`: mirror de estado, fórmulas SDK, round-trip de los 8 codificadores contra el decodificador BTI, y `gx_copy_efb_present_and_readback` end-to-end: dibujar→presentar→copiar→verificar píxeles; `bti_decode_cmpr` ampliado a 8×8 con fila 3 + subtiles inferiores), demo `--frames 5` limpio.

Herramientas (según gx.md): `--dump-gx` (serializa el estado espejo), `--gx-log` (TRACE de llamadas).

## M6 — Input completo ✅

- **Implementado**: `Platform::Input` SDL3 (teclado/ratón/gamepad, rumble,
  conexión en caliente, estado por frame) → `src/platform/input/`; KPAD (8) y
  WPAD (14) con semántica real en `src/compat/kpad/KPAD.cpp` +
  `src/compat/wpad/WPAD.cpp` (linkage `extern "C"`); config editable en
  `config/input.ini` (`InputConfig`); canal 0 teclado+ratón (WASD=nunchuk,
  Espacio=A, clic izq.=B, ratón=DPD, flechas=d-pad, +/− pausa, HOME=ESC),
  canales 1+ = gamepads SDL (nunchuk o Classic vía `use_classic`); speaker
  Wiimote no-op `TODO(PC_PORT)`; PAD GameCube no implementado (el juego no lo
  usa). Validado: **126/126 unit tests** (15 de input + 5 de shims), ctest
  verde.

## M6.5 — Shims host de cabeceras Petari (JGeometry/JMath) ✅

- **Shims en `src/compat/include/`** (lista y motivos en
  `compat/include/README.md`): `math_types.hpp`, `JMath/JMATrigonometric.hpp`,
  `JMath/JMath.hpp`, `JGeometry/TUtil.hpp`, `JGeometry/TVec.hpp`,
  `JGeometry/TMatrix.hpp`, `revolution/mem/heapCommon.h`. Destacan dos bugs
  silenciosos corregidos: `TVec3<f32>::dot()` y `operator=` quedaban **vacíos**
  fuera de Metrowerks (basura/no-op) y las tablas trig de JMath estaban
  declaradas pero sin datos ni ctors en la decomp.
- **Soporte host**: `jsystem/JMathCompat.cpp` (tablas trig rellenadas al
  arranque, `JMathInlineVEC::PSVEC*`, PSVEC globales de `mtx.h`, ctors de
  `TVec3<f32>`, `TAtanTable/TAsinAcosTable::atan2_/get_`), `PPCIntrinsics.cpp`
  (`__frsqrte`, `__fabsf`, `__abs`, `__memcpy`).
- **Integración real de game code**: los 7 wrappers `Game/System/WPad*.cpp`
  compilan en `pc_compat`; `game/SystemCompat.cpp` aporta los símbolos
  pequeños (verbatim del vendered) cuyos ficheros aún no compilan.
  Pendientes por la **decomp** (no por el port): `WPad.cpp` (necesita
  `WPadHVSwing`, sin `.cpp` en Petari), `updateRotate/updateAccAverage` sin
  matchear, `WPadPointer.cpp` (`Game/Util.hpp`), `WPadHolder.cpp`
  (`GameSystem.hpp`, M9).
- **Validado**: 5 tests nuevos `jmath_shim_test.cpp` (TVec3, TUtil, tablas
  trig, intrínsecas). Detalles: `docs/input.md` §5.3.
- Gamepad (Xbox, DualSense/DualShock vía SDL), teclado, ratón; mapeo Wiimote→PC (`docs/porting.md` §5).
- Config de input editable sin recompilar.
- *(Opcional)* Wiimote/Nunchuk real por Bluetooth detrás de `Platform::Input`.

## M7 — VFS + assets ✅

Sub-hitos (decisiones confirmadas: worker async, FST por escaneo, caché de
bytes diferida, verify-assets standalone):

- **M7.1 — compat/dvd síncrono ✅**: `src/compat/dvd/DVD.cpp` sobre el VFS
  (`Platform::Filesystem` ya existía de M1). **FST en memoria por escaneo**:
  `DVDInit`/primer uso construye la tabla de directorios del árbol de assets;
  entrynums = índices (root 0), estables; `DVDConvertPathToEntrynum` /
  `DVDFastOpen` O(1). Orden de visita de dos pasadas: los hijos de cada dir
  quedan **contiguos** (el FST inicial intercalaba el subárbol del primer
  hijo — `DVDReadDir` fallaba al 2º hijo; test `dvd_dir_iteration` lo cazó).
  `DVDOpen/Close`, `DVDReadPrio` (síncrona, bounds-checked, `-1` error),
  `DVDOpenDir/ReadDir/CloseDir/GetCurrentDir`, `DVDGetDriveStatus` (assets →
  `DVD_STATE_END`=0 ready; sin assets → `DVD_STATE_NO_DISK`), `DVDCheckDiskAsync`,
  `DVDGetCurrentDiskID`/`DVDCompareDiskID` (stub RMGK01), `DVDSetAutoInvalidation`,
  `DVDGetCommandBlockStatus`, `DVDCancel`. El entrynum se guarda en
  `DVDFileInfo::cb.userData` (campo SDK-interno); `cb.addr/offset/length` se
  rellenan como el hardware. Fix: `DVDOpen` rechaza directorios.
- **M7.2 — DVDReadAsyncPrio + worker ✅**: hilo dedicado (`dvd-worker`, con
  `Platform::Threading::Thread`) = el "hilo de interrupción del DVD" del SDK;
  cola estilo SDK ordenada por prioridad (estable); **callbacks invocados en
  el worker** (los tests verifican `callerThread != mainId`); `DVDCancel`
  marca cancelado → callback con `-2` (convención = Dolphin
  `DVD_RESULT_CANCELED`); lecturas en vuelo terminan (disco local).
  `compat::shutdownDVD()` drena/cancela y une el hilo (main.cpp y
  main_test.cpp). Validado: **138/138 tests** (7 nuevos en `dvd_test.cpp`:
  FST/entrynums estables, open/read/close + offsets + EOF, iteración de dirs,
  async worker + 2 lecturas concurrentes, cancel con 64 comandos en cola,
  drive state/disk id/checkdisk async, misc).
- **M7.3 — tools/verify-assets ✅**: standalone (`build/src/tools/verify-assets
  [root] [--quiet]`, exit 0/1). Core puro en `src/tools/verify_assets/`
  (`VerifyAssets.h/.cpp`) también compilado en el binario de tests (6 tests:
  árbol completo, stationed ausente = fatal, dir de boot ausente = fatal,
  root inválido, cabecera RARC/Yaz0 rota = warning, dirs opcionales =
  warning). El manifest de los 185 archivos stationed se **genera** del
  decomp (`Game/System/StationedFileInfo.cpp` → `StationedManifest.h`), no a
  mano. Sin caché de bytes (decisión diferida; el FST cachea metadatos y la
  page cache del SO cubre lecturas repetidas).
- **M7.4 — docs ✅**: `docs/assets.md` (extracción con Dolphin, colocación
  `./assets`/`--assets-dir`/`GALAXY_ASSETS_DIR`, notas legales, uso de
  verify-assets, capa DVD); `architecture.md` §6.4 actualizado.

Pendientes fuera de M7 (futuros): montaje **JKRArchive** (RARC/Yaz0) que
consume el DVD; `JKRDvdRipper`/`JKRDvdFile`; conexión de
`DVDReadAsyncPrio` con `FileLoaderThread` del juego.

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
