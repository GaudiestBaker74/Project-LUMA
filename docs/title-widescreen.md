# Title screen en pantallas anchas (16:9 → 21:9 / 4K)

> Estado: **implementado y verificado por tests** (`src/tests/ui_anchoring_test.cpp`,
> 226 tests en verde + 5 skips). No toca el contenido del juego: solo la
> proyección, el espacio de layout y el propio render del logo/prompt.

El problema original era el clásico "estirado a pantalla completa": la consola
dibuja en un framebuffer de 4:3 (640×456) y el port escalaba **todo** (fondo y
UI) a la ventana, con lo que en 16:9 la composición se deformaba horizontalmente
y los modelos 2D del título aparecían ensanchados.

## Modelo: FONDO adaptable + UI anclada al centro

Son **dos sistemas independientes**:

| | Fondo (cielo, estrellas, planeta) | UI (logo, prompt, iconos A/B, copyright) |
|---|---|---|
| Referencia | 4:3 / 640×456 de la consola | espacio de layout 608×456 |
| En pantalla ancha | revela **más contenido** a los lados | **mismo tamaño** y misma distancia al centro |
| Proyección | FOV vertical bloqueado (ver `TitleSky`) | escala uniforme centro-anclada |

### 1. Fondo — `src/compat/j3d/TitleSky.cpp`

El título dibuja el domo `CometNearOrbitSky` con la cámara del juego
(`FileSelectCameraController::exeTitle`: posición **(0, 15000, 15000)**, mira a
**(0, 15800, 0)**, up +Y, `cTitleFovy` **60** vertical, near/far 100/800000). La
regla de la consola es la de `CameraContext`: **fovy vertical fijo + aspect =
framebuffer**, así que en 16:9 se ve más cielo a los lados que en 4:3 con el
mismo encuadre vertical, sin magnificar nada:

```
aspect = fbWidth / fbHeight          // PÍXELES del render mode (= la ventana)
C_MTXPerspective(proj, 60.0f, aspect, 100.0f, 800000.0f)
```

Dos correcciones importantes aquí (ver el apartado «las dos trampas», abajo):

- **El aspecto salía de `fbWidth / 456`**, no de la altura real: en 1920×1080 daba
  `1920/456 = 4.21` con un `fovy` "bloqueado" de ~20.7°, o sea ~3× de aumento
  vertical y una anisotropía de 2.4:1 (el domo/burbuja del sol enormes, estrellas
  convertidas en rayas verticales, banda de mar mucho más alta de lo normal). Ahora
  el aspecto es el del framebuffer real y el `fovy` el de la cámara del juego.
- **La traslación de la vista se respetaba de más**: el `mBaseMtx` del actor es
  *solo rotación* (igual que `FileSelectSky::exeWait`, que invierte `rotY·rotX`), así
  que el domo está en el **origen del mundo** y es la posición de la cámara la que
  coloca el horizonte, la banda de mar y el sol en el encuadre. Un atajo anterior
  ponía la traslación a cero («los actores de cielo siguen a la cámara»), lo que
  metía la cámara *dentro* del centro del domo y cambiaba la composición entera.

El domo también gira por fotograma: `angleY += 0.001` y el balanceo
`angleX = (1 - cos(8·step·π/3000)) · 3π/8` (período 750 fotogramas, ~12.5 s), y los
ángulos se aplican al fotograma que se está calculando (no al anterior). Por eso
un fotograma suelto solo se puede comparar sabiendo el `step`: lo imprime el
diagnóstico de `TitleSky::draw`.

### 2. UI — `src/compat/game/UiAnchoring.{h,cpp}` (`compat::ui`)

Toda la UI pasa por un único mapeo espacio-de-layout → píxel (con `W` y `H` en
**píxeles** del framebuffer: ver «las dos trampas», §5):

```
escala   s = uiScale(W, H) = H / 456        (uniforme en x e y)
layout   →  pantalla:  (W/2 + x·s , H/2 + y·s)
matriz   fillUiViewMtx(): [0][0] = s·608/W , [1][1] = s , sin traslación
```

- La **escala solo depende de la altura** (y se reduce, nunca se amplía, si el
  framebuffer es más estrecho que 4:3). Por eso el logo, el prompt y el
  copyright conservan tamaño y distancia al centro en 1280×720, 1920×1080,
  2560×1080, 3440×1440 y 3840×2160.
- `layoutSpaceWidth()` devuelve 608 en 4:3 y 832 en 16:9 (el ancho que la
  consola usa en su modo panorámico): el espacio de layout **gana sitio a los
  lados**, igual que el fondo, en lugar de comprimirse.
- `convertScreenPosToLayoutPos` / `convertLayoutPosToScreenPos` son ahora la
  **inversa exacta** de ese mapeo (antes: `x·608/getScreenWidth()` con un factor
  1:1 en y, que era justo la deformación).

`LayoutManagerCompat::updateUiAnchoring()` escribe ese mapeo en el
`DrawInfo.mViewMtx` del layout, y se refresca en cada `calcAnim()` — así un
resize o el paso a pantalla completa no dejan la UI mal colocada.

### 3. Modelos 2D del título — `SceneCompat::drawInitFor2DModel`

La orto de 2D usaba un ancho fijo de 608 unidades, lo que en 3440×1440 estira
un 31 %. Ahora usa `compat::ui::visibleLayoutWidth()`, que **coincide con el
608/832 de la consola** en 4:3/16:9 y crece con el aspecto en pantallas más
anchas.

### 4. Línea de prompt — `src/compat/ui/ButtonPrompt.{h,cpp}`

- El texto de fallback es exactamente `Press both [A] and [B].`
  (`LayoutManagerCompat`, nombres de pane `PressStart`/`TxtStart`/`ShaStart`).
- `splitPromptMessage()` parte la frase en palabras + huecos de botón
  (`[A]`, `[B]`); los espacios **alrededor** de un token son separación, nunca
  parte de la palabra (evita el bucle infinito del tokenizador y las palabras
  con espacios raros).
- La frase se centra **como un bloque**: `startX = centro - anchoTotal/2`, con
  los desplazamientos relativos entre palabras e iconos intactos (nada se mueve
  a mano para tapar errores del fondo).
- Iconos vectoriales (sin picture font en el host): **A = disco blanco** con
  contorno y **B = cuadrado redondeado blanco** con contorno, ambos con la
  letra centrada y del tamaño de la altura de mayúscula del texto. Nada de
  "dos círculos blancos".

### 5. Las tres trampas: `MR::getScreenHeight()`, el eje Y de la matriz de vista y el `uiScale` de 1.0

`MR::getScreenHeight()` devuelve **456** siempre (la altura del *espacio de
diseño* de SMG; en la consola el EFB también mide 456 filas, por eso nadie se da
cuenta). En el host el framebuffer es la ventana (1920×1080, 2560×1080, …), así
que todo camino que use esos 456 como si fueran **píxeles** queda mal. Eran dos:

