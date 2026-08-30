# docs/porting.md — Guía de port (metodología y convenciones)

> Estado: M0. Se amplía con cada caso real resuelto (primeros módulos portados).

## 1. Cómo se porta un módulo (flujo de trabajo)

1. **Identificar el módulo** en `third_party/petari/src/...` y añadirlo a la lista de fuentes de CMake (build PC).
2. **Compilar en host** (`cmake --build build`). Recopilar los errores:
   - Errores de **toolchain** (asm PPC, intrinsics, placement new, `ptmf`, macros de CodeWarrior) → solucionar con parche de port (ver §3).
   - **Símbolos faltantes** del SDK de consola → añadir a `src/compat/` la implementación (o stub `TODO(PC_PORT)` si el comportamiento no es necesario todavía).
   - Errores de **endianness/layout** (lecturas que asumen BE host) → corregir en frontera (§4).
3. **Enlazar** y resolver dependencias hasta que el módulo **compile y enlace**.
4. **Tests**: si el módulo es de lógica pura (heaps, parsers, matemáticas, VFS), escribir tests unitarios en `src/tests/` y pasar `ctest`.
5. **Documentar** en el fichero correspondiente de `docs/`: qué se tocó, qué stubs quedan, qué decisión se tomó.
6. **Commit** con mensaje descriptivo. Cada milestone acaba con build+ctest verdes.

## 2. Convenciones de código del port

- **Idioma de código:** comentarios y nombres en **inglés** (coherente con el código upstream); documentación en **español** (docs/).
- **`#ifdef` de plataforma:** SOLO en `src/platform/windows/` y `src/platform/linux/`. El resto del árbol (game code, compat, renderer) no conoce la plataforma.
- **Macro de port:** `PC_PORT` (definida solo en el build PC). Se usa ÚNICAMENTE para aislar trozos del game code que no compilan en host (asm PPC, etc.) con el mínimo cambio posible. No se usa para lógica de plataforma.
- **Stubs:** todo stub incompleto lleva:
  ```cpp
  // TODO(PC_PORT): <qué debería hacer realmente> <módulo del que depende>
  ```
  y devuelve un valor seguro (no crashear). Los stubs se listan en el README de `src/compat/`.
- **Logging:** `Platform::Log` con niveles TRACE/DEBUG/INFO/WARN/ERROR/FATAL. El compat GX loguea a TRACE; el arranque a INFO; los fallos a ERROR/FATAL.
- **Sin código muerto:** no dejar funciones portadas sin usar; si una API de consola no la usa el juego, ni se implementa (inventario: docs/gx.md §3).
- **Sin abstracciones innecesarias:** si solo hay una implementación posible hoy, no crear interfaz virtual (YAGNI; ADR-006).

## 3. Parches de port (cómo tratar el código que no compila)

Los ficheros de Petari se mantienen intactos en el submodule. Cuando un fichero no compila en host:

1. **Preferir parche quirúrgico con `#ifdef PC_PORT`** dentro del propio fichero del submodule… **pero el submodule no se puede modificar** (ADR-012). Alternativas:
   - (a) **Parche en nuestro árbol**: copiar el fichero ofensivo a `src/compat/patches/<ruta>`, arreglarlo, y que CMake compile la copia en lugar del original (el original se excluye). El parche se documenta en `patches/README.md` con el motivo.
   - (b) Si el asm está aislado en una función (caso típico: `JMath.cpp` frsqrte), reescribir solo esa función con C equivalente (`1.0f / sqrtf`, o `std::sqrt` con `-ffast-math` desactivado para fidelidad) y mantener el resto del fichero.
2. **Casos conocidos (inventario inicial):**
   - `Game/Util/MathUtil.cpp` — asm en `initAcosTable`/funciones de math rápida.
   - `JSystem/JMath/JMath.cpp` — `asm { frsqrte ... }`.
   - `JSystem/J3DGraphBase/J3DTransform.cpp` — `J3DPSCalcInverseTranspose` (asm).
   - `nw4r/db/db_assert.cpp` — asm en assertion.
   - `Game/LiveActor/ShadowVolumeOvalPole.cpp`, `Game/MapObj/ClipAreaShape.cpp`, `J3DAnimation.cpp`, `J3DMtxBuffer.cpp`, `J3DShapeMtx.cpp`, `J3DSys.cpp` — asm/`__REGISTER` (verificar uno a uno).
