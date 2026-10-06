#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>
#include <falso_jni/FalsoJNI.h>

#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

/*
 * Gangstar Rio (GloftG4HM) -- puente Java->nativo.
 *
 * Todo el puente es ESTATICO (CallStatic*Method sobre la clase de la
 * Activity, IDs cacheados en GL2JNILib_init, out_ghidra.c:5001). FalsoJNI
 * resuelve por nombre+firma e ignora la clase, asi que esta tabla es
 * plana. Inventario completo verificado contra el .so real:
 *   - 30 metodos cacheados en init() (offset JNI 0x1c4 = GetStaticMethodID)
 *   - late-bound por FindClass: GL2JNILib.{stopUserMusic,pauseUserMusic,
 *     isUserMusicPlaying,notifyTrophy,minimize...}, GloftG4HM.{getIMEI,
 *     getVersion,getPhoneModel,getPhoneFW,getPhoneManufacturer,CloseGame,
 *     setDisableKey}, SUtils.{getPreferenceString,getPackage,getSaveFolder},
 *     Device.{getUserAgent}, TelephonyManager.getDeviceId (instancia).
 * Firmas literales extraidas de .rodata (base Ghidra +0x10000), dispatch
 * confirmado por offset de llamada (0x234 void / 0x210 boolean / 0x204 int /
 * 0x218 float / 0x1c8 object) -- leccion del hermano Miami: el dispatch
 * manda, no el tipo Java.
 *
 * Fuera de la tabla a proposito (inaccesibles desde nuestro arranque,
 * main.c nunca invoca esos Java_*): GameInstaller/GDRMPolicy/InAppBilling/
 * DataSharing/HDIDFV (installer + licencia + IAB, auto-contenidos; ver
 * PORTING_PLAN.md seccion 6).
 */

/* Estado que el juego lee de vuelta */
static int g_disable_key = 0;
static int g_pskey_enable = 0;
static int g_ac_enabled = 0;
static float g_ac_interval = 0.0f;
static int g_lang_ingame = 0;
static int g_clear_frames = 0;
static int g_noswap_frames = 0;
static int g_view_w = 0, g_view_h = 0, g_view_bpp = 0, g_view_depth = 0, g_view_stencil = 0;
static char g_igp_baseurl[256] = {0};

/* ------------------------------------------------------------------ */
/* Utilidades de strings Java (FalsoJNI: jstring = JavaString con utf8)*/
/* ------------------------------------------------------------------ */

static const char *jstr_utf8(JavaString *js) {
    if (!js || !js->utf8 || !js->utf8->array) return NULL;
    return (const char *)js->utf8->array;
}

/* Devuelve un String Java. OJO: este FalsoJNI modela jstring como
 * JavaString {utf16, utf8} (FalsoJNI_ImplBridge.h), NO como byte-array
 * crudo (esa es la variante del hermano Miami). Devolver un JavaDynArray
 * hacia GetStringUTFChars (0x2a4) leia `len` como puntero utf16 -> data
 * abort en AndroidGetDeviceModel justo tras "AndroidCreateView" (log_001).
 * NewStringUTF arma la estructura correcta (utf8 + utf16). */
static jobject jstr_ret(const char *s) {
    return (jobject) jni->NewStringUTF(&jni, s ? s : "");
}

/* ------------------------------------------------------------------ */
/* getResource(String) -> byte[] : EL pipeline de assets               */
/*                                                                     */
/* glf::AndroidGetResource (out_ghidra.c:4327) pide el contenido       */
/* completo de un asset por nombre y lo vuelca en un vector. En el     */
/* telefono lo servia la Activity desde el APK; aqui se lee de        */
/* DATA_PATH "data/..." (donde el toolkit deja los datos del juego).  */
/* NULL = "no existe" (el nativo lo chequea: iVar2 == 0 -> uVar6 = 0). */
/* ------------------------------------------------------------------ */

static SceUID res_open(const char *raw_path, SceOff *size) {
    char full[640];

    if (!raw_path || !raw_path[0]) return -1;

    /* El motor a veces pide con subdirectorio ya incluido ("data/..." o
     * rutas del Fs); probar tal cual y bajo data/. */
    static const char *prefixes[] = { DATA_PATH, DATA_PATH "data/" };
    for (int i = 0; i < 2; i++) {
        snprintf(full, sizeof(full), "%s%s", prefixes[i], raw_path);
        SceUID fd = sceIoOpen(full, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            *size = sceIoLseek(fd, 0, SCE_SEEK_END);
            sceIoLseek(fd, 0, SCE_SEEK_SET);
            return fd;
        }
    }
    return -1;
}