| Dónde | Síntoma en 1920×1080 |
|---|---|
| `compat::ui::framebufferSize()` (nuevo) alimenta el mapeo de la UI | escala 1.0 en vez de 1080/456 = 2.37 → logo, prompt y copyright **2.4× más pequeños**, y el viewport de 2D medía 1920×**456**, así que toda la UI aparecía además en la franja superior del fotograma |
| El aspecto del cielo (`fbWidth / getScreenHeight()`) | 4.21 en vez de 1.78 → anisotropía 2.4:1 (estrellas estiradas en vertical, mar gigante, sol enorme) |
| El eje Y de `fillUiViewMtx` (`eyeScaleY = scale`) | al pasarle la altura real, el eje Y se normalizaba **otra vez** por 456: 2.37 px/unidad en X pero 5.61 en Y → logo, prompt y botones **estirados 2.37× en vertical** y desplazados (el logo se salía por arriba y el prompt por abajo) |

La segunda mitad del problema estaba en la matriz de vista. La ortho de
`setupDrawForNW4RLayout` abarca **608 unidades de diseño a lo ancho y 456 a lo
alto** del framebuffer, así que un "ojo" no es un píxel en ningún eje:

```
px/unidad X = eyeScaleX × W/608        px/unidad Y = eyeScaleY × H/456
```

Las dos tienen que dar `scale`:

```cpp
eyeScaleX = scale * 608 / W;      // antes ya era así
eyeScaleY = scale * 456 / H;      // antes: eyeScaleY = scale  ← estiramiento vertical
```

`eyeScaleY` vale exactamente **1.0** cuando 456 unidades ocupan la altura (el caso
normal: 1280×720, 1920×1080, 2560×1080, 3440×1440, 3840×2160) y solo baja de 1 en
framebuffers más estrechos que 4:3, donde la composición se encoge para caber. El
error quedaba oculto mientras el llamador también pasaba la altura de diseño
(`scale` = 1.0, los dos ejes a 1:1) y se destapó al alimentar el mapeo con los
píxeles reales. Lo fija el test
`layout_pass_maps_layout_units_uniformly_to_pixels` para las cinco resoluciones.

El fix de la geometría es una única fuente de verdad en píxeles:

```cpp
f32 w, h;
compat::ui::framebufferSize(&w, &h);   // render mode del host = ventana
```

`compat::ui::framebufferSize()` (en `UiAnchoring.{h,cpp}`) lee
`Platform::CompatVi::hostRenderMode()` (`fbWidth`×`efbHeight`, sincronizado con la
ventana cada fotograma por `MainLoopFramework::beginRender`) y cae a 608×456 en
tests headless. Lo usan `LayoutManagerCompat` (`initDrawInfo`,
`updateUiAnchoring`, `convert*`, `getScreenWidth`), `SceneCompat`
(`drawInitFor2DModel`, `isScreen16Per9`) y `TitleSky::draw`. La ortho de
`setupDrawForNW4RLayout` (±228 / ±304) sigue en unidades de diseño: quien convierte
a píxeles es la matriz de vista, no la proyección.

### 6. Render mode / present

- `Platform::CompatVi::hostRenderMode()` es la única fuente de verdad del
  `GXRenderModeObj`; `syncHostRenderModeWithWindow()` lo actualiza desde el
  tamaño real del drawable (SDL_GetWindowSizeInPixels) en `beginRender`.
- El blit final EFB → swapchain es **centrado y con aspecto preservado**
  (letter/pillarbox), no un estirado a toda la extensión.

## Cómo comprobarlo

```bash
# ventana normal
./build/src/galaxy-pc --width 1920 --height 1080 --boot

# sin tarjeta gráfica (headless, lavapipe) + captura del EFB en PPM
Xvfb :99 -screen 0 3840x2160x24 &
DISPLAY=:99 VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  ./build/src/galaxy-pc --width 3440 --height 1440 --frames 120 \
  --screenshot /tmp/title.ppm
```

`--screenshot PATH` (PPM/P6) escribe el EFB tal cual se presenta: sirve para
medir costuras (discontinuidades de columna), la línea del horizonte y la
posición exacta del logo/prompt/iconos. Requiere assets montados
(`--assets-dir` / `GALAXY_ASSETS_DIR`, ver `docs/assets.md`) para ver el título
de verdad; sin ellos solo corre la demo M5.

### Capturar el título real (`--boot --screenshot`, tecla F12)

El `--screenshot` del bloque de arriba captura el bucle nativo (demo M5). Para
medir el título de verdad (`--boot`) hay dos vías; ambas escriben PPM/P6 con el
contenido exacto del EFB ya presentado (`Renderer::readRenderTarget` tras
`flushFrame()`), a la resolución del EFB (= resolución de la ventana):

```bash
# A) captura a fotograma fijo: presenta #1800 (~30 s, ya en el título) y sale
./build/src/galaxy-pc --boot --assets-dir <ruta-con-assets> \
  --width 1920 --height 1080 --screenshot title.ppm --log-file capture.log
#    --frames N     -> captura el presenta #N en vez del #1800

# B) a mano, sin adivinar el fotograma
./build/src/galaxy-pc --boot --assets-dir <ruta-con-assets> --width 1920 --height 1080
#    F12 -> luma-frame-NNNNN.ppm (uno por pulsación, en el directorio actual)
#    Esc -> cierre limpio (mismo camino que cerrar la ventana)
```

Cada `--boot` que llega al título deja además tres bloques de diagnóstico útiles
para comparar con la referencia: `TitleSky draw #N: fb=1920.0x1080.0 fovy=60.0
aspect=1.7778 angleX=... angleY=... step=...` (el `step` importa: el domo se
balancea con período de 750 fotogramas) y `GXSetProjection #N (perspective):
viewport=(0,0 1920x1080) depth=0.00..1.00` (el rectángulo con el que se dibujó la
pasada 3D), y `TitleSky model:` con el contrato del domo (por draw item: material,
shape, joint, `mtxType`, bbox del modelo, texgens, el modo de la matriz de textura
— 8/9 = projmap, 10/11 = envmap con matriz de efecto — y cull/blend/z). El bbox y
ese contrato son lo que decide dónde caen la banda de mar, la línea del horizonte y
el sol en el encuadre. Con eso, un fotograma suelto se puede situar en el tiempo y comparar
contra la captura de la consola sin adivinar.

Sin `--screenshot` ni F12 no se escribe nada: `--frames N` a secas solo sale
tras N presentes. La implementación vive en `src/compat/BootCapture.cpp` y se
engancha al present (`GXCopyDisp` → `compat::notifyBootPresent`), porque
`--boot` corre `gameMain()` y nunca vuelve al bucle de `main.cpp`.
`compat::kBootCaptureDefaultFrame` (1800) es el fotograma por defecto.

