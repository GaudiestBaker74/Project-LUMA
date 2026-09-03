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
| `Game/System/GameSystem.cpp` | (1) `void main(void)` → `void gameMain(void)` (el entry point PC es `src/main.cpp`). (2) Prólogo de vídeo host en `gameMain`: `GXRenderModeObj` NTSC-prog 640×456 + `JUTVideo::createManager` + 3 XFB del stationed heap + `MainLoopFramework::createManager` + `JUTDirectPrint::start` + init del `NameObjRegister` (equivalente a `initRenderMode`/`initDisplay` de consola). (3) **M9.5.1**: `JKRFileLoader::initializeVolumeList()` antes del LytInit — sin ella `sVolumeList` queda null y el primer montaje de archivo revienta. |
| `Game/System/HeapMemoryWatcher.cpp` | Casts puntero→u32 vía `uintptr_t` en `createRootHeap` (truncar a 32 bits daba una dirección MEM2 no mapeada → SIGSEGV). |
| `Game/System/MainLoopFramework.cpp` | (1) Null-guard del watchdog GX (`DrawSyncManager::sInstance` es stub). (2) Literales float MWERKS (`0f`/`1f`) inválidos en C++. (3) Marcador de primer retrace. (4) **M9.4**: puente del ciclo de frame del renderer — `beginRender` abre `beginFrame`+pass del EFB, `endRender` cierra el pass antes del `GXCopyDisp`, `endFrame` presenta (`Renderer::endFrame`+`GXCompatEndFrame`); flag `sHostFrameActive`. |
| `Game/Util/BothDirList.cpp` | Include del paraguas `Game/Util.hpp` → solo `BothDirList.hpp` (el paraguas arrastra árboles no compilados). |
| `Game/Util/StringUtil.cpp` | `#include <cstring>` (MWC daba strncpy/strrchr implícitos). |
| `JSystem/JAudio2/JASAiCtrl.cpp` | ABI de punteros de 32 bits: `AIInitDMA`/`JASDsp::syncFrame` reciben *tokens* del registro `compat::audio` en vez de direcciones (ver docs/audio.md §2). |
| `JSystem/JAudio2/JASCalc.cpp` | `reinterpret_cast<u32>` → `uintptr_t` en los checks de alineación de bcopy/bzero. |
| `JSystem/JAudio2/JASDSPInterface.cpp` | `setWaveInfo`: `_118` mantiene la palabra de 32 bits — el caller guarda el token del registro de punteros. |
| `JSystem/JAudio2/JASHeapCtrl.cpp` | Casts puntero→u32 vía `uintptr_t` en `initRootHeap`/`alloc`. |
| `JSystem/JKernel/JKRExpHeap.cpp` | (1) `createRoot` reserva `sizeof(JKRExpHeap)` (0x100 en host; upstream hardcodea 0x90 de PPC32). (2) **M9.4**: `createRoot` reentrante — si ya hay root heap (suite de tests), lo reutiliza en vez de dejar `heap=nullptr` y escribir `mAllocMode` sobre null (SIGSEGV). |
| `JSystem/JKernel/JKRArchivePub.cpp` | **M9.5.1**: (1) `JKRArchive::mount` MEM-only — ARAM/DVD/COMP sin portar (log WARN + `nullptr` en vez de fallo de link). (2) Definiciones de estáticos que el decomp declara y nunca define: `gCurrentFileLoader`, `sVolumeList`, `sCurrentDirID`, y la base `JKRArchive::getExpandedResSize` (fallback = `getResSize`; los subclases comprimidas la sobrescriben). |
| `JSystem/JKernel/JKRDecomp.cpp` | **M9.5.1**: decodificadores Yaz0/Yay0 síncronos y BE-safe — sin la heurística de cola de mensajes del decomp (no hay hilo de interrupción DVD en host); las cabeceras `SZS`/`Yaz0` se leen con swap de bytes, nunca vía struct pointer. |
| `JSystem/JKernel/JKRDvdFile.cpp` | **M9.5.1**: `doneProcess` localiza al `JKRDvdFile` dueño recorriendo `sDvdList` — upstream lee el back-pointer en el offset PPC32 `+0x3c` de `DVDFileInfo`, que en el layout del host cae dentro del command block (basura → mensaje de completion a dirección aleatoria). |
| `JSystem/JKernel/JKRDvdRipper.cpp` | **M9.5.1**: (1) lectura de cabecera Yaz0 BE-safe (bytes sueltos, no struct). (2) bucles de reintento eliminados — el fallo del DVD host es determinista, reintentar colgaría. (3) WARN cuando `JKRDvdFile::open` falla. |
| `JSystem/JKernel/JKRFileFinder.cpp` | **M9.5.1**: el decomp declara `JKRFileFinder::~JKRFileFinder()` sin cuerpo (la vtable no enlaza) — se aporta `{} `. Resto idéntico a upstream. |
| `JSystem/JKernel/JKRHeap.cpp` | (1) `OSPhysicalToCached(0)` (macro PPC de traducción de direcciones) → `compat::getBootInfo()` (stub del boot header en `compat/os`). (2) `operator new/new[]` con primer parámetro `u32` → `size_t` (el estándar C++ lo exige; GCC/Clang/MSVC rechazan `operator new(u32)`). (3) **M9.5.1**: definición de `JKRHeap::sGameHeap` — declarado en el header, nunca definido por el decomp (lo referencian `getFirstFile`/`prepareCommand`); zero-init es válido: los callers hacen fallback a `sCurrentHeap`. |
| `JSystem/JKernel/JKRMemArchive.cpp` | **M9.5.1**: (1) las dos variantes de `open()` y `mountFixed` convierten el RARC big-endian a formato host vía `compat::RarcHost` (dos pasos: cabecera/info y dirs/files; la entrada de fichero on-disk son **20 bytes**, con flag/nameOffset decodificados a mano). (2) `mFileDataStart = blob + headerSize + fileDataOffset`. (3) En fallo de conversión se libera el blob del ripper a mano (`_6C` aún es false; el destructor no lo vería). (4) Array host de `SDIFileEntry` propio (`mHostFileEntries`), liberado en el destructor. |
| `JSystem/JKernel/JKRSolidHeap.cpp` | `do_free(void*)` declarado por el header (slot de vtable) pero sin cuerpo en el decomp — se aporta. |
| `nw4r/lyt/lyt_texMap.cpp` | **M9.5.2**: `TexMap::ReplaceImage(TPLPalette*, u32)` convierte el TPL big-endian a un árbol de structs nativos vía `Platform::CompatGx::tplToHost` (conversión cacheada por blob). En consola se hacía `TPLBind` in-place con aritmética de offsets de 32 bits; en host los punteros son de 64 bits y ese layout no encaja. |
| `nw4r/lyt/lyt_textBox.cpp` | **M9.5.2**: (1) `NW4R_UT_RUNTIME_TYPEINFO_DEFINITION(TextBox, Pane)` — el decomp declara el RTTI de TextBox pero nunca lo define (fallo de link; las demás clases de pane sí tienen el suyo). (2) El texto on-disk es UTF-16BE en unidades `wchar_t` de consola (2 bytes): se lee como u16 BE y se amplía al `wchar_t` del host (4 bytes); `textBufBytes/2` por la misma razón. |
| `nw4r/ut/ut_ResFont.cpp` | **M9.5.2**: `ResFont::SetResource` detecta un brfnt big-endian (`RFNT` + marca BOM) y lo convierte a structs nativos (`FontInformation`/`FontTextureGlyph`/`FontWidth`/`FontCodeMap`) con caché por blob fuente; las imágenes de sheet se quedan en el blob original (texeles crudos, sin bytes que invertir). |
| `nw4r/ut/ut_TextWriterBase.cpp` | **M9.5.2**: aporta `PrintImpl` (declarado en el header, nunca definido por el decomp; la instanciación explícita `template class TextWriterBase<char/wchar_t>` al final del TU lo fuerza en el link) y la definición genérica del estático `mDefaultTagProcessor`. |
| `RVL_SDK/gd/GDBase.c` | `GDFlushCurrToMem`: `DCFlushRange` (flush de caché PPC) → no-op en PC. |
| `RVL_SDK/gx/GXLight.c` | Solo los builders puros `GXInitLight*` (escriben el struct del caller); las funciones que tocan registros HW se eliminaron. Se compila como C++ (los overrides de headers GX son C++). |
| `RVL_SDK/mtx/mtx44.c` | (1) `PSMTX44Identity`/`C_MTXOrtho`: asm PPC → stores escalares. (2) **M9.4**: `tan` con linkage C explícito bajo `__cplusplus` (se compila como C++; sin eso enmanglea y no enlaza contra libm). |

