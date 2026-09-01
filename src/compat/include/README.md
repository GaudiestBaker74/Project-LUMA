# compat/include — cabeceras con parche PC_PORT

Este directorio se añade al include path **con prioridad sobre** `third_party/petari/libs/RVL_SDK/include`. Cualquier fichero aquí **sustituye** a su homónimo upstream.

## Regla de uso

1. **No** copiar cabeceras aquí "por si acaso". Solo cuando la cabecera original no compila en host o produce tipos/layout incorrectos (ver `docs/porting.md` §3).
2. Cada copia lleva un bloque de comentario `PC_PORT PATCH` al principio explicando:
   - qué cambia exactamente,
   - por qué es necesario,
   - qué consecuencias tendría no parchear.
3. El resto del fichero debe ser idéntico al upstream para minimizar divergencia.

## Inventario actual

| Cabecera | Motivo del parche |
|---|---|
| `revolution/types.h` | (1) `s32/u32` usaban `signed long`, que es 64-bit en Linux x86-64/aarch64 → tipos de ancho fijo. (2) `#define nullptr 0` y `#define override` rompen C++ moderno → eliminados. (3) `ROUND_UP_PTR` truncaba punteros a `u32` → `uintptr_t`. (4) `ALIGN_PREV/ALIGN_NEXT`: `~((N)-1)` es máscara de 32 bits que trunca punteros 64-bit → cast a `uintptr_t`. (5) `NO_INLINE` con guard para no redefinir el de JSystem. |
| `Inline.hpp` | El `NO_INLINE __attribute__((noinline))` incondicional del upstream es inválido en MSVC y, además, el juego lo usa en posición postfija (`int f() NO_INLINE`), que MSVC no acepta ni con `__declspec`, **y que GCC/Clang también rechazan en definiciones dentro de templates de clase** (`JGeometry/TVec.hpp` lo usa así). El override expande `NO_INLINE` a **nada** en todas las toolchains host (noinline es solo optimización, no afecta ODR), con guarda `#ifndef` para no pisar el fallback de `types.h`. |
| `revolution/os.h` | `__OSBusClock`/`__MEM2End` pasan a `extern` (el original los define como globals inicializados, que fallaría al enlazar en host); `OSPhysicalToCached` es un macro PPC sin equivalente en host (patcheado en `JKRHeap.cpp` con `compat::getBootInfo()`). |
| `revolution/os/OSBootInfo.h` | El miembro `DVDDiskID DVDDiskID;` (nombre de miembro = typedef en scope) es rechazado por GCC/Clang en C++ → se usa el tag `struct DVDDiskID`. |
| `revolution/os/OSTime.h` | `OS_BUS_CLOCK_SPEED` pasa a `extern` (mismo patrón que `os.h`). |
| `revolution/base/PPCWGPipe.h` | Los miembros del `GXWGFifo` original son macros mwcc; se sustituyen por campos `_u8/_u16/_u32/_u64/_s8/_s16/_s32/_s64/_f32/_f64` en host (convención `PC_PORT`, ver porting.md). |
| `revolution/gx.h` | El upstream incluye sus sub-cabeceras con comillas (`#include "revolution/gx/GXVert.h"`); **MSVC resuelve las comillas buscando en toda la cadena de includes** y puede tomar el `GXVert.h` vendered por delante de nuestro override. El override usa `<...>` (ángulos), que resuelve siempre por el include path (nuestros overrides primero). |
| `revolution/gx/GXVert.h` + `GXRegs.h` | Los writers FIFO (`GXCmd1u8`, `GXPosition3f32`, …) escriben en un `GXWGFifo` global `extern` vía `gxfifo._u8`/`gxfifo._f32`… (los originales usan macros PPC de acceso a memoria y una definición global por TU que no enlaza en host). |
| `JSystem/JKernel/JKRHeap.hpp` | Los overloads de `operator new` (placement new con heap/alineación) estaban ocultos tras `#ifdef __MWERKS__` → sin ellos, todo `new (heap, align)` del game code falla al compilar en GCC/Clang/MSVC. También `dispose_subroutine` con `uintptr_t` (rango de punteros 64-bit) y `getMaxAllocatableSize` sin truncar a `u32`. |
| `JSystem/JGadget/linklist.hpp` | Añade `#include <iterator>` (necesario para `std::reverse_iterator` en C++ moderno). |
| `math_types.hpp` | El bloque `namespace std { atan2 }` del upstream era un shim para la stdlib de Metrowerks; redefinirlo choca con `std::atan2` de libstdc++/MSVC (error duro). Eliminado (la std ya lo provee). |
| `JSystem/JMath/JMATrigonometric.hpp` | `namespace std { struct pair { a1; b1; } }` del upstream choca con `std::pair` de la std real. El shim lo mueve a `JMath::pair` (mismos miembros `a1/b1`) y la tabla usa ese tipo. |
| `JSystem/JMath/JMath.hpp` | `JMAFastSqrt`, `JMAHermiteInterpolation` y `gekko_ps_copy12/16` tenían cuerpo solo bajo `__MWERKS__` (asm PPC) → en host quedaban vacíos (UB). El shim añade cuerpos host: `JMAFastSqrt` = `std::sqrt`, `JMAHermiteInterpolation` = transcripción de las fórmulas del asm, `gekko_ps_copy*` = `std::memcpy` de 48/64 bytes. |
| `JSystem/JGeometry/TUtil.hpp` | Faltaba `<cfloat>` para `FLT_EPSILON` (usado en `TUtil::epsilon()`). |
| `JSystem/JGeometry/TVec.hpp` | (1) `TQuat4::setRotate`: `set<f32>(...)` sin calificar solo parsea bajo Metrowerks → `TVec4<T>::template set<T>(...)`. (2) `negateInternal/subInternal/mulInternal`: la rama no-MW declaraba sin definir → cuerpos inline host (las definiciones PPC eran asm). (3) `TVec3<f32>::dot` y `operator=` tenían cuerpo **solo** bajo `__MWERKS__` → vacíos en host (¡devolvían basura / no asignaban!): cuerpos host (`x*xb+y*yb+z*zb`, copia de componentes). |
| `JSystem/JGeometry/TMatrix.hpp` | `TRotation3::mult33`: llamadas `get()`/`set<f32>()` sin calificar en template → `this->get(...)`. |
| `revolution/mem/heapCommon.h` | (1) include corto `<mem.h>` (path de Metrowerks) → `<revolution/mem.h>` (evita self-include). (2) `UIntPtr` era `u32` y truncaba punteros en host 64-bit → `uintptr_t` (mismo defecto que `types.h`); `RoundUp/RoundDown` idem. Necesario para `revolution/hbm.h` → `WPadHolder.hpp`. |