## La superficie de abajo del horizonte (mar/tierra): qué es y de dónde sale su color

> Diagnóstico hecho con el asset **real** (`ObjectData/CometNearOrbitSky.arc`),
> no con la demo: el modelo del domo se inspecciona sin arrancar el juego con
> `tools/arc_unyaz0.py` + `build/src/tools/sky-probe` (ver más abajo).

El trozo que se ve *debajo* de la línea blanca del horizonte es la **superficie
de la Tierra** del domo: tres draw items con materiales `EarthFar_v` (shape 4),
`EarthNightMat_v` (shape 5) y sus tres texturas `EarthKsMM` (máscara de
continentes I4), `EarthFarK` (el creciente con el degradado) y `Cloud01k`
(nubes CMPR). No es un disco ni un plano: son casquetes de la esfera de radio
`780778` unidades de modelo, y el domo entero está escalado a `0.8`.

Contrato del material `EarthFar_v` (idéntico al del asset de SMG1 según la
referencia):

| texgen | tipo | fuente | matriz | modo | efecto |
|--------|------|--------|--------|------|--------|
| tc0 | 3x4 | POS | TEXMTX0 | 8 (Projmap) | proyección esférica (…) |
| tc1 | 2x4 | TEX0 | TEXMTX1 | 0 (Basic) | — |
| tc2 | 3x4 | POS | TEXMTX2 | 8 (Projmap) | proyección esférica (…) |

El color lo decide el TEV de tres etapas:

* `tev0`: `EarthKsMM × K0(0,25,52) + rasc` → el fondo del mar (azul muy oscuro).
  La máscara de continentes aclara las zonas de tierra.
* `tev1`: `(1 − TEXA_map1) · prev + TEXA_map1 · (EarthFarK × K1(65,213,209))`,
  o sea **el color teal `(65,213,209)` de la referencia entra exactamente por el
  registro K1**, mezclado con el alfa del creciente.
* `tev2`: `prev + Cloud01k × K2(blanco)` → nubes.

Y la matriz de efecto: los dos texgens *projmap* no llevan la proyección
esférica escrita a mano en el `.bcf`/MAT3 porque el BMD la **sí** lleva escrita
(una proyección esférica completa, con `7000`/`8000` unidades de sesgo). El
juego la sustituye en tiempo de ejecución por `inverse(base)` del actor
(`ProjmapEffectMtxSetter::updateMtxUseBaseMtx`), así que lo correcto es
**componer** las dos (`efecto_del_BMD × inverse(base)`), no tirar la del BMD:
al sustituirla, la proyección esférica desaparece y el mar se convierte en un
papel pintado del mapa de la Tierra estirado sobre el casquete. Ese era el bug
de esta ronda (`BmdRenderer::calcTexMtx`).

Estado de esta ronda (las imágenes comparativas se retiraron del repositorio
por presupuesto de espacio; se regeneran con las capturas del puerto):

* **ANTES** (effect = `inverse(base)` sustituyendo a la del BMD): el mar es un
  papel pintado plano con los continentes de `EarthKsMM` estirados y el sol con
  un borde recto y negro debajo.
* **AHORA** (efecto del BMD compuesto por la derecha, `P × B⁻¹`): la superficie
  vuelve a ser una proyección (degradado continuo y franjas que convergen al
  horizonte), que es la estructura de la referencia.

Lo que **todavía no** cuadra con la referencia: franjas repetidas con un paso
muy visible (en vez del moteado fino) y el orden del degradado (en la referencia
el teal brillante está justo debajo de la línea y oscurece hacia abajo; aquí
oscurece hacia el horizonte y aclara hacia abajo), además de una singularidad en
el borde del sol. La pieza que falta es el
`ProjmapEffectMtxSetter::updateMtxUseBaseMtx` exacto (no está decompilado ni en
la copia vendored ni en `SMGCommunity/Petari@master`, solo la declaración), o el
camino `J3DMLF_UsePostTexMtx` + `J3DDifferedTexMtx` (donde la matriz de efecto
se aplica **después** del texgen, sobre la posición ya transformada por la
matriz de posición). Con la composición sustituida (`B⁻¹ × P`, u otra variante)
el mar se ve distinto pero tampoco coincide: no es una cuestión de "textura
equivocada", las texturas son las del juego.

Dato útil para la siguiente ronda: el atributo TEX0 del modelo tiene
`u ∈ (-3.6, 71.1)`, `v ∈ (-0.7, 11.0)` con `frac 8`; el texgen Basic del
creciente (`tc1`, matriz `(1,1)` con `+0.332` en u) lo repite decenas de veces en
horizontal — eso es lo que produce el moteado fino de la referencia, así que el
moteado no viene de la proyección esférica sino de ese teselado.

### Cómo inspeccionar el asset (sin arrancar el juego)

```bash
python3 tools/arc_unyaz0.py <tree>/ObjectData/CometNearOrbitSky.arc /tmp/sky.rarc
./build/src/tools/sky-probe /tmp/sky.rarc /tmp/skytex        # materiales + texturas
# y para verlo montado de verdad, con el juego:
./build/src/galaxy-pc --assets-dir <tree> --boot --width 1920 --height 1080 \
  --frames 1800 --screenshot sky.ppm --log-file sky.log
```

`sky-probe` imprime, por draw item: material, shape, joint, `mtxType`, bbox,
texgens, la matriz de textura (modo, proyección, SRT, **matriz de efecto
completa**), cull/blend/z y el mapeo `texNo[]` → textura; además escribe cada
textura del TEX1 como PPM.

### El arreglo de los texgens (M9.5.5)

La causa del "moteado fino" quedó localizada y corregida: **el puerto solo
evaluaba los generadores de texcoord cuyo atributo de destino aparecía en el
stream de vértices**. `GXCompat.cpp::finishVertex` recorría el VCD y llamaba a
`resolveTexGen` únicamente para los `GX_VA_TEXn` presentes; la cúpula de este
modelo declara POS/NRM/TEX0, así que los texgens **1 y 2** de `EarthFar_v`
(que son los que muestrean `EarthFarK`, el creciente/resplandor, y `Cloud01k`,
las nubes) nunca se evaluaban: heredaban el texcoord del vértice anterior
(mirado en consola: `in ≈ (-10.05, -10.05)`), y con una SRT de escala 1 eso
repite la textura decenas de veces — el enrejado que se veía en el mar.

`finishVertex` ahora copia una instantánea de los atributos de entrada y evalúa
**los 8 generadores** contra ella (la instantánea evita que un generador que
escribe TEX0 alimente al siguiente). Medido con el `LUMA_SKY_ONLY_MAT=Earth`
para aislar el mar:

| texgen | fuente | matriz | uv medidos (cúpula visible) |
|--------|--------|--------|----------------------------|
| tc0 (projmap) | POS | `mtx30` | u ∈ [-0.52, -0.34], v ∈ [-0.06, 0.90] → **una copia** de `EarthKsMM` |
| tc1 (basic)   | TEX0 | `mtx33` | franja vertical de `EarthFarK` |
| tc2 (projmap) | POS | `mtx36` | u ≈ 0.40 (casi constante), v ∈ [0.22, 0.5+] de `Cloud01k` |

Resultado: el mar pasa de una rejilla
repetida a estructuras suaves y largas, sin repetición. Lo que **sigue**
difiriendo de la referencia:

1. **Brillo/contraste**: el mar nuestro queda claro y uniforme; el de la
   referencia es oscuro con arcos brillantes y una banda teal. El sospechoso
   directo es `tc2` (nubes): su `u` es casi constante, así que aporta un blanco
   plano a toda la superficie; en la referencia las nubes dibujan los arcos.
2. **El teal**: `EarthFarK` es un degradado diagonal (brillo en la esquina
   superior) y su uv cae en la zona recortada (clamp) de la textura. Invertir
   el signo de las filas del proyector (`LUMA_SKY_UV_FLIP=u|v|uv`, prueba
   diagnóstica ya retirada) no cambia el resultado visible, así que no es un
   error de signo simple.

Para reproducir cualquiera de las pruebas: `LUMA_SKY_ONLY_MAT=Earth` /
`LUMA_SKY_HIDE_MAT=Comet` aíslan draw items, `LUMA_SKY_UV_SCALE=k` y
`LUMA_SKY_SCENE_SCALE=s` escalan (diagnóstico) los texcoords y la escena
completa respectivamente.

### Segunda ronda: los texcoords generados no llegaban al shader (M9.5.6)

El arreglo anterior (evaluar los 8 generadores) escribía el resultado en la
**instantánea** de los atributos, mientras el stream que consume el renderizador
se serializa desde `sCurAttr`: los texcoords de `tc1`/`tc2` se calculaban y se
tiraban. Además, aunque hubieran llegado al stream, `GXCompat.cpp::flushDraw`
rellena el layout fijo del TEV (`pos(3) clr0(4) clr1(4) tex0..7(2 cada uno)`)
con **cero** para todo texcoord que la forma no declare como atributo — y el
domo del cielo solo declara TEX0.

Los dos fallos se corrigen ahora:

* `resolveTexGen(coord, attrs, inputs)` lee sus filas fuente de `inputs` (los
  atributos tal como llegaron) y escribe en el atributo destino real.
* `flushDraw` guarda los ocho texcoords que produce el generador por vértice en
  un array paralelo (`sVertexTexCoords`, 8 pares uv) y los usa al construir el
  vértice del TEV en lugar de los del stream (o de un cero).

Medido a 1280x720, perfil del mar por filas (media RGB sobre x=60..1200):

| fila | referencia (consola) | puerto (antes) | puerto (ahora) |
|------|----------------------|----------------|----------------|
| 445  | (122,181,178) | (126,188,186) | (122,185,184) |
| 470  | (124,218,215) | (119,152,161) | (144,230,226) |
| 500  | ( 61,192,191) | ( 24, 57, 82) | ( 71,210,208) |
| 530  | ( 51,154,159) | ( 26, 59, 84) | ( 59,175,179) |
| 560  | ( 66,135,144) | ( 37, 70, 95) | ( 55,145,155) |
| 590  | ( 32, 88,105) | ( 48, 82,106) | ( 43,108,125) |
| 620  | ( 37, 71, 90) | ( 58, 92,116) | ( 33, 76, 98) |
| 660  | ( 17, 43, 67) | ( 69,103,127) | ( 36, 65, 91) |
| 700  | ( 16, 24, 33) | ( 79,112,137) | ( 45, 71, 97) |

Es decir: el mar pasó de **aclararse hacia abajo** (invertido) a reproducir el
degradado de la consola — banda teal en el horizonte, oscurecimiento hacia
abajo.

También se corrigió la **cámara**: la composición completa de la consola (la
banda teal, el resplandor del horizonte y el objeto brillante de la derecha)
está 55 px más abajo a 720p que el look-at sin más; la cámara del título mira
~5.1° más arriba (`kCamPitchDeg`), con lo que la banda teal empieza en la fila
438 (consola: 438) y los dos objetos brillantes caen en (1086,475)/(153,481)
(consola: (1081,477)/(165,478)). El *bob* del domo sigue congelado en angleX ~0,
como en la consola (`JMACosShort(step * 8)` trunca a un entero diminuto).

**Lo que queda**: el mar muestra un patrón de escalones ("affine warping"). La
división proyectiva se hace *por vértice* en `resolveTexGen`, así que dentro de
un triángulo el shader interpola el cociente ya dividido; la consola interpola
`(s, t, q)` y divide *por píxel* en la unidad de textura. Arreglarlo requiere
llevar el `q` (tercera fila de la matriz) al layout de vértice y dividir en el
fragment shader.

### Tercera ronda: división proyectiva por píxel (M9.5.7)

Los "escalones" que quedaban en el mar eran la división proyectiva hecha **por
vértice**: el generador `GX_TG_MTX3x4` dividía s/t entre la tercera fila en la
CPU, el rasterizador interpolaba el cociente ya dividido y dentro de cada
triángulo el resultado era *afín*. La consola interpola **(s, t, q)** y divide
**por píxel** en la unidad de textura (Dolphin `PixelShaderGen`: `coord.xy /
coord.z`).

Ahora el puerto hace lo mismo:

* `resolveTexGen` deja de dividir: publica `q` (`texGenW(coord)`, 1 cuando el
  generador no es proyectivo) y conserva los numeradores.
* El layout de vértice del TEV pasa de 27 a **35 floats**: `pos(3) clr0(4)
  clr1(4) tex0..7(3: s,t,q)`; `flushDraw` rellena los ocho texcoords generados
  (con su `q`) desde un array paralelo, no solo el que la forma declara.
* `gx_tev_vert.vert` transporta `vec3 inUV[8]` y `gx_tev_frag.frag` añade
  `texCoord(i)` = `vUV[i].xy / vUV[i].z`, usado por todas las etapas TEV (y por
  los desplazamientos indirectos).

Medición del rayado (fracción de filas del mar con un salto > 3 niveles de gris
entre filas contiguas / salto máximo): referencia 0.117 / 46.4, puerto **ahora
0.000 / 2.9** — el mar es un degradado continuo, sin bandas.

Perfil del mar por filas a 1280x720 (media RGB, x=60..1200), con la cámara ya
inclinada 5.1° como en la consola:

| fila | referencia | antes (por vértice) | ahora (por píxel) |
|------|-----------|---------------------|-------------------|
| 445  | (122,181,178) | (122,185,184) | (122,185,184) |
| 470  | (124,218,215) | (144,230,226) | (144,230,226) |
| 500  | ( 61,192,191) | ( 71,210,208) | ( 71,209,208) |
| 560  | ( 66,135,144) | ( 55,145,155) | ( 53,143,153) |
| 620  | ( 37, 71, 90) | ( 33, 76, 98) | ( 37, 80,102) |
| 700  | ( 16, 24, 33) | ( 45, 71, 97) | ( 38, 64, 90) |

Diferencia media por canal en esa banda: **13.7 / 255**. Lo que resta es
suavidad: la consola difumina más los arcos de nubes y el degradado del limbo
(probablemente filtrado/mip y la niebla del PE), y el continente de
`EarthKsMM` se marca más en el puerto.

## 4ª ronda — cadenas de mip: la causa de que "la consola difumine más"

Hasta aquí el puerto subía **sólo el nivel base** de cada textura y fijaba
`maxLod = 0` en el sampler (el TODO de `GXTexture.cpp`). La consola no: los
materiales del título llevan `mip = 1` y el sampler elige nivel por píxel.

Datos del BMD (cabeceras TEX1): sólo tres texturas traen cadena —
`IndBendMud` 128x128 (`maxLod` 3.0, bias -0.51), `EarthKsMM` 256x256 (4.0,
+1.00) y `Cloud01k` 256x256 (3.0, +2.00); el resto `mip = 0`. `EarthKsMM` es
el cuerpo del planeta: el mar. Un bias **+1.00** pide a propósito un nivel más
borroso que el natural — precisamente el efecto que faltaba.

Implementado de extremo a extremo:

* `Renderer::TextureDesc` acepta `levels` / `levelData` (niveles RGBA8
  empaquetados en un solo staging, un `VkBufferImageCopy` por nivel, barreras
  y vista cubriendo todos los niveles).
* `SamplerDesc.minLod/maxLod/lodBias` llegan a `VkSampler`
  (`mipLodBias`, `minLod`, `maxLod`), y el hash de la caché de samplers pasó a
  ser campo a campo.
* `GXTexture` decodifica la pirámide completa (`btiDecodeToRgba8` +
  `mipLevelOffset`, tileado por nivel) cuando el cargador declara la longitud
  real del blob (`setTexObjImageBytes`); `BmdModel` calcula ese tamaño con el
  tileado de cada nivel.
* El `maxLod` del sampler se acota a los niveles realmente subidos, para que
  una petición de nivel inexistente nunca muestree fuera de la vista.

Interruptor de A/B: `LUMA_GX_NO_MIPS=1` fuerza sólo el nivel base.

### Veredicto del A/B a 1280x720 (1800 fotogramas)

El cambio afecta **únicamente al mar** (y > 480): 10% de píxeles con |Δ| > 4,
y 47% en las filas 640-720, donde la textura minifica más. Cielo, horizonte y
nubes quedan idénticos bit a bit (`Cloud01k` casi no aporta en pantalla).

| métrica (mar, filas 478-688) | referencia | sin mips | con mips |
|------------------------------|-----------|----------|----------|
| media                           | 98.1 | 106.9 | 107.8 |
| desviación                      | 54.3 |  56.1 |  55.8 |
| alta frecuencia                 |  1.34 |  0.71 |  0.38 |

La referencia viene de un vídeo recomprimido, así que su alta frecuencia no
sirve como patrón de nitidez; manda el perfil por filas. Con la alineación
fila a fila ajustada contra la referencia (`a = -4`, `b = 0.991`), el error
medio por fila es un empate (sin 7.98 / con 8.25), pero **en las filas
profundas manda la cadena**: y=704 → error 8.8 sin mips contra **1.2** con
mips; y=648 → 7.7 contra 7.1. Son las filas donde el nivel base enmascaraba
aliasado; las costas de `EarthKsMM` pasan de bordes duros a la suavidad que
muestra la referencia, que es lo que el bias +1.00 del artista busca.

Decisión: **se conservan las cadenas**. La costura que quedaba ya no es la
falta de mips.

Nota de medición: la referencia tiene **barras negras** propias (filas 0-12 y
708-719, media < 20). Las comparaciones antiguas de la fila 700 medían parte
de nuestro mar contra esa barra; el perfil debe recortarse a las filas de
contenido (13-707).

## 5ª ronda — el detalle que falta está en el mar medio (y no son los mips)

Con las cadenas de mip ya dentro, quedaba una diferencia visible: la referencia
tiene moteado fino en el mar y el puerto sale liso. Medido con paso alto
(`a - gauss(a, σ=12)`, desviación del residuo) por bandas, y energía por
octavas (FFT del mar x150-1150 y490-707, % del total):

| banda (y) | hp_std referencia | con mips | sin mips |
|-----------|------------------|----------|----------|
| 480-560 (teal)      | 13.00 | 9.42 | 9.77 |
| 560-640 (mar medio) | **25.06** | **7.41** | **8.37** |
| 620-707 (profundo)  | 10.12 | 11.91 | 9.52 |

| octava (px) | >160 | 80-160 | 40-80 | 20-40 | 10-20 | 5-10 | <5 |
|-------------|------|--------|-------|-------|-------|------|----|
| referencia  | 70.7 | 15.8 | 7.6 | 3.4 | 1.5 | 0.8 | 0.1 |
| puerto      | 71.6 | 17.8 | 6.8 | 2.4 | 0.9 | 0.3 | 0.1 |

El déficit es de **3x** y está concentrado en el mar medio (560-640), justo la
franja del manto de nubes. Hipótesis descartadas con medición:

* **los mips**: con `LUMA_GX_NO_MIPS=1` la banda apenas cambia (7.41 → 8.37) y
  las octavas salen idénticas; el sesgo de mip no es lo que borra el detalle.
* **la resolución de la captura de referencia**: renderizar a 1920x1080 y
  reescalar a 720p (supersampling) da el mismo espectro (hp_std 9.16 frente a
  8.20, octavas iguales), así que no es que la referencia se vea más nítida
  por venir de una captura de más resolución.
* **el ruido de compresión**: el cielo de la referencia (zona plana) tiene
  hp_std 2.98 y su banda lateral no muestra periodicidad de bloques; el vídeo
  no es ruidoso.

Aislando materiales (`LUMA_SKY_ONLY_MAT=EarthFar`) el mar sale **idéntico** al
fotograma completo (hp_std 7.99 frente a 8.20; mar profundo 11.28 en ambos),
así que todo lo que falta está dentro de `EarthFar_v`.

