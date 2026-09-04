# Informe — pantalla en negro en el boot (M9.5.3c) y fixes aplicados

Fecha: 2026-09-03 · Repo: `GaudiestBaker74/Project-LUMA` @ `95a2002` ("M8-M9 doesnt appear anything on screen")

## 0c. LA SEGUNDA IMAGEN DEL WIIMOTE NO CARGA (arreglado, M9.5.3c+)

**Síntoma** (reporte del usuario tras el fix del crash): el strap muestra el
mando pero la segunda imagen (la que aparece al activarse la animación) nunca
llega. El boot.log avisaba `brlan: pat1 not converted` y
`group controllers not ported yet`.

**Diagnóstico**: el aviso de `pat1` era un espejismo — `pat1` es solo
metadatos; los cambios de textura reales viajan en el `pai1` como tags RLTP,
que el swapper YA convertía. El agujero estaba en el runtime
(`AnimTransformBasic::Animate`): los tags de material (RLTS/RLMC/RLTP) que
cuelgan de un contenido de PANE (así los escribe Nintendo en el brlan: el tag
nombra al pane) se ignoraban con el comentario "Material-side tags on a pane
content: ignored". El RLTP de la consola se aplica al material del pane
(`pPane->GetMaterial()`).

**Fix** (`LytMissing.cpp`): `ApplyMaterialTag()` compartido — el route de
pane lo llama con `pPane->GetMaterial()`; el RLTP resuelve
`entry->target` (slot del TexMap 0-7, con fallback a `entry->index`) contra la
tabla de ficheros del pai1 (`SetResource` → `GetResource('timg', nombre)`) y
hace `TexMap::ReplaceImage` con el clamp de frames de J3DAnmTexPattern.

**Verificación E2E**: el generador sintético (`tools/make_synth_strap.cpp`)
ahora emite una tabla de 2 ficheros (play.tpl/alt.tpl) + RLTP con clave
frame 100 → alt.tpl (verde 568x392) bajo el contenido de PANE "PicPlay". En el
sandbox: tabla resuelta en el log, 0 drops, y el probe del EFB en el frame 300
pasa de azul (play.tpl) a **verde (33,178,58)** — el swap de textura funciona
de verdad. De paso: fix de un desbordamiento memset en `SetResource`
(vendido) + logs de resolución de la tabla de ficheros (WARN si un nombre no
resuelve — es exactamente lo que hay que mirar si en el arc real alguna
imagen no carga).

**Nota para el usuario**: si con tu arc real la segunda imagen siguiera sin
aparecer, el log nuevo dirá `SetResource: file table[N] '<nombre>' NOT
FOUND — RLTP patterns using it will be dropped`: ese nombre es el que hay que
comparar con los ficheros timg de tu arc (case-sensitive, con extensión).

## 0d. M9.5.3d — TITLE SCENE (y la gran caza de memoria)

**Qué se pidió**: "por mi sigue con el Title" (docs/m9.5.3-plan.md).

**Qué llegó**:
- `requestGalaxyMove` real (tipo 7 → "Title"; los tipos con galaxy son M9.5.4),
  `MR::requestChangeSceneTitle`, ctor de `GalaxyMoveArgument`, y
  `notifyToGameSequenceProgressToEndScene` → Title (el Logo ya no se queda
  aparcado en Deactive).
- `TitleScene` (reconstrucción host, `TitleScene.cpp/.hpp`) + registro "Title"
  en SceneFactory + `EncouragePal60Window` inerte (en PC no hay PAL60) +
  `TitleSequenceProduct` vendido compilado (necesitaba: entrada real A/B vía
  KPAD, stubs de audio que devuelven "listo" en `isPreparedStageBgm`,
  `emitEffect`/`deleteEffectAll` no-op, `testCorePadButtonA/B` reales leyendo
  los canales KPAD — que de paso hacen el strap saltable con cualquier botón).
- Hito visible: Logo → strap → `createScene('Title')` → secuencia de título
  (con tus assets TitleLogo/PressStart.arc se ve el logo + press start; sin
  ellos, fondo negro con logs elegantes). Al Decide: "the title parks here"
  (FileSelector = M10).

**La caza de memoria** (el precio real de la primera transición de escena de
la historia del port — cada crash de esta ronda era el mismo enfermo):
1. El `operator new` global alimentaba el heap de escena JKR
   (`JKRHeap::alloc(nullptr)` = heap corriente). `destroySceneHeap()` en cada
   transición lo reventaba entero: todos los `std::unordered_map`/vectores de
   la capa platform (caché de pipelines del renderer, samplers, tablas GX)
   quedaban con nodos colgantes que el heap del Title reciclaba con datos
   cualesquiera (se encontraron `1.0f` de vértices donde debía haber un
   puntero de nodo). **Fix**: plain new → asignador del sistema; JKR queda
   para `new (heap, align)` y los caminos deliberados.
