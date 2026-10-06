#include "utils/init.h"
#include <stdlib.h>
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/dialog.h"
#include "utils/controls.h"
#include "video.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

// 192 MB, no 256: deja ~100 MB de RAM a vitaGL (ver gl_init()).
int _newlib_heap_size_user = 192 * 1024 * 1024;
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 16 * 1024 * 1024;
#endif

so_module so_mod;

/*
 * Gangstar Rio (GloftG4HM) -- ciclo de vida nativo confirmado por RE
 * (ver PORTING_PLAN.md seccion 3 y port_progress.md Fase 3):
 *
 * - libgl2jni.so SII tiene JNI_OnLoad (a diferencia del hermano Miami):
 *   GetEnv(0x10004) + VoxSetJavaVM + InAppBilling_setJavaVM. Hay que llamarlo.
 * - _Z15RegisterNativesv es solo registro interno de funciones gameswf
 *   (UI Flash), sin JNI de por medio: nada que hacer en el loader.
 * - GameInstaller/GDRMPolicy/InAppBilling/DataSharing/HDIDFV son el wrapper
 *   de installer + licencia y el loader no los llama (nativeStart haria
 *   exit(0) sin licencia). PERO GL2JNILib_init SI escribe en lockPointer3/4
 *   (que reservaba ese DRM): main() los inicializa antes de init -- ver el
 *   bloque "Licencia" mas abajo (crash del primer arranque en consola).
 * - Todo el puente Java->nativo es estatico (CallStatic*Method sobre la
 *   clase de la Activity, IDs cacheados en GL2JNILib_init). FalsoJNI resuelve
 *   por nombre+firma, la clase se ignora: pasar NULL como jclass es seguro
 *   (mismo patron que el port hermano Miami).
 *
 * Orden de arranque (el que la Activity/GLSurfaceView real usaria):
 *   JNI_OnLoad -> init -> getViewSettings (-> setupPaths -> setPaths) -> initGL ->
 *   resize -> [step + touchEvent/keyEvent/PowerA por frame] -> destroy
 *
 * Input: ver utils/controls.c. OJO: touchEvent usa la convencion Gameloft
 * 0=up/1=down/2=move (no la de MotionEvent), y el motor tiene esquema de
 * mando completo (Xperia Play / PowerA) que se activa con IsPowerAConnected.
 */

typedef void (*fn_void_env)(JNIEnv *);
typedef void (*fn_void_env_obj)(JNIEnv *, jclass);
typedef void (*fn_void_env_obj_ii)(JNIEnv *, jclass, jint, jint);
typedef void (*fn_void_step)(void);
typedef jint (*fn_jni_onload)(void *jvm);

static fn_void_env_obj   GL2JNILib_init;
static fn_void_step      GL2JNILib_getViewSettings;
static fn_void_env       GL2JNILib_initGL;
static fn_void_env_obj_ii GL2JNILib_resize;
static fn_void_step      GL2JNILib_step;
static fn_void_step      GL2JNILib_destroy;

#define RESOLVE(dst, sym) \
    dst = (void *) so_symbol(&so_mod, sym); \
    if (!dst) fatal_error("Could not resolve required symbol: %s", sym);

#define ENGINE_W 960
#define ENGINE_H 544