3. **`__REGISTER` y `__attribute__((register))`** (sintaxis mwcc): reemplazar por variables locales normales.
4. **Placement `new (heap, align)`**: es C++ estándar si JKR declara `operator new(size_t, JKRHeap*, int)` — verificar que esas overloads se compilan; si no, añadir las overloads en `compat/`.

### 3.1 Quirks de toolchain host descubiertos (M1/M2/M3)

Problemas que NO son asm PPC y que no aparecen en la decomp (PPC32/mwcc), pero rompen el build/runtime en host. Revisar ante cualquier síntoma raro:

1. **Máscaras de 32 bits truncan punteros 64-bit.** `ptr & ~0x1Fu` → `~0x1Fu` es `unsigned int` (0xFFFFFFE0) que se extiende con ceros a `0x00000000FFFFFFE0`; el AND pierde los 32 bits altos del puntero. En PPC32 funcionaba. Corregido en `ALIGN_PREV/ALIGN_NEXT` (`(uintptr_t)(N)`). **Regla: cualquier `& ~máscara32` sobre un puntero es un bug latente.** Síntoma típico: punteros tipo `0xd7a0c080` (truncados) en lugar de `0x7fff…`.
2. **No asumir tamaños de layout PPC32.** `JKRExpHeap` mide 0x100 en x86-64 (0x90 en PPC32); `CMemBlock` 0x18 (0x10 en PPC). `createRoot` hardcodeaba 0x90 → el primer heap hijo se escribía DENTRO del objeto raíz (corrupción silenciosa de `mAllocMode`/`mHeadFreeList`). **Regla: usar `sizeof`/`ALIGN_NEXT(sizeof(...))`, nunca constantes de layout PPC.** Verificar layouts con `gdb ptype /o`.
3. **`std::thread::id` no se formatea igual en todos los toolchains.** En glibc streama decimal; en MinGW/MSVC streama como puntero hexadecimal (`0x…`). No parsear el stream: copiar los bytes (`memcpy`, es trivially copyable) o usar la API nativa.
4. **`__attribute__` sin guarda en headers vendered.** `Inline.hpp` define `NO_INLINE __attribute__((noinline))` incondicionalmente → inválido en MSVC. Override con `__declspec(noinline)` para MSVC.
5. **`std::atomic<Stats>` (48 B, no lock-free)** necesita `-latomic` con GCC/MinGW; MSVC lo provee en la CRT. El CMake enlaza `atomic` solo donde hace falta.
6. **Comparaciones `s32` vs `u32` del original no son intercambiables.** En `allocFromHead`, `foundSize = -1` es un centinela que solo funciona si la comparación se hace en u32 (el -1 se convierte a 0xFFFFFFFF). Castear `block->mSize` a `s32` rompe la lógica (el primer bloque se salta siempre). Mantener la semántica original.
7. **Registros/estáticos que asignen dentro de locks**: el `operator new` global enruta al heap JKR (que toma `OSLockMutex`). Asignar con `operator new` dentro de un lock del registro de mutexes → deadlock (reentrada). Usar `Platform::Memory::allocate` + placement-new + allocator propio en el contenedor (ver `compat/os/OSMutex.cpp`).
8. **MSVC resuelve `#include "..."` con la cadena completa de includes**: busca el fichero relativo al directorio de cada archivo de la cadena de inclusión, no solo el inmediato. Un `#include "revolution/gx/GXVert.h"` desde un header vendered puede tomar el vendered aunque nuestro override esté primero en el include path (en GCC/Clang solo se busca el dir inmediato + include dirs → gana nuestro override). **Solución: en los overrides de cabeceras "paraguas" (p. ej. `revolution/gx.h`) usar `<...>` con ángulos**, que resuelve siempre por el include path. Síntoma: errores C2039 "no es un miembro de uPPCWGPipe" solo en MSVC.
9. **MSVC no acepta atributos de función en posición postfija.** El código del juego (y de JSystem) declara `int f(...) NO_INLINE {` con la macro DESPUÉS de la firma. GCC/Clang aceptan `__attribute__((noinline))` postfijo; MSVC solo acepta `__declspec` al INICIO de la declaración. Si `NO_INLINE` expande a `__attribute__((noinline))` (el vendered) MSVC da C2059 en cada uso; si expande a `__declspec(noinline)` postfijo, da C2143/C2059/C2334 ("falta ';' delante de conversión de estilo de función") en todas las funciones afectadas y puede desincronizar el parser (C2144 raros en cabeceras siguientes). **Solución: en MSVC `NO_INLINE` debe expandir a NADA** (`#define NO_INLINE`), con `#ifndef` para no redefinir el fallback de `types.h` (C4005). El `noinline` es solo una sugerencia de optimización: perderlo no cambia la semántica.
10. **El `operator new` global del juego con alineación 4 rompe el runtime x86-64.** El port enruta el `new` plano a través de JKR (`JKRHeap::alloc(size, 4, ...)`) en cuanto existe el root heap — fiel a la consola (PPC no exige alineación estricta). En x86-64, el runtime de C++ y cualquier librería `dlopen`-ada (SDL, lavapipe/LLVM, ...) exigen ≥ `alignof(max_align_t)` = 16 para el `new` plano: una tienda `movaps` a un puntero con alineación 4 → SIGSEGV. Síntoma clásico: crash dentro de `dlopen` (ctor de `llvm::Regex`) solo cuando el test/librería se ejecuta DESPUÉS de haber creado el root heap. **Solución: el `new`/`new[]` plano enruta con alineación 16** (`kPcNewAlign`); el código del juego que necesita más alineación ya usa las formas explícitas (`new (align)`, `new (heap, align)`), así que el comportamiento del juego no cambia. (Ver `JKRHeap.cpp` bloque PC_PORT.)
11. **SDL3 cambió las firmas de Vulkan respecto a SDL2.** `SDL_Vulkan_GetInstanceExtensions` en SDL3 NO recibe ventana: `const char* const* SDL_Vulkan_GetInstanceExtensions(Uint32* count)` (devuelve `nullptr` si falla; sin el doble paso "count luego rellenar" de SDL2). `SDL_Vulkan_CreateSurface(window, instance, allocator, &surface)` lleva un tercer parámetro `const VkAllocationCallbacks*` obligatorio (puede ser `nullptr`). En SDL2 eran `(window, count, names)` y `(window, instance, &surface)`.
12. **`SDL_WINDOW_VULKAN` es obligatorio al crear la ventana.** Sin ese flag, `SDL_Vulkan_CreateSurface` falla con "The specified window isn't a Vulkan window". SDL3 mantiene el flag (`0x10000000`); añadirlo junto a `HIGH_PIXEL_DENSITY | RESIZABLE`.
13. **No anidar un namespace cuyo nombre coincide con una clase del mismo ámbito.** `namespace Platform::Renderer::Detail {}` es ilegal si `class Renderer` existe en `namespace Platform` ("redeclared as different kind of entity"). Usar un nombre distinto (`Platform::RenderDetail`). Síntoma en GCC: errores en cadena (usos de `Detail::stringify`/`pickPhysicalDevice` sin declarar, `vector` no miembro de `std`) porque el namespace no se abrió.
14. **El header público de un módulo de plataforma no puede usar tipos de volk/Vulkan NI en métodos privados** (el header no debe depender de volk: M4.1 usó `void*` para los handles, pero los helpers privados `recordBeginPass`/`setDebugName` los declaré con `VkImageView`/`VkObjectType` → `error: 'VkImageView' has not been declared` al incluir el header sin volk). Solución: firmas opacas (`void*`, `uint32_t`) y cast dentro del .cpp. Y si un helper libre del .cpp necesita los miembros privados, **no lo definas en un namespace anónimo**: el `friend struct X;` del header declara `Platform::X`, y el namespace anónimo crea OTRA entidad distinta (`Platform::{anon}::X`) → `reference to 'X' is ambiguous` y la friend no aplica. Forward-declara `struct X;` antes de la clase, haz `friend struct X;`, y define `Platform::X` en el .cpp en namespace nombrado.
15. **`VK_KHR_swapchain` debe habilitarse en el device aunque "no lo uses directamente".** Si falta, `vkGetDeviceProcAddr` devuelve NULL para `vkCreateSwapchainKHR`/`vkGetSwapchainImagesKHR`/... y **volk los deja NULL** → crash en la primera llamada al swapchain. Síntoma: segfault en `recreateSwapchainInternal` tras crear el device (en M4.1 lo perdí al refactorizar; en M3 estaba).

