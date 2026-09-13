# M6 — Input completo (implementado)

*Estado: implementado y verificado (121/121 unit tests verdes; los 15 de input).
Implementación: `src/platform/input/` (SDL3), `src/compat/kpad/`, `src/compat/wpad/`,
`src/compat/kpad/InputConfig.cpp`, `src/tests/input_test.cpp`.*

## 1. Qué consume el juego (inventario real, verificado en Petari)

SMG es **solo Wiimote**: no hay usos de PAD de GameCube en `Game/` (los
matches de `PAD*` son subcadenas de `KPAD*`). Todo el input pasa por
**KPAD** (Wiimote + extensiones) y por un puñado de **WPAD** (dispositivo:
motor, speaker, batería, sensores). Los consumidores están en
`Game/System/WPad*.cpp` y `Game/Speaker/`.

### Funciones KPAD llamadas (8)
| Función | Consumidor | Semántica a replicar |
|---|---|---|
| `KPADInit` | `WPadHolder` ctor | reset estado por canal |
| `KPADReset` | `WPadHolder::resetPad` | reset + buffer |
| `KPADRead(chan, buf, 120)` | `WPadHolder::updateReadDataOnly` | estado por canal, hasta 120 muestras |
| `KPADSetBtnRepeat(chan, delay, pulse)` | `WPadButton` ctor (1/2.4 s, 1/6 s) | auto-repeat: bit `KPAD_BUTTON_RPT` en trig |
| `KPADSetSensorHeight(chan, level)` | WPadAcceleration | calibración acelerómetro |
| `KPADSetPosParam(chan, radius, sens)` | WPadPointer | calibración puntero (DPD) |
| `KPADSetHoriParam(chan, radius, sens)` | WPadPointer | idem horizonte |
| `KPADSetDistParam(chan, radius, sens)` | WPadPointer | idem distancia |

### Funciones WPAD llamadas (14)
`WPADRegisterAllocator`, `WPADSetConnectCallback`, `WPADSetExtensionCallback`,
`WPADProbe`, `WPADGetInfoAsync`, `WPADDisconnect`, `WPADControlMotor`,
`WPADControlSpeaker`, `WPADIsSpeakerEnabled`, `WPADGetSpeakerVolume`,
`WPADSendStreamData`, `WPADCanSendStreamData`, `WPADGetWorkMemorySize`,
`WPADGetSensorBarPosition`, `WPADSetAutoSleepTime` (+ `WPADStatus`/`WPADInfo`
en `WPadHolder::getHBMKPadData`, `WPadInfoChecker`).

### Campos de `KPADStatus` consumidos
- `hold` / `trig` / `release` — `WPadButton` (testButton*/testTrigger*), `MR::getPadDataForExceptionNoInit`
- `wpad_err` (`WPAD_ERR_NONE/BUSY/...`), `dev_type` (`WPAD_DEV_CORE/FREESTYLE/CLASSIC`)
- `ex_status.fs.stick` — `WPadStick` (nunchuk)
- `pos`, `dpd_valid_fg`, `dist` — `WPadPointer` (puntero estelar/StarPointer)
- `acc` (raw g), `acc_value`, `acc_speed` — `WPadAcceleration` / `WPadHVSwing`
- batería — vía `WPADInfo` de `WPADGetInfoAsync` (`WPadInfoChecker`)

### Constantes
`WPAD_BUTTON_*` (core 16 bits: LEFT/RIGHT/DOWN/UP/PLUS/2/1/B/A/MINUS/Z/C/HOME),
nunchuk (C/Z), classic (lstick/rstick/triggers), `WPAD_DEV_*`,
`WPAD_ERR_*`, `WPAD_MOTOR_STOP/RUMBLE`, `WPAD_SENSOR_BAR_POS_TOP/BOTTOM`,
`WPAD_BATTERY_LEVEL_*`, `WPAD_MAX_CONTROLLERS` (4), `KPAD_BUTTON_MASK`
(0x0000ffff), `KPAD_BUTTON_RPT` (0x80000000).

## 2. Decisión de arquitectura

**No se porta el stack Wii real** (`RVL_SDK/wpad/WPAD.c` 2451 líneas,
`WPADHIDParser.c` 1892, `BTE`/Bluetooth…): son bibliotecas de plataforma y se
sustituyen igual que se sustituyó GX — capa nativa + compat que mantiene las
**APIs y semántica** del SDK para que `Game/` compile sin cambios.

