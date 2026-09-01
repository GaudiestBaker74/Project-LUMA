# Assets (M7) — extracción, colocación y verificación

El juego necesita los archivos del disco de Super Mario Galaxy 1
(título `RMGE01`/`RMGP01`/`RMGJ01`/`RMGK01`). El port **no incluye ni
descarga ningún asset**: solo usa los que tú extraigas de una copia legítima.

## Dónde van

Por defecto el juego busca los assets en `./assets` (relativo al directorio
de trabajo). Se puede cambiar con:

- `--assets-dir DIR` (flag de `galaxy-pc`), o
- la variable de entorno `GALAXY_ASSETS_DIR`.

El contenido esperado es el **sistema de archivos del disco** (paths de
consola como `/StageData/...`, `/ObjectData/...`) montado bajo ese directorio.
Es decir: `./assets/StageData/...`, `./assets/ObjectData/...`, etc.

Sin assets montados el juego sigue arrancando (la demo M5 funciona): el
"drive" reporta "no disk" y cualquier lectura de archivo falla limpiamente.

## Cómo extraerlos (con Dolphin)

Dolphin puede volcar el contenido del disco a un directorio:

1. Abre Dolphin (>= 5.0) y carga el juego **una vez** para que descifre el
   disco, o usa tu copia en formato `.iso`/`.rvz`/`.wbfs`.
2. Menú **File → Open** (o doble clic en el juego).
3. Con el juego en la lista: botón derecho → **Dump Disc** (volcado de
   imagen completa) **no** es lo que queremos; lo que necesitamos es el
   **filesystem**:
   - **Dolphin 5.0+**: menú **Tools → Disc → Dump Disc** vuelca la imagen;
     el **filesystem** se extrae con **Tools → Disc → Dump Disc Partition
     File System** (en algunas versiones: *Dump File System*).
   - El resultado es un directorio con `sys/main.dol`, `sys/fst.bin` y los
     directorios de datos (`StageData/`, `ObjectData/`, `LayoutData/`, ...).
4. Copia **solo los directorios de datos** (lo que el juego lee como
   `/StageData/...` etc.) dentro de tu `./assets`:
   - `ObjectData/`  → `assets/ObjectData/`
   - `StageData/`   → `assets/StageData/`
   - `LayoutData/`  → `assets/LayoutData/`
   - `MessageData/` → `assets/MessageData/`
   - `HomeButton2/` → `assets/HomeButton2/`
   - `ParticleData/`→ `assets/ParticleData/`
   - (opcional) `MovieData/`, `AudioRes/`, `ModuleData/`, `MapPartsData/`,
     `DemoData/`

> La decompilación (build de Petari) solo necesita `orig/RMGK01/sys/main.dol`
> para comparar; ese requisito **no aplica** a este port. Aquí el árbol de
> assets es del usuario y no se redistribuye.

## Verificar la extracción

```
build/src/tools/verify-assets [root] [--quiet]
```

Comprueba:

- que el root existe y es un directorio;
- los **directorios de arranque** (`/StageData`, `/ObjectData`, `/LayoutData`,
  `/MessageData`, `/HomeButton2`, `/ParticleData`) — si falta uno, falla;
- los **185 archivos stationed** que el juego monta al arrancar
  (`StationedArchiveLoader`, p. ej. `/ObjectData/Mario.arc`,
  `/LayoutData/Font.arc`) — si falta uno, falla;
- la **cabecera de contenedor** de cada `*.arc` (`RARC` o `Yaz0`) — anomalías
  son *warnings* (los volcados varían);
- directorios opcionales (`/MovieData`, `/AudioRes`, ...) — *warnings*.

Exit code: `0` = listo, `1` = incompleto/roto. Los paths del manifest se
generan del código decompilado (`Game/System/StationedFileInfo.cpp`), no a
mano.

## Notas legales

- No incluyas ni redistribuyas los assets de Nintendo (ni ROMs, ISOs, WADs).
- El programa asume una **copia legítima** de tu propiedad.
- `verify-assets` solo comprueba **nombres y cabeceras**; nunca copia ni
  incluye contenido.

## Capa DVD (compat)

`src/compat/dvd/DVD.cpp` implementa la API DVD (`<revolution/dvd.h>`) que el
juego llama sobre este árbol:

- **FST en memoria**: `DVDInit` (o el primer uso) escanea el root y construye
  la tabla de directorios. Los `entrynum` son índices de la tabla (root = 0),
  estables durante la ejecución → `DVDConvertPathToEntrynum` /
  `DVDFastOpen` son O(1). Los nombres de `DVDDirEntry` apuntan al FST y
  siguen válidos hasta el siguiente `DVDInit`.
- **Lecturas**: `DVDReadPrio` = síncrona (camino de arranque del juego);
  `DVDReadAsyncPrio` corre en un **hilo worker dedicado** (el "hilo de
  interrupción del DVD" del SDK) con cola ordenada por prioridad y callbacks
  invocados en ese hilo. `DVDCancel` marca cancelado (resultado `-2`);
  lecturas en vuelo terminan (disco local rápido).
- **Estado**: assets montados → `DVD_STATE_END` (0, "ready"); sin assets →
  `DVD_STATE_NO_DISK`. Los juegos comprueban `0 == listo` (p. ej.
  `JASAramStream::dvdErrorCheck`).
- **Convención de resultados**: `>= 0` = bytes leídos / OK, `-1` = error
  (archivo ausente o lectura fuera de rango), `-2` = cancelado.
- **Sin caché de bytes en M7** (decisión): el FST cachea metadatos y las
  lecturas repetidas las sirve la page cache del SO.
- **No portado**: la capa de registros `DVDLow*` (el juego no la llama) y los
  helpers internos `__DVD*`.

`compat::initDVD()` (llamado desde `main.cpp` tras `initOS`) construye el
FST; `compat::shutdownDVD()` para el worker.

## Estado

| Sub-hito | Estado |
|---|---|
| M7.1 compat/dvd síncrono (FST, open/read/close, dirs, estado) | ✅ |
| M7.2 DVDReadAsyncPrio + worker + cancel | ✅ |
| M7.3 tools/verify-assets | ✅ |
| M7.4 docs (este archivo, milestones, architecture) | ✅ |
| *(futuro)* montaje JKRArchive (RARC/Yaz0) sobre el DVD | — |

Próximo tras M7: **M8 — audio** (AX/JAS), y más adelante el montaje de
archivos (`JKRArchive`) que consume el DVD.
