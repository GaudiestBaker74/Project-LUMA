# Super Mario Galaxy — Port nativo a PC

Port nativo (x86-64 / aarch64) de **Super Mario Galaxy 1** para **Windows y Linux**, construido sobre la decompilación [SMGCommunity/Petari](https://github.com/SMGCommunity/Petari).

**No es un emulador de Wii.** Es código C/C++ nativo que reutiliza la lógica del juego decompilado y sustituye la capa de APIs de la consola (GX, VI, DVD, OS, WPAD, AX…) por implementaciones nativas de PC:

```
Game code (Petari)  ─►  Wii compatibility layer  ─►  Platform abstraction  ─►  Windows / Linux
Galaxy rendering    ─►  GX compatibility layer   ─►  Renderer abstraction ─►  Vulkan
```

## Estado

- **M0 (análisis)** ✅ — ver `docs/architecture.md`.
- **M1 (build PC + plataforma)** ⏳ — pendiente de confirmación.
- Progreso detallado: `docs/milestones.md`.

| | |
|---|---|
| Lenguaje | C/C++ (C++20 en el port; el código upstream se compila tal cual) |
| Build | CMake (`cmake -B build -DPLATFORM=PC`) |
| Gráficos | Vulkan (único backend inicial; D3D12 futuro opcional) |
| Ventana/input/audio base | SDL3 |
| UI de debug | Dear ImGui solo en herramientas, nunca en el juego |
| Tests | doctest + ctest |

## Aviso legal e importante

- Este repositorio **no contiene ni redistribuye** ROMs, ISOs, WADs, modelos, texturas, música u otros assets propietarios de Nintendo.
- El port **asume que posees una copia legítima del juego** y solo utiliza datos extraídos de tu propia copia.
- **Nada se descarga de Internet.** Tú extraes los archivos del juego de tu copia (con Dolphin, igual que documenta Petari) y los colocas en `assets/` (ver `docs/build.md` §8 y `docs/milestones.md` M7).
- El código fuente de la decompilación es **CC0-1.0** (ver `third_party/petari/LICENSE`).

## Documentación

| Fichero | Contenido |
|---|---|
| `docs/architecture.md` | Análisis de Petari, entry point, dependencias Wii, ADRs, estructura |
| `docs/build.md` | Cómo compilar en Linux y Windows |
| `docs/porting.md` | Metodología para portar módulos, convenciones, stubs `TODO(PC_PORT)` |
| `docs/renderer.md` | Diseño de la abstracción de renderer (Vulkan) |
| `docs/gx.md` | Inventario y plan de la capa de compatibilidad GX |
| `docs/milestones.md` | Roadmap M0–M11 con criterios de éxito |

## Cómo está organizado

```
src/main.cpp            entry point PC
src/platform/           abstracción de plataforma (Log, Memory, Filesystem, Timing,
                        Threading, Input, Audio, Video, Renderer) + windows/ y linux/
src/compat/             capa de compatibilidad Wii (os, dvd, vi, gx, wpad, kpad, pad,
                        ax, nand, nwc24, sc, mem, mtx, tpl…)
src/tools/              utilidades CLI (verificación de assets, dumps)
src/tests/              tests unitarios (ctest)
third_party/petari/     submodule → SMGCommunity/Petari (upstream sin modificar)
docs/                   esta documentación
assets/                 datos extraídos por el usuario de SU copia (git-ignored)
```
