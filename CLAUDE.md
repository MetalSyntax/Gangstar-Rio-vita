# Gangstar Rio — Port a PS Vita

Port de `Gangstar-Rio-1-2-2b-patched.apk` (Android) a PS Vita vía soloader. Generado con **psvita-port-toolkit**.

## Estructura

- `gangstarrio_extract/` — APK extraído (gitignored).
- `decompiled/` — Java (jadx) y pseudo-C (Ghidra) del/los .so (gitignored, regenerable).
- `source/`, `lib/so_util`, `lib/falso_jni` — scaffold del boilerplate (SoLoader + FalsoJNI).
- `PORTING_PLAN.md` — plan vivo, actualizar a medida que se confirman cosas del motor real.
- `port_progress.md` — bitácora, un bug confirmado a la vez.
- `.psvita-toolkit.json` — config para el toolkit standalone (build/deploy/logs/LiveArea/crash dumps).

Este port **no** tiene una copia local de `porting_tools/` -- todo el build/deploy/debug se maneja
desde **psvita-port-toolkit**, la herramienta standalone (fuera de este repo). Abrí el toolkit y
elegí "Continuar con un port existente" apuntando a esta carpeta.

## Hallazgos de motor (confirmados por RE, ver PORTING_PLAN.md)

- Motor glitch + glf + Vox (OpenSL ES) + gameswf; GLES2; armeabi-v7a **softfp**.
- APK empaquetado (com.storm.fengyue): jadx no ve el Java real, todo sale del .so.
- `touchEvent` usa 0=UP/1=DOWN/2=MOVE (convención Gameloft). Mando físico vía esquema
  Xperia Play/PowerA (`source/utils/controls.c`).
- vitaGL/vitashark vendorizados en `lib/` (submódulos), no usar el libvitaGL.a global.
- Port hermano: `../Gangstar-Miami-Vindication-Vita` (glitch GLES1, mismo logger/receta de memoria).

## Flujo de trabajo esperado

1. Análisis de símbolos antes de tocar loader/source -- skill `psvita-port-init` cubrió la Fase 0-2.
2. Bootstrap del loader guiado por la skill `psvita-porting`.
3. Build/deploy con el toolkit standalone → probar en consola real.
4. Un bug a la vez, guiado por el log real -- skill `so-crash-triage`.
5. Actualizar `port_progress.md` con cada bug confirmado.