## 4. Endianness — reglas

- **No asumir endianness del host.** Los parsers del juego ya construyen valores con shifts en su mayoría (p. ej. lecturas de JKRArchive). Si se encuentra una lectura `*(u32*)ptr` sobre un buffer de datos del juego, es un bug de port → sustituir por lectura explícita (utilidad `ReadBE32` en `src/platform/Filesystem` o `compat`).
- **Conversiones de formato de textura** (GX→Vulkan) son un punto caliente: tests obligatorios (`src/tests/texture_conversion_test.cpp`).
- Los **ficheros de guardado** (`compat/nand`) conservan el formato BE del juego (portabilidad con la partida de la consola).
- Utilidad `tools/verify-assets` revisa la integridad de los assets extraídos (no su contenido legal — solo estructura/sumás de cabeceras).

## 5. Input — decisión de diseño (M6)

Mapeo Wiimote → PC (propuesta inicial, refinable con el usuario):

| Acción del juego | Mando Wii | PC — gamepad | PC — teclado/ratón |
|---|---|---|---|
| Movimiento | Nunchuk stick | Stick izquierdo | WASD |
| Cámara | — | Stick derecho | Ratón (movimiento) |
| Puntero estrella / UI | Wiimote IR | **Modo puntero**: stick derecho (tipo "gyro") o ratón | Ratón 1:1 |
| A (saltar/confirmar) | A | A (Xbox) / Cruz | Espacio / Enter |
| B (atacar/girar) | B | B (Xbox) / Círculo | Ctrl / Clic izq. |
| Shake (giro) | Sacudir Wiimote | Botón L/R o botón designado | Tecla asignada |
| 1/2, +/- | Botones Wiimote | Select/Start | Teclas F1-F4 |
| Home (menú) | Home | Botón guía | Escape |

