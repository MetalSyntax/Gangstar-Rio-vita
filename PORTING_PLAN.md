# Plan de Port — Gangstar Rio (PS Vita)

> Generado por psvita-port-toolkit el 2026-09-01; **documento vivo, última revisión 2026-10-01.**
> Lo marcado **CONFIRMADO (RE)** se verificó a mano contra el `.so` real (objdump/Ghidra). Nada de
> esto se probó todavía en consola física -- eso es lo próximo (ver sección 9).
> Bitácora cronológica en `port_progress.md`.

## 0. Contexto

- **Juego:** Gangstar Rio: City of Saints 1.2.2b
- **Paquete Java:** com.gameloft.android.ANMP.GloftG4HM
- **APK original:** `Gangstar-Rio-1-2-2b-patched.apk` + cache `Gangstar-Rio-City-of-Saints-v1-2-2b-cache.zip`
- **TITLEID asignado:** `PSVGR0001`

**Motor: CONFIRMADO (RE).** glitch engine de Gameloft (namespace `glitch::collada`, `glitch::video`)
sobre **glf** (framework de app de Gameloft: `glf::App`, `glf::InputManager`, `glf::Console`), audio
**Vox** (`vox::DriverAndroid`), UI Flash con **gameswf**. Es la misma familia que el port hermano
`Gangstar-Miami-Vindication-Vita` (glitch), pero Rio es GLES2 + glf, Miami es GLES1 sin glf: se
reusa infraestructura (logger, filtro de logs, receta de memoria de vitaGL), no código de motor.

**El APK está empaquetado** con un protector (`com.storm.fengyue.StubApplication`, `assets/jiami.dat`,
`libdexload_*.so`): jadx sólo ve 3 clases. Todo el lado Java se reconstruye leyendo el `.so`.

## 1. Binario

- **ABI:** armeabi-v7a, **softfp** (floats por registros enteros). Todo lo que reciba floats del
  `.so` (vitaGL, `nativeSetPowerARightJoystick`, etc.) tiene que compilarse con `-mfloat-abi=softfp`.
- **.so cargado:** `libgl2jni.so` (15 MB) → `ux0:data/gangstarrio/main.so`. `libgenerator.so` no es
  NEEDED de `libgl2jni.so`; no se carga.
- **NEEDED:** libOpenSLES, libGLESv2, libdl, liblog, libm, libc, libz, libstdc++.
- **GLES 2.0** (el `0x20000` del manifest). **No importa ningún símbolo EGL** (cero `bl egl*@plt`):
  el contexto lo crea la GLSurfaceView de Java → en Vita, `gl_init()` (vitaGL).
- **Imports:** 365; `source/dynlib.c` los cubre todos (se agregó `_ZnajRKSt9nothrow_t`).

## 2. Ciclo de vida nativo -- CONFIRMADO (RE)

Exports `Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_*` + `JNI_OnLoad`:

1. `JNI_OnLoad(vm)` → `GetEnv(0x10004)`, `VoxSetJavaVM`, `VoxSetAndroidAPILevel(9)` (→ Vox elige el
   driver OpenSL), `InAppBilling_setJavaVM`.
2. `init(env, cls)` → cachea ~30 `GetStaticMethodID`, `GetJavaVM(&vm)` (usado por
   `NVThreadGetCurrentJNIEnv`), `glf::AndroidInit()`.
3. `setPaths(env, cls, data, home, temp)` → `Fs::Set{Data,Home,Temp}Dir`.
4. `getViewSettings()` → crea la App (`App::MyInit`), devuelve settings por `setViewSettings`.
5. `initGL(env)`, `resize(env, cls, w, h)`.
6. Por frame: input + `step()` (`gApp->vtable[0x24]`) + `vglSwapBuffers`.

`step`, `getViewSettings` y `destroy` no leen argumentos (usan `gApp`/TLS). `_Z15RegisterNativesv` es
registro interno de gameswf, no JNI. GameInstaller/GDRMPolicy/InAppBilling/DataSharing son el
wrapper de installer+licencia y nunca se llaman, **pero** `init` escribe en `lockPointer3/4`, que
reservaba ese DRM: `main.c` los inicializa como licencia válida antes de `init` (Fase 5).

## 3. Tabla JNI (`source/java.c`) -- AUDITADA 2026-10-01