jobject Method_getResource(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    const char *path = jstr_utf8(js);

    SceOff size = 0;
    SceUID fd = res_open(path, &size);
    if (fd < 0 || size <= 0 || size > 64 * 1024 * 1024) {
        if (fd >= 0) sceIoClose(fd);
        fjni_logv_warn("[JNI] getResource(\"%s\"): not found", path ? path : "(null)");
        return NULL;
    }

    JavaDynArray *jda = jda_alloc((int)size, FIELD_TYPE_BYTE);
    if (!jda) {
        sceIoClose(fd);
        return NULL;
    }
    int got = sceIoRead(fd, jda->array, size);
    sceIoClose(fd);
    if (got != size) {
        fjni_logv_warn("[JNI] getResource(\"%s\"): short read %d/%d", path, got, (int)size);
    }
    fjni_logv_dbg("[JNI] getResource(\"%s\"): %d bytes", path, (int)size);
    return jda;
}

/* ------------------------------------------------------------------ */
/* Vista / superficie (sin GLSurfaceView real: contexto de vglInit)    */
/* ------------------------------------------------------------------ */

/* GL2JNILib.setupPaths(): callback que el nativo dispara desde
 * getViewSettings, DESPUES de NewApp (glf::AndroidSetupPaths,
 * out_ghidra.c:4418). En Android la Activity respondia llamando al nativo
 * setPaths(data, home, temp). Ese orden es obligatorio:
 * glf::AndroidSetPaths hace App::GetFs() sobre la instancia global de App,
 * que antes de NewApp es NULL -> strcpy a 0x100f8 (dump 1790968206,
 * R0 = 0x100f8). Por eso main.c ya no llama a setPaths. */
typedef void (*fn_set_paths)(JNIEnv *, jclass, jstring, jstring, jstring);

void Method_setupPaths(jmethodID id, va_list args) {
    (void)id; (void)args;
    fn_set_paths set_paths = (fn_set_paths) so_symbol(&so_mod,
        "Java_com_gameloft_android_ANMP_GloftG4HM_GL2JNILib_setPaths");
    if (!set_paths) {
        fjni_log_err("[JNI] setupPaths: GL2JNILib_setPaths not found");
        return;
    }
    /* El Fs del motor asume que estos directorios existen (en Android los
     * creaba la Activity/installer). Ignorar el error si ya estan. */
    sceIoMkdir(DATA_PATH "data", 0777);
    sceIoMkdir(DATA_PATH "saves", 0777);
    sceIoMkdir(DATA_PATH "temp", 0777);
    jstring data_dir = jni->NewStringUTF(&jni, DATA_PATH "data/");
    jstring home_dir = jni->NewStringUTF(&jni, DATA_PATH "saves/");
    jstring temp_dir = jni->NewStringUTF(&jni, DATA_PATH "temp/");
    set_paths(&jni, NULL, data_dir, home_dir, temp_dir);
    l_note("[JNI] setupPaths -> GL2JNILib_setPaths(%s{data,saves,temp}/)", DATA_PATH);
}

void Method_createView(jmethodID id, va_list args) {
    (void)id; (void)args;
    /* Sin-op: el contexto GL lo crea gl_init() (vitaGL). */
}

void Method_setViewSettings(jmethodID id, va_list args) {
    (void)id;
    g_view_w = va_arg(args, jint);
    g_view_h = va_arg(args, jint);
    g_view_bpp = va_arg(args, jint);
    g_view_depth = va_arg(args, jint);
    g_view_stencil = va_arg(args, jint);
    fjni_logv_warn("[JNI] setViewSettings(%d, %d, %d, %d, %d)",
                   g_view_w, g_view_h, g_view_bpp, g_view_depth, g_view_stencil);
}

jboolean Method_setCurrentContext(jmethodID id, va_list args) {
    (void)id;
    jint ctx = va_arg(args, jint);
    fjni_logv_dbg("[JNI] setCurrentContext(%d) -> true", (int)ctx);
    return 1;
}

void Method_enableAccelerometer(jmethodID id, va_list args) {
    (void)id;
    g_ac_enabled = va_arg(args, jint);
    double d = va_arg(args, double); /* float promocionado */
    g_ac_interval = (float)d;
    fjni_logv_dbg("[JNI] enableAccelerometer(%d, %f)", g_ac_enabled, g_ac_interval);
    /* TODO consola: alimentar accelerometerEvent con sceMotion. */
}

