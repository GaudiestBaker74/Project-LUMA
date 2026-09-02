# Boot (M9) — primer boot real

Status: **M9.4 (sistema de escenas + Logo) ✅** — 169 tests, 0 failed (con display; 167+2 skipped headless), validación Vulkan limpia
(headless; en máquina con display/GPU corren los 2 de Vulkan → 169). El boot
real (`galaxy-pc --boot`) crea la LogoScene y corre su cadena de nerves hasta
`Deactive` con el frame loop a 60 fps. M9.5 planificado abajo; cada sub-hito
cierra con "compila + enlaza + ctest verde".

## Plan M9 (sub-hitos)

- **M9.1 — Capa de hilos del SO ✅**: `OSThread`/`OSThreadQueue`/`OSMessageQueue`
  del SDK sobre `std::thread` (`src/compat/os/OSThread.cpp`) + `JKRThread.cpp`
  vendered compilado. Base de todo lo que viene: `JASDvdThread`/`JASAudioThread`
  reales (pueden sustituir el host glue de M8), `FunctionAsyncExecutor`
  (`MR::startFunctionAsyncExecute`), thread-switch.
- **M9.2 — Prólogo del boot ✅**: `galaxy-pc` llama a `gameMain()` (el `main()` de
  `GameSystem.cpp`, renombrado por patch PC_PORT) → `DVDInit/VIInit/
  HeapMemoryWatcher::createRootHeap/nw4r lyt init/FileRipper/GameSystemException/
  MR::initAcosTable` → `GameSystem::init()` retorna → `frameLoop()`. Árboles
  enormes con stubs documentados (`GameSystemFontHolder`, `HomeButtonLayout`,
  `DrawSyncManager`, `GameSystemResetAndPowerProcess`, …) hasta que el boot
  sea real.
- **M9.3 — VI + frame control ✅**: `VIInit`/`VIWaitForRetrace` → vsync de
  `Platform::Window`; `GameSystemFrameControl` a 60 fps; el frameLoop hace
  update+draw con la escena actual.
- **M9.4 — Sistema de escenas + Logo ✅**: `GameSequenceDirector` + infra NameObj/
  Scene + `LogoScene` (`SceneRendering`, minimización del logo si hace falta).
- **M9.5 — Título + una escena con Mario**: J3D (BMD/BDL), texturas, cámaras,
  LiveActor Mario.

## M9.1 — OSThread/OSMessageQueue host layer

`src/compat/os/OSThread.cpp` implementa la superficie completa que el juego usa
(`revolution/os/OSThread.h` + `OSMessage.h`), con semántica real de
bloqueo/resume:

| Área | Superficie |
|---|---|
| Ciclo de vida | `OSCreateThread` (crea suspendido; `attr` almacenado, sin auto-start — todos los callers hacen `resume()`), `OSResumeThread` (arranca/desbloquea), `OSSuspendThread`, `OSIsThreadSuspended`, `OSIsThreadTerminated`, `OSJoinThread` (join real + limpieza del registro), `OSDetachThread`, `OSCancelThread` (cooperativo), `OSExitThread` |
| Actual | `OSGetCurrentThread` (TLS única, compartida con el trampolín) |
| Prioridad | `OSGetThreadPriority`/`OSSetThreadPriority` — almacenada, no aplicada (`TODO(PC_PORT)`) |
| Colas de hilo | `OSInitThreadQueue`, `OSSleepThread` (lista intrusiva + cv por hilo), `OSWakeupThread` |
| Mensajes | `OSInitMessageQueue`, `OSSendMessage`, `OSJamMessage` (inserta al frente), `OSReceiveMessage` (`OS_MESSAGE_BLOCK/NOBLOCK`) |
| Otros | `OSYieldThread` (yield), `OSSleepTicks` (sobre `Platform::Timing`), `OSDisable/EnableScheduler` |

Decisiones documentadas (`TODO(PC_PORT)`):
- `OSSuspendThread` no puede preemptar un `std::thread` a mitad de ejecución;
  surte efecto en la siguiente llamada OS/bloqueo del hilo.