2. Faltaba `operator new(nothrow)`: el JIT LLVM de lavapipe asignaba con él
   (allocador del sistema) y liberaba por pcPortFree → ASAN lo cantó
   (heap-buffer-overflow en `Platform::Memory::free`).
3. `waitForTick` (MainLoopFramework) pasaba `(OSMessage*)&msg` con
   `u32 msg` — OSMessage es `void*` (8 bytes en host): 4 bytes de stack
   corruptos **por frame** (ASAN: stack-buffer-overflow).
4. El registro de hilos de `OSThread.cpp` se escribía con mutex pero se leía
   sin él (trampolines de hilos) — 37 data races de TSAN; un `HostThread*`
   basura durante un rehash = escrituras arbitrarias.
5. `~NameObjCategoryList` hacía delete doble (mDelegator/mDelegatorConst son
   una UNIÓN) — dormante desde siempre: la escena Logo nunca se destruyó
   hasta el Title.
6. `JKRExpHeap::create(size,...)` no marcaba `_6E=1` → `do_destroy` nunca
   devolvía el bloque al padre: el primer destroyGameHeap fundía el arena
   (el heap recreado tenía 32 KB libres y el Title pedía 17 MB).
7. Pendiente M9.5.4: la coalescencia del free-list del ExpHeap peta
   ("Bad Block" en joinTwoBlocks) al devolver heaps con historia real al
   padre; mientras tanto `destroyGameHeap` mantiene vivos los game heaps
   (van casi vacíos desde el fix 1).

**Evidencia**: suite 187/0/3 (2 tests nuevos: ciclo solid-heap y ciclo GDDR
completo). Boot sandbox ×2: vivo al timeout (exit 124), Title a 2400+ updates
sin crash. ASAN/TSAN/UBSan-free en las rutas ejercitadas.

## TL;DR

1. **Tu binario era más viejo que tus fuentes.** El `galaxy-pc.exe` con el que sacaste ese log
   estaba compilado de un árbol anterior a tu último commit: no emitía NI UNA de las líneas de
   diagnóstico que el código actual escribe SIEMPRE a INFO (`GXCopyDisp #0: efb=… blit=…`,
   `present #0: result=0 hadContent=…`) ni el sello de build que ahora se añade. Con ese exe
   viejo, el EFB llegaba 100 % negro (`nonBlack=0/291840`) — ni siquiera llegaba el quad blanco
   de `fillScreen` que el `LogoScene` pinta cada frame.
2. **El código actual (HEAD) funciona de verdad.** Compilado desde cero en el sandbox:
   - Boot sin assets → `EFB probe: nonBlack=291840/291840 avgRGB=(255,255,255)` (todo blanco, correcto).
   - Boot con un `WiiRemoteStrap.arc` sintético réplica del tuyo (mismos panes/tamaños/formatos) →
     `EFB probe: … avgRGB=(152,140,115)`, centro `(222,40,41)` = las barras de color de PicPlay.
     El layout SE DIBUJA. El test GPU `lyt_draw_lands_on_efb` también pasa.
3. **De camino encontré y arreglé un crash real** (SIGSEGV en `Layout::Build` con un brlyt cuyo
   `texIdx`/`nameStrOffset` del txl1 está fuera de rango) y un error de formato en el blob
   sintético del test del repo. Todo con tests nuevos que lo pinzan.

## Qué hacer en tu máquina (Windows)

En "Developer PowerShell for VS 2022":

```powershell
cd galaxy-pc
git pull            # o re-extrae el zip encima, como siempre
Remove-Item -Recurse -Force build    # IMPORTANTE: tu build estaba coja (el exe no reflejaba las fuentes)
cmake -B build -DPLATFORM=PC -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure    # esperado: 185 passed, 0 failed, 3 skipped
.\build\src\RelWithDebInfo\galaxy-pc.exe --boot --log-level INFO --log-file boot.log
```

Nota: `lyt_draw_lands_on_efb` te salía "no such test" porque tu `galaxy-pc-tests.exe` también
era viejo (el test está en `src/tests/lyt_layout_test.cpp` desde M9.5.3c). Con el rebuild aparece.

## Cómo verificar que el exe ya es el bueno (30 segundos)

Mira las PRIMERAS líneas de `boot.log`:

```
[INFO ] [main] galaxy-pc 0.5.0 (Milestone 5) — built <FECHA DE HOY>   ← NUEVO: sello de compilación
```

Si la fecha no es de hoy, el exe es viejo: borra `build` y recompila. Después, con el juego
arrancado, deben salir además:

```
[INFO ] [gx] GXCopyDisp #0: efb=640x456 blit=OK          ← present cada frame
[INFO ] [renderer] present #0: result=0 hadContent=1     ← swapchain recibiendo contenido
[INFO ] [gx] EFB probe: 640x456 nonBlack=…/291840 …      ← a los ~5 s; con tu arc debe ser > 0
```

y en la ventana: fondo blanco → strap de WiiRemote con su animación (608x456 escalado a la ventana).

## Causa raíz del "no such test" + del negro (resumen técnico)

| Síntoma | Causa |
|---|---|
| `no such test: lyt_draw_lands_on_efb` | Binario de tests anterior a M9.5.3c (el test está en el repo desde entonces). |
| `EFB probe: nonBlack=0/291840` con tu exe | Ejecutable pre-`95a2002`: sin los fixes/bridges de present de M9.5.3c, nada llegaba al EFB (ni el `fillScreen` blanco). |
| Ausencia de `GXCopyDisp #` y `present #` en tu log | Esas líneas las emite el código de HEAD siempre a INFO; su ausencia delata el exe viejo. |

Evidencia del sandbox (Linux + lavapipe, mismo código):

| Escenario | Resultado |
|---|---|
| HEAD, boot sin assets | `EFB probe: nonBlack=291840/291840 avgRGB=(255,255,255)` |
| Tu log (exe viejo, con assets) | `EFB probe: nonBlack=0/291840 avgRGB=(0,0,0)` |
| HEAD + `WiiRemoteStrap.arc` sintético | `EFB probe: nonBlack=291840/291840 avgRGB=(152,140,115)` — layout dibujado |

## Fixes incluidos en este changeset