## Headers shadow (`src/compat/include`)

Un *header shadow* es una copia de un header vendored en `src/compat/include`
que gana al original por orden del include path (compat va primero). Misma
regla que los .cpp: banner `PC_PORT` + cambio mínimo justificado.

| Header shadow | Cambio (`PC_PORT`) |
|---|---|
| `JSystem/JKernel/*.hpp` | **M9.5.1**: las declaraciones de placement-new (`operator new(u32, JKRHeap*, int)` etc.) están tras `#ifdef __MWERKS__` upstream; el shadow las expone para GCC/Clang/MSVC. |
| `nw4r/lyt/common.h` | **M9.5.2**: `detail::GetSignatureInt` leía la firma de 4 chars con `*reinterpret_cast<const s32*>(sig)` — una lectura de palabra big-endian en consola, que en host LE sale con los bytes al revés y hace que `Layout::Build` no reconozca ninguna sección. El shadow ensambla la palabra byte a byte (MSB primero, igual que los multichar literals de MWERKS) y de paso elimina la carga desalineada. |
| `nw4r/lyt/pane.h` | **M9.5.2**: el typedef no-MWERKS `LinkList<Pane, 0>` asumía `mLink` en el offset 0, pero `Pane` hereda de `PaneBase` (dtor virtual → vptr antes de `mLink`, offset 8 en 64 bits). Con offset 0, `PushBack` escribe prev/next encima del vptr y la primera llamada virtual al pane encadenado revienta (lo cazó `lyt_build_minimal_layout`). Ahora usa `offsetof` (conditionally-supported; GCC/Clang/MSVC lo aceptan). |
| `nw4r/lyt/group.h` | **M9.5.2**: idem para `GroupList` (`Group` tiene dtor virtual). `PaneLinkList` se queda con offset 0: `PaneLink` no tiene virtuales y su `mLink` es el primer miembro. |
| `nw4r/lyt/layout.h` | **M9.5.2**: idem para `AnimTransformList` (`AnimTransform` tiene virtuales). |