void Method_SetDisableKey(jmethodID id, va_list args) {
    (void)id;
    g_disable_key = va_arg(args, jint) ? 1 : 0;
}

void Method_setDisableKey(jmethodID id, va_list args) {
    (void)id;
    g_disable_key = va_arg(args, jint) ? 1 : 0;
}

void Method_setPSKeyEnable(jmethodID id, va_list args) {
    (void)id;
    g_pskey_enable = va_arg(args, jint) ? 1 : 0;
}

void Method_setClearFrames(jmethodID id, va_list args) {
    (void)id;
    g_clear_frames = va_arg(args, jint);
}

void Method_setNoSwapFrames(jmethodID id, va_list args) {
    (void)id;
    g_noswap_frames = va_arg(args, jint);
}

/* ------------------------------------------------------------------ */
/* Navegador / GLive / sociales / tracking (sin equivalente en Vita)  */
/* ------------------------------------------------------------------ */

void Method_voidStub(jmethodID id, va_list args) { (void)id; (void)args; }

void Method_openBrowser(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    fjni_logv_warn("[JNI] openBrowser(\"%s\"): ignored", jstr_utf8(js));
}

void Method_InGameBrowser_SetBaseUrl(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    const char *s = jstr_utf8(js);
    if (s) snprintf(g_igp_baseurl, sizeof(g_igp_baseurl), "%s", s);
}

void Method_InGameBrowser_OpenUrl(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    fjni_logv_warn("[JNI] InGameBrowser_OpenUrl(\"%s\"): ignored", jstr_utf8(js));
}

void Method_nativeGetLangIngame(jmethodID id, va_list args) {
    (void)id;
    g_lang_ingame = va_arg(args, jint);
    fjni_logv_warn("[JNI] nativeGetLangIngame(%d)", g_lang_ingame);
}

void Method_notifyTrophy(jmethodID id, va_list args) {
    (void)id;
    jint trophy = va_arg(args, jint);
    fjni_logv_warn("[JNI] notifyTrophy(%d): TODO sceNpTrophyUnlockTrophy", (int)trophy);
}

void Method_openIGP(jmethodID id, va_list args) {
    (void)id;
    jint v = va_arg(args, jint);
    fjni_logv_warn("[JNI] openIGP(%d): ignored (no IAB en Vita)", (int)v);
}

void Method_launchWelcomeScr(jmethodID id, va_list args) {
    (void)id;
    jint v = va_arg(args, jint);
    fjni_logv_dbg("[JNI] launchWelcomeScr(%d): ignored", (int)v);
}

/* ------------------------------------------------------------------ */
/* Consultas con valor de retorno                                      */
/* ------------------------------------------------------------------ */

jboolean Method_CheckDoneAcceptPP(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 1; /* PP aceptada: no mostrar dialogo inexistente */
}

void Method_SetDoneAcceptPP(jmethodID id, va_list args) { (void)id; (void)args; }

jint Method_isWifiAlive(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0; /* offline; AndroidGetWifiState descarta el valor */
}

jfloat Method_GetMemoryInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 256.0f; /* AndroidGetMemoryInfo descarta el valor */
}

/* GloftG4HM.IsPowerAConnected() -> int (CallStaticIntMethod, sig
 * DAT_00c9fae0). != 0 activa el esquema de mando fisico del motor
 * (Application::setPowerAConnected + XPERIAisSTU/isKeyEnabled,
 * out_ghidra.c:35587 y 37465): es lo que hace que los keycodes Xperia/PowerA
 * de utils/controls.c manejen el juego. La Vita "siempre tiene el mando". */
jint Method_IsPowerAConnected(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 1;
}

