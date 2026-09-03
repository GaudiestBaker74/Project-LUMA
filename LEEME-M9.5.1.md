# M9.5.1 — Pipeline de recursos (RARC / Yaz0 / JKRArchive)

¡Hola! Este zip contiene el hito **M9.5.1**: el juego ya sabe **leer los
archivos `.arc`** (los "cajas" donde viven los modelos, texturas y layouts)
desde tu carpeta de assets, descomprimirlos (Yaz0) y montarlos en memoria.
Es el paso previo a que aparezcan el título y Mario.

## Paso 1 — Copiar los ficheros

1. Abre la carpeta `C:\Users\sergi\Downloads\Galaxy\galaxy-pc`.
2. Copia TODO el contenido de este zip (la carpeta `files/`) **dentro** de
   `galaxy-pc`, encima de lo que haya. Cuando Windows pregunte, di
   **"Reemplazar los archivos del destino"**.
   - Son 23 ficheros. Ninguno borra trabajo tuyo: solo añade/sustituye
     ficheros del port.

## Paso 2 — Compilar

Abre PowerShell en `C:\Users\sergi\Downloads\Galaxy\galaxy-pc` y lanza,
**uno a uno**:

```powershell
cmake -B build -DPLATFORM=PC
```

```powershell
cmake --build build --config RelWithDebInfo
```

Si compile da error, mándame el error tal cual aparece (copiar/pegar).

## Paso 3 — Los tests

```powershell
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Tiene que decir **174 passed, 0 failed** (antes eran 169; los 5 nuevos son
los de este hito: `jkr_*`).

## Paso 4 — Boot con tus assets

```powershell
.\build\src\RelWithDebInfo\galaxy-pc.exe --boot --log-level INFO --log-file boot.log
```

- Seguirás viendo la ventana con el logo / pantalla en negro: **es lo
  esperado** en este hito (todavía no hay layouts ni escena de título).
- Lo interesante está en `boot.log`: búscame estas líneas y dime si
  aparecen o qué dice en su lugar:
  - `root dir set to ...`
  - `FST built from ... : N entries`
  - cualquier `WARN` o `ERROR` (sobre todo de `compat.rarc`).

## Paso 5 — Assets que faltan (importante)

En tu máquina faltaban estas carpetas/archivos dentro de los assets. Si los
puedes re-extraer del juego, mejor:

- `MessageData/` (carpeta entera, con `Message.arc` dentro)
- `LayoutData/HomeButton.arc`

Para comprobarlo tú mismo:

```powershell
.\build\src\tools\RelWithDebInfo\verify-assets.exe
```

(te dice qué archivos de los que espera el juego existen y cuáles no)

## Qué cambió por debajo (resumen corto)

- Nuevos parches en `src/compat/patches/JSystem/JKernel/`: montaje de
  archivos RARC (formato Wii, big-endian) con conversión a formato PC,
  descompresión Yaz0, y arreglos de símbolos que al decomp le faltaban.
- `src/compat/jsystem/RarcHost.*`: el traductor BE→PC de los `.arc`.
- DVD host: las lecturas que se pasan del final del archivo ahora se
  rellenan con ceros (la Wii hace lo mismo porque los discos tienen
  relleno) — sin esto el ripper fallaba siempre.
- `SceneCompat`: `MR::mountArchive` y `MR::loadToMainRAM` ya son reales.
- 5 tests nuevos con datos sintéticos (`src/tests/jkr_archive_test.cpp`).

Cualquier error de MSVC que no veas en mis instrucciones → copia/pega y lo
arreglo en la siguiente ronda.
