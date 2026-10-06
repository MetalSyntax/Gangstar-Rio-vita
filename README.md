# Gangstar Rio: City of Saints — PS Vita

Port del Gangstar Rio 1.2.2b de Android a PS Vita con so-loader + FalsoJNI.

**Estado:** compila y está listo para el primer arranque en consola real; todavía sin probar en
hardware. Ver `PORTING_PLAN.md` (detalles técnicos) y `port_progress.md` (bitácora).

## Requisitos
- `kubridge.skprx` y `libshacccg.suprx`.
- Tu propia copia del juego (APK + cache 1.2.2b).

## Instalación
1. `ux0:data/gangstarrio/main.so` = `lib/armeabi-v7a/libgl2jni.so` del APK.
2. Del zip de cache, copiar tal cual a `ux0:data/gangstarrio/`:
   `main.12030.com.gameloft.android.ANMP.GloftG4HM.obb` y
   `patch.12221.com.gameloft.android.ANMP.GloftG4HM.obb`.
3. Instalar `gangstarrio.vpk`.

## Controles (esquema Xperia Play del propio juego)
| Vita | Acción |
|---|---|
| Stick izquierdo | Movimiento |
| Stick derecho | Cámara |
| Cruz / Círculo / Cuadrado / Triángulo | Botones de acción (Círculo = atrás) |
| L / R | Hombros |
| Start / Select | Pausa / mapa |
| Pantalla táctil | Igual que en el teléfono |
