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
- Verificación hecha: build cruzado MinGW-w64 OK (compila contra `windows.h` real) y **tests 36/36 bajo Wine** en el `.exe` de Windows; demo M1 completa también bajo Wine. El build MSVC real lo valida el CI en `windows-latest`.

Notas de portabilidad descubiertas en M2 (ver `porting.md`):
- `std::thread::id` se formatea en hexadecimal en Windows (puntero) y decimal en Linux — no parsear su stream; copiar sus bytes.
- `Inline.hpp` del vendered usa `__attribute__` sin guarda → override con `__declspec(noinline)` para MSVC.
- `std::atomic<Stats>` no lock-free necesita `libatomic` en GCC/MinGW (no en MSVC).

## M3 — Ventana + Vulkan + bucle de juego

- SDL3: ventana (resizable, fullscreen), vsync.
- Vulkan (volk): device, swapchain, validation layers, debug labels, `VK_KHR_dynamic_rendering`.
- Bucle de juego: timestep fijo 60 Hz + presentación; delta time correcto; cierre limpio (señales/ventana).
- Input mínimo (SDL3) expuesto como `Platform::Input` básico.
- Demo: triángulo/quads con shader generado + overlay de FPS por `Platform::Log`.
- `GXInit` mínimo + `GXClearColor`/`GXSetViewport` dibujando el fondo (primer olor a GX).

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