0b. **CRASH A LOS ~3-5 SEGUNDOS (arreglado)** — `src/compat/gx/GXTexture.cpp`.
    Síntoma del usuario: el strap se veía bien y a los ~3 s el proceso moría
    (SIGSEGV). Causa raíz, reproducida y depurada en el sandbox:
    1) `Material::SetupGX` de nw4r reprograma UN MISMO `GXTexObj` de la pila
       para todos sus texmaps; la caché host de texturas va indexada por la
       DIRECCIÓN del objeto, así que la textura 2 de un material reprograma el
       registro de la textura 1.
    2) El guard de "la fuente cambió" destruye la textura renderer anterior…
       pero el slot TEXMAP0 seguía apuntando al handle DESTRUIDO (la textura 2
       carga en el slot 1 y nadie re-vincula el 0).
    3) El frame siguiente, el quad de clear del EFB carga el objeto Z24X8
       (formato no soportado en host) → `GXLoadTexObj` falla SIN tocar el slot
       → el handle colgado llega a `bindFragmentTextures` → crash DENTRO DEL
       DRIVER (NVIDIA en tu PC, lavapipe en el sandbox; backtrace idéntico).
    ¿Por qué a los ~3 s? `HookMessage` (el cartel del mando en horizontal) es
    el PRIMER material con 2 texturas del layout y su animación lo activa en
    el frame ~270 de 600 (≈4,5 s a 60 Hz). Mis arcs sintéticos anteriores no
    ejercitaban esa ruta: el generador ahora activa HookMessage en el frame
    270 como el arc real, y el boot reproducía el crash exacto (muerte justo
    tras el draw #300). Fix: al destruir un handle host se desvinculan los
    slots TEXMAP que lo referencien (`unbindTexMapHandle`) y un
    `GXLoadTexObj` fallido desvincula su slot (queda el fallback blanco).
    Verificación: el repro que moría en #301 ahora llega a #600+ vivo; suite
    completa 185 passed; el test GPU lleva el bloque de regresión (patrón de
    pila + Z24X8 + bind — pre-fix crasheaba el driver ahí mismo).
    NOTA de rendimiento conocida: la caché por dirección provoca churn
    (re-subida de texturas por frame al colisionar objetos de pila). En GPU
    real es despreciable; queda anotado refactor (caché por contenido) para
    M9.5.4.

0. **PANTALLA BOCA ABAJO (arreglada)** — `src/compat/gx/GXCompat.cpp` (`flushDraw`).
   Síntoma del usuario: la pantalla de inicio (strap de WiiRemote) se ve invertida
   verticalmente. Causa: la capa GX usa la convención de clip-space de GX/OpenGL
   (Y hacia arriba) y el shader la pasaba tal cual al NDC de Vulkan (Y hacia abajo)
   → **todo** el render GX salía volteado. Nadie lo había visto antes porque todos
   los checks sintéticos eran simétricos verticalmente (barras horizontales, fondos
   sólidos, controles centrados). Prueba: se añadió un marcador VERDE arriba del
   todo al test GPU — pre-fix rasterizaba en las filas de ABAJO
   (`green top=0 bottom=2835 orientation FLIPPED`); post-fix
   (`green top=2835 bottom=0 orientation OK`).
   Fix: negar la fila Y del MVP en `flushDraw` — el único punto por el que pasa
   TODA la geometría GX (inmediata + display lists) — de forma que el EFB queda
   con la orientación de consola (fila 0 = arriba) y el blit, las sondas
   `readRenderTarget` y los `GXCopyTex` ven todo correcto. Verificación end-to-end
   con el boot + arc sintético asimétrico (rojo arriba / azul abajo en PicPlay):
   `probe px upmid = (222,40,41)` rojo arriba, `dnmid = (49,81,230)` azul abajo.
   También: la sonda EFB del boot ahora imprime el par de orientación
   (`upmid`/`dnmid`) para detectar esto a simple vista.

1. **`src/compat/patches/nw4r/lyt/lyt_material.cpp` (NUEVO)** — hardening M9.5.3c.
   El ctor de `Material` resolvía cada textura como
   `ConvertOffsToPtr<char>(textures, textures[texIdx].nameStrOffset)` **sin validar nada**:
   con un brlyt cuyo `texIdx` excede `texNum` o cuyo `nameStrOffset` excede el bloque txl1,
   el puntero resultante es salvaje y `LayoutHolder::GetResource → ResTable::getRes →
   MR::getHashCodeLower` lo dereferencia → **SIGSEGV durante `Layout::Build`**
   (reproducido aquí con un arc sintético). Ahora: `texIdx < texNum`,
   `nameStrOffset < blockHeader.size - 12`, guard de `pTextureList == nullptr`; en fallo,
   degrada al camino de "textura no encontrada" (`ReplaceImage(nullptr)` ya está guarded →
   fallback blanco) SIN desalinear los índices `texMap` de los TevStages.
   Reproducido antes/después: antes SIGSEGV; ahora
   `WARN … texMap 0 out of range (texIdx 1, texNum 4, block size 88) — texture skipped` y el
   juego sigue vivo.
2. **`src/compat/patches/nw4r/lyt/lyt_textBox.cpp`** — la misma validación para la fuente del
   textbox (`fonts[fontIdx].nameStrOffset`, mismo patrón, mismo crash potencial).
3. **`src/main.cpp`** — sello de compilación en el log (`— built <fecha>`), en demo y boot:
   un exe viejo se reconoce al instante (tu caso).
4. **`src/tests/lyt_layout_test.cpp`** — (a) el blob sintético `makeRichBrlyt` usaba el formato
   txl1/fnl1 INCORRECTO (entradas de 4 bytes con offsets relativos al bloque; el formato real
   nw4r son entradas de 8 bytes `{u32 nameStrOffset; u8 type; u8 pad[3]}` con el offset relativo
   al array de entradas en `block+12` — pasaba porque el stub sirve cualquier nombre);
   ahora es formato real y el stub registra los nombres pedidos, con CHECKs que pinzan que se
   pide `tex1.tpl`/`font1.brfnt`. (b) Test nuevo `lyt_build_survives_out_of_range_texidx`:
   `Layout::Build` con `texIdx` fuera de rango debe completar sin crash y pedir el nombre vacío.
5. **`tools/make_synth_strap.cpp` (NUEVO, fuera del CMake)** — generador del
   `WiiRemoteStrap.arc` sintético (RARC + brlyt + brlan + TPLs I4/RGB565) que replica el árbol
   del arc real (RootPane 608x456 > PicBG > WiiRemoteStrap > PlayMessage > PicPlay 568x392 >
   HookMessage > PicHook 360x256+40x40, anim "Strap" 600 frames loop). Sirve para probar el
   boot SIN assets propietarios: `g++ -O2 -std=c++20 tools/make_synth_strap.cpp -o make_synth_strap
   && ./make_synth_strap assets/LayoutData/WiiRemoteStrap.arc`.
6. **`src/compat/patches/README.md`** — inventario actualizado.

## Estado de tests

```
185 passed, 0 failed, 3 skipped        (antes: 181 passed + 2 skipped)
```

incluido `lyt_draw_lands_on_efb` (GPU real, Xvfb + lavapipe): los píxeles del layout y los
quads de control aterrizan en el EFB.

## Próximos pasos sugeridos (M9.5.3d)

- Con el exe nuevo, confirma el hito visible: Logo → strap → (M9.5.3d) pantalla de título con
  `TitleSequenceProduct`.
- Cuando arranque con TUS assets reales, el `EFB probe` de los ~5 s debe dar `nonBlack > 0`.
  Si te diera 0 con el exe nuevo y el sello de hoy, pega el `boot.log` completo y lo seguimos.
