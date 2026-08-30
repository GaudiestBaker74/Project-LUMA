# docs/build.md — Build del port PC

> Estado: M0 (plan). Se valida en M1 (Linux) y M2 (Windows).

## 1. Contexto: dos builds conviven

Petari tiene su **build de decompilación** (Python `configure.py` → ninja → mwcc/PPC → objdiff). Ese build no se toca.

El port introduce un **build PC** independiente:

```sh
cmake -B build -DPLATFORM=PC        # configurar (Linux o Windows)
cmake --build build                 # compilar
ctest --test-dir build              # tests
```

La opción `PLATFORM=PC` es explícita para dejar claro que es un objetivo distinto (y porque en el futuro podría existir otro objetivo). Default del proyecto: `PLATFORM=PC`.

## 2. Cadena de herramientas

| Plataforma | Compilador | Notas |
|---|---|---|
| Linux (x86-64 y aarch64) | GCC ≥ 12 o Clang ≥ 16 | C++20 |
| Windows 10/11 | MSVC ≥ 2022 (o Clang-cl) | C++20; Win10/11 + Steam Deck (Linux) |

- Se usa **C++20** (estándar moderno, soportado por las tres cadenas).
- Los fuentes de Petari son C++03-ish: se compilan igual con C++20 (sin romper nada; si algún fichero requiere compat, se aísla con `set_source_files_properties(... COMPILE_OPTIONS -std=c++20)` por fichero, documentado).

## 3. Dependencias de terceros

| Dependencia | Uso | Cómo se obtiene |
|---|---|---|
| **Vulkan** (headers + loader) | Renderer | En Linux: paquete `vulkan-headers`/`vulkan-loader` o Vulkan SDK; en Windows: Vulkan SDK. **`volk`** como cargador dinámico (se vende en `third_party/volk` o FetchContent) |
| **SDL3** | Ventana, input, audio | `FetchContent` (tarball release) o paquete del sistema (`libsdl3-dev` en Debian/Ubuntu; en Windows se descarga el binario dev o vcpkg). Se prefiere **FetchContent** para que ambos SO usen la misma versión |
| **doctest** (o Catch2) | Tests unitarios | header-only (FetchContent) — decisión confirmable; doctest es de un solo header, encaja con "sin frameworks enormes" |

La política es: **pocas dependencias, todas multiplataforma**. SDL3 + Vulkan (vía volk) + doctest cubren el 100 % del build mínimo.

## 4. Estructura CMake

```
galaxy-pc/
├── CMakeLists.txt          # raíz: opciones (PLATFORM), subdirectorios
├── cmake/
│   ├── FetchSDL3.cmake
│   ├── FetchVulkan.cmake   # volk + headers
│   ├── CompilerWarnings.cmake
│   └── PlatformSelect.cmake  # PLATFORM=PC → windows/ o linux/
└── src/
    ├── CMakeLists.txt
    ├── main.cpp
    ├── platform/  (CMakeLists interno; selecciona platform/windows o platform/linux)
    ├── compat/    (CMakeLists interno)
    ├── tools/
    └── tests/     (ctest)
```

### Cómo se compila el código de Petari (submodule)

- El submodule `third_party/petari` se compila **desde su propia ubicación** (no se copia).
- Include paths (en este orden):
  1. `src/compat/include` — nuestros overrides de cabeceras de consola (solo lo imprescindible),
  2. `third_party/petari/include` — cabeceras del juego (`Game/*`),
  3. `third_party/petari/libs/RVL_SDK/include` — cabeceras originales (`revolution/*`, `dolphin/*`),
  4. `third_party/petari/libs/...` resto (JSystem, nw4r, MSL_C) según módulo.
- Los fuentes de Petari se listan explícitamente en CMake (módulo por módulo, no `glob`), porque:
  - no todo se compila en PC (p. ej. `src/RVL_SDK`, `src/MetroTRK`, `src/NDEV`, `src/Runtime` se excluyen; los `os/dvd/vi/...` se sustituyen por `compat/`),
  - queremos trazabilidad de qué módulos están habilitados (mismo espíritu que `splits.txt`).