- **Nada de teclas hardcodeadas en el gameplay:** el game code sigue usando `WPAD/KPAD`; `compat/wpad/kpad` traduce `Platform::Input` (estado abstracto: botones, sticks, puntero, shake) → los mapeos concretos viven en config (fichero `input` del port) y en `platform/Input`.
- **Wiimote real (opcional, post-M6):** SDL3 no soporta Wiimote nativamente como gamepad estándar fiable; se valorará vía Bluetooth (HID) directo o biblioteca especializada, detrás de la misma `Platform::Input`. No bloquea nada.

## 6. Audio — decisión de diseño (M8)

- El juego (JAudio2) mezcla en CPU por hilo propio y espera que el DSP haga la mezcla final. Nuestro `compat/ax` recibe los buffers mezclados de JASDriver y los **mezcla a PCM estéreo en CPU** (`Platform::Audio` → SDL3 callback).
- El **formato de datos de audio** (BNK, onda, remix) se lee tal cual desde el VFS (son parsers byte-oriented).
- El **micrófono** (`AudMicWrap`, `AudSpeakerWrap`) → stub `TODO(PC_PORT)` (no necesario para jugar; el juego lo usa en 2 niveles concretos — verificar y documentar).
- **Stub temprano:** hasta M8, `compat/ax` puede ser un silencio seguro (el juego arranca sin audio; `AudSystemWrapper` ya contempla estados de carga asíncrona).

## 7. Dónde vive cada cosa (mapa rápido)

| Necesito… | Voy a… |
|---|---|
| Implementar `DVDOpen` | `src/compat/dvd/dvd.cpp` (usa `Platform::Filesystem`) |
| Ver qué GX usa el juego | `docs/gx.md` §3 + script de inventario |
| Añadir un GX nuevo | `src/compat/gx/gx_<categoria>.cpp`, actualizar tabla de gx.md |
| Un log | `Platform::Log::info(...)` |
| Un stub de NWC24 | `src/compat/nwc24/` con `TODO(PC_PORT)` |
| Un test de endianness | `src/tests/endian_test.cpp` |
| Compilar | `cmake -B build -DPLATFORM=PC && cmake --build build` |
| Ejecutar tests | `ctest --test-dir build` |
| Ver el estado del port | README.md del repo (tabla de módulos portados/stub) |

## 8. Checklist de revisión de código

- [ ] ¿`#ifdef` de plataforma fuera de `platform/windows|linux`? → rechazar.
- [ ] ¿Stub sin `TODO(PC_PORT)`? → añadir.
- [ ] ¿Lógica duplicada con el código upstream? → reutilizar el upstream.
- [ ] ¿Test para la parte portable? → escribir (o justificar por qué no).
- [ ] ¿Docs actualizadas (architecture/gx/renderer/build/porting)? → actualizar la sección afectada.