- `attr` no se usa (no auto-start); el patrón Wii (crear + resume) es el único
  observado en el código del juego (`JKRThread::resume()` en línea).

**Detalles de implementación importantes (para no tropezar):**
- El hilo se arranca en `OSResumeThread` (crear `std::thread` cuesta µs; el
  Wii no lo costaba — el arranque sigue siendo por resume en ambos).
- `OSSleepThread` **no** espera con el mutex del registro puesto
  (`OSWakeupThread` lo necesita para despertar).
- `OSJoinThread` hace `join()` real del `std::thread` y **borra la entrada del
  registro**: si no, un `OSThread` nuevo en la misma dirección de pila falla
  con `OSCreateThread == FALSE` (lo cazó `os_thread_sleep_wakeup` en suite).
- El destructor de `HostThread` nunca destruye un `std::thread` joinable
  (join si terminado, detach si no) — evita `std::terminate` al salir.

### Tests (`src/tests/os_thread_test.cpp`)

- `os_thread_create_resume_join` — create suspendido → resume → mensaje →
  join (valor de retorno) → terminado.
- `os_message_queue_poll_and_jam` — poll en vacío, jam al frente, full+NOBLOCK.
- `os_thread_sleep_wakeup` — worker aparcado en `OSThreadQueue`,
  `OSWakeupThread`, join, flag.
- `jkr_thread_sends_message` — `JKRThread` derivado (el patrón de los hilos
  driver) enviando un mensaje; cierra con join + delete.

## El `main()` del juego (para M9.2)

`GameSystem.cpp`: `main()` → `OSInitFastCast/DVDInit/VIInit/
HeapMemoryWatcher::createRootHeap/MR::MutexHolder 0..2/nw4r::lyt::LytInit/
MR::setLayoutDefaultAllocator/SingletonHolder<HeapMemoryWatcher>/
FileRipper::setup/GameSystemException::init/MR::initAcosTable/
SingletonHolder<GameSystem>::init + init()` → `while(true) frameLoop();`.
Símbolo global `main` (declarado `#ifdef __MWERKS__` en GameSystem.hpp) →
renombrar a `gameMain()` vía patch PC_PORT y llamarlo desde `src/main.cpp`.

## M9.2 — Prólogo del boot (CERRADO ✅)

Estado: `./build/src/galaxy-pc --boot` llega al `frameLoop()` infinito
(verificado con timeout: RC=124 = sigue vivo). Compila, enlaza y la suite
sigue verde (158 passed / 0 failed / 2 skipped).

### Qué se ejecuta (en orden, desde el `main()` host)

`main.cpp --boot` → `gameMain()` (vendored, patched):
`OSInitFastCast` (inline no-op) → `DVDInit` (compat/dvd M7) → `VIInit`
(compat/vi M9.2) → `HeapMemoryWatcher::createRootHeap` → mutexes →
`nw4r::lyt::LytInit` (stub) → `MR::setLayoutDefaultAllocator` (stub) →
`HeapMemoryWatcher` singleton (crea los heaps reales: stationeed/game/wpad/
home-button sobre MEM1+MEM2) → `FileRipper::setup` → `GameSystemException::init`
(stub) → `MR::initAcosTable` (stub) → `MainLoopFramework::createManager`
(stub host) → `NameObjRegister` init → `GameSystem::init` (real) → `frameLoop()`
infinito (nerve `GameSystemInitializeAudio` → async create del `AudSystemWrapper`
vía `FunctionAsyncExecutor` real con hilos OSThread → `GameSystemInitializeLogoScene`,
donde `MR::requestChangeScene("Logo")` es stub hasta M9.4).

### Vendored real (compilado, sin tocar o con PC_PORT mínimo)