- **Ficheros con asm PPC** (`Game/Util/MathUtil.cpp`, `JSystem/JMath/JMath.cpp`, `nw4r/db/db_assert.cpp`, `J3DTransform.cpp`, `ShadowVolumeOvalPole.cpp`, `ClipAreaShape.cpp`, `J3DAnimation.cpp`, `J3DMtxBuffer.cpp`, `J3DShapeMtx.cpp`, `J3DSys.cpp`): se compilan con un **parche de port en `src/compat/patches/`** (sustitución de los bloques asm por C equivalente). La sustitución se hace por `#ifdef` con macro propia `PC_PORT` o por reescritura del fichero en `compat/` si el asm está aislado — ver porting.md.
- **Símbolos ausentes**: la decompilación no está al 100 % (68,62 %). Cualquier símbolo que falte y sea necesario se declara y se implementa como stub `TODO(PC_PORT)` en `src/compat/` (misma regla que el resto).

## 5. Linux (M1) — pasos

Requisitos (Debian/Ubuntu):

```sh
sudo apt install build-essential cmake ninja-build git
# Vulkan (headers + loader + glslc para la generación de shaders en runtime no — los shaders se compilan
# en runtime con glslang/SPIR-V; se vendila glslang en third_party o se usa shaderc)
sudo apt install libvulkan-dev glslang-tools
# SDL3 se obtiene con FetchContent (no hace falta paquete)
```

Configurar y compilar:

```sh
cmake -B build -DPLATFORM=PC -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Arranque rápido (sin assets, con backends de prueba):

```sh
./build/src/galaxy-pc --help
./build/src/galaxy-pc                  # demo M1: heap JKR de Petari corriendo nativo
./build/src/galaxy-pc --log-level TRACE
# o vía el script de conveniencia (compila si falta y ejecuta):
./run.sh
```

También se puede instalar el binario (a `bin/` dentro del build tree):

```sh
cmake --install build
```

## 6. Windows (M2) — pasos

Requisitos: Visual Studio 2022 (MSVC, componente C++), CMake, Git. Vulkan SDK de LunarG (solo para M3+; M2 no lo necesita).

```powershell
cmake -B build -DPLATFORM=PC -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Notas:
- Mismo código fuente: **cero `#ifdef _WIN32` fuera de `src/platform/windows/`** (ADR-010).
- En Windows el único "C" de plataforma vive en `platform/windows/`; SDL3 y Vulkan no requieren código Win32 en nuestro árbol (aunque puede haber un par de ficheros para cosas como el icono de la app o integración con el sistema de ficheros wide-path).
- `std::atomic<Platform::Memory::Stats>` (48 B, no lock-free) necesita `libatomic` con GCC/MinGW; MSVC lo provee en su CRT (el CMake lo enlaza solo para toolchains no-MSVC en Windows).

### Verificación M2 (hecha)

- **Build cruzado MinGW-w64** (`cmake/Toolchain-MinGW.cmake`): compila y enlaza `galaxy-pc.exe` y `galaxy-pc-tests.exe` contra `windows.h` real. Verificado en este repo.
- **Tests bajo Wine**: `galaxy-pc-tests.exe` → **36/36 OK** (incluye el arena 512 MiB vía `VirtualAlloc`, `%APPDATA%` como config dir, heaps JKR).
- **Build MSVC real**: lo ejecuta el CI en `windows-latest` (no verificable en Linux).
- **aarch64 Linux** (Steam Deck): cross-compile OK (`cmake/Toolchain-LinuxAArch64.cmake`), solo compilación (sin ejecutar).

### Toolchains de cross-verificación (desde Linux)

```sh
# Windows x64 (MinGW-w64) — compila contra windows.h y enlaza .exe
sudo apt-get install -y g++-mingw-w64-x86-64
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-MinGW.cmake -G Ninja
cmake --build build-mingw
wine build-mingw/src/tests/galaxy-pc-tests.exe   # si tienes wine

# Linux aarch64 (Steam Deck) — compila (no ejecuta)
sudo apt-get install -y g++-aarch64-linux-gnu
cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-LinuxAArch64.cmake -G Ninja -DPLATFORM=PC
cmake --build build-arm64
```

## 7. CI

- GitHub Actions (`.github/workflows/ci.yml`): tres jobs — Linux (GCC), Windows (MSVC `windows-latest`) y Linux aarch64 cross-compile. `ctest` en Linux y Windows.
- Artefacto (futuro): binario `galaxy-pc` + documentación de assets.

## 8. Configuración en runtime

- `galaxy-pc.toml` (o CLI): ruta de `assets/`, resolución de ventana, vsync, timestep, idioma, tv-format simulado (NTSC 60/PAL 50), flags de debug (`--gpu-debug`, `--gx-log`, `--dump-gx`).
- Sin ficheros de configuración en el árbol de assets del juego: la config del port es aparte (nunca se mezcla con `StageData/...`).