Siguiente paso: instrumentar la resolución del texgen (registrar las matrices
3x4 resueltas por material y el recorrido UV resultante sobre la geometría) y
comparar con lo que produce la consola. La pista previa —la referencia
muestrea las texturas del planeta ~una vez sobre el mar mientras nuestros
texgens recorren decenas de unidades, y escalar el SRT **no cambia nada**
(`LUMA_SKY_UV_SCALE` inefectivo)— apunta a que el recorrido UV lo fija la
matriz de efecto (el `ProjmapEffectMtxSetter` del juego), no el SRT, que es
exactamente la pieza que no está decompilada aguas arriba.

### La medida que lo cierra: cuánta textura recorre el mar

Con `LUMA_GX_UV_LOG=1` el puerto registra, por draw, el recorrido real de
coordenadas ya dividido (lo que ve el muestreador) y el rango de `q`. Sobre el
cilindro del planeta (`EarthFar`/`EarthNight`, 8 sectores):

| draw | vértices | recorrido tc0/q (Tierra) | recorrido tc2/q (nubes) | q |
|------|----------|--------------------------|--------------------------|---|
| anillo completo | 49 | u 30.1  v 25.8 | u 3.0  v 12.9 | 20500-272636 |
| sector 45° | 4-5 | u 8-10  v 12-14 | u 1.0  v 5.9-6.8 | 20931-272664 |

Es decir: **el mapa de la Tierra se repite ~10 veces por sector y ~30 sobre el
anillo**, con `q` variando 13x (el divide por píxel ya es correcto, pero el
recorrido de la proyección es demasiado fino). La referencia muestra el mapa
**una vez** sobre el mar visible — por eso lo que para la consola son
continentes es para nosotros un mosaico tan minificado que los mips lo
promedian: queda el degradado liso que se ve en la comparación.

Y esto explica por qué `LUMA_SKY_UV_SCALE` (que escala el SRT) no hacía nada:
en los texgens proyectivos el recorrido lo fija la **matriz de efecto**
(el `projmap` del juego), no el SRT. La palanca correcta son las filas x/y de
la matriz compuesta que se carga en `GXLoadTexMtxImm`, no el SRT.

Siguiente paso concreto: un interruptor de escala sobre esa matriz
(`LUMA_SKY_PROJ_SCALE`) para barrer el factor, y con él buscar la ley que usa
la consola (`ProjmapEffectMtxSetter::updateMtxUseBaseMtx`, la única pieza del
camino que no está decompilada aguas arriba).

### 6ª ronda — ni la escala del projmap ni un sesgo distinto lo cierran

Dos palancas nuevas, cada una medida:

**Escala de la matriz proyectiva** (`LUMA_SKY_PROJ_SCALE`, filas x/y de la
matriz compuesta). Si el recorrido de ~10 repeticiones por sector fuese el
problema, reducir la escala debería acercarnos a la referencia. Es al revés
(hp_std del mar medio a 640x360, referencia 29.9):

| escala | 1.0 | 0.5 | 0.25 | 0.1 |
|--------|-----|-----|------|-----|
| mar medio | 11.0 | 7.2 | 9.7 | 10.2 |

Reducir el recorrido agranda las manchas y **quita** detalle: el projmap no
está demasiado fino, descartado.

**Sesgo de mip** (`LUMA_GX_BIAS_SCALE`, multiplica el bias del BMD). Aquí sí
hay señal, sobre todo a 720p:

| hp_std 720p | ref | bias x1.0 (consola) | x0.5 | x0.25 | x0 | x-0.5 |
|-------------|-----|------|------|-------|----|-------|
| banda teal (480-560) | 13.00 | 9.42 | 9.42 | 9.42 | 10.04 | 11.06 |
| mar medio (560-640) | 25.06 | 7.41 | 11.99* | 13.62* | 10.62 | 8.87 |
| mar profundo (620-707) | 10.12 | 11.91 | 22.68* | 25.64* | 10.27 | 9.47 |
| mar completo | 21.29 | 8.20 | 13.54* | 14.89* | 9.33 | 8.93 |

(*medidos a 640x360, no comparables fila a fila con los de 720p.)

A 720p, el bias neutro (x0) suma ~43% de detalle en el mar medio y acerca la
fila 560 al color de la referencia —(51,149,158) → (56,154,162) frente a
(85,156,163)—, pero deja el mapa de la Tierra con costas marcadas donde la
referencia muestra una masa oscura y blanda. El bias de consola hace lo
contrario: velo blanco arriba, fondo blando. **Un solo sesgo global no puede
cuadrar las dos franjas**, señal de que lo que resta no está en el filtrado
de mip sino en la composición/proyección de las capas del material
(`EarthKsMM` + `EarthFarK` + `Cloud01k`), que es donde sigue el trabajo.

Ambas palancas se quedan con su valor por defecto (1.0 = comportamiento de
consola) para no cambiar el resultado sin decisión.


## Ronda B — los iconos [A]/[B] de "Press both [A] and [B]."

La línea la dibuja `compat/ui/ButtonPrompt.cpp` (el host no tiene sistema de
mensajes ni picture-font: en la consola los símbolos eran glifos del
picture-font metidos en el archivo de mensajes). Los iconos son geometría
vectorial reproducida a partir del fotograma original, **no** un rediseño.

Antes de tocar nada se intentó conseguir el sprite original: la hoja de The
Spriters Resource está detrás de Cloudflare (403 con navegador, proxies
devuelven 404) y no existe ningún rip público del picture-font; las capturas
alternativas son de 230-400 px. La referencia 1280x720 es, con diferencia, la
mejor fuente disponible — y es exactamente el objetivo a igualar.

### Medidas tomadas del original (cap height de la línea = 24 px)

| elemento | original | antes | ahora |
|---|---|---|---|
| [A] diámetro exterior | 39 px (1.62 x cap) | ~27 px (1.14) | 1.62 x cap |
| [A] cara blanca | 32 px (1.33 x cap) | — | 32 px |
| [A] anillo oscuro | ~3.5 px | ~1 px | 3.5 px (0.09 x alto) |
| [B] teja exterior | 33 x 39 px (0.85 x alto) | 27 x 27 (cuadrada) | 33 x 39 |
| [B] marco oscuro | ~4.5 px | ~1 px | 4.5 px (0.115 x alto) |
| [B] radio de esquina | ~6 px (0.18 x ancho) | — | 0.18 x ancho |
| letra [A] / [B] | 21 px / 19 px | ~15 px | 0.875 / 0.79 del cap |
| tinta de la letra | gris ~124 | casi negra (24) | gris 124 |
| centro del icono | 1.2 px bajo el texto | 0 (alineado) | 0.05 x cap |
| huecos de la línea | ~14 px | 8 px (0.34 cap) | 0.60 x cap |

Además se corrigió un fallo de trazado real: la teja [B] se generaba muestreando
radialmente la función soporte del rectángulo redondeado, lo que **pinza las
esquinas** (salía una forma de trébol de cuatro lóbulos, visible al comparar con
la referencia). Ahora se trazan los cuatro arcos de esquina unidos por lados
rectos. Eso dejó de ser evidente al mirar el juego, pero se veía al ampliar.

