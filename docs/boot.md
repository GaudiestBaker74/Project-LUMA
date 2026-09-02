# Boot (M9) — primer boot real

Status: **M9.1 (capas de hilos) ✅** — 158 tests, 0 failed. El resto de M9 está
planificado abajo; cada sub-hito cierra con "compila + enlaza + ctest verde".

## Plan M9 (sub-hitos)

- **M9.1 — Capa de hilos del SO ✅**: `OSThread`/`OSThreadQueue`/`OSMessageQueue`
  del SDK sobre `std::thread` (`src/compat/os/OSThread.cpp`) + `JKRThread.cpp`
  vendered compilado. Base de todo lo que viene: `JASDvdThread`/`JASAudioThread`
  reales (pueden sustituir el host glue de M8), `FunctionAsyncExecutor`
  (`MR::startFunctionAsyncExecute`), thread-switch.
- **M9.2 — Prólogo del boot**: `galaxy-pc` llama a `gameMain()` (el `main()` de
  `GameSystem.cpp`, renombrado por patch PC_PORT) → `DVDInit/VIInit/
  HeapMemoryWatcher::createRootHeap/nw4r lyt init/FileRipper/GameSystemException/
  MR::initAcosTable` → `GameSystem::init()` retorna → `frameLoop()`. Árboles
  enormes con stubs documentados (`GameSystemFontHolder`, `HomeButtonLayout`,
  `DrawSyncManager`, `GameSystemResetAndPowerProcess`, …) hasta que el boot
  sea real.
- **M9.3 — VI + frame control**: `VIInit`/`VIWaitForRetrace` → vsync de
  `Platform::Window`; `GameSystemFrameControl` a 60 fps; el frameLoop hace
  update+draw con la escena actual.
- **M9.4 — Sistema de escenas + Logo**: `GameSequenceDirector` + infra NameObj/
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

### Siguiente: M9.4 — Sistema de escenas + Logo

`GameSequenceDirector` + infra NameObj/Scene + `LogoScene` (`SceneRendering`).
Pendientes conocidos para no tropezar: `MathUtil.cpp` real (M9.4/M10),
`DrawSyncManager` real (hito async-GX), `GameSystemObjHolder::initDisplay`
real (trae `CaptureScreenDirector`/`ScreenPreserver`).
