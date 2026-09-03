# ZIP COMPLETO — M9.5.1 + M9.5.2 + M9.5.3a TODO JUNTO

## ¿Qué ha pasado?

El error que me mandaste dice que faltan archivos de M9.5.2
(`MEMAllocator.cpp`, `lyt_layout_test.cpp`). O sea: los zips anteriores
(M9.5.1 y/o M9.5.2) no llegaron a extraerse bien en tu carpeta, pero el
CMakeLists nuevo sí.

**Solución:** este zip lleva TODOS los archivos nuevos y cambiados desde
M9.4 (que eso sí lo tienes bien). Da igual lo que tuvieras instalado antes:
extrae este zip encima y tu carpeta queda completa y correcta.

## Paso 1 — Copiar los archivos

1. Descarga `m9-5-completo.zip`.
2. Clic derecho → **Extraer todo...**.
3. Abre la carpeta extraída: verás `src`, `docs` y archivos LEEME.
4. Copia TODO y pégalo en `C:\Users\sergi\Downloads\Galaxy\galaxy-pc`.
5. ¿Reemplazar? → **Sí a todo**.

## Paso 2 — Borrar la carpeta build (importante)

La carpeta `build` se quedó a medias con el error. En PowerShell, dentro de
`C:\Users\sergi\Downloads\Galaxy\galaxy-pc`:

```
Remove-Item -Recurse -Force build
```

## Paso 3 — Configurar

```
cmake -B build -DPLATFORM=PC
```

Tiene que terminar SIN errores rojos (al final dice "Build files have been
written to...").

## Paso 4 — Compilar

```
cmake --build build --config RelWithDebInfo
```

Ojo: al haber borrado `build`, esto recompila TODO y puede tardar bastante
(5-15 minutos). Es normal.

## Paso 5 — Los tests

```
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Lo esperado: **181 passed, 0 failed, 2 skipped**.

## Paso 6 — Arrancar el juego

```
.\build\src\RelWithDebInfo\galaxy-pc.exe --boot --log-level INFO --log-file boot.log
```

Déjalo 20-30 segundos y cierra la ventana.

## Paso 7 — Qué busco en boot.log

Abre `boot.log` con el Bloc de notas y busca `LayoutManager`.

Si tus assets tienen el layout, verás:

```
[INFO ] [compat.layout] LayoutManager 'WiiRemoteStrap': built from '/LayoutData/WiiRemoteStrap.arc' (N textures/fonts in arc)
[INFO ] [compat.tpl] TPL converted: ...
```

Si no lo tienes, verás:

```
[ERROR] [compat.layout] LayoutManager 'WiiRemoteStrap': cannot mount '/LayoutData/WiiRemoteStrap.arc' (layout will stay empty)
```

Las DOS opciones están bien (el juego no se cuelga ni en una ni en otra),
pero dime cuál sale.

## Paso 8 — Lo que necesito que me envíes

1. Las **últimas 30 líneas** de `boot.log` (o el archivo entero).
2. La salida de:

```
dir assets\LayoutData
```

(Si no existe, `dir assets` y me pegas lo que salga.)

3. La línea final del `ctest` (la de "passed").
4. Si algo falla en los pasos 3, 4 o 5: el mensaje de error COMPLETO, tal
   cual sale en rojo.