```
 Game/System/WPad*.cpp + Game/Speaker/Spk*.cpp + resto del juego (sin tocar)
   │  #include <revolution/kpad.h> <revolution/wpad.h>   (headers ya vendored)
   ▼
 src/compat/kpad/KPAD.cpp   ← implementación PC de las 8 KPAD* (nueva, no la
                               vendored) — replica semántica: repeat, clamps,
                               DPD, accel
 src/compat/wpad/WPAD.cpp   ← implementación PC de las 14 WPAD* — estado por
                               canal: conexión, extensión, motor, speaker,
                               batería, callbacks
   ▼
 Platform::Input  (src/platform/Input — crece desde el seed M3)
   │   SDL3: gamepad + teclado + ratón + rumble + config editable
   ▼
 SDL3  (gamepad/joystick/keyboard/mouse events, rumble)
```

La semántica KPAD que el gameplay espera y que la capa nueva debe replicar:
- **Botones**: `hold/trig/release` por canal; auto-repeat con los parámetros
  de `KPADSetBtnRepeat` (delay/pulse en 1/64 s → bit `KPAD_BUTTON_RPT`).
- **Sticks**: clamp circular/rectangular (nunchuk s8, classic s16 con deadzone
  y clamps de KPAD `clamp_stick_circle/cross`), escala a [-1,1] en
  `ex_status.fs.stick` / `cl.lstick/rstick`.
- **Puntero DPD**: `pos` (resolución DPD 512×384), `dpd_valid_fg` (0/1/2+),
  `dist`, `horizon`, `vec`, con calibración `KPADSetPosParam/HoriParam/
  DistParam/SensorHeight`. En PC el **ratón** es la fuente IR.
- **Acelerómetro**: `acc` (g, float), `acc_value`, `acc_speed`, `acc_vertical`.
  En PC se sintetiza (ver §4). `WPadAcceleration`/`WPadHVSwing` (shake/spin)
  funcionan sobre estos campos.

## 3. `Platform::Input` (SDL3)

Crece del seed M3 a la capa real, con el mismo estilo que `Platform::Video`:

- `InputState` por frame: botones virtuales por dispositivo (gamepad/teclado/
  ratón), ejes (stick izq/der, triggers), posición/cambio de ratón, flags
  edge (pressed/released), quit/fullscreen (los de M3 se conservan).
- **Gamepad**: `SDL_OpenGamepad`/`SDL_GetGamepads`, conexión/desconexión en
  caliente, `SDL_RumbleGamepad` para motor (rumble), mapeo SDL (layout
  estándar: SDL_GAMEPAD_BUTTON_* independiente de fabricante).
- **Teclado**: SDL scancodes → botones virtuales.
- **Ratón**: posición en píxeles → DPD (con calibración), botones.
- **Config editable**: `config/input.ini` (formato INI plano, parseado por
  la capa) con secciones por canal: fuente (gamepad/kb+mouse), mapeo
  acción Wiimote → control físico, sensibilidad puntero/acelerómetro.
  Por defecto se lee de disco; si no existe, defaults empotrados.

## 4. Mapeo Wiimote → PC por defecto (configurable)

| Acción Wiimote | Canal 0 por defecto |
|---|---|
| Nunchuk stick (movimiento) | WASD (o stick izq si hay gamepad) |
| A (salto) | Espacio / X / gamepad A |
| B (golpe) | Botón izq. ratón / gamepad B |
| C (cámara) | Shift / gamepad Y |
| Z (agarrar) | Botón der. ratón / gamepad X |
| 1 / 2 | Q / E (cámara) |
| + / − (pausa) | Enter / Backspace |
| D-pad (cámara) | Flechas |
| HOME | Escape |
| Puntero (IR/DPD) | Ratón (clic izq. = A; calibración desde `KPADSetPosParam`) |
| Shake (spin) | Tecla dedicada (por defecto K) — config |
| Rumble | SDL rumble del gamepad |
| Speaker | no-op (SpkSystem queda en `TODO(PC_PORT)`; los sonidos del Wiimote de SMG son accesorios — la música/SFX va por `Platform::Audio`) |