- `Game/System/GameSystem.cpp` (patched: main→gameMain + createManager +
  init NameObjRegister), `HeapMemoryWatcher.cpp` (casts), `FileRipper.cpp`
  (casts), `GameSystemFrameControl.cpp`, `GameSystemDimmingWatcher.cpp`,
  `FunctionAsyncExecutor.cpp`, `OSThreadWrapper.cpp`, `NerveExecutor.cpp`,
  `NameObj.cpp`, `NameObjRegister.cpp` (null-check), `Nerve.cpp`, `Spine.cpp`,
  `ActorStateKeeper.cpp`, `LayoutActorFlag.cpp`, `NerveUtil.cpp`,
  `MemoryUtil.cpp`, `TriggerChecker.cpp`, `JKRUnitHeap.cpp`,
  `JSystem/JKernel/*` heaps (M1/M8) + `JMath` + JAS (M8).

### Stubs host (`src/compat/game/GameBoot.cpp`), con TODO(PC_PORT) y su milestone

- M9.3: `MainLoopFramework` (+`JUTXfb::createManager`), `VI*` retrace real.
- M9.4: `GameSystemSceneController`, `GameSequenceDirector`,
  `GameSystemStationedArchiveLoader`, `SystemWipeHolder`, `GameSystemFunction` /
  `GameSequenceFunction` (namespace), `MR::requestChangeScene`,
  `NameObjHolder::add`, `MR::{isFileExist,getFileSize}` (FileUtil.cpp),
  `FunctionAsyncExecutor` → real ya; `JKRUnitHeap::create` (respaldado por
  JKRSolidHeap: en el decomp Petari JKRUnitHeap es abstracto).
- M9.5: `LayoutActor`/`HomeButtonLayout`/`HomeButtonStateNotifier`,
  `GameSystemFontHolder`, `GameSystemException` (JUT), `AudSystemWrapper`
  (M8.5), `MR::{setLayoutDefaultAllocator,initAcosTable}`, math scalars
  (getLinerValue/getEaseIn*/normalize, fórmulas SMG).
- `MR::strcasecmp`/`isEqualString` (StringUtil.cpp arrastra GameData — M9.4+);
  `MR::copyMemory`/`zeroMemory` (sin cuerpos en el decomp).

### PC_PORT fixes destacados de esta sesión

- **`HeapMemoryWatcher::createRootHeap`**: los punteros de arena MEM2 deben
  quedar en `uintptr_t` — truncarlos a u32 (como en el PPC32) produce una
  dirección 0x00d620c000 no mapeada (SIGSEGV escribiendo el vtable del heap).
- `JKRExpHeap::createRoot`: `sizeof(JKRExpHeap)` ya cubierto (0x100 host).
- `NameObjRegister::add`: null-check (primeros NameObjs antes de que exista un
  holder; M9.4 trae el NameObjHolder real).
- `FileRipper` alineación 0x40: aritmética u32 vía uintptr_t.
- Shims: `compat/include/va_list.h` → `<stdarg.h>` (MWC header).
- `JKRHeapAllocator<0>::sHeap/sAllocator/sAllocatorFunc`: especialización
  explícita host (el template no emite símbolos sin ella).
- `VICompat.cpp`: firmas exactas de `vi.h` (VIBool, VIConfigurePan de 4 args,
  `BOOL VIEnableDimming` — C linkage) + VIEnableDimming stub.

## M9.3 — VI + frame control (CERRADO ✅)

El bucle de frame real del juego ya está enlazado: `MainLoopFramework` con
pacing por retrace VI, `GameSystemFrameControl` a 60 fps y el prólogo del
boot de M9.2 intacto.

### Qué es real ahora

