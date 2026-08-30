# Super Mario Galaxy — Port nativo a PC: Análisis y arquitectura

> **Estado:** M0 (análisis) — documento vivo, se actualiza con cada decisión.
> **Referencia:** [SMGCommunity/Petari](https://github.com/SMGCommunity/Petari) @ `9ecdff2` (2026-08-29).
> **Versión del juego:** `RMGK01` (Rev 0, Korea) — la única soportada por Petari hoy.

---

## 1. Resumen ejecutivo

Petari es una **decompilación** de Super Mario Galaxy 1, no un port ni un emulador. Su objetivo es reproducir byte a byte el `main.dol` original compilando el código C/C++ con el compilador original de Nintendo (CodeWarrior para PowerPC) y verificando la coincidencia objeto a objeto con la herramienta `objdiff`.

Consecuencia práctica para nuestro proyecto:

- El **código del juego (`src/Game`) es C++ de alto nivel, casi 100% independiente de la máquina**. Es exactamente el código que queremos reutilizar.
- El código que **depende de la consola** (SDK RVL: OS, GX, VI, DVD, AX, WPAD…) está decompilado pero es código que toca hardware PowerPC (registros MMIO, TB, FIFO del GPU…). **Esa es la capa que hay que sustituir**, no reescribir el juego.
- El build actual es un **build de "estática de decompilación"**: no produce un ejecutable que corra, produce objetos binarios comparables con el DOL original. Para el port necesitamos un segundo build, nativo x86-64/aarch64, que conviva con el primero sin romperlo.

**Número clave:** Petari está al **68,62 % decompilado** (15,64 % fully linked) a fecha de hoy. El port podrá ejecutar cada vez más juego a medida que la decompilación avance; para un primer *vertical slice* (Logo → una galaxia sencilla con Mario) no necesitamos el 100 %.

---

## 2. Estado actual de Petari (datos medidos sobre el repo)

| Métrica | Valor |
|---|---|
| Ficheros fuente (`*.c`/`*.cpp`) | **2 109** |
| Líneas de código | **≈ 475 500** |
| `src/Game` (lógica del juego) | 1 505 ficheros |
| `src/JSystem` (librería de Nintendo) | 167 ficheros |
| `src/RVL_SDK` (SDK de consola) | 294 ficheros |
| `src/nw4r` (librería de Nintendo, layout/math/ut) | 29 ficheros |
| `src/MSL_C` (libc de Metrowerks) | 62 ficheros |
| `src/MetroTRK` (debugger PPC) | 27 ficheros |
| `src/Runtime`, `src/NDEV`, `src/RVLFaceLib` | 25 ficheros |
| Progreso decompilación | **68,62 %** (15,64 % fully linked) |
| Compilador del build de decomp | CodeWarrior (mwcc) para PowerPC, vía `wibo` |

El build de decompilación **no incluye assets del juego** y solo necesita `orig/RMGK01/sys/main.dol` (extraído con Dolphin de una copia propia) para comparar. Ese requisito **no aplica** al build PC.

---

## 3. Entry point y arranque

El entry point del juego está en **`src/Game/System/GameSystem.cpp`**, función `main(void)`:

```cpp
void main(void) {
    OSInitFastCast();                                  // modo de conversión float (PPC)
    DVDInit();                                         // filesystem del disco
    VIInit();                                          // vídeo (framebuffers, retrace)
    HeapMemoryWatcher::createRootHeap();               // JKR heaps raíz
    OSInitMutex(&MR::MutexHolder<0>::sMutex);          // 3 mutex globales
    OSInitMutex(&MR::MutexHolder<1>::sMutex);
    OSInitMutex(&MR::MutexHolder<2>::sMutex);
    nw4r::lyt::LytInit();                              // layout UI
    MR::setLayoutDefaultAllocator();
    SingletonHolder<HeapMemoryWatcher>::init();
    ... FileRipper::setup(0x20000, ...); GameSystemException::init();
    MR::initAcosTable();                               // tabla cos acos
    SingletonHolder<GameSystem>::init();
    SingletonHolder<GameSystem>::get()->init();
    while (true) pGameSystem->frameLoop();             // bucle principal
}
```

En el DOL real, `_start` (Runtime) inicializa el entorno C/C++ y llama a `main`. Para el port, `main.cpp` de PC hará ese papel (inicialización de plataforma → boot del game code).

### Cadena de inicialización de `GameSystem::init()`

1. `JKRAram::create(0xE00000, ...)` — crea el **ARAM** (memoria de audio dedicada, hardware Wii).
2. `GameSystemObjHolder` — contenedor central de subsistemas.
3. `GameSystemFontHolder` — fuente embebida (logo/debug).
4. Máquina de nervios (state machine del arranque):
   `InitializeAudio → InitializeLogoScene → LoadStationedArchive → Normal`
5. `initGX()`: `GXInit(mFifoBase, 0x80000)` — FIFO del GPU.
6. `DrawSyncManager::start(0x300, 15)`, `GameSystemSceneController`, `GameSystemFrameControl` (60/50 fps), `GameSystemStationedArchiveLoader`, `HomeButtonLayout`, …

### Bucle principal (`GameSystem::frameLoop()`)

```cpp
void GameSystem::frameLoop() {
    MainLoopFramework::sManager->beginRender();
    draw();
    MainLoopFramework::sManager->endRender();
    update();
    calcAnim();
    mObjHolder->captureIfAllowForScreenPreserver();
    MainLoopFramework::sManager->endFrame();
    MainLoopFramework::sManager->waitForRetrace();   // sincroniza con el retrace del VI
}
```

El juego se **sincroniza por retrace del VI** (60 Hz NTSC / 50 Hz PAL). El triple buffer de XFB (`JUTXfb`), `GXCopyDisp` (copia EFB→XFB) y los callbacks `setPreRetraceCallback`/`setPostRetraceCallback` de `JUTVideo` gestionan la presentación. **Todo este tramo VI/XFB se sustituye por la ruta de presentación del PC** (ver `docs/renderer.md`).

---

## 4. Mapa de subsistemas

### 4.1 `src/Game` (1 505 ficheros) — REUTILIZAR tal cual

Lógica de juego de SMG, dividida en los "módulos" clásicos del motor interno de Nintendo:

| Módulo | Ficheros | Contenido |
|---|---|---|
| `System` | 93 | GameSystem, escenas, FileLoader, heaps, secuencias, NAND, saves |
| `MapObj` | 309 | Objetos de nivel (bloques, plataformas, estrellas, interruptores…) |
| `Boss` | 169 | Jefes (Bowser, Kameck, StinkBug, DinoPackun…) |
| `Screen` | 131 | Layouts 2D, pantallas de menú, HUD, efectos de pantalla |
| `Camera` | 127 | Cámaras (órbitas, rail, programadas por escena) |
| `Enemy` | 102 | Enemigos |
| `Player` | 81 | Mario y su máquina de estados |
| `Map` | 82 | Mapas, colisiones (BSP), modelos de nivel |
| `NPC` | 72 | Personajes no jugables |
| `LiveActor` | 56 | Base de actores (Nerve, movimiento, efectos) |
| `AudioLib` | 24 | Envoltorio del audio del juego (BGM, SE, remix) |
| `AreaObj` | 36 | Áreas (gravedad, luz, niebla, agua…) |
| `Gravity` | 14 | Campos de gravedad (planetas) |
| `Util` | 72 | Utilidades (matemáticas, ficheros, memoria, singletons) |
| `Effect`/`Demo`/`Ride`/`Animation`/… | restantes | Partículas, demos, monturas, animaciones |

**Prácticamente ningún fichero de `src/Game` toca hardware directamente.** Los pocos puntos de contacto con el SDK están aislados:

- 154 `#include <revolution/...>` en todo `src/Game` (cabeceras de tipos: `GXEnum.h`, `GXGeometry.h`, `wpad.h`, `mtx.h`, `os.h`, `vi.h`, `sc.h`…).
- Llamadas directas a GX/VI/DVD/OS/WPAD (inventario en §6).
- **Inline asm PowerPC en 10 ficheros** de Game/JSystem/nw4r (`Game/Util/MathUtil.cpp`, `Game/LiveActor/ShadowVolumeOvalPole.cpp`, `Game/MapObj/ClipAreaShape.cpp`, `JSystem/JMath/JMath.cpp`, `JSystem/J3DGraphAnimator/J3DAnimation.cpp`, `JSystem/J3DGraphAnimator/J3DMtxBuffer.cpp`, `JSystem/J3DGraphBase/J3DShapeMtx.cpp`, `JSystem/J3DGraphBase/J3DSys.cpp`, `JSystem/J3DGraphBase/J3DTransform.cpp`, `nw4r/db/db_assert.cpp`) — operaciones de tipo `frsqrte` (raíz cuadrada rápida) y cálculo de matrices en asm. Se sustituyen por C equivalente (`1.0f/sqrtf`, etc.), documentado en `porting.md`.

### 4.2 `src/JSystem` (167 ficheros) — REUTILIZAR salvo backends de hardware

Librería "JSystem" de Nintendo (la misma de Mario Kart / Zelda):

| Subdir | Uso | Portabilidad |
|---|---|---|
| `JKernel` (JKR*) | Heaps (`JKRExpHeap`, `JKRSolidHeap`, `JKRUnitHeap`), archivos (`JKRArchive`, `JKRCompArchive`), hilos | **Heaps y archivos-en-memoria: portables tal cual.** Solo los rippers de DVD (`JKRDvdFile`, `JKRDvdRipper`, `JKRDvdArchive`) y **todo el ARAM** (`JKRAram*`) requieren sustitución |
| `J3DGraph*` | Motor 3D: materiales, formas, display lists, cámara | Lógica portable, pero **emite llamadas GX** → necesita nuestra capa GX |
| `J2DGraph` | Gráficos 2D (J2DOrthoGraph…) | Ídem |
| `JUtility` | `JUTVideo`, `JUTXfb`, `JUTTexture`, fuentes, consola debug | Texturas portables; **JUTVideo/JUTXfb se sustituyen** por la ruta de presentación PC |
| `JAudio2` | Sistema de sonido (JAISe, JAISeq, JASDriver, JASDSPChannel…) | Núcleo mezclador portable, pero **depende de AX/DSP de la consola** → capa de audio PC |
| `JParticle` | Partículas (JPA*) | Lógica portable; emite GX |
| `JMath` | Matemáticas rápidas | Portable salvo el asm de `JMath.cpp` |
| `JGadget`, `JSupport` | Contenedores | Portables |

### 4.3 `src/RVL_SDK` (294 ficheros) — NO compilar; reimplementar en `compat/`

SDK de Nintendo para Wii. Está decompilado, pero es **código que toca hardware PowerPC** (registros, MMIO, TB, FIFO). Nuestra capa `src/compat/` reimplementa **la misma API** (mismas cabeceras, mismos tipos) sobre la plataforma PC.

| Subdir | Ficheros | Función | Sustituto |
|---|---|---|---|
| `os` | 33 | Hilos, mutex, colas, ticks, excepciones, panics, caché | `Platform::Threading`, `Platform::Timing` |
| `gx` (+`gd`) | 14 + 8 | API gráfica GX (FIFO → GPU) | `compat/gx` sobre `Platform::Renderer` (Vulkan) |
| `vi` | 3 | Vídeo, retrace, framebuffers | `Platform::Video` + present |
| `dvd` | 8 | Disco: paths, lectura | `Platform::Filesystem` (VFS) |
| `ax`, `axfx`, `dsp` | 4+3+3 | Audio mezclador, DSP | `Platform::Audio` (mezclador CPU/SDL) |
| `wpad`, `kpad`, `pad`, `si` | 5+1+1+2 | Wiimote/Nunchuk/Pro, sensor bar | `Platform::Input` (SDL3: gamepad, teclado, ratón) |
| `nand`, `nwc24`, `sc` | 6+17+3 | Saves flash, mensajería online, settings | Saves locales en disco; NWC24 → stub |
| `mem`, `mtx`, `tpl`, `arc`, `fs` | 4+5+1+1+1 | Memoria, matrices, texturas TPL, arcos | `Platform::Memory`, matrices portable, resto portable |
| `bte` | 97 | Bluetooth (protocolo) | No necesario para PC (solo Wiimote real, opcional) |
| `vf` | 48 | Virtual filesystem de NAND | No necesario |
| `net`, `ipc`, `esp`, `exi`, `euart`, `usb`, `wud`, `thp`, `wenc`, `db`, `aralt`, `ai`, `rso`, `sc`… | resto | Varios | Stub o no usado por el juego |

### 4.4 `src/nw4r` (29 ficheros) — REUTILIZAR

- `lyt` — **sistema de layout de UI** (BFLYT): panes, textbox, picture, animaciones. El juego entero de UI depende de esto. La lógica es portable; el **dibujo** emite GX a través de `lyt_drawInfo` → requiere nuestra capa GX.
- `math`, `ut` — utilidades, endian helpers, listas enlazadas. Portables.

### 4.5 Otros

- `src/MSL_C` (62) — libc de Metrowerks: **usar la libc del host** (musl/glibc/msvcrt) en lugar de compilarla; la mayoría de ficheros son innecesarios en host.
- `src/MetroTRK` (27), `src/NDEV` (2) — infraestructura de debugger PowerPC: **no compilar**.
- `src/RVLFaceLib` (14) — caras Mii (editor de Mii): portable pero solo se usa en ciertas pantallas; se puede dejar para más adelante.
- `src/Runtime` (9) — runtime C/C++ PowerPC (excepciones, `ptmf`, `va_arg`): **no compilar**; el host aporta su runtime.

---

## 5. El build actual de decompilación (y por qué no nos sirve tal cual)

- `python configure.py` genera un proyecto **ninja**.
- Cada `.c/.cpp` se compila con **CodeWarrior para PowerPC** (descargado automáticamente; en x86_64 Linux se ejecuta vía `wibo`, un wrapper Win32 mínimo) con flags que fuerzan el layout del código original.
- Se compara objeto a objeto contra `orig/RMGK01/sys/main.dol` (requiere la copia del juego) con `objdiff` para validar que la decompilación "casa".
- El resultado **no es un ejecutable**: es una réplica verificable del binario de la consola.

**Decisión:** el build PC será **independiente y paralelo** (CMake, `-DPLATFORM=PC`), compilado con GCC/Clang/MSVC para x86-64/aarch64, **sin requisito de matching** y **sin necesidad del DOL original**. El build de decomp sigue existiendo en su propio árbol (`build/` de ninja) y no se toca. Ver `docs/build.md`.

---

## 6. Dependencias de Wii — inventario por capa (qué sustituir)

Medido sobre `src/Game` + `src/JSystem` + `src/nw4r` (lo que realmente usa el juego):

### 6.1 GX — la dependencia crítica (≈ 165 funciones distintas, > 2 500 llamadas)

Llamadas más frecuentes desde el código del juego:

| Llamada | Veces | Llamada | Veces |
|---|---|---|---|
| `GXSetVtxAttrFmt` | 145 | `GXSetTevColorIn` | 99 |
| `GXSetVtxDesc` | 141 | `GXSetTexCoordGen2` | 93 |
| `GXPosition3f32` (vértices inmediatos) | 139 | `GXSetTevAlphaOp` | 91 |
| `GXSetTevOrder` | 137 | `GXSetTevColorOp` | 91 |
| `GXTexCoord2f32` | 110 | `GXSetChanCtrl` | 80 |
| `GXSetTevAlphaIn` | 104 | `GXBegin`/`GXEnd` | 76+80 |

Categorización completa y estrategia de emulación en **`docs/gx.md`**. Resumen: el juego usa prácticamente **toda la API GX del modo "fixed function"**: TEV multi-etapa (color+alpha combinados por píxel), **texturizado indirecto (ind stages)** — usado por el brillo del puntero estrella, niebla, iluminación por canal (ChanCtrl), display lists, matrices de posición/normal/textura, y la ruta EFB→XFB (copy). Esto define la forma del renderer (no es un "engine genérico", es una máquina de traducción GX→shaders).

### 6.2 OS (≈ 40+ funciones distintas en uso)

`OSInitMutex/OSLockMutex/OSUnlockMutex` (88 en total), `OSInitMessageQueue/OSSendMessage/OSReceiveMessage/OSJamMessage`, `OSDisableInterrupts/OSRestoreInterrupts`, `OSGetTime/OSGetTick`, `OSCreateThread/OSResumeThread/OSSuspendThread`, `OSReport` (log), `OSPanic`, `OSRoundUp32B`, `OSf32tou8`/`OSf32tos16` (conversiones float→int con truncamiento específico de Wii), `OSInitFastCast`, `OSProtectRange`…

→ `compat/os`: hilos (`std::thread`), mutex (`std::mutex`), colas de mensajes (semáforo + cola), ticks (reloj de alta resolución), panics (log FATAL + abort), fastcast (semántica `float→int` bien definida). **Las conversiones `OSf32tou8` etc. son semánticamente delicadas** (el juego depende del truncamiento exacto; hay que replicar el comportamiento, no usar `(u8)float` a secas).

### 6.3 VI (17 usos)

`VIWaitForRetrace`, `VIFlush`, `VISetBlack`, `VIGetRetraceCount`, `VIGetTvFormat`, `VISetPreRetraceCallback`/`PostRetraceCallback`, `VISetNextFrameBuffer`, `VIScreenWidthRatio`, `VIInit`…

→ `compat/vi` sobre `Platform::Video`: el "retrace" se convierte en la sincronización de presentación (vsync); `VIGetTvFormat` devuelve una constante de configuración (NTSC 60 Hz por defecto, PAL opcional); los callbacks de retrace se disparan antes/después de presentar.

### 6.4 DVD (≈ 15 usos)

`DVDReadPrio` (15), `DVDConvertPathToEntrynum` (11), `DVDReadAsyncPrio` (7), `DVDOpen/DVDFastOpen/DVDClose`, `DVDToAram`, `DVDReadDir`…

→ `compat/dvd` sobre **VFS** (`Platform::Filesystem`): paths estilo `/StageData/...` se resuelven contra el árbol de assets extraído por el usuario. `DVDReadAsyncPrio` → lectura en hilo de carga (ya existe `FileLoaderThread` en el juego; se conecta con la cola de mensajes del compat OS).

### 6.5 Input Wii (≈ 20 usos)

`KPADRead`, `WPADControlMotor`, `WPADControlSpeaker`, `WPADSetConnectCallback`, `WPADProbe`, `WPADGetSensorBarPosition`, `PAD*`, `SISetSamplingMode`…

→ `compat/wpad` + `kpad` + `pad` sobre `Platform::Input` (SDL3): gamepad (Xbox/DualSense), teclado, ratón. **Semántica Wiimote a preservar:** puntero IR (el cursor estrella se apunta con el mando), shake (sacudir para girar), puntero de UI en todos los menús. El mapeo Wiimote→gamepad es una decisión de diseño de port (ver `porting.md`).

### 6.6 Audio (sin llamadas directas AX en Game; todo vía JAudio2)

El juego usa `JAudio2` (JSystem) que a su vez usa AX/DSP de la consola. La cadena: `GameAudio/AudioLib → JAISe/JAISeq → JASDriver (mezcla en CPU) → AX/DSP (mezcla final en hardware)`. El DSP no está decompilado al 100 % y es hardware.

→ `compat/ax` + `compat/dsp` + `Platform::Audio` (SDL3 audio): **reimplementar la mezcla final del DSP en CPU** (un mezclador estéreo de 32 canales es suficiente; el formato de onda y el BNK del juego se leen tal cual). Ver `docs/porting.md` (módulo de audio es de los más grandes).

### 6.7 NAND / NWC24 / SC / Misc

- `NANDManager` (saves) → directorio de saves del usuario en disco (`compat/nand`), mismo formato de archivo que en Wii.
- `NWC24` (mensajería, correos de Luigi) → **stub** (no tiene sentido en PC; `TODO(PC_PORT)`).
- `SC` (settings de consola, aspect ratio, idioma) → fichero de configuración del port.
- `Misc`: `MEM*` (arena allocator — el juego usa sobre todo JKR), `TPL` (texturas — portable), `MTX` (matrices — portable, implementación en CPU), `ARC`, `EUART` (UART debug → log), `DB` (debug → log), `THP` (vídeo — probablemente no usado por SMG), `WENC` (codificador WAV — no usado).

### 6.8 Runtime / MSL_C / MetroTRK

No compilar. El host (GCC/Clang/MSVC) aporta libc y runtime C++. Cuidado con:
- `ptmf` (punteros a métodos miembro con el ABI PowerPC de CodeWarrior) — el código fuente usa `MR::Functor` que internamente puede apoyarse en ese ABI; hay que verificar `Functor`/`ptmf` en host.
- `__init_cpp_exceptions` / `Gecko_ExceptionPPC` — excepciones C++ del ABI PowerPC; en host se usa el modelo nativo.
- Placement `new (heap, align)` — el código usa `new (pHeap, -4) u8[size]` y similar (35 usos en `src/Game`); JKR define los overloads, GCC/Clang los soportan igual (es C++ estándar), solo hay que asegurar que los overloads de `operator new` de JKR se compilan.

---

## 7. Qué se puede reutilizar directamente (sin tocar)

1. **Todo `src/Game`** (salvo los 6 ficheros con asm PPC y las llamadas SDK puntuales): lógica de actores, nervios, escenas, cámara, gravedad, física de Mario, colisiones, enemigos, jefes, UI lógica.
2. **JKR heaps** (`JKRExpHeap/JKRSolidHeap/JKRUnitHeap`) — son asignadores de RAM puros; **no hace falta una capa `Platform::Memory` para el juego**: JKR ya gestiona la memoria, solo hay que darle RAM del host como raíz. (La capa `Platform::Memory` se usa para el compat OS y para estadísticas.)
3. **JKRArchive en memoria** (`JKRMemArchive`, `JKRCompArchive`) + todo el **descompresor Yaz0** (`JKRDecomp`) — el juego lee archivos SZS comprimidos; el descompresor es código puro.
4. **Parsers de formatos del juego**: BCSV, BMD/BDL (via J3D loader), BTI (via JUTTexture), BFLYT/BRLYT (via nw4r lyt), BBNK/onda (via JAS), BCSV, etc. La mayoría son **byte-oriented** (construyen los valores con shifts, no con structs sobre memoria) → sobreviven al cambio de endianness; los que no, se arreglan puntualmente (ver §9.4).
5. **`src/nw4r`** completo (math, ut, lyt) salvo el dibujo.
6. **JParticle** (partículas) y **J3D graph** (materiales/geometría) como lógica.
7. **MSL_C**: reemplazada por libc del host; las funciones específicas de Metrowerks que el juego use directamente se envuelven en `compat/`.

---

## 8. Qué hay que implementar para PC

Ordenado por "cantidad de trabajo":

| Bloque | Dónde | Esfuerzo |
|---|---|---|
| **`compat/os`** (hilos, mutex, colas, ticks, panics, casts) | `src/compat/os/` | Medio — mecánico, pero exige fidelidad semántica |
| **`compat/dvd` + VFS** | `src/compat/dvd/`, `src/platform/Filesystem/` | Bajo-medio |
| **`compat/vi` + presentación** | `src/compat/vi/`, `src/platform/Video/` | Bajo |
| **`compat/gx` sobre Vulkan** | `src/compat/gx/`, `src/platform/Renderer/vulkan/` | **Muy alto** — el corazón del proyecto |
| **`compat/wpad/kpad/pad` + input** | `src/compat/input/`, `src/platform/Input/` | Medio (más la decisión de mapeo Wiimote) |
| **`compat/ax` + audio** | `src/compat/ax/`, `src/platform/Audio/` | Alto (JAudio2 + mezclador DSP) |
| **`compat/nand` (saves)** | `src/compat/nand/` | Bajo-medio |
| **`compat/nwc24`, `compat/sc`** | `src/compat/nwc24/`, `src/compat/sc/` | Bajo (stubs) |
| **`compat/mem`, `mtx`, `tpl`** | `src/compat/` | Bajo (la mayoría portable) |
| **main.cpp + bucle de juego** | `src/main.cpp` | Bajo |
| **Herramienta de extracción/verificación de assets** | `src/tools/` | Bajo-medio |
| **Logging, profiling, debug GPU, dumps** | `src/platform/Log/` etc. | Medio (transversal) |
| **Tests** | `src/tests/` | Continuo |

---

## 9. Principales dificultades técnicas (ranked)

1. **Emulación del pipeline TEV de GX (fija por píxel).** El juego usa TEV multietapa (hasta 16 etapas), combinadores color+alpha, valores K, **texturizado indirecto** (ind stages + matrices de distorsión, p. ej. brillo del puntero estrella), niebla por etapa, swap de canales RGBA, alpha compare, `GXSetZCompLoc`… Hay que **compilar el estado GX a shaders Vulkan** (generación y caché de pipelines) o traducir a un IR. Es el problema conocido de cualquier port GX→PC moderno y **define la forma del renderer** (§10 y `docs/gx.md`).
2. **Decompilación incompleta (68,62 %).** No se puede ejecutar lo que aún no está decompilado. Mitigaciones: (a) elegir un *vertical slice* que solo dependa de código ya decompilado; (b) seguir el ritmo del proyecto upstream (los módulos que faltan aparecen con el tiempo); (c) reimplementar puntualmente lo que falte y esté aislado, marcado `TODO(PC_PORT)`.
3. **Audio: JAudio2 + DSP.** Cadena profunda (Secuencias/SE → mezcla en CPU → mezcla final DSP). El DSP no es portable; hay que reimplementar la mezcla final en CPU y conectar `Platform::Audio`. Es un módulo grande, pero **no bloquea el primer boot**: el juego arranca con audio stub si es necesario.
4. **Endianness.** Los datos del juego son **big-endian**. Los parsers byte-oriented sobreviven; hay que auditar cualquier lectura que haga `memcpy` de structs o use bitfields sobre buffers. Además, los formatos en memoria del juego (p. ej. el `JUTTexture` y formatos de textura GX: I8, IA8, RGB565, RGBA8, CMPR/DXT1…) necesitan conversión a formatos Vulkan — trabajo mecánico + tests.
5. **Quirks de CodeWarrior/ABI PPC.** El código decompilado se escribió para mwcc/PPC: asm inline (10 ficheros), placement new con alineación, `ptmf`, orden de evaluación, struct layout implícito. No nos importa el *matching*, solo la **semántica correcta** en host: hay que compilar pronto un módulo real para destapar los problemas y documentarlos en `porting.md`.
6. **Modelo de threading de la consola.** El juego usa hilos OS con prioridades y colas de mensajes (audio, carga de ficheros, async executors, guardado). En PC hay que mapear prioridades y asegurar que los supuestos de sincronización del juego (p. ej. `OSSuspendThread(OSGetCurrentThread())` para pausar el hilo actual, mensajes con timeout) se comporten igual. Riesgo de carreras latentes que en Wii quedaban enmascaradas por la planificación cooperativa.
7. **Frame pacing.** El bucle está atado al retrace de 60/50 Hz del VI. En PC: **timestep fijo de simulación (60 Hz)** y presentación independiente (vsync opcional, sin tocar la lógica). `GameSystemFrameControl` decide 60 vs 50 según `VIGetTvFormat()` → configurable en el port.
8. **Semántica del mando Wiimote.** El puntero IR (cursor estrella, menús) y el shake son parte del *gameplay*, no solo del input: el mapeo a ratón/gamepad debe decidirse con cuidado (p. ej. modo ratón = puntero 1:1, modo gamepad = puntero tipo "giroscopio" con el stick derecho). Wiimote real por Bluetooth es opcional y va detrás de la misma `Platform::Input`.

---

## 10. Estrategia general del port

**Principio rector:** *mantener el código del juego original siempre que sea posible y reemplazar las APIs/plataformas de Wii por implementaciones nativas de PC.*

1. **El game code (`src/Game`) se compila tal cual** en el build PC, apuntando a las **mismas cabeceras** (`libs/RVL_SDK/include/...`, `revolution.h`) que usa hoy, de modo que el código no cambie (o cambie lo mínimo).
2. **`src/compat/` implementa la API de la consola** (GX, VI, DVD, OS, WPAD, AX, NAND…) sobre `Platform::*`. Las cabeceras originales se mantienen (tipos idénticos: `GXColor`, `Mtx`, `GXTexObj`, `GXRenderModeObj`…); las funciones se implementan en `compat/`.
3. **`Platform::*` es la capa de abstracción del PC** (Memoria, Filesystem, Timing, Threading, Input, Audio, Video, Renderer, Log). Cada módulo tiene implementaciones en `platform/windows/` y `platform/linux/` (más backends compartidos: SDL3, Vulkan). **`#ifdef _WIN32` solo dentro de esos dos directorios.**
4. **El renderer no es un engine**: es la traducción de *lo que el juego realmente pide a GX* (inventario en `docs/gx.md`) a Vulkan. API estrecha, pipelines generados a partir del estado GX.
5. **Build**: CMake `-DPLATFORM=PC`, Linux primero, Windows después; coexistencia pacífica con el build de decompilación.
6. **Sin assets propietarios**: el port lee un árbol de archivos extraído por el usuario de su propia copia (`assets/`). Herramienta de verificación para comprobar que la extracción está completa. Nada se distribuye, nada se descarga.
7. **Progreso por milestones pequeños y compilables**, cada uno con tests. El primer hito de éxito: *Mario renderizado en una escena sencilla* (M9), luego *primera galaxia* (M10).

---

## 11. Estructura de directorios propuesta

Difiere de la propuesta inicial en los puntos anotados `(Δ)`.

```
galaxy-pc/
├── CMakeLists.txt                  # build PC: -DPLATFORM=PC
├── cmake/                          # helpers (detección Vulkan/SDL3, toolchains)
├── README.md
├── LICENSE                         # CC0-1.0 (mismo espíritu que Petari)
│
├── src/
│   ├── main.cpp                    # entry point PC (SDL3): init plataforma → boot game code
│   │
│   ├── platform/
│   │   ├── platform.h              # fachada: Platform::Init(), tipos comunes
│   │   ├── Log/                    # niveles TRACE..FATAL, sinks, profiling hooks
│   │   ├── Memory/                 # respaldo del heap raíz, estadísticas, arena
│   │   ├── Filesystem/             # VFS: disco (y futuro: archivos empaquetados)
│   │   ├── Timing/                 # reloj de alta resolución, timestep fijo
│   │   ├── Threading/              # hilos, mutex, colas, semáforos
│   │   ├── Input/                  # teclado, ratón, gamepad (SDL3); mapeo Wiimote
│   │   ├── Audio/                  # backend audio PC (SDL3) + mezclador
│   │   ├── Video/                  # ventana, vsync, presentación
│   │   ├── Renderer/
│   │   │   ├── renderer.h          # API estrecha definida por el uso de GX (docs/renderer.md)
│   │   │   ├── vulkan/             # backend Vulkan (device, shaders, pipelines, buffers)
│   │   │   └── debug/              # validation layers, GPU labels, frame timing, dumps
│   │   ├── windows/                # #ifdef WIN32 SOLO aquí (impl. Win32/COM donde haga falta)
│   │   └── linux/                  # impl. POSIX (x11/wayland vía SDL3 en la práctica)
│   │
│   ├── compat/                     # ← capa de compatibilidad Wii (API de consola)
│   │   ├── include/                # (Δ) cabeceras propias SOLO para los casos en que las
│   │   │                           #     originales de Petari no compilen en host (override)
│   │   ├── os/                     # OSInitMutex, OSCreateThread, OSGetTime, OSPanic…
│   │   ├── dvd/                    # DVDOpen, DVDReadPrio, DVDConvertPathToEntrynum…
│   │   ├── vi/                     # VIInit, VIWaitForRetrace, VIGetTvFormat…
│   │   ├── gx/                     # GX*: estado GX + traducción al Renderer (docs/gx.md)
│   │   ├── wpad/  kpad/  pad/      # input de consola sobre Platform::Input
│   │   ├── ax/  dsp/               # audio de consola sobre Platform::Audio
│   │   ├── nand/                   # saves → disco
│   │   ├── nwc24/  sc/             # stubs + settings
│   │   ├── mem/  mtx/  tpl/  arc/  # envoltorios donde hagan falta (la mayoría portable)
│   │   └── runtime/                # (Δ) sustitutos de Runtime/MetroTRK/MSL_C no portables
│   │
│   ├── tools/                      # (Δ) CLI: verify-assets, dump-gx, extract-textures…
│   └── tests/                      # (Δ) tests unitarios (doctest/Catch2 o runner propio)
│
├── third_party/
│   └── petari/                     # (Δ) submodule git → SMGCommunity/Petari (upstream puro)
│       ├── src/Game/  src/JSystem/  src/nw4r/   # se compilan desde aquí, sin copiar
│       └── libs/RVL_SDK/include/revolution/     # cabeceras originales (include path)
│
├── docs/
│   ├── architecture.md             # ← este documento
│   ├── build.md
│   ├── porting.md
│   ├── renderer.md
│   ├── gx.md
│   └── milestones.md
│
└── assets/                         # extraído por el usuario de SU copia (git-ignored)
    └── StageData/ ObjectData/ MapData/ SoundData/ …
```

**Justificación de los cambios `(Δ)` frente a la estructura inicial:**

1. **Petari como submodule (`third_party/petari`)** en lugar de copiar `src/game/` dentro de nuestro árbol: el port y la decompilación avanzan a ritmos distintos; con submodule seguimos recibiendo módulos decompilados nuevos y los diffs quedan limpios. El código se compila directamente desde el submodule (CMake con paths absolutos del submodule) — sin copias duplicadas.
2. **`compat/include` aparte**: las cabeceras originales de `revolution/*` se usan tal cual como include path (los tipos deben ser idénticos). Solo los ficheros que no compilen en host se overridan con cabeceras propias (include path nuestro con prioridad).
3. **`compat/runtime`** explícito para los sustitutos de Runtime/MetroTRK.
4. **`tools/` y `tests/`** dentro de `src/` para que CMake los vea sin ruta mágica.
5. **`platform/backends/`** no existe como tal: los backends son SDL3/Vulkan y viven en sus módulos (`Input` usa SDL3, `Renderer/vulkan` es Vulkan, etc.). No inventamos una capa de abstracción de backends innecesaria.

---

## 12. Decisiones arquitectónicas (ADR)

Formato: problema → alternativas → solución → motivo.

### ADR-001 — Base del port: la decompilación, no la reingeniería
- **Problema:** construir un port de SMG desde cero es inviable; la decompilación contiene la lógica exacta.
- **Alternativas:** (a) emulador Wii + inyección (descartado por requisito); (b) decomp como base.
- **Solución:** usar Petari como base de código, sustituyendo solo la capa de API de consola.
- **Motivo:** requisito explícito del proyecto y única vía razonable.

### ADR-002 — Compilación host: GCC/Clang/MSVC, sin matching
- **Problema:** el código está escrito para CodeWarrior/PPC y el build de decomp verifica matching.
- **Alternativas:** (a) compilar host con mwcc vía wibo (frágil, sin soporte x86 output útil); (b) toolchain nativa.
- **Solución:** toolchains nativas (Linux: GCC/Clang; Windows: MSVC o Clang-cl), **sin requisito de matching**; la corrección se valida con tests y ejecución, no con objdiff.
- **Motivo:** el matching es irrelevante para un port; la semántica correcta se consigue igual manteniendo los parsers/estado tal cual.

### ADR-003 — Cabeceras GX/OS originales + implementación en `compat/`
- **Problema:** el game code usa tipos de la consola (`GXColor`, `Mtx`, `GXTexObj`, `GXRenderModeObj`).
- **Alternativas:** (a) redefinir tipos propios y adaptar el game code; (b) conservar cabeceras originales.
- **Solución:** conservar `libs/RVL_SDK/include` como include path; implementar las funciones en `compat/`.
- **Motivo:** cero cambios en el game code; los tipos ya son exactamente los que el juego espera.

### ADR-004 — No sustituir los heaps JKR; respaldarlos con memoria del host
- **Problema:** el juego asigna todo vía JKR heaps y además distingue heaps "Napa" (RAM principal) y "GDDR3" (RAM de vídeo).
- **Alternativas:** (a) reemplazar JKR por un asignador propio; (b) mantener JKR.
- **Solución:** JKR heaps se compilan tal cual; `Platform::Memory` proporciona la memoria bruta (malloc) para el heap raíz y las estadísticas; la distinción Napa/GDDR se conserva como mera partición lógica de RAM.
- **Motivo:** JKR es portable, ya funciona, y sustituirlo añadiría una abstracción sin beneficio (viola "no APIs abstractas innecesarias").

### ADR-005 — Renderer: API estrecha modelada por el uso real de GX
- **Problema:** un renderer genérico (scene graph, materiales arbitrarios) sería enorme y no encajaría.
- **Alternativas:** (a) renderer genérico; (b) traducción GX→Vulkan directa con API mínima.
- **Solución:** inventario del uso de GX (docs/gx.md) define los recursos y estados que expone `Renderer`; el compat GX genera pipelines/shaders a partir del estado TEV.
- **Motivo:** el 100 % del trabajo gráfico del juego pasa por GX; la API del renderer debe cubrir exactamente eso y nada más.

### ADR-006 — Vulkan como único backend inicial
- **Problema:** portabilidad Windows/Linux con un solo backend.
- **Alternativas:** (a) OpenGL (obsoleto, drivers inconsistentes); (b) Vulkan; (c) D3D12 también desde el inicio.
- **Solución:** Vulkan primero (Windows y Linux comparten el 100 % del backend); D3D12 como opción futura detrás de la misma `Renderer`.
- **Motivo:** requisito del proyecto; Vulkan funciona en Windows/Linux/Steam Deck con un solo código.

### ADR-007 — SDL3 para ventana, input y audio base
- **Problema:** ventana, input, audio y gamepad multiplataforma.
- **Alternativas:** GLFW (ventana/input, sin audio) + otra cosa; SDL3 (todo incluido, gamepad robusto, Steam Deck friendly).
- **Solución:** SDL3.
- **Motivo:** requisito del proyecto; un solo backend cubre ventana+input+audio y simplifica el árbol.

### ADR-008 — VFS sobre árbol de archivos extraído
- **Problema:** el juego direcciona por paths DVD (`/StageData/...`); el usuario tiene el disco extraído.
- **Alternativas:** (a) ISO montada (complejo); (b) directorio raíz + VFS.
- **Solución:** VFS con fuente "directorio del usuario" (`assets/`); los paths DVD se resuelven contra la raíz. Formato empaquetado propio = futuro opcional (no ahora).
- **Motivo:** mínimo esfuerzo, máximo respeto legal, y permite verificación de assets.

### ADR-009 — Bucle con timestep fijo de simulación
- **Problema:** el bucle del juego cuelga del retrace VI (60/50 Hz).
- **Alternativas:** (a) bucle variable acoplado a presentación; (b) simulación fija 60 Hz + presentación independiente.
- **Solución:** simulación fija (60 Hz por defecto, configurable), presentación a la frecuencia del monitor con vsync opcional.
- **Motivo:** la física y el frame control del juego asumen ticks fijos; desacoplar evita glitches de velocidad.

### ADR-010 — `#ifdef` solo en implementaciones de plataforma
- **Problema:** el proyecto debe compartir el máximo código entre Windows y Linux.
- **Alternativas:** `#ifdef _WIN32` disperso vs directorios por plataforma.
- **Solución:** `platform/windows/` y `platform/linux/` (elección por CMake), `#ifdef` solo ahí; el resto del árbol no conoce la plataforma.
- **Motivo:** requisito de calidad del proyecto.

### ADR-011 — Endianness: parsers byte-oriented + tests
- **Problema:** datos BE en CPU LE.
- **Alternativas:** (a) emular BE a nivel de CPU (costo enorme); (b) auditar parsers y convertir formatos en frontera.
- **Solución:** mantener los parsers tal cual (ya son mayoritariamente byte-oriented), añadir utilidades de conversión explícitas donde haga falta y tests de binary parsing/texture conversion.
- **Motivo:** los parsers del juego ya fueron escritos para leer BE sin asumir endianness del host en la mayoría de los casos; el trabajo es de auditoría + tests, no de reescritura.

### ADR-012 — Petari como submodule, no fork
- **Problema:** el port depende del avance de la decompilación.
- **Alternativas:** fork con cambios propios vs submodule limpio.
- **Solución:** submodule `third_party/petari` sin tocar; nuestro código vive en `src/`; las pocas cabeceras que haya que tocar van a `compat/include` (override por orden de include).
- **Motivo:** poder absorber los módulos recién decompilados sin conflictos de merge.

---

## 13. Orden de implementación recomendado

Detalle completo con criterios de éxito en **`docs/milestones.md`**. Resumen:

| Hito | Contenido | Bloquea |
|---|---|---|
| **M0** ✅ | Análisis (este documento) | — |
| **M1** | Build CMake `-DPLATFORM=PC` en Linux + núcleo de plataforma (Log, Memory, Filesystem, Timing, Threading) + tests + **smoke-test de compilación de un módulo real de Petari** (p. ej. JKR heap o `Game/Util`) | M2 |
| **M2** | Windows: mismo código compila con MSVC; CI | M3 |
| **M3** | Ventana + Vulkan device + bucle de juego con delta time + input mínimo | M4 |
| **M4** | `Renderer` (API estrecha según docs/gx.md) + backend Vulkan | M5 |
| **M5** | `compat/gx`: estado GX → shaders; vértices, texturas, TEV, blend, depth, display lists | M9 |
| **M6** | Input completo (gamepad/teclado/ratón; mapeo Wiimote; Wiimote real opcional) | M9 |
| **M7** | VFS + tool de verificación de assets + documentación de qué extraer | M9 |
| **M8** | Audio (`compat/ax` + mezclador PC) — puede ir después de M9 (stub antes) | M10 |
| **M9** | **Primer boot real: Logo → título → Mario en escena sencilla** | M10 |
| **M10** | Primera galaxia jugable (geometría, texturas, física, cámara, UI) | M11 |
| **M11** | Extender compatibilidad hasta el juego completo | — |

**El primer bloqueo real para ejecutar código en PC** no es el renderer ni el audio: es que **hoy no existe ningún artefacto compilable en host** — no hay build nativo, y el entry point del juego llama inmediatamente a `OSInitFastCast/DVDInit/VIInit/GXInit`. Por eso M1 empieza por: build CMake host + capa de plataforma mínima + **smoke-test que compile y ejecute un módulo real de Petari** (destapa los quirks de CodeWarrior/endianness antes de invertir en GX). Con M1+M3+M5 parcial (GX init + un triángulo/quads 2D) ya se puede ejecutar el esqueleto del boot del juego.
