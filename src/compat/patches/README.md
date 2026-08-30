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
| `JSystem/JKernel/JKRHeap.cpp` | (1) `OSPhysicalToCached(0)` (macro PPC de traducción de direcciones) → `compat::getBootInfo()` (stub del boot header en `compat/os`). (2) `operator new/new[]` con primer parámetro `u32` → `size_t` (el estándar C++ lo exige; GCC/Clang/MSVC rechazan `operator new(u32)`). |