| Área | Archivo | Notas |
|---|---|---|
| VI | `src/compat/vi/VICompat.cpp` (+ `revolution/vi.h` host) | Reloj de campo autoritativo a 59.94 Hz (periodo 16 683 360 ns) con hilo en `VIInit`; `VIWaitForRetrace` sobre CV con cap de 30 ms; `VIGetRetraceCount`; callbacks pre/post (`VISetPre/PostRetraceCallback`); `VISetNextFrameBuffer`/`VIGetNextFrameBuffer`; `VISetBlack`/dimming; `VIGetTvFormat=VI_NTSC`, `VIGetScanMode=VI_NON_INTERLACE`, `VIGetDTVStatus=1`. `fireRetrace()` es el hook documentado para el modo present-driven (M9.5): hoy el reloj de campo es la fuente autoritativa y `GXCopyDisp` NO lo llama. |
| Alarmas | `src/compat/os/OSAlarm.cpp` | `OSCreateAlarm`/`OSSetAlarm`/`OSSetPeriodicAlarm`/`OSCancelAlarm`/tags/userData, con un hilo + lista ordenada; los handlers se despachan con el lock del sistema, así que `OSCancelAlarm` nunca retorna con el handler de un alarm todavía corriendo (garantía de la que depende `waitDrawDoneAndSetAlarm`). |
| OSGetTime | `src/compat/os/OSTime.cpp` | **ARREGLADO**: medía `nanosecondsSince(now())` sobre un `now()` capturado al vuelo → devolvía ~0 y los `OSAlarm` nunca disparaban. Ahora hay un `kProcessStart` latcheado y `OSGetTime()` cuenta ticks desde el arranque del proceso (`× 60750000 / 1e9`). |
| GX sync | `src/compat/gx/GXSync.cpp` | `GXFlush`/`GXPixModeSync`/`GXDrawDone`/`GXAbortFrame`/`GXSetDrawDone`/`GXSetDrawSync` + callbacks, `GXGetCPUFifo`/`GXGetFifoPtrs`/`GXDisableBreakPt`/`GXGetGPStatus`/`GXReadXfRasMetric`, `GXInvalidateTexAll`/`GXInvalidateVtxCache`/`GXSetLineWidth`/`GXSetZTexture`. Los callbacks se registran **de verdad** pero no se disparan (no hay CP asíncrono en el host); el único sync real es el retrace VI. |
| JUTVideo | `JSystem/JUtility/JUTVideo.cpp` (vendored) | Creador del manager, `setRenderMode`, la cola de mensajes que alimenta `waitForRetrace` vía `postRetraceProc` ← callback post-retrace del VI compat. |
| JUTXfb | `JSystem/JUtility/JUTXfb.cpp` (vendored) | Triple buffer real sobre los buffers host (`GXCopyDisp` del M5.7c copia el EFB al XFB). |
| MainLoopFramework | `patches/Game/System/MainLoopFramework.cpp` | Real, parcheado solo con un null-guard en el watchdog GX (`DrawSyncManager::sInstance` es stub hasta el hito async-GX). `waitForRetrace` → cola de JUTVideo. |
| J2D | `J2DOrthoGraph.cpp`/`J2DGrafContext.cpp` (vendored) + `jsystem/J2DGrafContextCompat.cpp` | `place(f32,f32,f32,f32)`, `getGrafType()=J2DGraf_Ortho`, `C_MTXOrtho` (mtx44.c parcheado, sin asm MWERKS). |
| Otros | `GameSystemFrameControl.cpp`, `BothDirList.cpp`, `SchedulerUtil.cpp`, `GameSystemDimmingWatcher.cpp`, `JUTDirectPrint.cpp` (vendored) | `MR::ProhibitSchedulerAndInterrupts` sobre `OSDisable/OSEnableScheduler` (no-ops compat). |

### Cuelgue de la suite: causa raíz y fix

Síntoma: con `vi_frame_test.cpp` compilado, la suite completa colgaba (timeout
122 s; última línea `JAS driver up`); sin él, pasaba en 1.9 s. El bisect
apuntaba a los tests de alarmas/VI, pero **no eran su culpa**: los tests
arrancan dos threads *daemon* (`detach`) que viven para siempre — el reloj de
campo VI y el hilo de alarmas — y el proceso colgaba **en la salida** (después
de `main`, en el camino `exit()`, con el thread principal esperando en el CV
de alarmas), no en los tests.

Diagnosis: `fprintf` escalonados en el runner y en el shutdown de audio +
lectura de `/proc/<pid>/task/*/syscall` (futex uaddr → símbolos `nm`). El
shutdown de audio se completaba (los checkpoints llegaban a `all done`) y el
runner imprimía el summary; el cuelgue era posterior a `main()`, en la fase de
exit, con solo los dos daemons vivos.