/* GloftG4HM.IsFirstLaunched() -> boolean (out_ghidra.c:353354). */
jboolean Method_IsFirstLaunched(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

jboolean Method_isZEUSDevice(jmethodID id, va_list args) { (void)id; (void)args; return 0; }
jboolean Method_isSlideEnabled(jmethodID id, va_list args) { (void)id; (void)args; return 0; }
jboolean Method_isUserMusicPlaying(jmethodID id, va_list args) { (void)id; (void)args; return 0; }

/* Identidad del "telefono" */
jobject Method_getIMEI(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("358031234567890");
}

jobject Method_getDeviceId(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("358031234567890"); /* TelephonyManager.getDeviceId */
}

jobject Method_getVersion(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("1.2.2b");
}

jobject Method_getPhoneModel(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("PSVita");
}

jobject Method_getPhoneFW(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("3.74");
}

jobject Method_getPhoneManufacturer(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("Sony");
}

jobject Method_getUserAgent(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("PSVita");
}

/* glot (tracking de Gameloft, zona del .so que Ghidra no decompilo):
 * glot::GetDeviceIdentifier/GetDeviceHDIDFV/GetMacAddress (0xc3034c...)
 * llaman PortingJNI.getIdentifier / Device.getHDIDFV / Device.getMac
 * "()Ljava/lang/String;" y devuelven NULL si Java falla; el constructor de
 * glot::TrackingManager les hace strcpy sin chequear (dump 1790986927,
 * strcpy(dst, NULL) desde TrackingManager+0x1d4). Valores fijos: estables
 * entre arranques por si el motor los usa como clave de datos guardados. */
jobject Method_getIdentifier(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("358031234567890");
}

jobject Method_getHDIDFV(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("00000000-0000-0000-0000-0000000000a1");
}

jobject Method_getMac(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("02:00:00:00:00:00");
}

/* Android.os.Process.setThreadPriority(nice): sin equivalente directo (la
 * Vita usa prioridades de SceKernel, no "nice"). No-op registrado para que
 * no aparezca como "not found" en cada hilo nuevo del motor. */
void Method_setThreadPriority(jmethodID id, va_list args) {
    (void)id; (void)args;
}

/* Sin sensor de orientacion en la Vita: siempre landscape fijo. */
jboolean Method_isAndroidOrientationAvailable(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

jobject Method_getPackage(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret("com.gameloft.android.ANMP.GloftG4HM");
}

jobject Method_getSaveFolder(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jstr_ret(DATA_PATH "saves/");
}

/* SUtils.getPreferenceString(key, prefsName) -> String. El 2o argumento
 * es el NOMBRE del archivo de SharedPreferences (GameInstaller.
 * mPreferencesName, que ni registramos -> NULL), no un valor por defecto:
 * el Java real devuelve "" si la clave no existe. Unico consumidor:
 * AndroidOS::GetSDFolder("SDFolder"), que con "" cae a
 * /sdcard/gameloft/games/GloftG4HM/ (io.c lo traduce a DATA_PATH "data").
 * Devolver el 2o argumento hacia GetStringUTFChars + `*src` crasheaba. */
jobject Method_getPreferenceString(jmethodID id, va_list args) {
    (void)id;
    JavaString *key = va_arg(args, JavaString *);
    fjni_logv_dbg("[JNI] getPreferenceString(\"%s\") -> \"\"", jstr_utf8(key));
    return jstr_ret("");
}

/* GloftG4HM.getObbFileName/getObbPatchFileName(name) -> String.
 *
 * CStreamTextureManager() fija SIEMPRE el formato 3 (out_ghidra.c:200423),
 * asi que el arranque (out_ghidra.c:35411) pide exactamente
 * "Gangstar3ETC_patch.obb" y "Gangstar3ETC.obb" -- coincide con el obb del
 * cache 1.2.2b, que es la variante ETC (_packEtc/...). En el telefono la
 * Activity los traducia a /sdcard/Android/obb/<pkg>/{main,patch}.NNNN.<pkg>.obb;
 * aca se acepta tanto el nombre Android original (copiado tal cual del zip)
 * como el nombre logico, el que exista primero. */
#define OBB_PKG "com.gameloft.android.ANMP.GloftG4HM"

static int obb_exists(const char *p) {
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

static jobject obb_resolve(const char *name, int patch) {
    char full[512];
    const char *android = patch ? "patch.12221." OBB_PKG ".obb"
                                : "main.12030." OBB_PKG ".obb";

    /* 1) nombre del zip en la raiz, 2) la carpeta del zip tal cual
     * (<pkg>/<nombre>, como queda al extraer el cache), 3) nombre logico. */
    snprintf(full, sizeof(full), "%s%s", DATA_PATH, android);
    if (!obb_exists(full))
        snprintf(full, sizeof(full), "%s" OBB_PKG "/%s", DATA_PATH, android);
    if (!obb_exists(full)) {
        snprintf(full, sizeof(full), "%s%s", DATA_PATH, name ? name : "");
        if (!obb_exists(full))
            fjni_logv_err("[JNI] obb \"%s\" not found (copy %s or %s to %s)",
                          name, android, name, DATA_PATH);
    }
    fjni_logv_warn("[JNI] getObb%sFileName(\"%s\") -> \"%s\"",
                   patch ? "Patch" : "", name, full);
    return jstr_ret(full);
}

jobject Method_getObbFileName(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    return obb_resolve(jstr_utf8(js), 0);
}

jobject Method_getObbPatchFileName(jmethodID id, va_list args) {
    (void)id;
    JavaString *js = va_arg(args, JavaString *);
    return obb_resolve(jstr_utf8(js), 1);
}

/* ------------------------------------------------------------------ */
/* Salida del juego                                                    */
/* ------------------------------------------------------------------ */

void Method_CloseGame(jmethodID id, va_list args) {
    (void)id; (void)args;
    fjni_log_warn("[JNI] CloseGame: exit process");
    sceKernelExitProcess(0);
}

void Method_minimize(jmethodID id, va_list args) {
    (void)id; (void)args;
    fjni_log_warn("[JNI] minimize: ignored");
}

/*
 * JNI Methods
 */

NameToMethodID nameToMethodId[] = {
        // Recurso critico: pipeline de assets
        { 1, "getResource", METHOD_TYPE_OBJECT },
        // Vista / superficie
        { 2, "setupPaths", METHOD_TYPE_VOID },
        { 3, "createView", METHOD_TYPE_VOID },
        { 4, "setViewSettings", METHOD_TYPE_VOID },
        { 5, "setCurrentContext", METHOD_TYPE_BOOLEAN },
        { 6, "enableAccelerometer", METHOD_TYPE_VOID },
        { 7, "openBrowser", METHOD_TYPE_VOID },
        { 8, "SetDisableKey", METHOD_TYPE_VOID },
        { 9, "setDisableKey", METHOD_TYPE_VOID },
        { 10, "InGameBrowser_SetBaseUrl", METHOD_TYPE_VOID },
        { 11, "InGameBrowser_OpenUrl", METHOD_TYPE_VOID },
        { 12, "CheckDoneAcceptPP", METHOD_TYPE_BOOLEAN },
        { 13, "SetDoneAcceptPP", METHOD_TYPE_VOID },
        { 14, "RateApp", METHOD_TYPE_VOID },
        { 15, "GetGangster4ButtonSwitch", METHOD_TYPE_VOID },
        { 16, "LaunchFacebook", METHOD_TYPE_VOID },
        { 17, "LaunchTwitter", METHOD_TYPE_VOID },
        { 18, "NoBackWarning", METHOD_TYPE_VOID },
        { 19, "setPSKeyEnable", METHOD_TYPE_VOID },
        { 20, "launchGLLive", METHOD_TYPE_VOID },
        { 21, "nativeGetLangIngame", METHOD_TYPE_VOID },
        { 22, "notifyTrophy", METHOD_TYPE_VOID },
        { 23, "openIGP", METHOD_TYPE_VOID },
        { 24, "setClearFrames", METHOD_TYPE_VOID },
        { 25, "setNoSwapFrames", METHOD_TYPE_VOID },
        { 26, "launchG4Button", METHOD_TYPE_VOID },
        { 27, "launchCustomerCare", METHOD_TYPE_VOID },
        { 28, "launchWelcomeScr", METHOD_TYPE_VOID },
        { 29, "sendGameTracking", METHOD_TYPE_VOID },
        { 30, "GetMemoryInfo", METHOD_TYPE_FLOAT },
        { 31, "isWifiAlive", METHOD_TYPE_INT },
        // Late-bound: musica / dispositivo / lifecycle
        { 32, "stopUserMusic", METHOD_TYPE_VOID },
        { 33, "pauseUserMusic", METHOD_TYPE_VOID },
        { 34, "isUserMusicPlaying", METHOD_TYPE_BOOLEAN },
        { 35, "isZEUSDevice", METHOD_TYPE_BOOLEAN },
        { 36, "isSlideEnabled", METHOD_TYPE_BOOLEAN },
        { 37, "getIMEI", METHOD_TYPE_OBJECT },
        { 38, "getVersion", METHOD_TYPE_OBJECT },
        { 39, "getPhoneModel", METHOD_TYPE_OBJECT },
        { 40, "getPhoneFW", METHOD_TYPE_OBJECT },
        { 41, "getPhoneManufacturer", METHOD_TYPE_OBJECT },
        { 42, "getDeviceId", METHOD_TYPE_OBJECT },
        { 43, "getUserAgent", METHOD_TYPE_OBJECT },
        { 44, "getPackage", METHOD_TYPE_OBJECT },
        { 45, "getSaveFolder", METHOD_TYPE_OBJECT },
        { 46, "getPreferenceString", METHOD_TYPE_OBJECT },
        { 47, "CloseGame", METHOD_TYPE_VOID },
        { 48, "minimize", METHOD_TYPE_VOID },
        // OBB (elige PVR vs ETC segun GPU)
        { 49, "getObbFileName", METHOD_TYPE_OBJECT },
        { 50, "getObbPatchFileName", METHOD_TYPE_OBJECT },
        // Mando fisico (Xperia Play / PowerA) + varios GloftG4HM
        { 51, "IsPowerAConnected", METHOD_TYPE_INT },
        { 52, "IsFirstLaunched", METHOD_TYPE_BOOLEAN },
        { 53, "SetAutoOrientationOn", METHOD_TYPE_VOID },
        { 54, "SetAutoOrientationOff", METHOD_TYPE_VOID },
        // glot (tracking)
        { 55, "getIdentifier", METHOD_TYPE_OBJECT },
        { 56, "getHDIDFV", METHOD_TYPE_OBJECT },
        { 57, "getMac", METHOD_TYPE_OBJECT },
        { 58, "setThreadPriority", METHOD_TYPE_VOID },
        { 59, "isAndroidOrientationAvailable", METHOD_TYPE_BOOLEAN },
};

MethodsBoolean methodsBoolean[] = {
        { 5, Method_setCurrentContext },
        { 12, Method_CheckDoneAcceptPP },
        { 34, Method_isUserMusicPlaying },
        { 35, Method_isZEUSDevice },
        { 36, Method_isSlideEnabled },
        { 52, Method_IsFirstLaunched },
        { 59, Method_isAndroidOrientationAvailable },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
        { 30, Method_GetMemoryInfo },
};
MethodsInt methodsInt[] = {
        { 31, Method_isWifiAlive },
        { 51, Method_IsPowerAConnected },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
        { 1, Method_getResource },
        { 37, Method_getIMEI },
        { 38, Method_getVersion },
        { 39, Method_getPhoneModel },
        { 40, Method_getPhoneFW },
        { 41, Method_getPhoneManufacturer },
        { 42, Method_getDeviceId },
        { 43, Method_getUserAgent },
        { 44, Method_getPackage },
        { 45, Method_getSaveFolder },
        { 46, Method_getPreferenceString },
        { 49, Method_getObbFileName },
        { 50, Method_getObbPatchFileName },
        { 55, Method_getIdentifier },
        { 56, Method_getHDIDFV },
        { 57, Method_getMac },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
        { 2, Method_setupPaths },
        { 3, Method_createView },
        { 4, Method_setViewSettings },
        { 6, Method_enableAccelerometer },
        { 7, Method_openBrowser },
        { 8, Method_SetDisableKey },
        { 9, Method_setDisableKey },
        { 10, Method_InGameBrowser_SetBaseUrl },
        { 11, Method_InGameBrowser_OpenUrl },
        { 13, Method_SetDoneAcceptPP },
        { 14, Method_voidStub }, /* RateApp */
        { 15, Method_voidStub }, /* GetGangster4ButtonSwitch */
        { 16, Method_voidStub }, /* LaunchFacebook */
        { 17, Method_voidStub }, /* LaunchTwitter */
        { 18, Method_voidStub }, /* NoBackWarning */
        { 19, Method_setPSKeyEnable },
        { 20, Method_voidStub }, /* launchGLLive */
        { 21, Method_nativeGetLangIngame },
        { 22, Method_notifyTrophy },
        { 23, Method_openIGP },
        { 24, Method_setClearFrames },
        { 25, Method_setNoSwapFrames },
        { 26, Method_voidStub }, /* launchG4Button */
        { 27, Method_voidStub }, /* launchCustomerCare */
        { 28, Method_launchWelcomeScr },
        { 29, Method_voidStub }, /* sendGameTracking */
        { 32, Method_voidStub }, /* stopUserMusic */
        { 33, Method_voidStub }, /* pauseUserMusic */
        { 47, Method_CloseGame },
        { 48, Method_minimize },
        { 53, Method_voidStub }, /* SetAutoOrientationOn */
        { 54, Method_voidStub }, /* SetAutoOrientationOff */
        { 58, Method_setThreadPriority },
};

/*
 * JNI Fields
 */

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
		{ 2, "mDeviceInfo", FIELD_TYPE_OBJECT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
		{ 2, NULL }, /* GameInstaller.mDeviceInfo: sin telefonia -> rama Build */
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