Canal 1+ = gamepads SDL adicionales si están presentes; si no, canales vacíos
(`KPADRead` devuelve 0, `WPADProbe` → `WPAD_ERR_NO_CONTROLLER`). Un canal con
`source=gamepad` reporta Wiimote+nunchuk (`WPAD_DEV_FREESTYLE`) salvo que tenga
`use_classic=true`, que reporta Classic Controller (`WPAD_DEV_CLASSIC`, stick
derecho = C-stick).

### Config: `config/input.ini`

Se lee en `init()` (salvo que la config se fije explícitamente con
`setConfig()`, que tiene prioridad). Formato `[channelN]` (N = 0..3):

```ini
[channel0]
source=keyboard_mouse        ; keyboard_mouse | gamepad | none
bind_a=key:enter             ; bind_<accion>=key:<tecla> | mouse:left|right|middle | none
bind_b=none
bind_up=key:up
bind_down=key:down
bind_left=key:left
bind_right=key:right
bind_one=key:q
bind_two=key:e
bind_plus=key:enter
bind_minus=key:backspace
bind_home=key:escape
bind_c=key:shift
bind_z=mouse:right
shake=key:j                  ; alias de bind_shake
stick=arrows                 ; arrows | wasd (canal teclado)
pointer_sensitivity=1.0
sensor_height=0.35

[channel1]
source=gamepad
gamepad=0
use_classic=false            ; true => Classic Controller en vez de nunchuk
```

Acciones: `up down left right a b one two plus minus home c z shake`.
Teclas: nombres `Platform::keyFromName` (p. ej. `space`, `enter`, `escape`,
`left`, `up`, `w`, `j`, `shift`, `backspace`). Valores desconocidos o `none`
dejan la acción sin mapear.

## 5. Entregables M6

1. ✅ `Platform::Input` completo (SDL3 gamepad/teclado/ratón, rumble, conexión
   en caliente, estado por frame) — `src/platform/input/`.
2. ✅ `src/compat/kpad/KPAD.cpp` (8 funciones) + `src/compat/wpad/WPAD.cpp`
   (14 funciones) sobre esa capa, linkage `extern "C"`.
3. 🟡 Compilar en el build PC los wrappers reales del juego
   `Game/System/WPad*.cpp` — **M6.5 (shims) hecho; parcial**: el sub-hito de
   shims host de cabeceras Petari (cadena JGeometry/JMath Metrowerks) está
   completo y los **7 wrappers compilan** en `libpc_compat`
   (`WPadButton/Stick/Acceleration/Rumble/RumbleData/InfoChecker/LeaveWatcher`),
   con definiciones host de los símbolos pequeños que referencian
   (`src/compat/game/SystemCompat.cpp`, cuerpos verbatim del vendered).
   **Sigue pendiente** (depende de la decomp, no del port):
   - `WPad.cpp` (agregador): su ctor usa `WPadHVSwing`, que no tiene `.cpp`
     en Petari (métodos sin matchear) → no compila/linkea aún.
   - `WPadAcceleration::updateRotate/updateAccAverage`: declarados, sin
     definición en la decomp (el objeto compila; solo se enlaza si el juego
     lo referencia).
   - `WPadPointer.cpp`: requiere `Game/Util.hpp` (agregado de game code).
   - `WPadHolder.cpp`: requiere `GameSystem.hpp` (M9).
   Las semánticas de `WPadButton` quedan fijadas por el test espejo
   `kpad_wpadbutton_semantics` y el material matemático por
   `jmath_shim_test.cpp` (5 tests).
4. ✅ Tests con el runner propio (15 de input, todos verdes): KPAD sintético
   (botones/repeat/sticks/DPD/accel), WPAD (conexión/extensión/motor/batería/
   callbacks), mapeo+config, gamepad nunchuk/classic, espejo WPadButton.
5. ✅ Docs: este fichero + `docs/porting.md` (§5 flujo) — la tabla de
   inventario de funciones KPAD/WPAD está en §1.

### M6.5 — Shims host de cabeceras Petari (sub-hito)