Todo el puente es estático (`CallStatic*Method`); FalsoJNI resuelve por nombre e ignora la clase.
Dispatch por offset de `JNINativeInterface`: `0x234` void, `0x210` boolean, `0x204` int, `0x218`
float, `0x1c8` object (lección de Miami: manda el offset, no la firma Java).

Cruzando todos los nombres que el `.so` pide con `GetStaticMethodID`/`GetMethodID` contra la tabla,
faltaban y se agregaron: `IsPowerAConnected` (int, **= 1**, ver sección 5), `IsFirstLaunched`
(boolean, 0), `SetAutoOrientationOn/Off` (void). Quedan sin cubrir a propósito los de
`android/os/Bundle` (`ABundle`, sólo GLive/installer), `LicenseCheck.startGame/getSDFolder` y
`Policy.UpdatePreferences2` (DRM, inalcanzable).

## 4. Assets / filesystem -- CONFIRMADO (RE)

- `CStreamTextureManager()` fija **siempre** el formato 3 (`out_ghidra.c:200423`) → el arranque
  (`out_ghidra.c:35411`) monta `getObbPatchFileName("Gangstar3ETC_patch.obb")` y
  `getObbFileName("Gangstar3ETC.obb")`. El cache 1.2.2b es justo la variante **ETC** (`_packEtc/...`).
  vitaGL soporta ETC1 nativo en GXM.
- `java.c` acepta el nombre Android original o el lógico, el primero que exista en `DATA_PATH`:
  `main.12030.com.gameloft.android.ANMP.GloftG4HM.obb` (2.0 GB, < 2^31: offsets de 32 bits alcanzan)
  y `patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb`.
- Rutas absolutas del `.so` (`source/reimpl/io.c`):

| Android                                         | Vita                  |
|-------------------------------------------------|-----------------------|
| `/sdcard/gameloft/games/GloftG4HM`              | `DATA_PATH "data"`    |
| `/data/data/com.gameloft.android.ANMP.GloftG4HM`| `DATA_PATH "saves"`   |
| `/sdcard` (resto)                               | `DATA_PATH`           |

- `main.c` crea `data/`, `saves/`, `temp/` antes de `setPaths`.

## 5. Input -- CONFIRMADO (RE), `source/utils/controls.c`

- **`touchEvent` usa la convención Gameloft 0=UP, 1=DOWN, 2=MOVE** (no la de MotionEvent):
  `AndroidAddTouchEvent` 0→0xd8, 1→0xd6, 2→0xd7 y `TouchScreenBase` trata 0xd6/0xd7/0xd8 como
  began/moved/ended (vtable +0x20/+0x24/+0x28). El main.c anterior lo tenía invertido.
- El motor trae **esquema de mando completo (Xperia Play / PowerA MOGA)**. Se activa cuando
  `GloftG4HM.IsPowerAConnected()` ≠ 0 (`Application::setPowerAConnected` + `XPERIAisSTU` +
  `isKeyEnabled`). Keycodes → glf (tabla `transfer_POWER_A_toGLF`, vaddr `0xc8ff08`):

| Vita     | Keycode Android | glf  |
|----------|-----------------|------|
| D-pad    | 19/20/21/22     | 0x24/0x26/0x27/0x25 |
| Cruz     | 23 DPAD_CENTER  | 0x20 |
| Círculo  | 4 BACK          | 0x36 |
| Cuadrado | 99 BUTTON_X     | 0x0f |
| Triángulo| 100 BUTTON_Y    | 0x0e |
| L / R    | 102 / 103       | 0x21 / 0x0c |
| Start / Select | 108 / 109 | 0x10 / 0x52 |

- **Stick izquierdo** → touchpad izquierdo del Xperia Play: `touchEvent` con action 3/4/5
  (up/down/move) y x ≤ 360 → eventos 0xde/0xdc/0xdd fuente 1, recentrados por `glf::tx/ty` sobre
  `centerX_L/centerY_L` (centro del pad = 180,180). `nativeSetPowerALeftJoystick` es un `bx lr`.
- **Stick derecho** → `nativeSetPowerARightJoystick(env, cls, float x, float y)` (ruta analógica
  PowerA, 0xd9/0xda/0xdb con flag normalizado).
- Touch frontal: 4 slots (IDs 0-3), stick izquierdo ID 4; `TouchScreenBase` tiene 8 slots.