Fix (el correcto, no un parche de timing): **los daemons ya no se detachan**.
`VIInit` y `ensureAlarmThread` registran un handler `atexit` que pone el flag
de stop, notifica el CV y hace `join()` del hilo, así el binario termina
limpio y el orden de destrucción de los estáticos es seguro. Verificado: la
suite completa pasa en ~1.8 s y `ctest` da 100 %.

### Tests (`src/tests/vi_frame_test.cpp`, 6 TEST_CASE)

`vi_field_clock_ticks_at_60hz` (59.94 Hz observada), `vi_wait_for_retrace_blocks_about_one_field`
(5 `VIWaitForRetrace` secuenciales → `≥5` retraces, `dt ≥ 50 ms`), `vi_retrace_callbacks_fire`
(pre/post disparan), `os_alarm_one_shot_fires`, `os_alarm_cancel_prevents_fire`,
`os_alarm_periodic_fires_repeatedly`.

### Limitación headless

El binario `galaxy-pc` necesita un dispositivo de vídeo real (SDL3 + Vulkan):
en este sandbox sin X/Wayland falla limpio en `SDL_CreateWindow` (FATAL,
RC=134) y 2 tests se marcan `[SKIP]` (`renderer_dynamic_buffer`,
`gx_copy_efb_present_and_readback`). El smoke del boot con ventana se hace
localmente.

### Siguiente: M9.5 — Título + una escena con Mario

## M9.4 — Sistema de escenas + Logo (CERRADO ✅)

El boot real llega hasta la LogoScene y corre su máquina de nerves completa
(`StrapFadein → StrapDisplay → StrapFadeout → WaitReadDoneSystemArchive →
MountGameData → Deactive`) con el GameSystem en `Normal`. Verificado por
`scene_test.cpp` (headless) y por el smoke `galaxy-pc --boot` con ventana.

### Qué es real ahora

| Área | Archivo | Notas |
|---|---|---|
| Controlador de escenas | `Game/System/GameSystemSceneController.cpp` (**vendered tal cual** — sin parche) | La máquina de nerves completa del decomp compila en host: `requestChangeScene` → `WaitDrawDone` → `ChangeWavebank` → `InitializeScene` (asíncrono vía `FunctionAsyncExecutor`, como en consola) → `InvalidateSystemWipe` → `ReadyToStartScene` → `startScene` → `Normal`. Los heaps (file cache/scene/game) y el `AudSystemWrapper` (stub M8.5) se resuelven contra lo ya existente. |
| Escena base + LogoFader | `Game/Scene/Scene.cpp`, `Game/Screen/LogoFader.cpp` (vendered) | Compilan sin cambios sobre el `LayoutActor` host (nerve machine real vía `Spine`; el layout manager nw4r llega en M9.5). |
| LogoScene + SceneFactory | `patches/Game/Scene/LogoScene.cpp`, `patches/Game/Scene/SceneFactory.cpp` | LogoScene con null-guard del `MainLoopFramework::sManager` (tests). SceneFactory corrige el bucle roto del decomp (`break` en el match, no en el no-match) — sin eso `createScene("Logo")` devolvía siempre GameScene. Marcador de progreso `createScene('X')` por log. |
| Infra NameObj/Scene | `compat/game/SceneCompat.cpp` | `NameObjHolder`, `NameObjCategoryList` (listas reales por categoría con add/remove/execute), `NameObjListExecutor`/`SceneNameObjListExecutor`, `SceneObjHolder` + `MR::createSceneObj`, `NameObjExecuteHolder` (registro connect-to-scene real), los scene objects del boot (CameraContext/NameObjGroup/StopSceneController/SceneNameObjMovementController), las escenas pequeñas host (GameScene/Intermission/PlayTimer/ScenarioSelect/ScenarioDataParser), `SceneFunction`/`CategoryList` con el orden de ejecución vendered (menos las llamadas de draw buffer, M9.5/M10) y el glue `MR::connectToScene*`/LayoutUtil/DrawUtil. |
| Stub tree del boot | `compat/game/GameBoot.cpp` | `GameSequenceDirector::update` real (sustituye a `GameSequenceProgress`: `startScene` + `tryToLoadSystemArchive` cuando el controller está ready), `GameSystemStationedArchiveLoader` "done" inmediato (TODO M9.4+: montaje JKRArchive real), `MR::requestChangeScene` → controller, `GameSystemFunction`/`GameSequenceFunction`, LayoutActor con nerve machine real. |
| Frame del renderer | `patches/Game/System/MainLoopFramework.cpp` | **Puente PC_PORT del ciclo de frame**: `beginRender` abre `Renderer::beginFrame` + el pass del EFB (`CompatGx::getEfbRenderTarget`), `endRender` cierra el pass antes del `GXCopyDisp` (blit EFB→swapchain), `endFrame` presenta (`Renderer::endFrame` + `GXCompatEndFrame`). Si `beginFrame` falla (swapchain out-of-date) el frame host se salta: los draws GX fuera de pass se descartan (guarda `inPass()` nueva en `flushDraw`). |
| Destrucción diferida de RTs | `platform/Renderer` (`mRetiredRenderTargets`) | `destroyRenderTarget` ya NO destruye en el acto: encola y se libera en el `endFrame` tras la fence (y en shutdown). Bug real cazado por el smoke: `ensureEfb` recrea el EFB a mitad de frame (el `GXSetDispCopySrc` del `prepareCopyDisp` cambia 640×448→640×456 en el primer frame) mientras `blitPassToSwapchain` aún lee el target viejo vía `mPassTarget` → use-after-free (crash en el worker de lavapipe). Con la destrucción diferida el blit usa la imagen vieja (válida hasta la fence) y el frame siguiente ya usa la nueva. |
| `--boot` | `src/main.cpp` | `galaxy-pc --boot` = ventana + renderer + `gameMain()` (no retorna; el cierre de ventana/eventos llega con el modo present-driven de M9.5). |