Cabeceras shimmeadas en `src/compat/include/` (detalle de cada parche en
`compat/include/README.md`): `math_types.hpp` (sin `std::atan2` fake),
`JSystem/JMath/JMATrigonometric.hpp` (`std::pair` → `JMath::pair`),
`JSystem/JMath/JMath.hpp` (cuerpos host de `JMAFastSqrt`/`JMAHermiteInterpolation`/
`gekko_ps_copy12/16`), `JSystem/JGeometry/TUtil.hpp` (`<cfloat>`),
`JSystem/JGeometry/TVec.hpp` (calificación de templates + cuerpos host de
`dot`/`operator=`/`negateInternal`/`subInternal`/`mulInternal`),
`JSystem/JGeometry/TMatrix.hpp` (`this->get/set`), `revolution/mem/heapCommon.h`
(include corto + punteros 64-bit).

Soporte host (`src/compat/jsystem/JMathCompat.cpp`): tablas trig de JMath
**rellenadas al arranque** (la decomp no ha reconstruido los datos horneados;
`TSinCosTable`/`TAsinAcosTable`/`TAtanTable` — sus ctors y `atan2_`/`get_`
tampoco estaban definidos), `JMathInlineVEC::PSVEC*` y PSVEC globales
(`mtx.h`) como matemática escalar, ctors no-inline de `TVec3<f32>`.
Intrínsecas PPC en `src/compat/os/PPCIntrinsics.cpp`: `__frsqrte`, `__fabsf`,
`__abs`, `__memcpy` (además de `__cntlzw`).

Los wrappers del juego se compilan con `-w` (estilo decomp) en `pc_compat`;
`src/compat/game/SystemCompat.cpp` define con cuerpos verbatim los símbolos
cuyos ficheros propietarios aún no compilan (`WPad::getKPadStatus/...`,
`WPadReadDataInfo::*`, `MR::getHashCode/getWPadMaxCount/isDeviceFreeStyle`,
`MR::getWPad` stub `TODO(PC_PORT)`). **Borrar esas definiciones cuando los
ficheros originales compilen** (duplicados → no linkea).

### Notas de implementación

- `KPADInit()` lee `config/input.ini` solo si no se fijó config explícita
  (`setConfig()`); así los tests inyectan config sin que `init()` la pise.
- Shake = impulso sintético (`shakeFrames`) con la tecla de `bind_shake`; el
  acelerómetro en reposo devuelve ~1 g sobre Z (como el nunchuk real).
- Puntero: 1:1 con el ratón (sin suavizado DPD); `KPADSet*Param` se acepta por
  fidelidad de API y se ignora (no hay hardware que suavizar) — `TODO(PC_PORT)`
  documentado en `KPAD.cpp`.
- Rumble: `WPADControlMotor` → sink `Platform::CompatInput` (rumble SDL del
  gamepad del canal); `gRumbleSink` es invocable desde el código del juego.
- Speaker: no-op + `TODO(PC_PORT)` (test `wpad_speaker_noop`).

## 6. Fuera de alcance / notas
- PAD de GameCube: el juego no lo usa → no se implementa (la cabecera existe,
  la implementación vendered `Pad.c` de 20 líneas se ignora).
- Stack Bluetooth/BTE/HID: no se porta.
- Speaker del Wiimote: no-op silencioso + `TODO(PC_PORT)` documentado
  (SpkSystem no bloquea el juego).
- DPD con 2 objetos/sensor bar: se reduce a un puntero único (ratón); los
  campos `horizon/dist` se rellenan con valores coherentes para que
  `WPadPointer` no entre en estados raros.

## 6. M10 — el input en el path de boot (A+B en el title)

Hasta M9.5.4 el input solo vivía en el loop de demo de `main.cpp`
(`Input::poll` + `CompatInput::updateFrame`). Con `--boot` eso no se ejecuta
nunca: `gameMain()` no retorna y el bucle de frames está dentro del código
vendored, así que `KPADRead` devolvía siempre vacío y la secuencia del title
no veía el A+B (el prompt no decidía jamás).

El pump de eventos del boot ya existía (`compat/vi pumpHostEvents`, una vez
por retrace, para que la ventana no se cuelgue en Windows), pero no alimentaba
la capa de input. Añadir `Input::poll` ahí robaría eventos al propio pump, así
que se separó un muestreo SIN eventos:

* `Platform::Input::sample(w, h)`: consultas de estado SDL
  (`SDL_GetKeyboardState`, `SDL_GetMouseState`, `SDL_GetGamepad*`), mismo
  post-procesado que `poll` (deadzones, normalizado del ratón con y arriba,
  deltas por diferencia de posición). `quit`/`fullscreenToggle` siguen siendo
  evento-driven y los sigue poniendo el dueño de la cola.