## 6. Audio -- CONFIRMADO (RE)

Vox usa OpenSL ES 1.0.1 estándar (`vox::DriverAndroid::_InitOSL` @ 0x945020): engine → OutputMix →
AudioPlayer, locator `SL_DATALOCATOR_BUFFERQUEUE` (2 buffers), PCM 16-bit estéreo 44.1 kHz LE,
interfaces `SL_IID_PLAY` + `SL_IID_BUFFERQUEUE`. Se enlaza el **`libOpenSLES.a` real del vitasdk**
(wilhelm portado con salida `sceAudioOut`) en vez del stub mudo anterior. Sin floats en la API usada.

## 7. Video -- intro del logo en el loader (SceAvPlayer)

El motor nativo nunca reproduce video: `MediaPlayerAndroid::playMovie/stopMovie/pauseMovie` son
vacíos e `isFinish()` devuelve 1. El único video es `res/raw/logo_gameloft_320x480.mp4` (H.264
Constrained Baseline 480x270 30 fps + AAC 44.1 kHz), que en Android mostraba la Activity Java
antes de arrancar el motor. El loader lo reproduce igual: `source/video.cpp` (de Shadow Guardian,
SceAvPlayer), llamado desde `main.c` tras `gl_init()`; skip con Cross/Start. Ver
`port_progress.md` Fase 13.

## 8. Gráficos

- **vitaGL vendorizado** (`lib/vitaGL`, submódulo upstream) + **vitashark** (`lib/vitashark`): el
  `libvitaGL.a` global lo recompilan otros ports con sus parches y EGL propio. Flags:
  `HAVE_SOFTFP_ABI`, `SKIP_SPLASHSCREEN`, `LOG_ERRORS`. Se usa el `egl.c` de vitaGL (el `.so` no
  llama EGL; las entradas egl* de dynlib son del boilerplate).
- Receta de memoria de Miami (confirmada en hardware allá): heap newlib **192 MB**, pool circular
  64 MB, `vglInitExtended(..., 8 MB, MULTISAMPLE_NONE)`.
- Shaders GLSL → traductor de vitaGL (`VGL_MODE_POSTPONED`), GXP cacheados en `DATA_PATH "gxp/"`.
- Contextos extra: el motor sólo usa contextos GL secundarios si el Java llama `setNumExtraContext`
  (no lo llamamos → 0) → todo el GL queda en el hilo principal, como necesita vitaGL.

## 9. Checklist

- [x] Repo creado, `.gitignore` anti-DMCA (ahora también `decompiled/disasm/`).
- [x] Análisis del motor real (secciones 0-2).
- [x] Bootstrap del loader, build OK (`psvita-toolkit build --preset release`).
- [x] Tabla JNI auditada (sección 3).
- [x] Gráficos: vitaGL softfp vendorizado + receta de memoria.
- [x] Input: touch con convención correcta + mando Xperia/PowerA (sección 5).
- [x] Audio: OpenSL ES real.
- [x] Video: intro del logo por SceAvPlayer (Fase 13).
- [x] LiveArea: icono/fondo/startup/pic0 desde el arte del APK, validados por el toolkit.
- [ ] **Primer arranque en consola real** (lo único que falta: todo lo de arriba es RE, no hardware).
- [ ] Confirmar en consola: obb montado, shaders traducidos, audio, sentido/escala de sticks, HUD en
      modo mando, rendimiento.

## 10. Instalación para probar

1. `kubridge.skprx` y `libshacccg.suprx` instalados.
2. Subir `ux0_data/gangstarrio/` (ya armado localmente) a `ux0:data/gangstarrio/`. El loader acepta:
   - `.so`: `main.so` o `libgl2jni.so` (el del APK sin renombrar).
   - obb: en la raíz de `ux0:data/gangstarrio/` o dentro de
     `com.gameloft.android.ANMP.GloftG4HM/` (como sale del zip de cache), con sus nombres originales
     `main.12030.<pkg>.obb` / `patch.12221.<pkg>.obb`; o con los nombres lógicos
     `Gangstar3ETC.obb` / `Gangstar3ETC_patch.obb`.
3. Instalar `build/gangstarrio.vpk`. Logs en `ux0:data/gangstarrio/logs/log_NNN.log` y por
   UDP (`psvita-toolkit logs-live`).