### Integración de build (el commit M8-M9 añadió las fuentes SIN cablear)

Los 63 ficheros del commit "M8-M9 Done" no estaban en ningún CMakeLists — esta
sesión los cableó todos: `pc_compat` (audio M8, OSThread M9.1, boot tree M9.2,
VI/frame M9.3, escenas M9.4, parches JAS/JKernel/mtx, vendered JUT*/J2D*/JAS*/
GameSystemSceneController/Scene/LogoFader), `pc_platform` (`Audio/Audio.cpp`),
tests (audio×4, os_thread, vi_frame, scene) y el include dir de nw4r.

Fixes de portabilidad necesarios al compilar ese árbol (todos con comentario
PC_PORT in situ):

- `compat/include/revolution/os.h`: macros de direcciones con `uintptr_t`
  (`OSRoundUp32B` etc.) — el cast `(u32)` de un puntero es error duro en
  GCC/Clang/MSVC. (Ya estaba documentado en audio.md §8 pero sin commitear.)
- `compat/os/OSMutex.cpp`: registro de mutexes como singleton Meyers — los
  globales vendered (`JASHeap audioAramHeap`) llaman a `OSInitMutex` durante
  la inicialización estática, antes de que el `unordered_map` global de otro
  TU exista (SIGFPE pre-`main`: bucket count 0). (Documentado en audio.md §7,
  sin commitear.)
- `patches/JSystem/JKernel/JKRExpHeap.cpp`: `createRoot` reentrante — en consola
  se llama una vez; en la suite el root heap ya existe (jkr_heap_test) y el
  decomp dejaba `heap = nullptr` → SIGSEGV. Ahora reutiliza el root existente.
- `compat/os/PPCIntrinsics.cpp`: `__cvt_dbl_usll`/`__cvt_fp2unsigned` (runtime
  MWC, declarados en el shim `runtime.h` que ya existía) con saturación.
- `compat/jsystem/JMathCompat.cpp`: `PSMTXIdentity`/`PSMTXCopy` escalares (el
  `mtx.c` vendered es asm Paired-Singles puro).
- `compat/jsystem/JUTConsoleCompat.cpp` + `JUTExceptionCompat.cpp`:
  `JUTConsoleManager::sManager`/`draw`/`drawDirect` y
  `JUTAssertion::flushMessage`/`flushMessage_dbPrint` (host: no-op, los
  mensajes ya van directos a Platform::Log).