### Cómo se verifica

`tools/prompt_icons.py` (offline) dibuja la MISMA geometría a la escala del
fotograma original y la vuelve a medir con el mismo detector usado sobre la
referencia:

```
python3 tools/prompt_icons.py medir
  ORIGINAL       cara 32x32     REPRODUCCION  cara 31x31     ([A])
  ORIGINAL       cara 23x28     REPRODUCCION  cara 24x29     ([B])
```

Los tests unitarios (`ui_anchoring_test.cpp`) fijan esas proporciones, de modo
que la línea conserva el aspecto original a cualquier resolución (todo se
deriva del cap height del layout). Palancas de ajuste fino:
`LUMA_PROMPT_ICON_SCALE`, `LUMA_PROMPT_B_WIDTH`, `LUMA_PROMPT_RING`,
`LUMA_PROMPT_RING_B`, `LUMA_PROMPT_CORNER`, `LUMA_PROMPT_WORD_GAP`,
`LUMA_PROMPT_LETTER_A/B`.

## Ronda C — los iconos son los del juego

El volcado del font de la consola (`LUMA_PICFONT_DUMP`, ver `PictureFontDump.h`)
ya se ha corrido sobre los assets reales. Lo que dijo el log:

```
picture font: RFNT, 197036 bytes, header 16, 4 data block(s)
FINF: type=1 encoding=1 height=32 width=32 ascent=25 alterChar=0 ...
TGLP sheet 0: 128x128 RGB5A3, grid 3x3, cell 34x35, size 32768, off 96
CMAP: codes 0x0020..0x0066 (method 1)  ->  71 glyph cells
```

La hoja de contactos (fila 2 = códigos 0x0030..0x0037) muestra, en orden,
[A], [B], [C], wiimote, wiimote, "1", "2", estrella ⇒ **0x0030 = [A]** (glifo 1,
celda de hoja (col 1, fila 0)) y **0x0031 = [B]** (glifo 2, (2,0)).

Así que `ButtonPrompt` ya no dibuja una reproducción cuando el font está
montado, sino **los texeles originales**:

* `compat/ui/PictureGlyphs.cpp` resuelve el código contra el font que el port ya
  monta (`/LayoutData/Font.arc` → `/PictureFont.brfnt`, `GameBoot.cpp`) con
  `nw4r::ut::Font::GetGlyph`, decodifica la hoja (RGB5A3 → RGBA8, `Bti.h`) y mide
  las dos cajas de tinta del glifo: `solid` (el icono, la letra incluida) y
  `outer` (el icono más su sombra suave).
* El quad se dibuja con la altura de `solid` igual a la medida del icono
  (1.62 × cap) y con `solid` centrada en el hueco, de modo que la sombra del
  propio glifo cuelga por debajo igual que en el original. La UV sale de
  `cellX/cellY`, que en NW4R es `col*(cellW+1)+1`, `row*(cellH+1)+1`
  (`ut_ResFontBase.cpp`), con `GX_CLAMP` + `LINEAR`.
* El avance del hueco usa el ancho del arte, no una constante, así que los
  huecos de la línea salen del propio font.
* Sin font (assets sin `LayoutData/`, tests) se sigue dibujando la reproducción
  vectorial medida de la referencia: nada cambia en ese caso.

El log de arranque dice cuál de los dos caminos se usó:

```
[compat.font] picture font installed for the [A]/[B] icons (cell 34x35)
[compat.font] picture font: code 0x0030 -> cell (36,1) 34x35 of 128x128, icon 24x24 texels, with shadow 27x31
[compat.font] picture font: [A]=real glyph [B]=real glyph
```

El volcado también se corrigió: usaba `width / gridW` como paso de celda (42 en
una hoja de 128 con rejilla 3x3) en vez del real (celdas de 34x35 con canal de
1 texel), así que las ventanas de la hoja de contactos quedaban ~7 texeles
corridas y enseñaban trozos del glifo vecino. Ahora `PictureFontDump` sigue el
trazado de NW4R y `picfont_codes.txt` anota el origen de cada celda.

### Ronda C, continuación: la escala es UNA y los iconos van alineados abajo

Con el font montado, el log dice cuánto ocupa cada glifo:

```
picture font installed for the [A]/[B] icons (cell 34x35)
picture font: code 0x0030 -> cell (36,1) 34x35 of 128x128, CWDH ..., icon 28x28 texels, with edge 30x30
picture font: code 0x0031 -> cell (71,1) 34x35 of 128x128, CWDH ..., icon 22x28 texels, with edge 22x28
picture font: [A]=real glyph [B]=real glyph
```

Dos consecuencias, las dos visibles:

1. **El arte de [A] y [B] NO mide lo mismo**: 28x28 texeles contra 22x28. La
   consola dibuja todos los glifos de un font a UNA sola escala (la que pide el
   layout), así que la diferencia de tamaño es del arte, no un ajuste. Escalar
   cada glifo a la misma altura (lo que hacía la primera versión) estiraba [B]
   un 7% y rompía la proporción entre los dos iconos. Ahora la escala se deriva
   una vez del glifo [A] y se comparte.
2. **Van alineados por abajo, no centrados**: en la referencia [A] llega a
   y=544 y [B] a y=548, pero los dos acaban en y=580 — el patrón de dos glifos
   apoyados en la misma línea dentro de su celda. El icono más bajo se sube
   `(altoA - altoB)/2`, que es lo que hace el original.

Medido en la referencia a 720p (ventana de cada icono): [A] ≈ 37 px de alto,
[B] ≈ 33 px, con los bordes inferiores a la misma altura. Con la escala única
(1.62 x cap para el cuadro de tinta de [A]) salen 38.9 px y 36.3 px: dentro de
un par de píxeles, y sin ninguna constante extra.

También se confirmó que **los glifos no traen sombra suave propia** (la caja de
"tinta" y la de "tinta + borde" casi coinciden): la sombra de cuatro pasadas se
queda solo en la reproducción vectorial de respaldo, que es la que se calibró
contra la referencia cuando no había font.

## Ronda D — los iconos salen, y el flash de la intro

### 1. Los iconos [A]/[B] no se veían (UV de la hoja)

Con el font ya montado, la primera captura del usuario mostraba la línea
correctamente compuesta pero **sin iconos**: el hueco del [A] y el del [B]
estaban reservados y vacíos, y tampoco aparecía la reproducción vectorial
(el glifo "real" devolvía `true`, así que la caída nunca se ejecutaba).

La causa estaba en `inkBoxOfRgba8()`: devolvía la caja de tinta **relativa a la
celda** (0..30), pero `fillRects()` la normalizaba como si fuese un rectángulo
de la **hoja completa**. Las UV del quad apuntaban por tanto a la esquina
superior izquierda de la hoja — que en este font es la **celda 0, el glifo del
espacio**: un cuadro transparente. El quad se dibujaba, con el tamaño y la
posición correctos, muestreando el vacío.