int main() {
    l_note("==== Gangstar Rio (PSVGR0001) -- loader start ====");
    soloader_init_all();
    l_checkpoint(1, "main: soloader_init_all() done");

    RESOLVE(GL2JNILib_init, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_init")
    RESOLVE(GL2JNILib_getViewSettings, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_getViewSettings")
    RESOLVE(GL2JNILib_initGL, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_initGL")
    RESOLVE(GL2JNILib_resize, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_resize")
    RESOLVE(GL2JNILib_step, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_step")
    RESOLVE(GL2JNILib_destroy, "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_destroy")
    l_checkpoint(2, "main: all GL2JNILib symbols resolved");

    // JNI_OnLoad real (out_ghidra.c:352508): GetEnv(JNI_VERSION_1_4) +
    // VoxSetJavaVM + VoxSetAndroidAPILevel(9) + InAppBilling_setJavaVM.
    // Devuelve 0x10004 si el entorno se obtuvo bien.
    fn_jni_onload JNI_OnLoad = (fn_jni_onload) so_symbol(&so_mod, "JNI_OnLoad");
    if (!JNI_OnLoad) fatal_error("Could not resolve required symbol: JNI_OnLoad");
    jint jni_ver = JNI_OnLoad(&jvm);
    l_checkpoint(3, "main: JNI_OnLoad() done (ver=0x%x)", (unsigned)jni_ver);

    // AndroidOS_JNIOnload(vm) (out_ghidra.c:8992): fija AndroidOS_JavaVM
    // -- el JavaVM PROPIO de AndroidOS_GetEnv, distinto del s_javaVm de
    // JNI_OnLoad -- y corre AndroidOS::Init (cachea SUtils/Device/
    // GameInstaller). En Android lo disparaba el Java InAppBilling.nativeInit
    // al arrancar. Sin esto, el primer AndroidOS_GetEnv (glot::
    // GetDeviceIdentifier en el TrackingManager) desreferencia NULL
    // (dump 1790980572, PC AndroidOS_GetEnv+0x74).
    typedef void (*fn_androidos_onload)(void *jvm);
    fn_androidos_onload AndroidOS_JNIOnload =
        (fn_androidos_onload) so_symbol(&so_mod, "AndroidOS_JNIOnload");
    if (!AndroidOS_JNIOnload) fatal_error("Could not resolve required symbol: AndroidOS_JNIOnload");
    AndroidOS_JNIOnload(&jvm);
    l_checkpoint(31, "main: AndroidOS_JNIOnload() done");

    gl_init();
    l_checkpoint(4, "main: gl_init() done");

    // Video intro: en Android la Activity mostraba res/raw/logo_gameloft_
    // 320x480.mp4 (MediaPlayer Java) ANTES de arrancar el motor; el nativo
    // no lo pide (MediaPlayerAndroid::* son stubs en el .so). Mismo modelo
    // que Shadow Guardian: se reproduce aca, bloqueante, y se libera todo
    // (texturas, memblocks, sysmodule) antes de que el motor reserve memoria.
    // Si el archivo falta o el decoder falla, video_play() retorna igual.
    video_init();
    video_play("res/raw/logo_gameloft_320x480.mp4");
    video_shutdown();
    l_checkpoint(42, "main: intro video done");

    // init(env, activityClass): cachea ~30 method IDs via GetStaticMethodID,
    // arma el mapa de idiomas y llama a glf::AndroidInit(). En el loader no
    // hay Activity: NULL como jclass (FalsoJNI ignora la clase).
    // Licencia: GL2JNILib_init hace `if (!isCheckLicence) *lockPointer4 = 1`
    // y luego `*lockPointer3 += 1` dos veces (out_ghidra.c:5032/5158). Esos
    // punteros los reserva el DRM (LicenseCheck/GameInstaller.nativeStart,
    // out_ghidra.c:14679/15224) que el loader no ejecuta -> quedaban NULL y
    // el primer arranque en consola murio aca (data abort en 0x37256c,
    // dump 1790912977). Se dejan como los deja una licencia VALIDA:
    // lockPointer1..3 = malloc(4) con 1, lockPointer4 = malloc(*lp3 * 4) con 1.
    static const char *lock_syms[] = { "lockPointer1", "lockPointer2", "lockPointer3", "lockPointer4" };
    for (int i = 0; i < 4; i++) {
        int **lp = (int **) so_symbol(&so_mod, lock_syms[i]);
        if (!lp) fatal_error("Could not resolve required symbol: %s", lock_syms[i]);
        if (!*lp) {
            *lp = (int *) malloc(4 * sizeof(int));
            (*lp)[0] = 1;
        }
    }
    l_checkpoint(41, "main: license lockPointer1..4 initialised");

    GL2JNILib_init(&jni, NULL);
    l_checkpoint(5, "main: GL2JNILib_init() done");

    // setPaths NO se llama desde aca: necesita la App ya creada. Lo dispara
    // el propio getViewSettings (NewApp -> AndroidSetupPaths -> callback
    // Java setupPaths -> Method_setupPaths en java.c), igual que en Android.

    // getViewSettings(): sin args JNI (los ignora; usa TLS interno). Crea
    // NewApp, pide setupPaths, corre App::MyInit y devuelve los settings por
    // el callback Java setViewSettings (nuestro stub los registra en el log).
    GL2JNILib_getViewSettings();
    l_checkpoint(7, "main: GL2JNILib_getViewSettings() done");

    // initGL(env): guarda el JNIEnv del hilo GL.
    GL2JNILib_initGL(&jni);
    l_checkpoint(8, "main: GL2JNILib_initGL() done");

    // resize(env, class, w, h) -> AndroidResizeScreen (fuerza landscape:
    // si w < h los intercambia). 960x544 nativo del panel Vita.
    GL2JNILib_resize(&jni, NULL, ENGINE_W, ENGINE_H);
    l_checkpoint(9, "main: GL2JNILib_resize() done -- entering main loop");

    controls_init();

    // Instrumentacion de progreso de frame (igual patron que Miami [022]):
    // las primeras 10 vueltas en detalle, luego heartbeat cada 5 s + linea
    // para frames con step > 0.5 s (las cargas tardan segundos por frame).
    int frame_no = 0;
    SceUInt64 last_beat = 0;
    int last_beat_frame = 0;

    while (1) {
        frame_no++;
        if (frame_no == 11)
            logger_boot_done();
        SceUInt64 frame_start = sceKernelGetProcessTimeWide();
        if (frame_no <= 10)
            l_note("[022] main loop: frame %d enter (t=%llu)", frame_no, frame_start);

        // Touch frontal, botones y sticks -> touchEvent/keyEvent/PowerA
        // (mapeo y RE en utils/controls.c).
        controls_poll();

        // step(): App::update -- un frame de simulacion + render.
        GL2JNILib_step();
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        SceUInt64 step_us = sceKernelGetProcessTimeWide() - frame_start;
        if (frame_no <= 10) {
            l_note("[022] main loop: frame %d returned (%llu us)", frame_no, step_us);
        } else {
            if (step_us > 500000)
                l_note("[022] main loop: frame %d slow step (%llu ms)", frame_no, step_us / 1000);
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (last_beat == 0) {
                last_beat = now;
                last_beat_frame = frame_no;
            }
            if (now - last_beat > 5000000) {
                float fps = (float)(frame_no - last_beat_frame) * 1000000.0f /
                            (float)(now - last_beat);
                l_note("[022] frame %d | %.1f fps | step %.1f ms",
                       frame_no, fps, (float)step_us / 1000.0f);
                last_beat = now;
                last_beat_frame = frame_no;
            }
        }
        gl_swap();

        // Sin sleep fijo: vglSwapBuffers ya bloquea en vblank (60 Hz).
        // TODO consola: suspendGame/resumeGame en eventos de sleep,
        // accelerometerEvent con sceMotion, stateChanged/orientationChanged.
    }

    sceKernelExitDeleteThread(0);
}