- `patches/RVL_SDK/mtx/mtx44.c`: `tan` con linkage C explícito (se compila
  como C++; `extern f64 tan(f64);` enmanglea y no enlaza contra libm).
- `compat/gx/GXTexture.cpp`: warning de formato no soportado una vez por
  formato — el `clear_z_tobj` (Z24X8) del clearEfb se carga CADA frame y
  llenaba el log a 60 Hz.

### Tests (`src/tests/scene_test.cpp`, 3 TEST_CASE) + validación

- `scene_factory_creates_logo_scene` — la factory parcheada crea LogoScene /
  GameScene / nullptr para nombres desconocidos.
- `logo_fader_nerve_timeline` — el LogoFader vendered: fade-in/out de 30
  frames con la nerve machine real del LayoutActor.
- `game_system_boot_reaches_logo` — el boot completo headless: prólogo
  (heaps/JUTVideo/MainLoopFramework) → `GameSystem::init` → drive por
  `update()`: audio asíncrono real (FunctionAsyncExecutor + hilos OSThread),
  `requestChangeScene("Logo")`, init asíncrono de la escena, LogoScene
  creada (dynamic_cast), su cadena de nerves hasta `Deactive`
  (`isDisplayStrapRemineder()==false`) y GameSystem en `Normal`
  (`isDoneLoadSystemArchive()`).
- Suite: **169 passed / 0 failed / 0 skipped** con display (Xvfb +
  llvmpipe + validation layers en sandbox; headless → 167 + 2 skipped).
- Smoke con ventana (Xvfb + llvmpipe en sandbox): `--boot --gpu-debug` →
  ventana 1280×720, EFB 640×448→640×456, `createScene('Intermission')`,
  primer retrace, `createScene('Logo')`, 60 fps estables 20 s, **0 errores de
  validación**.
- Pasada de validación (M9.4, con `vulkan-validationlayers` + display por
  primera vez en sandbox): destapó y corrigió 6 defectos reales del renderer
  que llevaban latentes desde M5.7c (UB en el submit de `endFrame`, swapchain
  sin `TRANSFER_DST`, aspect de depth en D24S8, dynamic state fuera de
  grabación, layouts de RT sin trackear y present de frames vacíos) — ver
  `docs/renderer.md` §8. Los 2 tests Vulkan que antes se skipeaban ya corren
  y pasan.

### Limitaciones conocidas (no bloquean M9.4)

- La pantalla del boot se queda en el clear del EFB: el gráfico del strap
  ("WiiRemoteStrap") necesita el motor de layouts nw4r (M9.5) — el
  `SimpleLayout`/`LayoutManager` es stub y el Logo "fadea a través de negro".
- `GameSystemStationedArchiveLoader` reporta "done" inmediato: el montaje real
  de los archives staged (JKRArchive/RARC sobre el VFS) es prerrequisito de
  M9.5/M10 (los assets del usuario ya se montan como FST en el DVD compat, pero
  nada consume RARC todavía).
- `--boot` no bombea eventos SDL (cerrar la ventana no sale; Alt+F4/kill).
  El modo present-driven con bombeo de eventos está documentado en M9.3
  (`fireRetrace()`).
- `MR::getFileSize` devuelve 0 (TODO) y `MathUtil.cpp` real sigue pendiente
  (M9.4/M10 → escalares math ya provistos host-side en GameBoot.cpp).

### Siguiente: M9.5 — Título + una escena con Mario

J3D (BMD/BDL), texturas, cámaras, LiveActor Mario + el motor de layouts nw4r
(los gráficos del Logo/strap y el HomeButtonLayout). Pendientes conocidos para
no tropezar: montaje JKRArchive (RARC/Yaz0) para el StationedArchiveLoader,
`DrawSyncManager` real (hito async-GX), `GameSystemObjHolder::initDisplay`
real (trae `CaptureScreenDirector`/`ScreenPreserver`), bombeo de eventos SDL
en el frame loop.