Corregido: la caja se devuelve en coordenadas de IMAGEN (incluye el origen de
la celda), que es lo que el sampler usa directamente. El test de la caja fija
justamente eso (`solidX == 3` para un bloque en (3,3) de una imagen de 12x12,
es decir origen + offset, no offset a secas).

### 2. El flash de la intro dejaba los laterales sin cubrir

`bug.png` (frame de la intro, 1280x720) mostraba el flash de aparición del logo
cubriendo **exactamente** un rectángulo de x=160..1119 y de altura completa —
o sea 960 px de ancho: el área de diseño 4:3 (608x456 unidades a 1.579 px/unidad).
Fuera de ese rectángulo la escena quedaba sin destello, con un corte vertical
duro en los bordes (visible a simple vista en los laterales de la captura).

El pane responsable es **`PicFlash`** (TitleLogo.arc): un `pic1` con una textura
de 8x8 estirada sobre toda el área de diseño y desvanecida por la animación
"Appear" del logo. La consola solo tenía que cubrir 4:3; en pantalla ancha hay
que cubrir el framebuffer entero.

Regla añadida (`UiAnchoring`, pura y con test):

```
screenCoveringPaneScaleXForFramebuffer(paneW, paneH, pixelW, pixelH)
```

Un pane que cubre el área de diseño (ambos ejes, con 1 unidad de tolerancia) es
un EFECTO que ocupa pantalla, no UI: se ensancha **simétricamente respecto a su
centro** (que es el centro de la pantalla) hasta cubrir el ancho visible. Es
exactamente la misma regla del fondo: ampliar los lados, mantener el centro.
Los panes normales (logo, botones, textos) no se tocan nunca: factor 1.

A 1280x720 el factor sale 1280/(720/456)/608 = **1.333**, y el pane cubre los
810.7 unidades de layout que muestra la ventana (no las 832 del "16:9" de la
Wii, que usa píxeles no cuadrados). En ultrapanorámicas crece más, hasta cubrir
siempre el ancho visible.

El dibujo se hace en `Picture::DrawSelf` (patch de `lyt_picture.cpp`) moviendo
el punto base y el ancho del quad, así que no toca ninguna matriz ni el resto
del estado GX. Se reporta una vez por pane en el log:

```
[compat.lyt] screen-covering pane 'PicFlash': 608x456 design units widened x1.333 to cover the framebuffer
```

## Ronda E — los iconos, de pie y a la altura del texto

Sus capturas de la ronda anterior (`Iconos.png`) dejaron tres cosas claras: los
iconos ya **se dibujan** (el bug de UV de la Ronda D está resuelto), pero salían
**boca abajo**, **demasiado altos** y **algo grandes**. Las tres tienen la misma
raíz: suposiciones de signo y de tamaño que no se habían medido contra el frame.

### 1. Eje Y: hacia abajo, igual que el texto

El espacio en el que se dibuja la línea (el del `WideTextWriter`, que es el que
cargó `LoadMtx`) tiene **+Y hacia abajo**: `printRun` deja la pluma en el borde
superior y los glifos crecen hacia valores mayores de y. Las UV del quad estaban
emparejadas al revés (`v0` con el borde de abajo), así que el icono salía
invertido. Ahora `v0` (la fila superior de la hoja) va al borde de **arriba**.

El mismo error de signo estaba en el respaldo vectorial: el degradado del bisel,
la sombra (que caía hacia arriba en vez de hacia abajo) y la Y de la letra.

Y estaba también, más visible, en la **posición**: el icono se colocaba a
`textTop − capHeight/2`, es decir media línea **por encima** del texto. Medido
sobre su captura, la cara del icono quedaba 43 px por encima del centro de la
línea; en la referencia queda **1.5 px por debajo**. Ahora:

```
capsTop   = textTop + (ascent − capHeight)      // la pluma va en la línea de ASCENSO
iconCenterY = capsTop + capHeight/2 + dropY     // dropY = 0.05 cap (≈1.3 px)
```

### 2. Tamaño: 1.62 cap es la caja EXTERIOR, no la cara

Los 39 px que se midieron en la referencia son la **caja exterior** del icono,
que es la cara **más el anillo oscuro** por los dos lados: cara 32 px + 2 x 3.5 px
= 39 px, o sea `cara = 0.82 x exterior` (= `1 − 2·ringRatio`, la misma relación
que usa el respaldo vectorial). El glifo del font de imágenes lleva su propio
borde en el arte, así que **se escala a la CARA**, no a la caja:

```
faceSize   = size * (1 − 2·ringRatio)      // 32 px  (1.33 cap)
glyphScale = faceSize / glyph.solidH       // caja sólida del glifo (28 texeles)
```

Escalando a la caja exterior el disco salía de 49 px en vez de 32 (lo que se ve
en `Iconos.png`), y con ello la línea entera se estiraba ~50 px de más.

### 3. El ascenso no es la altura de las mayúsculas

`ctx.capHeight` es lo que reporta el writer: el **ascenso** de la fuente. Todas
las medidas de la referencia están en **alturas de mayúscula** (la línea mide
24 px de mayúscula), y en esta fuente el ascenso es mayor: medido sobre el
título a 720p, mayúsculas 26 px contra ~30 px de ascenso. El factor
`kFontAscentToCapHeight = 0.87` (26/30) convierte una en otra, y por él pasan el
tamaño del icono, sus huecos, la separación entre palabras y `dropY`.

### 4. Diagnóstico del brillo de la intro (abierto)

Pendiente de dato: el destello puede venir del propio pane `PicFlash` o de
`PicBloomA/B` (TitleLogo.arc), y su posición vertical depende de la animación
RLPA de esos panes. Los `emitEffect("TitleLogoLight*")` del juego siguen siendo
un stub (`effects stub (M10)`), así que lo que se ve es solo el layout.

Para poder contestarlo con datos en vez de a ojo, el volcado del árbol de panes
(`dumpPaneTree`) pasa a ser **por layout**: antes usaba un contador global y solo
lo imprimía el primer layout que dibujaba (WiiRemoteStrap), de modo que el árbol
del TitleLogo nunca aparecía en el log. Ahora cada layout lleva su propio
contador y se vuelca en los dibujos 1, 2, 3, 4, 6, 9, 13, 18, 25, 35, 50, 70,
100, 140, 190, 250 y 300 — la secuencia de aparición completa (~4 s) al
principio y luego más espaciado. Cada línea trae `glbPos`, `pos`, `scale`,
alpha, `anm` (número de animaciones ligadas al pane) y la textura del material,
que es lo que identifica a `PicFlash` (8x8) frente a los bloom.