* `Platform::CompatInput::setInputSource(input, window)` +
  `pumpFrame()`: el pump del retrace llama a `pumpFrame()`, que samplea y
  avanza los canales KPAD/WPAD con su dt medido. Sin source registrado es
  no-op (tests headless, demo).
* `main.cpp` (`--boot`): crea el `Platform::Input` del boot, registra el
  source y el rumble sink antes de `gameMain()`.

Con esto el `TitleSequenceProduct` recibe el trig de A+B por la cadena de
siempre (`WPadHolder` → `KPADRead`), la secuencia llega a Decide/Dead y la
escena aparca sobre el cielo vivo (ya no un frame negro, que parecía un
freeze justo cuando el input funcionaba). El siguiente paso de la cadena de
consola es el `FileSelector` dentro de la propia TitleScene (M10).

## 7. M10.1 — canal 0 fusionado y botones de ratón como acciones

Síntomas reales reportados: espacio iba bien, el clic izquierdo no y el mando
no pintaba nada. Tres causas:

1. **Decide = A Y B a la vez** (`mAButtonChecker->getLevel() &&
   mBButtonChecker->getLevel()` en `TitleSequenceProduct::exeLogoDisplay`,
   chan 0). Con el mapping por defecto A=espacio, B=clic izquierdo: hay que
   mantenerlos juntos (como el "Press both A and B" de consola).
2. Los botones de ratón nunca entraban en `state.keys[]` (el lector de binds
   lee ahí): `poll()`/`sample()` ahora espejan `mouseLeft/Right/Middle` en el
   array de teclas.
3. El juego **solo lee WPAD_CHAN0** para title/menús, y los defaults aparcaban
   los gamepads en chan 1-3 → muertos. Nuevo source
   `KeyboardMouseGamepad` (INI: `keyboard_mouse_gamepad`): chan 0 =
   teclado+ratón UNIÓN gamepad 0 (botones por OR); chan 1-3 = gamepads 1-3
   para multijugador.

Puntero en canal fusionado: el ratón manda en absoluto cuando se mueve; en
reposo, los sticks lo giran tipo giroscopio (derecho primero, izquierdo como
fallback, 1.8 u/s) para que un jugador solo-mando alcance menús y el cursor
del FileSelect. `dpd_valid_fg` nunca cae en fusionado (el puntero siempre
existe). Shake = tecla K o `misc1` del pad.

Mapping rápido de referencia (chan 0):

| Wii | Teclado/ratón | Gamepad (SDL standard) |
|---|---|---|
| A | espacio | A (sur) |
| B | clic izquierdo | B (este) |
| Z | clic derecho | Y (norte) |
| C | Shift izq | X (oeste) |
| 1 / 2 | Q / E | LB / RB |
| + / - | Enter / Backspace | Start / Back |
| HOME | Escape | Guide |
| shake | K | misc1 |
| puntero | ratón | sticks en reposo del ratón |
| stick nunchuk | WASD/flechas | stick izquierdo |


## 8. M10.2 — subsistema de gamepad de SDL (arreglo raíz)

El mando no funcionaba porque `Window.cpp` inicializaba SDL con
`SDL_INIT_VIDEO | SDL_INIT_EVENTS` únicamente: sin `SDL_INIT_GAMEPAD`,
`SDL_GetGamepads()` devuelve siempre 0 pads y ningún canal ve jamás un mando
(el puente KPAD era correcto; el subsistema nunca estaba encendido). Ahora
`SDL_Init` incluye `SDL_INIT_GAMEPAD`.

Diagnóstico en log (categoría `platform.input`):

- Al primer `sample()` del boot: `SDL gamepad subsystem initialized; N pad(s) attached`
  + nombre de cada pad detectado.
- En cada apertura perezosa/hotplug: `gamepad attached -> slot N: <nombre>` /
  `gamepad detached from slot N`; fallos de `SDL_OpenGamepad` salen como WARN
  con el mensaje de SDL.

Si el log dice `0 pad(s) attached` con el mando enchufado, el problema es de
detección de SDL (driver/Bluetooth), no del mapeo del port.
