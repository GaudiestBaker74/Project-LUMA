# compat/patches — parches de fuente de Petari para el build PC

**Regla:** el código vendered (third_party/petari) no se modifica. Cuando un
fichero .cpp no puede compilar/correr correctamente en host, se copia aquí
(manteniendo la misma ruta relativa), se parchea y CMake compila **esta copia**
en lugar del original.

Cada parche lleva un comentario `PC_PORT` en los cambios, explicando:
qué cambia, por qué, y qué pasaría sin el parche.

## Inventario

| Fichero parcheado | Cambios (`PC_PORT`) |
|---|---|
| `Game/NameObj/NameObj.cpp` | El ctor solo registra en el singleton `NameObjRegister` si ya está inicializado (los primeros NameObj del boot pueden existir antes). |
| `Game/NameObj/NameObjRegister.cpp` | `add()` tolera un holder actual nulo. |
| `Game/Scene/LogoScene.cpp` | Null-guard de `MainLoopFramework::sManager` en ctor/dtor (los tests crean la escena sin manager). |
| `Game/Scene/SceneFactory.cpp` | (1) El bucle del decomp hacía `break` en el primer NO-match (siempre devolvía GameScene); restaurado el break en el match → `createScene("Logo")` crea LogoScene. (2) Marcador de progreso `createScene('X')` por log (el init de escena es asíncrono). |
| `Game/System/FileRipper.cpp` | Cast puntero→u32 vía `uintptr_t` (alineación 0x40 con punteros de 64 bits). |
| `Game/System/GameSystem.cpp` | (1) `void main(void)` → `void gameMain(void)` (el entry point PC es `src/main.cpp`). (2) Prólogo de vídeo host en `gameMain`: `GXRenderModeObj` NTSC-prog 640×456 + `JUTVideo::createManager` + 3 XFB del stationed heap + `MainLoopFramework::createManager` + `JUTDirectPrint::start` + init del `NameObjRegister` (equivalente a `initRenderMode`/`initDisplay` de consola). |
| `Game/System/HeapMemoryWatcher.cpp` | Casts puntero→u32 vía `uintptr_t` en `createRootHeap` (truncar a 32 bits daba una dirección MEM2 no mapeada → SIGSEGV). |
| `Game/System/MainLoopFramework.cpp` | (1) Null-guard del watchdog GX (`DrawSyncManager::sInstance` es stub). (2) Literales float MWERKS (`0f`/`1f`) inválidos en C++. (3) Marcador de primer retrace. (4) **M9.4**: puente del ciclo de frame del renderer — `beginRender` abre `beginFrame`+pass del EFB, `endRender` cierra el pass antes del `GXCopyDisp`, `endFrame` presenta (`Renderer::endFrame`+`GXCompatEndFrame`); flag `sHostFrameActive`. |
| `Game/Util/BothDirList.cpp` | Include del paraguas `Game/Util.hpp` → solo `BothDirList.hpp` (el paraguas arrastra árboles no compilados). |
| `Game/Util/StringUtil.cpp` | `#include <cstring>` (MWC daba strncpy/strrchr implícitos). |
| `JSystem/JAudio2/JASAiCtrl.cpp` | ABI de punteros de 32 bits: `AIInitDMA`/`JASDsp::syncFrame` reciben *tokens* del registro `compat::audio` en vez de direcciones (ver docs/audio.md §2). |
| `JSystem/JAudio2/JASCalc.cpp` | `reinterpret_cast<u32>` → `uintptr_t` en los checks de alineación de bcopy/bzero. |
| `JSystem/JAudio2/JASDSPInterface.cpp` | `setWaveInfo`: `_118` mantiene la palabra de 32 bits — el caller guarda el token del registro de punteros. |
| `JSystem/JAudio2/JASHeapCtrl.cpp` | Casts puntero→u32 vía `uintptr_t` en `initRootHeap`/`alloc`. |
| `JSystem/JKernel/JKRExpHeap.cpp` | (1) `createRoot` reserva `sizeof(JKRExpHeap)` (0x100 en host; upstream hardcodea 0x90 de PPC32). (2) **M9.4**: `createRoot` reentrante — si ya hay root heap (suite de tests), lo reutiliza en vez de dejar `heap=nullptr` y escribir `mAllocMode` sobre null (SIGSEGV). |
| `JSystem/JKernel/JKRHeap.cpp` | (1) `OSPhysicalToCached(0)` (macro PPC de traducción de direcciones) → `compat::getBootInfo()` (stub del boot header en `compat/os`). (2) `operator new/new[]` con primer parámetro `u32` → `size_t` (el estándar C++ lo exige; GCC/Clang/MSVC rechazan `operator new(u32)`). |
| `JSystem/JKernel/JKRSolidHeap.cpp` | `do_free(void*)` declarado por el header (slot de vtable) pero sin cuerpo en el decomp — se aporta. |
| `RVL_SDK/gd/GDBase.c` | `GDFlushCurrToMem`: `DCFlushRange` (flush de caché PPC) → no-op en PC. |
| `RVL_SDK/gx/GXLight.c` | Solo los builders puros `GXInitLight*` (escriben el struct del caller); las funciones que tocan registros HW se eliminaron. Se compila como C++ (los overrides de headers GX son C++). |
| `RVL_SDK/mtx/mtx44.c` | (1) `PSMTX44Identity`/`C_MTXOrtho`: asm PPC → stores escalares. (2) **M9.4**: `tan` con linkage C explícito bajo `__cplusplus` (se compila como C++; sin eso enmanglea y no enlaza contra libm). |
