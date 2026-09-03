# M9.5.3a — La pila de layouts REAL ya carga (instrucciones paso a paso)

¡Hola! Este paso es grande por dentro pero muy fácil de instalar por fuera.
Antes los layouts (los menús/dibujos de la pantalla) eran de mentira (stubs).
Ahora el juego **de verdad** abre el archivo del layout, lo convierte y lo
construye. Todavía no se DIBUJA (eso es el paso 3c), pero ya se carga.

## Paso 1 — Copiar los archivos

1. Descarga `m9-5-3a-layouts.zip`.
2. Haz clic derecho → **Extraer todo...**.
3. Abre la carpeta extraída. Dentro verás carpetas `src`, `docs`...
4. Copia TODO lo que hay dentro (las carpetas `src` y `docs`) y pégalo en:
   `C:\Users\sergi\Downloads\Galaxy\galaxy-pc`
5. Windows preguntará si reemplazar archivos → di que **Sí a todo**.

## Paso 2 — Compilar

Abre PowerShell en `C:\Users\sergi\Downloads\Galaxy\galaxy-pc` y escribe
(un comando, Enter, espera a que termine):

```
cmake --build build --config RelWithDebInfo
```

Si hay algún error en rojo, cópialo y me lo pegas tal cual.

## Paso 3 — Los tests

```
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Lo esperado al final: **181 passed, 0 failed, 2 skipped**
(antes eran 180; hay 3 tests nuevos y uno viejo se arregló).

## Paso 4 — Arrancar el juego

```
.\build\src\RelWithDebInfo\galaxy-pc.exe --boot --log-level INFO --log-file boot.log
```

Déjalo unos 20-30 segundos y ciérralo tú (la ventana del logo no hace nada
más por ahora, es normal).

## Paso 5 — Qué mirar en boot.log (¡esto es lo importante!)

Abre `boot.log` (con el Bloc de notas) y busca la palabra `LayoutManager`.

**Si tus assets tienen el layout del strap**, verás algo como:

```
[INFO ] [compat.layout] LayoutManager 'WiiRemoteStrap': built from '/LayoutData/WiiRemoteStrap.arc' (N textures/fonts in arc)
[INFO ] [compat.tpl] TPL converted: ...
```

**Si no lo tienes**, verás:

```
[ERROR] [compat.layout] LayoutManager 'WiiRemoteStrap': cannot mount '/LayoutData/WiiRemoteStrap.arc' (layout will stay empty)
```

Las dos cosas están BIEN (el juego no se cuelga ni en un caso ni en el
otro), pero dime cuál de las dos sale.

## Paso 6 — Lo que necesito que me envíes

1. Las **últimas 30 líneas** de `boot.log` (o el archivo entero si quieres).
2. La salida de este comando (es para el paso 3d, la pantalla del título):

```
dir assets\LayoutData
```

(Si `assets\LayoutData` no existe, prueba `dir assets` y me pegas lo que
salga.)

3. La salida final de `ctest` (la línea de "passed").

## Qué hay dentro de este zip (para el registro)

- `LayoutManagerCompat.cpp` — el LayoutManager reconstruido (petari no lo
  tiene decompilado): monta el arc, convierte el brlyt, lo construye, crea
  los controladores de animación. Tolera que falten assets sin colgarse.
- `LayoutHolder.cpp`, `ResourceInfo.cpp`, `LayoutAnmPlayer.cpp`,
  `PaneEffectKeeper.cpp` — copias parcheadas de archivos de petari.
- `LayoutActor.cpp`, `SimpleLayout.cpp`, `LayoutPaneCtrl.cpp` — ahora se
  compilan los REALES de petari (antes había stubs en su lugar, ya borrados).
- `J3DFrameCtrlCompat.cpp` — el contador de frames de animación, copiado
  tal cual del código decompilado.
- `layout_holder_test.cpp` — 3 tests nuevos con datos sintéticos.
- Detalles técnicos completos en `docs/m9.5.3-plan.md` (sub-paso a ✅).
