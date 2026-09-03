# M9.5.2 — Layouts (nw4r lyt) + fuentes

¡Hola! Este zip contiene el hito **M9.5.2**: el juego ya sabe **leer los
layouts** (los menús, marcadores y pantallas de título del juego) y las
**fuentes** (las letras). Los archivos del juego están escritos "al revés"
(big-endian, como la Wii); este hito los traduce a formato PC antes de
usarlos. Es el paso previo a que aparezca la pantalla de título (M9.5.3).

## Paso 1 — Copiar los ficheros

1. Abre la carpeta `C:\Users\sergi\Downloads\Galaxy\galaxy-pc`.
2. Copia TODO el contenido de la carpeta `files/` de este zip **dentro** de
   `galaxy-pc`, encima de lo que haya. Cuando Windows pregunte, di
   **"Reemplazar los archivos del destino"**.
   - Son 29 ficheros. Ninguno borra trabajo tuyo: solo añade/sustituye
     ficheros del port. No toca nada de `third_party`.

## Paso 2 — Compilar

Abre PowerShell en `C:\Users\sergi\Downloads\Galaxy\galaxy-pc` y lanza,
**uno a uno**:

```powershell
cmake -B build -DPLATFORM=PC
```

```powershell
cmake --build build --config RelWithDebInfo
```

Si compila da error, mándame el error tal cual aparece (copiar/pegar).

## Paso 3 — Los tests

```powershell
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Tiene que decir **180 passed, 0 failed** (antes eran 174; los 6 nuevos son
los de este hito: `lyt_*`, `tpl_*` y `resfont_*`).

## Paso 4 — Boot con tus assets

```powershell
.\build\src\RelWithDebInfo\galaxy-pc.exe --boot --log-level INFO --log-file boot.log
```

- Verás lo mismo que en M9.5.1 (ventana con el logo / pantalla en negro):
  **es lo esperado**. La pantalla de título llegará en M9.5.3.
- Este hito no cambia lo que se ve en el boot, pero deja toda la tubería de
  layouts/fuentes lista y probada con datos sintéticos.
- Si el boot peta o hace algo raro, mándame el `boot.log`.

## Qué lleva dentro (resumen)

- **Traductor de layouts (brlyt)**: convierte el archivo entero de Wii a PC
  en memoria, sección por sección (paneles, imágenes, textos, ventanas,
  materiales, grupos...). Se puede ejecutar varias veces sin romper nada.
- **Traductor de texturas TPL y de fuentes brfnt**: mismo sistema, con caché
  para no repetir el trabajo.
- **Texto de los TextBox**: en Wii está en UTF-16 "al revés"; ahora se lee y
  se convierte al texto ancho del PC.
- **Piezas que faltaban en la decompilación**: Petari declara varias
  funciones que nunca define (BuildPaneObj, PrintImpl, el RTTI de TextBox,
  etc.). Reconstruidas en `src/compat/nw4r/LytMissing.cpp` y parches.
- **Bug gordo cazado**: los `LinkList` de Petari fuera de Metrowerks usaban
  offset 0 para clases con vptr (Pane, Group, AnimTransform) — al encadenar
  un pane se pisaba su tabla de virtuales y la primera llamada virtual
  petaba. Corregido con shadows de `pane.h`/`group.h`/`layout.h`.
- **Matemáticas de matrices**: las funciones PSMTX* de la Wii son assembly
  de PowerPC; reimplementadas en escalar para PC (`JMathCompat.cpp`).

## Lo que NO está todavía

- Las animaciones de layout (brlan) están en "stub": si un layout intenta
  animarse, se registra un aviso en el log y no se anima. Llegan en M9.5.3
  junto con la pantalla de título.
